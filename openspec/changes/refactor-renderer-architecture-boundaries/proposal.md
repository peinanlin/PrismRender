## Why

PrismRender 已具备双后端、RenderGraph、多视图及完整 HPWater 光学链路，但应用编排、Feature 接入和场景复制仍集中在少数核心类，新增功能需要跨多个公共结构修改。以 2026-08-28 已完成的 HPWater 为基线，分阶段收敛职责、生命周期和数据发布边界，降低修改影响范围及每帧场景复制成本，同时保留已有渲染结果与同步修复。

## What Changes

- P0 前置修复（用户确认继续）：修复已复现的 no-Editor 应用/UI 链接边界、lights D3D12 描述符分配失败和 Vulkan 子资源布局错误；限定必要的应用/RHI/资源初始化改动，保持默认画质和算法，修复证据与随后纯结构对照分开保存。
- 后续用户确认的 P0 范围：不可变 sampler 表复用、关闭天空时的已初始化 LUT 后备及开关绑定、完整 FFT GPU fixture 的严格资源状态校验修复。不提高堆容量、不降低校验或数值/图像门限，不改变海洋算法。
- 用户再次确认纳入 P0：修复 SSR 静态绑定已关闭 PlanarReflections 未初始化纹理的问题，使用已初始化后备并覆盖双视图开关/尺寸重建；保持开启路径、着色器与 Demo 默认配置不变。
- 用户已统一批准 P0 可选 Feature 关闭状态绑定审计/修复：从 GTAO 出发，核对所有消费者的有效开关、图访问和后备资源，修复同类无效绑定；不要求对同一类缺陷逐个重复确认，不改变开启算法或放宽验收。
- 用户已确认继续处理 P0 的四个 Fluid/native 共享资源布局错误与 PBF 同二进制重复截图不稳定：补全真实资源依赖，追踪固定输入及模拟读写/遍历顺序；不通过切 serial、降低画质或放宽门限验收，不重写流体求解算法。
- 用户已明确批准确定性修复后的 PBF D3D12、PBF Vulkan、Fluid Toon Vulkan 三组稳定图作为新 P0 视觉参考。批准仅限完整 Editor 构建、单 Game view、Demo 默认设置、native、1280×800、固定第 30 帧；新旧图及失败报告均保留，V2 门限不变，其他视角/质量/连续帧仍独立验收。

- 用户已批准将独立 OceanTessellation GPU fixture 的缓冲初态、交换链状态和测试 Domain shader spacing 修复纳入 P0；固定严格校验请求，保留所有数值断言与容差，不改生产 OceanSurface、RHI 规则或海洋算法。
- 在结构迁移前补齐内建实时性能可观测性：明确区分 Editor 主循环、Game/Scene 视图、CPU 主/渲染/worker lane、GPU、提交/Present 与等待成本，提供有界帧历史和 `off/basic/detailed/capture` 分析等级；普通运行不得无条件承担完整逐 Pass 调试开销，现有含义模糊的 FPS 必须标明统计范围。
- 建立可与 Unity/UE 口径对照的固定性能场景，分别记录 Editor 双视图、Editor Game-only、无 Editor/Standalone、空负载与完整 Demo；GPU 动态频率和验证环境作为报告条件，不再以管理员锁频、机器级 Vulkan 注册或跨批绝对 GPU 稳定性阻塞 CPU 架构工作。
- 提取 `ApplicationHost` 中的场景会话、资产运行时、编辑器协调、截图/自动化职责；状态随职责迁移，不创建能够任意访问整个 Host 的转发包装类。
- 将 `SceneRenderer` 收敛为视图编排入口，保留现有几何 Pass 与 Feature 实现；建立可测试的 Feature 生命周期及类型化图资源输入/输出，区分设备共享资源、每帧共享模拟和每视图历史。
- 引入版本化、不可变的渲染场景快照及独立 `RenderView`：未改变的对象数据跨帧复用，相机、时间、视图选择及可见性反馈不再要求完整复制场景。完整重建路径作为迁移期间的对照后备，完成所有变更源审计后才切换默认路径。
- 保持 `RenderGraph` 公共入口和算法语义，分离声明数据、编译产物、生命周期/别名规划、Barrier 规划、队列调度和执行；保留 HPWater 验收期间修复的跨队列资源释放、fence 与逐 mip 状态规则。
- 对齐 CMake target 与基础设施、资产、场景、渲染、编辑器边界；每个实现源文件只有一个生产库归属，使用依赖检查而非仅靠拆库约束跨层 include。
- 在以上边界稳定后，以常驻、有界工作线程池替换逐 Pass 的 `std::async` 录制，再接入独立渲染线程。主线程负责窗口/编辑/场景生产，渲染线程负责渲染状态、GPU 上传、建图/提交和呈现；本 change 不增加独立 RHI 线程。
- 保留同一执行路径的 inline 参考模式，通过不可变帧包、有界 FIFO、可靠控制命令和 GPU 退役约束实现线程交接；不能丢弃已接受的模拟帧来获得吞吐量。
- 建立贯穿各阶段的基线、图结构/图像/生命周期回归和性能证据；性能门禁优先采用同进程/同配置 A/B、线程时间线与匹配环境对照，区分观察性数据和行为正确性门禁。每阶段可单独验收与回退，不同时改变渲染算法和软件结构。
- 将全部 20 个 Demo 的既有效果列为硬兼容边界：固定输入图像及动态连续帧分别验收，比较原基线、新 inline、新多线程路径，未通过前不切换多线程默认值；不得通过降画质、停模拟或放宽门限掩盖回归。
- **BREAKING（内部 C++ 接口）**：迁移后的 Feature 注册接口、渲染帧输入、部分公共头文件及 CMake 链接依赖发生变化，仓库内调用方随阶段迁移。CLI、场景文件、材质含义、默认设置和自动化既有字段保持兼容；不新增运行时第三方依赖。

## Capabilities

### New Capabilities

- `renderer/feature-lifecycle`: 提供明确作用域、调用顺序、初始化失败清理、图输入输出、线程亲和及每视图历史隔离的 Feature 接入契约，使新 Feature 不必扩展集中式资源/回调全集。
- `scene/versioned-render-snapshots`: 提供跨帧快照复用、完整变更失效、独立视图输入、稳定对象索引、跨线程有序交接及旧帧资源保活的渲染数据发布契约。

### Modified Capabilities

无。现有 HPWater、Spectral Ocean、Fluid、RHI 等功能行为不改变；目前 `openspec/specs/` 没有对应主规格，保留现有 change 中的功能契约，不复制或改写它们。ApplicationHost 拆分、RenderGraph 内部解耦、CMake target 整理属于保持行为的重构，在 design/tasks 中约束，不为拆文件人为新增行为规格。本 change 因引入上述两项实际接口/发布行为而不设置 `skip_specs`。

## Impact

- 应用与编辑器：`src/Core/ApplicationHost*`、`WaterValidationSequence.cpp`、`src/UI/EditorLayer*`、UI 统计和文档操作协调；`main.cpp` 保持最小入口。
- 性能诊断：新增有界 Frame Profiler、只读快照和 Editor 时间线面板，接入当前 CPU trace、GPU timestamp、FramePacingStatistics、RenderGraph/cache 与 active-view 统计；分析等级和面板不得引入未声明的逐帧 GPU 等待。
- 场景与资产：`src/Scene/RenderScene*`、World/Streaming bridge、CameraController、AssetRegistry 运行时绑定更新、场景工厂与序列化调用点。
- 渲染：`src/Renderer/SceneRenderer*`、`RenderFeatureRegistry*`、`SharedRenderGraphFrontend*`、`Pipeline/ScenePipelinePlan*`、各 Feature 接入点及 `RenderGraph*`。
- 构建：`CMakeLists.txt`、`cmake/Prism*Sources.cmake`、Windows 构建 presets/CI 与测试链接；编辑器桥接代码不应成为基础 RHI 的 UI 依赖。当前 change 以 Windows D3D12 和 Windows Vulkan 为交付平台，Linux 兼容性属于可选的后续工作，不作为实施或阶段验收门禁。
- 验证：新增 Host/Feature/快照/依赖边界测试，复用现有 RenderGraph、Ocean、WaterOptics、ShaderCompiler、World bridge、GPU fixture 和 `scripts/Validate-HpWater.ps1` 等验证设施。
- 验证预算：架构重构按风险选择直接模块测试和少量代表性渲染冒烟，不在每个子任务重复全部 Demo、双批 V3 或全源码哈希。`pbf`、`fluid-render`、`fluid-caustics`、`fluid-toon` 四个粒子流体 Demo 不属于本 change 的常规回归矩阵；若后续直接修改 Fluid 契约，只运行对应的无窗口模块/图 fixture，不启动四 Demo 全矩阵。完整源码哈希只在阶段不可变封存时生成一次，普通增量验证记录构建配置与直接输入即可。
- 线程：新增 Core 工作调度器、应用帧交接与渲染执行服务，适配 UI 绘制数据/纹理保活、资产绑定完成反馈和捕获帧身份；保留现有 Asset IO worker 与 GPU queue mode 的含义。
- 范围限制：不改 FFT、光学、流体、剔除、GPU 调度算法，不在本 change 中直接新增独立 RHI 线程、ECS 或图形后端，不扩大 GPU 异步重叠范围，不改变 GPU 资源退役安全性，不借本 change 完成其他尚未完成的 Ocean change。P7 后只依据时间线形成独立 RHI Thread 的实施/不实施结论；若确需实现，另行更新范围。
- 系统状态限制：不要求写入机器级 Vulkan layer 注册、不要求修改驱动全局设置或固定 GPU 时钟才能进入结构阶段；此类实验只可作为明确授权的旁路诊断，不能替代普通用户环境下的 strict validation 与 Release/Standalone 对照。
- 依赖：实施前冻结可复现的完成版 HPWater 基线；确认没有其他任务同时写入重叠源码/构建输出。用户已确认将多线程纳入本 change 一起实施；任务勾选仍仅代表实际实现且验证完成，不把规划状态当作实现状态。
