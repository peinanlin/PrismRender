# 架构边界与多线程改造验收记录

Change：`refactor-renderer-architecture-boundaries`。本文件记录实际进度，不是完成声明。

## 状态与范围

2026-08-31 最新：P0–P8 全部完成（128/128），OpenSpec 最终严格校验通过。Application 服务边界、RenderGraph 内部拆分、Feature 生命周期/类型化图契约、版本化 Scene/RenderView、CMake 模块边界、常驻 worker pool 与独立 Render lane 已进入生产路径。下文早期“等待批准/尚未进入 P1/Linux 待验收”等文字是实施过程的历史轨迹；Linux 已按用户决定移出本 change 的门禁，最新结论以本节、tasks.md 及文末 P8 记录为准。

不改水体/流体/阴影/TAA 算法或默认画质。先池化独立录制，再引入独立渲染线程；不引入独立 RHI 线程。任何 Demo 回归阻止阶段通过和默认模式切换。

## P7 Render lane 阶段验收（8.1–8.16）

生产路径现由 `RenderExecutionService` 统一执行 inline/threaded；默认 `threaded+pool`，显式 `inline+inline` 为同实现后备。Main N+1 与 Render N 之间只传 immutable `FrameEnvelope` 和可靠 control，队列上限为一项 waiting 加一项 executing。帧 completion 驱动 capture/max-frames，latest feedback 只驱动 UI 统计。完整协议、关闭顺序、UI 锁和回退命令见 [Render Execution 跨线程协议](RENDER_EXECUTION_PROTOCOL.md)。

8.12 的无 sleep 门闩 fixture 固定 Render N 后在 Main 完成 N+1 packet，最终 frame 1/2 FIFO、accepted=consumed、cancelled=0、peak waiting≤1；8.13 又交错执行 128 frame/128 reliable controls，并与 4096 worker task、asset binding lease 和 UiDrawPacket lease 测试共同证明有界收拢。当前 Windows MSVC 环境无 TSan/等价 race detector，此项如实登记为工具不可用。

8.14 使用原 P0 封存证据作为 original 参考；当前三种模式的双后端 HPWater 20 帧图像均通过同后端门限。D3D12 inline→threaded 为 MAE/RMSE/changed/SSIM `0.000011/0.001252/0.000075/0.999983`，Vulkan 为 `0.000014/0.001583/0.000100/0.999955`。两后端 threaded+pool 的 preview、shadows、terrain-vt、waveworks-ocean 风险冒烟通过；四个粒子 Demo 明确排除。UI GLFW 回调已进入 ImGui 跨 lane mutex；最终 D3D12 HPWater 五次连续进程与 Vulkan 捕获通过。

Profiler 能区分 Main/Render/Worker、queue wait/depth、completion wait、producer overlap 与 input-to-Present。D3D12 Preview 的代表样本显示 threaded producer interval 约 `17.07 ms`、Main N+1 overlap 约 `0.38 ms`，但 input-to-Present 约 `34.19 ms`，即当前一帧 render lag 的明确延迟成本。P7 不把该结果表述为低延迟收益，也不实现或暗示独立 RHI thread。

## P8 最终交付验收（9.1–9.8）

临时接口审计见 [架构重构临时接口审计](ARCHITECTURE_TEMPORARY_INTERFACE_AUDIT.md)：运行时 Host 回指、live backend/renderer 跨 lane、旧 mailbox/逐帧 mutable RenderScene 入口、逐 Pass `std::async` 和第二套 inline renderer 均为零。有意保留 versioned/full-rebuild、threaded/inline 和 pool/inline 的同路径诊断后备。

两个 Windows preset 已完成最终构建与 CPU-safe 测试。`windows-ci` 干净 RelWithDebInfo build 为 566 步，CTest **33/33**；`windows-vulkan-only-ci` 在 Editor/D3D12/Harness 关闭下 build 通过，CTest **30/30**。两者的 `PublicHeaderSelfContainment` 与 `ModuleBoundaries` 均通过。首次 windows-ci 增量测试因旧 ABI 对象出现三次进程异常和一条旧 settings-revision 断言；Debug 全新配置及 RelWithDebInfo 干净重建后四项与完整套件全部通过，失败尝试未隐藏。汇总见 `artifacts/architecture-refactor/p8-final-20260831/README.md`。

最终视觉结论采用累计门禁而不重复制造全矩阵。P0 verified index 包含双后端七视角、三质量、两尺寸共 26 case，每项三次稳定捕获；双后端三次 330 帧/64 控制点序列覆盖水线、reset、resize、quality 与 scene switch。P7 当前生产路径的 inline→threaded 同后端 D3D12/Vulkan 对照分别为 `0.000011/0.001252/0.000075/0.999983` 和 `0.000014/0.001583/0.000100/0.999955`（MAE/RMSE/changed/SSIM）；当前 threaded 跨后端为 `0.000129/0.005102/0.000678/0.999612`，通过独立宽门限。最终 P7 日志无 validation error/device removal；D3D12 warning 820 仍作为既有警告保留。

风险代表集为两后端 Preview、shadows、terrain-vt、WaveWorks Ocean；文件 save/load、事务、undo/redo/replay、streaming/reliable control 和自动化退出/报告由最终 CPU suite 与既有集成样本覆盖。按用户明确要求没有启动 `pbf`、`fluid-render`、`fluid-caustics`、`fluid-toon`，也没有恢复 20 Demo 全矩阵或重复全源码哈希。

复制/并发/性能结论：默认 versioned 静态 120 帧为 1 build/119 reuse、零完整对象复制，HPWater lifecycle 330 帧为 4/326；128 frame + 128 control 压力 accepted=consumed、cancelled=0、peak waiting≤1。matched Preview 中 threaded+pool 的 loop 约 17.69 ms、input-to-Present 约 34.9 ms，未显示吞吐优势并增加一帧延迟，因此不以线程数宣称 FPS 收益。独立 RHI thread 不实施，详见 [决策记录](RHI_THREAD_DECISION.md)。最终架构、owner、六类生命周期、Feature/mutation 范例和回退见 [渲染器架构](RENDERER_ARCHITECTURE_CN.md)。

已知限制与恢复：当前 Windows/MSVC 环境没有 TSan/等价 race detector；变更公开结构布局后旧多配置 build 目录可能残留不一致对象，应使用 clean build；threaded 低延迟不合适时设置 `PRISM_RENDER_EXECUTION_MODE=inline`，worker 问题设置 `PRISM_RENDER_TASK_EXECUTOR=inline`，scene 复用问题设置 `PRISM_RENDER_SCENE_PUBLICATION_MODE=full-rebuild`。三项都保留同一生产实现。源码级恢复使用已封存阶段快照复制到新空目录，不覆盖当前工作区、不使用 destructive reset；原 `artifacts/hpwater-validation/` 未被本 change 覆盖，其他 Ocean change 未被勾选或归档。

任务清单最终审查为 128/128；`openspec validate refactor-renderer-architecture-boundaries --strict` 通过。此结论表示 change 的实施与验收完成，不表示已经归档；归档需另行执行。

## 可恢复基线（1.1）

- ID：`20260828-hpwater-complete`；源配置快照在 `artifacts/architecture-refactor/20260828-hpwater-complete/snapshot/`。
- `snapshot-manifest.json`：809 个文件，181,363,576 字节，逐文件 SHA-256；manifest SHA-256 为 `2578BC5ACDD6CF09374656530240B4D0E3AEC5539EB0FC78DEE6C24F45AE588C`，另存 `.sha256` 封印。
- 用户已认可在 Git 不可用时先备份源码/配置。当前 `.git` 仅有 `ms-persist.xml`，不是可用 Git repository；没有初始化/修复/重置 Git，也未清理原工作区。
- `Backup-ArchitectureBaseline.ps1 -BaselineId 20260828-hpwater-complete -Verify` 已再次验证全部 809 个副本。创建时完成源→副本校验、最终源哈希和文件集合复核。未进行破坏性的原地恢复演练。
- 恢复方法：先停止相关进程并另存待恢复工作；校验 manifest 和全部副本；将 snapshot 复制到一个新的空目录，按 manifest 验证后配置新 build 目录。原地覆盖恢复须另行授权并按文件检查冲突，不使用 `reset --hard` 或删除工作区。
- 快照排除 `.git/.agents/.codex`、build、运行缓存、报告/截图；不包含可执行文件。构建需现有 SDK/依赖；项目内 third_party 已备份，CMake FetchContent 外部缓存/Windows SDK 不在可恢复源码集合内。
- HPWater change 记录 38/38 完成；本次未替其他 Ocean change 勾选/归档。检查时任务“HPwater”“引擎架构”均 idle，只有“引擎架构--readme”在写此计划；未向其他任务发送修改指令。此检查是当时快照，不是永久文件锁。
- 启动 GPU 基线前对比 manifest：`src/`、`assets/`、`cmake/`、根 CMake 文件与快照差异为 0。当前只有规划、验证脚本/测试及本文发生修改。

## 现状调用轨迹与 owner（1.2）

以下为迁移前源码轨迹，不是未来应实现的顺序：

1. `ApplicationHost::Initialize`：创建 Window/backend、Game/可选 Scene renderer、Registry/RenderScene/mailbox/CommandProcessor、两相机、可选 Editor 和 FrameTimer → backend.Initialize → 环境/资产与场景加载 → renderer/shared 初始化 → ImGui 接线 → 标记初始化完成。
2. `Run`：PollEvents → WaterValidationSequence → Tick → Fluid 输入 → consume resize/backend+两 renderer/UI 纹理重绑/两相机 aspect → ImGui/UI 编辑与导航/World 同步。
3. 帧渲染：BeginFrame；out-of-date 时 resize 并 continue → streaming TickUploads/激活/捕获请求/evict → delayed capture → 时间（确定性时 completedFrameCount/60）→ mailbox.Publish 全场景复制 → AcquireLatest → CreateGameView 再复制 → Game Render。
4. 按可见性/交互/刷新策略决定 Scene view；构造其场景并处理 GPU 可见性显示 → Scene Render → 图报告/UI draw → EndFrame → completedFrameCount 增加 → capture resolve/报告/退出判断。
5. Run 尾部 WaitForGpu 并输出 streaming 报告；2.8 后 Shutdown 仅在 backend 已初始化且尾部尚未 drain（或最终 streaming pump 新提交上传）时等待 → ImGui shutdown → UI/相机/World/mailbox/scene → streaming/import/registry → Scene/Game renderers/shared → backend/window/timer。部分初始化按已创建资源清理，重复 Shutdown 幂等。

| 当前 owner | 实际持有状态 | 迁移目标 |
| --- | --- | --- |
| ApplicationHost | Window/backend/timer、API 与启动选择、initialized | 组合根；P7 backend 转执行服务 |
| ApplicationHost | ImportService、Registry、StreamingManager、环境 cubemap、manifest/status | AssetRuntimeCoordinator |
| ApplicationHost | RenderScene/mailbox、CommandProcessor/World、两 camera controller、scene ID/path/label/dirty/request counter | SceneSession |
| ApplicationHost | Game/Scene renderer、shared resources、Scene refresh/visibility | RenderFrameCoordinator |
| ApplicationHost | DebugPanel/EditorLayer/ImGui、UI 状态组装 | EditorCoordinator |
| ApplicationHost | 延迟/deferred capture、streaming 激活状态、报告路径/写出标志、退出条件、水验证 | CaptureAutomationController（资产激活数据仍由资产服务返回） |
| SceneRendererSharedResources | IBL、ShaderManager、PipelineCache、InteractiveTerrain、SpectralOcean、LocalWaveGpuResources | 保持设备共享 owner，不复制到每视图 |
| 每个 SceneRenderer | RDG/统计/profiler、设置、viewport targets、各 Feature、历史、query/readback、FluidFeature | 保持视图职责，再接生命周期契约 |

Feature 作用域证据：`SceneRenderer.h` 中 `FluidFeature m_fluidFeature` 为值成员，`SceneRenderer.cpp` 注册其回调；不是 SharedResources 成员，当前每 renderer 有一实例，不能迁移时擅自变成两视图共享 PBF。WaterOptics、TAA、ScreenSpaceEffects、PlanarReflections、VarianceShadowMaps、LocalLightShadows、ClusteredLighting、GpuDrivenVisibility、VirtualTextureCache、SkyAtmosphere 均是 renderer 成员。谱海洋/LocalWave GPU 资源从 shared owner 借用；`m_viewIndependentSimulationProducer` 区分生产者。FftOcean 仍有每 renderer 接入成员，是否提交必须沿原生产策略核对，不能仅据成员位置判重复模拟。

### Mutation 来源表

| 来源 | 当前入口/可变对象 | 发布所需分类 |
| --- | --- | --- |
| World 命令、undo/redo、journal、加载保存 | Host dirty 标志、WorldRenderSceneBridge 同步对象/相机/灯光 | transaction/scene/data revision；失败不发布中间状态 |
| Editor property/gizmo/UI 直接编辑 | RenderScene mutable getters、材质 shared_ptr、Renderer settings | 对象/绑定 dirty 与小型视图设置分开 |
| 两相机导航、resize、水验证、无 Editor 导航 | GetCamera/GetGameCamera、aspect、preset | ViewId/epoch/小型动态输入，不重建对象 |
| Demo/程序化/glTF 工厂与切换 | ApplicationHostScene、DemoSceneCatalog/各 factory | scene epoch/稳定对象索引，工厂 GPU 创建待亲和迁移 |
| streaming 上传/激活/eviction、reimport | TickUploads、TryActivateStreamingScene、AssetRuntimeLoader/Registry | GPU 完成后的 binding revision，旧绑定保活 |
| GPU visibility/readback、地形辅助显示 | Game 反馈与 Scene view 副本对象标志 | versioned per-view feedback，不回写已发布数据 |
| 时间、Fluid 输入、Ocean preset/reset | Host 时间/输入与 renderer 设置 | LogicalFrameId、显式 reset 与每视图历史事件 |

`RenderScene` 公开 mutable camera/object/light getters，`RenderObject` 资源通过可变 shared_ptr 引用；浅 const 不等于不可变。P4 必须逐调用点审计，当前表是迁移风险来源清单，不是已接入 dirty tracker 的声明。

### 生产 target 现状

| target | 当前源归属 | 必须保留/修正 |
| --- | --- | --- |
| PrismRHI | Core CpuTrace/Environment、Platform Window、RHI 两后端/profiling | P5 分出 Core/Platform，RHI 不依赖 UI |
| PrismRenderer | Asset、Scene、Renderer；清单先重复包含 RHI 再 REMOVE_ITEM | 分出 Asset/Scene，生产源唯一 owner |
| PrismEngine | Engine/命令/序列化/自动化相关基础 | 保留实际依赖，不假装无 GPU 工厂 |
| PrismEditor | UI、Platform FileDialog、位于 RHI 目录的 ImGui backends | 按真实 target 归属移动 UI backend，保留条件编译 |
| PrismRender | main/Launcher/Host/HostScene/WaterValidation/FrameTimer/平台诊断 | 新 Application 库；main 不扩业务 |
| PrismDiagnostics | Windows BuildSymbolIdentity/MinidumpSymbolizer | 保留平台边界 |
| Tools/Harness/Tests | GoldenImageCompare、VisibilityCheck、Harness、McpServer 等 | 明确直接链接与测试重编译例外 |

源清单依据 `cmake/Prism{Rhi,Renderer,Engine,Editor,Application}Sources.cmake` 和根 CMakeLists；P5 才生成可执行的模块边界清单及负例。

## 线程与保活现状（1.10）

| 对象/路径 | 当前调用线程与保活 | 迁移约束 |
| --- | --- | --- |
| GLFW、ImGui BeginFrame/编辑、World | Host 主线程 | 窗口/编辑仍主线程，UI draw 改不可变副本 |
| backend Begin/End/resize、Feature/资源/上传 | 主线程，同步 Run 调用 | P7 独占 render lane，控制命令可靠有序 |
| RenderGraph 编译/屏障/队列提交 | SceneRenderer::Render 内联调用 | 公共图语义及 GPU queue mode 不变 |
| parallelRecordable Pass | `RenderGraph.cpp` 两处 `std::async`，native 或 DeferredCommandContext；统一 future.get 后才提交 | 常驻 pool；上下文独占、packet/lambda 保活到 join |
| AssetStreamingManager IO | 独立 std::thread；GPU TickUploads 在 Host BeginFrame 后 | IO 不直接发布/改 GPU；上传完成反馈提交 binding revision |
| ShaderManager/PipelineCache/descriptor/dynamic upload | renderer/shared 和后端 owner；现有并行录制可读引用 | 当前没有“所有方法线程安全”证明；P6 逐入口审计，未证明的共享写留 execution lane |
| World/Demo/程序化工厂与 hot reload | 主线程，部分调用设备；reimport 显式 WaitForGpu | recipe 或安全点同步派发，不能跨 lane 任意访问 device |
| GPU completion/texture/descriptor | 原 backend frame/fence 和资源 owner | CPU shared_ptr 存活/帧号不能代替 GPU fence |
| capture/max frames/history | completedFrameCount 同步循环；读取环境与 renderer mutable stats | P7 固定包内设置、实际完成帧/epoch，不以主线程领先帧号确认截图 |

尚无独立渲染线程或 RHI 线程；CPU Pass 并行不等于 GPU async compute。运行配置冻结、可靠控制/异常唤醒和纹理 leases 均属未实施任务。

## 已执行验证与入口

- Windows `cmake --build --preset windows-ci`：成功，`ninja: no work to do`；原始引擎输入未变。
- Windows CPU：11/11 通过（42.59 s）。日志/XML：`artifacts/architecture-refactor/20260828-hpwater-complete/P0/windows-ci/build-cpu-01/ctest.{log,xml}`。
- Windows 显式 GPU suites：5/5 通过（14.22 s），含 VulkanRuntime、D3D12/Vulkan FFT 与 tessellation。证据：`.../P0/d3d12/gpu-fixtures-01/index.json` 和 `ctest.stdout.log`；目录标签为驱动参数，测试实际覆盖两后端，不能把它算成全 Demo 验收。
- 验证脚本首轮 CPU 正反例 9/9：`artifacts/architecture-refactor/tool-tests/186ce057e323434bb7eafff56746110c/tests.json`。包括路径越界/immutable snapshot 拒绝、20 Demo dry-run、缺二进制、默认参数、隔离参数、防覆盖、子进程失败/环境隔离、同后端严格/跨后端宽门限分离。人工位图仅用于测试比较器，不是 Demo 基线。
- 最新脚本测试 10/10：`artifacts/architecture-refactor/tool-tests/2ab02c86cb6846bf98a5c5c793a2c86a/tests.json`；增加 HPWater 汇总 12 条 benchmark/7 条 parity 的原 schema 与独立 comparator/防覆盖检查，并要求失败测试确实启动子进程，不能把锁/前置检查失败当成正确传播子进程退出。
- 实际 HPWater D3D12 underwater 捕获成功：`.../P0/d3d12/hpwater-isolation-01/isolated-underwater.bmp`，同目录 `.json`/`.capture-input.json`/trace/identity/graph。使用独立二进制参数、输出和 working directory，没有向原 `artifacts/hpwater-validation/` 写入。单次捕获不等于全 HPWater 矩阵通过。
- D3D12 全 Demo 首轮试跑 `.../P0/d3d12/all-demo-pilot-01/index.json`：preview/showcase/reflections/shadows 四项完成捕获（showcase 已目视检查有实际场景输出），第 5 项 lights 退出 1，剩余 15 项未运行，不报 20/20。
- lights D3D12 单独复现：`.../P0/d3d12/lights-repro-01/`，仍是 `D3D12Resources.cpp:460` 创建单 descriptor 的 texture-view heap 返回 `0x8007000e`。未降低分辨率/画质；不能仅凭该 HRESULT 断言是显存不足，根因仍待进一步诊断。
- lights Vulkan 对照：完整 Windows/Editor 二进制（不是失败的 no-Editor 构建），`.../P0/vulkan/lights-repro-01/`；校验层报告多 mip/layer 在 UNDEFINED/SHADER_READ_ONLY_OPTIMAL 布局上不一致（VUID-vkCmdDraw-None-09600），应用明确因 validation errors 退出 1。没有关闭校验后冒充通过。
- Vulkan-only Windows preset configure 成功，build 失败：ApplicationHost 无条件引用 UI::CreateImGuiSystem、ImGuiSystem、EditorLayer/DebugPanel，但 `PRISM_RENDER_BUILD_EDITOR=OFF` 时未链接 PrismEditor，也没有编译期排除或无 Editor 实现，共 17 个 LNK2019/LNK1120。两次重现；完整日志 `artifacts/architecture-refactor/20260828-hpwater-complete/P0/vulkan/no-editor-build-01/build.log`。复用本地 GLFW source 和 DirectXMath 路径，无依赖升级。不能通过启用 Editor 冒充 Vulkan-only/no-Editor 组合通过，也不在此处改动源基线。
- Linux：本机 `wsl --list --quiet` 未返回可用发行版而显示安装帮助，`wsl --status` 退出码 50；尚无可用 Linux 执行环境，未安装系统组件。Linux 验收保持待完成。

新增/修改验证文件及数据流：

- `Validate-ArchitectureRefactor.ps1` → 配置/目录覆盖检查 → 独立 run 目录/跨进程锁 → 串行 child → `index.json`（输入、binary hash、产物 hash、执行状态）。`executed` 只代表运行完成，不代表视觉/性能通过。
- `ArchitectureValidation.Common.ps1`：路径约束、固定比较门限、子进程隔离与失败传播。
- `ArchitectureDemoCases.json`：20 个 key 与固定采样端点；catalog 不一致就失败。端点独立运行，不能冒充连续帧/lifecycle 验收。
- 原 `PRISM_RENDER_RDG_REPORT_PATH` 在首次可报告时写图，不保证与第 30 帧截图同帧；`index.json` 明示该限制。不能用这一张图报告证明逐帧模拟次数/历史，仍需后续帧级证据。
- `Compare-ArchitectureImages.ps1`：按两后端身份自动选择门限，严格比较强制 enforce，不覆盖已存在比较结果。
- `Capture/Validate/Summarize-HpWater.ps1`：新增可选二进制/输出与 NoClobber 参数，默认路径/原报告字段不变；架构调用使用新目录和独立缓存/trace/布局。
- `tests/scripts/ArchitectureValidationTests.ps1`：无需新增测试框架，运行 `pwsh -NoProfile -File tests/scripts/ArchitectureValidationTests.ps1`。

例：`pwsh -NoProfile -File scripts/Validate-ArchitectureRefactor.ps1 -BaselineId 20260828-hpwater-complete -Phase P0 -Backend d3d12 -Scenes preview -Repeats 3`。输出位于独立 run ID；第二次运行不会覆盖第一批。增加 `-DryRun` 仅显示命令，不能证明运行通过。

## 未完成门禁

### P0 前置修复进行中（用户确认后）

- 无 Editor：`PRISM_RENDER_HAS_EDITOR` 仅在应用链接 PrismEditor 时定义；Host 的 UI 包含、成员及调用按该条件编译，无 Editor 保留原导航、捕获与主循环路径。没有通过启用 Editor 绕过链接错误。
- D3D12 纹理绑定：`WriteTexture` 直接在集合已有的 shader-visible 槽创建同描述的 SRV，绑定保活仍持有纹理。独立 texture view 路径不变；两路径共用 `ToNativeSampledTextureViewDescription`，CPU 测试覆盖 2D/array/cube/cube-array/depth 与非法 mip。`TextureDescriptorGpuD3D12` 覆盖三轮各 16,384 个绑定、替换、弱引用保活及帧槽回收。
- 上述纹理修复后 lights 不再卡在单槽 CPU texture-view heap，而在 `Shader-visible sampler heap is exhausted` 处失败（`P0/d3d12/lights-descriptor-fix-01/`）。这不是完整修复成功；不可变 sampler 表复用属于额外分配策略调整，已向用户提出纳入请求，尚未实施。
- Vulkan 首个失败纹理已用临时 native handle 追踪确认为 `SpectralOcean.GradientNormalFolding`（4 layers、8 mips），见 `P0/vulkan/lights-resource-identity-01/`。非海洋场景没有生成谱海洋内容却将全 mip 视图交给通用材质。新增 `OceanFallbackResources` 提供所有子资源均已上传的 1×1×4、单 mip 零值/向上法线数组；只在谱海洋未启用时选择，HPWater 启用时仍绑定原始真实模拟纹理。临时追踪代码不是最终交付内容。
- 新文件：`src/Renderer/Features/Ocean/OceanFallbackResources.{h,cpp}`、`tests/TextureDescriptorGpuTests.cpp`。修改：`CMakeLists.txt`、`cmake/PrismRendererSources.cmake`、`src/Core/ApplicationHost.{h,cpp}`、`src/RHI/D3D12/D3D12Resources.cpp`、`src/RHI/D3D12/D3D12TypeConversions.{h,cpp}`、`src/Renderer/SceneRenderer.cpp`、`src/Renderer/SceneRendererSharedResources.{h,cpp}`、`tests/RhiTypeTranslationTests.cpp`、`tests/OceanFftGpuTests.cpp`。
- 数据流：共享 owner 初始化后备数组并完成正常上传 → 每帧/创建材质时按功能启用状态选择真实或后备视图 → descriptor set 保活 → 原帧/fence 回收。未改 shader、模拟算法、质量、分辨率或可见对象。
- 原 `build-windows-ci` 的 Ninja deps 对 `SceneRendererSharedResources.cpp.obj` 记录为 **0 个依赖**，showIncludes 前缀与实际编译输出不匹配。`lights-fallback-native-01` 的 SEH 来自包含旧 class layout 的混合对象；不将该次增量构建当作有效验收。新 `build-architecture-windows-ci` 对同一对象记录 **25 个依赖**，包含真实头文件。
- 两个全新目录按原 preset 开关构建：`build-architecture-windows-ci`（Editor/D3D12/Harness ON）与 `build-architecture-vulkan-ci`（三者 OFF）；configure/build 成功。复用本地 GLFW/ImGui/ImGuizmo 与原 DirectXMath/Slang，不升级依赖。设置 VSLANG=1033，但实际工具输出仍为中文；以 Ninja 实际依赖记录为准，不凭环境变量宣称语言已切换。
- 全新 Windows CPU **11/11**，无 Editor CPU **10/10**；证据分别为 `P0/windows-ci/fresh-cpu-01.{log,xml}`、`P0/vulkan/fresh-no-editor-cpu-01.{log,xml}`。构建日志为同目录 `fresh-build-01.log`、`fresh-no-editor-build-01.log` 和对应 configure 日志。
- 无 Editor Vulkan HPWater 在 native queue + validation 下完成 30 帧捕获，`P0/vulkan/no-editor-hpwater-fresh-01/`；stderr 只有校验层启用通知，无 validation error。检查生成的 Ninja PrismRender 链接命令和 target 图，没有 PrismEditor/ImGui/ImGuizmo 链接或 `PRISM_RENDER_HAS_EDITOR` 定义。
- 完整 Editor D3D12 preview 捕获通过（仍有原 warning 820），`P0/d3d12/preview-fresh-repair-01/`；与原 `preview-pilot-01` 同后端严格比较逐像素一致：MAE=0、RMSE=0、changed=0、SSIM=1，见 `P0/comparisons/preview-original-repair-01/`。因此 1.11 完成；这不等于 20 Demo 全覆盖通过。
- 修正测试 buffer usage 后，新建目录 GPU suites **6/6** 通过，`P0/d3d12/fresh-gpu-02/`。FFT 两后端实际执行了 fallback 逐层读取断言；其中 descriptor GPU fixture 显式启用 D3D12 validation。目录 backend 标签不代表所有 fixture 都启用同一校验配置，实际 Demo 的 validation 结果另列。
- 新建 fallback GPU fixture 首轮因测试输出 buffer 少了 RHI 要求的 CopyDestination usage 而失败，保留 `P0/d3d12/fresh-gpu-01/`；修正测试后必须重跑，不修改生产校验或忽略失败。
- 全新 Vulkan lights 已不再报告谱海洋多 mip/layer 错误，但还有两张单层单 mip 纹理的未初始化布局错误（`P0/vulkan/lights-fallback-fresh-native-01/`）。临时追踪进一步确认是 Game/Scene 各自 192×108 的 SkyView LUT，native handles 与校验错误完全相同，见 `P0/vulkan/lights-sky-identity-01/`。这是另一条关闭功能却绑定未初始化资源的路径，未修改 SkyAtmosphere，1.13 保持未完成。

上述 `P0/...` 相对证据根均为 `artifacts/architecture-refactor/20260828-hpwater-complete/`。重建前另存的原始可运行 exe 与运行 DLL 位于 `original-windows-binaries/`。原始 snapshot/golden 没有覆盖；新目录的构建通过也不能替代全场景视觉回归。

### 本轮最终验收与剩余问题

- 默认 `build-windows-ci`、`build-windows-vulkan-ci` 也已 `cmake --fresh` 重新配置并 clean build，不只留下旁路构建。首次链接个别工具 exe 出现 LNK1104，保留 `default-*-clean-build-01.log`；确认没有在运行的对应工具后原命令重试成功，未修改源码绕过。最终默认 preset 构建日志：`P0/windows-ci/final-build-02.log`、`P0/vulkan/final-no-editor-build-02.log`。重新生成后检查 FFT 测试对象记录 31 个头文件依赖，不再是零依赖。
- 默认目录最终 CPU：Windows 11/11、无 Editor 10/10，证据为 `P0/windows-ci/default-final-cpu-01.{log,xml}`、`P0/vulkan/default-final-no-editor-cpu-01.{log,xml}`。
- 完整 Editor Vulkan HPWater native + validation 的 30 帧捕获也通过，`P0/vulkan/editor-hpwater-final-native-01/`，stderr 无校验错误。
- D3D12 HPWater underwater/high/1280×800/30 帧/auto + validation：独立输出和 working directory 的 `P0/d3d12/hpwater-repair-underwater-02/` 通过；同后端严格对照原 underwater：MAE=0.000027、RMSE=0.000323、changed=0、max channel error=0.003922、SSIM=0.999938，见 `P0/comparisons/hpwater-underwater-original-repair-02/`。只有该固定视角样本，不宣称完整水线/时域矩阵。
- `hpwater-repair-underwater-01` 试跑使用了原隔离 working directory，因此不作为独立工作目录验收；复核原 imgui.ini 运行前后 SHA-256 同为 `7CCD6C2A94C9BFAC596DE5C8815F0C2C66548461650FF87CFBBE4371A88866AD`，原截图/报告未覆盖，随后用新目录重跑为上述 02。
- 新增独立 CTest `OceanFallbackGpuD3D12` / `OceanFallbackGpuVulkan`（原 FFT 全套测试仍保留）。显式 `--fallback-only` + 两后端 validation 均通过，逐层数值及 stderr 同时检查，`P0/gpu/fallback-only-validation-02/` 无任何 validation diagnostics。第一轮 Vulkan 测试呈现阶段的错误是新 fixture 没有将 acquired Present backbuffer 转为 RenderTarget；已在 fixture 中正确 clear/render，再 EndFrame，未改后端、未隐藏校验。
- 最终默认目录普通 GPU suite **8/8** 通过（15.17 s），`P0/d3d12/final-gpu-02/{index.json,ctest.stdout.log,ctest.xml}`；完整 FFT 默认配置没有启用 validation，不能用此结果覆盖下一项失败记录。
- **额外完整 FFT 严格校验未通过**：`P0/gpu/fallback-validation-01/d3d12.stderr.log` 包含 validation 527（COMMON 与 UAV/SRV 不匹配），出现在完整 fixture 第一次 EndFrame、新增 fallback 测试之前。该命令在 D3D12 失败后没有继续 Vulkan；不能标成两后端全套严格校验已通过，也尚不能只凭发生位置宣布根因属于旧代码。普通数值 GPU suite 与独立 fallback strict 验收分开记录。
- 当前仍需确认/处理：lights 的 sampler 表重复分配、关闭天空的真实/后备绑定及开关路径、完整 FFT strict fixture 的状态归因。原始 P0 图像/时域/性能矩阵仍未完成；1.12–1.14 不勾选。按 openspec-apply-change 的范围守卫，新增分配策略/功能修复先请求纳入，不无声混入 P1。
- 部分修复恢复点使用独立 ID `20260828-p0-prerequisite-partial`，保留原始 `20260828-hpwater-complete`。此恢复点只用于恢复当前进度，不是 P0 接受基线。恢复步骤仍为校验 manifest 后复制到新的空目录，禁止自动覆盖原工作区。
- 该部分恢复点已创建并验证 **818 个文件**，manifest SHA-256：`DBA25861841EA2E234699BCFD799D65AA18444F222D8CE6C2E97AB63E5988923`。本条记录及最后的 GPU 汇总是在恢复点创建后补入文档；代码未再变更。OpenSpec 严格校验通过，实施进度仍为 **6/112**。

P0 仍须完成全 Demo/HPWater 图像稳定性、真实连续帧与控制序列、关键区域/图状态/共享模拟次数、性能分布及所需工具补齐。旧 warning 820 在 preview validation 试跑中仍出现，单列为基线警告；不能抑制错误或修改算法放行。缺环境/未运行均不算通过。

最新用户确认已批准 sampler 表复用、关闭天空后备/开关路径和完整 FFT strict fixture 三项，按 1.12、1.13、1.15 继续实现，无需再次请求确认。原始快照保留；新增修复基线另存，不覆盖 golden。P0 没通过前不开始 Host 所有权迁移；P1–P8 均未验收。

### 本轮实现与构建注意事项

- 新增 `src/RHI/D3D12/D3D12SamplerTable.{h,cpp}`：完整原生参数键、layout 内弱缓存、不可变表、帧 fence 退役。`D3D12Resources` 在绑定时按最终配置驻留表，合法默认值兼容未使用的反射槽位，首次并行绑定受互斥锁保护；不提高 2048 sampler heap 容量。
- 修改 `SkyAtmosphere.{h,cpp}` 和 `SceneRenderer.cpp`：已上传黑色 2D 后备；根据 physicalAtmosphereEnabled && skyboxEnabled 在当前帧槽刷新材质/海洋/延迟光照/天空/tonemap/水合成消费者。启用路径继续使用真实 LUT 和原 Pass。
- 新增 `tests/SkyAtmosphereGpuTests.cpp`：两套真实 SceneRenderer，四阶段开关覆盖每个 frame slot，并校验图中天空生产 Pass。扩展 texture descriptor fixture：4096 表复用、等值/异值更新、默认槽位、并发首次绑定和退役。完整 FFT fixture 修复已创建 Storage buffer 的初态、已上传 local texture 的初态、Foam 前后位移/梯度布局、compute-only 呈现契约；未修改 shader/数值容差。
- 构建陷阱再次复现：Launch-VsDevShell 改变 PATH 后选中 VS 自带 CMake 4.3.1-msvc1，重新生成的 Ninja 本地化 includes 前缀与编译输出编码失配，D3D12GraphicsDevice 对象 `#deps 0`，类布局变更没有重新编译消费者。`sky-sampler-strict-03` 运行被主动终止，不能作为有效测试结果。固定调用 `C:/Users/23500/.cmake-deps/cmake/win/x64/bin/cmake.exe`（4.1.2），重配置并 clean build；恢复正确前缀后同对象记录 22 个依赖（含 Resources/SamplerTable）。所有后续配置/构建必须使用该绝对路径，不靠 DevShell 后的 `cmake` 名称解析。

### 本轮验证结果（同一原始基线下独立存放）

以下路径均相对 `artifacts/architecture-refactor/20260828-hpwater-complete/`：

| 验证 | 结果 | 证据 |
| --- | --- | --- |
| 固定 CMake 完整 Windows clean build 与最终链接 | 通过；检查 RHI header 依赖 22 项 | `P0/windows-ci/pinned-cmake-clean-build-01.log`、`prerequisites-final-build-02.log` |
| Vulkan-only/no-Editor 最终构建 | 通过；SceneRenderer 对象 87 项头依赖 | `P0/vulkan/prerequisites-final-no-editor-build-02.log` |
| Windows / no-Editor CPU | 11/11、10/10 通过 | `P0/windows-ci/prerequisites-final-cpu-02.{log,xml}`、`P0/vulkan/prerequisites-final-no-editor-cpu-02.{log,xml}` |
| 描述符压力、完整 FFT 两后端显式 strict | 通过；sampler 每轮 4096 表只占 3 槽，异值表另占 3 槽，3 轮均回收；FFT 原数值断言不变 | `P0/gpu/prerequisites-strict-04/PrismTextureDescriptorGpuTests-.*.log`、`PrismOceanFftGpuTests-{d3d12,vulkan}.*.log` |
| 真实双视图天空开关，Lights preset，显式 strict | 两后端通过全部 4 阶段、每阶段覆盖所有帧槽 | `P0/gpu/sky-lighting-strict-01/{d3d12,vulkan}.*.log` |
| 默认 deferred 双视图天空开关 | D3D12 strict 通过；Vulkan strict **失败**，见下文独立 PlanarReflections 问题 | `P0/gpu/prerequisites-strict-04/PrismSkyAtmosphereGpuTests-*`、`P0/gpu/sky-texture-trace-01/` |
| lights 默认画质，native / serial | D3D12/Vulkan 全部捕获成功，stderr 无新增错误；两 API 各自 native/serial 逐像素一致 | `P0/{d3d12,vulkan}/lights-prerequisites-{native,serial}-01/`、`P0/comparisons/lights-{d3d12,vulkan}-native-serial-01/` |
| preview 与原始 golden | 逐像素一致，MAE/RMSE/changed=0、SSIM=1 | `P0/d3d12/preview-prerequisites-01/`、`P0/comparisons/preview-prerequisites-original-01/` |
| atmosphere 原二进制 / 新二进制 | 同输入、同后端逐像素一致，MAE/RMSE/changed=0、SSIM=1 | `P0/d3d12/atmosphere-original-binary-01/`、`atmosphere-prerequisites-01/`、`P0/comparisons/atmosphere-prerequisites-original-01/` |
| HPWater underwater/high/1280×800/30/auto，D3D12 strict | 通过；对原图 MAE=0.000020、RMSE=0.000279、changed=0、SSIM=0.999954 | `P0/d3d12/hpwater-prerequisites-underwater-01/`、`P0/comparisons/hpwater-prerequisites-underwater-original-01/` |
| HPWater Full Editor Vulkan/native/30 | 捕获与 validation 通过 | `P0/vulkan/hpwater-prerequisites-native-01/` |
| HPWater no-Editor Vulkan-only/native/30 | 捕获与 validation 通过，不链接 Editor | `P0/vulkan/hpwater-prerequisites-no-editor-01/` |
| 最终默认 GPU CTest 配置 | 10/10 通过（40.45s）；**并非全套 Vulkan strict**，默认未强制安装校验层 | `P0/d3d12/prerequisites-final-gpu-01/{index.json,ctest.stdout.log,ctest.xml}` |

第一次额外 FFT strict 的根因已逐项归因到 fixture：GpuOnly Storage buffer 创建后本来就是 UAV，localTexture 已上传为 SRV，原测试却用 Undefined 作为 before state；Foam 采样位移/梯度时缺少基 mip 的 UA→SRV→UA；compute-only 帧没有走正确的呈现前状态。均只修测试，不改海洋生产算法。CTest Vulkan FFT/fallback/天空测试增加 validation 输出失败匹配，避免最后一帧的晚到校验错误只打印日志却被计作通过；实际 strict 运行同时检查退出码与 stderr。

新发现且未修改的独立问题：默认 deferred + SSR 配置即使关闭平面反射，ScreenSpaceEffects 仍绑定未生产的 PlanarReflections color texture。临时原生句柄追踪确认失败的 `0x4370000000437` / `0x78b000000078b` 是两视图各自 160×100 的半分辨率 color texture（后跟同尺寸 depth），不是 192×108 SkyView 或 1×1 后备。失败发生在 sky phase 0，不能用 Lights preset 的成功代替这条默认配置验收。默认失败 fixture 保留，未关闭 SSR 或修改 Demo 默认设置来放行；后续修复该独立问题需要单独确认纳入 P0。

临时追踪已完全移除，`src/RHI/Vulkan/VulkanContext.cpp` 与上一恢复点 SHA-256 一致：`D18DA7A2B459018DA9D4AD72C40A8D831FEE26BAFB4CE5DB8F7EBF76F51F23D7`。本轮未修改任何 shader / 海洋算法 / PlanarReflections / ScreenSpaceEffects。原始 809 文件及上一恢复点 818 文件均重新验证通过。

本轮恢复点 ID 使用 `20260828-p0-sampler-sky-fft`，仅保存当前部分修复进度，不代表 P0 全矩阵接受。1.12、1.15 标为完成，1.13 的完整 Vulkan 消费者验收和 1.14 仍保留未完成；P1–P8 不启动。

恢复点已创建并验证 **821 文件**，manifest SHA-256：`FE2665C2CDB53E1EF2B4EFBF16A3C9319AA970FAE7AC96DDDFB781F85300B69C`。本条封存说明在快照后补入，代码未变更；OpenSpec 严格校验通过，8/113。

隔离注意事项：最初的 `sky-fft-strict-02` 诊断未指定 queue cost cache，运行时写入了默认 `automation/cache/rdg-queue-cost-model.json` 的测试配置记录。后续测试已明确使用独立 cache（手工运行位于该次证据目录，CTest 位于对应 build 目录）；没有清空默认 cache，以免覆盖既有记录。所有图像对照使用隔离缓存，原始截图/快照不覆盖；此项不冒充首次诊断完全无运行时缓存副作用。

### PlanarReflections 前置修复（用户再次批准，1.16）

以上“待确认”是历史记录；用户“好的，请继续实现”已批准此独立缺陷纳入 P0，不再等待重复确认。

- 修改 `src/Renderer/Features/ScreenSpaceEffects.{h,cpp}`：每视图 owner 持有上传为 ShaderResource 的黑色 1×1 后备；Resize 的全新集合先绑定后备，Update 仅替换当前安全帧槽的 binding 21。选择真实输出的条件与图中 producer、shader 常量相同：deferredEnabled && planarReflectionsEnabled。不改变 PlanarReflections 本身、图依赖、shader、默认设置或算法。
- 修改 `tests/SkyAtmosphereGpuTests.cpp`：保留默认 deferred+SSR 测试和可选 Lights preset；扩展至 8 阶段，两个真实视图各自开关，断言天空/平面反射 producer，每阶段覆盖所有帧槽。phase 4/6 在 GPU idle 安全点从 320×200→400×250→320×200 重建，分别从关闭/开启状态进入；检查每视图实际输出尺寸。
- 无新增生产文件。数据流是设置→当前帧常量/descriptor→原 Pass→SSR；关闭时 descriptor 指向永久只读后备，不新增渲染 Pass。
- 固定 CMake 的 Windows 和 no-Editor 构建通过：`P0/windows-ci/planar-build-02.log`、`P0/vulkan/planar-no-editor-build-01.log`；SceneRenderer 编译对象仍有 87 个有效头文件依赖。CPU 分别 11/11、10/10，`P0/windows-ci/planar-cpu-01.{log,xml}`、`P0/vulkan/planar-no-editor-cpu-01.{log,xml}`。
- 默认双视图完整 fixture 的 **D3D12/Vulkan × native/serial 四轮显式 strict 全部通过**：`P0/gpu/planar-strict-01/`。Vulkan stderr 确认 Khronos validation layer enabled，没有 validation 消息；D3D12 保留已有 820 clear-value 性能警告，无新增错误。旧 phase 0 失败路径未移除、未改为 forward 或关闭 SSR。
- D3D12/Vulkan preview/reflections/atmosphere 原始同后端图像对照均 MAE=0、RMSE=0、changed=0、SSIM=1：`P0/comparisons/planar-{preview,reflections,atmosphere}-original-01/` 及 `planar-vulkan-{preview,reflections,atmosphere}-original-01/`。
- HPWater 默认视角 native/1280×800/30 帧，两后端严格同后端对照通过（并非逐像素一致）：D3D12 MAE=0.000010、RMSE=0.001353、changed=0.000041、SSIM=0.999966；Vulkan MAE=0.000002、RMSE=0.000456、changed=0.000011、SSIM=0.999996。证据 `P0/comparisons/planar-{d3d12,vulkan}-hpwater-ocean-original-01/`。尚不代表三次稳定性或全水线/时域验收。
- 修复后受影响四 Demo 的实际 validation 捕获两后端均通过：`P0/{d3d12,vulkan}/planar-affected-native-01/`；补拍原二进制图像在 `planar-original-native-01/`。原二进制这些补拍未开 validation，仅提供外观对照，不能当作旧 Vulkan 状态正确的证据；新二进制始终显式开启。原始 golden 没有覆盖。
- 1.13、1.16 完成，进度 10/114。其余 P0 矩阵继续，尚未以部分通过替代整体门禁。
- 最终常规 GPU suites：Windows 10/10（44.22s）、no-Editor 5/5（22.50s），`P0/d3d12/planar-final-gpu-01/` 和 `P0/vulkan/planar-final-no-editor-gpu-01/`。Vulkan CTest 默认仍不强制校验层；显式 strict 结论以 `planar-strict-01` 为准，不混淆两类测试。
- 复核原始快照中的 113 个 assets/Ocean/Demo 设置文件，哈希差异为 0；对上一修复快照，生产源码差异只有 ScreenSpaceEffects 的 .h/.cpp。
- 全 Demo native strict 初测 `P0/vulkan/planar-all-demos-native-01/` 在第 4 个 `shadows` 失败，之前 preview/showcase/reflections 完成；错误为两张单层单 mip 图像处于 Undefined 却按 ShaderReadOnly 采样。图报告没有 GTAO Pass，AmbientOcclusion 为 inactive/Undefined，DeferredLighting binding 24 仍指向 GTAO 输出，初步归因于关闭 GTAO 的绑定，尚未修改。后续场景未执行，不能标成 Vulkan 20/20。D3D12 全目录检查另在运行；其前四场景与原图逐像素一致，证据 `P0/comparisons/planar-full-{preview,showcase,reflections,shadows}-original-01/`。
- 当前获批修复代码将独立封存为 `20260828-p0-planar-fallback`，不覆盖三个旧恢复点；这是 P0 前置修复恢复点，不是全 P0 验收基线。新增 GTAO 问题只诊断/保留证据，未经确认不把修复悄悄并入本轮。
- 该恢复点已创建并验证 **821 文件**，manifest SHA-256：`947262B58E79A327353C161B067915F8A93C0EEBE88E4138E7E009E0C9162AE6`。1.14 完成，进度 **11/114**。本条封存记录、1.14 勾选和后续只读诊断结果在快照后更新，生产代码未再修改。恢复仍只复制到新的空目录并校验，不自动覆盖工作区。

#### 本轮最终状态

- **D3D12 全部 20 Demo** 的 native/默认画质/1280×800/30 帧 capture + strict 通过，20 张图和逐项报告齐全，stderr 无除既有 820 外的 validation 消息；`P0/d3d12/planar-all-demos-native-01/index.json` 为 executed、completed=20。这不是 20 Demo 都完成原图/三次重复/连续帧/性能验收。
- 补测 `P0/gpu/planar-forward-no-editor-strict-01/`：D3D12 与 Vulkan 的 Lights forward preset、Vulkan-only/no-Editor 的默认 deferred 各通过一次 8 阶段双视图/resize strict；包括 deferred=false 时即使 planar 标志为 true 也不产生 Pass。连同前述 native/serial，实际显式 strict 共 7 轮。
- Vulkan 全目录仍为 3 个完成、shadows 失败、后续 16 个未执行（本轮另有 atmosphere/HPWater 独立通过，不将这些混为全目录完成）。两张失败图像为 `0x4970000000497` / `0x8150000000815`，尚未进行逐原生句柄定位；原图报告/源码支持关闭 GTAO 的 consumer 绑定问题，不宣称已修复。
- 原始可执行文件也在同一 shadows/native/strict 配置失败，`P0/vulkan/shadows-original-strict-01/`。初次旧日志被校验层默认 10 次同 VUID 限制截断；根据本机 layer JSON 的已声明环境项，仅把该诊断子进程 `VK_LAYER_DUPLICATE_MESSAGE_LIMIT` 提高到 1000 后复现，`shadows-original-strict-expanded-01/` 共 196 条：三张旧数组各 64 个子资源、四张单 mip/layer 图像各 1 条。未关闭校验、过滤消息或改渲染配置，父进程参数已还原。该结果说明原始场景本来就不能通过 strict；不是对当前两张原生句柄身份的独立证明。原始与当前的 ShadowLab GTAO=false、DeferredLighting binding 24 无条件绑定逻辑相同。
- 下一步需要明确把 **关闭 GTAO 等可选 Feature 的无效资源绑定排查/修复** 纳入 P0；当前不悄悄新增其修复任务或进入 P1。1.5–1.9 的完整基线、控制序列、性能和 Linux 环境仍未验收。所有子进程均已结束，没有留下后台 GPU 作业。

### 可选 Feature 关闭状态统一修复（用户已批准，1.17）

以上等待确认是历史状态；最新“好的”已批准同类问题统一排查/修复，新增 1.17。采用 openspec-apply-change，仍在 P0，当前 11/115，不提前进入 P1。完整 producer/consumer/图访问清单见 `docs/OPTIONAL_FEATURE_BINDINGS_AUDIT.md`。

- `ScreenSpaceEffects.{h,cpp}`：全白中性 AO 后备，Deferred binding 24 / SSR binding 20 在当前帧槽选择；初始 SSR 集合绑定后备。真实 GTAO、SSR 算法和 producer 不变。
- `SceneRenderer.cpp`：普通/海洋材质的 spectral 与 local 绑定同时检查 renderWater / local.enabled，不再把“已分配”当作“本帧有输出”；Legacy FFT 选择同步包含 renderWater。沿用现有 OceanFallbackResources 和 VT atlas，不改海洋模拟、共享 owner 或默认设置。
- 调试中间纹理 capture 同样检查 deferred / bloom / HPWater 有效 producer。缺失输出时沿用已有 Water 未分配时的当前 scene color 后备，而非采样 Undefined 或上一代纹理；不强制开启 Pass。该后备不是有效的所请求中间阶段 golden，正常 Tonemap 与启用阶段的行为不变。
- `tests/SkyAtmosphereGpuTests.cpp` / `CMakeLists.txt`：扩展 optional/ocean 模式及 GTAO producer 断言，两个真实视图、8 阶段、每阶段覆盖所有帧槽、两次 resize；另有 water 模式用于中间纹理 capture 验证。保留默认 deferred+SSR 和 Lights forward 路径，不以删掉失败路径放行。
- 对原始基线核对 114 个 assets / Ocean 实现 / Demo 设置相关文件，hash 差异为 0；相对上一恢复点，生产代码只改 ScreenSpaceEffects 两文件与 SceneRenderer.cpp，另改测试和 CMake。没有新增生产模块或修改 shader。

#### 本轮已完成的验证

所有路径相对 `artifacts/architecture-refactor/20260828-hpwater-complete/`，未覆盖已有证据。

| 检查 | 结果 | 证据 |
| --- | --- | --- |
| hidden water + allocated local 的负例 | 修复前 Vulkan phase 0 失败，日志保留 | `P0/gpu/ocean-bindings-before-01/` |
| Bloom 关闭 / forward GBuffer capture 负例 | 两者修复前 phase 0 失败，日志保留 | `P0/gpu/capture-bindings-before-01/` |
| default/optional/ocean × 双后端 × native/serial | 12/12 strict 通过；在最后 capture 补充修复之前 | `P0/gpu/optional-bindings-strict-02/` |
| 最后完整 Windows 构建 | 通过；SceneRenderer header deps=87 VALID | `P0/windows-ci/optional-bindings-final-build-01.log` |
| 最后无 Editor 构建 | 通过；第一次两个 exe 链接遭临时写入失败，重试通过，未删文件/改代码 | `P0/vulkan/optional-bindings-final-no-editor-build-01.log`、`-02.log` |
| CPU | 完整版 11/11；无 Editor 10/10 | `P0/windows-ci/optional-bindings-cpu-01.{log,xml}`、`P0/vulkan/optional-bindings-cpu-01.{log,xml}` |

构建额外注意：不仅要用固定 CMake 执行 build，也要保证 configure 缓存/自动 regeneration 中的 CMAKE_COMMAND 同样固定；本轮使用 4.1.2 `--fresh` 重配并重建。Ninja Multi-Config 的依赖检查明确传 `-f build-RelWithDebInfo.ninja`，避免用默认配置检查错误的对象集合。中途构建/检查失败日志全部保留，不冒充最终验收结果。

全 Demo、最终 capture strict、HPWater 同后端对照和新恢复点仍在验证，1.17 暂不勾选；原 1.5–1.9 与所有 P1–P8 保持未完成。

#### 后续发现与补充修复

- `optional-capture-strict-01` 前 5 项通过，Vulkan 的 HPWater 启用阶段失败；不捕获的 `water-enable-isolation-01` native/serial 均在 phase 1 失败。短期诊断 `water-enable-trace-01` 精确定位为 OceanGpuQuery binding 18 的 1×1 dummy local 图像（`0xb2a0000000b2a`），不是 Water GBuffer。它虽提供 initial data，却在 BeginFrame 之后惰性创建，上传还未刷新就被查询消费。
- 在 `SceneRenderer::Initialize` 提前创建同一零值、同格式后备，删除帧中创建代码；不修改 OceanGpuQuery、海洋 shader 或算法。后备跨 resize 保活，原 query 参数/时序不变。VulkanResources 临时追踪已完全移除，文件 hash 与上一快照一致。
- 新增 `scripts/Test-OptionalFeatureBindings.ps1`，main 覆盖四种 fixture × 双队列 × 双后端，capture 覆盖三种中间纹理模式 × 双后端；要求实际 Vulkan validation enabled，记录 binary hash、环境、逐项结果并串行持锁。CTest 增加 water 模式，常规 GPU 数量变为 Windows 16、无 Editor 8。
- 新最终构建两种配置通过：`P0/windows-ci/optional-query-final-build-01.log`、`P0/vulkan/optional-query-final-no-editor-build-01.log`；测试注册重生成使用已固定 CMake，未触发额外编译。
- 原始 Vulkan 二进制 native 20/20 视觉参考捕获完成（validation 关闭，仅作参考）：`P0/vulkan/optional-original-all-demos-native-01/`。修复版 `optional-all-demos-native-01` 显式 strict 前 16 个通过，PBF 第 17 个失败，后三个未在该轮运行；前 16 个原图严格对照全部通过，HPWater 默认视角本次逐像素一致。
- PBF 报六层图像为 General 而非期望 ShaderReadOnly。serial strict 可通过，native 失败；源码指向 Fluid.Environment 图外静态消费者与首用 Compute 的 prologue Common 转换冲突，尚未逐原生句柄确认。该已开启 Feature 的图依赖/跨队列问题不属于关闭状态绑定修复，RenderGraph/Fluid 实现未修改，不切默认 serial 放行。
- 原 native 与修复 serial 的 PBF 图像诊断也超过严格门限（changed=0.001955、SSIM=0.997015）；属于混合队列条件，不当作等输入对照或新绑定修复的算法回归证明。保留 `P0/comparisons/optional-vulkan-pbf-serial-diagnostic-01/` 的失败结果，未放宽门限。

因此 1.17 与 P0 整体暂不标完成；后续必须明确纳入独立的 Fluid/native 图依赖修复后再继续全目录门禁。

#### 最后生产二进制验证（query 后备提前初始化之后）

- 显式严格开关矩阵 **16/16**：`P0/gpu/optional-final-strict-01/`；中间纹理 capture 矩阵 **6/6**：`P0/gpu/optional-capture-strict-02/`。包含关闭 local 的首个 HPWater 查询、两视图、全部帧槽、8 阶段和两次 resize。Vulkan 确认 layer enabled；D3D12 仅已有 820 warning。
- CPU 完整版 **11/11**、无 Editor **10/10**：`P0/windows-ci/optional-final-windows-cpu-01.{log,xml}`、`optional-final-no-editor-cpu-01.{log,xml}`。
- D3D12 native/1280×800/30 帧全目录 **20/20 capture/strict**：`P0/d3d12/optional-all-demos-native-02/`。对上一已保存的 Planar 前置修复图像，19/20 通过严格同后端门限，PBF 未通过：MAE=0.000161、RMSE=0.003146、changed=0.001426、SSIM=0.997806；证据 `P0/comparisons/optional-d3d12-*-baseline-01/`。不把该参考写成原始二进制，不把 capture 20/20 写成视觉 20/20。
- HPWater 默认视角对**原始二进制**的最终同后端比较两 API 均通过：D3D12 MAE=0.000039、RMSE=0.002730、changed=0.000228、SSIM=0.999833；Vulkan MAE=0.000002、RMSE=0.000456、changed=0.000011、SSIM=0.999996。`P0/comparisons/optional-d3d12-hpwater-original-01/`、`optional-query-vulkan-hpwater-original-01/`；最终 Vulkan capture 在 `P0/vulkan/optional-query-hpwater-native-01/`。这不是全部七视角/水线时域验收。
- Vulkan 对剩余 fluid-render/fluid-caustics/fluid-toon 的独立 strict 重跑均失败，均是 `0x4e000000004e` 六层图像 General→ShaderReadOnly。证据 `P0/vulkan/optional-fluid-*-native-01/`。连同 PBF，此四个 Fluid 路径的 native 门禁没有通过；失败日志完整保留，没有启用 serial 代替验收。
- 原始 Vulkan 对照中前 16 场景的 Pass 名称和 graphSignature 均与修复前一致；不等同于已完成 1.5 的捕获帧状态、共享模拟次数与历史重置仪表。
- 重新校验 114 个 assets / Ocean / Demo 设置文件仍为零 hash 差异。新增生产修复仍仅 ScreenSpaceEffects.{h,cpp} 和 SceneRenderer.cpp；本轮没有改 RenderGraph、Fluid、Ocean 算法文件或 RHI 实现。

#### 本轮封存状态

- 常规 GPU 最终 **16/16**（完整版，122.28s）、**8/8**（无 Editor）：`P0/d3d12/optional-final-gpu-01/`、`P0/vulkan/optional-final-no-editor-gpu-01/`。这些组件测试不等同于全 Demo 门禁。
- 无 Editor 显式 strict **8/8**：`P0/gpu/optional-no-editor-strict-01/`。连同最后两后端 16 项主矩阵和 6 项 capture，最后二进制显式严格测试共 **30 项通过**。
- 最后 Vulkan PBF 再复核仍为同一 General→ShaderReadOnly 失败，`P0/vulkan/optional-final-pbf-native-01/`；四个 Fluid/native 路径均未验收。没有修改其 RenderGraph 声明/交接逻辑或默认队列。
- D3D12 PBF 同一最终二进制、同 native/1280×800/30 帧设置独立运行三次，capture/strict 全通过，但 1 对 2、1 对 3 的图像比较均失败。分别 changed=0.001485 / 0.002271、SSIM=0.997690 / 0.996294。证据 `P0/d3d12/optional-pbf-repeat-native-01/`、`P0/comparisons/optional-d3d12-pbf-repeat-1-vs-{2,3}/`。这直接复现运行间波动，说明不能仅由一次新旧图像失败断言本次绑定修复改变了 PBF；也不能据此豁免视觉门禁。尚未确定其完整根因，未改 solver 或门限。
- 验证工具 CPU 自测 **10/10**，包含原/跨后端门限正反例、子进程失败、隔离输出和不覆盖：`artifacts/architecture-refactor/tool-tests/d92a28b257004e6197ca8b1bce2cfe08/`。
- 原始 809 文件快照、上一 Planar 821 文件快照重新验证通过。最新代码将封存为 `20260828-p0-optional-bindings-partial`；它是部分修复恢复点，不是 P0 完成基线。
- 任务 **1.17 未勾选，整体 11/115**。本轮绑定修复代码已实现并通过专项测试，但全目录/稳定图像验收未闭环。1.5–1.9、Linux 环境和 P1–P8 均不冒充完成。
- 下一步需用户明确纳入 **Fluid/native 共享资源图依赖与队列交接、PBF 图像稳定性** 两项 P0 排查/修复；不得直接修改同步/模拟算法、放宽门限或开始架构阶段来绕过。全量七视角/质量/尺寸/时域/性能矩阵仍有未完成项。

恢复点 `20260828-p0-optional-bindings-partial` 已创建并验证 **824 文件**，manifest SHA-256：`4F3CCE432A5A39F3317AE52704651D7D46FF5F796BFEAEEBB45A50CC263673CD`。本条封存说明在快照后追加，生产代码未再修改。OpenSpec 严格校验通过，实际进度 11/115；所有 GPU/构建/测试子进程均已结束。恢复仍只复制到新的空目录，不自动覆盖工作区或旧 golden。

### 后续已授权的 Fluid / PBF P0 修复

用户“好的，请继续”已明确批准此前两项问题，新增 1.18/1.19，任务总数为 117。完整文件清单、数据流、测试及数值见 [Fluid P0 验证](FLUID_P0_VALIDATION.md)。本阶段没有启动 ApplicationHost/RenderGraph 拆分或渲染线程实现。

- 共享环境：SceneRenderer 提供唯一 Environment 图导入；Fluid 不再私有导入同一 cubemap，Deferred（包含 Skybox）、Forward、Transparent、Planar、WaterOptics 的实际消费者声明读取。只改资源接线和读依赖，未改 RHI/队列算法或 HPWater shader。
- 重复稳定：原 GPU 原子网格插入造成邻居浮点累加顺序变化；新增桶内 ID 排序及必要 UAV barrier，保持 PBF 方程、参数、步数和默认 Demo 设置。初始值相同的旧模拟首步已有差异；修复后三次 GPU 数据重放、两 API 各三次 PBF 截图以及 Vulkan Toon 三次截图均完全一致。排序每子步增加一个 dispatch，其性能仍须 P0 完整测量。
- 已完成构建与 CPU 验证：完整 Windows / Vulkan no-Editor 均通过，CPU 11/11 + 10/10；87 个 SceneRenderer header deps 均 VALID，固定 CMake 4.1.2。已保留编译和 LNK1168 输出占用失败记录，未删除 exe 或终止未知进程，重试后构建通过。
- 共享资源修复后的四 Fluid × 双 API × native/serial 共 16 组 capture/strict 通过。最终 GPU 数值三配置 strict 通过；最终 PBF 两 API 三次、Vulkan Toon 三次、相关 HPWater/Fluid 各 API 四项捕获也通过。次数不代替图像验收，原失败报告保留。
- HPWater 默认视角、1280×800、native、f30 对原始二进制的同后端比较均通过：D3D12 SSIM=.999981、changed=.000042；Vulkan 完全相同。并未完成完整七视角/三质量/两尺寸/时域矩阵，不宣称全面效果已验收。
- 旧图门禁仍失败：PBF D3D12（上一修复版）SSIM=.996944、changed=.001870；PBF Vulkan（原始二进制）SSIM=.996851、changed=.001966；Toon Vulkan SSIM=.998911。其余三张 D3D12 表面 Demo、两张 Vulkan realistic/caustics 满足门限。未覆盖旧 golden、未放宽 V2，也未采用跨 API 宽门限放行。
- `P0/d3d12/pbf-sorted-repeat-native-01/` 第三次曾在初始化期间 exit 1，无 validation error/截图/crash report，原因未查明。代码未再改动的 `-02` 三次均成功且相同；原失败批次仍标 failed，不能用后续成功抹去。

需要用户明确批准的下一步是 **PBF（双 API）及 Toon Vulkan 的新稳定视觉基线**，不是再次批准已完成的代码排查。继续保留原样本和 V2 门限，新基线未获批准前不勾选 1.17–1.19、不进入 P1；整体仍为 **11/117**。Linux、完整时域/性能/所有 Demo 矩阵按原计划保持待验收。工作区已有的 `imgui.ini` 变化未回滚，未将其归为本次引擎改动。

最终专项严格开关矩阵已完成：`P0/gpu/fluid-final-optional-strict-01/` **16/16**，`P0/gpu/fluid-final-no-editor-strict-01/` **8/8**；连同 `pbf-replay-final-01` 的三配置 GPU 数据测试为 **27 项专项 strict**。显式 Vulkan layer enabled，两视图、全部帧槽及 resize 覆盖通过；不是完整 P0 视觉/时域/性能通过。原 809 文件与上一 824 文件快照再次验证无变化；本轮保存 `20260828-p0-fluid-determinism-partial`，作为可恢复代码检查点而非新 golden，恢复仍只允许复制到新的空目录。

### 三项视觉参考批准后的最终复核

用户最新“好的，请继续”已批准前述三个稳定样本；登记表为 `scripts/ArchitectureVisualBaselineRevisions.json`，用法见 [视觉基线说明](ARCHITECTURE_VISUAL_BASELINES.md)。严格限定完整 Editor、单 Game view、默认参数、native、1280×800、f30；旧图与旧失败报告不覆盖，V2 门限不变。新工具按精确输入匹配、验证图像/metadata/源码来源，不启用修订时 PBF 旧图三次比较仍失败。

本轮仅修改验证脚本、工具测试、计划及文档，没有修改 C++、shader、Demo 设置或重建生产二进制。与上一 827 文件检查点核对既有引擎/测试 C++ 源无差异；工具测试变更另列。最终运行证据均位于 `artifacts/architecture-refactor/20260828-hpwater-complete/`：

| 检查 | 结果 | 证据 |
| --- | --- | --- |
| 全目录 native capture/strict | D3D12 20/20、Vulkan 20/20；实际层启用且无新增校验诊断 | `P0/{d3d12,vulkan}/approved-final-all-demos-native-01/` |
| 全目录同后端 V2 | 各 20/20；仅三项使用批准参考，其余 D3D12 对上一修复版、Vulkan 对原始二进制 | `P0/comparisons/approved-final-all-demos-{d3d12,vulkan}-01/` |
| HPWater 直接原始图对照 | D3D12 SSIM=.999998、changed=.000020；Vulkan SSIM=.999996、changed=.000011 | `approved-final-hpwater-d3d12-original-01/` 与全目录 Vulkan 中 HPWater 项 |
| 四 Fluid serial strict | 每 API 4/4，与 native 合为 16/16 | `P0/{d3d12,vulkan}/approved-final-fluid-serial-01/` |
| 当前 native/serial 图像一致性 | 八组逐像素一致；仅是队列一致性检查，不批准额外 serial golden | `P0/comparisons/approved-final-queue-diagnostic-*/` |
| CPU | 完整 Windows 11/11、无 Editor 10/10 | `P0/windows-ci/approved-final-cpu-01/` |
| GPU，显式 validation 请求 | 完整构建 16/18、无 Editor 8/9；均因 tessellation fixture 失败 | `P0/d3d12/approved-final-gpu-01/`、`P0/vulkan/approved-final-no-editor-gpu-01/` |
| 工具测试 | 原工具扩充后 12/12、新基线工具 10/10 | `artifacts/architecture-refactor/tool-tests/e1f33156691d402884f1424a757cd786/`、`baselines-28dbb8f724894887ab02c027bcdfab11/` |

GPU 通过项不全等同 strict：完整版 15 项、无 Editor 7 项具有真实校验层启用输出；各自另一个 `VulkanRuntime` 通过但未输出该标记，单列为 Runtime 检查。两个失败批次保留 failed 状态。完整日志、初态/spacing 代码定位与最小待批准修复见 [细分测试阻碍](OCEAN_TESSELLATION_P0_BLOCKER.md)，尚未修改其实现。

最终生产二进制 SHA-256 保持：完整 `C42D75E6E948AC02528615D64E815F428185F5097D9C977E27B648EF7E09DCF4`，无 Editor `52C011849A34A803B6F8EF677D50EB9FD0650D659A87FCEEFF64B55D8514B7E4`。原 809 与上一 827 文件快照再次验证通过；本轮封存为 `20260828-p0-approved-visual-baselines`，仍只向新空目录恢复，不覆盖工作区或旧图。

实际进度 **14/117**：1.17/1.18/1.19 闭环，1.5–1.9 仍未完成，P1–P8 尚未开始。完整七视角/质量/尺寸、连续控制/历史/模拟次数、V3 性能和 Linux 验收均不由本轮固定帧结果替代。当前没有注册的 WSL 发行版，也未安装环境；新的 tessellation fixture 修复需明确纳入 P0 后继续。

### 已批准的细分 fixture 修复完成（1.6 / 1.20）

用户已明确批准上述独立修复。完整文件清单、后端状态契约、断言保持核对、构建身份及所有证据见 [细分测试修复记录](OCEAN_TESSELLATION_P0_BLOCKER.md)。只修改测试 C++ 的初态/呈现边界、独立 Domain shader 的 spacing 和 CTest 属性；370 个生产源文件均与上一恢复点相同，生产海洋 shader 和 Demo 设置不变。

- 两 Windows 构建成功；固定 CMake 4.1.2，最终 full build 均无待构建工作。两 fixture 头依赖各 36 个、SceneRenderer 87 个均 VALID，修复缓存引起的 fixture 依赖漏记，没有更改求解或渲染逻辑。
- CPU **11/11 + 10/10**；完整 GPU **18/18 + 9/9**，分别有 17、8 项图形测试确认真实校验启用，Runtime 各单列。最新 fixture 二进制的 CTest 定向 **2+1** 再次通过，验证请求来自 CTest 属性，不是仅靠外层环境。
- `P0/{d3d12,vulkan}/tessellation-fix-all-demos-native-01/` 各 **20/20** capture/strict；`P0/comparisons/tessellation-fix-all-demos-{d3d12,vulkan}-01/` 各 **20/20** 同后端原 V2 对照通过，参考为上一已验收修复版，不混称所有图均对原始二进制。
- HPWater 默认视角额外直接原始图对照：D3D12 SSIM=.999998、changed=.000020；Vulkan 逐像素一致。HPWater 既有 **330 帧** lifecycle 两 API strict/最终覆盖率恢复通过，证据 `P0/{d3d12,vulkan}/tessellation-fix-hpwater-lifecycle-01/`；没有将最终截图当作所有控制点的连续图像验收。
- 验证工具 **12+10** 自测通过。原始 809 文件快照完好；所有输出在 architecture-refactor，旧失败批次、批准参考和 `artifacts/hpwater-validation/` 未覆盖。

修复后恢复点：`20260828-p0-tessellation-fixture`，**833 文件**，manifest SHA-256 `FD893CD10EB5D4A488EAE85540DE333D89840FA3C9144F3A62557E6E4CD6B7CA`；创建及重复校验通过。快照保存已验证代码和证据说明，本节及最终勾选在封存后追加；生产/测试代码此后未改。恢复仍只向新的空目录复制，不原地覆盖工作区。

实际进度 **16/118**，本轮完成 1.6、1.20。下一步是 1.5 的捕获帧图/状态/模拟/历史诊断，以及 1.7–1.9 剩余质量/尺寸、连续帧与性能基线；不再次请求已批准的细分修复授权，不提前进入 P1。Linux 当前未提供可执行环境，其后续验收仍单列未完成。

### 捕获帧诊断与独立比较入口完成（1.5）

详见 [捕获帧诊断实现和验收](ARCHITECTURE_FRAME_DIAGNOSTICS.md)，包含完整文件清单、数据流、计数/状态含义、复现入口及精确二进制身份。当前 **17/118**，仅关闭 1.5，未进入 P1。

- 默认关闭的逐帧诊断将图像绑定实际 frame/view；保留原首帧 RDG 报告行为，记录 Pass/逻辑资源状态、共享模拟 Pass 次数、TAA/WaterOptics/volumetric 历史和 reset 请求。旧图/未知 Scene 源不冒充当前帧，比较时拒绝。
- 本轮初版诊断的两个后端全目录各 **20/20** capture/strict、原 V2 图像对照、结构自比较通过，跨 API 全目录语义检查 **20/20**；共 1200 个连续逻辑帧诊断。没有覆盖旧 golden 或放宽门限，60 个 assets 文件全与上一恢复点一致。
- 随后只修正诊断本身的重复完成通知、保存错误判断和无 Editor swapchain 接线。最终两构建成功，CPU **12/12 + 11/11**、GPU **18/18 + 9/9**；实际有 17+8 项图形测试启用校验，两个 Runtime 项单列。构建缓存的 10 个丢头依赖对象已重建，当前固定 CMake 4.1.2，依赖非零且 VALID。
- 最终边界集成 **4+2**：第 3 帧截图后继续到第 7 帧；预期保存失败不生成成功报告。HPWater 两 API 各 **330 帧**的实际 reset/resize/scene 诊断与既有覆盖率恢复断言通过，最终截图对上一 lifecycle 的 V2 比较通过。最终 Game/Scene 共六次捕获及其诊断通过，HPWater 默认视角还直接与原始图对照通过。
- 工具正反例 **13+11** 通过。实现期间的失败日志/负例均保留，并明确标记为本轮诊断/测试实现问题；全目录初版与最终修正后的构建证据不混用。

1.7–1.9 的完整质量/尺寸、连续控制点图像与 V3 性能基线仍未完成；连续 JSONL/最终截图不代替这些门禁。Linux 后续验收仍无可用环境；未修改生产海洋算法，也未启用新渲染/RHI 线程。

本轮恢复点 `20260828-p0-frame-diagnostics`：**843 文件**，manifest SHA-256 `B1EDC9C513B8782DF478789C0D317B5536E9A3403FD8392EFFADF042F762B826`；已创建并验证。任务 1.5 勾选包含在快照中，本条和专用诊断文档的哈希登记在封存后追加，代码未变。原 809 与上一 833 文件快照完好；恢复只复制到新的空目录，不原地覆盖。当前 **17/118**，所有本轮构建、GPU/CPU 测试进程均已结束。

### 同进程连续图像与水基线完成（1.7）

详见 [连续采样实现、配置、失败证据与验收](ARCHITECTURE_CAPTURE_SEQUENCES.md)。当前 **18/118**，本轮只关闭 1.7，不提前进入架构拆分或线程迁移。

- 新增应用层 FrameCaptureSequence、逐请求诊断输出、序列驱动/完整性检查与比较；旧单张入口和默认运行不变，不改海洋算法、shader 或 Demo 设置。
- 两 API 各三次 330 帧、每次 64 张控制点图像，共 384 张。全部两两同 API 图像/结构比较及 64 点跨 API 检查通过，包含水线、reset、resize、质量和三海洋场景切换；不会以最终一张代替时域证据。
- 七视角、三 optical quality×两尺寸各三次，选定 26 个配置 / 78 张图 / 78 次两两比较。`P0/water-baseline-set-01.json` 绑定所有子运行路径和 SHA，并重新核验同一源码/二进制、实际尺寸/质量；SHA 为 `8CDC99B1A32FF172AC15D4322514CF4621B4D3CEB24154558CA80C2B6EA423FC`。
- 初次矩阵保留一次 Vulkan High 大尺寸实际变为 2556×1406 的失败，不能当作 2560×1417 稳定性样本。只补验证工具的请求尺寸校验，Vulkan 质量矩阵用已有隐藏窗口开关重测全部通过，Normal/High 的隐藏/显示窗口图像与结构对照通过；未改生产窗口逻辑。失败 aggregate 未变成通过，错误尺寸样本明确排除，完整来源与窗口模式登记在新清单中。
- 两 API 的 26 个首样本额外对旧 HPWater 图片全部通过，13 个视角/质量配置的 parity 通过。旧批次 queue/cache 元数据不完整，这些补充图像检查不冒充完整等输入性能验证。
- 最终两 API 各 20/20 Demo 默认 capture/strict 和上一验收图 V2 通过：`P0/{api}/sequence-final-all-demos-native-01`、`P0/comparisons/sequence-final-all-demos-{api}-01`。60 个 assets 文件对上轮快照全部未变。
- 两构建通过，CPU 12/12+11/11、GPU 18/18+9/9（strict 图形测试 17+8，Runtime 各 1 单列）；正常多帧与提前退出负例覆盖 D3D12、Vulkan、无 Editor Vulkan，工具 13+11+10 组通过。

1.8 的 V3 性能、1.9 的全 Demo 连续交互/双视图/流送矩阵及后续 Linux 验收仍保持未完成；采样的 GPU 等待不作为性能证据，尚未实现新渲染/RHI 线程。

本轮恢复点 `20260828-p0-capture-sequences`：**853 文件**，manifest SHA-256 `3B1E86160072855D5938671248169436C39A02C88F5E94F00E897540D71851D6`；创建及校验通过，任务 1.7 勾选包含在快照中。本条及专用文档的恢复点哈希在封存后追加；生产/测试代码未变。上轮 843 文件快照完好，恢复仍只复制到新的空目录，不自动覆盖用户工作区。最终进度 **18/118**。

## P0 1.8 逐帧性能工具与未通过的性能门禁（2026-08-29）

实现/定义/文件清单及完整证据见 `docs/ARCHITECTURE_FRAME_PERFORMANCE.md`。新增 opt-in CPU extraction/UI/frame、进程内存、退役、设备 PSO 创建计数和逐 view/slot/generation 的 GPU 来源映射；不新增逐帧 GPU 等待，不修改 shader、模拟或调度算法。默认路径没有 recorder；新增报告与其他高开销捕获/trace 互斥。

- 两构建通过；CPU 13/13+12/12，GPU 18/18+9/9（实际 strict 图形测试 17+8，Runtime 各一项单列）。工具 13+11+10+31 项通过，真实进程的冲突/非确定性输入负例两后端各 4 项通过。
- 两 API 各 20/20 Demo 默认图像及 strict、对上一验收图的原 V2 比较全部通过：`P0/{api}/performance-final-all-demos-native-01/` 和 `P0/comparisons/performance-final-all-demos-{api}-01/`。
- HPWater 两 API 各 330 帧/64 张控制点图像，对上一序列的图像/结构比较全部通过：`P0/{api}/performance-water-controls-01/`、`P0/comparisons/performance-water-controls-{api}-01/`。60 个 assets 哈希不变；图像不是性能测量数据。
- `P0/performance-native-baseline-01/`：四负载×两 API×两批，每批 3 次独立预热和 5 次 180 帧独立测量，128 个采样进程全部有效。跨批仅 D3D12 shadows、Vulkan HPWater 通过，其余 6 项超限，aggregate 保持 `failed`。
- `P0/performance-preview-long-retest-01/`：D3D12 preview 增至 180/900/8 帧、每批 3+5 个进程的两批复测，16 次运行仍有效，但稳定性仍失败。CPU frame median 7.58165→15.2265 ms，主要差异在 BeginFrame（1.5327→9.8226 ms）；来源 slot/逻辑帧延迟核对未发现错配。没有证据把问题归于算法回归，呈现/提交节拍影响仍需细分诊断。
- 144 次运行及 1,296 个子产物封存复核通过。两个失败 aggregate 的 index SHA 分别为 `DCB83F0443BA60B11F0AFFBF006D411C6AA8ED09ACDEF7842D0C732E61743E5C`、`FCC6FDECFD154B9C1208E846748B5B8F6747AB75D8D9496801D7FDD5E3656854`；旧报告与 golden 不覆盖。

本轮交付是可用的采样/验证实现和明确的失败证据，不是 P0 通过。仍需固定并记录窗口/显示器/呈现条件、细分帧等待并收敛性能基线；1.8、1.9 保持未完成，**18/118**，P1/线程迁移未启动。Linux 后续验收仍未提供运行环境。

恢复点 `20260829-p0-performance-sampling`：**865 文件**，manifest SHA-256 `84E8B14A5FCF04A68090D4855AB1F1650D7ECEE4E7A69ABE7B1F562BE1420885`，已保存并校验。本条及专用文档的恢复点说明在封存后追加；生产/测试代码未变。旧快照和失败证据保留，恢复只复制到新的空目录，不自动覆盖工作区。

## P0 1.8 帧等待与呈现条件诊断（2026-08-29）

详见 [实现清单、测量定义与证据](ARCHITECTURE_FRAME_PACING.md)。新增 opt-in 后端细分 CPU 计时和原生状态、只读窗口/显示器诊断、性能专用坐标输入；未增加 GPU 等待，未改海洋算法、shader、普通 Demo 配置或呈现同步策略。

- 最终两构建通过；CPU 13/13+12/12，GPU 18/18+9/9（strict 图形 17+8，Runtime 各 1）；工具 13+11+10+52 与最终真实进程配置负例 7+7 通过。
- 双 API 各 20/20 Demo strict/原 V2 图像通过；HPWater 各 330 帧/64 个控制点图像与结构对上一轮全部通过；60 个 assets 哈希不变。最终源码身份和报告目录在专用文档逐项列出。
- 隐藏 D3D12 初试实际返回 DXGI_STATUS_OCCLUDED，被新校验拒绝；该发现不证明所有旧性能双峰的原因。窗口位置初试失败也保留；在首次事件处理后一次性定位，最终 (100,100) 短测与无 Editor Vulkan 路径通过。
- `P0/pacing-visible-preview-baseline-02/`：双 API preview、180/900/8 帧、正常可见呈现；第一批 16 次运行有效，第二批第一次预热有效，第二次在第 310 帧焦点 true→false，状态校验失败。18 个进程中 17 个有效，第二批正式测量未执行，不将第一批或失焦片段当作两批 V3 通过。
- 18 个子运行的 161 个产物、17 个成功子 index 和两份第一批汇总封存复核通过。aggregate SHA `590DCB2581AF4A0907E38232860926CC952B59EBFAA3FB43F55E3186676E097B`；失败子 index SHA `FAF1477CE3DAF2973476BD013ADDF9EA939EF51070036680BC2D1B2A8609D7DA`。

按 openspec-apply-change 暂停在同条件采样阻碍：需要焦点稳定的前台测试时段，或显式确定另一种窗口策略后重新建基线。不会自动抢焦点、关闭 VSync、过滤失败帧或改变门限。**18/118，1.8/1.9 未完成，未进入 P1 或线程迁移**；Linux 仍待环境。

恢复点 `20260829-p0-frame-pacing`：**870 文件**，manifest SHA-256 `FBB40823726D83A193B514666E5966A71065EB636D423D34D81F2AAD938FE952`，创建及重复校验通过；454 项生产输入与最后有效测试运行一致。本条及专用文档恢复点说明在封存后追加，生产/测试代码未变。旧恢复点和失败证据不覆盖，恢复只复制到新的空目录。

## P0 1.8 再测与整批离线审计（2026-08-29）

继续实施后新增 `scripts/Summarize-ArchitecturePerformanceBaseline.ps1`、`scripts/ArchitecturePerformanceReport.Common.ps1`、`tests/scripts/ArchitecturePerformanceReportTests.ps1`。完整代码、使用方法、数据流、证据及 SHA 见 [帧等待/呈现诊断记录](ARCHITECTURE_FRAME_PACING.md)。新工具 26/26、原性能工具 52/52；重算真实不完整批次和完整超限批次，Enforce 均正确拒绝。不会只信任原 summary 或把未登记的失败子进程漏掉。

- `P0/pacing-visible-preview-baseline-03/` 是全新的双 API preview 180/900/8 两批测量，窗口/同步配置与上轮相同。第一批 16 次及第二批 r1 有效；r2 第 682 帧失焦，其他显示/呈现条件保持，工具拒绝。18 个进程中 17 个有效、剩余 14 个未运行，只有第一批 9,000 个正式被测帧，不能声称两批稳定性通过。
- `P0/pacing-preview-audit-03/report.json` 自动核验 163 个产物及相关 index，确认唯一变化字段 focused；原批次 SHA `AE53B60D70B99B52AA7BB87563E6171518693433076A1D2883B08A978C316DF1`，审计报告 SHA `A7FE4C1A3269F527926064697A1898C6BD6A5E733C6C23C3BF744B1D19D6BF5B`。
- 旧中断批次 `pacing-preview-audit-02` 核验 163 个产物，复现第 310 帧失焦；旧完整长测 `pacing-legacy-complete-audit-01` 核验 146 个产物，复现原 CPU 7.58165→15.2265 ms 超限。旧证据未被覆盖，新旧观测协议未混用。
- 454 项生产输入及完整二进制哈希均未改变；没有改 HPWater、Demo、shader、同步或画质。沿用冻结版本此前功能/图像结果，不声称本轮重新跑了这些套件；OpenSpec strict 通过。

当前 **18/118**，按 openspec-apply-change 暂停在 P0 显示条件门禁，1.8/1.9 未完成，P1/线程迁移未启动。需要明确后续测试窗口策略；固定无焦点的测试专用配置尚未实施或默认批准，不自动抢焦点或降低门限。

恢复点 `20260829-p0-performance-audit`：**873 文件**，创建及重复校验通过；manifest SHA-256 `BF8BA129C6888AD4AADAE477D4339426889DDF67D082B17DD1A19F4236A7B96C`。本条及专用文档的恢复点说明在封存后追加；三个新离线文件与 tasks 已包含在快照，生产源码和资产未变。旧恢复点保留，恢复只复制到新的空目录。

## P0 已批准的无焦点性能窗口（2026-08-29）

用户已确认将测试专用无焦点模式纳入实施，上节待批准状态解除。新增 `PerformanceWindowFocus.h` 及 Window 构造策略、Host 接线、性能脚本/严格参数校验和测试；具体文件、完整数据流、默认兼容与运行方式见 [帧等待文档](ARCHITECTURE_FRAME_PACING.md)。仅显式性能采样且窗口可见时允许，创建/显示不主动聚焦，实际每个 measured 帧仍必须无焦点；不抢焦点、不隐藏窗口、不改同步/画质、不放宽门限。

已完成两构建与 CPU 13/13+12/12、GPU strict 18/18+9/9；新采样工具 64/64、审计 26/26、旧工具 13+11+10。两 API 各 20/20 Demo 普通窗口图像通过原 V2；HPWater 各 330 帧/64 图及结构对上一序列全部通过；两个 API 各 10 项真实进程负例通过，新模式两 API 短测及无 Editor Vulkan 单视图短测通过。报告均在 `20260828-hpwater-complete/P0/` 下 `unfocused-*` 独立目录，完整索引见专用文档。Renderer/RHI 实现与 assets 内容未变，主循环及海洋算法不重写。

`unfocused-preview-baseline-01` 将新配置独立分两批测量，不能使用之前的前台样本补齐。新模式功能已实现且回归通过不等于 V3/P0 整体通过；结果未验收前仍 **18/118**，1.8/1.9 不勾选，P1/线程迁移未启动。

两批最终完成：**32/32 合法运行、20 个正式进程、18,000 个正式被测帧**，全部维持实际无焦点和原生呈现条件。Vulkan preview 全部原 V3 双向门限通过；D3D12 的 CPU frame/loop、Game GPU 通过，Scene GPU median 1.13646→0.84365 ms、p95 1.87658→1.24525 ms，反向 +34.708%/+50.699% 超限。第一批和第二批内部都存在快慢两类运行，不能将第二批更快当作稳定性通过，也不能继续归因为失焦。

`P0/unfocused-preview-audit-01/report.json` 重算全部 JSONL/汇总/双向比较并核验 292 个产物及 32 个登记 index；`complete=true, passed=false`，Enforce 按预期拒绝，原始报告完整保留。aggregate SHA `E515238E1D70939D4172E3C2CB9BD1C9B0523788DD3FBA84F57F3CB41E489010`；audit SHA `0EC548C37B1EEDDC536A01603FA6FDBA067375ED21CDBBC353327889FB39A753`。455 项生产输入、60 项 assets 和两二进制均通过完成后哈希核验。

按 openspec-apply-change 暂停在 D3D12 Scene GPU 稳定性阻碍，**18/118**；后续建议补充快慢运行的 GPU 时钟/负载和双视图提交关联证据，不能预设原因、删样本、改系统电源或放宽门限。其余三个负载的新协议性能矩阵、1.9 全 Demo 连续配置与 Linux 验收仍待完成。无焦点模式的实现及功能回归已交付，尚未进行架构拆分或线程迁移。

恢复点 `20260829-p0-unfocused-window`：**874 文件**，创建及重复校验通过；manifest SHA-256 `E69BDB3727F705C987EE3EE458696C7C6375111D7FF69D8C9E7A9D89C9418EE6`。本轮另有 1,525 个功能/短测产物哈希复核一致。此条及专用文档恢复点说明在封存后追加，生产/测试代码未再改变；旧恢复点和失败报告保留，恢复仅复制到新的空目录。

## P0 D3D12 Scene GPU 频率关联诊断（2026-08-29）

用户确认继续后新增 NVIDIA 只读旁路诊断及离线汇总，未修改生产源码、资产、构建、二进制或 V3 算法。工具 8/8；最终版 2 次真实自检完整覆盖并有效。主诊断 8 次中有 5 次同时满足原性能子运行有效且遥测覆盖初始化至退出，另外 3 次因焦点或遥测提前结束排除且保留。五次进程级 Scene GPU/graphics clock Pearson 为 **-0.99053**；Game GPU 0.34474、CPU frame 0.10665。低时钟 1537/1665 MHz 对应 1.143/1.155 ms 慢类，高时钟 2370–2430 MHz 对应 0.840–0.879 ms 快类。

该结果支持 DVFS 相关解释但不是因果证明，也不是门禁豁免。报告、目录、逐项结果及三个 SHA 见 [帧等待/呈现诊断记录](ARCHITECTURE_FRAME_PACING.md)。驱动支持锁频/reset，但本轮没有修改用户 GPU 状态；也未筛样本、增大正式运行数或改变聚合。当前保持 **18/118**，1.8/1.9 未完成。需要用户选择临时锁频并可靠恢复（推荐）、明确修订更大样本协议，或明确接受 DVFS 例外；选择前不进入 P1/线程迁移。

恢复点 `20260829-p0-d3d12-dvfs-diagnostic`：**878 文件**，创建及重复校验通过；manifest SHA-256 `42CE58F51619097B889CC96F65C07D36546A4A4820FAF9F7BFE55663DD7B8B21`。本条及专用文档恢复点说明在封存后追加，生产/测试代码未再改变；旧恢复点和诊断失败目录均保留。

## P0 FPS 口径与锁频权限核验（2026-08-29）

新增 view-throughput 离线报告和 opt-in NVIDIA 锁频包装器，仅修改 scripts/tests/docs/tasks。真实 `loopIntervalMs` 证明 Vulkan preview 完整 Editor `game,scene` 为中位 **166.872 FPS**，无 Editor `game` 为 **363.458 FPS**（2.178×）；完整数据、Unity/UE 官方口径和计时边界见 [帧等待文档](ARCHITECTURE_FRAME_PACING.md)。当前 P0 完整 Editor 样本确实包含实际 Scene Render；`-View game` 只选捕获，不关闭 Scene。报告 SHA `3FBBB8D573C3FE4A9DFD0692CF547D208F992B915D50800AD9D75106EC3BE021`。

获准的 2400 MHz 锁频请求被 NVIDIA 驱动以权限不足（exit 4）拒绝，原 Benchmark 未启动；三次 reset 同样无权限。锁定从未成功，只读状态仍显示动态频率/无 applications-clock limiter，因此没有证据表明 GPU 状态被改变。失败 index SHA `003CF438ECE9BA56C999BE2C86C90BE5FBA0A102F1BC65E6CFF5337FB7AD86CA`，不冒充锁频 V3。工具 2+8+64 通过；view-throughput 对真实封存样本端到端通过。**18/118**，1.8/1.9 保持未完成，需管理员条件下运行包装器或另行批准测量协议修订，不进入 P1。

恢复点 `20260829-p0-fps-scope-clock-permission`：**882 文件**，创建及重复校验通过；manifest SHA-256 `739DA9364EBEFEA987CB9B3EB7487AA2B69A576D31FAAC2115F670C97F2085A7`。恢复只复制到新的空目录，不覆盖工作区；本条在封存后追加。

## P0 同一 Editor 二进制 Active View 归因（2026-08-29）

新增只在显式性能采样下可用的 `game` active-view 策略和封存汇总，不改变普通 Demo/HPWater、Feature、shader、同步或画质。Vulkan preview 同一完整 Editor EXE 的 A/B 为：默认 Game+Scene loop median **5.7432 ms / 174.119 FPS**，Editor Game-only **3.25615 ms / 307.111 FPS**；Scene Render 省 **1.81085 ms**、BeginFrame 省 **0.45615 ms**，Game Render 仅 1.9142→1.8770 ms。默认延迟管线的各具体 GPU Pass 中位均不超过 0.06 ms，双视图前后基本不变；现有 100 多 FPS 的主要已证实成本是第二个 Scene view 和伴随的帧/队列压力，不是单一 DeferredLighting 技术。

完整实现、边界和失败样本见 [帧等待/窗口/呈现诊断](ARCHITECTURE_FRAME_PACING.md)。报告 SHA `02C21A5E0E7A9062D95FA484CA10D85F4F9A9A83301D3F907BE81E0D4D179756`。构建、C++ 测试、性能工具 65/65、Vulkan 真实负例 13/13 通过；普通路径双 API 各 20/20 Demo strict/冻结 V2 图像通过，包含 HPWater，比较 index SHA 为 `923F616653C2F32E3691884FC968D2EF35B69E7D81E43E4C2E38477A624A371B`、`0A919EA7960D39BA75383107EECBE472C04240C19BE297832501A9F636CD3E53`。D3D12 性能 A/B 因实际 `DXGI_STATUS_OCCLUDED` 被拒绝并保留，不冒充交叉 API 结论。1.8 的两批稳定性门禁仍未通过，保持 **18/118**，不进入 P1。

完整 Editor/无 Editor Vulkan 两构建及 CPU **13/13 + 12/12**、OpenSpec strict 通过。恢复点 `20260829-p0-active-view-cost`：**884 文件**，manifest SHA-256 `DF79D214951A8E2AFE5343E9C38B88446911EA9B8D6C59DBE79D34B1FE21F73B`，创建及重复校验通过；本条在封存后追加。

## P0 全 Demo 连续序列（2026-08-29）

任务 1.9 已完成。新增严格 20 key 目录、version 2 有序动作、实际状态诊断和矩阵 runner；详细文件、数据流、控制语义、失败分析及复现命令见 [全 Demo 连续序列门禁](ARCHITECTURE_DEMO_SEQUENCES.md)。所有能力默认关闭，普通 Demo、HPWater 算法/shader/资产、队列、同步和默认 Game/Scene 行为不变。

最终 Vulkan 与 D3D12 各 **20 scenes / 40 independent runs / 20 same-backend comparisons** 全部通过。Vulkan index SHA `32B041233F53C70636F75A284AC9317742CC9C41DD4DA4D6A806109A42D4F98C`；D3D12 index SHA `D5AFD5C091F3F083A36DEA11B203E0420A912AF4C0DAF2A943F966C6FC6C9EFE`；目录 SHA `ADBB9B14320231C1F121E398F0C46E68034D56E1D97EDF4E3E998288030DE51B`。三个 Ocean、四个 Fluid、TAA/阴影、流送激活及按需双视图均有连续采样与显式控制/结构断言。HPWater 两后端 330 帧 lifecycle 重复通过；使用 301 帧观察 debug sweep 退出后的最终画面，不用通用对比度门禁误判第 300 帧合法 debug buffer。

完整/无 Editor 构建、两个构建的 FrameDiagnostics 测试、脚本 12 组正反例和 dry-run 通过。完整 CTest 首跑 29/31；PBF Vulkan/Ocean tessellation Vulkan 两项因缺 `VK_LAYER_PATH` 返回 `VkResult=-6`，在项目本地 layer path 且保持 GPU validation 时复测 **2/2** 通过。失败批次未覆盖。当前 **19/118**；1.8 的两批 V3 稳定性仍是独立阻碍，P0 未完成，不进入 P1 或渲染/RHI 线程迁移。

恢复点 `20260829-p0-demo-sequences`：**888 文件**，创建及重复校验通过；manifest SHA-256 `57CE3BE2BD69A1830F25B80CADBB42694B102D7E7E36B811C2FB04CC96B9E441`。本段恢复点说明在封存后追加，旧恢复点与失败证据保留。

## P0 实时 Profiler 默认行为与全目录回归（2026-08-29）

任务 1.25、1.28 已完成。Scene View 的 session-local `catalog-default/live/on-interaction/30hz/paused` 控制、刷新原因和实际 rendered frame 已进入完成帧 Profiler；显式 Scene capture 可越过隐藏/暂停策略刷新，Game 与共享模拟不因 Scene 限频停止。策略与 Panel 的 C++ 正反例、off/basic/detailed 行为检查及普通用户权限双后端 strict/Release/Standalone 冒烟均已通过。

最终默认 `catalog-default + basic` 回归使用一致重建后的 `build-windows-ci/RelWithDebInfo/PrismRender.exe`：

| 后端 | 当前矩阵 | 重复比较 | 对冻结矩阵 | 结果 |
| --- | --- | --- | --- | --- |
| D3D12 | `p0-default-basic-demo-sequences-d3d12-20260829-03` | 40/40 runs，20/20 | `p0-default-basic-vs-frozen-d3d12-20260829-02`，20/20 | 通过 |
| Vulkan | `p0-default-basic-demo-sequences-vulkan-20260829-03` | 40/40 runs，20/20 | `p0-default-basic-vs-frozen-vulkan-20260829-02`，20/20 | 通过 |

覆盖三个 Ocean、四个 Fluid、TAA/阴影开关、流送激活和双视图；HPWater 两后端各有两次 330 帧 lifecycle，画面、Pass/资源、共享模拟、历史、reset/resize/scene switch 与冻结任务 1.9 一致。额外的单项确认位于 `p0-default-basic-hpwater-{d3d12,vulkan}-coherent-build-20260829` 及对应 `vs-frozen` 目录。

初次候选批次发现 HPWater 首帧后缺 `/views/scene`，但 Game 图像一致。探针确认当前源码的生命周期驱动每帧都设置 `requiresRefresh=1`；差异来自矩阵所用 `build-windows-ci` 中过期的 `WaterValidationSequence.cpp.obj`，而另一完整重建目录行为正确。强制该翻译单元重编译后，两后端单项和完整矩阵均恢复。失败目录原样保留，未以 Panel 顺序、Scene 限频结果或放宽门限放行。为防止后续恢复快照导致同类假回归，架构阶段的每个正式矩阵必须在源码修改后重建实际记录在 index 中的二进制，并核对其 SHA；不能用另一 build tree 的成功替代。

本轮没有修改 HPWater/Fluid 渲染算法、shader、资产或 Demo 默认值；`WaterValidationSequence.cpp` 只为既有 Scene 刷新赋值补充契约注释，Profiler Panel 保持在 Editor DockSpace 建立后提交。任务 1.8 的三轮 Release 公平场景矩阵、同进程等级 A/B、内存/PSO sidecar 和聚合报告现已完成，详见 [逐帧性能文档](ARCHITECTURE_FRAME_PERFORMANCE.md)。当前 OpenSpec 进度 **28/128**；只剩 1.29 的 P0 恢复点，尚未进入 P1。

## P0 最终检查点（2026-08-29）

P0 tasks 1.1–1.29 全部完成，当前 **29/128**。实时 Profiler、session-local Scene 策略、六场景公平矩阵、三轮独立 Release 采样、同进程 240 帧等级 A/B、内存/PSO sidecar，以及 D3D12/Vulkan 各 20 Demo/HPWater 默认 `catalog-default + basic` 冻结比较均有独立证据。当前 Preview Release 双视图约 260.75 FPS，Editor Game-only 约 414.63 FPS，Standalone 约 556.14 FPS；主要差异在第二个 Scene View、Editor/Main 工作和等待，不是默认延迟管线的单一 GPU Pass。GPU DVFS、窗口和显示状态保留在报告中，但不再要求管理员锁频或机器级 Vulkan 注册。

最终门禁：完整 Editor Release 的 FrameProfiler/SceneView/Panel/两项脚本测试 **5/5**，无 Editor Release **4/4**；OpenSpec strict 通过。正式矩阵所用二进制均在源码修改后从对应 build tree 重建，避免快照恢复时间戳造成过期对象文件假回归。

恢复点 `20260829-p0-profiler-fair-matrix`：**913 文件**，创建及二次校验通过；manifest SHA-256 `B1025EE93B99E17BDABD0F378323A91B119B4789559976F32EB7ABA58C07EA3F`。恢复只复制到新的空目录，不 reset/覆盖用户工作区；build/cache/capture、Git 元数据和旧 HPWater/失败证据不在恢复复制范围。P1 尚未修改 ApplicationHost 所有权，也尚未增加 worker/render/RHI 线程。

## P1 应用组合顺序契约（2.1，2026-08-29）

新增 `src/Core/Application/ApplicationComposition.{h,cpp}` 与 `tests/ApplicationCompositionTests.cpp`。`IApplicationCompositionActions` 只允许执行命名的粗粒度动作和读取关闭条件，不持有 Host、Renderer、RHI 或 UI；`ExecuteApplicationComposition` 固定当前初始化、每帧和退出顺序，后续 2.2–2.8 将各 action 逐步替换为拥有自身状态的 coordinator，而不是再给 coordinator 一个完整 Host 引用。

数据流为：`ApplicationCompositionOptions`（Editor/Scene View/最大帧数）→ 顺序执行 `IApplicationCompositionActions` → `ApplicationCompositionResult`（完成帧数/退出原因）。动作只描述所有权边界，不传递渲染资源，不添加 GPU Pass，也不改变 HPWater、Fluid、Demo、shader、设置、同步或线程模型。

固定的四条轨迹如下：

| 路径 | 关键断言 |
| --- | --- |
| 正常 Editor 双视图 | 初始化 Editor；每帧 Game 后 Scene；完成反馈后才结束；退出包含 Run 尾部 drain 与显式 Shutdown |
| 无 Editor | 不出现 Editor 初始化、Editor 事务、Scene View 渲染或 Editor 释放，其他帧/退出顺序保持 |
| 最大帧数提前退出 | 目标帧的 EndFrame、完成反馈和 FrameComplete 全部发生后才退出，退出原因是 maximum-frame |
| 内容加载阶段失败 | 2.1 时仅记录 InitializeFailed 并重新抛出、不进入显式 Shutdown；该历史风险已由 2.8 的逐阶段 RAII 清理修复 |

修改的构建接线为 `cmake/PrismApplicationSources.cmake` 与根 `CMakeLists.txt`；新测试目标 `PrismApplicationCompositionTests` 同时进入完整 Editor 和无 Editor 配置。验证结果：两种配置的 `PrismRender` 与全部测试目标构建成功；专项 ApplicationComposition 各 **1/1**；完整 Editor 排除一个 PowerShell 运行时选择问题后的 C++/模块集合 **18/18**，无 Editor 全集合 **18/18**。被排除的 `ArchitecturePerformanceScenarioSummary` 在项目绑定 PowerShell 7 下单独通过；完整 Editor CMake cache 仍指向 Windows PowerShell 5，后者缺少双参数 `Path.GetFullPath`，此环境兼容问题未归因到 2.1。下一项是 2.2 `AssetRuntimeCoordinator`；当前进度 **30/128**。

## P1 资产运行时所有权迁移（2.2，2026-08-29）

新增 `src/Core/Application/AssetRuntimeCoordinator.{h,cpp}` 与 `tests/AssetRuntimeCoordinatorTests.cpp`。`ApplicationHost` 已移除 `AssetImportService`、`AssetRegistry`、`AssetStreamingManager` 和 manifest path 四份直接状态，只持有一个 `AssetRuntimeCoordinator`；Coordinator 独占三个服务及 project/manifest identity。环境 cubemap 仍按当前 Renderer 初始化路径由 Host 持有，不在本任务提前迁移。

文件变更：

- 新增 `src/Core/Application/AssetRuntimeCoordinator.h/.cpp`：导入初始化、Import/Reimport 后 GPU 安全等待和 runtime reload、streaming request/release、IO 等待、render-preparation 上传、驻留淘汰、统计/报告及幂等 Shutdown。
- 修改 `src/Core/ApplicationHost.h/.cpp`、`src/Core/ApplicationHostScene.cpp`：所有 Registry/Import/Streaming 调用改经 Coordinator；供 `AssetStreamingSceneBridge` 使用的 manager 借用是标注生命周期的 2.3 过渡接口，不转移所有权。
- 修改 `cmake/PrismApplicationSources.cmake`、`CMakeLists.txt`：接入生产源和双配置测试目标 `PrismAssetRuntimeCoordinatorTests`。

数据流保持为：资产 IO worker 读取/解码到 `AssetStreamingManager` 私有 completion queue → `ApplicationHost` 在原 `BeginFrame` 成功之后调用 `ProcessStreamingAtRenderPreparation` → Coordinator 在该调用线程更新 Registry/GPU binding → streaming Scene 激活与 deferred capture → budget eviction → 发布 `RenderSceneMailbox`。Worker 没有 World、RenderScene、mailbox 或帧快照引用；本任务没有建立渲染线程，也没有移动 GPU 上传安全点。

专项测试使用自包含的 5 个 Cooked Texture + 1 个 Cooked Material 和假设备：取消请求后等待 IO 并调用 render-preparation，上传数仍为 0；重新请求后 `WaitForStreamingIo` 得到 `completedIoCount=6`、`completedUploadCount=0` 且 Registry 无 material binding；连续两次 render-preparation 调用分别上传 5/1 个资源，最终 `completedUploadCount=6`。`AssetStreamingManager::Cancel` 只在 reference/pin 同时归零时递增尚未提交工作的 generation，使迟到 completion 失效；Uploading/Resident 沿用原 GPU 退役语义。同一测试还验证立即销毁有排队工作的 worker join、重复 Shutdown，以及外部 PNG 导入、runtime reload 和既有 GPU wait 回调。

验证结果：

- 完整 Editor/无 Editor 两种 `PrismRender` 构建通过；非 GPU CTest 分别 **19/19**、**18/18**，专项 Coordinator 两配置均通过。
- 完整 GPU 集 **18/18**，无 Editor Vulkan GPU 集 **9/9**。完整集首轮仅 PBF Vulkan 与 OceanTessellation Vulkan 因未设置本地 `VK_LAYER_PATH` 返回 `VkResult=-6`；使用项目 validation layer 路径保持 strict 后复测 **2/2** 通过，失败批次保留且不归因到代码。
- 最终二进制的实际 Asset Streaming 连续序列 `p1-asset-runtime-streaming-{d3d12,vulkan}-03` 均为 `captured-and-validated`，每后端 `completedUploadCount=7`、`failedCount=0`、Scene activation success 且 1 个对象；与 P0 `streaming-r1` 的 1/29/30/31/60/90/120 帧图像和结构逐项通过。第一次候选漏传第 30 帧 activate-streaming action，因输入不一致被比较器拒绝并保留；后续按原输入通过。
- D3D12/Vulkan 的 streaming 与 HPWater 固定帧 strict capture 成功，直接同后端 P0 图像门限均通过。没有修改 HPWater/Ocean/Fluid 算法、shader、Demo 设置、资源状态或队列策略。

2.2 完成，当前进度 **31/128**。下一项是 2.3 `SceneSession`；2.8 的部分初始化清理与整体销毁顺序仍未提前改变。

## P1 场景会话所有权迁移（2.3，2026-08-29）

新增 `src/Scene/SceneSession.{h,cpp}` 和 `SceneSessionActions`。`SceneSession` 现在独占 editable `RenderScene`、`RenderSceneMailbox`、`CommandProcessor/World`、Scene/Game 两套 `CameraController`、场景身份、dirty/camera-sync 合并状态及命令 request counter；它只借用 `AssetRegistry`，不拥有 graphics device，也不包含或反向引用 `ApplicationHost`、Renderer、UI 或 ImGui。`ApplicationHost` 仅保留渲染热路径所需的非拥有指针别名，生命周期由 Session 明确覆盖。

文件变更：

- 新增 `src/Scene/SceneSession.h/.cpp`：集中处理 RenderScene→World 初始化、World→RenderScene 同步、相机事务、命令 request ID、Demo Populate/导航、场景身份与 pending dirty 合并。
- 新增 `tests/SceneSessionTests.cpp`：覆盖 RenderScene/World bridge、事务、undo/redo、journal 确定性回放、World 存取档、相机回写、dirty flush 和场景身份。
- 修改 `src/Core/ApplicationHost.h/.cpp`、`src/Core/ApplicationHostScene.cpp`：移除上述对象的直接所有权和重复身份/dirty/request 状态；Editor document action 通过 `SceneSessionActions` 存取档，Demo/streaming 激活和 World 初始化通过 Session。
- 修改 `cmake/PrismRendererSources.cmake`、`CMakeLists.txt`：接入 SceneSession 生产源和双配置 `PrismSceneSessionTests`。

数据流保持为：编辑器/自动化命令 → `SceneSessionActions` → Session 内的 `CommandProcessor/World` → 帧内 pending dirty flush → editable `RenderScene`；资产流式 GPU 上传仍在原 `BeginFrame` 后、mailbox publish 前完成。Demo 切换仍先在 Host 等待 GPU，随后 Session 同步 Populate、导航和 World 导入，最后 Host 只应用 Renderer preset/scene-changed 通知。没有增加线程、Pass、shader、设置或上传位置。

验证结果：

- 完整 Editor 与无 Editor clean build 均通过；`SceneSession` 两配置通过，既有 `EngineHarness`、`WorldRenderSceneBridge`、`ApplicationComposition`、`AssetRuntimeCoordinator` 专项合计 **5/5**。
- 完整 GPU 集 **18/18**，无 Editor Vulkan GPU 集 **9/9**。非 GPU 全集除 CTest 仍调用 Windows PowerShell 5 的已知双参数 `Path.GetFullPath` 运行时问题外全部通过；同一 `ArchitecturePerformanceScenarioSummaryTests.ps1` 在 PowerShell 7 下直接通过。
- D3D12 与 Vulkan 各完成 330 帧 HPWater 生命周期运行；64 个固定采样覆盖相机/质量/历史重置/resize，以及 192/204/216 帧 WaveWorks→Ocean→HPWater 场景切换。`p1-scene-session-water-controls-d3d12-02` 和 `p1-scene-session-water-controls-vulkan-01` 均为 `captured-and-validated`，与 P0 `sequence-water-controls-03` 的同后端图像和结构逐帧通过。
- D3D12 首次生命周期候选因同一秒内 Host 布局变更后残留旧增量对象而在退出时触发 ABI 混编堆异常；完整 clean rebuild 后 100 帧最小复现正常退出，原代码不需要绕过销毁。第一次比较因 clean build 未包含 `PrismGoldenImageCompare` 工具而被拒绝；补建工具后原候选直接通过。两批失败证据均保留且未误标为通过。

2.3 完成，当前进度 **32/128**。下一项是 2.4 `RenderFrameCoordinator`；2.8 的部分初始化清理与整体销毁顺序仍未提前改变。

## P1 渲染帧所有权迁移（2.4，2026-08-29）

新增 `src/Renderer/RenderFrameCoordinator.{h,cpp}`。Coordinator 现在独占 Game `SceneRenderer`、可选的 Editor Scene `SceneRenderer` 和设备生命周期 `SceneRendererSharedResources`；`ApplicationHost` 移除了这些对象的直接所有权，只保留两个标注为非拥有的 renderer 热路径别名。Coordinator 负责构造、共享资源初始化、诊断/profiling 广播、scene-changed 通知、两视图 swapchain 资源重建和幂等销毁。当前 Render 调用及帧输入仍留在原单线程主循环，没有提前建立 render/RHI thread，也没有改变 native/serial/auto queue 决策。

文件变更：

- 新增 `src/Renderer/RenderFrameCoordinator.h/.cpp`：声明 Game→Scene 的有序 `RenderFrameViewPlan`，固定 Game 为共享模拟 producer、Scene 为 consumer，并集中 renderer/shared-resources 的 RAII 所有权。
- 新增 `tests/RenderFrameCoordinatorTests.cpp`：覆盖 Standalone Game-only、Editor Scene 隐藏/按需、Game→Scene 顺序、唯一 producer、设置广播和重复 Shutdown。
- 修改 `src/Core/ApplicationHost.h/.cpp`、`src/Core/ApplicationHostScene.cpp`：通过 Coordinator 初始化与重建资源，场景刷新计划由 Coordinator 返回；原渲染热路径不复制帧输入。
- 修改 `cmake/PrismRendererSources.cmake`、根 `CMakeLists.txt`：接入生产源及双配置测试目标；Windows/Posix 的进程诊断源和 `PrismDiagnostics` 链接按平台选择，未扩大跨平台依赖。

验证结果：

- 完整 Editor 与无 Editor 两套完整构建通过；`RenderFrameCoordinator`、`SceneViewRefreshPolicy`、`ApplicationComposition` 在两配置均 **3/3**。无 Editor 非 GPU 全集 **21/21**；完整 Editor 为 **21/22**，唯一失败仍是 CTest cache 选择 Windows PowerShell 5 后不支持双参数 `Path.GetFullPath`，同一脚本在 PowerShell 7 直接通过。
- 完整 GPU 首轮 16/18；PBF Vulkan 与 OceanTessellation Vulkan 仅因测试进程未继承项目 `VK_LAYER_PATH` 以 `VkResult=-6` 初始化失败，在项目 validation layer 路径下保持 strict 后复测 **2/2**，合计 **18/18**。无 Editor Vulkan GPU 在同一 layer 环境下 **9/9**。
- D3D12 `p1-render-frame-coordinator-water-controls-d3d12-01` 和完整 Editor Vulkan `p1-render-frame-coordinator-water-controls-vulkan-02` 均完成 330 帧、64 个控制点的 HPWater lifecycle；对应 comparison **64/64** 图像和结构逐帧通过 2.3 基线，覆盖 reset、168/180 resize 与 192/204/216 scene switch。每个完整 Editor capture 都含 `game,scene`，共享模拟 Pass 次数逐项与参考相同。
- 无 Editor Vulkan `p1-render-frame-coordinator-water-controls-vulkan-01` 本身 `captured-and-validated`，64 个 capture 均只有 `game` 且输出 `swapchain`。最初将它误与 Editor 双视图参考比较时，比较器在全部帧准确报告缺少 Scene View 和不同 output target；该失败目录保留为产品配置不兼容的负例，未作为回归或通过证据。

本任务没有修改 HPWater/Ocean/Fluid 算法、shader、资产、Demo 默认设置、模拟步数、Present 顺序或 queue mode。2.4 完成，当前进度 **33/128**。下一项是 2.5 `EditorCoordinator`；2.8 的部分初始化故障清理和总体销毁顺序仍未提前改变。

## P1 编辑器会话所有权迁移（2.5，2026-08-29）

新增 `src/UI/EditorCoordinator.{h,cpp}`，成为 `ImGuiSystem`、`EditorLayer`、`DebugPanel` 和 `PerformanceProfilerPanel` 的唯一 owner。它在 UI 模块内完成双 viewport 纹理注册、BeginFrame/Draw/Render/Shutdown、Debug/viewport 性能统计组装、SceneSession 文档与命令动作适配、内容浏览器资产回调接线、Demo 请求返回以及 Game/Scene 相机输入选择。`ApplicationHost` 不再包含上述四个 UI owner，只构造逐帧输入并消费 `EditorCoordinatorFrameResult`；Coordinator 头/实现中没有 Host 指针、friend 或私有状态访问。

文件变更：

- 新增 `src/UI/EditorCoordinator.h/.cpp` 与 `tests/EditorCoordinatorTests.cpp`。
- 修改 `src/Core/ApplicationHost.h/.cpp`：用一个条件编译的 Coordinator owner 代替四个 UI owner；resize/out-of-date 后纹理刷新、Scene 可见性/gizmo/shading 查询和 UI draw 统一经 Coordinator。
- 修改 `cmake/PrismEditorSources.cmake` 与根 `CMakeLists.txt`：Coordinator 只进入 `PrismEditor`；专项测试也只在 Editor 配置生成，测试所需 profiler/diagnostics 实现显式链接。

数据边界为：Host 构造本帧 backend/窗口/streaming 基础值及 import/instantiate 窄回调 → Coordinator 从 `SceneSession`、`AssetRegistry`、两个 renderer 和已完成 `FrameProfiler` 快照组装面板输入 → `EditorLayer` 同步提交动作 → Session dirty 时 Coordinator 同步 World→RenderScene 并返回 Scene refresh → Host 处理可选 Demo 请求。没有保存 Host、跨帧 callback 或可变统计引用；文件对话框仍由 ContentBrowser/FileDialog 的 Editor-only 链接边界拥有。

验证结果：

- 专项测试通过：五种 Game/Scene/gizmo 输入路由；文件对话框符号边界；SceneSession save/load；事务 create/edit、undo、redo；world-dirty 回调；Vulkan UI session 构造和重复 Shutdown。完整 330 帧应用运行同时执行 DockSpace、EditorLayer、Debug/Profiler panels 和 ImGui backend 的逐帧路径。
- 完整 Editor 与无 Editor 完整构建通过。完整 Editor 非 GPU 为 **22/23**，唯一失败是已登记的 CTest Windows PowerShell 5 `Path.GetFullPath(path, basePath)` 兼容问题；同脚本 PowerShell 7 直跑通过。无 Editor 为 **21/21**，其 build graph 搜索不到 `PrismEditor`、`EditorCoordinator` 或 ImGui 源/target 引用。
- D3D12 `p1-editor-coordinator-water-controls-d3d12-01`、完整 Editor Vulkan `p1-editor-coordinator-water-controls-vulkan-01`、无 Editor Vulkan `p1-editor-coordinator-water-controls-vulkan-no-editor-01` 均为 330 帧/64 点 `captured-and-validated`；分别与 2.4 相同配置比较 **64/64** 图像和结构通过。两次 resize、reset、三海洋 scene switch、Game→Scene 双视图和 Game-only swapchain 路径保持。

本任务没有修改 HPWater/Ocean/Fluid 算法、shader、资产、Demo 默认值、Scene refresh policy、Present 或 queue mode。2.5 完成，当前进度 **34/128**。下一项是 2.6 `CaptureAutomationController`；Water validation 的动作实现仍留待 2.7 独立迁移。

## P1 自动化捕获所有权迁移（2.6，2026-08-29）

新增 `src/Core/Application/CaptureAutomationController.{h,cpp}`。Controller 现在独占 `FrameDiagnostics`、`FrameCaptureSequence`、普通延迟截图、流送场景 deferred capture、流送激活结果、RDG/GPU timing/asset streaming 报告路径和一次性写出标志、`PRISM_RENDER_MAX_FRAMES` 以及 capture/report/maximum-frame 退出条件。`ApplicationHost` 已移除这些状态及 `RequestTextureCapture`、`TryRequestDelayedCapture`、`TryRequestDeferredStreamingCapture` 三个辅助方法，只在原阶段向 Controller 提供窄引用；Controller 不保存 Host 指针或跨帧 renderer 引用。

文件变更：

- 新增 `src/Core/Application/CaptureAutomationController.h/.cpp`：解析并校验现有自动化环境契约，协调 capture sequence、readback resolve、报告写出、流送激活结果和退出判定。
- 修改 `src/Core/ApplicationHost.h/.cpp`：用一个 Controller owner 替代分散的 capture/report/exit 状态；诊断记录、sequence action、报告和退出检查仍停留在原主循环位置。
- 修改 `src/Core/ApplicationHostScene.cpp`：流送场景激活的 ID、成功/失败结果和 delayed capture 就绪通知改经 Controller，实际资产激活及 SceneSession 导入顺序不变。
- 修改 `cmake/PrismApplicationSources.cmake`：将 Controller 接入完整 Editor 与无 Editor 的应用源集合。

数据流保持为：启动环境/CLI → Controller 建立可选 diagnostics/sequence 与报告状态 → 原 BeginFrame 后的资产上传及流送激活 → Controller 在原帧点触发 delayed/deferred capture → Game/按需 Scene 渲染 → 原 EndFrame 后 resolve diagnostics/sequence → 报告/错误/退出判断。GPU 上传、图录制、readback、Present、streaming activation 和 `WaitForGpu` 均没有换线程或移动安全点；`WaterValidationSequence` 的相机、质量和场景动作仍由 Host 执行，留待 2.7。

验证结果：

- 两种生产配置完整构建通过。CLI、FrameDiagnostics、ApplicationComposition、AssetRuntimeCoordinator、SceneSession、RenderFrameCoordinator、EditorCoordinator 和 FrameProfiler report 契约测试通过。完整 Editor 非 GPU 为 **22/23**，唯一失败仍是 CTest 选择 Windows PowerShell 5 后缺少双参数 `Path.GetFullPath`；相同脚本由 PowerShell 7 直接执行通过。无 Editor 为 **21/21**。
- 完整 GPU 初跑 **16/18**，PBF Vulkan 与 OceanTessellation Vulkan 只因测试进程缺少项目 validation-layer 查找路径而在 instance 创建前返回 `VkResult=-6`；显式使用项目内 `VK_LAYER_PATH` 后复测 **2/2**，总契约 **18/18**。无 Editor Vulkan 在相同严格环境中 **9/9**。没有写 HKLM 或修改机器级 Vulkan 配置。
- 固定帧正例：`p1-capture-automation-water-controls-{d3d12,vulkan}-01` 与 `p1-capture-automation-water-controls-vulkan-no-editor-01` 均完成 330 帧、64 个 HPWater lifecycle 控制点；相对 2.5 同配置的图像/结构比较各 **64/64** 通过，覆盖 resize、reset、三海洋 scene switch、双视图和 Game-only swapchain。
- `max frames`/退出码反例：`P1/capture-automation/p1-capture-controller-early-exit-01` 在 D3D12 与 Vulkan 均将序列上限强制为第 4 帧，进程按预期非零退出，错误包含未完成 sequence；帧诊断恰有 4 行，只存在 frame 3/4 的两个成功图像和 capture report，没有伪造 frame 5。
- 流送就绪：`p1-capture-automation-streaming-d3d12-02` 与 `p1-capture-automation-streaming-vulkan-01` 均按第 30 帧 activate action 完成 1/29/30/31/60/90/120 的 **7/7** 图像与结构对照。两后端 `PrismAssetStreamingSceneReport` 均保持 version 1、激活成功和 20 个 render objects；D3D12 的两个流送报告相对 2.2 逐字节相同，Vulkan Scene report 逐字节相同，Asset report 只存在 staging allocator 容量/页数/high-water 运行统计差异，字段、resident 资产、上传计数和失败计数不变。
- 报告字段：`p1-capture-automation-reports-{d3d12,vulkan}-01` 实际生成截图、frame/capture diagnostics、RDG、GPU timing、CPU trace 和 identity。`PrismRenderGraphReport` v11 与 `PrismGpuTimingReport` v2 相对 P0 的递归字段集合均为零差异；`PrismCaptureDiagnostics` v1 保留全部 P0 字段，并保留 P1 前序工作已加入的 view/source settings 字段。

本任务没有修改 HPWater/Ocean/Fluid 算法、shader、资产、Demo 默认值、Scene refresh policy、Present、线程或 queue mode。2.6 完成，当前进度 **35/128**。下一项是 2.7 `WaterValidationSequence`；2.8 的故障清理与总体 RAII 销毁顺序仍未提前改变。

## P1 水验证编排迁移（2.7，2026-08-29）

原 `src/Core/WaterValidationSequence.cpp` 只是文件层面的拆分，仍然定义 `ApplicationHost::UpdateWaterValidationSequence`，直接访问 Host 的 window、Game renderer、editable scene 和 Scene View refresh 状态。本任务删除该旧文件、成员声明与定义，新增真正独立的 `src/Core/Application/WaterValidationSequence.{h,cpp}`；实例由 `CaptureAutomationController` 独占并在初始化时缓存 `PRISM_RENDER_WATER_VALIDATION_SEQUENCE`。普通交互运行只检查缓存布尔值，不再每帧读取环境变量。

独立 Sequence 每帧只借用当前 `SceneRenderer`、editable `RenderScene`、`Platform::Window` 和一个同步 Demo 激活回调；它不保存 Host、backend、renderer、scene 或跨帧 callback。返回值只表示本帧应强制刷新独立 Scene View，Host 不再含水验证成员方法或水验证状态。动作仍发生在原 `FrameTimer::Tick` 之前：三 Ocean scene switch → 普通 Ocean 不得调度 WaterOptics 的兼容检查 → quality/reset/resize/render toggle → camera/waterline → debug views → coverage 恢复断言 → progress 输出 → Scene refresh。

验证结果：

- 完整 Editor 与 Vulkan/no-editor 生产构建通过；FrameDiagnostics、ApplicationCommandLine、ApplicationComposition、AssetRuntimeCoordinator、SceneSession、RenderFrameCoordinator 和可用 EditorCoordinator 契约分别 **7/7**、**6/6** 通过。检索确认旧 `src/Core/WaterValidationSequence.cpp` 不存在，`ApplicationHost::UpdateWaterValidationSequence` 的声明、定义和调用为零。
- `p1-water-validation-sequence-d3d12-01`、`p1-water-validation-sequence-vulkan-01` 和 `p1-water-validation-sequence-vulkan-no-editor-01` 均完成 330 帧、64 个 lifecycle 控制点并为 `captured-and-validated`；相对 2.6 对应配置的同后端图像和结构比较各 **64/64** 通过。
- 三个运行各输出 29 条 `WaterValidation frame` progress 事件，事件文本和顺序与 2.6 逐条相同。实际 trace 继续覆盖 history/local/full reset、96–120 水下和水线相机、Normal/High/Extreme、chromatic/disabled caustics、168/180 resize、192/204/216 WaveWorks→Ocean→HPWater、228/240 render toggle、272–300 debug sweep 和 320 coverage recovery；完整 Editor 保持 Game→Scene，no-editor 保持 Game-only swapchain。

本任务只迁移所有权和编排位置，没有修改 HPWater/Ocean/Fluid 算法、shader、设置数值、动作帧号、资产、Demo 默认值、Present、线程或 queue mode。2.7 完成，当前进度 **36/128**。下一项是 2.8：将 Host 收敛为组合根和主循环，并修正部分初始化失败与重复 Shutdown 的 RAII 轨迹。

## P1 Host 生命周期与 RAII 收敛（2.8，2026-08-30）

`ApplicationHost` 现在以 `Uninitialized → Initializing → Initialized → ShuttingDown → Uninitialized` 显式状态管理生命周期，取代只有完整初始化末尾才置位的 `m_isInitialized`。`Initialize` 用异常边界包围所有创建阶段：任何异常都会同步进入幂等 `Shutdown` 后重抛；析构再次调用时状态已经是 Uninitialized，不重复访问或释放。额外的 `m_backendInitialized` 只在 backend 完整初始化返回后置位，避免 services/Backend 内部部分失败时调用不可用 FrameContext；`m_gpuDrainedForShutdown` 区分真正需要等待的在途工作。

销毁顺序保持现有依赖：必要时 GPU wait → Editor/ImGui → capture automation → SceneSession 及非拥有 alias → environment/asset runtime（先停止 streaming worker/import/registry）→ RenderFrameCoordinator 的 Scene/Game/shared GPU 资源 → backend/window → profiler/refresh/timer。全部非拥有 alias 在 owner reset 前清空；部分加载遗留的 environment cubemap 也显式释放；可重新初始化的 session-local 标量恢复默认。`Shutdown` 为 noexcept，GPU wait 失败进入进程诊断但仍继续 CPU owner 收拢。

正常 `Run` 的尾部等待成功后设置 drained；析构不再重复 WaitForGpu。为保持流送安全，最后一次 `ProcessStreamingAtRenderPreparation` 如果确实提交新上传，会清除 drained 标志，使 Shutdown 执行一次必要等待；没有新上传则仍不等待。进程日志新增 `renderer.shutdown.begin` 的 `details.waitForGpu` 和唯一 `renderer.shutdown.completed`，可直接区分必要等待与重复等待。

验证结果：

- `ApplicationCompositionTests` 现在覆盖 services 创建失败、backend 初始化失败、content、renderer、Editor 初始化失败以及 Run 中间失败；只释放成功创建的阶段，backend 成功且未 drain 的失败路径恰好等待一次，正常/最大帧退出的 Drain 后 Shutdown 不再包含第二个 Wait。完整与 no-editor 两配置各 **1/1**。
- 新增 `tests/scripts/ApplicationLifecycleIntegrationTests.ps1`，实际启动生产 EXE。`p1-host-lifecycle-d3d12-02` 与 `p1-host-lifecycle-vulkan-no-editor-02` 的 normal + services/backend/content/renderers/editor 各 **6/6**：normal 都只出现一次 Shutdown 且 `waitForGpu=false`；services 为 false；backend 之后四阶段为 true；所有故障以预期非零退出并恰有一条 begin/completed，随后析构没有第二条。首次 Vulkan 脚本在 services 失败前错误要求 validation-enabled 文本，修正为仅 backend 已创建路径检查，失败目录 `...-01` 保留。
- 完整 Editor 与 no-editor 构建通过；非 GPU 分别 **22/23**、**21/21**。唯一失败仍为完整 build 的 CTest 调用 Windows PowerShell 5 后缺少双参数 `Path.GetFullPath`，同一脚本 PowerShell 7 直跑通过。
- `p1-host-lifecycle-water-controls-{d3d12,vulkan}-01` 与 `p1-host-lifecycle-water-controls-vulkan-no-editor-01` 均为 330 帧/64 点 `captured-and-validated`，相对 2.7 同配置图像和结构各 **64/64**。`p1-host-lifecycle-streaming-d3d12-01` 相对 2.6 streaming 为 **7/7**，报告保持且尾部没有新上传时 shutdown 记录 `waitForGpu=false`。

`src/main.cpp` 保持原最小入口，ApplicationHost 仍只负责组合和主循环；没有增加线程、改变 Present/queue mode、移动正常帧上传安全点或修改 HPWater/Ocean/Fluid 算法、shader、资产和 Demo 默认值。2.8 完成，当前进度 **37/128**。下一项是 2.9 P1 全量集成验收、V3 性能门限和服务级恢复点。

## P1 服务边界集成验收与恢复点（2.9，2026-08-30）

P1 最终验收使用 2.8 后同一源码重新确认完整 Editor 与 no-editor 构建均无待编译工作。完整非 GPU CTest 为 **22/23**；唯一失败仍是 CTest 选择 Windows PowerShell 5 后不支持双参数 `Path.GetFullPath`，同一 `ArchitecturePerformanceScenarioSummaryTests.ps1` 由 PowerShell 7 直接运行通过。no-editor 非 GPU 为 **21/21**。显式设置项目 Vulkan validation layer 后，完整 GPU 为 **18/18**、no-editor Vulkan GPU 为 **9/9**；没有新增 validation error、device removal 或被跳过的数值断言。

全部 20 个 `DemoSceneCatalog` key 在完整 Editor、1280×800、默认设置、native queue、固定第 30 帧下分别完成 D3D12 与 Vulkan capture/strict 冒烟，结果为 **20/20 + 20/20**。证据索引为 `20260828-hpwater-complete/P1/{d3d12,vulkan}/p1-service-boundaries-all-demos-*-01/index.json`；每项包含截图、RenderGraph、GPU timing、CPU trace、性能身份和日志。测试没有关闭 TAA、阴影、水光学或 Fluid/Ocean 模拟来取得通过。

HPWater 以 native queue 连续运行 330 帧，覆盖相机/水线、resize、reset、三质量、caustics/debug view 及 WaveWorks→Ocean→HPWater 场景切换的 64 个控制点。`p1-service-boundaries-water-controls-{d3d12,vulkan}-01` 均为 `captured-and-validated`，相对 2.8 的同后端生命周期参考，图像和结构各 **64/64** 通过；比较索引位于 `20260829-p0-profiler-fair-matrix/P1/comparisons/`。

V3 使用 Vulkan/preview/native/Game capture、1280×800、RelWithDebInfo、60 帧预热、180 个有效帧，执行每批 3 次预热与 5 次独立测量、共两批。首次 `p1-service-boundaries-vulkan-preview-native-01` 的 CPU frame、Editor Loop、Game GPU 均通过，但 Scene GPU 中位批间 +8.34% 超过 5%，按规范保留失败并复测。完全相同的 `...-02` 复测为 `baseline-repeatability-passed`：CPU frame 中位 -0.05%/p95 -1.92%，Editor Loop +0.62%/-0.48%，Game GPU +1.26%/+4.27%，Scene GPU +0.29%/-4.52%，全部低于 5%/10% 门限。早期 P0 formal 样本的测量 driver/helper hash 与 pacing schema 不同，因此不把跨协议数值冒充严格 P0→P1 对照；P0 最终公平场景特征仍作为范围/瓶颈背景保留。

Host 审计确认业务状态随 owner 迁移：SceneSession 独占 CommandProcessor/World/可编辑场景/相机，AssetRuntimeCoordinator 独占导入/Registry/Streaming 状态，RenderFrameCoordinator 独占双 Renderer/shared resources，EditorCoordinator 独占 ImGui/面板/统计拼装，CaptureAutomationController 独占 capture/WaterValidation 状态。Host 仅保留组合 owner、生命周期标量、主循环阶段和有 owner 生命周期保证的非拥有热路径别名；没有 ImGui 调用、UI 统计算法、业务状态机 owner、Host 反向指针或 friend。`src/main.cpp` 未改。

服务级恢复点为 `20260830-p1-service-boundaries`，由 `Backup-ArchitectureBaseline.ps1` 创建并用 manifest seal 逐文件校验；恢复时从其 `snapshot/` 复制所需项目输入，先核对 `snapshot-manifest.json` 与 `snapshot-manifest.sha256`，不得覆盖原 HPWater/P0 证据。2.9 完成，当前进度 **38/128**；下一项进入 P2 的 3.1，在拆分前固化 RenderGraph compiler/hazard CPU 基线。

## P2 RenderGraph 拆分前编译契约（3.1，2026-08-30）

新增独立的 `tests/RenderGraphCompilerTests.cpp` 与 `PrismRenderGraphCompilerTests` CTest 目标。fixture 只使用 mock texture/command context，固定五类编译语义：从显式输出反向计算 liveness 并裁剪不可达 Pass；保留生产者到消费者的约简执行依赖；跟踪资源版本、graph generation 和双 history import；计算无重叠 transient lifetime 并建立 alias 依赖；为 native graphics/compute handoff生成批次依赖、prologue 与逐子资源恢复。

跨队列回归特意覆盖两个历史危险点：首个 compute batch 必须把 imported 和复用 transient 从 ShaderResource 转为 Common，并在 graphics 消费前恢复；单独写 mip 2 时只能为 mip 2 生成 ShaderResource→UnorderedAccess→ShaderResource 两次屏障，不能错误迁移整个纹理。output culling fixture 同时证明被裁剪的独立分支不会破坏仍活跃资源的生产者依赖，alias fixture 证明后续复用仍携带原资源释放/排序依赖。

完整 Editor 配置 `build-windows-ci` 与 no-editor Vulkan 配置 `build-windows-vulkan-ci` 均重新生成并构建新增目标；两配置中的既有 `RenderGraph` 与新增 `RenderGraphCompiler` 测试分别 **2/2** 通过。此任务没有修改 `src/Renderer/RenderGraph.*`、公共 API、HPWater/Ocean/Fluid、shader、Demo 默认值、线程或 queue mode。3.1 完成，当前进度 **39/128**；下一项是 3.2 的内部数据边界。

## P2 RenderGraph 声明、计划与执行边界（3.2，2026-08-30）

新增 `src/Renderer/Graph/GraphDescription.h`、`CompiledGraph.h` 和 `GraphExecutionState.h`。`GraphDescription` 拥有导入/输出、transient 与注册句柄、Pass 声明、回调、Blackboard 及影响编译的策略；`CompiledGraph` 只保存 fingerprint、无回调 Pass 计划、资源寿命/别名结果、queue sync/batch、编译摘要和 signature；`GraphExecutionState` 拥有实际纹理逐子资源状态、buffer range 状态、逐次 Pass 计时、执行 generation 及完成帧诊断。RenderGraph facade 的原 `QueueClass`、`PassOptions`、诊断结构和所有公开方法继续以原名称可用，仓库调用方不需要改变建图语义。

编译 cache 现在直接以 `CompiledGraph` 为值，不保存 `std::function` 或活 RHI 指针。执行使用 RAII scope：开始时清除上一帧 Pass timing 并复制纯编译摘要作为基准；仅成功返回时发布该次执行诊断，异常路径清除部分 timing；结束后恢复编译计划摘要，因此上一次的 queue execution/alias barrier 可变计数不会写回可缓存 plan。RenderGraph 明确禁止复制和移动，防止迁移期 facade 引用被错误重绑；这些引用只用于在 3.3–3.8 逐算法迁移期间保持既有实现稳定。

新增边界测试在销毁持有捕获对象的 `GraphDescription` 后检查 weak lifetime 已释放而 `CompiledGraph` 仍有效，并验证 `GraphExecutionState` 的 Begin/End 会清除旧 timing、保存完成诊断且把 active summary 恢复为 plan 基准。完整 Editor 和 no-editor Vulkan 两配置的生产 EXE、全部测试/工具 target 均编译链接；两配置的 `RenderGraph` + `RenderGraphCompiler` 各 **2/2**。完整非 GPU 唯一失败仍为 Windows PowerShell 5 双参数 `Path.GetFullPath`，pwsh 7 直跑通过；no-editor 非 GPU **22/22**。没有修改 HPWater/Ocean/Fluid、shader、Demo 默认设置、Pass/queue 算法、线程或 Present。3.2 完成，当前进度 **40/128**；下一项是 3.3 `GraphCompiler`。

## P2 独立 GraphCompiler（3.3，2026-08-30）

新增 `src/Renderer/Graph/GraphCompiler.{h,cpp}`。独立 compiler 直接消费 `GraphDescription`，负责 declaration index 有效性检查、liveness dependency、output/side-effect roots 裁剪、稳定 execution index、RAW/WAR/WAW、同队列顺序和跨队列 accessor 依赖，以及传递约简；最终向 `CompiledGraph` 写入不含回调的 Pass plan。`RenderGraph::Compile()` 只保留调用顺序和后续尚未迁移 planner 的编排，旧 `BuildLivenessDependencies`、`CullPasses`、`BuildExecutionDependencies`、`ReduceTransitiveDependencies` 成员声明/定义已删除且检索为零。

直接 CPU fixture 使用 Producer、DeadBranch、Transform、Consumer 四个 Pass 和 Output root：保留 3 个 Pass，DeadBranch 继续报告 `not_reachable_from_output`，Consumer 的 A/B 读与 Output 写保持完整，直接依赖在传递约简后仍为原 `{2}`；把原始索引改成越界值会在编译规划前失败。Facade 的既有测试继续覆盖输出裁剪诊断、版本/history、alias dependency、native handoff 和逐 mip 恢复。完整 Editor/no-editor Vulkan 的全部生产、测试和工具目标均编译链接，两配置的 `RenderGraph` + `RenderGraphCompiler` 各 **2/2**。本项未移动或改变 resource lifetime、alias、queue scheduling、barrier、executor、HPWater/Ocean/Fluid、shader、Demo 默认值、线程或 Present。3.3 完成，当前进度 **41/128**；下一项是 3.4 `ResourceLifetimePlanner`。

## P2 独立 ResourceLifetimePlanner（3.4，2026-08-30）

新增 `src/Renderer/Graph/ResourceLifetimePlanner.{h,cpp}`。Planner 只接收 const `GraphDescription`、const `GraphExecutionState` 和输出 `CompiledGraph`：声明提供导入、transient、注册 version/history 与 Pass 访问；执行状态只提供当前绑定资源初态和可选原生 transient allocation；输出记录逻辑资源类型、活动性、首末 execution index、估算字节与原生池元数据。它不访问 RenderGraph facade、回调、Blackboard、alias slot、queue schedule 或 barrier state。原 `RenderGraph::BuildResourceLifetimes` 声明/定义和仅由它使用的字节估算 helper 已删除。

直接 fixture 固定 imported Input 为 0..0、4 mip transient Temp 为 0..1、Output 为 1..1、History 为 imported/history/version 2，并验证被裁剪的 NoConsumer 资源保持 inactive 且 first/last invalid；Pass 的 mip 2 写范围在规划前后不变。既有 facade fixture 继续覆盖真实 handle generation、history、子资源状态、transient alias 与 native allocation 诊断。完整 Editor/no-editor Vulkan 全部生产、测试和工具目标编译链接，两配置 `RenderGraph` + `RenderGraphCompiler` 各 **2/2**。没有改变资源寿命算法、HPWater/Ocean/Fluid、shader、Demo 默认值、queue mode、线程或 Present。3.4 完成，当前进度 **42/128**；下一项是 3.5 `TransientAliasPlanner`。

## P2 独立 TransientAliasPlanner（3.5，2026-08-30）

新增 `src/Renderer/Graph/TransientAliasPlanner.{h,cpp}`。Planner 依次完成逻辑 alias slot 规划、当前执行资源的 native pool 复用统计与寿命合法性检查、alias 复用导致的 Pass 依赖注入；输入收窄为 `GraphDescription`、`GraphExecutionState` 和 `CompiledGraph`。原 `RenderGraph::BuildTransientAliasingPlan`、`BuildNativeTransientAliasingPlan`、`BuildAliasingDependencies` 及 facade 内专用兼容性 helper 已删除，`RenderGraph::Compile()` 仅在资源寿命之后、传递约简之前调用新 Planner，公共建图/执行 API 和阶段顺序不变。

直接 CPU fixture 覆盖四种逻辑资源：兼容且不重叠的 AliasA/AliasB 复用同一 slot；相同描述但寿命重叠的 Overlap 和宽度不同的 Incompatible 均不得复用该 slot。AliasA 最后在 compute 使用、AliasB 首次在 graphics 使用，Planner 注入跨队列依赖；传递约简后 AliasB 的直接依赖由 `{0,1}` 正确收敛为 `{1}`，没有丢掉 alias 所需的前一资源最后使用 Pass。另一 fixture 用两项 mock native allocation 固定同 pool/id 的逻辑 65536 B、物理 32768 B、单 allocation 和 2 个预期 alias barrier；既有 facade 测试继续实际执行 texture/buffer alias barrier 并核对计数。

`build-architecture-windows-ci` 与 `build-architecture-vulkan-ci` 的全部目标均编译链接；两配置的 `RenderGraph` + `RenderGraphCompiler` 各 **2/2**，完整非 GPU 回归分别 **24/24** 与 **22/22**。本项没有改 queue cost model、批次、barrier/handoff、executor、HPWater/Ocean/Fluid、shader、Demo、线程或 Present。3.5 完成，当前进度 **43/128**；下一项是 3.6 `QueueScheduler`。

## P2 独立 QueueScheduler（3.6，2026-08-30）

新增 `src/Renderer/Graph/QueueScheduler.{h,cpp}`。Scheduler 只消费 `GraphDescription` 中已完成裁剪、alias 依赖注入和传递约简的 Pass DAG，向 `CompiledGraph` 写入跨队列同步资源、queue batches、Pass→batch 映射、稳定拓扑提交顺序、跨队列 batch 依赖数和静态重叠机会；原 facade 的 `BuildQueueSchedule`/`BuildQueueBatches` 声明和实现已删除。`RenderGraph::Compile()` 在传递约简后调用 Scheduler，serial/native/automatic 的运行时选择、RHI capability 条件和 QueueSchedulingCostModel 本身未移动或改写。

直接 fixture 固定 Root 同时产生 `GraphicsInput`/`ComputeInput`，两个分支并行后由 Join 汇合：得到 4 个 batch，Pass→batch 为 `{0,1,3,2}`，稳定提交顺序为 `{0,3,1,2}`，跨队列同步资源精确为 `ComputeInput` 与 `ComputeResult`，Join batch 依赖 graphics branch 和 compute branch，静态 overlap 至少为 1。fixture 还确认 automatic 模式及已注入的 profitable native decision 不被编译规划器修改。既有 facade fixture 继续实际验证 serial、显式 native、安全 automatic、盈利 automatic、独立 batch fence/wait、graphics resume，以及 cost model 持久 cache、历史过期和 DAG pass-history 预测。

`build-architecture-windows-ci` 与 `build-architecture-vulkan-ci` 全部目标均编译链接；两配置的 `RenderGraph` + `RenderGraphCompiler` 各 **2/2**，非 GPU 回归分别 **24/24** 与 **22/22**。本项没有改变 queue cost model、模式选择阈值、RHI fence/submit、barrier/handoff、executor、HPWater/Ocean/Fluid、shader、Demo、线程或 Present。3.6 完成，当前进度 **44/128**；下一项是 3.7 `ResourceStateTracker`。

## P2 独立 ResourceStateTracker（3.7，2026-08-30）

新增 `src/Renderer/Graph/ResourceStateTracker.{h,cpp}`，集中负责同一 Pass 冲突状态检查、逐 mip/layer texture 状态、buffer range 分裂/合并、UAV barrier、native alias barrier、资源转 Common、顺序 queue handoff 和 queue-batch tail handoff。原 facade 的七个 barrier/handoff 成员声明与实现及专用访问 helper 已删除。GraphExecutor 尚未迁移，但其调用位置和顺序保持：native alias 先重置复用资源，随后根据本次访问生成状态 barrier；batch body 完成后再根据完整连续访问关系准备 queue release。传递约简后的 fence DAG 不用于替代完整资源访问关系。

直接 CPU fixture 先确认全纹理已处于 ShaderResource 时不生成非法同态 transition；随后只写 mip 2/layer 1，依次验证 ShaderResource→UAV、UAV→UAV 同态写屏障和 UAV→ShaderResource 恢复，其他 7 个子资源保持不变。graphics→compute handoff 把完整已初始化纹理转为 Common；两项同 native pool/allocation 的资源复用时，alias barrier 的 before/after 身份、目标状态表清空和执行计数均正确。既有 facade fixture 继续覆盖 buffer range、后端不支持 range 时的 whole-buffer fallback、首个 compute 使用的 graphics prologue、跨帧导入状态恢复、upload/readback 固定状态和 batch queue handoff。

`build-architecture-windows-ci` 与 `build-architecture-vulkan-ci` 全部目标均编译链接；两配置 `RenderGraph` + `RenderGraphCompiler` 各 **2/2**，非 GPU 回归分别 **24/24** 与 **22/22**。本项没有改变 RHI barrier 规则、queue schedule/executor、HPWater/Ocean/Fluid、shader、Demo、线程或 Present。3.7 完成，当前进度 **45/128**；下一项是 3.8 `GraphExecutor`。

## P2 独立 GraphExecutor（3.8，2026-08-30）

新增 `src/Renderer/Graph/GraphExecutor.{h,cpp}`。Executor 接收当前 `GraphDescription`、每次执行独有的 `GraphExecutionState`、无回调 `CompiledGraph`、profiling 开关，以及可选 command context/marker；负责无 context 回调、serial/native switching、independent queue batches、deferred/native parallel recording、fence wait/signal、graphics continuation 和执行诊断。`RenderGraph::Execute` 两个公共重载保持签名，只调用 `Compile()` 后转交 executor。原 facade 的 ExecuteQueueBatches、ValidateReads、RecordPassResult、两套执行循环和执行 RAII scope 已删除。

失败语义由直接 fixture 固定：同步第二 Pass 抛异常时，第一 Pass 的部分 timing 不发布、`executing` 复位、完成摘要保持无效，替换失败回调后下一次执行可完整发布。另一个 native DAG 使用 graphics producer、parallel-recordable compute、graphics consumer；compute 的 deferred recording future 抛异常后，executor 收拢任务并在任何 `BeginQueueBatch` 前传播失败，queue batch 提交记录为空，未出现部分失败帧。既有 facade fixture 继续覆盖 serial/native/automatic 选择、queue switching、batch wait/signal/graphics resume、native/deferred parallel recording、首 compute graphics prologue、alias/state barrier 和末尾 compute 资源恢复。

`build-architecture-windows-ci` 与 `build-architecture-vulkan-ci` 的全部目标均编译链接；两配置 `RenderGraph` + `RenderGraphCompiler` 各 **2/2**，新增执行失败 fixture 两配置均通过；生产迁移后的非 GPU 回归分别 **24/24** 与 **22/22**。没有修改 Vulkan/D3D12 context、RHI submit/Present、HPWater/Ocean/Fluid、shader、Demo、线程池或 queue 算法；逐 Pass `std::async` 仅被原样迁移，P6 才替换。3.8 完成，当前进度 **46/128**；下一项是 3.9 cache/diagnostics 接回及 facade 临时状态清理。

## P2 编译缓存与诊断接回（3.9，2026-08-30）

RenderGraph facade 已移除全部 compiled/execution 引用别名。缓存恢复、存储、资源/Pass/queue 诊断、计时和 queue profile 现在直接访问 `CompiledGraph` 与 `GraphExecutionState` owner；声明期别名暂留给公共建图 facade。`Reset()` 统一调用 `CompiledGraph::Clear()` 与 `GraphExecutionState::InvalidateCompiledPlan()`，因此 compiled passes、declaration fingerprint、资源计划、queue 计划、签名和上次完成摘要同步失效，不再存在 reset 后短暂返回旧诊断的窗口。

缓存仍是无回调、无 live RHI 指针的 `CompiledGraph` 副本。新增 fixture 先以第一个纹理和捕获 lambda 编译执行，再 Reset 后用相同描述/初态的新纹理和新 lambda 重建：第二次命中 cache，但 `ResolveTexture`、实际 texture barrier、最终资源状态和 v11 RenderGraph 报告均来自当前纹理；旧 lambda 计数不再增加。初始状态改变或 texture description 改变分别强制 cache miss。既有 culling cache fixture 改为实际执行 cache hit，并确认旧图 lambda 未被计划保留。Reset 后立即读取 summary、pass timing、signature 和 resources 均为空/无效。

完整 Editor 与 no-editor Vulkan 两配置的全部目标均编译链接；两配置 RenderGraph 与 RenderGraphCompiler 专项均通过，非 GPU 回归分别 **24/24**、**22/22**。没有修改 queue scheduler/cost model、barrier/handoff、Vulkan/D3D12 context、RHI submit/Present、线程、HPWater/Ocean/Fluid、shader 或 Demo。3.9 完成，当前进度 **47/128**；下一项是 3.10 P2 图等价和双后端集成验收。

## P2 精简集成验收（3.10，2026-08-30）

完整/no-editor GPU fixtures 在显式 validation 下为 **18/18 + 9/9**。HPWater D3D12/Vulkan native 330 帧 lifecycle 均完成，并相对既有同后端最终图像通过；serial/auto 在两后端各完成 60 帧冒烟，RDG 报告记录正确 requested mode。D3D12 保留既有 clear-value warning 820，未出现资源状态错误、device removal 或异常退出；Vulkan Khronos validation 无错误。

在用户收紧验证预算前，D3D12/Vulkan 各 20 个 Demo 已完成候选捕获和同后端比较，均 **20/20**；这些结果保留，但后续不再重复全矩阵，`pbf`、`fluid-render`、`fluid-caustics`、`fluid-toon` 四个粒子流体 Demo 明确排除。Vulkan Preview 完成一个 V3 批次的 3 次预热 + 5 次正式测量；相对 P1 单批 CPU frame/loop 与 Game GPU 有约 6% 观察信号、Scene GPU 通过。用户要求停止过度验证后第二批已中止，因此这里只记诊断，不称正式双批通过，也不继续消耗运行预算。

阶段恢复点 `20260830-p2-rendergraph-boundaries` 已封存并验证 948 个输入文件。后续普通任务不再重复全源码哈希，只在阶段封存时生成一次。RenderGraph 公共 API、HPWater/Ocean/Fluid、shader、Demo、queue 算法及 RHI 提交语义保持不变。3.10 完成，当前进度 **48/128**，进入 P3。

## P3 Feature 生命周期描述（4.1，2026-08-30）

新增 `Renderer/Features/RenderFeature.h` 与 `RenderFeatureContext.h`：稳定字符串 ID、设备共享/视图局部 scope、阶段、依赖、必要操作、初始化前置条件和操作专用 context 均由普通描述结构表达，现有 Feature 只需绑定回调，不继承统一大基类。现有 `RenderFeatureRegistry` 增加生命周期注册重载和只读查询，legacy 注册、Apply 与 Blackboard 发布路径未改变。

新增 `PrismRenderFeatureLifecycleTests`，直接验证 ID、两种 scope、必要回调、可省略的 prepare/resize/scene 回调、依赖必要性及初始化前置条件。只构建并运行该测试目标，结果通过；未启动任何 Demo、GPU/性能矩阵或整仓哈希。4.1 完成，当前进度 **49/128**，下一项是 4.2 注册校验、依赖排序与失败清理。

## P3 Feature 注册解析与失败清理（4.2，2026-08-30）

`RenderFeatureRegistry` 现在于初始化前解析依赖图：重复 ID、缺失必要依赖、依赖环及遗漏的必要操作均抛出包含 Feature ID/原因的错误；可选缺失依赖不阻塞。所有可运行节点按最早参与阶段、再按原注册顺序稳定排序，显式依赖保持优先。

初始化回调失败时，Registry 先调用失败实例的清理，再逆序释放已初始化依赖并保留原始异常；正常 Shutdown 会继续收拢全部实例、传播首个清理异常，且活动列表预先移出使重复调用幂等。扩展后的单元测试覆盖四类无效注册、确定排序、失败实例/依赖释放顺序和正常逆序幂等释放；只构建运行该测试，结果无警告通过，未启动 Demo 或额外矩阵。4.2 完成，当前进度 **50/128**，下一项是 4.3 类型化图数据约束。

## P3 类型化 Feature 图数据约束（4.3，2026-08-30）

现有 `RenderGraphBlackboard` 保留 legacy `Set/Get`，并增加以 slot 类型和 value 类型寻址的 Feature 发布路径。发布元数据记录 producer、shared/view scope、ViewId、graph generation 和 value version；冲突生产者不能覆盖，更新必须由同一 producer 使用 `PublishNext` 显式推进版本。必要读取会拒绝缺失、错误 value 类型、过期 generation/version 和 view-local 跨视图访问；可选读取只在 slot 缺失时使用调用方明确提供的后备值，已有但无效的发布不会被静默掩盖。`Clear()` 同时清除两类数据，仍保持每图生命周期。

新增 `FeatureGraphTypes.h` 仅定义 Opaque/Lit/Composite 三类通用场景 slot/数据结构，Feature 私有 scratch 不进入全集；Blackboard 发布不替代既有 RDG Read/Write/MarkOutput。新增 `PrismFeatureGraphContractTests` 覆盖发布、冲突、类型、代次、版本、视图和后备路径；该测试与代表性 `PrismRenderGraphTests` 均通过，未启动场景或额外验证矩阵。4.3 完成，当前进度 **51/128**，下一项是 4.4 内置 Feature 组装与低耦合试点。

## P3 内置 Feature 组装与首个试点（4.4，2026-08-30）

新增 `BuiltInRenderFeatures.{h,cpp}`，具体内置 Feature 的组装知识集中在这一层；通用 Registry/阶段调度不依赖 `SkyAtmosphere`。首个试点保留稳定 ID/诊断名 `SkyAtmosphere`，声明 view-local scope 和 `FramePreparation` 阶段。SceneRenderer 为 Game/Scene 提供临时稳定 ViewId 1/2，并只调用 Registry 的通用阶段入口。

现阶段 adapter 将 Sky 的 graph operation 转接到既有 SharedRenderGraphResources/Callbacks，未改变它的资源 owner、Update、EndFrame、shader 或 AddPasses；adapter 明确留待 4.17 删除。直接生命周期测试确认错误阶段不执行、正确阶段只执行一次且发布原字段；既有 RenderGraph fixture 继续确认关闭时 17 Pass、开启时 19 Pass，`AtmosphereTransmittance` 与 `AtmosphereSkyView` 仍位于最前且两个 LUT 输出 version 为 1。只构建运行生命周期与 RenderGraph 两目标，结果通过；未启动 Demo/GPU/哈希矩阵。4.4 完成，当前进度 **52/128**，下一项是 4.5 WaterOptics 图契约。

## P3 WaterOptics 图契约与生命周期阶段（4.5，2026-08-30）

`WaterOpticsGraphInputs`、`WaterOpticsGraphResult` 和 `WaterOpticsGraphCallback` 已从共享前端移至 `Renderer/Features/Ocean/WaterOpticsGraph.h`。该文件只暴露跨 Feature 的权威输入输出；WaterOptics 的临时纹理、历史和资源退役状态仍由具体 Feature 私有持有。结果契约不返回 opaque depth，明确阻止专用 composite depth 覆盖不透明深度。

组装层以稳定 ID `WaterOptics` 注册 view-local `WaterOptics` 阶段，SceneRenderer 的通用上下文提供 view 尺寸、帧索引和 scene；旧的每帧 WaterOptics 回调拼装块已删除。启停仍由原 ScenePipelinePlan 和共享前端控制，因此关闭 HPWater 时即使 Feature 已注册也不会执行回调或创建专用光学 Pass。已有 RenderGraph fixture 验证启用路径的 DepthCopy/Visibility/Refraction/Composite/Publish 顺序、opaque/composite depth 隔离、HDR 和 motion 版本，以及关闭路径零 WaterOptics Pass；生命周期与 RenderGraph 两个目标构建并通过。未改海洋算法、shader 或 Demo，未执行 GPU、场景、性能和哈希矩阵。4.5 完成，当前进度 **53/128**，下一项是 4.6 每视图事件与资源退役。

## P3 WaterOptics 每视图事件与资源退役（4.6，2026-08-30）

Registry 现在统一分发 PrepareFrame、Resize 和 SceneChanged；WaterOptics 声明 view-local 的 prepare/build/resize/scene/shutdown 操作。SceneRenderer 在每帧安全点传递自己的 ViewId、extent、frame slot、逻辑帧和 scene。配置判断继续使用 `ClassifyWaterOpticsDirtyScopes` 的字段比较，不引入全对象或全仓哈希；camera cut、surface/history reset 和 scene revision 均保留为每个 renderer 实例独有的历史输入，不会调用共享 SpectralOcean reset。

质量或光学分辨率变化不再执行全局 `WaitForGpu`：旧 generation 进入既有 frame-ring retirement，序号只在 `BeginFrame` 已回收当前 slot 后推进，因此完整 ring 后的释放对应 slot fence 完成而非单纯 CPU 时间。swapchain resize 仍由上层 RHI resize 的 GPU-idle 安全点触发 immediate release；禁用与重新启用会显式失效该视图历史。完整 `PrismRender`、生命周期/RenderGraph/WaterOptics CPU 测试通过，双 Registry fixture 确认一个 ViewId 的事件不会进入另一个 Registry。D3D12 定向 GPU validation 通过；Vulkan validation 在创建实例前因本机验证层不可用返回 `VkResult=-6`，未继续进行机器级注册表操作，关闭验证层的同一 Vulkan fixture 通过。未运行任何 Demo、四个粒子水场景、全场景或哈希矩阵。4.6 完成，当前进度 **54/128**，下一项是 4.7 Fluid 图契约。

## P3 Fluid 自有图契约与作用域（4.7，2026-08-30）

新增 `Renderer/Features/Fluid/FluidGraph.h`，共享前端只通过 `FluidGraphInputs` 传入当前 scene color、HPWater composite depth 或 opaque depth、以及共享 environment，并通过 `FluidGraphResult` 接收新 scene color。PBF 模拟 buffers、screen-space reconstruction scratch 与 caustics 资源均未进入共享全集，仍由 Fluid 私有持有。

`BuiltInRenderFeatures` 使用稳定 ID `Fluid`、view-local scope 和 PostProcess 阶段接线；SceneRenderer 的旧固定回调已删除。`FluidFeature` 仍是每个 SceneRenderer 的值成员，现有 Resize、RequestReset、Update、EndFrame 及所有 PBF/重建/caustics Pass 未改，不会因架构迁移变成跨视图共享模拟。无窗口 RenderGraph fixture 与生命周期 fixture 通过，分别固定输入资源身份/读写和阶段/scope；未启动四个粒子流体 Demo、场景矩阵、GPU 套件或哈希验证。4.7 完成，当前进度 **55/128**，下一项是 4.8 ClusteredLighting 与 LocalLightShadows。

## P3 灯光 Feature 生命周期接入（4.8，2026-08-30）

`BuiltInRenderFeatures` 已为 `ClusteredLighting` 与 `LocalLightShadows` 建立稳定 ID、view-local scope 和 `Lighting`/`Shadows` 阶段。SceneRenderer 不再保存这两项的旧固定图注册回调，只调用通用阶段入口。两项仍通过现有 SharedRenderGraph bridge 发布 buffer、shadow texture、初态和执行 callback；私有 PSO、shadow atlas、caster/cache signature 与算法未进入通用 FeatureGraphTypes。

ClusteredLighting 的运行时尺寸重建已接入统一 `ResizeLifecycleFeatures`，并要求调用方处于 GPU-idle swapchain 安全点。`AreLifecycleFeaturesInitialized` 区分初始化阶段和运行阶段：初始 `CreateSizeDependentResources` 直接建立一次 cluster buffers；之后 resize 只由生命周期事件建立一次，避免同一尺寸事件重复分配。灯光 Update 与 EndFrame 暂保留原 Clustered→Local 顺序，避免结构迁移改变 CPU 数据准备/缓存提交次序。

直接验证结果：`PrismRender`、`PrismRenderFeatureLifecycleTests` 与 `PrismRenderGraphTests` 构建通过，两个 CTest **2/2**。生命周期 fixture 固定两项 ID/scope/stage、Shadows→Lighting 排序、Clustered resize 操作以及 fixed-resolution LocalShadow 无 resize 操作；既有 RenderGraph fixture 继续覆盖启停后的 Pass 裁剪。最终 `task-4.8-lights-final-02` 在 D3D12/native/strict、1280×800、固定第 30 帧完成，图报告含 `ClusteredLightBuild`；与 P0 `lights-prerequisites-native-01` 同后端比较为 MAE/RMSE/change/max error 全 0、SSIM 1.0。只比较两张直接相关图片，没有运行全场景、四个 Fluid Demo、性能批次或全仓哈希。

`shadows` 阻碍最终定位为构建产物不一致，而不是生产代码回归：`SceneRenderer.h` 的更新时间晚于 `SceneRendererPasses.cpp.obj`、`SceneRendererPipeline.cpp.obj` 等对象文件，增量构建未重编依赖；执行前由新对象访问 TAA RTV 正常，进入旧 `RenderGBufferPass` 对象后却按过期成员布局读取。目标级 clean rebuild 重新编译全部 `SceneRenderer` 翻译单元后，同一 D3D12 `shadows` 2 帧探针从退出 1 恢复为退出 0。此前二分失败批次全部保留，未把它们作为代码回归或通过证据。

clean rebuild 后重新运行 `PrismRenderFeatureLifecycleTests` 与 `PrismRenderGraphTests`，两项均通过。最终证据 `P3/d3d12/task-4.8-shadows-final-clean-01` 使用 native、strict、1280×800、固定第 30 帧；图报告包含 `ClusteredLightBuild`、`SpotShadows` 和 `PointShadows`，相对 P0 `approved-final-all-demos-native-01/shadows-game-f30-r1.bmp` 的同后端比较为 MAE/RMSE/change/max error 全 0、SSIM 1.0。D3D12 只报告既有 warning 820，没有新增 validation error。4.8 完成，当前进度 **56/128**；未运行全 Demo、四个 Fluid Demo、性能或全仓哈希，下一项是 4.9。

## P3 方差阴影与平面反射生命周期接入（4.9，2026-08-30）

`VarianceShadowMaps` 与 `PlanarReflections` 已移除 SceneRenderer 中的旧固定 Registry 注册，改由 `BuiltInRenderFeatures` 以稳定 ID 和 view-local `Shadows` 阶段组装。Variance 的 moments/scratch 保持固定分辨率和 Feature 私有状态，只声明 BuildGraph；Planar 的 color/depth 随视图尺寸重建，因此声明 BuildGraph 与 GPU-idle Resize。初始 `CreateSizeDependentResources` 仍建立一次 Planar 资源，生命周期初始化后由 resize 事件重建一次，随后 ScreenSpaceEffects 继续绑定同一新 color；没有增加第二份资源或改变 Update/EndFrame 状态历史。

生命周期 fixture 固定两项 ID、scope、stage、Variance 无 resize 与 Planar 有 resize 的契约；RenderGraph fixture 继续覆盖 graph 资源读写和 Pass 顺序。最终 `P3/d3d12/task-4.9-shadows-graph-probe-01` 保留 `ShadowMoments`、`ShadowMomentsHorizontal`、`ShadowMomentsVertical`；`P3/d3d12/task-4.9-reflections-final-01` 使用 native、strict、1280×800、固定第 30 帧并保留 `PlanarReflection`，相对 P0 `approved-final-all-demos-native-01/reflections-game-f30-r1.bmp` 的同后端比较全部误差为 0、SSIM 1.0。Feature 私有 scratch 未加入 `FeatureGraphTypes`，PSO、shader、阴影/反射算法和 Demo 设置未改。4.9 完成，当前进度 **57/128**；未运行全 Demo、四个 Fluid Demo、性能或全仓哈希，下一项是 4.10。

## P3 屏幕空间与时域 Feature 生命周期接入（4.10，2026-08-30）

`ScreenSpaceEffects` 与 `TemporalAntiAliasing` 已从 SceneRenderer 的旧固定 Registry 注册迁入 `BuiltInRenderFeatures`。两项保持 view-local owner，分别声明 `PostProcess` 与 `Temporal` 阶段；ScreenSpace 必须依赖 PlanarReflections，TAA 必须依赖 ScreenSpaceEffects。运行时 swapchain 重建先释放旧尺寸资源并建立新 GBuffer、HiZ、HDR，再由 Registry 依照 Planar→ScreenSpace→TAA 顺序分发 GPU-idle Resize，因此 ScreenSpace 取得新 opaque/planar 输入，TAA 取得新 composite 输入，且同一尺寸事件不会在 direct path 与 lifecycle path 重复执行。初始化阶段仍使用原 direct path，Feature 资源所有权没有改变。

scene-change 的 TAA reset 由生命周期事件统一分发；camera-cut 判定仍在每个 SceneRenderer 的 `UpdateHistoryValidity` 中清空本视图 TAA history、前一 world-view-projection 表和 jitter sample，禁用路径与 `EndFrame` history ping-pong 保持不变。生命周期 fixture 用两个独立 Registry 和两个 TAA 实例确认 Game 的 scene event 不进入 Editor history，并固定 Planar→ScreenSpace→TAA resize 顺序。共享 RenderGraph fixture 新增显式断言，固定 `ScreenSpaceReflections`→`TemporalResolve`→`BloomExtract`→`Tonemap`；两项直接测试均通过。

最终证据 `P3/d3d12/task-4.10-post-process-final-01` 使用 native、strict、1280×800、固定第 30 帧；实际图包含 `TemporalResolve`→`BloomExtract`→`BloomHorizontal`→`BloomVertical`→`Tonemap`，相对 P0 `approved-final-all-demos-native-01/post-process-game-f30-r1.bmp` 的同后端比较为 MAE/RMSE/change/max error 全 0、SSIM 1.0。D3D12 只报告既有 clear-value warning 820，没有资源状态错误、device removal 或异常退出。当前/前一矩阵生成、camera-cut 阈值、ScreenSpace/TAA shader、PSO、Demo 设置均未修改。4.10 完成，当前进度 **58/128**；未运行全 Demo、四个 Fluid Demo、性能或全仓哈希，下一项是 4.11 SkyAtmosphere。

## P3 SkyAtmosphere 每帧生命周期接入（4.11，2026-08-30）

4.4 已接入的 `SkyAtmosphere` view-local `FramePreparation` BuildGraph 试点现补齐 `PrepareFrame`。SceneRenderer 原 `UpdateConstants` 内的 `SkyAtmosphere::Update` 调用移入注册时绑定的每视图 prepare callback；callback 从同一 RenderScene 取得相机和方向光，以相同归一化路径更新当前 frame slot 的 atmosphere 常量。统一 prepare 仍发生在常量/资源准备完成后、图构建前，WaterOptics 和其他 Feature 的既有次序不变，没有第二次参数写入。

Transmittance LUT 固定为 256×64，SkyView LUT 固定为 192×108，不依赖 view extent；启用时每帧由原两个 Pass 重新生成，禁用时消费者继续绑定既有已初始化黑色 2D 后备。因此本次没有添加无语义的 Resize 或 SceneChanged 回调，也没有重复初始化资源。初始化、`EndFrame` 状态提交、fallback、descriptor、shader、PSO 和启用谓词保持原实现。生命周期 fixture 固定 ID/scope/stage、PrepareFrame 输入和“不声明 Resize/SceneChanged”的固定 LUT 契约；RenderGraph fixture 继续固定 AtmosphereTransmittance→AtmosphereSkyView 的最前主干。

专项证据 `P3/d3d12/task-4.11-sky-bindings-01` 在 strict/native 下以两个独立 SceneRenderer 覆盖物理天空与 skybox 的不同启停组合、所有 frame slot、其他可选绑定和两次 320×200↔400×250 Resize，8 个 phase 全部通过。固定图证据 `P3/d3d12/task-4.11-atmosphere-final-01` 使用 1280×800、第 30 帧，图执行索引 0/1 分别为 `AtmosphereTransmittance`/`AtmosphereSkyView`；相对 P0 `approved-final-all-demos-native-01/atmosphere-game-f30-r1.bmp` 的 MAE/RMSE/change/max error 全 0、SSIM 1.0。只有既有 D3D12 warning 820。4.11 完成，当前进度 **59/128**；未运行全 Demo、四个 Fluid Demo、性能或全仓哈希，下一项是 4.12。

## P3 Ocean 图输出生命周期接入（4.12，2026-08-30）

`FftOcean`、`SpectralOceanSimulation` 与 `LocalWaveGpuResources` 已从 SceneRenderer 的旧固定 Registry 回调迁入 `BuiltInRenderFeatures`，保持原稳定诊断 ID、实现选择条件和 `FramePreparation` 图输出顺序。Legacy FFT 仍由每个 SceneRenderer 作为值成员持有并标记 view-local；Spectral 与 LocalWave 仍由唯一 `SceneRendererSharedResources` 通过 `shared_ptr` 保活，生命周期注册只借用引用，不声明 initialize/shutdown，也不会分配或复制模拟实例。LocalWave 显式依赖 SpectralOcean，确保资源与 callback 发布次序保持 Spectral→LocalWave。

双视图共享生产规则未移入 Feature 内部：现有 RenderFrameCoordinator 仍选择 producer，secondary view 仍将 `oceanSimulationEnabled` 设为 false，只导入并采样相同共享纹理；因此本次没有引入基于 frame slot 的新逻辑帧判断，也没有第二条 Spectral/foam/local-wave 链。Ocean 参数更新、FFT/foam/local-wave GPU 资源、Update、EndFrame、reset、shader 和 Pass 实现均未修改。

定向构建 `PrismRender`、`PrismRenderFeatureLifecycleTests` 与 `PrismRenderGraphTests` 通过，两个直接测试均退出 0。生命周期 fixture 固定三项 ID/scope/stage、共享 owner 的 borrowed 生命周期、LocalWave 依赖和错误阶段不执行；既有 RenderGraph fixture 继续固定 Legacy/Spectral 互斥和七阶段 Spectral Pass。D3D12 native/strict 代表证据为 `P3/d3d12/task-4-12-ocean-final-01` 与 `P3/d3d12/task-4-12-waveworks-final-01`：Legacy FFT 图像相对 P0 逐像素一致；WaveWorks 图实际只有 `SpectralOcean.InitialSpectrum`、Evolution、双向 FFT、OutputMaps、Foam、Mips，以及 LocalWave 的 DisturbanceUpload/Simulation，共 7+2 个模拟 Pass，无 `FftOcean` Pass。WaveWorks 同后端比较为 MAE 0.000003、RMSE 0.000283、changed 0.000021、SSIM 0.999996，通过原 V2 门限。只有既有 D3D12 warning 820。4.12 完成，当前进度 **60/128**；未运行全 Demo、HPWater 矩阵、四个 Fluid Demo、性能或全仓哈希，下一项是 4.13。

## P3 地形与虚拟纹理生命周期接入（4.13，2026-08-30）

`InteractiveTerrain` 与 `VirtualTextureCache` 已从 SceneRenderer 的固定接线迁入 `BuiltInRenderFeatures`，稳定 ID 分别为 `InteractiveTerrain` 和 `VirtualTextureCache`，统一位于 `FramePreparation`。共享地形仍由 `SceneRendererSharedResources` 唯一持有，注册为 device-shared borrowed reference，不增加 initialize/shutdown 或第二份高度资源；scene-change 清除旧场景 command revision 水位并将已有高度图标记为待初始化，下一次启用图执行仍走原 brush→erosion 算法。VT 仍是每个 SceneRenderer 的 view-local 值成员，原相机邻域请求、CPU LRU、页表编码、atlas 内容与启用条件不变。

VT atlas 替换增加按 `framesInFlight` 定界的资源退役环：当前帧槽重写 descriptor 前，将旧 texture/view 保留到同槽下一次 BeginFrame 已完成 fence 等待；scene reset 也延迟到安全帧槽执行并发布新 generation。新增 `PrismVirtualTextureCacheTests` 用 mock RHI 覆盖初始页、49 页请求、hit/miss 与相机移动、reset、generation、两帧退役上界，以及 weak reference 在对应帧槽回收前后精确存活/释放；`PrismRenderFeatureLifecycleTests` 固定两项 scope/stage/scene event/borrowed ownership，`PrismRenderGraphTests` 保持通过。

代表证据 `P3/d3d12/task-4-13-terrain-final-02` 使用 D3D12 native、strict、1280×800、固定第 30 帧，进程正常退出，图中 `InteractiveTerrain.BrushAndErosion` 恰好一次；相对 P0 `approved-final-all-demos-native-01/terrain-vt-game-f30-r1.bmp` 的 MAE/RMSE/change/max error 全 0、SSIM 1.0。首次探针还定位到中文 MSVC `/showIncludes` 尾随空格使 Ninja 头依赖数据库为空、旧对象类布局未重编；CMake 已固定实际 UTF-8 前缀且移除尾随空格，重新构建后 `RenderFrameCoordinator.cpp.obj` 记录 84 个头依赖，消除后续同类 ABI 风险。只有既有 D3D12 warning 820。4.13 完成，当前进度 **61/128**；未运行全 Demo、四个 Fluid Demo、性能或全仓哈希，下一项是 4.14。

## P3 GPU-driven visibility 生命周期接入（4.14，2026-08-30）

`GpuDrivenVisibility` 已从 SceneRenderer 的固定 legacy 注册迁入 `BuiltInRenderFeatures`，稳定 ID 为 `GpuDrivenVisibility`，作用域保持 view-local，参与 `FramePreparation`。PrepareFrame 继续使用当前 Scene、Camera、frame slot、extent、HiZ validity 和原 culling settings；BuildGraph 继续通过过渡 bridge 发布同一 object/argument/count buffers。原 `ObjectRecord` 构造未改，`firstInstance` 仍等于共享 RenderObject index，batch 内 `drawArgumentOffset`/`firstArgument` 和 indirect count 路径不变。禁用或后端不支持时不发布 GPU resources/Pass，SceneRenderer 原 CPU draw path 保持。

Resize 只在 `RecreateSwapChainResources` 已取得 GPU-idle 的统一 lifecycle 事件中绑定新 HiZ；初始化仍在 lifecycle 注册前绑定首次 HiZ。SceneChanged 新增的窄通知只清空旧场景延迟 readback 的 object-index/camera 关联及 pending 标志，不释放仍由 frame resources 持有的 buffer；下一次启用 Update 按原 memcmp/容量规则重建 object records。这样旧场景反馈不会染入 Editor 辅助显示，而 GPU 资源寿命仍由原 frame-slot 安全点控制。

直接测试 `RenderFeatureLifecycle` 与 `RenderGraph` 为 2/2。新增 `PrismGpuDrivenVisibilityGpuTests` 在 D3D12/Vulkan 为 2/2：Vulkan 实际执行多帧 `GpuVisibility`，验证候选对象数、readback reason/camera、关闭后的 CPU fallback、480×300→640×360 resize、GpuDriven→Preview 场景切换和新对象映射；D3D12 验证当前不支持 indexed object drawing 时不产生 GPU visibility Pass 且仍渲染 CPU fallback。Vulkan 代表证据 `P3/vulkan/task-4-14-gpu-driven-final-01` 的图包含 `GpuVisibility`、`IndirectArguments`、`IndirectDrawCounts` 及 `drawIndirectCount=true`。该样本显式开启 GPU-driven，而 P0 deterministic golden 会在初始化时关闭它，因此两种模式的失败图像比较保留在 `P3/comparisons/task-4-14-gpu-driven-vulkan-01`，不被误判为结构回归。协议匹配的 fallback 证据 `P3/vulkan/task-4-14-gpu-driven-fallback-01` 不含 `GpuVisibility`，与 P0 `approved-final-all-demos-native-01/gpu-driven-game-f30-r1.bmp` 的 MAE/RMSE/change/max error 全 0、SSIM 1.0。4.14 完成，当前进度 **62/128**；未运行全 Demo、四个 Fluid Demo、性能或全仓哈希，下一项是 4.15。

## P3 LogicalFrameId 共享生产与作用域事件路由（4.15，2026-08-30）

`RenderFrameCoordinator::BeginLogicalFrame` 现在以单调 `LogicalFrameId` 选择一次 device-shared producer，并把循环 `frameSlot` 明确限制为资源回收索引。Game 是默认生产者；Game 不可用时回退到 Scene；无消费者时不选择生产者。同一逻辑帧重复查询不会增加决策次数，新的逻辑帧可以安全复用相同 frame slot。协调器把选择写入两个 SceneRenderer，显式逻辑帧 Render 会拒绝未分配、错序或同一视图重复提交；直接使用 SceneRenderer 的独立测试仍保留局部单调 ID 后备。

Feature Registry 新增按 `RenderFeatureScope` 分发 PrepareFrame、Resize 与 SceneChanged。每个视图始终接收 view-local 事件，只有选中的 producer 接收 device-shared Prepare；协调器 resize/scene switch 则只让 Game Registry 分发一次 shared 事件、Scene Registry 只处理本视图事件。共享 SpectralOcean、LocalWave 和 InteractiveTerrain 的 mutation 由逐帧 producer 开关控制，而资源消费契约仍显式存在：secondary terrain view 继续导入 raw/eroded height，在 Shadow/GBuffer 中声明 `ShaderResource` 读取，只省略 `InteractiveTerrain.BrushAndErosion` 写入 Pass。因此 GPU barrier/queue dependency 不会被一个 CPU 布尔值替代。

定向 `PrismRenderFrameCoordinatorTests` 覆盖双消费者单 producer、同 LogicalFrameId 单次决策、frame slot 0/1/0 回绕、默认 Game 缺席时 Scene 接管、无消费者、Scene 隐藏及恢复；`PrismRenderFeatureLifecycleTests` 固定 shared/view 事件不串线；`PrismRenderGraphTests` 固定 secondary view 有地形 GPU read、无 mutation，producer 恰有一次 mutation，三项均通过。D3D12 代表证据 `P3/d3d12/task-4-15-logical-frame-02` 为 `waveworks-ocean` 连续 30 帧双视图：两视图 LogicalFrameId 均匹配 1..30，frame slot 为 0/1 回绕，Game 每帧记录 8 类 Spectral/LocalWave 共享模拟 Pass，Scene 为 0。第 30 帧相对 P0 `approved-final-all-demos-native-01/waveworks-ocean-game-f30-r1.bmp` 的 MAE 0.000001、RMSE 0.000123、changed 0.000004、maximum error 0.062745、SSIM 0.999999，通过原同后端门限。首次 2560×1440 探针因与 1280×800 基线尺寸不同被正确拒绝，保留在 `P3/comparisons/task-4-15-logical-frame-d3d12-01`，不作为视觉失败。4.15 完成，当前进度 **63/128**；未运行全 Demo、四个 Fluid Demo、性能或全仓哈希，下一项是 4.16。

## P3 独立 Feature 扩展与错误归因（4.16，2026-08-30）

新增 `tests/IndependentRenderFeatureFixture.h`，只编入测试。该 fixture 以稳定 ID `test.independent-post-process`、view-local scope 接入既有 PostProcess 阶段；输入、输出、slot tag 全部在测试头中定义，通过 `RenderGraphBlackboard::Require/Publish` 交换，并登记 `TestIndependentPostProcess` Pass。它不读取或写入 legacy resource/callback bridge，生产 `SharedRenderGraphResources`、`SharedRenderGraphCallbacks`、`FeatureGraphTypes` 均未为测试扩展增加字段，生产管线也从不注册该 Feature。

`RenderGraphBlackboardRequest` 增加可选 `resourceName` 诊断字段。slot/value C++ 类型仍决定实际契约，名称只用于错误归因；缺失、错误类型、过期 generation、错误 version 和跨视图访问现在都能同时指出 consumer Feature ID 与资源名。直接 fixture 先由既有 Temporal Feature 发布 token 41，再接入独立 PostProcess 输入 9/输出 10，确认既有 token 保持 41；禁用独立 Feature 时 build count、Pass 和输出均为零；缺少输入时异常同时包含 `test.independent-post-process` 与 `IndependentPostProcess.InputColor`。`PrismFeatureGraphContractTests`、`PrismRenderFeatureLifecycleTests` 均退出 0，完整 `PrismRender` 重新编译链接通过。4.16 完成，当前进度 **64/128**；未运行 Demo、GPU、性能或全仓哈希，下一项是 4.17。

## P3 legacy 图桥清理与类型化契约收口（4.17，2026-08-30）

ClusteredLighting、LocalLightShadows、VarianceShadowMaps、PlanarReflections、ScreenSpaceEffects、TemporalAntiAliasing、FftOcean、SpectralOceanSimulation 与 LocalWave 已从 legacy shared resources/callback bridge 迁移为各自的类型化 contribution。`BuiltInRenderFeatures` 现在统一登记全部 15 个内置 Feature 接入点；view-local 与 device-shared 发布仍遵循既定作用域，Frontend 只在对应执行路径要求可选 contribution，并为主照明保留必须采样的阴影和时域资源。

Registry legacy adapter、旧 shared-struct Feature 字段、重复的 RegisteredFeatures Blackboard 发布及 SceneRenderer 接线已经删除。生产树中 `legacyResources`、`legacyCallbacks`、`RegisterLegacyAdapter`、`RenderFeatureRegistrationContext`、`RegisteredRenderFeatures` 与 `PublishToBlackboard` 调用者检索为零。`SharedRenderGraphResources/Callbacks` 只剩场景几何与渲染主干输入，不包含新的 Feature 全集；SceneRenderer 仍负责合理的主阴影、GBuffer、主照明、HiZ、OceanQuery、Bloom/Tonemap/Output 编排。定向构建 `PrismRender`、`PrismRenderFeatureLifecycleTests` 和 `PrismRenderGraphTests` 通过，两个直接测试均退出 0。4.17 完成，当前进度 **65/128**；未运行 Demo、GPU、性能或全仓哈希，下一项是 4.18。

## P3 生命周期集成验收（4.18，2026-08-30）

直接 Feature 契约继续由 `PrismFeatureGraphContractTests`、`PrismRenderFeatureLifecycleTests`、`PrismRenderFrameCoordinatorTests` 与 `PrismRenderGraphTests` 覆盖；4.17 清理后重新执行直接受影响的生命周期和 RenderGraph fixture，均通过。当前 D3D12 `waveworks-ocean` 以 strict/native 连续运行 30 帧，诊断包含 Game LogicalFrameId 1..30 与一次按需 Scene 同帧消费；Game 是唯一共享模拟 producer，Scene 不重复产生 Spectral/LocalWave Pass，请求 native、实际执行 `dag_multi_queue`。固定第 30 帧相对 P0 的 MAE 0.000001、RMSE 0.000123、changed 0.000004、maximum error 0.062745、SSIM 0.999999。

HPWater 使用与冻结 P2 完全相同的 underwater/high/single/reference/native/strict 配置连续运行 330 帧，stdout 到达 frame 320 恢复断言，最终 water coverage 为 0.7886064648628235；相对 P2 同后端 lifecycle 图像的 MAE 0.000070、RMSE 0.003398、changed 0.000403、SSIM 0.999793。请求 native、实际为 `dag_multi_queue`，没有 D3D12 warning 820 之外的 validation 信息。首次 default-camera 探针因与 underwater 基线协议不一致，仅作为无效比较证据保留，不计入结果。4.18 完成，当前进度 **66/128**，P3 完成；未运行全 Demo、四个 Fluid Demo、Vulkan、性能双批或全仓哈希，下一项是 5.1。

## P4 场景发布与双视图复制基线（5.1，2026-08-30）

新增 `RenderSceneExtractionStatistics`，将 scene-data build/reuse、mailbox envelope、未来 RenderFramePacket 小对象计数、publication/Game/Scene 完整复制次数、估算对象/字节及三段 CPU 时间独立记录。字节估算包含 `RenderScene` 固定值、`RenderObject` 数组和对象字符串，不递归计算 shared_ptr 指向的 GPU 资产。统计只有显式设置 `PRISM_RENDER_SCENE_PUBLICATION_STATS_PATH` 时启用并在 Run 结束后一次写出，默认渲染不执行 O(N) 统计扫描。

新增 `Benchmark-ScenePublication.ps1`，使用确定性有限帧输入、隔离输出目录和不覆盖策略生成 plan/report，并检查当前 legacy full-copy 模型的计数自洽。D3D12 `preview` 两次独立 8 帧运行得到相同结构：每帧 6 对象，8 次 publication、8 次 Game View、16 次 Scene View 完整复制，总计估算 192 个对象和 250272 字节；scene-data build 8、reuse 0、FramePacket 小对象 0。CPU publication/Game/Scene build 时间分别记录而不作为结构复现门禁。最终二进制另以 2 帧确认报告仍为 2/2/4 次复制和 62568 字节。`PrismRender` 与 `PrismWorldRenderSceneBridgeTests` 构建通过，场景桥测试退出 0。5.1 完成，当前进度 **67/128**；未运行全 Demo、Fluid、GPU validation、性能分布或哈希，下一项是 5.2。

## P4 版本化场景、帧与视图类型契约（5.2，2026-08-30）

新增强类型 `SceneGeneration`、`RenderSceneDataRevision`、`RenderViewId` 与 `LogicalFrameId`。`RenderSceneData` 封装不可变 RenderObject CPU 值；每帧共享的方向光/点光/聚光灯属于 `RenderFrameDynamicData`；相机、前一相机、extent、history/cut revision 与基于共享对象索引的 selection 属于 `RenderView`。`RenderFramePacket` 使用 `shared_ptr<const RenderSceneData>` 组合逻辑帧、模拟时间、动态块和多个独立 View，因此发布新 revision 不会原地修改旧 packet。

构造时拒绝零身份、空 scene data、非法时间、空/重复/零尺寸 View、超容量灯光及越界对象 selection。新 `PrismRenderScenePublicationTests` 直接覆盖源容器修改不影响已发布数据、旧/新 packet revision 并存、Game/Scene history 隔离、FindView 和错误输入；测试退出 0，完整 `PrismRender` 构建链接通过。新类型尚未接管 mailbox 或 SceneRenderer，现有 `WorldRenderSnapshot`、WorldSerializer 和文件格式源码未编辑。5.2 完成，当前进度 **68/128**；未运行 Demo、GPU、性能或哈希，下一项是 5.3。

## P4 稳定对象身份与视图索引映射（5.3，2026-08-30）

新增带 World/programmatic domain 的 128-bit `RenderObjectId` 与场景局部 `RenderObjectIdentityRegistry`。World 对象直接保留 EntityId；无 EntityId 对象必须提供稳定 programmatic key，并由当前 SceneGeneration 分配。相同 generation 的 data revision 重建复用 object ID 与 firstInstance；新的 topology generation 清空程序化/instance 映射，World EntityId 值仍保持，但会与新 SceneGeneration 一起构成反馈身份。

`BuildRenderSceneData` 先解析全部身份，再把 World 或 programmatic parent 引用映射到当前共享数组索引，拒绝重复 key/EntityId、缺失父节点、双重父声明与环。`RenderSceneData` 保存只读 metadata 和 ID→index 映射。`BuildRenderViewSelection` 只生成共享数组索引，Game/Editor selection 不删除或压缩对象。直接测试覆盖对象新增、删除、输入重排、topology rebuild、两类父子映射，并确认过滤前后 parentIndex 与 firstInstance 完全不变；测试与完整 `PrismRender` 构建通过。5.3 完成，当前进度 **69/128**；未运行 GPU、Fluid、场景矩阵或哈希，下一项是 5.4。

## P4 CPU 参数值化与运行时资产绑定版本（5.4，2026-08-30）

`AssetRegistry` 新增单调 `RuntimeAssetBindingRevision`。mesh、texture、material 的每次运行时发布、替换或 eviction 都为对应槽写入新 revision；binding 查询返回原 32-bit handle、revision 和当时的 shared resource lease，原 `GetRuntime*` API 保留。`AssetHandle` 本身与 SceneSerializer 均未修改，已有文件格式不增加 revision 字段。

新 `RenderSceneObjectAssetBindings` 将 mesh 和五个材质 texture 作为 const lease 保活，将 `Material::Parameters` 复制为 CPU 值。新场景数据中的 RenderObject 不再携带可变 mesh/material 指针，因此源材质原地改写或 Registry 后续替换不会改变旧快照的可观察参数和绑定。这里不深拷贝 GPU resource 内部内容；上传完成、descriptor 更新、frame-slot fence 与资源退役仍服从既有同步路径，binding revision 只描述 Registry 槽发布身份。

定向 `PrismRenderScenePublicationTests` 覆盖 mesh/material/texture 三类替换、源 Material 原地改写、texture eviction、旧 lease 保活、新旧 revision 不同，以及 handle 值和 32-bit 布局不变；测试退出 0。`PrismRender` Release 完整编译链接通过。5.4 完成，当前进度 **70/128**；新模型仍未接管生产 mailbox，未运行 Demo、GPU、Fluid、性能、场景矩阵或哈希，下一项是 5.5。

## P4 场景变更分类与原子提交（5.5，2026-08-30）

新增 `Engine::SceneChangeTracker`，按 entity topology、hierarchy、transform、visibility、material、camera、lighting 与保守 full rebuild 分类。CommandProcessor transaction.begin 开启 change scope，成功 mutation 只累积类别，commit 对外发布一个单调 revision，rollback 同时恢复 World 并丢弃 scope；独立命令、undo/redo 和 journal replay 复用同一入口。simulation 配置/步进不误标为静态场景，未知 component 则保守要求 full rebuild。请求缓存命中不会重复发布。

SceneSession 的 dirty 来源已改为 tracker 的 committed change set，原手动 dirty API 作为未知直接写入的 FullRebuild 兼容入口。`WorldRenderSceneBridge` 接收并回传 revision/category/mutationCount；只有 bridge 成功后 Session 才消费 pending set，因此 transaction 中间状态和 rollback 状态不会发布到 RenderScene。Bridge 当前仍执行完整同步，增量 extraction 策略留给 5.8。

定向 `PrismSceneSessionTests` 覆盖 transaction 内无 pending、commit 单 revision、entity/hierarchy/transform/visibility/material 分类、rollback 无发布、undo/redo/replay 重发布和 bridge consume；`PrismWorldRenderSceneBridgeTests` 固定 change identity 透传。两项测试退出 0，`PrismRender` Release 链接通过。5.5 完成，当前进度 **71/128**；未运行 Demo、GPU、Fluid、性能、场景矩阵或哈希，下一项是 5.6。

## P4 资产绑定与场景激活 revision 接线（5.6，2026-08-30）

AssetRegistry 的统一 runtime setter 现在同时记录 Published/Cleared change，因此同步 manifest load、editor reimport、streaming upload complete 与 eviction 无需复制失效逻辑。SceneSession 消费 change batch 并保存最新 binding revision；binding 替换不推进 scene generation 或 data revision。World commit 经 bridge 成功应用时只推进 data revision；Demo/streaming/programmatic 场景通过 `InitializeWorldFromRenderScene` 推进 scene generation 并将 data revision 重置为 1。

RenderScene 新增 topology revision，标准 Add/Clear/Replace 路径可观察。Demo 和程序化 factories、glTF loader、streaming activation 已使用 Add/Clear；World bridge 与 SceneSerializer 的整数组替换迁到 Replace。streaming activation result 显式报告 topologyChanged。旧 mutable accessor 仅保留值编辑兼容，未知拓扑写入仍必须走 Session `MarkWorldChanged`，它发布 FullRebuild。

`PrismAssetRuntimeCoordinatorTests` 固定 import/reload 与 streaming upload 的 Published binding，以及极小预算 eviction 的 Cleared binding；`PrismSceneSessionTests` 固定 binding-only 不改变 scene/data、World 同步只推进 data、程序化重建推进 generation 并重置 data。三项定向测试（含 World bridge）退出 0，完整 `PrismRender` Release 链接通过。旧快照 lease 保活沿用 5.4 证据。5.6 完成，当前进度 **72/128**；未运行 Demo、GPU、Fluid、性能、场景矩阵或哈希，下一项是 5.7。

## P4 动态相机、灯光与设置输入（5.7，2026-08-30）

新增 `RenderDynamicInputState` 并由 `SceneSession` 独占。状态容量固定为 Game/Scene 两个 View，包含逻辑帧与模拟时间、按值复制的小型灯光块、当前/前一相机、extent、camera/history/cut revision 及历史失效原因。场景 generation 切换会重置动态视图，但普通导航、灯光或设置变化不改变 `RenderSceneDataRevision`；World bridge 的 Camera/Lighting-only change set 也不再误推对象 revision。

`RenderHistorySettingsTracker` 留在 Renderer 层，使用现有 `ClassifyWaterOpticsDirtyScopes` 以及 TAA、GTAO、SSR、Planar、HDR、deferred、viewport shading 和 Ocean reset/quality 等历史相关字段生成单调 token。Scene 只接收 token，不包含或反向依赖完整 `RenderSettings`。ApplicationHost 在原有 BeginFrame 成功后、RenderScene 发布前采集两相机、两 View extent、灯光、设置 token 与 simulation time；Editor 导航、无 Editor 导航、capture action 和 WaterValidationSequence 因此共享同一输入路径。WaterValidationSequence 在既有 frame 96、120、252 镜头切换处显式通知两个 View 的 CameraCut，原相机值、设置修改、场景切换和 HPWater 代码保持不变。

定向验证：Release 构建目标 `PrismRenderScenePublicationTests`、`PrismSceneSessionTests`、`PrismRender` 均成功；两个测试程序退出 0。覆盖 camera-only 不推进对象 data revision、Camera/Lighting change category 正交、Game/Scene 相机隔离、history settings 只使目标 View 失效、灯光 revision、连续 simulation time 与负高度水下相机值。遵照本轮验证约束，未运行四个 Fluid Demo、任何 Demo/全场景矩阵、GPU/性能或哈希验证。5.7 完成，当前进度 **73/128**；下一项是 5.8。

## P4 FullRebuild / Versioned 提取策略（5.8，2026-08-30）

新增 `RenderSceneExtractor`，两种诊断策略共用 `BuildRenderSceneData`、稳定对象身份 registry 与运行时绑定捕获。`full-rebuild` 每次产生新的不可变数据；`versioned` 只在 scene generation、data revision、runtime binding revision 和 RenderScene topology revision 全部一致时复用缓存值，已知 transform/binding/topology 变化均重建。未知 mutable 写入由既有 `SceneChangeCategory::FullRebuild` 驱动，versioned 结果会显式报告 effective full-rebuild、`conservativeFallback=true` 与 `unknown-write-full-rebuild`，不静默冒充复用。

`PRISM_RENDER_SCENE_PUBLICATION_MODE` 缺失或空值继续选择 `full-rebuild`；合法值只有 `full-rebuild` 和 `versioned`，非法值由 SceneSession 创建提取器时抛错。SceneSession 已建立 owner，但 5.9 前旧 mailbox 和 SceneRenderer 仍使用原 RenderScene 帧，因此此项没有切换生产输入或改变 Demo 默认画面。

定向 `PrismRenderScenePublicationTests` 对同一初始、transform mutation 和 runtime mesh binding mutation 分别运行两策略，并比较对象值、metadata 和 binding revision；另验证 full 每次重建、versioned 静态复用、未知写入可见 fallback 及配置正反例。`PrismRenderScenePublicationTests`、`PrismSceneSessionTests` 退出 0，`PrismRender` Release 构建链接通过。未运行 Demo、GPU、Fluid、场景矩阵、性能或哈希。5.8 完成，当前进度 **74/128**；下一项是 5.9。

## P4 自洽 FramePacket mailbox（5.9，2026-08-30）

SceneSession 现在把 extractor 返回的 `shared_ptr<const RenderSceneData>` 与动态输入状态组装为 `RenderFramePacket`。Packet 冻结 LogicalFrameId、simulation time、共享灯光值和 Game/Scene 两个 RenderView；每个 View 的 selection 只保存共享对象数组索引，Game 排除 editor-only，Scene 保留完整映射。Versioned 模式下连续静态帧复用同一 scene-data 指针，但每帧仍产生独立的小 packet。

`RenderSceneMailbox` 保留原小互斥锁与 latest-only 容量，并新增 `PublishFramePacket/AcquireLatestPacket`。为分阶段迁移，latest envelope 同时保存新 packet 和旧 RenderScene 值；旧 SceneRenderer 在 5.10 前继续读取该适配值。替换 latest 不删除外部仍持有的旧 envelope/packet/data，外部释放后则立即回收；mailbox 自身不保存历史队列。ApplicationHost 已改为先提取/组包再一次发布，提取统计现在分别记录 scene-data build/reuse 和一个 FramePacket 小对象。

`PrismRenderScenePublicationTests` 覆盖连续发布、scene generation 400→401 切换、latest packet 一致性、旧 consumer 保活、consumer 释放后的 weak packet/data 回收，以及连续 16 帧只保留 latest。`PrismSceneSessionTests` 覆盖真实 Session 的两帧 versioned scene-data 指针复用、逻辑帧/时间推进、双 View 与 mailbox latest；World bridge 旧 mailbox 测试保持通过。三项测试退出 0，`PrismRender` Release 构建链接通过。未运行 Demo、GPU、Fluid、场景矩阵、性能或哈希。5.9 完成，当前进度 **75/128**；下一项是 5.10。

## P4 SceneRenderer 直接消费不可变 packet/view（5.10，2026-08-30）

新增 `Scene/RenderSceneView.h` 作为只读渲染输入适配器。packet-backed view 强持有 `shared_ptr<const RenderFramePacket>`，相机、前帧相机、灯光、对象选择和共享对象数组分别从指定 View 与 packet 动态块读取；mesh、材质参数和五类纹理则从 `RenderSceneData` 冻结的 runtime binding lease 解析。该适配器不压缩或复制对象数组，地形 LeafOnly/RootOnly 策略只控制稳定索引是否参与当前 View。旧 `RenderScene` 构造入口暂留给 5.15 前的直接 fixture 和兼容调用者。

SceneRenderer、几何/透明 draw、主阴影、ClusteredLighting、GpuDrivenVisibility、PlanarReflections、LocalLightShadows、FluidFeature 以及 RenderFeature frame/graph context 已迁移到只读 view。运动矩阵读取 View 独立的 current/previous camera；光照读取 packet dynamic block；材质常量、descriptor texture 和实际 draw mesh 均使用冻结 binding。所有跨 RenderGraph 建图、异步录制与执行的 callback 都捕获强引用 view。ApplicationHost 在同一逻辑帧直接把同一 packet 以 GameRenderViewId/SceneRenderViewId 交给两个 renderer，删除主循环中的 `CreateGameView`、`CreateEditorView` 与 visibility debug scene 全量复制；mailbox 生产发布只保存 packet，旧双参数 API 仅保留测试适配。

定向 `PrismRenderScenePublicationTests` 验证 packet view 保活、共享对象数组地址一致、灯光/选择读取及地形双策略不改索引；`PrismRenderGraphTests` 以 Graphics→Compute 原生异步并行录制验证两个 graph lambda 均可读取 retained packet，并确认 graph reset 后旧 packet 回收。`PrismRender`、Scene publication 和 RenderGraph Release 目标构建通过，两项测试退出 0。D3D12 headless `preview` 仅运行 3 帧并以 0 退出，覆盖真实 packet、资源 lease、建图和提交路径。未运行 HPWater、四个 Fluid Demo、全场景矩阵、GPU validation、性能分布或哈希。5.10 完成，当前进度 **76/128**；下一项是 5.11。

## P4 版本化 GPU 可见性反馈与 Editor 辅助显示（5.11，2026-08-30）

新增不可变 `RenderViewFeedback`。GpuDrivenVisibility 在提交 diagnostics readback 时把来源 `SceneGeneration`、`RenderSceneDataRevision`、`RenderViewId`、`LogicalFrameId` 和 culling camera 保存到对应 frame slot；BeginFrame 已等待该 slot 后，ResolveReadback 将 identity、逐对象 reason 与相机一起发布为共享只读结果，仍保持原有延迟回读且不增加 GPU wait。场景资源重建、scene change、readback 关闭和无效来源会清除反馈。

ApplicationHost 在 Game 渲染后把其最新 feedback 交给按需 Scene View。`RenderSceneView` 只接受 scene/data revision 与当前 packet 一致、来源 ViewId 为 Game 且逻辑帧不晚于当前帧的反馈，reason 数量也必须与共享对象数组一致。错误 scene generation、旧 data revision、错误 ViewId、未来帧或尺寸不匹配的结果被丢弃。匹配结果利用 `RenderSceneObjectMetadata::parentIndex` 选择 GPU 已拆分的地形 root/child，terrain debug 颜色读取反馈 reason；Editor 的 `GameCameraFrustum` 使用反馈中实际执行 culling 的相机。无有效反馈时保守显示 root 并使用 packet Game camera。全程不写 `RenderObject::gpuVisibilityReason`、transform 或共享对象数组。

`PrismRenderScenePublicationTests` 覆盖匹配反馈、三类迟到/错视图拒绝、root/child 双视图选择、父子索引不变、辅助视锥位置与源对象 reason 保持 Unknown；该测试和 `PrismRenderGraphTests` 均退出 0。`PrismRender` Release 构建链接通过，D3D12 headless `preview` 仅运行 3 帧并正常退出。未运行 HPWater、四个 Fluid Demo、全场景矩阵、GPU validation、性能分布或哈希。5.11 完成，当前进度 **77/128**；下一项是 5.12。

## P4 Mutation 完整性与 raw mutable 审计（5.12，2026-08-30）

`RenderScene` 不再提供会被非 const 对象误选的可写 `GetRenderObjects()`；生产期确需原地值修改的入口改名为 `EditRenderObjectsForFullRebuild()`，并以 O(1) raw-mutation revision 使 extractor 能识别未提交写入。标准 Add/Clear/Replace 的 topology revision 与 raw revision 分开。Versioned 模式发现未伴随 scene/data commit 的拓扑或值写入时分别报告 `untracked-topology-full-rebuild`、`raw-mutable-write-full-rebuild`，effective mode 为 full-rebuild，绝不静默复用；正常静态复用的 `sourceObjectVisitCount` 为 0，不扫描或哈希对象数组。

完整 D5 mutation fixture 覆盖 topology、hierarchy、transform、visible/editorOnly、mesh/material、camera/light、scene generation、unknown fallback 和 binding revision；每个已提交静态 mutation 第一次 rebuild、同 revision 第二次 reuse。旧 packet/view 在 mesh/texture 替换后继续持有旧 lease，最后 consumer 释放后 weak lease 回收。SceneSession 另覆盖部分失败命令和外层 transaction rollback 不泄漏发布状态。生产 raw-write 来源和保守后备映射见 `RENDER_SCENE_MUTATION_AUDIT.md`。定向 Scene publication 与 SceneSession 测试通过；未运行 Demo、GPU/性能矩阵或全仓哈希。5.12 完成，当前 **78/128**。

## P4 FullRebuild / Versioned GPU 对照与保活（5.13，2026-08-30）

D3D12/native HPWater 60 帧等输入 A/B：full-rebuild 的 59 个已完成统计帧为 59 build/0 reuse，versioned 为 1 build/58 reuse，两者完整对象复制均为 0；末帧 MAE 0.000046、RMSE 0.002147、changed 0.000245、SSIM 0.999884。Terrain-VT 30 帧为 29/0 对 1/28，末帧逐像素一致。证据分别位于 `artifacts/architecture-refactor/P4/task-5.13-hpwater-*` 与 `task-5.13-terrain-*`。

额外只运行一次必要的 versioned HPWater lifecycle：330 个已完成帧为 4 build/326 reuse，覆盖 history reset、168/180 resize、192/204/216 三次 Ocean scene switch 以及 frame 320 coverage 恢复；相对 P3 同输入 full-rebuild 参考为 MAE 0.000033、RMSE 0.001617、changed 0.000119、SSIM 0.999927。D3D12 只有既有 warning 820，无新增 validation/lifetime 错误；旧 packet binding lease、并行 graph lambda 和最终回收由 5.10/5.12 CPU fixture 固定。没有运行四个 Fluid Demo、全 Demo、Vulkan 或性能双批。5.13 完成，当前 **79/128**。

## P4 静态、相机与持续编辑复制/性能验收（5.14，2026-08-30）

`artifacts/architecture-refactor/P4/task-5.14-static-versioned-120/scene-publication.json` 的 D3D12/native `preview` 连续 120 帧为 1 build/119 reuse；后续复用帧不访问源对象，publication/Game/Scene 完整对象复制和估算字节均为 0，每帧只创建一个 FramePacket 小对象。`PrismRenderScenePublicationTests` 进一步固定 120 帧 camera-only 为 0 build/120 reuse/0 source visit，以及 120 个连续 Transform 编辑批次各自第一次恰好 rebuild、同 revision 第二次 reuse，从而同时拒绝漏 rebuild、重复 rebuild 和动态场景伪复用。

结构阶段仅采集一个代表场景，不重复双批矩阵。`artifacts/architecture-refactor/P4/task-5.14-performance-preview-versioned-v1/` 使用与 P0 匹配的 D3D12/native、双视图、1280×800、可见且不聚焦窗口，预热 180 后保留 180 个有效帧。相对 P0 `locked-clock-final-baseline-20260829-03/benchmark/d3d12-preview-b1-r1`，CPU frame/loop 与 Game/Scene GPU Renderer 的 median/p95 均通过 5%/10% 信号门限；当前 extraction median/p95 为 0.0034/0.0050 ms，P0 为 0.0123/0.0184 ms。该单次结果是结构回归信号，不宣称正式 P7/P8 多进程性能收益。5.14 完成，当前 **80/128**。

## P4 默认 Versioned 与旧帧输入清理（5.15，2026-08-30）

缺失/空 `PRISM_RENDER_SCENE_PUBLICATION_MODE` 与默认构造的 extractor 现在选择 `versioned`；显式 `full-rebuild` 保留为诊断/恢复策略，非法值仍启动失败。同 mutation 的对象值、metadata 和 binding 对照 fixture 继续证明两策略输出等价。`artifacts/architecture-refactor/P4/task-5.15-default-versioned-120/scene-publication.json` 未设置 mode 环境变量，实际 120 帧为 1 build/119 reuse、零完整对象复制。

Mailbox 已删除 `Publish(RenderScene)`、packet+mutable scene 双参数发布和公开 legacy envelope 获取；Host 只发布/取得同一不可变 packet。SceneRenderer 删除逐帧 `RenderScene` Render 重载，两个 GPU fixture 使用共用 `tests/RenderFramePacketFixture.h` 构造真实不可变帧输入。`RenderSceneView` 的 mutable scene 形式需要显式 `RenderSceneControlView` tag，且只在 Initialize/SceneChanged 生命周期中使用，不可误作帧提交。旧 mailbox/逐帧 RenderScene 接口调用检索为 0。`PrismRender`、Scene publication、World bridge、GpuDriven/Sky GPU fixture 目标构建通过，两个 CPU fixture 退出 0；GPU fixture 只编译未运行。CLI、World 文件、统计 JSON format/version 和既有字段未改。5.15 完成，当前 **81/128**。

## P4 规格覆盖、恢复与阶段结论（5.16，2026-08-30）

版本化快照规格的 mutation 覆盖由 5.5、5.6、5.8、5.12 的 SceneSession/World bridge/publication fixture 固定；旧 CPU/GPU lease 生命周期由 5.4、5.10、5.12、5.13 固定；camera-only、双视图隔离、稳定 selection/feedback 和零复制分别由 5.7、5.3/5.11、5.10、5.14 固定。HPWater/terrain 等输入图像与 lifecycle 证据使用 5.13 报告，默认路径与 V3 单样本使用 5.14–5.15 报告。四个粒子 Fluid Demo 按用户要求不属于本阶段运行集，Fluid 只沿用模块 fixture。

运行时首选恢复无需回退源码：设置 `PRISM_RENDER_SCENE_PUBLICATION_MODE=full-rebuild`，其 FramePacket/RenderView/Feature/RDG 消费路径与默认模式相同，仅禁用 scene-data reuse；`Benchmark-ScenePublication.ps1 -PublicationMode full-rebuild` 可验证每帧 rebuild，删除环境变量即可恢复默认 versioned。结构性恢复则以阶段快照复制到新的空目录后重新配置构建；不得原地覆盖工作区或使用 destructive git reset。P4 checkpoint 的文件数与 manifest seal 在创建后补记于下方，封存后对本段补记和 tasks 勾选不重复做全仓哈希。

阶段恢复点 `20260830-p4-versioned-scene-publication` 已创建并在封存流程内验证 **986 个输入文件**；manifest seal 为 `1CEA72DF40F4BE06A2D94BB597C4BDEF3A962A58C487DBAC4A5FC4C640AA72DE`。快照包含 5.1–5.15 的生产代码、测试、脚本、规格与当时的验收文档，不包含 build、capture/cache、Git 或 Codex 元数据；本条 seal 补记与 tasks 的 5.16 勾选发生在封存后，不再次运行全文件验证。OpenSpec strict 在封存前后均通过。P4 完成，当前 **82/128**；下一项是 6.1 CMake 模块边界清单。

## P5 模块边界事实清单（6.1，2026-08-30）

新增 `cmake/PrismModuleBoundaries.json`，以机器可读形式记录生产源的唯一 owner、公开/内部头、直接 target 边、禁止边以及现有测试重编译例外。归属不依赖目录名粗判：`Core/Application` 与 ProcessDiagnostics 属于 PrismApplication，FileDialog 和两个尚在 RHI 路径下的 ImGui backend 属于 PrismEditor，BuildSymbolIdentity/MinidumpSymbolizer 属于 PrismDiagnostics。

轻量路径审计扫描 `src` 下全部 **217** 个 C/C++ 生产源，结果为 217 个均恰好一个 owner，重复和漏归属均为 0；JSON 解析通过。本项未编译、未运行 Demo/场景/GPU 验证，也未执行哈希。6.1 完成，当前 **83/128**；下一项是 6.2 PrismCore/PrismPlatform targets。

## P5 Core/Platform targets（6.2，2026-08-30）

新增 `PrismCoreSources.cmake` 和 `PrismPlatformSources.cmake`，建立真实的 PrismCore/PrismPlatform 静态库。CpuTrace、Environment、FrameTimer 只由 Core 生产；Window/WindowDiagnostics 只由 Platform 生产，FileDialog 继续归 Editor；RHI 通过 target edge 依赖 Core/Platform，Engine 显式依赖 Core。Renderer 和 Application 清单不再重复编译这些基础源。

windows-ci 重新 configure 成功，RelWithDebInfo 定向构建 PrismCore、PrismPlatform、PrismRHI、PrismEngine 和 PrismRender 均通过。未运行 Demo、GPU/场景矩阵、Fluid 或哈希验证。6.2 完成，当前 **84/128**；下一项是 6.3 PrismAsset target。

## P5 Asset target（6.3，2026-08-30）

新增 `PrismAssetSources.cmake` 和 PrismAsset 静态库，20 个 Asset 生产源由该 target 唯一生产。PrismAsset 显式链接 PrismCore、PrismRHI 与 PrismSlang，公开 JSON include，只向自身暴露 tinygltf include 与 glTF 编译开关；PrismRenderer 通过 PrismAsset target edge 使用资产，其 Ninja 对象规则中 Asset 源为 0。

windows-ci RelWithDebInfo 定向构建 PrismAsset、PrismRenderer、PrismRender、ShaderCompiler 和 AssetRuntimeCoordinator fixture 通过；`ShaderCompiler` 与 `AssetRuntimeCoordinator` 两项测试均通过。未运行 Demo、GPU/场景矩阵、Fluid 或哈希验证。6.3 完成，当前 **85/128**；下一项是 6.4 PrismScene target。

## P5 Scene target（6.4，2026-08-30）

新增 `PrismSceneSources.cmake` 和 PrismScene 静态库，SceneSession、Extractor、mailbox/frame packet、World bridge、序列化与场景工厂等 29 个源由 Scene target 唯一生产。依赖审计确认 Scene 没有反向 include Renderer；保留真实的 Core、Platform、Engine、Asset 和 RHI 直接边，没有为了形式分层改写 GPU-backed 场景工厂。Renderer 通过 PrismScene target edge 使用场景，其 Ninja 对象规则中 Scene 源为 0。

windows-ci RelWithDebInfo 定向构建 PrismScene、PrismRenderer、PrismRender 及三个场景 fixture 通过；`SceneSession`、`WorldRenderSceneBridge` 和 `RenderScenePublication` 均通过。未运行 Demo、GPU/场景矩阵、Fluid 或哈希验证。6.4 完成，当前 **86/128**；下一项是 6.5 PrismApplication target。

## P5 Application target（6.5，2026-08-30）

PrismApplication 现在独立生产 ApplicationHost/Launcher、命令行、capture/asset/runtime services、profiling 与当前平台的 ProcessDiagnostics。其显式链接 Core、Platform、Engine、Asset、Scene、RHI 和 Renderer；Windows 条件链接 Diagnostics，Editor 开关则条件链接 PrismEditor 并把 `PRISM_RENDER_HAS_EDITOR` 定义在真正编译 ApplicationHost 的 target 上。POSIX 仍由原清单条件选择 `ProcessDiagnosticsPosix.cpp`。

PrismRender 可执行文件名与 `main.cpp` 入口未变，它现在只编译 main 并链接 PrismApplication；Ninja 规则确认没有其他应用源再直接编入可执行文件。windows-ci RelWithDebInfo 的 PrismApplication/PrismRender 构建及 `ApplicationComposition`、`ApplicationCommandLine` 两项测试通过。未运行 Demo、GPU/场景矩阵、Fluid 或哈希验证。6.5 完成，当前 **87/128**；下一项是 6.6 UI backend 文件迁移。

## P5 Editor UI backend 归属（6.6，2026-08-30）

D3D12/Vulkan 的 ImGuiRenderer 文件分别迁入 `src/UI/Backends/D3D12` 和 `src/UI/Backends/Vulkan`，ImGuiFactory、自包含路径和 Editor 源清单已同步；类实现、后端调用、descriptor/sampler 与 draw-data 提交逻辑均未改。边界 JSON 移除了旧 RHI 路径特例，217 个生产源重新审计仍为零重复/零漏归属，RHI 中 UI include 为 0。

windows-ci 的 PrismEditor/PrismRender 构建通过，D3D12 与 Vulkan 各运行一次 headless preview 3 帧 UI/backend 冒烟并正常退出。windows-vulkan-only-ci 在 Editor=OFF/D3D12=OFF 下重新 configure，PrismRHI/PrismRender 构建通过，生成的 object/link 规则中 PrismEditor/ImGui backend 为 0。未运行其他 Demo、GPU validation/场景矩阵、Fluid 或哈希验证。6.6 完成，当前 **88/128**；下一项是 6.7 清理其余 target/源清单。

## P5 target/源清单收敛（6.7，2026-08-30）

PrismRendererSources 现在只列出 Renderer 自有文件，已删除 RHI/Asset/Scene/D3D12 的混合收集和顶层 `REMOVE_ITEM` 补丁。Renderer 只直接链接 Core/RHI/Asset/Scene；Editor 按实际 include 显式声明 Core/Platform/Engine/Asset/Scene/RHI/Renderer；直接包含 GLFW 的 RHI、Scene 和两个 GPU fixture 各自声明 GLFW，不再借 Renderer 的 PUBLIC 边传递。Slang 是 PrismAsset 的私有编译/链接依赖，既有 runtime 复制规则保留。

新增 PrismTools 和 PrismAutomation 共享库。Golden/Diagnostics 工具只编译各自 main 并链接 PrismTools；Harness/MCP 只编译各自 main 并链接 PrismAutomation；GoldenImage/EngineHarness 测试也改用 targets。生成图共 292 条生产对象规则，非 owner 重编译均能命中明示 fixture 例外，未声明项为 0。windows-ci RelWithDebInfo 的全目标增量编译/链接通过；其间发现并修复了 5.15 后 RenderFeatureLifecycle fixture 遗留的隐式 mutable scene view，现使用显式 control-view tag。`RenderFeatureLifecycle`、`GoldenImageMetrics`、`EngineHarness` 通过。未运行 Demo、GPU/场景矩阵、Fluid 或哈希验证。6.7 完成，当前 **89/128**；下一项是 6.8 边界检查器与正反例。

## P5 可执行模块边界门禁（6.8，2026-08-30）

新增 `cmake/CheckModuleBoundaries.cmake`，从机器清单核对生产源/头文件唯一 owner、公开与内部头跨模块使用、实际和声明的直接 target edge、禁止边、环以及 fixture 生产源重编译例外。严格生产审计覆盖 13 个模块和 218 个生产源并通过。审计由 `PRISM_RENDER_CHECK_MODULE_BOUNDARIES` 显式开启，默认配置保持关闭；轻量正反例始终注册为 CTest，避免每次本地 configure 重复进行完整源码扫描。

新增 `ModuleBoundaryTests.cmake` 及无编译器最小 fixture：合法 Editor native backend、显式测试重编译 2 个正例通过；RHI→Editor、Renderer→Application、内部头越界、target 环、缺失直接依赖、重复 owner、漏 owner 7 个负例都失败且命中预期诊断。边界落地同时把 FrameProfiler/SceneViewRefreshController 明确归属 PrismCore，并以 Core `DiagnosticLog` sink 解除 Renderer→Application 的 ProcessDiagnostics 反向依赖；渲染/海洋算法未改。

windows-ci RelWithDebInfo 定向构建 PrismCore、PrismRenderer、PrismApplication、PrismRender 与 RenderFeatureLifecycle fixture 通过；`ModuleBoundaries`、`RenderFeatureLifecycle` 通过，D3D12 headless preview 仅运行 3 帧并正常退出。未运行其他 Demo、GPU/场景矩阵、四个粒子 Fluid Demo 或哈希验证。6.8 完成，当前 **90/128**；下一项是 6.9 公开头 self-containment 与 presets/CI 接入。

## P5 公开头独立编译与 CI 接入（6.9，2026-08-30）

新增 `cmake/PrismPublicHeaderSelfContainment.cmake`：按模块清单找出当前配置实际存在 target 的 public headers，为每个头生成只包含该头的独立翻译单元，并继承 owner target 的公开 include、define 与依赖。`PrismPublicHeaderSelfContainment` 是默认构建目标，CTest 同名检查可在未先全量构建时补建；内部头不混入此契约。windows-ci 完整配置独立编译 132 个公开头，windows-vulkan-only-ci 在 D3D12/Editor/Harness 关闭时独立编译 124 个公开头，均通过。

该检查暴露并修复了一个原先被 Editor 传递依赖掩盖的问题：Application 的 WaterValidationSequence 直接包含 GLFW，但 Vulkan-only 配置没有声明自己的使用依赖；现在 PrismApplication 私有链接 GLFW，运行和水体算法未改。两个 Windows 配置的 `PublicHeaderSelfContainment` 与 `ModuleBoundaries` CTest 均通过，Editor-off 配置没有生成 Editor/Automation 公开头目标。

三个 CI presets 均显式开启生产模块边界和公开头检查；Windows workflow 已通过 presets 自动接入，Linux workflow 改用 `linux-vulkan-debug` configure/build/test preset，保留固定 Slang SDK 环境、Mesa/Xvfb 和原超时。既有 `prism_copy_slang_runtime` 及 Windows DXC/DXIL 必需文件探测/复制规则未改。Linux 实机编译属于 6.10 三组合验收，本项不以 Windows 结果代替。未运行 Demo、GPU/场景矩阵、四个粒子 Fluid Demo 或哈希验证。6.9 完成，当前 **91/128**；下一项是 6.10 P5 三构建组合验收。

## P5 三组合验收进行中（6.10，2026-08-30）

`windows-ci` 已以 preset 的严格边界/公开头开关完成 configure 和全目标 RelWithDebInfo build。CPU-safe 套件首次为 29/30：唯一失败是旧 CMake cache 选择 Windows PowerShell 5.1，而性能汇总脚本使用 PowerShell 7 Path API；测试发现逻辑已改为明确寻找 `pwsh`，该单项补测通过，因此本组合 30 项均有本轮通过结果。`windows-vulkan-only-ci` 同样完成严格 configure、全目标 build，并为 28/28 CPU-safe CTest 通过。完整配置 D3D12 preview 与 Vulkan-only 配置 Vulkan preview 各 headless 运行 3 帧并正常退出；未启用 GPU validation，未运行其他 Demo、场景矩阵或四个粒子 Fluid Demo。

本机 WSL 未安装任何 Linux 发行版，Docker/Podman 也不可用，因此无法执行 `linux-vulkan-debug` 的真实 configure/build/test。Linux GitHub workflow 已接入同名 preset，但工作流定义不能替代一次实际 Linux 结果。按 6.10 的显式验收规则，本项保持未勾选，P5 不宣称完成，当前仍为 **91/128**。
