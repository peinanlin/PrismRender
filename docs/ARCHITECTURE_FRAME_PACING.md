# P0 帧等待与呈现条件诊断

## 2026-08-31 当前空场景 VSync 基线

`optimize-frame-pacing-and-present-mode` 以以下原始证据作为改动前基线：

- `artifacts/architecture-refactor/current-empty-threaded-query-20260831-01/editor-dual-view/frames.jsonl`
- `artifacts/architecture-refactor/current-empty-threaded-query-20260831-01/editor-game-only/frames.jsonl`

两项均为同一 Release D3D12、1280×800、validation off、native、`threaded+pool`、empty workload；帧 1–30 预热，31–150 为 120 个样本。旧协议使用 `pacingVersion=1`，尚无独立 display-admission 字段，因此同步显示的反压只可观察为后续 frame-resource fence 等待；该限制必须保留在基线解释中，不能把全部 fence 时间直接宣称为 GPU work。

下表统一为 120 帧中位数；FPS 为 `1000 / median loop ms`。

| Active views / mask | Loop ms / FPS | Main active / wait ms | Render active ms | GPU frame ms | Resource fence ms | Native Present ms | Display admission ms | Input-to-Present ms | Waiting/peak depth |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Game + Scene / 3 | 17.9573 / 55.69 | 0.3753 / 17.5326 | 2.6653 | 0.9677 | 15.0251 | 0.1125 | unavailable in V1 | 35.7939 | 1 / 1 |
| Game only / 1 | 17.7754 / 56.26 | 0.4449 / 17.1962 | 1.6628 | 0.7742 | 15.8174 | 0.1114 | unavailable in V1 | 35.1317 | 1 / 1 |

`activeViewPolicy` 和每帧 view 记录分别证明双视图与 Game-only；FPS 来自完整 loop interval，不来自 GPU pass 倒数。后续 V2 节拍协议必须新增 display admission、limiter、resource fence 和 outstanding depth，才能进行匹配 A/B。

## 2026-09-01 Frame Pacing Profile 实现与验收

当前实现不再把“关闭 VSync”作为单一优化开关，而是采用 Unity/UE 类似的分层策略：应用选择意图，RHI 将意图映射为后端能力，Profiler 同时记录 requested/effective 状态。默认仍是 `interactive-smooth`，不会改变普通启动行为。

| Profile | 语义 | D3D12 | Vulkan | 默认队列上限 |
| --- | --- | --- | --- | ---: |
| `interactive-smooth` | 平滑同步显示 | `Present(1,0)` + frame-latency waitable object | `FIFO` | 2 |
| `low-latency` | 同步但减少排队 | `Present(1,0)` + maximum latency 1 | `MAILBOX`，不可用时 `FIFO` | 1 |
| `benchmark` | 最大吞吐、允许撕裂 | `Present(0,ALLOW_TEARING)`，能力不足时回退 | `IMMEDIATE→MAILBOX→FIFO` | 2 |
| `custom` | 显式 presentation、FPS、队列深度 | 按 capability 产生 effective state | 按 present-mode capability 回退 | 1–8 |

帧准入发生在 `FrameTimer` 和输入采样之前。Main 只提交可靠的 `AdmitRenderFrameCommand` / `ChangeFramePacingCommand`，所有 DXGI/Vulkan 原生对象和等待都留在 Render execution lane。Render lane 在 Present 后预取下一帧 admission token；这样同步显示等待可以和下一帧前的 CPU 准备重叠，同时仍保留每个 frame-resource fence。target FPS limiter 使用单调时钟；Windows 使用高精度 waitable timer，最后仅有短 `yield`，hitch 后重置 deadline，不追赶历史欠帧。

Profiler 将等待拆成 `display-admission`、`frame-limiter`、`frame-fence`、`acquire`、`image-fence`、`native-present` 与 `queue-backpressure`，并值化 profile、requested/effective presentation、native mode、generation、swapchain image count、frame-resource slots、configured/effective queue depth、tearing/fallback。UI 只消费 feedback 快照，不跨线程读取 live backend。

### 真机结果与证据

固定机器为 RTX 5060、2560×1440 200 Hz 显示器，可见且 focused，`threaded+pool`。以下 FPS 都是完整 loop interval 的中位数，不是 GPU Pass 倒数：

| Backend / profile | Editor 双视图 FPS | Game-only FPS | Standalone FPS | 原生结果 |
| --- | ---: | ---: | ---: | --- |
| D3D12 interactive | 200.10 | 200.44 | 200.24 | sync=1、flags=0、queue=2 |
| D3D12 low-latency | 199.89 | 199.53 | 200.05 | sync=1、flags=0、queue=1 |
| D3D12 benchmark | 412.20 | 700.38 | 881.60 | sync=0、flags=512、tearing=true |
| Vulkan interactive | 202.09 | 201.61 | 316.94 | FIFO、queue=2 |
| Vulkan low-latency | 388.53 | 554.88 | 718.75 | MAILBOX、queue=1 |
| Vulkan benchmark | 429.83 | 726.80 | 881.76 | IMMEDIATE、queue=2 |

Custom limiter 的可见窗口样本为：60 FPS 实测 60.00、P99 17.6455 ms；120 FPS 实测 119.92/119.99、P99 9.7392/9.1537 ms；144 FPS 实测 144.01/144.07、P99 8.0685/7.9013 ms。隐藏窗口会受到 DWM 遮挡/节流影响，因此不与可见同步样本混合。

Low-latency queue 配对使用同一最终 D3D12 二进制、Game-only、30 warmup + 120 valid + 8 drain。configured queue 1 的 FPS 中位数 199.26、input-to-present 6.1496 ms、peak waiting depth 1；configured queue 2 为 200.57 FPS、6.2903 ms、peak depth 2。吞吐几乎不变而管线延迟略增。两组原生 DXGI maximum latency 都被 low-latency presentation intent 收紧为 1，证明应用 frame queue 与原生显示队列必须分开报告。证据为 `d3d12-custom-lowlat-queue1` 和 `d3d12-custom-lowlat-queue2`。

运行时确定性 smoke 使用仅在“deterministic + finite frames + performance report”下合法的 `PRISM_RENDER_FRAME_PACING_SEQUENCE` 与 `PRISM_RENDER_RESIZE_SEQUENCE`。D3D12 和 Vulkan 都在第 20/40/60 帧完成 generation 1→2→3→4，在第 30/50 帧完成 view epoch 1→2→3；D3D12 为 synchronized→low-latency-synchronized→immediate→synchronized，Vulkan 为 FIFO→MAILBOX→IMMEDIATE→FIFO，所有报告 `transitionPending=false`。

证据根：`artifacts/architecture-refactor/frame-pacing-20260901/`。最终短测包括 `d3d12-preview-transition-resize-final`、`d3d12-waveworks-transition-resize-v2`、`vulkan-runtime-transition-resize-v2` 与 `vulkan-waveworks-transition-resize`。WaveWorks 的首次 D3D12 resize 测试保留在 `d3d12-waveworks-transition-resize`：它暴露 placed transient resource 的 backing heap 先于 resource 释放；修复后两者进入同一 frame-retirement batch，并保证 resource 先释放。Vulkan 功能 smoke 因本机缺少请求的 validation layer 以 validation off 重跑；首次 `VK_ERROR_LAYER_NOT_PRESENT` 记录保留，不冒充验证层通过。

最终 D3D12 matched 矩阵由同一个 RelWithDebInfo 可执行文件完成，三组汇总内的 executable hash 均为 `45aaa84ad0846d17`。完整 Editor 构建仅在 deterministic + frame-profiler report 条件下接受 `PRISM_RENDER_PERFORMANCE_STANDALONE=1`，因此 standalone 场景关闭 Editor coordinator，但不更换二进制。汇总器现在显式保存 average、median、P95、P99、minimum 和 maximum。

| Profile / view | Avg FPS | Loop P95 / P99 ms | Main / Render / GPU avg ms | Display / resource-fence avg ms | Peak queue | Input-to-Present avg ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| interactive / dual | 199.82 | 5.514 / 5.666 | 2.999 / 2.256 / 0.907 | 2.042 / 0.0004 | 2 | 7.421 |
| interactive / game | 200.34 | 5.291 / 5.467 | 1.919 / 1.265 / 0.664 | 3.080 / 0.0004 | 2 | 6.346 |
| interactive / standalone | 200.17 | 5.178 / 5.332 | 1.696 / 1.395 / 0.584 | 3.303 / 0.0003 | 2 | 6.475 |
| low-latency / dual | 200.53 | 5.426 / 5.589 | 2.842 / 2.106 / 0.689 | 2.155 / 0.0004 | 1 | 7.186 |
| low-latency / game | 200.45 | 5.336 / 5.522 | 1.887 / 1.207 / 0.437 | 3.111 / 0.0006 | 1 | 6.297 |
| low-latency / standalone | 200.09 | 5.176 / 5.242 | 1.672 / 1.368 / 0.463 | 3.327 / 0.0003 | 1 | 6.455 |
| benchmark / dual | 403.68 | 3.075 / 3.255 | 2.509 / 2.021 / 0.751 | 0 / 0.0002 | 2 | 4.610 |
| benchmark / game | 689.19 | 1.895 / 2.183 | 1.482 / 1.027 / 0.511 | 0 / 0.0002 | 2 | 2.593 |
| benchmark / standalone | 806.57 | 3.665 / 4.786 | 1.471 / 1.025 / 0.661 | 0 / 0.2374 | 2 | 2.805 |

对应最终证据为 `d3d12-matched-interactive-final-v3`、`d3d12-matched-low-latency-final-v3`、`d3d12-matched-benchmark-final-v3`；9 个 footer 均为 `complete`，每个进程完成 158 帧并严格抽取 30 warmup 后的 120 帧。脚本同时验证 Editor 双视图 mask=3、Game-only/standalone mask=1 和 `editorEnabled` 身份。

最终门禁还修复了两处生命周期问题。首先，execution service 曾在局部 `FrameEnvelope` 析构前发布 completion，使调用方偶发观察到“完成但输入仍被持有”；现在在发布 acceptance ID 前释放 frame/UI 输入，RenderFrameQueue 在 Editor 连续 50 次、两个 no-Editor 配置各 20 次通过。其次，`d3d12-matched-benchmark-final-v2/empty-editor-game-only` 在退出时暴露 AssetRegistry 纹理晚于 D3D12 backend 析构；现改为 render lane 上 GPU drain、UI/renderer 释放、AssetRuntimeCoordinator 释放、backend 销毁的顺序。失败 crash/minidump 保留，定向复测 `d3d12-benchmark-game-shutdown-fix` 和完整 v3 矩阵均正常退出。

最终 shutdown 补测为 `d3d12-final-shutdown-validation-smoke`（D3D12 validation on）与 `vulkan-final-shutdown-smoke`（本机 validation layer 缺失，validation off）；两者均生成 measured-and-validated summary 并正常退出。最终定向 CTest 为完整 Editor 7/7、D3D12 no-Editor 8/8、Vulkan no-Editor 7/7。

D3D12 no-Editor 也不再是构建限制：`windows-d3d12-standalone-ci` preset 固定 D3D12 ON、Editor/Harness OFF，并启用模块边界和公开头检查。等价本地缓存配置已完成 381-step build；最终定向 CTest 为 8/8，包含 130 个公开头自包含和 13 模块/231 production sources 边界检查。新 preset 的空目录配置同样通过；本机首次联网 FetchContent 因 Git/MSYS 缺少 `basename/sed` 失败，指定现有 GLFW source cache 后完成配置，未将该工具链失败记成引擎通过或失败。

本项继续 `refactor-renderer-architecture-boundaries` 的 1.8；不是 P1 架构迁移，不改变 HPWater、shader、画质、队列策略或 GPU 安全等待。进度仍按完整验收判定，不能以诊断工具完成代替 P0 通过。

## 文件与数据流

新增文件：

- `src/RHI/FramePacingStatistics.h`：单帧有界统计、默认关闭的 CPU wall-time scope；无 GPU 对象、分配或额外等待。
- `src/Platform/WindowDiagnostics.h`、`src/Platform/WindowDiagnostics.cpp`：主线程只读窗口与 Win32 最近显示器查询，未知平台明确报告 unavailable。
- `src/Core/Application/PerformanceWindowPosition.h`：性能测试专用坐标解析，默认不改变普通窗口位置。
- `docs/ARCHITECTURE_FRAME_PACING.md`：本说明及证据。

修改文件：`src/RHI/IFrameContext.h`、`src/RHI/D3D12/D3D12Context.cpp`、`src/RHI/Vulkan/VulkanContext.h/.cpp`、`src/RHI/Vulkan/VulkanSwapChain.cpp`、`src/Platform/Window.h/.cpp`、`src/Core/ApplicationHost.cpp`、`src/Core/Application/FramePerformanceSampling.h/.cpp`、`cmake/PrismRhiSources.cmake`、`cmake/PrismApplicationSources.cmake`、`scripts/ArchitecturePerformance.Common.ps1`、`scripts/Measure-ArchitecturePerformance.ps1`、`scripts/Benchmark-ArchitecturePerformance.ps1`、`tests/FramePerformanceTests.cpp`、`tests/scripts/ArchitecturePerformanceTests.ps1`、`tests/scripts/ArchitecturePerformanceIntegrationTests.ps1`；另更新原性能/验收文档和 tasks 的实际进展记录。

数据流：Host 创建 opt-in recorder → 在 frame owner 启用统计 → 后端在原 BeginFrame/EndFrame 边界观测 → 应用取得只读值并查询窗口 → 原 JSONL 的 `resources.pacing/window` → 严格校验及分布汇总。没有全局 recorder、GPU 查询或 UI 回调注入后端。`main.cpp` 不变。

## 定义和兼容边界

- `frameFenceMs` 包含原帧槽 fence 等待调用；D3D12 同时记录 target、调用前 completed 值和是否进入事件等待。Vulkan 的原 `vkWaitForFences` 总是被调用，不以调用次数判断是否真正阻塞。
- `reclaimMs` 为原描述符、退役对象及暂存命令/上传资源回收；Vulkan 也包含紧邻的原批次状态清理。
- `acquireMs`、`imageFenceMs` 分别为 Vulkan acquire 和交换链 image fence 区间；D3D12 对应项为零而非额外调用。
- `uploadMs`、`prepareMs` 为原上传刷新与命令准备；`submitMs` 只统计 EndFrame 最后的原生提交，不声称涵盖 RenderGraph 的全部提交。
- `nativePresentMs` 只测原生 Present 调用；Host 原 `presentMs` 仍覆盖整个 EndFrame。`signalMs` 是 D3D12 呈现后的原 fence signal。它们全是 CPU wall time，不是 GPU 时长、显示完成或输入延迟。
- D3D12 仍调用 `Present(1, 0)`，保留原错误处理；Vulkan 记录实际创建时的 `VkPresentModeKHR`，不改变模式选择、信号量或提交顺序。诊断读取不到的 scan-out/VRR/DWM 状态不作推断。
- 窗口每帧记录位置、逻辑/像素尺寸、可见/焦点/最小化、内容缩放、最近显示器名/范围/当前整数刷新率。最近显示器不是实际扫描输出证明；整数刷新率也不等于实际每帧呈现间隔。
- `observer.windowMs` 单列查询/JSON 组装开销；它计入 CPU frame/loopInterval，不扣除或伪装成零成本。后端计时 scope 的微小开销同样保留。
- `pacingVersion=1` 是 header 的附加观测协议。旧 JSONL 仍可读，但禁止和新协议当作等输入性能对照。没有更改旧字段语义或图像门限。

## 固定输入与验证

仅有 `PRISM_RENDER_FRAME_PERFORMANCE_PATH` 时启用统计。`PRISM_RENDER_PERFORMANCE_WINDOW_X/Y` 必须成对、为 [-32768,32767] 的完整整数；只在性能路径使用。测量脚本默认请求 (100,100)，逐帧核实实际值，不能把 OS 调整后的窗口当作请求成功。

```powershell
./scripts/Measure-ArchitecturePerformance.ps1 -Backend d3d12 -Scene preview -VisibleWindow -WindowX 100 -WindowY 100 -OutputDirectory artifacts/architecture-refactor/pacing-example
./scripts/Benchmark-ArchitecturePerformance.ps1 -Backends d3d12,vulkan -Scenes preview -VisibleWindow -WindowX 100 -WindowY 100 -OutputDirectory artifacts/architecture-refactor/pacing-batches
```

`-VisibleWindow` 是测试显式配置，普通应用默认设置不变；旧隐藏测量入口保留用于诊断，但 D3D12 若返回遮挡状态会失败。观测结果不符合正常 Present、acquire、wait，缺字段、过期代次/错误 slot、测量区间内显示条件变化、跨运行条件不一致均拒绝，不静默过滤这些帧或放宽 V3。原始数据和失败 index 保留。

验证方法：C++ 覆盖默认/关闭 scope、重复 End、帧槽回绕/清空与坐标解析；PowerShell 覆盖旧协议兼容、新协议两 API 正例和遮挡、suboptimal、缺失/非法字段、刷新率/位置变化等负例；真实进程验证非法配置；双构建、CPU/GPU suites、全部 Demo 图像与 HPWater 连续控制点比较；性能进程串行，两批正常可见呈现定向复测。

## 初步实测（不代表稳定性通过）

证据根为 `artifacts/architecture-refactor/20260828-hpwater-complete/P0/`。

- `pacing-smoke-d3d12-01/`：隐藏窗口，原始 JSONL 完成 30 帧，采样校验明确失败，Present 返回十进制 142213121（0x087A0001，Windows SDK `winerror.h` 定义为 `DXGI_STATUS_OCCLUDED`）。显示器 `DISPLAY1`，2560×1440、200 Hz。旧报告缺少此字段，不能倒推旧 144 次运行的全部状态或宣称已经找到所有波动原因。
- `pacing-visible-d3d12-01/`：可见窗口，60/180/8 帧，正常 Present、输入/完整性检查通过。CPU frame median 11.2358 ms；帧 fence 3.72625 ms、回收 1.3729 ms、原生 Present 0.0603 ms、窗口观测 0.0292 ms。只是一次运行，不是两批 V3 通过。此时尚未固定坐标，不混入最终固定坐标批次。
- 两次初试二进制早于最终坐标解析修正；各自 index/source-inputs 保存其真实身份，不冒充最终版本回归。

`pacing-visible-preview-baseline-01/` 的首次预热和 `pacing-position-d3d12-02/` 保留为实际坐标不匹配的失败。即时 readback 能读到请求坐标，但第一次事件处理后又变回初始位置；未证明是哪一个 OS 消息造成。最终将一次性测试坐标应用放到首次 PollEvents 之后，不调整普通窗口事件顺序、不每帧重置位置。`pacing-position-d3d12-03/` 的 10/12/8 短测通过，实际 (100,100)、200 Hz、可见且正常 Present；不计入 V3。

最终代码两构建通过，CPU 13/13、12/12（`artifacts/architecture-refactor/pacing-cpu-{full,no-editor}-02.xml`）。52 项性能工具正反例通过（`tool-tests/performance-40e4f61a106f4fce8004a76d814b8a9c/`）；最终全矩阵仍在执行。最终完整构建 SHA-256 `D29E8FDEDCFF36FBBC89F59A64AEA9E8FBC78AA8614BEB81063919A92B032CE7`，无 Editor 构建 `1296B777BAEB4B8035F178E439DA3A02A3E3E4ADBDA8B571CE8519E7C7AA7FBA`。全部 60 个 assets 对上一 865 文件恢复点哈希不变。

构建期间修正了新代码的 API 枚举名和 Windows/GLFW include 顺序；两个构建目录的 CMake 本地化 includes 前缀恢复为实际编译器输出，最终 Host 依赖记录 126 项 VALID。没有修改编译器版本或依赖包。

## 最终功能回归

- GPU：`P0/d3d12/pacing-gpu-full-01/` 18/18，`P0/vulkan/pacing-gpu-no-editor-01/` 9/9；strict 图形测试 17+8，另各 1 项 VulkanRuntime。GPU 锁串行执行，未关闭校验层。
- 全部 Demo：`P0/{d3d12,vulkan}/pacing-final-all-demos-native-01/` 各 20/20 capture/strict；`P0/comparisons/pacing-final-all-demos-{d3d12,vulkan}-01/` 对上一 `performance-final-all-demos-native-01/` 全部原 V2 通过。
- HPWater：`P0/{d3d12,vulkan}/pacing-water-controls-01/` 各 330 帧/64 图；`P0/comparisons/pacing-water-controls-{d3d12,vulkan}-01/` 对上一轮序列的图像和结构比较全部通过。
- 真实配置负例：`P0/pacing-negative-{d3d12,vulkan}-02/`，两后端各 7/7；包含三种坐标错误及原四种冲突/非确定性输入。早期 D3D12 `-01` 的 7 项记录保留，不混淆二进制身份。
- 最终工具 13+11+10+52 项通过，路径依次为 `tool-tests/1e4dadd3cc4146c79e4d61a57687d8fe/`、`tool-tests/baselines-df570e0a876a48dba822a64df2482d77/`、`tool-tests/sequences-613a6172b3f64ba9b1f0fdd61f0f5101/`、`tool-tests/performance-c43d9a37c52c437599224758b5237dd5/`。旧工具首次因 GPU 锁被占用而没有触及预期子进程失败边界，失败记录 `tool-tests/9d879fa1beb147a89f7855f427c6fe0d/` 保留；释放锁后完整重跑通过，不将锁冲突当作负例成功。
- OpenSpec strict 通过。Windows 两构建 Host 的最终依赖记录分别为 126/119 项 VALID；Linux 仍无执行环境。

## 长测停止于焦点变化（不通过 P0）

`pacing-no-editor-vulkan-01/`：最终无 Editor 二进制、可见 (100,100)、60/180/8 帧，采样/严格校验通过；它是单次路径验证，不是两批基线。index SHA-256 为 `341B735669CD57DAA63FD7DDD13FEAA3086EA499BAA1313A65CC09E187F45FED`。

`pacing-visible-preview-baseline-02/` 请求双 API preview、两批每批 3 次独立预热+5 次正式测量，每次 180/900/8 帧，native、完整 Editor/双视图、1280×800、可见 (100,100)。D3D12 为原 SyncInterval=1，Vulkan 实际 presentMode=1、3 张交换链图像；两者最近显示器均为 DISPLAY1、2560×1440、200 Hz。

第一批双 API 全部完成（16 次运行，其中 10 次正式测量、共 9,000 个有效正式被测帧）。第二批 D3D12 第一次预热有效，第二次预热在样本校验时失败：第 310 帧 `focused` 从 true 变为 false，其他固定条件未变化，原始进程仍正常完成 1,088 帧。工具正确拒绝该运行；没有过滤失焦区间、重新聚合好样本、抢回焦点、改 Present 或放宽门限。

这轮不是“V3 分布比较超限”，而是没有完成同条件的第二批，因此没有合法的两批比较结论。总计 18 个已执行进程、17 个有效子运行；第二批正式测量尚未开始，剩余运行未执行。整体 aggregate 保持 `failed`。

失焦运行的辅助定位（不同长度区间，只作描述，不是独立重复实验或因果证明）：被测帧 181–309 共 129 帧有焦点，310–1080 共 771 帧无焦点；CPU frame median 为 8.0981/8.4932 ms，BeginFrame 为 1.5567/1.6300 ms，frame fence median 均 0.0004 ms、p95 为 0.0010/0.8652 ms。相应 Game GPU Renderer median 为 2.497184/3.221632 ms。不能据此宣称失焦解释了此前所有 7.58→15.23 ms 的双峰，也不能确认焦点变化由哪个程序或操作造成。

证据封存复核：18 个子运行的 161 个产物 SHA 全部一致；aggregate 已登记的 17 个有效子 index 及两份第一批汇总全部核验。失败的第 18 个子 index 没有被原 batch driver 登记为成功，单独封存如下：

- aggregate `pacing-visible-preview-baseline-02/index.json`：`590DCB2581AF4A0907E38232860926CC952B59EBFAA3FB43F55E3186676E097B`。
- 失败子运行 `d3d12-preview-b2-r2/index.json`：`FAF1477CE3DAF2973476BD013ADDF9EA939EF51070036680BC2D1B2A8609D7DA`。

当前交付：默认关闭的细分诊断、严格条件校验、测试专用一次性定位，以及通过的功能/图像回归。**1.8、1.9 仍未完成，18/118，未进入 P1。** 下一轮需有焦点稳定的前台测试时段；若改用无焦点窗口策略，应显式确定新配置并重新建立两批基线，不能继承前台样本或自动改变用户桌面焦点。其余 shadows/HPWater/PBF 性能组合仍需按新观测协议独立完成。

恢复点 `20260829-p0-frame-pacing` 已保存并重复校验 **870 文件**；manifest SHA-256 `FBB40823726D83A193B514666E5966A71065EB636D423D34D81F2AAD938FE952`。本条与验收总表中的恢复点说明在封存后追加；生产/测试代码未变，454 项生产输入仍与最后有效运行的 source-inputs 一致。旧快照、图像和失败报告保留；恢复仅复制到新的空目录，不覆盖工作区。

## 离线整批核验入口

新增 `scripts/Summarize-ArchitecturePerformanceBaseline.ps1`、`scripts/ArchitecturePerformanceReport.Common.ps1` 和 `tests/scripts/ArchitecturePerformanceReportTests.ps1`。它们不改采样器、引擎、原始证据或 V3 算法，不启动 GPU 进程。数据流为已结束的 batch index → 子产物 SHA/身份核验 → 原 JSONL 重算 → 两批分布及双向原门限 → 独立报告。

```powershell
./scripts/Summarize-ArchitecturePerformanceBaseline.ps1 -InputDirectory artifacts/architecture-refactor/20260828-hpwater-complete/P0/pacing-visible-preview-baseline-03 -OutputDirectory artifacts/architecture-refactor/20260828-hpwater-complete/P0/pacing-preview-audit-03 -Enforce
./tests/scripts/ArchitecturePerformanceReportTests.ps1
```

请在性能采样结束后运行离线核验，避免 CPU/磁盘负载影响测量。仅接受已结束的批次；核验批次登记的成功子 index、全部存在的预期子目录（含未登记的失败进程）、子产物 SHA、运行配置/源码/二进制、重算的批次汇总和门限。旧 batch 未登记的失败子 index 在报告中单独记录 SHA，不重新归类为成功。报告的 `verifiedArtifacts` 包括根目录批次汇总及所有子产物，不包含 index 本身，index SHA 单列。

报告保留各正式运行的 median/p95 数组，列出所有指标的绝对差与相对差；参考为零时相对差为 null，不作除零或制造无穷大。失败子运行另检查测量区间的窗口条件，记录变化帧、变化字段及前后值；最多保存 64 个例子，完整变化次数保留。这是描述性定位，不证明性能差异的原因，也不替代原采样合法性检查。原始截断 JSONL 可记录诊断错误，但该运行仍为失败。

默认命令返回“报告生成成功”，不等于验收通过；自动验收须带 `-Enforce`，不完整/超限时先保留报告再非零退出。产物被篡改或汇总与原始数据矛盾直接失败，不生成可信报告；输出不能写到原基线目录或覆盖既有报告。测试覆盖完整/不完整、双向超限、成功标记伪造、丢失预热/比较、路径越界、哈希/汇总/身份篡改、零值和焦点/遮挡诊断。原始 JSONL 解析复用已有性能工具，单测隔离整批规则，真实证据另外端到端验证。

## 再次复测及离线工具验收（2026-08-29）

用户确认继续后，独立重跑 `P0/pacing-visible-preview-baseline-03/`。配置仍为两个 API 的 preview、native、完整 Editor/双视图、可见 (100,100)、1280×800、200 Hz、180/900/8 帧、每批 3+5 个进程；没有复用上次的第一批作为本次测量。第一批两 API 的 16 次运行完成，第二批 D3D12 r1 有效，r2 在第 **682 帧**焦点 true→false，被原采样条件校验拒绝。相邻 681/682 帧及整段诊断证明只有 `focused` 改变，位置/尺寸/刷新率/可见性/正常 Present 条件未变；不能确定失焦来源。原进程完成 1,088 帧，测量区间仍有全部 900 行，不截取焦点正常的子区间冒充有效运行。

本次 **18 个已执行进程、17 个登记有效进程、1 个失败进程、14 个未执行进程**；正式测量仅第一批的 10 次/9,000 帧。两批性能门禁未形成完整结论，不能宣称稳定或超限通过。D3D12 第一批 r7 是有效但较慢的运行：CPU frame median 10.4183 ms、BeginFrame 4.5341 ms、帧 fence 3.07225 ms；其他四个正式运行的 CPU median 为 7.5201–7.6099 ms。所有值都保留在原分布数组中，不删除离群运行，也不把不同指标的中位数相加当作同一帧分解。

本轮仅新增上述三个离线脚本/测试文件，并更新本文、验收总表和任务进展。**454 项生产输入与冻结测量清单逐项一致**，完整构建二进制仍为 `D29E8FDEDCFF36FBBC89F59A64AEA9E8FBC78AA8614BEB81063919A92B032CE7`。没有修改采样/呈现代码、HPWater、shader、Demo、画质或门限；沿用同一版本已通过的功能/图像验证，不声称本轮重新执行这些 GPU/图像套件。

离线验证结果：

- 最终新工具 **26/26**（包括 CLI 覆盖保护），证据 `artifacts/architecture-refactor/tool-tests/performance-report-4dff5da8f8024a82a1450d7ad464eae5/`；前一轮 23 项记录也保留。既有性能工具 **52/52**，证据 `tool-tests/performance-efda0f86de4b456d962c1819a13c92f8/`。
- 本轮真实批次审计 `P0/pacing-preview-audit-03/report.json`：163 个产物（161 个子产物+2 份根汇总）、17 个登记 index 及失败子 index 完整；自动定位第 682 帧唯一焦点变化。`complete=false, passed=false`，`-Enforce` 按预期非零退出且保留报告。
- 上轮中断批次审计 `P0/pacing-preview-audit-02/report.json`：同样 163 个产物通过，准确复现第 310 帧焦点变化；不把旧失败改成成功。
- 旧协议完整长测审计 `P0/pacing-legacy-complete-audit-01/report.json`：16 次/9,000 正式帧、146 个产物通过，`complete=true, passed=false`；重算 CPU 7.58165→15.2265 ms、BeginFrame 1.5327→9.8226 ms，与旧结论一致。`-Enforce` 正确拒绝完整但超限的批次。旧协议不与新协议混合比较。
- OpenSpec strict 通过；本轮无新增 CPU/C++ 或 GPU 实现，不重建已冻结二进制。

封存 SHA-256：

| 文件（相对 P0） | SHA-256 |
| --- | --- |
| `pacing-visible-preview-baseline-03/index.json` | `AE53B60D70B99B52AA7BB87563E6171518693433076A1D2883B08A978C316DF1` |
| `pacing-visible-preview-baseline-03/d3d12-preview-b2-r2/index.json` | `42BFF447105B77F6A3249148D64F32CAA57194BA3BC102744499D0C91F2907F6` |
| `pacing-preview-audit-03/report.json` | `A7FE4C1A3269F527926064697A1898C6BD6A5E733C6C23C3BF744B1D19D6BF5B` |
| `pacing-preview-audit-02/report.json` | `36E14B59B20AD1B526AACAC1FC07F7C4D9F8EC8B707F23CB7C9D1E8C608DBE4F` |
| `pacing-legacy-complete-audit-01/report.json` | `EA6B6112EC676FA3AA68B57131D17B38C221F7C50D433741E92BD0E6B153FC77` |

**当前仍为 18/118，1.8/1.9 未完成。** 按 openspec-apply-change 停在 P0，不进入 P1。前台焦点稳定性连续两轮无法满足；下一步需明确是否采用测试专用固定无焦点窗口配置（仍检查可见性、遮挡和原生呈现成功），并完全重建独立两批数据。该策略尚未实施或默认批准，不会自动抢焦点、过滤失焦帧或放宽门限。

本轮恢复点 `20260829-p0-performance-audit`：**873 文件**，创建及重复校验通过，manifest SHA-256 `BF8BA129C6888AD4AADAE477D4339426889DDF67D082B17DD1A19F4236A7B96C`。与上一 870 文件恢复点相比仅新增三个离线脚本/测试、修改本文/验收总表/tasks；源码与资产不变。本条及验收总表的恢复点哈希在封存后追加。旧恢复点不覆盖，恢复只复制到新的空目录。

## 已批准的测试专用无焦点模式

用户随后明确同意此模式；上节“尚待批准”属于当时状态。新增 `src/Core/Application/PerformanceWindowFocus.h`，修改 `src/Platform/Window.h/.cpp`、`src/Core/ApplicationHost.cpp`、`cmake/PrismApplicationSources.cmake`、`tests/FramePerformanceTests.cpp`、`scripts/Measure-ArchitecturePerformance.ps1`、`scripts/Benchmark-ArchitecturePerformance.ps1`、`scripts/ArchitecturePerformance.Common.ps1`、`tests/scripts/ArchitecturePerformanceTests.ps1`、`tests/scripts/ArchitecturePerformanceIntegrationTests.ps1`；另更新本文/验收总表/tasks。

`PRISM_RENDER_PERFORMANCE_WINDOW_FOCUS=unfocused` 只允许在显式性能采样且非 headless 的配置中使用。省略仍为普通默认；显式值只接受 `default` / `unfocused`，非法值或非性能运行使用此参数在窗口/设备创建前失败。应用解析策略并通过窄 `WindowFocusPolicy` 构造参数传给 Platform；Platform 不读取性能专用环境变量，RHI 与 Renderer 不参与此策略。

Window 创建时为新模式设置 `GLFW_FOCUSED=false`、`GLFW_FOCUS_ON_SHOW=false`；普通路径显式恢复两项默认 true，避免 GLFW 全局 hint 残留。它只禁止创建/显示时主动聚焦，不锁定桌面输入、不回抢焦点、不隐藏鼠标、不改变窗口 z-order 策略；用户点击或系统行为仍可赋予焦点。实际 GLFW 行为已对照仓库构建所用 `glfw3.h`、`window.c`、`win32_window.c`，不升级依赖。

Recorder header 增加 `metadata.windowFocusPolicy`，从窗口保存的创建策略取得，不在主循环重新读环境。旧报告缺字段时按 `default` 解释；新 summary 同样保存策略。校验将请求环境、原始 header、实际 measured 帧与 summary 对齐；`unfocused` 要求每个 measured 帧 `visible=true, focused=false`，仍执行所有原有位置/尺寸/显示/原生返回值及代次检查。全程有焦点也会拒绝，不仅检查焦点是否变化。新策略与旧策略禁止混合进行等输入比较，未放宽 5%/10% 门限，也不删除任何失败帧。

```powershell
./scripts/Measure-ArchitecturePerformance.ps1 -Backend d3d12 -Scene preview -VisibleWindow -UnfocusedWindow -OutputDirectory artifacts/architecture-refactor/unfocused-example
./scripts/Benchmark-ArchitecturePerformance.ps1 -Backends d3d12,vulkan -Scenes preview -VisibleWindow -UnfocusedWindow -WarmupFrames 180 -SampleFrames 900 -OutputDirectory artifacts/architecture-refactor/unfocused-two-batches
```

两个开关必须同时使用；默认的隐藏/普通窗口入口均保持。测试期间不要点击或最小化测试窗口；其他应用遮挡或改变显示状态仍可能使测量无效，本模式不保证稳定性门禁必然通过。

验证方法：纯 C++ 策略默认/非法/缺采样/隐藏冲突测试；工具覆盖双 API 无焦点正例、全程错误焦点、隐藏、未知策略、缺观测、跨策略比较和伪造请求；真实进程三种新增错误应在窗口/设备前失败，不能伪称启用了 GPU 校验。双构建及原 CPU/GPU suites、20 Demo 默认图像和 HPWater 连续控制点回归，随后冻结新二进制运行独立两批。

初步结果：两构建通过；CPU 13/13、12/12（`artifacts/architecture-refactor/unfocused-cpu-{full,no-editor}-01.xml`）。完整构建最初生成的中文 MSVC include 前缀不匹配，重新 configure 并仅触发受影响源重新编译后恢复，Host 依赖分别 127/120 项 VALID；触碰时间戳没有改变其他源内容。最终完整二进制 SHA `D188A40A34770B570F97834A7EB9735A2FCB4AD007F61966DB77326BCDA03902`，无 Editor SHA `7A688B7ED60DF501D2CBBD682B177F77720068AE0A09E51D109DADBF8E1C43E4`。两 API 的 `P0/unfocused-smoke-{api}-01/` 均通过 60/180/8 帧短测：可见 (100,100)、1280×800、DISPLAY1 200 Hz、focused=false，原 D3D12 syncInterval=1 / Vulkan presentMode=1 不变。完整功能与两批基线验收仍需下面的最终结果，不以短测代替。

新模式最终功能回归（同一冻结二进制）：

- `P0/d3d12/unfocused-gpu-full-01/` **18/18**、`P0/vulkan/unfocused-gpu-no-editor-01/` **9/9**；strict 图形 17+8，另各 1 项 VulkanRuntime 单列，校验未关闭。
- `P0/{api}/unfocused-final-all-demos-native-01/` 是新版本的**普通默认窗口**回归，不是无焦点性能运行。两 API 各 **20/20** capture/strict，对 `pacing-final-all-demos-native-01/` 原 V2 图像比较全部通过，报告在 `P0/comparisons/unfocused-final-all-demos-{api}-01/`。
- `P0/{api}/unfocused-water-controls-01/` 各 **330 帧/64 图**，对 `pacing-water-controls-01/` 的全部图像和结构检查通过；比较目录 `P0/comparisons/unfocused-water-controls-{api}-01/`。旧序列、默认设置及图像门限未改。
- `P0/unfocused-negative-{api}-01/` 各 **10/10** 真实进程负例。原 7 项在启用校验的启动流程中失败；新增 3 项是窗口/设备前参数检查，index 明确区分 boundary，不声称它们启用了 GPU 层。无 Editor 的新模式 `P0/unfocused-no-editor-vulkan-01/` 60/180/8 帧通过、实际仅 game view，不当作两批性能基线。
- 最终工具 **13+11+10+64+26** 项通过。对应 `artifacts/architecture-refactor/tool-tests/` 下 `db22491d15ec41f2ac2eaf5cfb103d28/`、`baselines-8049a11f567246e99ba4d646ea9b3bd7/`、`sequences-8e37cdb6cc464784ab72ff23638c8c0e/`、`performance-f82d06a8f7cb44ce95341f5ff96f8a88/`、`performance-report-34bd607c393044a8b907fea1e1d66f70/`。OpenSpec strict 通过。
- 与上一 873 文件恢复点对照，已有生产输入中只有 Window.h/.cpp、ApplicationHost.cpp、PrismApplicationSources.cmake 内容改变，另新增纯参数解析头；Renderer/RHI 实现和所有 assets 内容未变。全矩阵回归已实际执行，仍不宣称后续 Linux 验收完成。

功能通过后冻结生产输入、二进制及采样工具，再单独启动 `P0/unfocused-preview-baseline-01/`：两个 API、native、preview、完整 Editor/双视图、180/900/8 帧、每批 3 次预热+5 次独立正式进程。新目录与旧前台数据完全隔离，结果必须在完整结束后按原 V3 判定。

## 无焦点两批长测最终结果（2026-08-29）

上述两批 **32/32 个进程全部完成且采样有效**，其中 20 个正式运行、18,000 个正式被测帧；另 12 个独立预热进程不进入正式分布。所有 measured 帧保持 visible=true、focused=false、(100,100)、1280×800、DISPLAY1 200 Hz，D3D12 原 SyncInterval=1/flags=0/2 张交换链图像，Vulkan 原 presentMode=1/3 张图像。没有截取、丢帧、重跑替换或使用旧前台样本。

| 同配置两批比较 | 第一批 median / p95（ms） | 第二批 median / p95（ms） | 原 V3 双向结果 |
| --- | --- | --- | --- |
| D3D12 CPU frame | 7.17735 / 8.1376 | 7.25545 / 8.1878 | 通过 |
| D3D12 CPU loop | 7.29955 / 8.2496 | 7.38425 / 8.3159 | 通过 |
| D3D12 Game GPU Renderer | 2.32949 / 2.82707 | 2.37698 / 2.83904 | 通过 |
| D3D12 Scene GPU Renderer | 1.13646 / 1.87658 | 0.84365 / 1.24525 | **失败** |
| Vulkan CPU frame | 5.74285 / 6.6568 | 5.76965 / 6.7053 | 通过 |
| Vulkan CPU loop | 5.86785 / 6.7850 | 5.90210 / 6.8588 | 通过 |
| Vulkan Game GPU Renderer | 1.87958 / 2.29744 | 1.89230 / 2.35056 | 通过 |
| Vulkan Scene GPU Renderer | 0.64882 / 1.14221 | 0.64693 / 1.07952 | 通过 |

D3D12 Scene GPU 第一批五次正式运行的 median 为 `[1.14501, 0.84616, 1.13646, 1.14616, 0.84720]` ms；第二批为 `[0.84365, 1.12414, 0.85098, 0.84146, 0.83179]` ms。快慢两类并非只分属不同批次。第一批→第二批虽变快，但反向 median +34.708%、p95 +50.699%，超出原 5%/10% 稳定性门限；不能把单向性能改善视为可重复基线。

辅助定位：D3D12 BeginFrame 两批 median 1.38225/1.36345 ms，frame fence median 均 0.0003 ms；本轮不能继续用之前的失焦或较长 BeginFrame 解释 Scene GPU 差异。第一批 r4/r5 的 Scene DeferredLighting median 为 0.141248/0.079648 ms、SSR 为 0.090400/0.051360 ms、GBuffer 为 0.049456/0.027584 ms，多项 Pass 同时变化，不只是外层 Renderer 区间。这些是描述性差异，不将各 Pass 中位数相加作为 GPU 关键路径，也没有足够证据归因为 GPU 频率、电源、驱动调度、计时错误或算法变化。没有为通过门限而改变系统电源、驱动设置、同步或渲染负载。

离线 `P0/unfocused-preview-audit-01/report.json` 从全部原始 JSONL 重算，**292 个产物**（288 个子产物+4 份批次汇总）、32 个登记子 index 及源码/工具/二进制身份核验通过；`complete=true, passed=false, failedRuns=0, missingRuns=0`。这里 `failedRuns=0` 仅指子运行均合法，整批稳定性仍失败。`-Enforce` 写出报告后按预期非零退出，没有将失败重新归类为成功。

- aggregate index SHA-256：`E515238E1D70939D4172E3C2CB9BD1C9B0523788DD3FBA84F57F3CB41E489010`。
- audit report SHA-256：`0EC548C37B1EEDDC536A01603FA6FDBA067375ED21CDBBC353327889FB39A753`。
- 完成后复核 **455 项生产输入**与测量清单完全一致，含 60 项 assets；两二进制 SHA 与上节冻结值一致。
- 另对本轮两个 GPU 套件、两个全 Demo 捕获、两个 HPWater 序列、三个新模式短测的 index 产物逐项重算，**1,525 个功能/短测产物 SHA** 全部一致；这项是证据完整性复核，不额外计作重新执行测试。最终 OpenSpec strict 通过。

**交付的是已实现并通过功能回归的无焦点测试模式，不是 P0 通过。** 按 openspec-apply-change 暂停在新确认的 D3D12 Scene GPU 稳定性阻碍，保持 **18/118**、1.8/1.9 未完成。后续建议针对同一配置的快慢运行补充 GPU 时钟/负载与双视图提交时间线的关联证据；不预设原因、不自动修改系统设置或同步策略。shadows/HPWater/PBF 的新协议两批基线及全 Demo 连续矩阵仍未执行，不能用 preview 或已通过的静态图像代替，P1/线程迁移未启动。

恢复点 `20260829-p0-unfocused-window` 已创建并重复校验 **874 文件**；manifest SHA-256 `E69BDB3727F705C987EE3EE458696C7C6375111D7FF69D8C9E7A9D89C9418EE6`。与上轮相比新增一个性能策略头，其余为本节列出的接线、校验、测试和记录变更。本条及验收总表恢复点说明在封存后追加，生产/测试代码不再变化；旧恢复点和全部失败证据保留，恢复仅复制到新的空目录。

## D3D12 Scene GPU 动态频率关联诊断（2026-08-29）

用户确认继续后，本轮只新增 `scripts/ArchitectureGpuClockDiagnostic.Common.ps1`、`scripts/Measure-ArchitectureGpuClockDiagnostic.ps1`、`scripts/Summarize-ArchitectureGpuClockDiagnostic.ps1` 和 `tests/scripts/ArchitectureGpuClockDiagnosticTests.ps1`，并更新本文、验收总表和 tasks。没有修改 `src/`、`assets/`、CMake、二进制、Renderer/RHI/HPWater/Demo 或现有性能比较算法。

数据流为：原 `Measure-ArchitecturePerformance.ps1` 继续生成并严格校验逐帧 CPU/GPU/窗口证据；同时一个隐藏的 `nvidia-smi` 只读进程按 250 ms 记录 P-state、graphics/SM/memory clock、利用率、功耗和温度。包装器不写 GPU 设置，不锁频、不改 power limit；子运行结束后封存两侧 SHA。汇总器核验全部原产物，只将遥测从 `renderer.initialize.completed` 到 `process.exit` 全覆盖且原性能子运行有效的进程纳入相关计算。旁路观测包括初始化及后处理、可能扰动计时，因此所有输出明确 `diagnosticOnly=true`，**不能替代或放行无旁路的 V3 基线**。

工具对截断末行、非末行损坏、非法 P-state、时区转换、数值分布、Pearson 相关和 dry-run 契约共 **8/8** 通过。真实实现自检 `P0/d3d12-preview-gpu-clock-diagnostic-final-smoke-01/` 两次均完整覆盖、原性能严格校验有效，aggregate SHA `11D89159C3E51641EF505AC6FD07367E09250B460B3D86C32605E5D9C58D244A`。前两版诊断目录保留：`-01` 暴露 `nvidia-smi` 被停止时的截断末行；`-02` 暴露完成后立即重复解析大 JSONL 的瞬态问题。最终实现只允许丢弃最后一条不完整遥测，其他坏行失败；复用已由原测量器严格校验并封存的 summary，不重复把同一大文件当作新的 gate。

主证据 `P0/d3d12-preview-gpu-clock-diagnostic-03/` 执行 8 次：7 次原性能子运行有效，1 次因实际获得焦点被原规则拒绝；其中 5 次同时具有从初始化至退出的完整遥测，另外 3 次遥测进程提前结束，汇总时明确排除而不补样本。主 aggregate SHA `1BC7B7E283CD3E527B833775C9B2F4E5630CBE43B310C1AEB2E8668B8ACDB1D7`。离线报告 `P0/d3d12-preview-gpu-clock-summary-03/report.json` SHA `D30D7C550F87574CEE57A11451123325C475C9242D4A0C1E2E0C759412DA7E4D`，五个有效关联如下：

| run | graphics clock median (MHz) | Scene GPU Renderer median (ms) | Game GPU Renderer median (ms) | CPU frame median (ms) |
| --- | ---: | ---: | ---: | ---: |
| 1 | 1665 | 1.15466 | 2.31024 | 7.2468 |
| 2 | 2430 | 0.84002 | 2.34142 | 7.2126 |
| 3 | 2415 | 0.84000 | 2.39552 | 7.2801 |
| 5 | 2370 | 0.87915 | 2.44021 | 7.4457 |
| 8 | 1537 | 1.14286 | 2.39314 | 7.3185 |

进程级 Pearson：Scene GPU 对 graphics clock **-0.99053**，Game GPU 为 0.34474，CPU frame 为 0.10665。最终实现的两次真实自检也都处于低时钟/慢 Scene 类（1642 MHz/1.13936 ms、1507 MHz/1.14322 ms）。这与原始帧内结论一致：慢进程几乎全部 measured 帧属于慢类；快进程仍有约 157–182 个慢类帧并频繁切换，多项 Scene Pass busy 同时变化，视图/窗口/设置不变。

结论边界：证据强烈支持 **GPU 动态频率状态与 D3D12 Scene Pass 时长相关**，并把焦点、尺寸、TAA/阴影配置、CPU frame 及单一外层 timestamp 问题排除为当前主要解释；但 5 个进程级样本和非逐帧对齐的 vendor observer 不能证明 DVFS 是唯一因果，也不能批准豁免。只读查询确认当前驱动提供 `nvidia-smi -lgc/-rgc`，但锁频或切“prefer maximum performance”会改变用户系统状态，本轮未执行。增加正式运行数、按外部时钟筛样本或改变 V3 聚合也属于测量协议变更，未自行采用。

因此仍为 **18/118**：无焦点功能完成，D3D12 preview V3 仍失败，1.8/1.9 和 P1–P8 不勾选。下一步需要在“临时锁定并最终恢复 GPU clocks 后重建全部新协议基线”“修订为更多独立正式进程的协议”“明确接受本机 DVFS 例外”之间由用户选择；推荐第一项，且必须先验证此消费级 GPU/权限实际支持、记录原状态并保证失败路径执行 `-rgc`。不推荐以当前相关性报告直接接受门禁。

恢复点 `20260829-p0-d3d12-dvfs-diagnostic` 已创建并重复校验 **878 文件**；manifest SHA-256 `42CE58F51619097B889CC96F65C07D36546A4A4820FAF9F7BFE55663DD7B8B21`。与上一恢复点相比仅新增四个诊断脚本/测试并更新本文、验收总表和 tasks，生产输入仍为冻结的 455 项。此条与验收总表的恢复点说明在封存后追加；旧恢复点和所有有效/失败诊断证据不覆盖。

## FPS 统计口径、双视图成本与锁频权限结果（2026-08-29）

本轮新增 `scripts/Summarize-ArchitectureViewThroughput.ps1`、`scripts/ArchitectureGpuClockControl.Common.ps1`、`scripts/Run-ArchitecturePerformanceLockedGpuClock.ps1` 和 `tests/scripts/ArchitectureGpuClockControlTests.ps1`。只修改验证工具和本文/验收/tasks，不修改 `src/`、CMake、二进制、HPWater、Demo、shader、Present、同步或画质。锁频包装器是显式 opt-in：查询离散频率 → 请求固定频率 → 运行原两批 Benchmark → `finally` 最多三次 `-rgc` → 写出锁定/恢复退出码和紧急恢复命令。只有锁定命令返回成功才把 reset 失败视为“可能残留状态”；锁定被权限拒绝时仍尝试 reset，但不会谎称曾成功改变状态。

当前 recorder 的两个时间含义必须分开：`cpu.frameMs` 从 `BeginFramePerformance` 到 `CompletePerformanceFrame`，包含事件/更新、Editor UI 构建、BeginFrame、场景发布与 view 复制、Game Render、实际发生的 Scene Render、UI draw、EndFrame/Present、capture resolve；不包含最终内存读取和 JSON 写入。`cpu.loopIntervalMs` 是相邻两次 frame-start 的墙钟间隔，因此也包含上一帧 observer/JSON 成本，是本工具可用于真实循环吞吐/FPS 的来源。`gpu.*.Renderer` 和 `cpu.gameRenderMs/sceneRenderMs` 只用于归因，**不倒数为应用 FPS，也不把各阶段中位数相加**。

源码核对证明 Scene 不是无条件渲染：只有 Scene viewport 可见且满足 always/on-demand 刷新条件时才调用第二个 `SceneRenderer::Render`；但当前完整 Editor `preview` 性能样本每个 measured frame 的 active views 都是 `game,scene`，所以这批 P0 计时确实包含 Scene。`-View game` 只选择捕获视图，不会关闭 Scene 渲染。无 Editor 构建没有第二 renderer，active views 为 `game`。

离线工具先通过原 index/artifact SHA、原始 JSONL、GPU validation log、设备/驱动和输入配置校验，再从每个 measured frame 的 `loopIntervalMs` 计算 `1000 / interval`；它没有用 `1000 / GPU pass`。同一 Vulkan、preview、native、1280×800、严格 GPU validation、无焦点可见窗口的已封存短测结果如下：

| 完整运行路径 | active views | loop median (ms) | 实际循环 FPS median | FPS p05 | 关键阶段 median |
| --- | --- | ---: | ---: | ---: | --- |
| 完整 Editor | game,scene | 5.99260 | 166.872 | 130.169 | Game CPU 1.92865 ms；Scene CPU 1.87630 ms |
| 无 Editor / standalone | game | 2.75135 | 363.458 | 276.625 | Game CPU 1.80935 ms；Scene 0 ms |

Standalone 中位 FPS 是 Editor 的 **2.178×**。这直接回答当前低 FPS 是否包含 Scene：P0 完整 Editor 双视图基线包含；Game-only standalone 已达到约 363 FPS，与用户观察的 Unity 空 Game View 300–400 FPS 同量级。不过 preview 仍是完整 Demo，且严格校验开启；Editor 与 standalone 的差值还包含 UI、第二 view、不同应用壳和 GPU 调度，不能把全部 2.178× 都归因于 Scene，也不能把此短测当作两批 V3 或“空场景 retail”基准。报告为 `P0/view-throughput-comparison-02/report.json`，SHA-256 `3FBBB8D573C3FE4A9DFD0692CF547D208F992B915D50800AD9D75106EC3BE021`。

官方口径对照：Unity 6 的 Game View Rendering Statistics 将 FPS 定义为当前可绘制帧率；其中 CPU Main 明确包含 Scene View、其他 Editor 窗口和 editor-only 工作，而 Render 只描述 Game View 更新。Unity 官方也要求精确性能验证使用目标平台 Player，因为 Play Mode 与 Editor 同进程，其他 Editor 窗口会消耗 render thread/GPU；FrameTiming 的 total CPU frame time 则是相邻帧边界间隔，包含等待和开销。UE 的 `stat unit` 同时报告 Frame/Game/Draw/GPU/RHIT，Frame 通常接近同步链中最慢的一项；官方线程模型让 Game Thread、Render Thread、RHI Thread 流水重叠，而不是把三段时间相加得到帧时间。参考：

- Unity Rendering Statistics：https://docs.unity3d.com/6000.0/Manual/RenderingStatistics.html
- Unity Profiling your application：https://docs.unity3d.com/2022.2/Documentation/Manual/profiler-profiling-applications.html
- Unity FrameTiming：https://docs.unity3d.com/6000.0/ScriptReference/FrameTiming.html
- Unreal Stat Commands：https://dev.epicgames.com/documentation/unreal-engine/stat-commands-in-unreal-engine
- Unreal Parallel Rendering Overview：https://dev.epicgames.com/documentation/unreal-engine/parallel-rendering-overview-for-unreal-engine

获准的 2400 MHz D3D12 实验没有开始基线：`nvidia-smi -lgc 2400,2400` 在本机以 exit 4 明确返回“current user does not have permission”，随后三次 `-rgc` 也因同一权限被拒绝。锁定命令从未成功，Benchmark 目录未创建；失败 index 原样保留为 `P0/d3d12-preview-locked-2400-baseline-01/index.json`，SHA-256 `003CF438ECE9BA56C999BE2C86C90BE5FBA0A102F1BC65E6CFF5337FB7AD86CA`。只读复核仍显示动态 current clock、max 3090 MHz，applications clock limiter 为 Not Active；没有证据表明本轮改变了 GPU 状态。管理员权限属于当前执行环境之外的新条件，不能绕过或把未运行实验写成通过。

验证：锁频 plan/dry-run **2/2**、原 GPU clock 只读工具 **8/8**、原性能工具 **64/64**；view-throughput 报告对两个真实封存 run 端到端通过。当前仍为 **18/118**，1.8/1.9 未完成，不进入 P1。下一步若由管理员终端运行已生成的锁频包装器，可重建 D3D12 两批；否则只能在获得明确协议修订后采用更大独立样本，不能自行接受 DVFS 例外。

恢复点 `20260829-p0-fps-scope-clock-permission` 已创建并重复校验 **882 文件**；manifest SHA-256 `739DA9364EBEFEA987CB9B3EB7487AA2B69A576D31FAAC2115F670C97F2085A7`。455 项生产输入与冻结测量清单逐项一致；本条、验收总表和 tasks 的恢复点登记在封存后追加，工具实现未再改变。旧恢复点、锁频权限失败和 FPS 报告均保留。

## 同一 Editor 二进制的 Active View 成本隔离（2026-08-29）

为回答“100 多 FPS 是默认延迟管线过重，还是 Editor/架构开销”而新增一个默认关闭的诊断输入。`PRISM_RENDER_PERFORMANCE_ACTIVE_VIEWS=game` 只允许与逐帧性能采样一起使用；`Measure-ArchitecturePerformance.ps1 -ActiveViews game` 是公开入口。普通运行未设置时仍执行原 Game + 可见 Scene View 路径。显式 Game-only 时不提交 Scene renderer，Scene 面板复用已转换到 sampled 状态的 Game 输出，保留 Editor shell/UI 成本并避免采样未初始化 Scene 纹理。Scene capture 与该模式互斥，非法值、脱离性能采样使用和 Scene capture 都在测试中拒绝。

文件与数据流：新增 `src/Core/Application/PerformanceActiveViews.h`、`scripts/Summarize-ArchitectureActiveViewCost.ps1`；修改 `ApplicationHost.{h,cpp}`、性能采样脚本/common 和 C++/PowerShell/真实进程负例。输入环境 → Host 窄解析 → 仅控制是否提交第二个 Scene renderer → recorder header 记录 `activeViewPolicy` → 原始每帧 CPU/GPU view 集合 → 汇总器要求相同 EXE/source/hardware/window/presentation 后才做 A/B。没有修改 Feature 设置、RenderGraph、shader、HPWater、Present 或 fence 顺序。

同一完整 Editor EXE、Vulkan、preview、1280×800、strict validation、visible+unfocused、60/180/8 帧的两次独立封存结果：

| 指标（median） | 默认 Game+Scene | Editor Game-only | 节省 |
|---|---:|---:|---:|
| loop interval / 实际 FPS | 5.7432 ms / 174.119 | 3.25615 ms / 307.111 | 2.48705 ms / 1.764× FPS |
| CPU frame | 5.6235 ms | 3.16745 ms | 2.45605 ms |
| Game Render | 1.9142 ms | 1.8770 ms | 0.0372 ms |
| Scene Render | 1.81085 ms | 0 ms | 1.81085 ms |
| BeginFrame | 0.8696 ms | 0.41345 ms | 0.45615 ms |
| UI build + draw | 0.4529 ms | 0.44005 ms | 0.01285 ms |

因此当前 Overlay/`FrameTimer` 的 FPS 对“整个 Editor 主循环”是正确的，但它不是 Game View 单独吞吐；`Tick()` 位于循环开始，统计相邻循环边界。当前不改 UI 文案，避免影响已冻结界面图像；性能报告明确使用 `actualLoopFps` 和 active views 消除歧义。A/B 的 2.48705 ms 差值中，第二个 Scene Render 的中位差为 1.81085 ms，BeginFrame 压力下降再贡献 0.45615 ms，剩余 0.22005 ms。Game Render 中位仅变化 0.0372 ms，未呈现“Game 默认延迟管线本身占掉数毫秒”的特征。

GPU 具体 Pass 也支持这个判断：Game-only 的 DeferredLighting 0.06 ms、SSR 0.05 ms、TemporalResolve 0.04 ms、Shadow/GTAO 各 0.03 ms、GBuffer/HiZ 各 0.02 ms；双视图时这些 Pass 的中位数基本相同。外层 Game `Renderer` 区间从 1.887 ms 降至 0.632 ms，而具体 Pass 未同步下降，说明该外层区间还受双视图队列交错/等待影响，不能把它解释为某个延迟光照 Pass 的成本，也不能对 Pass 求和。汇总报告 `P0/active-view-cost-vulkan-01/report.json` SHA-256 为 `02C21A5E0E7A9062D95FA484CA10D85F4F9A9A83301D3F907BE81E0D4D179756`。

D3D12 可见窗口 A/B 两次都由现有状态门禁以 `DXGI_STATUS_OCCLUDED` 拒绝，即使窗口观察为 visible/focused；失败目录保留，不混入 Vulkan 结论，也不关闭 VSync/放宽状态检查。Vulkan A/B 是单次归因实验，不替代 1.8 的两批 V3 稳定性门禁。

兼容验证：完整 Editor 重新构建、C++ FramePerformance 测试、性能工具 **65/65**、Vulkan 真实进程负例 **13/13** 通过。普通路径 D3D12/Vulkan 各 **20/20** Demo strict 与冻结 V2 图像比较全部通过，包含 Ocean/WaveWorks/HPWater/PBF/三种流体效果；比较 index SHA 分别为 `923F616653C2F32E3691884FC968D2EF35B69E7D81E43E4C2E38477A624A371B`、`0A919EA7960D39BA75383107EECBE472C04240C19BE297832501A9F636CD3E53`。这轮完成了耗时归因与诊断工具，不代表 P0 稳定性门禁完成；仍为 **18/118**，1.8/1.9 不勾选，不进入 P1/线程迁移。

最终完整 Editor/无 Editor Vulkan 两构建通过，CPU 套件 **13/13 + 12/12**；OpenSpec strict 通过。恢复点 `20260829-p0-active-view-cost` 已创建并重复校验 **884 文件**，manifest SHA-256 `DF79D214951A8E2AFE5343E9C38B88446911EA9B8D6C59DBE79D34B1FE21F73B`。恢复点之后仅追加本哈希说明，旧恢复点和全部失败性能目录不覆盖。

## 管理员锁频复测与 Vulkan 验证层阻碍（2026-08-29）

管理员权限复测确认 `nvidia-smi -lgc 2400,2400` 在本机可用。驱动在命令返回后存在短暂查询滞后，因此锁频包装器不再把 exit 0 当作已稳定：它以 250 ms 间隔、最多 5 秒轮询实际 graphics clock，要求落在目标的 ±20 MHz 内，并把全部观察值写入 index。本次由 2812 MHz 收敛至 2392 MHz；中断路径执行 `-rgc`，最终只读检查恢复为动态 2812 MHz，applications-clock limiter 为 Not Active。

D3D12 的旧前台策略仍会受 Codex/桌面抢焦点影响，强制抢焦又会污染帧分布。性能专用无焦点窗口因此补齐 Win32 稳定协议：恢复意外最小化、固定测试几何、`WS_EX_NOACTIVATE`、topmost/no-activate 显示，并将焦点交回创建测试窗口前的前台窗口。该逻辑仅由显式性能窗口策略启用。最终 `unfocused-return-elevated-full-20260829-r1..r8` 的 **8/8** 个完整 D3D12 进程均得到 368 帧，所有帧保持 visible=true、focused=false、1280×800、非最小化、Present 成功，Game/Scene GPU readback 完整。干净重建 217/217、FramePerformance、GPU clock 工具 **3/3**、性能工具 **72/72** 与 OpenSpec strict 均通过。

完整锁频基线 `locked-clock-final-baseline-20260829-08` 已完成 D3D12 第一批四负载共 **32/32** 次有效运行。随后 Vulkan preview 在创建 instance 前以 `VK_ERROR_LAYER_NOT_PRESENT` 失败。对照 `vulkaninfoSDK` 证明：普通进程可通过项目绝对 `VK_LAYER_PATH` 发现 `VK_LAYER_KHRONOS_validation`，但管理员高完整性进程会忽略该环境层路径，只看系统注册的五个 layer；因此不能在保持 strict validation 的同时直接完成管理员包装器内的 Vulkan 批次。

本轮没有关闭验证层、没有把 D3D12 数据冒充双后端结果，也没有留下系统改动：GPU clock 已 reset，HKCU/HKLM Vulkan ExplicitLayers 临时键均不存在。继续的最小可行方案是在锁频运行的最外层 `try/finally` 中，临时将项目验证层 manifest 注册为 `HKLM\\SOFTWARE\\Khronos\\Vulkan\\ExplicitLayers` 的启用值，运行前用管理员 `vulkaninfo` 验证，结束后精确恢复旧值或删除本次新增值。它会在运行期间影响机器上所有用户的管理员 Vulkan 进程，且断电/强制终止可能留下该值，所以必须获得用户对这一机器级风险的明确授权后才可实施。当前 **19/118**，1.8 未完成，不进入 P1。
