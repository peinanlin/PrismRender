## Context

动机见 `proposal.md`。本文是完整实施设计，不是已实施报告；文中的新增文件、target、接口和门限均为本 change 的计划。

2026-08-28 只读核对结果：

- `add-hpwater-ocean-optics-lab` 记录为 38/38 完成；其基线文档为 `docs/HPWATER_OCEAN_LAB_CN.md`，现有证据位于 `artifacts/hpwater-validation/`。本次规划未重新运行该验收。
- `ApplicationHost` 仍管理窗口、后端、两套 SceneRenderer、共享资源、World 命令、可变 RenderScene、资产、相机、编辑器和截图。`WaterValidationSequence.cpp` 仍定义 Host 成员方法。
- `SceneRendererPipeline.cpp`、`SceneRendererResources.cpp` 与 `RenderGraphCompiler.cpp` 已经分文件，但仍实现原类的成员函数，不能将已有分文件重复计为架构解耦。
- `RenderFeatureRegistry` 目前是固定回调序列；已有类型化 `RenderGraphBlackboard` 和带 generation/version 的图句柄，直接复用而不是再建一套。
- `WaterOpticsFeature` 已有 Initialize/Shutdown、资源回收、历史、AddPasses 与独立光学资源；它是生命周期适配样本，不重写其算法。
- `RenderSceneMailbox::Publish` 复制场景；Game/Editor view 构造再次复制。RenderObject 包含可变 shared_ptr 资源引用，因此 const 快照不自动等于深层不可变。
- Asset/Scene 目前编入 PrismRenderer；基础 Core/Window 编入 PrismRHI；ImGui 后端文件虽位于 RHI 目录，却属于 PrismEditor target。
- HPWater 联调修复涉及传递约简后的资源释放、首个 compute 访问的 graphics prologue、跨队列 fence、逐 mip 状态恢复和呈现同步。不能用“保持公开 API”替代这些内部语义的回归。
- `fix-waveworks-ocean-controls-spectrum-and-foam` 仍为 36/38，migration visibility 为 0/16。它们不是本次实施任务；实施前确认是否有人正在修改共同文件，不要求为启动本 change 而完成所有无关工作。
- 当前 UI 将 `FrameTimer` 的整个 Editor 主循环值显示为未限定范围的 FPS，同时只展示 Game `SceneRenderer` 的 CPU/GPU 总时长；主循环实际顺序执行 Game 与可见 Scene 两个视图。已有离线 recorder 能细分部分阶段，但没有常驻低开销时间线、线程/lane、等待原因和 active-view 范围的统一实时快照。

## Goals / Non-Goals

**Goals:**

1. 保留既有 renderer 分层和图形算法，降低一个功能变更需要理解/修改的模块数量。
2. 应用服务和 Feature 对自己的状态负责；依赖通过窄接口传递，避免 Host/SceneRenderer 成为任意访问入口。
3. 明确设备、逻辑帧、帧槽、视图、场景版本和图代次六种不同生命周期。
4. 分开进行结构迁移和发布机制优化，每个阶段有独立提交边界、证据和回退方式。
5. 对新生命周期与快照 API 建立正式规格；纯保持行为的实现拆分由设计、测试和依赖规则约束。
6. 边界稳定后引入常驻 worker pool 与独立渲染线程，使主线程帧准备可以与上一帧渲染重叠；保留 inline 对照路径，不增加独立 RHI 线程。
7. 全部 20 个现有 Demo 的静态图像和时域行为不退化，算法、默认画质、模拟步数及交互语义保持。
8. 在结构迁移前提供可直接判断 Main/Render/Worker/GPU/等待瓶颈的实时 Profiler 和公平场景对照，使后续线程化收益、延迟与观察开销可被同一套数据证明。

**Non-Goals:**

- 不新增游戏引擎系统、通用 ECS、插件 DLL、脚本运行时或第三个图形后端。
- 不重写 FFT、光学/焦散/体积算法、PBF、地形、LOD/剔除算法、PSO key 或队列成本模型。
- 本 change 不直接引入独立 RHI 线程，不扩大异步 GPU 重叠范围；P0–P5 保持线程模型，P6/P7 单独引入并验收 CPU 并发。P7 后只形成基于证据的 RHI Thread 决策，实际新增属于后续明确范围。
- 不以管理员锁频、驱动全局设置或机器级 Vulkan ExplicitLayers 注册作为结构阶段硬门禁；这些系统状态实验不是运行时功能，也不能替代普通环境的正确性验证。
- 不强制所有 Feature 继承一个庞大基类；不把每个函数都拆成一个类。
- 不以文件行数作为通过条件；不进行全仓格式化、命名清洗或无关依赖升级。
- 不承诺 FPS 提升。快照复制消除有可测目标，其余主要是维护性收益；GPU 工作量原则上不增加。

## Decisions

### D0. 先建立低开销实时 Profiler，再解释或改变线程模型

新增 `FrameProfiler` 作为应用组合层拥有的有界采样器，生产不可变 `FrameProfilerSnapshot`；Editor 面板只消费上一已完成快照，不直接读取正在变化的 renderer/backend 状态。首版保留最近 240 个完成帧，使用预分配 ring buffer，未知或尚不存在的 lane 显示为 unavailable，不用 0 ms 冒充已测量。

统一指标范围：

| 指标 | 含义 |
| --- | --- |
| Editor Loop | 相邻主循环边界，包含实际执行的 Game/Scene/UI/Present；现有 FPS 改名为 `Editor Loop FPS` |
| Main | 事件、输入、Editor 构建、场景提交及未来 FrameEnvelope 生产中的有效 CPU 工作，等待单列 |
| Render | 当前 inline 时为主线程中的渲染区间；P7 后为渲染执行 lane 的工作区间 |
| Worker | P6 后任务排队、录制、join、活跃 worker/队列峰值；此前标为 unavailable |
| Game / Scene | 两个视图各自 CPU Render、GPU total、extent、是否实际刷新及历史来源 |
| Submit / Present / Wait | BeginFrame fence/reclaim/acquire、上传、submit、native present、signal 和背压等待 |
| GPU | 已完成帧的总区间及逐 Pass/queue 详情；带实际 resolved frame/generation，绝不与当前 CPU 帧错配 |

分析等级在启动配置与 Editor 控件使用同一枚举：`off` 关闭可选观察，`basic`（默认）保留 loop、主要 CPU 区间、active views、粗粒度每视图/GPU total 和等待类别，`detailed` 增加逐 Pass GPU timestamp、RenderGraph/cache 与细阶段，`capture` 固定 detailed 字段并写出可重放报告。切换只影响后续帧，快照记录请求等级、实际等级和 profiler overhead；任何等级都不得调用每帧 `WaitForGpu`。自动性能/capture 工具显式选择等级，不能把不同等级样本比较为同一协议。

Scene View 调试策略为 session-local `catalog-default/live/on-interaction/30hz/paused`。默认 `catalog-default` 保留 DemoSceneCatalog 现有行为；限频/暂停只影响 Scene 视图刷新，Game、共享模拟生产、capture 请求和显式 Scene capture 仍遵循原契约，UI 明示当前活跃视图和策略。它是归因工具，不以停 Scene 或降画质作为默认性能优化通过条件。

公平场景矩阵固定同一尺寸、后端、构建配置、验证状态与 profiling level，至少包含 Editor Game+Scene、同 EXE Game-only、无 Editor/Standalone Game-only，以及专用空负载和完整 preview。空负载通过测试/性能入口构造，不新增或改变 20 个 Demo 的目录/default。跨引擎 FPS 仅作背景；Prism 的结论来自同二进制 A/B 和 CPU/GPU 时间线。

GPU DVFS、焦点和显示状态继续记录，但 P0 不再要求管理员锁频或跨批绝对 GPU 分布稳定。正确性门禁使用普通用户可运行的 strict validation、图像/结构/连续帧；性能特征门禁使用同进程/相邻 A/B、CPU lane 数据和匹配条件的独立重复。锁频证据可保留为旁路，不写机器级 Vulkan 注册表。

替代方案：仅把现有 FPS 改名，或先增加 RenderThread 再补时间线。前者无法定位等待/双视图/观察开销，后者无法证明线程化是否真正重叠或只是增加积压，因此拒绝。

### D1. 固定兼容边界，分阶段替换内部组织

P0 实测发现的 no-Editor 链接错误及 lights D3D12 描述符分配/Vulkan 子资源布局问题，已获用户确认作为前置修复纳入（tasks 1.11–1.14）。允许为这些明确问题修改必要的编译边界、资源分配/初始化/状态路径，但不借机重写后端或改变算法/画质/队列策略。修复前原始快照与可运行场景输出保留，修复后另建恢复点；修复结果和结构迁移结果分开归因。

外部兼容：保持 CLI/环境变量语义、场景/材质/World 文件格式、undo/redo/journal、Lab 默认值、UI 操作、截图输出含义、既有机器报告字段。新诊断只增加命名字段，不更换旧字段语义。

后续确认的 P0 修复：D3D12 在 layout 内按完整原生 sampler 描述和设备实例复用不可变表，集合更新采用替换，最后引用通过原帧 fence 退役；不增加 sampler heap 容量。SkyAtmosphere 为关闭状态提供已初始化黑色 2D 后备，每帧槽根据当前设置刷新所有消费者，开启时仍使用原 LUT 和 Pass。完整 FFT fixture 按实际资源初态和呈现契约校正 barrier，并在两后端显式启用校验，不改变计算 shader 或数值容差。以上分别对应 1.12、1.13、1.15，严格校验和 Demo 对照是完成条件。

Sampler 表在绑定时按最终配置延迟驻留，避免依次写入多个 binding 时分配短命中间表；每集合的发布互斥锁覆盖多线程录制首次绑定。反射中保留但 shader 未使用的 sampler 槽位采用合法默认描述，不强加“所有反射 binding 必须写入”的新契约。已写入槽位仍使用完整原描述，配置不变不产生分配。

用户已批准 tasks 1.16：ScreenSpaceEffects 持有已上传的黑色 1×1 ShaderResource 后备。Resize 重建 descriptor 时先绑定后备；Update 只改当前安全帧槽的 planar binding，使用与 PlanarReflections producer 相同的 `deferredEnabled && planarReflectionsEnabled` 条件选择真实输出。真实输出仍通过原 RenderGraph Pass/读依赖转换，后备永久只读，无额外模拟或绘制。GPU fixture 保留默认 deferred+SSR 路径并覆盖两个独立视图的关闭/开启/关闭、每帧槽和尺寸重建，双后端显式严格校验及原二进制图像对照通过后才勾选。

用户已批准 tasks 1.17 的统一可选资源绑定审计。按 producer predicate、实际 shader/descriptor 消费、图中读依赖和跨帧状态四项建立清单；GTAO 关闭时由 ScreenSpaceEffects 提供已上传的白色中性 AO 后备，DeferredLighting 和 SSR 只在当前安全帧槽切换，开启时继续读取原 AO 输出。其他可选资源若已有有效只读图访问/初始化或 consumer 完全不执行，则记录依据，避免无关改写；发现同类无效绑定时在本授权范围内修复并扩展负例。覆盖双视图、首次关闭、开关往返、尺寸重建、native/serial 及全 Demo 严格校验，保持所有开启算法与旧图像门限。

渲染兼容：固定输入下保留输出、Pass 名称与依赖、资源读写/最终状态、历史失效条件、共享模拟次数及现有队列模式。新增类型化发布层不是新的 GPU Pass；诊断可按逻辑资源标识比较，忽略地址、计时和新的内部对象名称。

用户已批准 P0 tasks 1.18–1.19：修复四个 Fluid Demo 的 Vulkan native 共享输入布局错误，以及 PBF 同二进制重复捕获的不稳定。1.18 先核对环境纹理在图形与计算消费者中的同一资源身份及完整读依赖，允许补齐原图漏声明的依赖，不改变队列调度算法或强制 serial。1.19 先验证固定时间、初始化、捕获身份，再检查模拟读写竞争与邻居遍历次序；只修复确定性/同步缺陷，不改变 PBF 方程、默认参数或模拟步数。原始失败样本保留，修复后的三次独立捕获及同后端原图分别验收；稳定性通过不自动代表原图兼容通过。

内部 C++ 接口可以变化，但每一步同步迁移所有仓库内调用方。暂存适配器必须有删除任务，不长期维护两套生产逻辑。

用户已单独批准三组稳定视觉参考修订：PBF D3D12、PBF Vulkan、Fluid Toon Vulkan，完整 Editor / RelWithDebInfo、单 Game view、native、1280×800、默认设置、确定性第 30 帧。登记于 `scripts/ArchitectureVisualBaselineRevisions.json`，包含原图与新图 SHA-256、三次独立捕获、二进制与源码检查点身份及原失败比较。按精确配置匹配，不外推到无 Editor、其他时间/视角/质量/尺寸；旧 V2 失败仍是失败，不覆盖文件或改变门限。比较入口只有显式 `-UseApprovedRevisions` 才使用修订，否则继续对原参考验收。

替代方案：一次整体重写。拒绝，因为无法归因同步、图像和生命周期回归。仅拆文件也不足，因为不改变依赖/所有权。

用户已批准 P0 task 1.20：独立 OceanTessellation GPU fixture 按实际后端初态声明 barrier（D3D12 Storage 缓冲创建/上传后为 UAV，Vulkan 上传输入最后访问为 CopyDestination、未使用输出为 Undefined；BeginFrame 后 backbuffer 分别为 RenderTarget/Present）。只给测试 `Ocean/OceanDomain.slang` 补齐 Hull 已有的 fractional_odd，不改生产 OceanSurface。固定 CTest 校验请求和 Vulkan 诊断失败条件；两种 Windows 构建、CPU、三个 fixture 配置及完整 GPU suites 验证，所有光学数值断言/容差逐字保持，Demo/HPWater 同后端图像复核后才完成。原失败报告不覆盖。

### D2. ApplicationHost 仅做组合与主循环，状态随职责迁移

目标所有权如下；箭头表示拥有，借用依赖不取得所有权：

```text
ApplicationHost
  -> Window + IRenderBackend + FrameTimer
  -> AssetRuntimeCoordinator -> ImportService / Registry / StreamingManager
  -> SceneSession -> CommandProcessor（内部拥有 World）/ editable RenderScene
                   / scene identity / camera controllers / change tracker / mailbox
  -> RenderFrameCoordinator -> SceneRendererSharedResources
                            -> Game SceneRenderer / Editor SceneRenderer
  -> optional EditorCoordinator -> ImGuiSystem / EditorLayer / panels
  -> CaptureAutomationController -> capture/report state / WaterValidationSequence
```

- `SceneSession` 位于 Scene，借用 AssetRegistry；负责 World/render bridge、保存/加载/事务结果、场景脏标记及导航状态。不创建图形设备、选择 shader 或调用 ImGui。Demo activation 返回场景变更结果；渲染 preset 仍由 Renderer 按 DemoSceneId 应用。
- `AssetRuntimeCoordinator` 位于 `Core/Application`，作为应用服务协调资产 IO、上传完成和驻留事件，只有约定的渲染准备线程调用 GPU 上传/Registry 更新。CPU IO worker 不直接改渲染快照。
- `EditorCoordinator` 位于 UI，消费 SceneSession 的动作接口和只读统计；不持有 ApplicationHost 指针，不访问其私有状态。文件对话框和 ImGui 后端仍只属于编辑器。
- `CaptureAutomationController` 位于 `Core/Application`，通过明确动作回调请求场景、相机、质量变化和截图；不直接进入 SceneRenderer 私有成员。保留延迟截图、流送就绪、max frames、报告和退出码条件。
- `RenderFrameCoordinator` 位于 Renderer，管理视图及共享生产者策略；不处理文件对话框、JSON 编辑命令或自动化环境变量解析。

P1 提取时主循环保留原阶段顺序：事件/时钟与输入 → 验证/编辑事务 → 场景同步 → BeginFrame → 上传完成/流送激活 → 发布帧输入 → Game/按需 Editor 渲染 → 截图/报告/UI → EndFrame → 完成反馈。具体操作点以基线调用顺序为准，禁止提取过程中改变 BeginFrame 前后的 GPU 安全点。P7 再按 D10 显式迁移线程归属和交接，不在提取阶段顺手异步化。

退出顺序：停止接收操作、停止并 join IO worker、等待已提交 GPU 工作完成、销毁 UI 引用、销毁视图/共享资源和场景绑定、销毁资产、后端、窗口。初始化中途失败也清理已创建成员，不能依赖只有全部初始化成功才置位的单一标志。正常退出不为每个子对象重复 WaitForGpu。

替代方案：Coordinator 共享整个 Host 引用或 `friend`。拒绝，这仅搬移源码。也不建立全局 service locator/event bus；目前明确的回调与结果类型足够。

### D3. 渐进统一 Feature 契约，不替换完成的功能实现

使用一个小型 `RenderFeatureRegistration` 描述和 `RenderFeatureContext`，由现有 Registry 管理；回调适配现有类方法，不要求虚函数继承。注册包含稳定 ID、作用域、参与阶段、必要/可选输入、初始化/准备/建图/尺寸变更/场景通知/释放操作。必要操作校验，可选操作可省略。

生命周期：Uninitialized → Initialized → Prepared → GraphBuilt/Executed → Initialized；尺寸、场景、配置变化作为显式事件。Shutdown 幂等；部分初始化失败由依赖逆序清理已经成功初始化的实例，并清理失败实例的已取得资源。

作用域分离：

| 数据/工作 | Owner | 更新依据 |
| --- | --- | --- |
| ShaderManager、PipelineCache、IBL | SharedResources | Device lifetime |
| Spectral FFT、foam、local waves、共享 terrain 更新 | 共享模拟 owner，由 RenderFrameCoordinator 指定一次生产 | LogicalFrameId，不是 frame slot |
| GBuffer、WaterOptics、屏幕空间输出和 TAA/volume history | 对应视图实例 | ViewId、extent、camera/history revision |
| RDG handles/Blackboard 发布值 | 当前 RenderGraph | Graph generation/version |

首阶段保持 Game 先生产、Editor 后消费的现有顺序；每个逻辑帧选择一个有效生产视图，若默认生产视图不可用则选第一个需要该模拟的有效消费者。没有消费者时不推进该工作链；恢复时沿用现有时间政策。生产链可以嵌入该视图图的既有位置，不额外创建第二套 FFT 图或纹理。生产者完成的 GPU 依赖仍通过当前 RHI 队列和资源状态规则保证，不能只用 CPU bool 表示可采样。

区分 mutable GPU state 与 immutable CPU scene：海洋纹理按原生命周期持续更新，快照复用不阻止模拟与光学逐帧工作。Fluid 暂时保持现有作用域，迁移前以基线确认，不能因为名称相似擅自改成共享模拟。

新增 `BuiltInRenderFeatures` 只做内置功能组装，允许依赖具体 Feature 类；其余生命周期调度只依赖描述和上下文。先迁移一个低耦合 Feature，再 WaterOptics/Fluid，再其余功能。SceneRenderer 保留几何 draw 构建与核心 opaque/transparent Pass；没有必要为每个 draw 都加一层接口。

替代方案：一次改所有 Feature 或引入通用动态插件系统。拒绝，增加无关复杂度。统一 lifecycle 不意味着统一内部资源结构。

### D4. 复用类型化 Blackboard，收缩共享资源/回调全集

`SharedRenderGraphFrontend` 保留场景主干阶段与阶段扩展点：已有 shadow/opaque/lighting、水光学、透明、temporal、bloom/tonemap 顺序由基线确定。Feature 声明阶段内的资源输入/输出，依赖排序稳定；新阶段本身需要显式修改主干，不能承诺任意新管线完全无需修改主干。

- `FeatureGraphTypes` 只定义通用场景资源契约，如 OpaqueSceneInputs、LitSceneOutputs、CompositeSceneOutputs，附生产者与版本。
- `WaterOpticsGraphInputs/Result` 移至 Ocean 自有 `WaterOpticsGraph.h`；Fluid 的子图契约移至 Fluid 自有头文件。其他 Feature 同理，不把私有 scratch 加进新的全集。
- Blackboard 生命周期仍是每个 graph，不能缓存上一帧的 handle。必要输入缺失、重复生产者、同类型多视图冲突在建图时校验；每视图使用独立 graph/blackboard，输出覆盖采用显式的新版本发布而不是盲目 Set。
- Blackboard 声明不能取代 RDG 的 Read/Write/MarkOutput；两者都需要，并核对读写版本。只将资源放进 Blackboard 并不会自动产生 barrier。
- 暂存 legacy shared-struct adapter 供分批迁移，最终删除所有调用者已迁出的字段和适配器；新 Feature 测试禁止扩展旧结构。

替代方案：新增另一套字符串资源总线或将所有对象放入 `any`。拒绝，已有 typed handles/Blackboard 足够，必须保持验证能力而非隐藏依赖。

### D5. 快照先测量，再引入版本复用；首版允许脏帧整体重建

新 CPU 数据模型：

```text
RenderSceneData = immutable object/transform/material values
                  + stable object/parent/index mapping
                  + retained immutable runtime binding versions
RenderView = ViewId + camera/current-previous matrices + extent
             + history/cut revision + visibility selection/feedback
RenderFramePacket = LogicalFrameId + simulation time
                    + shared_ptr<const RenderSceneData> + views
```

与原 `WorldRenderSnapshot`（文件/自动化序列化用途）区别命名，不改变其格式。场景 generation 在整体切换时更新；data revision 在一次提交的对象/绑定变更时更新；相机、灯光等动态值可位于独立的小型帧数据块。视图选择是稳定索引/掩码，不复制 RenderObject 数组，不通过删除元素改变父索引或 firstInstance。

没有 Engine EntityId 的程序化节点分配场景内稳定 RenderObjectId；拓扑重建才更换对应 generation，并同步重建 GPU object records 与反馈映射。GPU readback 使用来源 SceneGeneration/DataRevision/ViewId，晚到结果不能修改新场景。

变更来源必须逐项接入：

| 来源 | 失效/更新 |
| --- | --- |
| Entity 增删、hierarchy、transform、visible/editorOnly、mesh/material override | 提交时 data revision |
| undo/redo、transaction commit、journal replay、world load | 一次一致性提交；失败/回滚不发布半更新 |
| glTF/Demo/streaming scene activation、程序化拓扑变化 | scene generation 或 data revision，更新索引映射 |
| AssetRegistry runtime binding 替换、upload complete、reimport/eviction | binding revision；旧帧保留旧资源 |
| Editor/验证脚本/无编辑器导航相机、灯光、设置 | 更新对应小型动态块/视图历史；不误判为静态 |
| GPU 可见性反馈与 Editor 辅助显示 | 视图反馈版本与选择映射，不原地修改已发布对象 |
| 海洋/Fluid/terrain GPU 模拟与时间 | 每帧 Feature 更新，不能依赖 scene data dirty |

`SceneChangeTracker` 接收带类别的变更；使用 `SceneEditScope` 或等价提交范围包装现有 mutable 入口。未知写入走 FullRebuild 并记录原因；迁移完成前默认仍 FullRebuild。不能仅缓存 `m_worldDirty`，也不能每帧扫描/哈希整个场景来伪装成免复制优化。

不可变绑定策略：在快照中固化 CPU material/transform 值并保留对应 mesh/texture/material binding 版本；更新通过替换新版本，不原地改旧快照能观察到的参数。可共享底层 GPU 资源，但它们的内容更新仍受原有 GPU 生命周期约束；const shared_ptr 不作为深层不可变证明。

首版脏帧可以整体重建一次，重点消除静态/仅相机变化帧的 N 次复制；不增加逐对象持久化容器复杂度。FramePacket 每帧发布，但引用复用的数据。P4 的 Mailbox 继续使用小互斥锁和当前线程模型，P7 才接入 D10 的有界 FIFO，不以 latest-wins mailbox 丢帧。图 lambda/并行录制任务持有 packet 或等价强引用直到录制结束；GPU 完成保活由已有资源 owner/retirement 完成。

新增 opt-in 诊断模式 `full-rebuild` / `versioned`，通过测试配置及 `PRISM_RENDER_SCENE_PUBLICATION_MODE` 选择；完成正反向对照后默认 versioned，保留显式 full-rebuild 诊断后备。这是同一输入模型的两种提取策略，不长期保留旧渲染入口。

替代方案：只在 World 编辑时刷新。拒绝，会漏资产、视图、模拟和 UI 直接修改。一次做全增量 ECS extraction 也拒绝，范围过大。

### D6. RenderGraph facade 保持不变，分离编译与执行状态

公开 `RenderGraph` 继续暴露 Declare/Import/AddParameterPass/Compile/Execute/diagnostics；内部按以下顺序提取：

```text
GraphDescription（声明、访问、版本）
   -> GraphCompiler
      -> liveness/culling + execution dependencies
      -> ResourceLifetimePlanner + TransientAliasPlanner
      -> QueueScheduler
   -> CompiledGraph（不可变计划，可缓存）
   -> GraphExecutor + ResourceStateTracker（当前执行独有）
   -> ICommandContext
```

- 编译产物不持有跨帧悬空资源指针/回调；cache 命中时绑定当前图资源，动态初始状态、别名池和描述变化按现有 fingerprint 规则失效。
- 保留声明访问图和约简后的执行 DAG 两份语义：队列释放不能只查约简后的边。别名重用形成的依赖不能在排序中丢失。
- ResourceStateTracker 保存逐 mip/layer 与 buffer range 状态，生成现有 barrier、UAV/alias barrier 和 queue handoff；每帧按实际导入状态恢复。
- 保留 graphics prologue 对首次 compute 使用资源的释放、准确 fence 等待、跨帧状态恢复、upload/readback 堆固定状态和相同原生状态不产生无效 transition 的行为。
- GraphExecutor 保留串行、native queue batches、parallel recording 的选择及完成等待。编译缓存不能复用前次执行的可变状态。
- 现有 QueueSchedulingCostModel 与 RHI backend 语义不改。平台呈现信号量修复保留，不顺带重构 VulkanContext/D3D12Context。

不先设计一个大型 GraphContext 把所有原私有成员重新塞回去。每个 planner 输入只读声明/前阶段产物，输出独立计划；Executor 只访问执行所需状态。纯编译逻辑可由 CPU fixture 测试。

替代方案：仅把成员函数移进更多 cpp。可作为短暂机械步骤，但不满足最终独立状态/输入输出边界。

### D7. 最后收紧构建边界，不做纯形式拆库

目标依赖方向如下，箭头为消费者依赖提供者；实际 direct edge 以源文件使用为准，不能依赖不必要的 PUBLIC 传递链接：

```text
PrismCore                         （通用 assert/math/environment/trace/timer）
PrismPlatform -> PrismCore         （Window/GLFW，无 Editor）
PrismRHI -> PrismCore, PrismPlatform
PrismEngine -> PrismCore           （World/commands/serialization，允许 JSON）
PrismAsset -> PrismCore, PrismRHI   （允许 Slang、JSON、tinygltf）
PrismScene -> PrismCore, PrismPlatform, PrismEngine, PrismAsset, PrismRHI
PrismRenderer -> PrismScene, PrismAsset, PrismRHI, PrismCore
PrismEditor -> PrismRenderer, PrismScene, PrismEngine, PrismAsset, PrismPlatform
PrismApplication -> 以上所需 target（PrismEditor 条件依赖）
PrismRender -> PrismApplication, 必要 Diagnostics
```

Scene 暂时保留工厂创建 GPU 资产的现实依赖；不为追求理想图而同时拆离线 cooker/runtime asset。PrismCore 不包含 ApplicationHost/Launcher/应用环境策略；这些仍可位于 `Core/Application` 或现有 Core 路径，但归 PrismApplication。路径和 target 的例外必须由源归属清单明确，不能仅按目录推断层级。

`PrismDiagnostics` 保持 Windows 条件边界；ProcessDiagnostics 的 POSIX/Windows 实现与应用链接保持可用。测试/工具按实际依赖链接；保留所有可执行文件名字、Slang/DXC runtime 拷贝和配置开关。

ImGui API 后端适配器迁至 `src/UI/Backends/D3D12` 与 `Vulkan`，只由 PrismEditor 编译；底层 RHI 不包含 UI。迁移只改位置/include/namespace 必要部分，不改绘制逻辑。FileDialog 也归 Editor，不泄漏进基础 PrismPlatform。

用 `cmake/PrismModuleBoundaries.json` 记录每个生产源归属、公开头文件与允许依赖，`cmake/CheckModuleBoundaries.cmake` 校验非法 include/target edge/漏归属/重复源。保留 `src` 根 include 以避免一次全仓 include 重写，但配合规则检测内部头文件越界；Header self-containment 用编译测试补充，文本检查不是完整 C++ 语义证明。测试 fixture 中有意重编译的源要单独声明，不允许生产库重复编译。

替代方案：只添加 PrismAsset/PrismScene target，然后继续把所有依赖 PUBLIC 链给所有目标。拒绝，不能形成有效边界。

### D8. 验收以行为、生命周期和可测复制量为准

性能和视觉门限见 Validation Method。结构验收额外要求：

- Host 不再包含水验证序列、资产状态机或 UI 统计拼装算法；每个服务拥有自己的状态，不出现任意 Host 反向引用。
- Feature 扩展 fixture 能在已有阶段接入，不修改 shared resource/callback 全集；私有 scratch 不外泄。
- 静态场景预热后连续 120 帧的 `sceneDataBuildCount` 增量为 0、完整对象复制量为 0；仅移动相机同样成立。动态对象每帧变更允许重建，不伪报复用。
- 快照统计至少包含 build/reuse count、objects/bytes copied、extraction CPU、fallback reason、scene/data/view revision；字节定义明确为 CPU 对象元数据复制估算，不宣称总内存或 GPU 显存。
- Graph planner 可单测，编译计划与每次执行状态不混用；target 依赖检查的故意违规 fixture 必须失败。
- Profiler 面板能同时看到 Editor Loop、Main/Render/Worker/GPU/等待、Game/Scene active view 和观察等级；关闭/基础/详细模式的开销和字段可区分，历史容量固定且不产生 GPU 同步读回。

### D9. 常驻工作线程池只并行独立录制，不改变图调度语义

当前 `RenderGraph.cpp` 使用逐 Pass 的 `std::async(std::launch::async)`，最后收集 future 再提交队列；它不是独立渲染线程。在 P6 用 Core 的 `TaskScheduler`、`TaskGroup` 和可注入 `ITaskExecutor` 替代任务启动机制。worker 数和待执行任务数有界，支持 inline 执行和小任务直执行；不增加运行时依赖。

- 编译/屏障规划保持在 render execution lane；仅声明 parallel-recordable 且拥有独立命令录制上下文的任务进入池。录制上下文、帧包和临时分配器保活到 TaskGroup 完成，资源/descriptor/PSO 缓存共享写必须经亲和审计，不默认所有设备方法线程安全。
- 每 Pass 独占结果槽；主执行 lane 按原稳定 Pass 顺序收集和回放。CPU worker 完成顺序不得改变 GPU batch/fence/Read/Write、模拟步数、随机数或历史输入。
- 任一录制任务失败要收集并完成/取消同组所有任务，禁止提交部分失败帧；异常上送执行 lane。禁止 worker 同步等待同池未完成任务造成饥饿；首版明确拒绝嵌套阻塞等待。
- 退出停止接收、唤醒队列等待者、join 已启动 worker 后释放帧数据；启动失败和空任务组也可安全清理。记录任务排队/执行/等待及 worker 峰值，不用线程数量等同加速。

### D10. 独立渲染线程采用有界帧交接和可靠控制协议

P7 的 `RenderExecutionService` 拥有并在执行 lane 管理后端、RenderFrameCoordinator、渲染资源和 GPU 上传；`ApplicationHost` 组合服务而不跨线程操作 backend。`inline` 模式在调用线程运行同一套执行代码，`threaded` 模式由一个常驻渲染线程运行。`PRISM_RENDER_EXECUTION_MODE=inline|threaded` 与 `PRISM_RENDER_TASK_EXECUTOR=inline|pool` 启动时解析并冻结；非法值失败。现有 GPU queue mode 独立且含义不变。先默认 inline，全部门禁通过再切默认 threaded，两个诊断开关长期保留。

| owner / lane | 可写状态与职责 | 交接约束 |
| --- | --- | --- |
| 主线程 | GLFW/窗口事件、ImGui 帧构建、编辑事务、World/SceneSession、相机与自动化控制 | 发布不可变 FrameEnvelope，不读取渲染器可变状态 |
| 渲染执行 lane | backend frame context、GPU 上传、Feature/历史/PSO、建图执行、UI draw、呈现/截图 | 只消费已接受帧及可靠命令，反馈带 scene/data/view/frame 标识 |
| 常驻 worker | 独立 Pass 命令录制 | 不调用 GLFW/ImGui，不写共享 Feature 状态，不直接提交 GPU |
| 资产 IO worker | 文件读取/CPU 解码 | 完成消息交 AssetRuntimeCoordinator；不直接改 snapshot 或 GPU 对象 |

`RenderFrameQueue` 初始容量为一帧等待加一帧执行。接受后的帧严格 FIFO、恰好消费一次，逻辑帧时间/步长取包内值，不用消费时墙钟，不随队列满而跳帧。主线程背压时仍可泵必要窗口事件；resize/close 不等待一个只有主线程继续执行才能满足的回调。关闭/失败状态唤醒双方，禁止无界队列或轮询忙等。

`FrameEnvelope` 在 Application 层组合 Scene 的 RenderFramePacket 与 UI 自有 `UiDrawPacket`，Scene 不反向依赖 UI。主线程 ImGui 产生顶点/索引/命令值拷贝，render lane 只绘制此副本，不并发访问 ImGui context。纹理标识必须配资源 lease/GPU 退役；不把 ImDrawData 或临时 UI 指针直接放进队列。窗口创建/事件及 GLFW 亲和操作保留主线程；各后端初始化/呈现的平台约束需审计和失败测试。

resize、scene switch、quality/reset、capture/退出使用带单调序号及 scene/view epoch 的可靠命令，明确对应帧前/后边界并确认执行；不可与可丢弃的统计/可见性反馈混在 latest-only 通道。历史以前一实际渲染帧为准，不用生产者最新相机覆盖前帧；共享海洋仍每个逻辑帧一次，Fluid 保持原作用域。capture delay/max frames/截图报告绑定被接受及实际完成的逻辑帧，不用 main loop 次数冒充 rendered frame。

资产 IO 完成后在 render lane 的原 GPU 安全点上传，再将绑定 revision 完成消息交主线程提交到下一帧；旧帧仍持有旧绑定。当前需要设备的场景工厂须拆出 CPU recipe 与有序 GPU bootstrap，或在场景切换安全点同步派发，不能在主线程任意调用已转移的设备。运行时设置一律包内值化，不在渲染线程临时读取可变 UI 状态/环境变量。

停机：停止生产和 IO → 关闭帧入口并处理已接受工作（正常退出 drain；失败明确取消未提交帧并使其捕获失败）→ 收拢录制任务 → 等待已提交 GPU → 释放 UI GPU 引用/渲染器/资产绑定/后端 → join 渲染线程 → 释放窗口及主线程状态。异常、部分初始化失败、设备错误均须唤醒生产者/控制命令等待者，不伪造成功 ack。不得仅凭 shared_ptr 引用归零就回收 GPU 资源。

### D11. 所有已完成 Demo 的效果是线程迁移硬约束

基线覆盖 DemoSceneCatalog 的 20 个 key：preview、showcase、reflections、shadows、lights、gpu-driven、post-process、render-graph、materials、streaming、atmosphere、large-world、terrain-vt、ocean、waveworks-ocean、hpwater-ocean、pbf、fluid-render、fluid-caustics、fluid-toon。目录新增项须显式补齐验证配置，不能静默漏掉。

每场景保存后端、Editor 能力/实际活跃视图、固定相机、分辨率、设置、seed/确定性步长、流送就绪条件和采样帧；不通过关闭 TAA、阴影、折射、体积或降低采样数使图像相同。对该 Demo 支持的配置验证原始基线 → 新 inline → inline+pool → threaded+pool，全部使用 V2 同后端门限，原始 golden 不覆盖。

水波/泡沫、四个 Fluid Lab、TAA、阴影、流送和双视图需要连续采样帧与 reset/resize/切场景序列，检查模拟次数、history epoch、图结构与中间黑帧/错视图/旧帧，不能只比最终截图。个别场景不适用某配置必须说明原因，不将不适用计作通过。门限不稳定先调查基线，禁止自动放宽门限或用跨 API 宽门限放行；有回归停在当前阶段，默认模式不切换。

## Files to Add

以下均为未来实施文件；`.{h,cpp}` 表示对应头文件与实现文件各一份，不是本轮已经生成。

| 文件 | 职责/阶段 |
| --- | --- |
| `src/Core/Application/AssetRuntimeCoordinator.{h,cpp}` | 资产导入、流送上传及绑定变更协调，P1 |
| `src/Core/Application/CaptureAutomationController.{h,cpp}` | 截图/报告/退出条件，P1 |
| `src/Core/Application/WaterValidationSequence.{h,cpp}` | 从 Host 提取的 opt-in 验证驱动，P1 |
| `src/Core/Profiling/FrameProfiler.{h,cpp}`、`FrameProfilerSnapshot.h` | 有界 CPU/lane/view/GPU/等待快照与聚合，P0 |
| `src/UI/PerformanceProfilerPanel.{h,cpp}` | Editor 实时时间线、瓶颈和分析等级/Scene 刷新控制，P0 |
| `src/Scene/SceneSession.{h,cpp}`、`src/Scene/SceneSessionActions.h` | 场景会话/窄动作契约，P1 |
| `src/UI/EditorCoordinator.{h,cpp}` | Editor 会话、面板输入输出与统计组装，P1 |
| `src/Renderer/RenderFrameCoordinator.{h,cpp}` | 多视图生命周期与共享生产者选择，P1/P3 |
| `src/Renderer/Features/RenderFeature.h`、`RenderFeatureContext.h` | 小型注册/生命周期描述，P3 |
| `src/Renderer/Features/BuiltInRenderFeatures.{h,cpp}`、`FeatureGraphTypes.h` | 内置组装与通用图契约，P3 |
| `src/Renderer/Features/Ocean/WaterOpticsGraph.h`、`src/Renderer/Features/Fluid/FluidGraph.h` | Feature 自有图输入输出，P3 |
| `src/Scene/RenderSceneData.{h,cpp}`、`RenderView.h`、`RenderFramePacket.h` | 不可变场景、动态视图及帧载荷，P4 |
| `src/Scene/SceneChangeTracker.{h,cpp}`、`RenderSceneExtractor.{h,cpp}` | mutation 版本和两种提取策略，P4 |
| `src/Scene/RenderSceneExtractionStatistics.h` | 复制、复用、失效诊断，P4 |
| `src/Renderer/Graph/GraphDescription.h`、`CompiledGraph.h`、`GraphExecutionState.h` | 声明/编译/执行数据边界，P2 |
| `src/Renderer/Graph/GraphCompiler.{h,cpp}` | 裁剪/依赖和编译流程，P2 |
| `src/Renderer/Graph/ResourceLifetimePlanner.{h,cpp}`、`TransientAliasPlanner.{h,cpp}` | 生命周期与别名规划，P2 |
| `src/Renderer/Graph/QueueScheduler.{h,cpp}`、`ResourceStateTracker.{h,cpp}`、`GraphExecutor.{h,cpp}` | 队列、状态和执行，P2 |
| `cmake/PrismCoreSources.cmake`、`PrismPlatformSources.cmake`、`PrismAssetSources.cmake`、`PrismSceneSources.cmake` | 新 target 源清单，P5 |
| `cmake/PrismModuleBoundaries.json`、`CheckModuleBoundaries.cmake` | 生产源归属/依赖规则与检查，P5 |
| `tests/ApplicationCompositionTests.cpp`、`RenderFeatureLifecycleTests.cpp`、`RenderScenePublicationTests.cpp` | 新契约测试，P1/P3/P4 |
| `tests/RenderGraphCompilerTests.cpp`、`tests/cmake/ModuleBoundaryTests.cmake`、`tests/cmake/fixtures/` 下的正反例 | planner 与构建边界验证，P2/P5 |
| `scripts/Validate-ArchitectureRefactor.ps1`、`Benchmark-ScenePublication.ps1` | 隔离输出的回归/性能驱动，P0/P4 |
| `scripts/Benchmark-ArchitectureScenarios.ps1`、`ArchitecturePerformanceScenarios.json` | 空负载、完整 Demo、双视图/Game-only/Standalone 的匹配对照，P0 |
| `scripts/ArchitectureValidation.Common.ps1`、`ArchitectureDemoCases.json`、`Compare-ArchitectureImages.ps1` | 子进程/路径隔离、全 Demo 输入目录及严格同后端比较，P0 |
| `scripts/ArchitectureVisualBaselineRevisions.json`、`ArchitectureVisualBaselines.Common.ps1`、`Compare-ArchitectureDemoRun.ps1` | 用户批准的精确参考修订、来源完整性校验及整批同后端图像比较，P0 |
| `tests/scripts/ArchitectureVisualBaselineTests.ps1`、`docs/ARCHITECTURE_VISUAL_BASELINES.md` | 修订范围/哈希/失败批次正反例和使用说明，P0 |
| `src/Core/Threading/ITaskExecutor.h`、`TaskScheduler.{h,cpp}`、`TaskGroup.{h,cpp}` | 常驻有界任务调度与失败传播，P6 |
| `src/Core/Application/RenderExecutionService.{h,cpp}`、`RenderFrameQueue.{h,cpp}`、`RenderControlCommand.h` | 执行 lane、有界帧交接与可靠控制，P7 |
| `src/UI/UiDrawPacket.{h,cpp}` | UI 值拷贝与纹理保活，不反向污染 Scene，P7 |
| `tests/TaskSchedulerTests.cpp`、`RenderFrameQueueTests.cpp`、`RenderThreadLifecycleTests.cpp`、`UiDrawPacketTests.cpp` | 并发/异常/停机与 UI 交接测试，P6/P7 |
| `tests/scripts/ArchitectureValidationTests.ps1` | 验证工具隔离、失败传播、门限正反例，P0 |
| `docs/RENDERER_ARCHITECTURE_CN.md`、`docs/ARCHITECTURE_REFACTOR_ACCEPTANCE.md` | 最终架构与逐阶段证据索引，P0/P8 |

## Files to Modify or Relocate

- `src/Core/ApplicationHost.h/.cpp`、`ApplicationHostScene.cpp`、`ApplicationLauncher.cpp`、`ApplicationCommandLine*`、`FrameTimer*`、`CpuTrace*` 的必要接线及测试；先接入 FrameProfiler，`WaterValidationSequence.cpp` 在迁移调用方后删除旧定义。`main.cpp` 不增加业务逻辑，原则上不改。
- `src/UI/EditorLayer*`、`ViewportStatisticsOverlay*`、`DebugPanel*`、`OceanLabPanel*`、`WaterOpticsPanel*`、`ImGuiFactory.cpp`：接入 profiler/coordinator、动态视图和诊断；FPS 标签明确范围，不改变材质操作含义。
- `src/Scene/RenderScene*`、`RenderObject.h`、`WorldRenderSceneBridge*`、`AssetStreamingSceneBridge*`、`CameraController*`、`DemoSceneCatalog*`、工厂和序列化路径：接入变更范围/稳定 ID/新输入，保持资产文件格式。
- `src/Engine/CommandSystem*`：提供事务完成/回滚/undo/replay 的变更通知，不改变命令 JSON 契约；`src/Asset/AssetRegistry*`、`AssetStreamingManager*` 和运行时装载：绑定版本及驻留通知。
- `src/Renderer/SceneRenderer*`、`SceneRendererSharedResources*`、`RenderFeatureRegistry*`、`SharedRenderGraphFrontend*`、`Pipeline/ScenePipelinePlan*`、`RenderSettings.h` 的必要接入、`RendererStatistics.h`、`RenderGraph*`、`RenderGraphDiagnostics*`、`GpuTimingReport*`、`RHI/Profiling/GpuProfiler*`；GPU profiler 按等级记录而不改变图执行。
- `src/Renderer/Features/` 下 ClusteredLighting、GpuDrivenVisibility、LocalLightShadows、PlanarReflections、VarianceShadowMaps、ScreenSpaceEffects、TemporalAntiAliasing、SkyAtmosphere、FftOcean、InteractiveTerrain、VirtualTextureCache、Ocean/SpectralOceanSimulation、LocalWaveGpuResources、WaterOpticsFeature、Fluid/FluidFeature 的生命周期/图接入点；算法实现保持。
- `src/RHI/D3D12/D3D12ImGuiRenderer.{h,cpp}` → `src/UI/Backends/D3D12/`；Vulkan 对应文件 → `src/UI/Backends/Vulkan/`。RHI 底层实现原则上不变，必要的 include 路径更新单独审查。
- `CMakeLists.txt`、所有受影响 `cmake/Prism*Sources.cmake`、`CMakePresets.json`、`.github/workflows/windows-ci.yml`；测试/Tools/Automation target 按实际依赖重接线。Linux workflow/preset 可以保留为非阻塞兼容性入口，但不属于本 change 的完成条件。
- 现有 `tests/RenderGraphTests.cpp`、`WorldRenderSceneBridgeTests.cpp`、`EngineHarnessTests.cpp`、`ApplicationCommandLineTests.cpp`、`OceanSettingsTests.cpp`、`WaterOpticsTests.cpp`、`OceanFftGpuTests.cpp`、`OceanTessellationGpuTests.cpp`、`ShaderCompilerTests.cpp`；保留原 fixture 的断言强度。
- `scripts/Validate-HpWater.ps1`、`Capture-HpWater.ps1`、`Summarize-HpWater.ps1` 仅在需要独立输出路径/选择构建二进制时增加兼容参数，不改现有默认行为。

## Data Flow

```text
UI / CLI / validation commands / streaming completions
  -> SceneSession + AssetRuntimeCoordinator
  -> commit mutation categories / immutable binding revisions
  -> RenderSceneExtractor
       unchanged data: reuse shared RenderSceneData
       changed data: publish one new coherent version
  -> RenderFramePacket { scene data, logical time, independent views }
  -> bounded FIFO FrameEnvelope + reliable ordered controls (P7; inline before P7)
  -> RenderFrameCoordinator
       one shared simulation producer
       per-view SceneRenderer / Feature lifecycle
  -> pipeline stages + feature-owned typed graph inputs/outputs
  -> RenderGraph facade -> compiled plan -> execution state
       independent recording -> persistent worker pool -> ordered join
  -> RHI submission/present on render lane (no separate RHI thread)
  -> completion/readback tagged with scene revision + ViewId
  -> FrameProfilerSnapshot { loop/main/render/worker/views/GPU/waits/level }
  -> per-view debug/PerformanceProfilerPanel / capture controller
```

数据所有者负责保活；借用 spans/references 的生命周期不得超出 packet/feature owner。GPU 完成和 CPU 发布是不同边界，不用发布 generation 代替 GPU fence。

## Validation Method

### V0. 实施前基线与证据隔离

- 确认 HPWater 完成版本、工作区未提交变更、其他活动写入和可复现构建。若 Git 元数据仍无法解析，先取得用户认可的可恢复版本/快照，不执行 reset/checkout 清理，不在无可靠恢复点时开始大规模搬移。
- `artifacts/hpwater-validation/` 保持原样。每个架构阶段结束时最多创建一次不可变 source revision/file-hash manifest，并在 `artifacts/architecture-refactor/<baseline-id>/<phase>/<backend>/` 记录 build config、依赖版本、GPU/驱动、尺寸、视图数、相机、固定时间、设置和队列模式。阶段内的普通单测/冒烟不重复遍历和哈希全部源码；记录二进制、直接相关输入与配置即可。
- 复用既有 capture 工具，给二进制/输出根增加兼容可选参数或在独立 worktree 执行；禁止并行进程写同一个 build/cache/report。旧错误和警告单列，不将既有基线问题伪称本次已修复。
- Baseline 是可重放的输入与证据，不只是一张最终图片；报告保存 Pass 序列、资源版本/状态、共享模拟次数、history reset/retirement 和对象映射。
- P0 不修改 `HKLM\\SOFTWARE\\Khronos\\Vulkan\\ExplicitLayers`、驱动全局设置或强制 GPU 时钟。旧锁频实验作为旁路证据保留；普通 strict validation 与 profiler/Release 场景矩阵是可重放入口。

### V1. CPU 与模块测试

- 所有现有 CPU suites 继续通过；新增 Host 顺序/部分失败析构、Feature 生命周期/重复与循环依赖/图输入错误、快照 mutation 矩阵/旧帧保活/事务/选择反馈、planner/cache 等测试。
- 新增发布测试包含静态场景、单独相机、对象动画、材质替换、异步上传/驱逐、undo/replay、拓扑切换和旧 GPU feedback；FullRebuild 与 Versioned 对照。
- Graph fixtures 必须覆盖传递约简不丢 queue release、首 compute 访问 prologue、逐 mip 恢复、buffer range、alias、history、cache hit 初始状态改变、串行/native/parallel recording。
- CMake 边界负例验证 RHI→UI 的逆向 include、Renderer→App、重复生产源、内部头越界、缺失直接链接；正例验证 Editor backend 例外按实际 target 归属识别。
- P6/P7 增加队列容量/背压/FIFO/异常唤醒/停机、线程亲和、录制结果稳定顺序、UI 副本保活、可靠命令与旧反馈拒绝测试。用 barrier/latch 证明主线程 N+1 与渲染 N 的实际重叠，不用 sleep 猜测。可用时运行 CPU 组件数据竞争检测，未提供的检测环境如实记待验收。

### V2. GPU 与图像

- 阶段级门禁在 Windows D3D12/Vulkan 运行与改动风险直接相关的 GPU fixtures；HPWater/RDG 改动保留 native lifecycle 与 serial/auto 冒烟。Vulkan 缺校验层必须报告未验证，不将无层运行称为 validation pass。单个实现任务默认只跑直接 CPU fixture 和一个代表性后端冒烟，不重复完整 GPU 集。
- Demo 图像按改动风险抽取代表集：至少覆盖基础 Preview、RenderGraph/队列、阴影或透明、流送、terrain 以及 Ocean/HPWater 中受影响的一项；不再要求每阶段重跑全部 20 个 Demo。`pbf`、`fluid-render`、`fluid-caustics`、`fluid-toon` 四个粒子流体 Demo 明确排除出本 change 的常规架构回归；直接修改 Fluid 契约时使用对应模块/图 fixture，不以四 Demo 全矩阵验收。P7 默认线程模式切换和 P8 最终交付也沿用风险代表集，不恢复无差别全矩阵。
- HPWater 七种等输入视角、三档质量、两种尺寸、相机切换/水线/resize/reset/scene switch；其中 underwater temporal sequence 不用单帧截图替代。
- 同后端前后对照优先要求完全相同；有可重复的浮点/栅格误差时，默认上限 MAE ≤ 1/255、RMSE ≤ 2/255、SSIM ≥ 0.999、单像素 8/255 容差外变化比例 ≤ 0.001，并核对水深/遮挡/历史关键区域。基线自身至少重复 3 次验证稳定；不稳定先修正采样，不能自动放宽门限。超限停在当前阶段并定位，调整门限需要明确理由与用户认可。
- 双后端 parity 另外沿用 HPWater 已有门限（MAE 0.03、RMSE 0.10、SSIM 0.90、变化比例 0.25），不能拿较宽的跨后端门限替代同后端重构比较。
- 保留现有 D3D12 warning 820 的基线记录，但不允许新增 validation error、device removal、无效 descriptor 或异常退出；不为消除警告改变渲染代码。

### V3. 性能与复制量

- P0 首先建立字段语义和公平场景特征，不把两个独立 GPU 批次的绝对一致性当作进入 P1 的条件。每个结果必须记录 build、backend、validation、profiling level、active views、Scene 刷新策略、窗口/显示和 GPU clock 条件；不同观察协议不得直接比较。
- 实时面板与导出必须把 Editor Loop、Main、Render、Worker、Game/Scene、GPU、Submit/Present/Wait 分开；总帧受最长 critical lane/等待约束，不能把阶段中位数相加或倒数单个 Pass 冒充 FPS。inline 阶段 Render 是主线程子区间，P7 后才是独立线程。
- 预热后静态和仅相机场景各连续至少 120 帧：完整对象复制量与场景数据重建增量为 0；每个真实变更批次最多发布一次数据重建。报告 frame packet 小对象分配与对象数组复制分别计数。
- CPU extraction/UI/frame、GPU 总时长/阶段、峰值内存/退役集合、PSO 创建次数分别记录。GPU 阶段时长之和不等于多队列 critical path，不推算 FPS。
- P0 已建立完整场景特征。P1–P6 的结构阶段只对一个受影响的代表场景做一次预热后不少于 180 个有效帧的抽样，用于发现明显异常，不在每个阶段重复两批 3+5。P7 默认线程模式切换与 P8 最终交付才执行正式多进程 A/B；需要跨日/跨批结论时报告分布和 DVFS，不要求为了通过而锁频。auto 只在隔离并固定初始 cost-model cache 时对照。
- 结构阶段的单次性能抽样若 Main/Render/Worker CPU、Editor Loop 或输入至呈现出现超过 5%/10% 的信号，记录为待观察并先用直接 profiler 定位；不只为跨批噪声重复大量 GPU 运行，也不阻止与该热路径无关的后续拆分。P7/P8 正式 A/B 中相同门限仍是默认切换硬门禁；DVFS 不匹配标记不可判定。GPU 行为、代表图像与 validation 仍必须通过。快照优化没有固定 FPS 承诺，但必须满足复制量契约且不引入无界保活。
- 多线程增加 main/render/worker CPU trace、队列等待/峰值深度、端到端输入至呈现延迟、线程数、帧完成数量与内存保活峰值，防止以积压、丢帧或额外帧延迟冒充性能收益；GPU 指标仍按原队列模式对照。

### V4. 构建组合

本 change 的构建门禁为两个 Windows configure/build/test preset：`windows-ci` 和 `windows-vulkan-only-ci`。新增依赖检查与公开头自包含编译必须适用于这两个组合，并分别覆盖完整 Editor/D3D12 能力与无 Editor、无 D3D12 的 Vulkan-only 边界。Linux preset/CI 若存在，可继续作为非阻塞兼容性信号；其缺失、未运行或失败不阻止本 change 的 Windows 架构实施，也不进入阶段完成计数。

参考现有入口：`cmake --preset windows-ci`、`cmake --build --preset windows-ci`、`ctest --preset windows-ci`；GPU tests 需显式 `ctest --test-dir build-windows-ci -C RelWithDebInfo -L gpu --output-on-failure -j1`，因为 Windows CPU preset 排除了 gpu 标签。HPWater 复用 `Validate-HpWater.ps1 -Mode lifecycle/views/benchmark/details` 与 `Summarize-HpWater.ps1 -EnforceParity`，实际运行前使用隔离输出配置。

## Risks / Trade-offs

- [服务拆成转发壳，中心对象不减负] → 状态跟随 owner 迁移，依赖测试禁止 Host 回指，不以行数验收。
- [初始化/退出次序变化] → mock 事件轨迹与故障注入，先证明 GPU/IO 生命周期再移动 owner。
- [Feature 顺序或共享模拟次数变化] → LogicalFrameId 与 ViewId 分离，固定阶段，双视图计数与图输出对照。
- [Blackboard 隐藏资源依赖] → typed input 校验与 RDG read/write 同时存在，缺输入/过期句柄负例。
- [快照 shallow const 和漏 dirty] → 值化参数、绑定版本、集中提交/完整 mutation 矩阵，未知写入强制重建。
- [GPU feedback 索引错配] → generation/revision/view 标识与稳定对象映射，不原地筛掉共享数组元素。
- [旧 snapshot 保留导致内存峰值] → CPU 引用和 GPU retire 分别测量，完成后不保存历史队列，停机 drain。
- [Graph 拆分破坏 HPWater 同步修复] → 拆分前固化特定 hazard tests，保留未约简访问语义与执行时状态。
- [拆库引入环或平台链接失败] → 先列 target/source owner，再按底向上拆；保留 Editor 条件依赖与第三方 runtime 拷贝。
- [跨阶段改动太大难回退] → 每阶段只接受该阶段范围，保留可构建 checkpoint，不混入算法调整。
- [线程间裸引用/缓存竞争/退出死锁] → 独占执行 lane、值化 UI/设置、TaskGroup 保活、可靠命令 ack 与失败唤醒，保留 inline 执行同路径。
- [跳帧/历史错配掩盖效果退化] → 有界 FIFO 不丢已接受帧，按实际渲染帧推进历史/捕获，全部 Demo 连续帧与基线对照后才切默认。
- [Profiler 本身降低 FPS 或把旧 GPU 数据错配到当前帧] → 分级采样、显式 resolved frame/generation、固定 ring 容量、无逐帧 GPU wait，并对 off/basic/detailed 开销做同进程 A/B。

## Migration Plan

这是一个完整 change、九个顺序阶段，不是要求一次提交。实施顺序为 P0（完成既有基线证据并先交付实时 Profiler/公平场景矩阵）→ P1 → P2 → P3 → P4 → P5 → P6（worker pool）→ P7（render thread）→ P8（最终验收）；对应 tasks.md 的 1–9 节。P0 不再因机器级锁频/Vulkan 注册阻塞。P2 保持公共接口，可在 Feature/快照接口变化前独立验收；禁止在 P2 同时修改 scheduler 算法。P3 接触 Ocean 接入前必须确认两个未完成 Ocean change 没有共同写入，过期的 12/80 migration visibility 计划需先更新、暂停或归档。

| 阶段 | 内容 | 进入下一阶段的条件 | 回退单位 |
| --- | --- | --- | --- |
| P0 | 恢复点、前置缺陷修复、图像/连续帧基线、实时 Profiler、分级观察和公平场景矩阵 | 普通 strict/图像行为通过；指标范围可解释、场景 A/B 可重放；无需机器级锁频或 Vulkan 注册 | profiler/工具/文档与各前置修复分别保存 |
| P1 | Host 服务及视图协调器提取 | 原操作顺序、生命周期、所有 Lab 冒烟、HPWater 对照通过 | 单个服务提取提交 |
| P2 | RDG 内部解耦 | 公共 API、图计划、关键 hazard/cache、GPU 验证等价 | 单个 planner/executor 提取提交 |
| P3 | Feature 生命周期/typed graph 迁移 | 所有内置功能适配、旧全集字段收缩、共享/视图语义通过 | 单个 Feature 迁移提交 |
| P4 | 版本化快照/RenderView | 全 mutation 对照、零静态复制、保活与性能通过后切默认 | 同一模型下切 FullRebuild 后备；再回退阶段提交 |
| P5 | CMake/公开依赖边界 | 两个 Windows 构建组合、边界正反例、runtime deployment 通过 | 单个 target 拆分提交 |
| P6 | 常驻工作线程池，替换逐 Pass async | 异常/有界调度/亲和、录制顺序、全部 Demo 同后端效果与性能通过 | task executor 切 inline；回退阶段补丁 |
| P7 | 独立 render thread、帧 FIFO、UI/资产/控制交接及 RHI Thread 数据决策 | 全线程/时域/双视图/停机压力与原基线对照通过再切默认；时间线给出 RHI 实施/不实施结论 | execution mode 切 inline；回退阶段补丁 |
| P8 | 全矩阵验收、清理临时适配、最终架构文档 | 所有任务证据齐全，无待验证项目伪报通过 | 上一个通过验收的阶段 checkpoint |

回退先停止本阶段写入并保存当前 diff；使用经授权的 revert/反向补丁或隔离 checkout，不 reset 掉他人工作。原 HPWater 验收证据和实现保持可恢复。最终只删除已无调用者的临时旧接口，不移除 full-rebuild 诊断策略；OpenSpec 归档另行请求，不在实现完成时自动归档其他 change。

## Open Questions

无阻碍范围或实施路线的待决设计。需在 P0 实测但不改变方案的输入包括：基线自身图像/性能抖动、现存 raw mutable 调用点数量及重建统计。Windows 门禁环境或行为门限异常会阻止相应验收通过；Linux 环境不属于本 change 的硬前提。
