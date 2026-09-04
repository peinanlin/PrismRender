# PrismRender 渲染器架构

本文描述 `refactor-renderer-architecture-boundaries` 完成后的生产架构。PrismRender 仍是实时渲染器，不扩展为通用游戏引擎；D3D12 与 Vulkan 共用上层渲染结构，默认使用主线程、渲染线程和常驻录制 worker，当前没有独立 RHI 线程。

## 1. 顶层所有权

`ApplicationHost` 是组合根和主循环，不再拥有场景、编辑器、捕获或资产系统的业务状态机。

| Owner | 主要职责 | 不负责 |
| --- | --- | --- |
| `ApplicationHost` | 创建/连接服务、主循环、按 RAII 顺序关闭 | 场景命令实现、Feature 算法、UI 面板逻辑 |
| `SceneSession` | World、可编辑 RenderScene、命令/事务、相机、变更跟踪、帧数据发布 | GPU 设备、ImGui、渲染 Pass |
| `AssetRuntimeCoordinator` | CPU 导入/流送结果、render-lane 上传请求、绑定 revision | 直接改写已发布帧 |
| `EditorCoordinator` | ImGui session、面板、文件操作和窄 Scene action | Host 回指、Render lane 可变状态 |
| `CaptureAutomationController` | 固定帧捕获、报告、max-frames、Water validation 编排 | 伪造生产帧完成 |
| `RenderExecutionService` | inline/threaded 共用执行协议、有界 FIFO、可靠控制、完成/反馈 | 第二套渲染实现 |
| `RenderRuntimeExecutionTarget` | backend、frame context、`RenderFrameCoordinator`、上传、建图、提交、Present | GLFW 事件和可变 ImGui context |
| `RenderFrameCoordinator` | Game/Scene renderer、共享资源、每逻辑帧共享生产者选择 | 文件、命令 JSON、环境变量解析 |

构建边界由 `cmake/PrismModuleBoundaries.json` 和 CMake 检查强制。主要 target 为 `PrismCore`、`PrismPlatform`、`PrismRHI`、`PrismAsset`、`PrismEngine`、`PrismScene`、`PrismRenderer`、可选 `PrismEditor`、`PrismApplication`。RHI 不依赖 UI，Renderer 不反向依赖 Application；每个生产源只有一个 owner。

## 2. 三类 CPU 执行 lane

- Main lane：GLFW 事件、输入、World/SceneSession、相机、Editor/ImGui 构建、自动化状态，以及 N+1 帧不可变输入准备。
- Render lane：backend 与 frame context、GPU 上传安全点、Feature 可变状态、RenderGraph 建图/执行、提交、Present、capture resolve。
- Worker pool：只录制 `GraphParallelRecordingContract` 允许并行且拥有独立 command context 的任务。descriptor、PSO/cache、动态上传、Feature 共享状态和提交仍由 Render lane 独占。

默认配置：

```text
PRISM_RENDER_EXECUTION_MODE=threaded
PRISM_RENDER_TASK_EXECUTOR=pool
```

兼容/排障回退：

```text
PRISM_RENDER_EXECUTION_MODE=inline
PRISM_RENDER_TASK_EXECUTOR=inline
```

inline 与 threaded 使用同一个 `FrameEnvelope`、`RenderExecutionService`、target 和 `GraphExecutor`。回退不会选择旧 renderer。独立 RHI 线程未实现；当前 API submit/Present CPU 区间不足以成为可分离主瓶颈，依据见 `RHI_THREAD_DECISION.md`。

## 3. 六种不可混淆的生命周期

| 生命周期 | 身份/边界 | 典型数据 |
| --- | --- | --- |
| Device | backend 创建至销毁 | ShaderManager、PipelineCache、共享 IBL |
| Logical frame | 单调 `LogicalFrameId` | 模拟时间、共享 FFT/foam/local-wave 一次生产 |
| Frame slot | backend frames-in-flight 回绕 | descriptor/upload/retirement ring；不能代替逻辑帧 |
| View | 稳定 `RenderViewId` + view epoch | 相机、extent、TAA/光学历史、选择/反馈 |
| Scene data | scene generation + data/binding revision | 不可变对象、材质参数值、GPU binding lease |
| RenderGraph | graph generation + resource version | 当前图 handle、barrier、queue plan、执行状态 |

CPU `shared_ptr` 保活和 GPU fence 完成是两个边界。对象析构、逻辑帧增长或 frame-slot 回绕都不能替代 GPU 完成证明。

## 4. 帧数据流与线程交接

```text
Main N+1                                      Render N
Poll GLFW / input                             dequeue acceptance N
apply completed binding feedback              apply ordered controls
commit World mutations                        BeginFrame / safe uploads
extract or reuse RenderSceneData              choose one shared producer
build RenderView + dynamic lights             Game + optional Scene render
copy UiDrawPacket with texture leases         execute graph / draw UI
freeze FrameEnvelope                          capture / EndFrame / Present
wait before the single N+1 submit   <------   completion + latest feedback
submit N+1                         ------->    dequeue N+1
```

`RenderFrameQueue` 最多保留一项 waiting 加一项 executing。已接受 frame/control 严格 FIFO 且恰好执行一次；队满使用背压，不替换旧帧，也不通过丢模拟帧制造 FPS。

`FrameEnvelope` 冻结 `LogicalFrameId`、scene/data/settings revision、scene/view epoch、时间/步长、每视图相机与设置、`RenderFramePacket`、`UiDrawPacket`、capture/profiling 请求和输入时间戳。Render N 执行期间 Main 可准备 N+1，但不会准备第二个未提交帧。

Render lane 返回两类结果：

- `RenderFrameCompletion`：可靠且有序，用于 capture、max-frames、退出和失败传播。
- `RenderFrameFeedback`：容量一的 latest-only 统计，用于 UI；旧 scene/data/view/frame 身份会被拒绝，不能代替控制确认。

resize、history reset、quality、scene switch、capture、资产 render work/import、报告、drain 和 stop 使用 `RenderControlCommand`。命令带 ID、目标帧、scene/view epoch 和 `BeforeFrame`/`AfterFrame` 边界，ack 必须匹配身份。

## 5. 版本化 Scene 发布

静态/对象数据与每帧/每视图数据已经分离：

```text
World / editable RenderScene
  -> SceneChangeTracker commit set
  -> RenderSceneExtractor
       unchanged: reuse shared_ptr<const RenderSceneData>
       changed: rebuild one coherent revision
       unknown raw mutation: conservative full rebuild + diagnostic reason
  -> RenderFramePacket {
       LogicalFrameId, simulation time/step,
       immutable RenderSceneData,
       small RenderFrameDynamicData,
       vector<RenderView>
     }
```

`RenderSceneData` 保存稳定对象身份、层级/实例映射、值化材质参数和资源 binding lease。相机、灯光、时间、extent、历史失效和选择属于小型动态输入；只移动相机不会复制完整对象数组。

mutation 范例：一次事务同时修改 transform 和 material 时，命令系统只在 commit 边界产生合并后的 change set；`RenderSceneExtractor::Extract` 发布一个包含两项修改的新 data revision。旧在途 packet 继续持有旧参数与绑定。若流送上传稍后完成，render lane 返回有序 binding completion，Main 在后续帧发布新的 binding revision，不原地覆盖旧帧。

默认策略为：

```text
PRISM_RENDER_SCENE_PUBLICATION_MODE=versioned
```

诊断回退 `full-rebuild` 仍使用相同 packet 和 renderer，只禁止复用。未分类写入选择保守重建并记录原因，不使用每帧 O(N) 内容哈希猜测是否变化。

## 6. Render Feature 接入

Feature 通过 `RenderFeatureRegistration` 注册稳定 ID、`DeviceShared`/`ViewLocal` scope、阶段、依赖、初始化前置条件和所需操作。它把现有具体类方法绑定为回调，不要求继承庞大基类。

阶段顺序是：`FramePreparation`、`Shadows`、`Opaque`、`Lighting`、`WaterOptics`、`Transparent`、`Temporal`、`PostProcess`。可用操作为 `Initialize`、`PrepareFrame`、`BuildGraph`、`Resize`、`SceneChanged`、`Shutdown`；Feature 不需要为空的可选操作提供占位实现。

接入范例：

1. 在 Feature 自有目录定义类型化 graph inputs/result；私有 scratch 不进入共享全集。
2. 在 `BuiltInRenderFeatures` 组装 `RenderFeatureRegistration`，声明真实 scope、stage 和 required dependency。
3. `RenderFeatureRegistry::ResolveLifecycleOrder` 在 GPU 提交前拒绝重复 ID、缺失依赖和依赖环。
4. 通过当前 graph 的类型化 Blackboard 发布/读取带 generation/version 的资源；必要输入缺失、类型错误或旧 graph handle 显式失败，可选输入声明中性后备。
5. Resize/SceneChanged 只进入匹配 scope。Shutdown 按已初始化依赖逆序且幂等；在途 GPU 资源进入原 frame/fence retirement。

共享 Spectral FFT、foam 和 local waves 按 `LogicalFrameId` 由 `RenderFrameCoordinator` 选择一个有效 view 生产一次；Game/Scene 消费同一版本。TAA、WaterOptics、屏幕空间临时资源及历史按 `RenderViewId` 隔离。Fluid 保持已完成的原作用域，不因本次架构工作改变算法。

## 7. RenderGraph 内部边界

公开 `RenderGraph` facade 保持建图语义，内部拆为：

- `GraphDescription`：声明的 Pass、资源和访问；
- `GraphCompiler`：校验、liveness、裁剪、依赖 DAG；
- `ResourceLifetimePlanner` / `TransientAliasPlanner`：生命周期与别名；
- `ResourceStateTracker`：逐资源/子资源状态与 barrier；
- `QueueScheduler`：graphics/compute queue 计划与 fence；
- `CompiledGraph`：不可变编译计划；
- `GraphExecutionState` / `GraphExecutor`：本次执行、worker 录制、稳定收集与提交。

并行录制完成顺序可以不同，但 Render lane 按编译计划稳定收集；任一任务失败时不提交部分帧。HPWater 已修复的跨队列 release、首 compute prologue、fence 和逐 mip 状态语义保留。

## 8. UI、Profiler 与安全退出

Main 在 `ImGui::Render` 后复制顶点、索引、draw command 和纹理 lease；Render lane 只消费不可变 `UiDrawPacket`，不访问可变 ImGui context。FPS、帧时间和 FFT/WaveWorks 场景控制位于 Game viewport overlay；全局 Debug 页面只保留跨场景项。

`FrameProfilerSnapshot` 使用 240 帧有界 ring，区分 Editor Loop、Main/Render/Worker、Game/Scene、GPU、Submit/Present/Wait、queue depth、N+1 overlap 与 input-to-Present。threaded 当前增加约一帧输入延迟；不能将线程数量、GPU Pass 时长倒数或积压当作 FPS 收益。

正常关闭顺序：停止接受 → drain 已接受工作 → 收拢 worker → Render lane 等待必要 GPU 完成 → UI backend → renderer/shared resources → assets/backend → Main 的 Scene/Window。失败保存首异常，取消未执行项并唤醒 producer、completion/control 和 shutdown 等待者；重复或并发 Shutdown 汇合到同一完成状态。

## 9. 哪些改变了，哪些没有

保持外部行为的拆分：ApplicationHost 服务提取、RenderGraph 内部类拆分、CMake target 对齐、ImGui backend 文件归属和 UI 面板重组。这些不改变 Demo 默认设置、shader、Pass 算法、场景/材质文件、CLI 或截图含义。

真实的内部契约变化：Feature 生命周期与依赖校验、类型化 graph 数据交换、不可变 versioned scene/frame packet、可靠跨线程控制、UiDrawPacket 值拷贝、默认 threaded+pool。仓库内 C++ 调用方已经迁移，因此内部头/API 不是旧版源码兼容面。

明确未做：没有重写 HPWater/WaveWorks/Fluid 算法，没有独立 RHI thread，没有扩大 GPU async compute 策略，没有通过降低画质或跳帧换取性能。WaveWorks/Ocean 的其他 OpenSpec change 也没有被本 change 自动标记完成。

## 10. 定位入口

- 应用组合与执行：`src/Core/ApplicationHost.*`、`src/Core/Application/RenderExecutionService.*`、`RenderRuntimeExecutionTarget.*`
- 场景与发布：`src/Scene/SceneSession.*`、`RenderSceneExtractor.*`、`RenderFramePacket.*`、`RenderDynamicInputState.*`
- Feature：`src/Renderer/Features/RenderFeature*.h`、`src/Renderer/RenderFeatureRegistry.*`、`BuiltInRenderFeatures.*`
- RenderGraph：`src/Renderer/Graph/` 和 `src/Renderer/RenderGraph.*`
- UI：`src/UI/EditorCoordinator.*`、`UiDrawPacket.*`、`GameViewportDebugOverlay.*`
- 模块规则：`cmake/PrismModuleBoundaries.json`、`cmake/CheckModuleBoundaries.cmake`
- 跨线程细节：`docs/RENDER_EXECUTION_PROTOCOL.md`
- RHI 线程结论：`docs/RHI_THREAD_DECISION.md`
- 验收与回退：`docs/ARCHITECTURE_REFACTOR_ACCEPTANCE.md`
