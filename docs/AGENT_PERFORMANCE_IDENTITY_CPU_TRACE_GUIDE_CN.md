# PrismRender Stage 17-G：性能身份与 CPU Trace 学习指南

## 1. 阶段目标

Stage 17-F 已经能够回答“哪个 RDG Pass 的 GPU 时间变慢了”，但原有性能数据还缺少两个关键上下文：

1. 这次数据由哪块 GPU、哪个驱动、哪种构建和哪份 Shader 产生。
2. GPU 之外，初始化、Shader 编译、资产加载、帧更新和 RDG 提交分别消耗了多少 CPU 时间。

Stage 17-G 因此实现：

- D3D12/Vulkan 公共显卡身份。
- CPU、构建配置、编译器、架构、Shader 修订和可执行文件哈希。
- 跨模块、可嵌套、可关联 Harness 请求的 CPU Span Trace。
- `performance.measure` 的 CPU Span 统计与 P95 预算。
- 性能基线 v2 的硬件与运行环境兼容性校验。

本阶段不是完整的系统级采样器，也没有读取 CPU 硬件性能计数器。它提供的是引擎内部可控代码区域的墙钟时间，用于自动化定位和回归判定。

## 2. 文件变更

### 2.1 新增文件

- `src/RHI/GraphicsAdapterInfo.h`
- `src/Renderer/PerformanceIdentity.h`
- `src/Renderer/PerformanceIdentity.cpp`
- `src/Core/CpuTrace.h`
- `src/Core/CpuTrace.cpp`
- `examples/harness/performance_identity_cpu_trace.jsonl`
- `docs/AGENT_PERFORMANCE_IDENTITY_CPU_TRACE_GUIDE_CN.md`

### 2.2 主要修改文件

- `src/RHI/D3D12/D3D12Context.h`
- `src/RHI/D3D12/D3D12Context.cpp`
- `src/RHI/Vulkan/VulkanContext.h`
- `src/RHI/Vulkan/VulkanContext.cpp`
- `src/Core/ApplicationLauncher.cpp`
- `src/Core/Application.cpp`
- `src/Core/VulkanApplication.cpp`
- `src/Core/ProcessDiagnostics.cpp`
- `src/main.cpp`
- `src/Renderer/D3D12SceneRenderer.cpp`
- `src/Renderer/VulkanSceneRenderer.cpp`
- `src/Asset/AssetRuntimeLoader.cpp`
- `src/Asset/SlangShaderCompiler.cpp`
- `src/Scene/SceneLoader.cpp`
- `src/Scene/WorldRenderSnapshot.cpp`
- `src/Automation/HarnessTools.cpp`
- `src/Automation/PerformanceBaseline.h`
- `src/Automation/PerformanceBaseline.cpp`
- `tests/EngineHarnessTests.cpp`
- `CMakeLists.txt`

## 3. 总体数据流

```text
Harness requestId
  -> PRISM_RENDER_CORRELATION_ID
  -> PrismRender child process
  -> structured log / crash report / CPU Trace 使用同一个关联 ID

D3D12 DXGI Adapter / Vulkan PhysicalDevice
  -> GraphicsAdapterInfo
  -> RuntimePerformanceIdentity
  -> .identity.json
  -> Harness validation
  -> PerformanceBaseline v2

Application / Asset / Shader / Frame / Renderer / RDG
  -> CpuTraceSpan RAII
  -> parentSpanId + threadId + duration
  -> .cpu.json
  -> Harness summary
  -> Median / P95
  -> cpuSpanBudgets
```

## 4. 公共显卡身份

### 4.1 RHI 数据结构

`GraphicsAdapterInfo` 是 RHI 公共结构，包含：

```cpp
struct GraphicsAdapterInfo
{
    std::string name;
    std::uint32_t vendorId;
    std::uint32_t deviceId;
    std::uint64_t dedicatedVideoMemoryBytes;
    std::uint64_t sharedSystemMemoryBytes;
    std::uint64_t driverVersionRaw;
    std::string driverVersion;
    std::string apiVersion;
};
```

上层性能系统只依赖这个结构，不需要直接读取 `IDXGIAdapter` 或 `VkPhysicalDevice`。

### 4.2 D3D12 后端

D3D12 使用：

- `DXGI_ADAPTER_DESC1` 获取名称、Vendor ID、Device ID 和显存。
- `IDXGIAdapter::CheckInterfaceSupport` 获取驱动版本。
- 固定记录当前最低设备要求 `D3D_FEATURE_LEVEL_12_0`。

驱动同时保留数值和可读字符串。基线判断使用数值，JSON 展示使用字符串。

### 4.3 Vulkan 后端

Vulkan 使用：

- `vkGetPhysicalDeviceProperties` 获取设备和驱动信息。
- `vkGetPhysicalDeviceMemoryProperties` 汇总 Device Local 和共享内存堆。
- `VK_VERSION_MAJOR/MINOR/PATCH` 输出 API 版本。

NVIDIA 等厂商可能对 `driverVersion` 使用厂商编码，所以必须保留 `driverVersionRaw`，不能只依赖格式化字符串。

## 5. 构建身份

`CaptureRuntimePerformanceIdentity` 还会记录：

- CPU 名称：通过 CPUID Brand String。
- 构建配置：Debug、Release 或 RelWithDebInfo。
- 编译器：MSVC 完整版本。
- 架构：x64 或 arm64。
- Shader 修订：按路径排序后，对全部 `.hlsl/.slang` 内容计算稳定哈希。
- Shader 文件数量。
- 当前 `PrismRender.exe` 内容哈希。

报告格式：

```json
{
  "format": "PrismRuntimePerformanceIdentity",
  "version": 1,
  "identity": {
    "graphicsApi": "d3d12",
    "adapterName": "NVIDIA GeForce RTX 5060",
    "vendorId": 4318,
    "deviceId": 11525,
    "driverVersionRaw": 9007199255733218,
    "cpuName": "13th Gen Intel(R) Core(TM) i5-13600KF",
    "buildConfiguration": "Debug",
    "compiler": "MSVC 195136248",
    "architecture": "x64",
    "shaderRevision": "c96116d48fdfaf05",
    "executableHash": "..."
  }
}
```

Harness 通过 `PRISM_RENDER_PERFORMANCE_IDENTITY_PATH` 请求该报告，并检查报告中的 API 是否与请求一致。

## 6. CPU Trace 实现

### 6.1 RAII Span

业务代码只需要创建局部对象：

```cpp
CpuTraceSpan span("RendererPipelineInitialize", "initialization");
```

构造时调用 `BeginSpan`，析构时调用 `EndSpan`。提前结束可显式调用 `span.End()`。

这种方式能覆盖正常返回和 C++ 异常，不需要每条返回路径手工写结束事件。

### 6.2 父子关系

每个线程维护独立 Span 栈：

```text
Application
  RendererInitialize
    RendererPipelineInitialize
      ShaderCompile
  RendererRun
    Frame
      FrameRender
        SceneRenderer
          RenderGraphExecute
            GBuffer
```

JSON 中每条 Span 包含：

- `id`
- `parentSpanId`
- `name`
- `category`
- `threadId`
- `startMilliseconds`
- `durationMilliseconds`
- `complete`

`complete=false` 表示进程异常结束时 Span 仍处于活动状态。这能帮助 Agent 判断崩溃发生在哪个尚未返回的阶段。

### 6.3 当前分类

- `process`：进程和 Renderer 生命周期。
- `initialization`：设备、Pipeline、编辑器初始化。
- `diagnostics`：性能身份采集。
- `shader`：Slang 编译。
- `asset`：Manifest、缓存、Cooked 资产和 glTF。
- `frame`：更新、渲染、提交和 Present。
- `renderer`：场景常量与 Renderer 主入口。
- `rdg`：RenderGraph 构建、执行和具体 Pass。
- `readback`：截图与 GPU Timing Resolve。

Span 名称是自动化契约。修改名称时需要同步预算、测试和学习文档。

### 6.4 写出与崩溃路径

`main.cpp` 启动时调用：

```cpp
CpuTrace::InitializeFromEnvironment();
```

正常退出和 C++ 异常路径都会调用 `Flush`。SEH 未处理异常过滤器也会写出当前已完成和未完成 Span。

报告先写临时文件，再原子替换目标文件，避免 Harness 读到半个 JSON。

## 7. RDG 与 CPU/GPU 双时间

两个后端在 `RenderGraph::Execute` 的 Marker 回调中同时执行：

```text
Pass Begin
  -> CpuTraceSpan(category = "rdg")
  -> GpuProfiler::BeginPass

Pass Execute

Pass End
  -> GpuProfiler::EndPass
  -> CpuTraceSpan End
```

两种时间含义不同：

- RDG CPU 时间：CPU 记录命令、绑定资源和执行 Pass 回调的时间。
- RDG GPU 时间：GPU 真正执行该 Pass 的时间。

例如 CPU 时间增长而 GPU 时间稳定，通常说明命令生成、资源绑定或同步代码退化；GPU 时间增长而 CPU 时间稳定，通常说明 Shader、带宽、采样或绘制规模退化。

## 8. Harness 接入

`render.capture` 和 `rdg.describe` 现在额外返回：

- `cpuTracePath`
- `cpuTrace`
- `performanceIdentityPath`
- `performanceIdentity`

子进程失败时，这些数据也会进入 `process` 错误详情，供 Agent 联合日志、Crash Report 和不完整 Span 分析。

`engine.describe` 新增能力：

- `runtimePerformanceIdentity`
- `cpuSpanTrace`
- `correlationIds`
- `cpuSpanBudgets`

## 9. CPU Span 性能预算

`performance.measure` 会读取每个样本 CPU Trace 的 `summary`。

对于每个 `category/name`：

1. 取单个子进程中该 Span 的最大耗时。
2. 汇总多个样本。
3. 计算 Minimum、Maximum、Mean、Median 和 P95。
4. 使用 P95 与预算比较。

示例：

```json
{
  "command": "performance.measure",
  "arguments": {
    "api": "d3d12",
    "stage": "tonemap",
    "warmupCount": 1,
    "sampleCount": 5,
    "cpuSpanBudgets": {
      "frame/FrameUpdate": 4.0,
      "renderer/SceneRenderer": 3.0,
      "rdg/GBuffer": 1.0,
      "rdg/Tonemap": 0.5
    },
    "enforceCpuBudgets": true
  }
}
```

GPU 预算仍使用 `passBudgets`，CPU 和 GPU 预算可以在同一请求中同时启用。

## 10. 性能基线 v2

性能基线升级为 `PrismPerformanceBaseline` version 2。

兼容性判断包含：

- API、捕获阶段、分辨率。
- World Hash 和 Asset Manifest Hash。
- GPU Vendor ID、Device ID、显存和驱动数值版本。
- API 版本。
- CPU 名称。
- 构建配置、编译器和架构。

Shader 修订和可执行文件哈希会被记录，但不参与“环境是否可比较”的判断。

原因是性能基线本来就用于比较代码改动前后的构建。如果把可执行文件哈希设为必须相同，每次重新编译都会拒绝比较，无法发现新构建引入的回归。

旧 version 1 基线仍可读取，但新运行带有硬件字段，因此建议重新生成 v2 基线。

## 11. 验证方法

### 11.1 编译

```powershell
cmake --build build-windows-ci --config Debug --parallel 8
```

### 11.2 自动测试

```powershell
ctest --test-dir build-windows-ci -C Debug --output-on-failure
```

Stage 17-G 验证结果：

- 7/7 CTest 通过。
- CPU Trace 父子关系和 `correlationId` 单元测试通过。
- Performance Baseline v2 保存、加载和驱动不匹配测试通过。
- Runtime Performance Identity JSON 契约测试通过。

### 11.3 真实后端验证

2026-07-16 在本机 Debug 构建验证：

- D3D12 `render.capture` 成功。
- Vulkan `render.capture` 成功。
- 两端均输出 GPU Timing、CPU Trace 和 Runtime Performance Identity。
- 两端 CPU Trace 均包含 Shadow、GBuffer、DeferredLighting、Bloom 和 Tonemap RDG Span。
- `performance.measure` 的 CPU/GPU 预算同时通过。

Debug 构建会包含 Slang 运行时编译成本，不应直接作为 Release 性能目标。

## 12. 阅读顺序

建议按以下顺序学习：

1. `src/RHI/GraphicsAdapterInfo.h`
2. `src/RHI/D3D12/D3D12Context.cpp` 与 `src/RHI/Vulkan/VulkanContext.cpp`
3. `src/Renderer/PerformanceIdentity.cpp`
4. `src/Core/CpuTrace.cpp`
5. `src/Core/ApplicationLauncher.cpp`
6. `src/Core/Application.cpp` 与 `src/Core/VulkanApplication.cpp`
7. `src/Renderer/D3D12SceneRenderer.cpp`
8. `src/Renderer/VulkanSceneRenderer.cpp`
9. `src/Automation/HarnessTools.cpp`
10. `src/Automation/PerformanceBaseline.cpp`
11. `tests/EngineHarnessTests.cpp`

先理解身份和 Trace 数据结构，再看业务模块如何放置 Span，最后看 Harness 如何聚合与判定。

## 13. 设计取舍

### 13.1 为什么不直接使用外部 Profiler

PIX、RenderDoc、Nsight 和 Tracy 更适合人工深度分析，但 Agent Harness 需要：

- 无 UI、可在 CI 运行。
- 每条请求都有稳定 JSON。
- 可设置自动预算。
- 能与 World Hash、Crash Report 和 GPU Timing 关联。

内建 CPU Trace 负责自动化闭环，外部 Profiler 仍用于人工调查复杂问题。

### 13.2 为什么 Span 由业务代码显式命名

自动采样所有函数会产生大量噪声，也无法保证跨构建稳定。显式 Span 只覆盖有工程意义的阶段，并把名称变成可维护的自动化契约。

### 13.3 为什么 CPU Trace 与 GPU Timestamp 分开

CPU 和 GPU 位于不同时间线，不能用一个计时器替代另一个。分开采集、使用相同 RDG Pass 名称关联，既保留真实语义，也方便 Agent 做因果定位。

## 14. 下一阶段

Stage 17-G 完成后，建议继续：

1. Cooked Asset v2：版本块、压缩、校验和、向后兼容与更细粒度加载诊断。
2. Asset Cache GC：引用扫描、容量预算、LRU/保留策略和 dry-run 报告。
3. 离线 Dump 符号化：CI 收集 `.dmp` 后在独立工具中使用匹配 PDB 还原调用栈。
4. Stage 17-K 当时只完成提交探针；Stage 17-L 至 17-P 后续已完成高级 RDG 多队列 Pass、Timestamp、DAG Batch、成本模型和原生命令并行录制。
5. MCP Agent Adapter 已在 Stage 25 完成，Stage 30 又加入双 API 资产流送运行门禁。

下一步优先实现 Cooked v2 与缓存 GC，因为现有性能和诊断链路已经能够可靠衡量资产管线修改带来的启动、内存与加载时间变化。
