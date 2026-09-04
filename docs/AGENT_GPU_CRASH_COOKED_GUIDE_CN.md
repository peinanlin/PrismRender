# PrismRender Stage 17-F：GPU 性能、崩溃诊断与 Cooked Asset 学习指南

## 1. 阶段目标

Stage 17-F 完成了四条 Agent 自动化所需的工程链路：

1. D3D12/Vulkan 使用同一套 `GpuProfiler` 输出 RDG Pass GPU 时间。
2. `performance.measure` 可以为每个 Pass 设置预算并按 P95 判定。
3. Windows 崩溃时生成 Minidump，并使用 PDB 输出函数、文件和行号。
4. glTF 导入结果被写成 `.prismmesh/.prismtex/.prismmat`，运行时直接加载 Cooked 数据。

这使 Harness 不仅能回答“是否渲染成功”，还可以回答：

- 哪个 RDG Pass 变慢了。
- 性能是否超过预算。
- 子进程为什么崩溃、崩溃栈在哪里。
- 运行时实际加载了哪些离线资产。

## 2. 文件变更

### 2.1 新增

- `src/Renderer/GpuTimingReport.h`
- `src/Renderer/GpuTimingReport.cpp`
- `src/Asset/CookedAssetIO.h`
- `src/Asset/CookedAssetIO.cpp`
- `examples/harness/gpu_crash_cooked_assets.jsonl`
- `docs/AGENT_GPU_CRASH_COOKED_GUIDE_CN.md`

### 2.2 主要修改

- `src/Renderer/GpuProfiler.h/.cpp`
- `src/Renderer/D3D12SceneRenderer.h`
- `src/Renderer/D3D12SceneRenderer.cpp`
- `src/Renderer/VulkanSceneRenderer.h/.cpp`
- `src/Core/Application.h/.cpp`
- `src/Core/VulkanApplication.h/.cpp`
- `src/Core/ProcessDiagnostics.cpp`
- `src/Automation/HarnessTools.h/.cpp`
- `src/Asset/AssetDatabase.cpp`
- `src/Asset/AssetRuntimeLoader.h/.cpp`
- `src/Scene/WorldRenderSnapshot.h/.cpp`
- `tests/EngineHarnessTests.cpp`
- `CMakeLists.txt`

## 3. 总体数据流

```text
RDG Pass
  -> RenderGraph begin/end marker
  -> GpuProfiler
  -> D3D12 QueryHeap / Vulkan QueryPool
  -> PrismGpuTimingReport JSON
  -> performance.measure
  -> Median/P95
  -> passBudgets

Unhandled failure
  -> main catch / SEH Filter
  -> ProcessDiagnostics
  -> JSON Crash Report + .dmp
  -> DbgHelp + PDB symbolization
  -> crash.inspect

glTF + buffer + image
  -> AssetDatabase::ImportGltf
  -> GltfLoader
  -> MeshAsset/TextureAsset/MaterialAsset
  -> CookedAssetIO
  -> content-hash Cooked cache
  -> Asset Manifest
  -> AssetRuntimeLoader
  -> AssetRegistry
  -> D3D12/Vulkan GPU resource
```

## 4. 统一 GPU Timestamp

### 4.1 公共 Profiler

`GpuProfiler` 现在具有两组后端重载：

```cpp
Initialize(D3D12Context&)
Initialize(VulkanContext&)
BeginFrame(...)
BeginPass(...)
EndPass(...)
EndFrame(...)
ResolveSubmittedFrame(...)
```

上层 Renderer 与 Harness 不需要理解 QueryHeap 或 QueryPool。

### 4.2 D3D12 实现

D3D12 路径使用：

- `ID3D12QueryHeap`
- `D3D12_QUERY_TYPE_TIMESTAMP`
- Readback Buffer
- `ID3D12CommandQueue::GetTimestampFrequency`
- `ResolveQueryData`

每帧使用独立查询区间。GPU 完成后将时间戳差值除以 Queue Frequency，得到毫秒。

### 4.3 Vulkan 实现

Vulkan 路径为每个 Frame-in-Flight 创建一个 `VkQueryPool`：

- 帧开始时 `vkCmdResetQueryPool`
- Pass 前后调用 `vkCmdWriteTimestamp`
- GPU 完成后调用 `vkGetQueryPoolResults`
- 使用 `VkPhysicalDeviceLimits::timestampPeriod` 转换为毫秒

查询池按帧分离，避免重置仍被 GPU 使用的查询。

### 4.4 RDG 接入

`RenderGraph::Execute` 原有 Marker Callback 现在连接到 `GpuProfiler`：

```text
beginMarker(pass.name) -> BeginPass
execute pass
endMarker(pass.name)   -> EndPass
```

D3D12/Vulkan 使用相同的公共 Pass 名：

- `Shadow`
- `GBuffer`
- `DeferredLighting`
- `BloomExtract`
- `BloomHorizontal`
- `BloomVertical`
- `Tonemap`
- `Renderer`

D3D12 还包含负责截图 Readback 的 `SceneColorReady`。

### 4.5 GPU Timing 报告

Renderer 在收到 `PRISM_RENDER_GPU_TIMING_REPORT_PATH` 时写出：

```json
{
  "format": "PrismGpuTimingReport",
  "version": 1,
  "graphicsApi": "Vulkan",
  "passes": [
    {
      "name": "GBuffer",
      "gpuMilliseconds": 0.0149
    }
  ]
}
```

独立文件便于 CI 留档，也避免把全部采样数据混入进程日志。

## 5. RDG Pass 性能预算

`performance.measure` 新增：

```json
{
  "passBudgets": {
    "Shadow": 2.0,
    "GBuffer": 2.0,
    "DeferredLighting": 2.0,
    "Renderer": 8.0
  },
  "enforceGpuBudgets": true
}
```

每个 Pass 会返回：

- Median GPU milliseconds
- P95 GPU milliseconds
- Budget
- Pass 是否存在
- 是否通过

判定使用 P95，而不是单次最小值或平均值：

```text
passed = passP95 <= budget
```

如果预算中的 Pass 不存在，也会失败。这可以发现 Pass 被意外改名、关闭或移出 RDG。

墙钟 Baseline 与 GPU Pass Budget 解决的是不同问题：

- 墙钟时间：发现启动、Shader 编译、资产上传、Readback 等整体退化。
- GPU Pass 时间：定位具体渲染算法或资源访问退化。

预算必须按目标机器、分辨率、场景、构建配置分别维护。

## 6. Windows Minidump 与 PDB 符号化

### 6.1 崩溃边界

两种失败路径都会进入 `ProcessDiagnostics`：

- C++ 异常：`main.cpp` 顶层 `catch`
- SEH 异常：`SetUnhandledExceptionFilter`

Harness 为子进程设置：

```text
PRISM_RENDER_CRASH_REPORT_PATH
PRISM_RENDER_MINIDUMP_PATH
PRISM_RENDER_LOG_PATH
```

### 6.2 Minidump

`MiniDumpWriteDump` 将当前进程写入 `.dmp`。SEH 路径会携带 `EXCEPTION_POINTERS`；普通 C++ 异常没有原始 SEH 上下文，但仍会生成进程 Dump 和 catch 边界调用栈。

### 6.3 符号化

DbgHelp 使用：

- `SymInitialize`
- `SymFromAddr`
- `SymGetLineFromAddr64`
- `CaptureStackBackTrace`

Debug 构建旁边的 `PrismRender.pdb` 使 Crash Report 可以包含：

```json
{
  "symbol": "main",
  "file": "D:/unity_project/PrismRender/src/main.cpp",
  "line": 18
}
```

### 6.4 Harness 查询

新增命令：

```json
{
  "requestId": "inspect-crash",
  "command": "crash.inspect",
  "arguments": {
    "path": "automation/reports/process/example.crash.json"
  }
}
```

它返回 Minidump 是否存在、异常符号、调用栈和结构化错误详情。Agent 不需要直接解析二进制 `.dmp` 就能完成第一轮定位。

## 7. Cooked Asset 格式

### 7.1 为什么同时保留 Source Bundle

Source Bundle 与 Cooked Asset 不是重复功能：

- Source Bundle 保存 glTF、Buffer、图片，支持审计、重导入和离线恢复。
- Cooked Asset 保存运行时需要的数据，避免 `AssetRuntimeLoader` 再次解析 glTF。

### 7.2 内容寻址路径

```text
automation/cache/cooked/<contentHash>/<stableAssetId>.prismmesh
automation/cache/cooked/<contentHash>/<stableAssetId>.prismtex
automation/cache/cooked/<contentHash>/<stableAssetId>.prismmat
```

源内容变化会得到新的 `contentHash` 目录；Stable Asset ID 保持不变。

### 7.3 公共文件头

三个格式都包含：

```text
Magic: PRCOOKED
Version: 1
Kind: Mesh / Texture / Material
```

读取时会校验 Magic、版本、类型、长度和元素数量上限。

### 7.4 `.prismmesh`

保存：

- Mesh 名称
- Bounds
- Vertex Count
- Index Count
- `MeshVertex` 数组
- 16-bit Index 数组

代码使用 `static_assert` 固定 v1 的 Header、Bounds 和 Vertex 尺寸。修改 `MeshVertex` 时必须升级格式版本或提供迁移。

### 7.5 `.prismtex`

保存：

- 名称与源纹理路径
- Solid Color
- Width/Height
- RGBA8 像素数据

没有源图片的材质槽会 Cook 成 1x1 语义正确的默认纹理，例如法线为 `(0.5, 0.5, 1, 1)`。

### 7.6 `.prismmat`

保存：

- PBR/Blinn-Phong 参数
- Alpha、Normal、Occlusion、Emissive 参数
- 五类纹理启用标记
- 五个 Stable Texture Asset ID

Material 不保存运行时 Handle，因为 Handle 每次启动都可能变化。运行时先加载 Texture，再通过 Stable Asset ID 解析 Handle。

### 7.7 运行时加载顺序

`AssetRuntimeLoader` 按固定顺序加载：

```text
Texture -> Mesh -> Material
```

Material 加载时验证五个 Texture 依赖已经进入 `AssetRegistry`，然后为 D3D12 或 Vulkan 创建后端资源。

World Receipt 新增：

```json
{
  "cookedAssetCount": 7,
  "cookedAssetBytes": 1228153
}
```

`asset.cache.status` 同时列出 Source Bundle 和每个 Cooked 文件的格式、版本、路径、大小与有效性。

## 8. 当前格式边界

Cooked v1 已经是真正被运行时消费的二进制格式，但还不是最终发行格式：

- Mesh 使用当前引擎 `MeshVertex` 内存布局。
- 数值使用当前平台的 little-endian IEEE 浮点表示。
- Texture 只保存 RGBA8，没有 BC/ASTC 压缩和完整 Mip 链。
- Index 只有 16-bit。
- 没有 Chunk CRC、压缩、流式读取和 Schema Migration。
- 没有按目标平台生成不同 Cook Profile。

因此 v1 适合当前 Windows D3D12/Vulkan 开发闭环。跨平台发布前应升级为显式字段序列化的 Cooked v2。

## 9. 自动化验证

2026-07-16 实测：

```text
MSVC Debug build: passed
CTest: 7/7 passed
Cooked files: 7
Cooked bytes: 1,228,153
Source Bundle: 3 files / 123,306 bytes
D3D12 Cooked runtime load: passed
Vulkan Cooked runtime load: passed
```

同一 World/Manifest：

```text
World Hash: bdcb4bc8636f216f
Asset Manifest Hash: 64915859e3028ce2
D3D12/Vulkan unresolved assets: 0 / 0
```

单样本 GPU `Renderer` 时间：

```text
D3D12: 0.244064 ms
Vulkan: 0.120160 ms
```

这些数字只用于证明 Timestamp 管线工作，不应直接当作稳定性能结论。正式预算需要 Release 构建、预热和更多样本。

Cooked 场景跨 API Golden Image：

```text
MAE: 0.0000500908
RMSE: 0.0004531224
Changed pixel ratio, tolerance 8: 0.0
Result: passed
```

真实崩溃验证：

```text
Minidump written: true
Minidump size: 95,419 bytes
PDB size: 53,276,672 bytes
Crash stack contains function/file/line: true
crash.inspect: passed
```

## 10. 运行示例

先确保正式 Manifest 已重新导入：

```powershell
'{"requestId":"reimport","command":"asset.reimport","arguments":{"path":"assets/scenes/StartupScene.gltf"}}' |
  .\build-windows-ci\Debug\PrismHarness.exe --headless --project-root .
```

再执行完整示例：

```powershell
.\build-windows-ci\Debug\PrismHarness.exe `
  --headless `
  --project-root . `
  --commands examples\harness\gpu_crash_cooked_assets.jsonl `
  --output automation\stage17f-results.jsonl
```

## 11. 推荐学习顺序

1. 阅读 `GpuProfiler.h`，先理解公共接口和 FrameData。
2. 对照阅读 D3D12/Vulkan 的 `Initialize` 与 `WriteTimestamp`。
3. 阅读 `RenderGraph::Execute` 的 Marker Callback。
4. 阅读 `GpuTimingReport.cpp`，理解文件协议。
5. 阅读 `MeasurePerformance` 的 Pass 聚合与 P95 预算。
6. 阅读 `ProcessDiagnostics.cpp` 的顶层异常、Minidump 和 DbgHelp。
7. 运行 `crash.inspect`，对照 Crash JSON 与 PDB。
8. 阅读 `CookedAssetIO.h`，画出三个文件格式。
9. 阅读 `AssetDatabase::ImportGltf` 的 Write 路径。
10. 阅读 `AssetRuntimeLoader` 的 Texture/Mesh/Material 加载顺序。
11. 打开 Asset Manifest，核对 `metadata.cooked`。
12. 打开 World Receipt，核对 Cooked 数量、字节数与 Manifest Hash。
13. 最后运行双 API Golden Image，确认资产格式变化没有改变画面。

## 12. 下一阶段

建议 Stage 17-G 按以下顺序推进：

1. 为性能身份增加 GPU Adapter、Driver、CPU、Build Config 和 Shader Revision。
2. 增加 CPU Frame、Render Thread、Asset Load、Shader Compile 的 Span/Correlation ID。
3. 将每 Pass GPU Baseline 持久化，建立 CI 趋势和自动回归报告。
4. 实现 Cooked v2：显式字段布局、CRC、压缩、Mip、BC/ASTC、32-bit Index 和 Schema Migration。
5. 增加 Cook Cache GC、容量预算、文件锁、并发导入和远程 Cache。
6. 离线 Minidump 符号化工具与 Build/PDB 身份匹配已在 Stage 17-I 完成，详见 `docs/AGENT_OFFLINE_MINIDUMP_SYMBOLIZATION_GUIDE_CN.md`；符号服务器和 CI Dump 工件归档仍待实现。
7. Stage 17-K 已完成原生 Alias Memory、Aliasing Barrier 和 Compute Queue 提交探针；继续实现高级 Async Compute Pass 提交和 Subresource Tracking。
8. 增加 MCP/Agent Adapter，让外部 Agent 通过稳定工具协议调用 Harness。
