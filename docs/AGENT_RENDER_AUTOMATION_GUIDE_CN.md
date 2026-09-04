# Stage 17-B：ECS 渲染同步与 Agent 渲染自动化学习指南

本文记录 Stage 17-B 的真实代码实现。目标是把 Stage 17 的 Headless World/Command 闭环接到正在运行的渲染器，并让 Agent 能通过结构化命令完成 Shader 编译、RenderGraph 查询、截图和跨 API Golden Image 对比。

这一阶段没有把 PrismRender 改造成完整游戏引擎，也没有实现 MCP 网络服务。它完成的是 MCP/Agent Adapter 下面必须稳定存在的本地执行层。

## 1. 本阶段完成内容

1. 增加 `Engine::World -> Scene::RenderScene` 同步桥。
2. D3D12 编辑器启动时把现有 RenderScene 导入 ECS，之后由 ECS 保存场景数据。
3. ImGui 中的 Transform、Visible、DirectionalLight 和 Gizmo 编辑统一提交 `component.set`。
4. 连续拖动使用显式 Transaction 分组，形成一个 Undo/Redo 单元。
5. RenderGraph 开放结构化 Pass、Resource、Read/Write State 查询。
6. D3D12/Vulkan 运行时都能输出版本化 RDG JSON 报告。
7. `PrismHarness` 增加 Shader 编译、截图、Golden Image、双 API 对比和 RDG 查询命令。
8. 渲染子进程支持隐藏窗口、固定输入、超时退出和项目路径限制。
9. 增加同步桥与 RDG 查询单元测试，并完成真实 D3D12/Vulkan 端到端验证。

## 2. 文件清单

### 2.1 新增文件

- `src/Scene/WorldRenderSceneBridge.h/.cpp`
- `src/Automation/HarnessTools.h/.cpp`
- `src/Renderer/RenderGraphDiagnostics.h/.cpp`
- `tests/WorldRenderSceneBridgeTests.cpp`
- `examples/harness/render_validation.jsonl`
- `docs/AGENT_RENDER_AUTOMATION_GUIDE_CN.md`

### 2.2 主要修改文件

- `src/Scene/RenderObject.h`
- `src/Engine/CommandSystem.h/.cpp`
- `src/UI/EditorLayer.h/.cpp`
- `src/Core/Application.h/.cpp`
- `src/Core/VulkanApplication.h/.cpp`
- `src/Platform/Window.cpp`
- `src/Renderer/RenderGraph.h/.cpp`
- `src/Renderer/D3D12SceneRenderer.h`
- `src/Renderer/D3D12SceneRenderer.cpp`
- `src/Automation/HarnessRunner.h/.cpp`
- `src/Automation/HarnessMain.cpp`
- `tests/RenderGraphTests.cpp`
- `CMakeLists.txt`

## 3. 总体数据流

```text
ImGui Inspector / Gizmo
        |
        v
Engine::CommandProcessor
  Transaction + Journal + Undo/Redo
        |
        v
Engine::World（权威可持久化数据）
        |
        v
WorldRenderSceneBridge
        |
        v
Scene::RenderScene（运行时渲染视图）
        |
        v
RenderGraph -> RHI -> D3D12 / Vulkan
```

自动化路径为：

```text
JSON / JSONL
    |
    v
HarnessRunner
    |----------------------|
    v                      v
CommandProcessor       HarnessTools
World/ECS 命令         Shader / Capture / Golden / RDG
                           |
                           v
                 隐藏的 PrismRender 子进程
                     D3D12 或 Vulkan
```

`HarnessRunner` 只负责解析和路由。World 命令仍由 `CommandProcessor` 执行；GPU 和文件产物命令由 `HarnessTools` 执行。两类结果使用相同的 `success/requestId/command/worldHash/data/error` 外层格式。

## 4. ECS 到 RenderScene 同步桥

### 4.1 为什么需要桥

`Engine::World` 适合 UUID、组件、序列化、事务和 Replay；`Scene::RenderScene` 包含 Camera、运行时 Mesh/Material 指针和渲染器当前需要的紧凑数组。两者职责不同，不能让渲染器直接依赖序列化细节，也不能继续让编辑器绕过 ECS 修改 RenderScene。

因此本阶段采用“权威 World + 派生 RenderScene”结构，而不是立刻删除现有 RenderScene。

### 4.2 ImportRenderScene

应用启动时，`WorldRenderSceneBridge::ImportRenderScene` 把当前场景转换为 ECS：

- Camera -> `Transform + Camera`
- DirectionalLight -> `DirectionalLight`
- PointLight -> `Transform + PointLight`
- RenderObject -> `Name + Transform + MeshRenderer`

每个 `RenderObject` 新增 `entityId`，作为两种表示之间的稳定关联键。资源引用优先保存 Asset Path；没有路径的旧资源使用 `prism-handle://mesh/N` 和 `prism-handle://material/N` 临时保留句柄信息。

### 4.3 SynchronizeToRenderScene

World 发生修改后，同步桥按 UUID 重建 RenderObject 列表：

1. 读取 Name、Transform、MeshRenderer。
2. 用 Asset Path 从 `AssetRegistry` 解析句柄。
3. 用 UUID 找回旧 RenderObject 的运行时 Mesh/Material 指针。
4. 写入 Camera、DirectionalLight 和最多四个 PointLight。
5. 返回无法解析的资源列表，不静默吞掉错误。

相机同步可独立关闭。编辑器每帧同步场景对象和灯光时不覆盖正在由 CameraController 操作的 Viewport Camera；保存和加载时再显式同步相机。

## 5. ImGui 为什么必须经过 CommandProcessor

如果 Inspector 直接写 `RenderObject::transform`，Headless Agent、Journal、Undo/Redo 和 UI 会形成四种不同修改路径。现在以下 ECS 数据编辑都构造成 `component.set`：

- Transform position/rotation/scale
- MeshRenderer visible
- DirectionalLight direction/color/intensity
- ImGuizmo Transform

ImGui 拖动控件会在 `IsItemActivated()` 时执行 `transaction.begin`，在 `IsItemDeactivatedAfterEdit()` 时执行 `transaction.commit`。Gizmo 使用 `ImGuizmo::IsUsing()` 的开始和结束边沿完成相同分组。

因此一次持续拖拽即使产生很多帧的 `component.set`，也只形成一个 Undo 单元。命令失败时错误会进入编辑器状态消息，事务可以 Rollback，不会留下部分属性更新。

天空、Grid、Bloom 等 `RenderSettings` 仍是渲染配置，不属于 ECS 组件，所以本阶段继续直接编辑。后续可增加 RenderSettings Command 或 Project Settings 数据模型。

## 6. RenderGraph 查询如何实现

`RenderGraph` 增加两个只读查询：

```cpp
std::vector<ResourceDescription> GetResourceDescriptions() const;
std::vector<PassDescription> GetPassDescriptions() const;
```

Stage 17-K 后，资源描述包含瞬态标记、活跃状态、FirstUse/LastUse、规划别名槽、原生 Pool/Allocation 和原生分配字节；Pass 描述包含裁剪状态、执行索引、Queue 和依赖。

`RenderGraphDiagnostics::BuildRenderGraphReport` 把查询结果转换为：

```json
{
  "format": "PrismRenderGraphReport",
  "version": 3,
  "graphicsApi": "Direct3D 12",
  "compilation": {},
  "resources": [],
  "passes": [],
  "queueSync": []
}
```

D3D12 与 Vulkan Application 读取以下环境变量，在第一帧执行后写出报告：

```text
PRISM_RENDER_RDG_REPORT_PATH
PRISM_RENDER_EXIT_AFTER_RDG_REPORT=1
```

Stage 17-K 当时只完成 Queue 提交探针。后续 Stage 17-L 至 17-P 已完成
Compute Bloom/Hi-Z、跨队列 Timestamp、DAG Queue Batch、历史成本模型和
D3D12/Vulkan 原生命令并行录制；Stage 20 已完成资源版本、Texture 子资源
和 Buffer Range。报告继续明确区分可用能力、`auto` 决策与本帧实际应用。

## 7. HarnessTools 安全边界

Harness 不提供任意 Shell 命令。渲染工具采用以下限制：

- 输入路径必须位于 `--project-root` 内；生成物只能写入 `automation/` 或 `build*` 目录。
- 图形 API 白名单只有 `d3d12`、`vulkan`。
- Capture Stage 使用固定白名单。
- 子进程默认 120 秒超时，可配置范围为 1 到 600 秒。
- 超时后终止子进程并返回结构化错误。
- `PRISM_RENDER_HEADLESS=1` 让 GLFW 创建隐藏窗口。
- `PRISM_RENDER_DETERMINISTIC=1` 固定相机和动态输入。
- 同一 `requestId` 返回缓存结果，不重复截图或覆盖产物。

工具命令仍可能需要 GPU 和图形驱动。“Headless”在这里表示无交互隐藏窗口，不表示软件渲染或完全无 GPU。

## 8. 新增命令

### 8.1 shader.compile

同一份 HLSL/Slang 源码可编译为 DXIL 或 SPIR-V，并返回 Reflection：

```json
{
  "requestId": "compile-mesh-spirv",
  "command": "shader.compile",
  "arguments": {
    "path": "assets/shaders/Mesh.hlsl",
    "entryPoint": "PSMain",
    "stage": "pixel",
    "format": "spirv",
    "output": "automation/shaders/Mesh.PSMain.spv"
  }
}
```

支持 `vertex/pixel/compute` 和 `dxil/spirv/dxbc/metal`。返回 Bytecode 大小、实际入口、诊断文本与 Descriptor Binding Reflection。

### 8.2 golden.compare

```json
{
  "requestId": "compare-approved",
  "command": "golden.compare",
  "arguments": {
    "reference": "baselines/d3d12.bmp",
    "candidate": "automation/captures/d3d12.bmp",
    "enforce": true,
    "meanThreshold": 0.01,
    "rmseThreshold": 0.02,
    "changedThreshold": 0.05,
    "pixelTolerance": 8
  }
}
```

返回尺寸是否一致、MAE、RMSE、变化像素比例和最大通道误差。`enforce=true` 时超出阈值返回 `golden_threshold_exceeded`。

### 8.3 render.capture

```json
{
  "requestId": "capture-shadow-vulkan",
  "command": "render.capture",
  "arguments": {
    "api": "vulkan",
    "stage": "shadow",
    "output": "automation/captures/vulkan-shadow.bmp",
    "useCurrentWorld": false
  }
}
```

Stage 支持 `shadow/gbuffer0/gbuffer1/gbuffer2/gbuffer3/hdr/bloom/tonemap`。

### 8.4 render.compare_apis

此命令依次启动 D3D12 和 Vulkan，捕获同一 Stage，再执行严格 Golden 对比：

```json
{
  "requestId": "compare-tonemap",
  "command": "render.compare_apis",
  "arguments": {
    "stage": "tonemap",
    "useCurrentWorld": false,
    "enforce": true,
    "meanThreshold": 0.001,
    "rmseThreshold": 0.005,
    "changedThreshold": 0.01
  }
}
```

### 8.5 rdg.describe

```json
{
  "requestId": "rdg-vulkan",
  "command": "rdg.describe",
  "arguments": {
    "api": "vulkan",
    "output": "automation/reports/rdg-vulkan.json",
    "useCurrentWorld": false
  }
}
```

返回完整版本化报告，而不只是报告文件路径。Agent 可直接按 Pass 名称和资源状态分析渲染管线。

## 9. 运行完整示例

```powershell
.\build-windows-ci\Debug\PrismHarness.exe `
  --headless `
  --project-root . `
  --commands examples\harness\render_validation.jsonl `
  --output automation\render-validation-results.jsonl
```

执行 `engine.describe` 可发现新增命令、支持的 API 和 Capture Stage，不需要把这些选项硬编码在 Agent Prompt 中。

## 10. 验证结果

本阶段在 2026-07-16 完成以下验证：

```text
CMake configure: passed
MSVC Debug build: passed
CTest: 7/7 passed
DXIL Mesh.PSMain: 19104 bytes
SPIR-V Mesh.PSMain: 25132 bytes
D3D12 RDG query: 8 passes
Vulkan RDG query: 7 passes
D3D12/Vulkan tonemap: 1600 x 900
MAE: 0.0000175418
RMSE: 0.0002698141
Changed pixel ratio (tolerance 8): 0.0
Strict parity thresholds: passed
```

`WorldRenderSceneBridgeTests` 覆盖 UUID、Asset Path、Transform、Visible、Camera、DirectionalLight 和 PointLight 同步。`RenderGraphTests` 覆盖 Pass/Resource 描述以及自动 Barrier 后的最终状态。

真实 Harness 验证还实际启动了两个图形后端，因此同时覆盖了隐藏窗口、子进程超时框架、环境变量、运行时 RDG 报告和截图产物。

## 11. 当前边界与下一步

本阶段完成的是基础闭环，仍有以下边界：

1. Vulkan Application 尚未接入完整 ImGui Editor。
2. Stage 17-D 已加入 glTF Asset Import/Reimport；通用纹理、独立 Mesh 和离线中间格式仍未完成。
3. Editor 的 RenderSettings 还没有 Command/Undo 数据模型。
4. RDG 的 D3D12/Vulkan Pass 名称和逻辑资源命名还未完全规范化。
5. Stage 17-L 至 17-P 已完成高级 Async Compute、DAG Batch、A/B、成本模型和原生命令并行录制；默认场景无收益时 `auto` 会保持串行。
6. Stage 17-F 至 17-I 已完成 Minidump、Build/PDB 身份和离线符号化；企业 Symbol Store 与远程趋势服务仍不属于当前本地基线。
7. MCP Adapter 已在 Stage 25 完成，Stage 30 已把资产流送双 API 长流程接入 `prism.execute`。

推荐后续顺序：

1. 将 Stage 17-E Source Bundle 扩展为 Cooked 中间格式、Cache GC 和文件监听。
2. 把 Undo/Redo 菜单和快捷键接到同一 CommandProcessor。
3. 规范化双后端 RDG Pass/Resource 名称和 BackBuffer/Depth 状态。
4. 强类型 RDG Handle、自动资源实例化和 Compute Bloom/Hi-Z 已完成，见 Stage 20、27 与 17-L。
5. D3D12 Fence、Vulkan Timeline、跨队列提交与 Timestamp 已完成，见 Stage 17-M/N。
6. MCP Adapter 与资产流送运行门禁已完成，见 Stage 25 与 Stage 30；可恢复的远程 Job 服务仍是未来产品扩展。

## 12. 推荐学习顺序

1. 阅读 `WorldRenderSceneBridge.h`，先理解 Import 与 Synchronize 两个方向。
2. 阅读 `WorldRenderSceneBridge.cpp`，跟踪一个 MeshRenderer 从 Asset Path 到运行时指针。
3. 阅读 `Application::SynchronizeWorldToRenderScene` 和保存/加载路径。
4. 阅读 `EditorLayer::SubmitTransform`，观察 UI 如何构造结构化命令。
5. 阅读 Edit Transaction 与 Gizmo Transaction 的开始/提交边沿。
6. 阅读 `RenderGraph::GetPassDescriptions/GetResourceDescriptions`。
7. 阅读 `RenderGraphDiagnostics.cpp`，理解运行时图如何变为稳定 JSON。
8. 阅读 `HarnessRunner.cpp`，理解 Engine 命令与 Tool 命令路由。
9. 阅读 `HarnessTools.cpp` 的路径限制、环境变量 Guard 和子进程超时。
10. 运行示例，故意降低 Golden 阈值或写错 Shader 入口，观察结构化失败结果。

## 13. Stage 17-C：当前 World 渲染闭环

Stage 17-C 已补齐 Stage 17-B 的关键身份缺口：

- `render.capture`、`render.compare_apis` 和 `rdg.describe` 默认冻结并渲染当前 Harness World。
- 完整 `PrismEngineSnapshot` 同时携带 ECS、固定时间步、Tick 和随机种子。
- D3D12/Vulkan 都注册相同的内置与 glTF Asset Path。
- 子进程返回 `PrismRenderedWorldReceipt`。
- Harness 在截图比较前校验 `worldHash == renderedWorldHash`。
- 缺少 Camera、太阳或 Mesh/Material 时返回结构化错误。
- `useCurrentWorld=false` 仍可验证传统启动场景。

完整实现、数据流、命令参数、回执格式和实测结果见：

- `docs/AGENT_WORLD_RENDER_LOOP_GUIDE_CN.md`

## 14. Stage 17-D：Agent Asset Pipeline

Stage 17-D 已增加：

- `PrismAssetManifest` 与稳定 `prism-asset://` 路径。
- `asset.list`、`asset.describe`、`asset.import`、`asset.reimport`。
- glTF 主文件、Buffer 和图片依赖 Hash。
- D3D12/Vulkan 共用的运行时资产重建入口。
- 渲染前 Asset Manifest Hash 校验。
- `PrismRenderedWorldReceipt` 中的资产版本和诊断。

完整实现、命令示例和实测双 API 结果见：

- `docs/AGENT_ASSET_PIPELINE_GUIDE_CN.md`

## 15. Stage 17-E：可观测性、性能与缓存

Stage 17-E 已把每次 Capture/RDG/Performance 子进程的 JSONL 日志、stdout、stderr 和 Crash Report 接入 Harness 结果，并增加：

- `asset.cache.status`
- `performance.measure`
- Performance Baseline 保存、身份校验和回归阈值
- Content Hash Asset Cache Bundle
- World Receipt Cache Hit/Miss/Bytes

完整实现与实测结果见：

- `docs/AGENT_OBSERVABILITY_CACHE_GUIDE_CN.md`
