# PrismRender 架构收敛实施结果

实施日期：2026-08-06

## 1. 这次修改解决了什么

重构前最难理解的地方不是某一个类写得不好，而是项目同时存在两套高层流程：

- D3D12 使用 `Application + D3D12SceneRenderer + D3D12Context`。
- Vulkan 使用 `VulkanApplication + VulkanSceneRenderer + VulkanContext`。
- Asset/Scene 的部分入口接收具体 Context，导致资产层知道 GPU API。
- Feature 虽然已经拆成类，但它们的资源指针和执行回调仍由大 Renderer 逐项填写。
- 旧 Triangle Renderer、原生 Vertex/Index Buffer 和 PSO Cache 已无调用者，却仍参与构建。

修改后只有一条高层路径：

```text
main
  -> ApplicationLauncher
  -> ApplicationHost
  -> RenderBackendFactory
       -> D3D12RenderBackend
       -> VulkanRenderBackend
  -> SceneRenderer
  -> Renderer/Features/*
  -> SharedRenderGraphFrontend
  -> IGraphicsDevice + ICommandContext + IFrameContext
```

`ApplicationHost`、`SceneRenderer`、Asset 和 Scene 只依赖公共接口。D3D12/Vulkan 的 Device、Command List/Buffer、Descriptor 和 Swapchain 对象留在 RHI 后端；D3D12 ImGui 的原生接入也被隔离到 `RHI/D3D12/D3D12ImGuiBridge`。

## 2. 修改前后对照

| 关注点 | 修改前的问题 | 修改后的结果 |
| --- | --- | --- |
| 应用入口 | `Application` 和 `VulkanApplication` 分别维护初始化与主循环 | 只有 `ApplicationHost`，运行时通过工厂选择 Backend |
| 高层 Renderer | D3D12/Vulkan 各有一个 SceneRenderer | 只有公共 `Renderer/SceneRenderer` |
| 帧生命周期 | 高层直接调用具体 Context | `IFrameContext` 统一 BeginFrame、EndFrame、Resize、BackBuffer 和 Depth |
| 设备与命令 | 高层持有或识别具体 API 对象 | 使用 `IGraphicsDevice`、`ICommandContext`、`IRenderBackend` |
| Asset/Scene | 存在 D3D12/Vulkan 专用加载入口 | 只接收公共 RHI Device/Command 接口 |
| Feature 组装 | SceneRenderer 集中填写每个 Feature 的资源和回调 | Feature 通过 `RegisterRenderGraph` 提交自己的资源状态与执行回调 |
| 截图 | 两个应用分别处理 API 截图 | `IRenderBackend` 统一 Request/Record/Resolve，具体读回仍在后端 |
| 目录 | Feature 与高层 Renderer 混在一个目录 | 可插拔功能统一放入 `Renderer/Features/` |
| 遗留路径 | Triangle Renderer、原生 Buffer 包装和旧 PSO Cache 仍在编译 | 无调用者代码已移除 |
| 验证 | Shader/RenderGraph 测试不稳定 | Debug 全量构建、8/8 CTest、双 API 运行和 Golden 均通过 |

## 3. 当前模块职责

### Core

- `ApplicationLauncher`：解析 `--api`，建立进程级诊断，然后启动唯一 Host。
- `ApplicationHost`：窗口、场景、资产、Renderer、可选编辑器 UI 和主循环的 Composition Root。
- 它可以决定“装哪些模块”，但不实现 D3D12/Vulkan 命令。

### RHI

- `IRenderBackend`：一个图形 API 后端的组合入口。
- `IFrameContext`：帧开始/结束、Resize、Swapchain 与 Depth 的公共视图。
- `IGraphicsDevice`：Buffer、Texture、View、Pipeline、DescriptorSet 等资源创建。
- `ICommandContext`：Draw、Dispatch、Barrier、Rendering Scope 和队列命令。
- `RenderBackendFactory`：唯一知道具体 Backend 类型的创建位置。
- `RHI/D3D12` 与 `RHI/Vulkan`：原生 API 实现。
- `RHI/Profiling`：GPU Timestamp 的 RHI 级跨后端服务；公共 Renderer 只消费 Timing 结果。

### Renderer

- `SceneRenderer`：组织一帧所需的共享常量、主 GBuffer/Deferred/PostProcess 流程和 Render Graph 执行。
- `SharedRenderGraphFrontend`：保存跨 API 一致的 Pass 拓扑与资源依赖。
- `Renderer/Features`：Clustered Lighting、GPU Visibility、Local Light Shadows、Planar Reflection、VSM、GTAO/SSR 和 TAA 的资源所有者及执行注册者。
- Renderer 中不再出现 `ID3D12*`、`Vk*`、`D3D12Context` 或 `VulkanContext`。

### Asset 与 Scene

- Asset 负责 CPU 数据、导入、缓存、Streaming 以及通过 `IGraphicsDevice` 创建 GPU 资源。
- Scene 负责 Camera、RenderObject、Light、序列化以及 World/Streaming 到 RenderScene 的桥接。
- 两个模块都不知道当前运行的是 D3D12 还是 Vulkan。

### UI

- UI 面板只处理编辑器数据和交互。
- ImGui 官方 D3D12 Backend 必须接触原生 Descriptor Heap，因此它作为明确的后端集成适配器放在 `RHI/D3D12`，没有泄漏到 Application 或 Renderer。

## 4. 一帧现在怎样流动

```text
ApplicationHost
  1. IFrameContext::BeginFrame
  2. AssetStreamingManager 上传已完成的资产
  3. SceneRenderer::Render(IRenderBackend, RenderScene)
       a. 更新公共 Frame/Object/Light 常量
       b. 各 Feature 注册资源、初始状态与执行回调
       c. SharedRenderGraphFrontend 建立跨 API DAG
       d. RenderGraph 编译 Barrier、Culling、Aliasing 与队列批次
       e. ICommandContext 执行 Draw/Dispatch
  4. Backend 记录可选截图读回
  5. IFrameContext::EndFrame
  6. Backend 解析截图与 GPU Timing
```

D3D12 和 Vulkan 的区别从第 3.e 步以下才出现：同一个 `DrawIndexed` 在 D3D12 Adapter 中变成 D3D12 命令，在 Vulkan Context 中变成 Vulkan 命令。高层不再维护两套业务代码。

## 5. Slang 在这里负责什么

Slang 解决的是 Shader 源码和目标二进制的跨 API：

```text
同一份 Shader/入口/宏
  -> Slang
       -> DXIL（D3D12）
       -> SPIR-V（Vulkan）
```

它不会替代引擎的 RHI。Shader 二进制之外仍然需要创建 Texture/Buffer、Pipeline、Descriptor、Command、Barrier、Swapchain 和同步对象。这些工作已经只实现于两个 RHI Backend，而不是在两个高层 Renderer 中重复实现。

## 6. Feature 自注册的含义

以 TAA 为例，`TemporalAntiAliasing::RegisterRenderGraph` 负责提交：

- Motion Vector、Resolved、History Read/Write 资源。
- 这些资源进入图之前的状态。
- Temporal Resolve Pass 的执行回调。

`SceneRenderer` 不再逐项读取 TAA 内部资源，也不再自己构造 TAA 回调。共享 Frontend 仍集中保存全局 DAG 的先后依赖，这是有意保留的：Feature 拥有资源与执行，Frontend 拥有跨 Feature 的拓扑规则。这样既避免“大 Renderer 知道所有内部细节”，也避免 Feature 相互 include 并自行猜测全局顺序。

## 7. 关键文件变化

新增：

- `src/Core/ApplicationHost.h/.cpp`
- `src/RHI/IFrameContext.h`
- `src/RHI/IRenderBackend.h`
- `src/RHI/RenderBackendFactory.h/.cpp`
- `src/RHI/D3D12/D3D12RenderBackend.h/.cpp`
- `src/RHI/Vulkan/VulkanRenderBackend.h/.cpp`
- `src/Renderer/SceneRenderer.h/.cpp`
- `src/Renderer/Features/*`

移除：

- `Core/VulkanApplication.*`
- D3D12/Vulkan 两套旧高层 SceneRenderer
- `Renderer/TriangleRenderer.*`
- `Renderer/VulkanTriangleRenderer.*`
- `Renderer/PSOCache.*`
- RHI 根目录旧原生 `VertexBuffer/IndexBuffer` 包装

主要修改：

- Asset/Scene 的 GPU 创建入口改用 `IGraphicsDevice`。
- D3D12/Vulkan Context 实现公共帧接口。
- Application、截图、GPU Profiling 和 RenderGraph 报告改走公共 Backend。
- CMake 源清单按 Application、Engine、Renderer、Editor 模块拆分。

## 8. 验证结果

Debug 全量构建成功。

CTest：

```text
8/8 passed
RhiTypeTranslation
ShaderCompiler
VulkanRuntime
RenderGraph
GoldenImageMetrics
EngineHarness
WorldRenderSceneBridge
MinidumpSymbolizer
```

真实运行：

```text
PrismRender.exe --api=d3d12  -> exit 0
PrismRender.exe --api=vulkan -> exit 0
```

严格跨 API Tonemap Golden Image：

```text
dimensions_match=true
mean_absolute_error=0.000016
root_mean_square_error=0.000252
changed_pixel_ratio=0.000000
maximum_channel_error=0.015686
structural_similarity=0.999998
```

阈值为 MAE `0.001`、RMSE `0.005`、变化像素比例 `0.01`，本次结果全部通过。

## 9. 仍然值得继续拆的部分

这次解决的是“重复架构”和“API 边界”，不是把所有大文件一次性改写。下一阶段最有价值的工作是：

1. 将 `ApplicationHost` 的编辑器动作、World 同步和 Asset Streaming 编排拆成服务，Host 只保留生命周期。
2. 将 `RenderGraph.cpp` 的 Builder、Compiler、Scheduler、Executor 分文件，但保持现有行为和测试。
3. 将 GPU Profiler 的 D3D12/Vulkan 原生实现进一步拆入各自后端文件，保留 `RHI/Profiling` 公共门面。
4. 将 D3D12/Vulkan Context 中的 Swapchain、Upload、Descriptor 和 Queue 管理器继续拆类。

这些属于可继续演进的内部粒度问题，不再是两套高层渲染路径并存的问题。
