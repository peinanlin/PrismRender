# P0 逐帧性能采样（2026-08-29）

对应 `refactor-renderer-architecture-boundaries` task 1.8。本文记录实际实现；完整两批性能基线和 P0 门禁是否通过，以末尾验收结果为准。没有拆分 Host 所有权、引入线程或改动渲染/模拟算法。

## 文件与数据流

新增：

- `src/Core/Application/FramePerformanceRecorder.{h,cpp}`：有界、默认关闭的逐帧 CPU/内存/资源诊断及 GPU 来源映射。
- `src/Core/Application/FramePerformanceSampling.{h,cpp}`：应用层连接 Renderer/RHI 只读统计，避免 recorder 依赖图形设备或持有 GPU 对象。
- `src/RHI/PipelineCreationStatistics.h`：成功创建图形/计算管线的设备生命周期计数。
- `tests/FramePerformanceTests.cpp`：CPU 计时累计、帧槽回绕、稀疏 Scene 刷新、重复/错误结果、提前退出和并发计数测试。
- `scripts/ArchitecturePerformance.Common.ps1`、`Measure-ArchitecturePerformance.ps1`、`Benchmark-ArchitecturePerformance.ps1`：原始数据验证、独立采样、两批运行与分布比较。
- `tests/scripts/ArchitecturePerformanceTests.ps1`、`ArchitecturePerformanceIntegrationTests.ps1`：统计/门限/来源/缺失数据及真实进程冲突配置负例。

修改：`ApplicationHost.cpp`、`SceneRenderer.{h,cpp}`、`IGraphicsDevice.h`、`D3D12GraphicsDevice.cpp`、`VulkanContext.cpp`、`cmake/PrismApplicationSources.cmake`、`cmake/PrismRhiSources.cmake`、`CMakeLists.txt`。`main.cpp`、assets/shader、Feature 算法、队列调度和 GPU 退役规则未修改。

数据流：Host 的既有 CPU 阶段 → scope elapsed time；Render 后读取旧帧槽已完成的 profiler 数据 → recorder 先映射旧 slot，再登记当前逻辑帧 → EndFrame 后采样资源/进程内存 → JSONL → CPU 工具核验帧身份和分布。报告没有 capture，不调用 `ResolveGpuTimings` 或新增 `WaitForGpu`。进程正常退出原有 GPU drain 保留。

## 测量定义与限制

- 默认不创建 recorder。只有 `PRISM_RENDER_FRAME_PERFORMANCE_PATH` 非空时启用，要求确定性时间和有限 `MAX_FRAMES`。与截图、多帧截图、CPU trace、逐帧图结构、GPU 单次报告、RDG 报告和水验证控制序列互斥，避免把不同诊断负担混在同一性能基线。原路径默认行为保持。
- `cpu.frameMs` 是当前循环开始到末尾采样前的墙钟耗时，不是 OS thread CPU 时间。包括现有 BeginFrame 等待、Renderer、UI 和 Present；不包括末尾进程内存查询/JSON 写出。`cpu.loopIntervalMs` 是相邻循环开始间隔，包含上一帧完整观察器开销。两项均作 V3 对照。
- `extractionMs` 累计 mailbox Publish/Acquire、Game view 构造、Scene debug copy/选择标注/Editor view 构造；不是 P4 的对象/字节复制计数，也不宣称静态零复制。UI build 包含当前 Editor 面板、统计拼装和导航处理；UI draw 单列。Game/Scene Render、BeginFrame、Present 分别记录。
- GPU generation 仅在对应 view profiler 内有意义，不等于 logical frame。录制器保留两个视图各最多 16 个 slot 的帧映射；Game 每帧和按需 Scene 的映射独立。每个新的 GPU 结果必须有唯一的历史来源；重复结果不重复计数，倒退/未知来源失败。最终未解析 tail 不补零、不冒充已完成；脚本要求被测区间的每帧/活跃 view 都有 GPU 结果。
- GPU `Renderer` 是既有视图渲染区间，非整个应用（不含 UI/Present）的 GPU 总时长；所有阶段单列。多队列阶段之和不作为 critical path，不由此推算 FPS。原始 timeline/calibration 信息保留。
- 进程内存：Windows `GetProcessMemoryInfo` 的 working set/private commit、进程 lifetime peak working set/commit。不是 GPU VRAM；lifetime peak 包含初始化，不能误称仅测量窗口峰值。退役 pending/high-watermark、累计 retired/reclaimed、upload staging、descriptor 水位另列。Linux 代码保留 getrusage peak RSS，未知当前/private 字段为 null；Linux 尚未实际验证。
- PSO 是两后端 `IGraphicsDevice` 入口创建成功的累计数，与 Renderer cache occupancy 分开；外部 ImGui 原生后端管线不经过该入口，明确排除。计数用 relaxed atomic，不承担 GPU 同步或资源回收职责。
- 记录器只保留当前帧数据和有界 slot 映射；JSONL 流式写出，不积累整场运行的 trace。CPU 分析工具离线读取原始数据。

## 可重放运行

单次采样（不代表 V3 完成）：

```powershell
./scripts/Measure-ArchitecturePerformance.ps1 -Backend d3d12 -Scene hpwater-ocean `
  -OutputDirectory artifacts/architecture-refactor/<new-run>
```

默认 60 帧进程内预热 + 180 被测帧 + 8 普通尾帧；尾帧仅使正常 slot reuse 回收计时，不增加等待。默认使用已有隐藏窗口，1280×800、native、Game、固定 1/60 输入、Demo 原默认画质、完整 Editor 构建；实际活跃 view 从报告核验。`-View scene` 沿用既有 Scene 刷新请求，非强制创造不存在的视图。no-Editor 用 `-BinaryPath` 显式选择。

两批入口：

```powershell
./scripts/Benchmark-ArchitecturePerformance.ps1 -Backends d3d12,vulkan `
  -Scenes preview,shadows,hpwater-ocean,pbf `
  -OutputDirectory artifacts/architecture-refactor/<new-benchmark>
```

每 case 每批先 3 次独立预热进程，再 5 次独立测量进程；每进程仍包含上述 60/180/8 帧。GPU 串行、共享 architecture lock。native/serial 可选；本入口不接受 auto，避免自适应策略/初始成本模型引入另一变量。每进程成本模型文件从不存在开始；原始 imgui.ini 从已封存初始快照复制。清理继承 PRISM 环境，记录实际输入、CPU/GPU/驱动/build/shader identity、二进制和源码 SHA、产物 SHA；不清除全局 OS/驱动 shader cache，按预热条件对照。

逐次报告保存 min/max/mean/median/p95（nearest rank）。批次保存 5 个 run median/p95，使用 median-of-run-medians、median-of-run-p95 汇总，保留全部逐帧原始数据；不把 900 个帧样本当作 900 次独立实验。两批 CPU frame/loop interval 和各 view GPU Renderer 区间按 5% median、10% p95 双向检查稳定性。未稳定时保留失败，不覆盖 golden、不自动放宽门限。单次 `measured-and-validated` 和批次 `baseline-repeatability-passed` 均不代表全 P0 或控制/视觉矩阵通过。

可用 `-WarmupFrames`（至少 60）和 `-SampleFrames`（至少 180）增加采样长度。复测保持 3 次独立预热、5 次独立测量和两批要求；较长区间是另一组明确输入，不能与旧区间逐帧混比。门限比较使用 `candidate <= reference * limit`，避免除法后减一把恰好 5%/10% 的边界误判为超限；没有增加容差。

## 验收结果

已完成的实现与回归（证据根为 `artifacts/architecture-refactor/20260828-hpwater-complete/P0/`）：

- CMake 4.1.2 / MSVC 195136248，两种 Windows RelWithDebInfo 构建通过。补齐 RHI header 源清单后两次 configure/build 均为 `ninja: no work to do`，二进制不变。Ninja 的 Host 依赖为 124 个有效条目，不是漏依赖的空记录。
- 完整二进制 SHA-256：`959A08724825536F9FD0AB9CEA98A15371CFC838D4C445A3D137FA26308D663B`；无 Editor：`23EA05CB886EC14459A6250867E7BA4F7A942F69FD542B1E9B63E0F5EB6E2A6D`。
- CPU：13/13、12/12，JUnit 为 `artifacts/architecture-refactor/performance-cpu-{full,no-editor}-01.xml`。GPU：18/18、9/9，分别在 `d3d12/performance-gpu-full-01/`、`vulkan/performance-gpu-no-editor-01/`；实际校验层图形测试为 17、8，另各有一项 VulkanRuntime，原 820 warning 仍单列。
- 默认关闭性能采样：两 API 各 20/20 Demo capture/strict，`{api}/performance-final-all-demos-native-01/`。对上一 `sequence-final-all-demos-native-01/` 的 V2 比较均 20/20，通过记录在 `comparisons/performance-final-all-demos-{api}-01/`；没有扩展批准的视觉修订或改变门限。
- HPWater 连续控制：两 API 各 330 帧、64 张控制点图像，`{api}/performance-water-controls-01/`；对 `sequence-water-controls-01/` 的 64 点图像/结构比较分别通过，`comparisons/performance-water-controls-{api}-01/`。这不是每帧都有截图，亦非性能采样数据。
- 三次真实性能短试跑：`performance-smoke-{d3d12,vulkan,no-editor}-01/`，均 10/12/8 帧，来源、尺寸、严格校验通过；每个被测区间 PSO graphics/compute 增量为 0。不将 12 帧试跑计入 V3。
- 两后端各 4 个真实进程负例：`performance-negative-{d3d12,vulkan}-01/`，分别拒绝 GPU 单次计时、CPU trace、RDG 报告冲突及非确定性输入；确认应用在预期边界失败，未生成伪完成性能文件。
- 性能工具 28 项测试：`artifacts/architecture-refactor/tool-tests/performance-c75223404783403bb5aecabca45e7c6b/tests.json`。覆盖正负分布、帧来源、缺失/重复样本、数据封存、硬件/窗口策略不匹配、两批汇总门限。前期 21/26 项通过记录保留。
- 对上一 853 文件快照核验，全部 60 个 assets 哈希未变；OpenSpec strict 通过。Linux 仍未提供执行环境。

最终工具测试为 31 项：`artifacts/architecture-refactor/tool-tests/performance-3eb3bc781ac7486a86807f6c71ea40dd/tests.json`，新增精确门限边界和 CLI 最小预热/测量区间负例。

### 两批长测未通过（保留失败）

`performance-native-baseline-01/` 的 128 次独立运行全部 `measured-and-validated`，包含 48 次进程预热和 80 次正式测量；每次 60/180/8 帧，共 31,744 个完整逻辑帧，正式测量共 14,400 帧。源码/二进制/实际尺寸/视图集合/计时归属/strict 均通过，但两批分布只有 2/8 组合通过，整批明确为 `failed`，不选取好看的样本替代。

| 组合 | 两批稳定性 | 超限示例 |
| --- | --- | --- |
| D3D12 preview | 失败 | CPU frame median +103.23%、p95 +86.11% |
| D3D12 shadows | 通过 | — |
| D3D12 HPWater | 失败 | GPU Game median 反向比较 +9.77%（第二批变快，仍非稳定基线） |
| D3D12 PBF | 失败 | GPU Game p95 +21.27% |
| Vulkan preview | 失败 | GPU Game p95 +24.93% |
| Vulkan shadows | 失败 | GPU Game median +7.62% |
| Vulkan HPWater | 通过 | — |
| Vulkan PBF | 失败 | CPU frame median +80.91%，GPU Game p95 +201.70% |

以上是同一二进制自身的跨批波动，不能称为架构改动引起的回退。D3D12 preview 的进一步定位：CPU frame 中位数 7.5019→15.2458 ms，BeginFrame 1.49795→9.90585 ms；extraction 0.0106→0.0104 ms、Game Render 2.5599→2.3691 ms、Scene Render 2.4658→2.20135 ms。Game GPU Renderer 区间 3.223744→1.100960 ms，但 Shadow/GBuffer 等具体 Pass 变化小。抽查 b1-r4、b2-r4、b2-r8 的被测区间，slot 0/1 各 90 帧、GPU 来源延迟均为 2 帧，未发现来源错配。b2-r8 的 CPU frame median 又回到 8.53985 ms，表明不是一项持续不变的代码开销。

现有 `D3D12Context::BeginFrame` 包含 per-frame fence 等待，`EndFrame` 仍是原有 `Present(1, 0)`；native 分支明确使用 `forced_native` 而非成本模型自动选择。呈现/提交节拍影响是待验证推断，不把它或某个外部程序写成已证明原因，也不修改 Present、等待、安全 fence、画质或门限来放行。

`performance-preview-long-retest-01/` 已完成 D3D12 preview 独立复测：每次 180 帧预热、900 帧测量、8 尾帧，每批 3+5 个独立进程、两批，共 16 次有效运行、17,408 个完整逻辑帧、9,000 个正式被测帧。仍未通过：CPU frame median 7.58165→15.2265 ms（+100.83%），p95 10.1819→16.6944 ms（+63.96%）；BeginFrame median 1.5327→9.8226 ms；Game GPU Renderer median 3.214496→1.037856 ms。第二批的五个 run median 为 7.52545、15.2265、15.2487、15.26285、8.09645 ms，再次出现两种运行节拍，不能视为已收敛。仅增加采样长度未解决问题。

两批证据的最终封存 SHA-256：

- `performance-native-baseline-01/index.json`：`DCB83F0443BA60B11F0AFFBF006D411C6AA8ED09ACDEF7842D0C732E61743E5C`。
- `performance-preview-long-retest-01/index.json`：`FCC6FDECFD154B9C1208E846748B5B8F6747AB75D8D9496801D7FDD5E3656854`。

复核 144 个子 index 及 1,296 个子产物 SHA 全部一致；所有采样子运行有效，两个 aggregate 均保持失败。最终旧工具 13+11+10、新性能工具 31 项测试通过，最新性能测试在 `artifacts/architecture-refactor/tool-tests/performance-0608e2fa6117468e84cfde7cc20602a5/`。一次复测期间的 `nvidia-smi` 只读快照为 RTX 5060 / P0 / 47% / 2812 MHz / 56°C；这不是先前失败时段的连续遥测，不能排除外部负载或证明具体原因。

当前未记录窗口位置、实际输出显示器/刷新节拍、Present 返回状态、BeginFrame 内 fence wait 与回收成本分解。这些是下一步应补齐的诊断与固定输入，不在缺乏证据时修改生产同步策略。task 1.8 / 1.9 保持未勾选，整体 **18/118**；未进入 P1。

恢复点 `20260829-p0-performance-sampling` 已保存并校验 **865 个文件**，manifest SHA-256 为 `84E8B14A5FCF04A68090D4855AB1F1650D7ECEE4E7A69ABE7B1F562BE1420885`。本恢复点说明及验收总表中的同一说明在封存后追加，生产/测试代码未变。旧快照不覆盖，恢复只复制到新的空目录，不自动覆盖工作区。

后续实现与新证据见 [帧等待/窗口/呈现诊断](ARCHITECTURE_FRAME_PACING.md)：已经补齐本节上述诊断缺口，并加入性能专用的一次性固定坐标。隐藏 D3D12 窗口实测返回 DXGI_STATUS_OCCLUDED；可见窗口固定条件的长测第一批完成，第二批因第 310 帧失去焦点被正确拒绝，仍未形成两批性能基线。旧 144 次证据不覆盖，也不倒推其未记录的呈现/焦点状态。新观测协议不得与旧协议当作等输入性能数据直接比较。
# P0 公平场景最终特征基线（2026-08-29）

任务 1.8 按修订后的 V3 完成。当前源码完整重建 `build-architecture-windows-ci/Release/PrismRender.exe` 与 `build-architecture-vulkan-ci/Release/PrismRender.exe`；未写机器级 Vulkan layer 注册、不锁 GPU 频率。固定协议为 Vulkan、Release、1280×800、strict validation、serial queue、basic profiler、30 帧预热、120 帧正式样本、8 帧解析尾段。`Benchmark-ArchitectureScenarios.ps1` 的六个场景独立运行三轮，共 18 个有效进程：

| 场景 | 三轮 FPS 范围 | 跨轮中位 FPS | Loop / Main active / Main wait | Game / Scene CPU | GPU critical hint |
| --- | ---: | ---: | --- | --- | --- |
| Empty Editor 双视图 | 343.83–395.99 | 373.41 | 2.67 / 1.95 / 0.44 ms | 0.71 / 0.55 ms | 0.24 ms |
| Empty Editor Game-only | 552.76–589.97 | 561.32 | 1.74 / 1.26 / 0.27 ms | 0.70 / — | 0.24 ms |
| Empty Standalone | 973.90–980.49 | 977.04 | 1.02 / 0.73 / 0.29 ms | 0.60 / — | 0.23 ms |
| Preview Editor 双视图 | 256.04–263.68 | 260.75 | 3.80 / 2.94 / 0.61 ms | 1.14 / 0.99 ms | 0.35 ms |
| Preview Editor Game-only | 396.45–435.41 | 414.63 | 2.41 / 1.82 / 0.29 ms | 1.16 / — | 0.34 ms |
| Preview Standalone | 531.07–570.39 | 556.14 | 1.80 / 1.19 / 0.48 ms | 1.03 / — | 0.34 ms |

证据目录为 `artifacts/architecture-refactor/p0-fair-scenarios-vulkan-release-r{1,2,3}-20260829/`。新增 `scripts/Summarize-ArchitecturePerformanceScenarios.ps1` 逐项拒绝 backend/build/validation/profiling level/queue/尺寸/场景覆盖/adapter/executable 不匹配，并汇总各轮中位，不选择最快批次。聚合报告为 `artifacts/architecture-refactor/p0-performance-scenario-aggregate-vulkan-release-20260829/report.json`，SHA-256 `7C8FB123327E4AA8F0EFAB7CA79F04F01106DF59D050DEF62632A30D003EAE98`。

同一 Release Editor 进程另运行相邻 `off:60,basic:60,detailed:60,capture:60` 的 240 帧 A/B，每段丢弃前 10 帧。Loop 中位分别为 3.0058、2.9364、3.1596、3.0859 ms；直接记账的 Profiler overhead 中位为 0.0002、0.0001、0.0002、0.0002 ms。由于四段有固定顺序且 GPU 采样能力不同，不能将 Loop 差全部归为 Profiler；直接 overhead 单列才是观察成本。报告 `p0-profiler-level-ab-vulkan-release-20260829/level-summary.json` SHA-256 为 `3F093728FAF13461C580C1E193E20404C71B9D110E79BF4AD6E3BCE267611170`。

内存、退役和 PSO 条件沿用详细 recorder 的独立 sidecar `20260828-hpwater-complete/P0/performance-native-baseline-01/vulkan-preview-b1-r4/summary.json`：工作集 median 627,525,632 bytes、private commit median 1,745,158,144 bytes、retirement pending median 0，测量期间 graphics/compute PSO 新建均为 0。该 sidecar 是 RelWithDebInfo/detailed 协议，只用于固定内存/PSO 条件，不与上述 Release/basic FPS 做数值回归。

结论：当前 Release 空负载已经达到或超过用户所述 Unity 空项目约 400 FPS 的同量级；Editor 双视图仍明显低于 Game-only/Standalone，原因是第二个完整视图和 Editor/Main 等成本。Preview 双视图 GPU hint 仅约 0.35 ms，Loop 约 3.80 ms，当前首先是 CPU/双视图/等待边界，不是单个延迟渲染 Pass 的 GPU 瓶颈。Render 仍为 Main 的内联子区间，Worker 正确显示 unavailable；后续 P7 才能用同一 Profiler 证明真实 Main/Render 重叠，不能提前以线程数量宣称收益。
