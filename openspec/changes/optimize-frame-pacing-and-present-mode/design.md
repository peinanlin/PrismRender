## Context

当前 `D3D12Context::EndFrame` 固定 `Present(1, 0)`，`BeginFrame` 在复用当前 back-buffer frame context 前等待 fence。本机 Release 空场景中，Render lane active 约 1.7–2.7 ms、GPU 约 0.9 ms，而 frame-resource fence wait 约 15–16 ms；`Present` 调用本身约 0.1 ms。显示队列的反压因此延后暴露在资源 fence，但 fence 本身仍是正确资源生命周期同步，不能删除。

现有方案把策略压缩为 `Uncapped|VSync`，默认改为 uncapped，并把 Vulkan `MAILBOX` 当成 uncapped 首选。这混淆了三个事实：`MAILBOX` 仍在垂直消隐时显示；软件目标帧率与 display sync 不等价；CPU 可领先帧数会同时影响吞吐和输入延迟。

Backend 由 `RenderRuntimeExecutionTarget` 在执行 lane 创建并唯一持有。Main lane 负责输入和 Editor/Game 更新，因此低延迟 pacing 必须在 Main 开始生产帧之前建立“帧准入”，同时不能向 Main/UI 泄露 DXGI waitable handle 或 Vulkan 对象。

## Goals / Non-Goals

**Goals:**

- 默认保持稳定、无撕裂的交互体验，同时提供显式、可重放的 benchmark。
- 独立控制 display presentation、target FPS 和 max queued frames。
- 将显示/限帧等待前移到帧准入边界，尽量在准入后采样最新输入。
- 让 D3D12/Vulkan 共享意图和诊断模型，但按各自 API 语义正确实现。
- 明确分离 CPU/GPU work、display admission wait、limiter wait、resource fence wait 和 Present CPU 时间。
- 支持 Editor 中安全切换预设，并发布 requested/effective 状态和 fallback 原因。

**Non-Goals:**

- 不实现 NVIDIA Reflex、AMD Anti-Lag、平台厂商睡眠 API 或独立 RHI thread。
- 不承诺软件 limiter 在所有 Windows 计时环境达到专业竞技游戏级精度；本 change 要求可测量、无持续 busy-spin，并为后续平台优化保留接口。
- 不把 present 优化宣称为 RenderGraph/GPU 算法提速，也不在本 change 裁剪空场景 pass。
- 不改 shader、默认画质、模拟步长、场景数据或完整 Demo 行为。
- 不运行完整 Demo/粒子矩阵或整仓哈希。

## Decisions

### 1. 用预设表达产品意图，用解析后的配置驱动后端

公共层新增：

```cpp
enum class FramePacingProfile
{
    InteractiveSmooth,
    LowLatency,
    Benchmark,
    Custom
};

enum class PresentationIntent
{
    Synchronized,
    LowLatencySynchronized,
    Immediate
};

struct FramePacingConfiguration
{
    FramePacingProfile profile;
    PresentationIntent presentation;
    std::optional<uint32_t> targetFps;
    uint32_t maxQueuedFrames;
};
```

预设解析为以下默认值：

| Profile | Presentation intent | Target FPS | Max queued frames | 用途 |
|---|---|---:|---:|---|
| `interactive-smooth` | Synchronized | display-driven | 2 | 默认 Editor/Game 交互 |
| `low-latency` | LowLatencySynchronized | display-driven | 1 | 无撕裂优先的低延迟 |
| `benchmark` | Immediate | uncapped | 2 | 显式吞吐测试 |
| `custom` | 用户指定 | 可选正整数 | 1..N（受实现上限约束） | 工程调试 |

`targetFps` 为空表示不增加软件 deadline；它不自动改变 presentation intent。`maxQueuedFrames` 不等于 swapchain image count，也不等于 frame-resource slot 数量。

默认选择 `interactive-smooth`，不再引入“默认 uncapped”的破坏性变更。环境/配置 parser 严格验证预设和 custom 字段；UI 使用相同类型，不让各 backend 自行读取配置。

### 2. 帧准入是显式接口，资源 fence 只保护资源

在 execution target 增加值化的 frame-admission 操作。Main lane 在 `FrameTimer`/输入采样/场景更新前请求下一帧准入：

1. execution target 根据最大排队帧数限制 Main 对 Render lane 的领先量；
2. backend 所属执行 lane 等待原生 display readiness（若当前策略需要）；
3. 公共 limiter 等待可选 target FPS deadline；
4. 返回带时间戳和等待分类的 admission result；
5. Main 采样输入、更新并提交新 frame packet；
6. backend 继续使用原有 fence 保护 allocator、back buffer 和 deferred resource retirement。

Threaded target 通过命令/完成值执行等待，native handle 始终留在 backend lane；inline target 调用同一接口。不得让 Main/UI 直接等待 DXGI/Vulkan handle，也不得通过忙轮询 Render lane 状态。

这一边界与 UE 将 Game Thread 帧开始同步到 RHI/交换链进度的目标一致，同时保留 PrismRender 当前架构的明确所有权。

### 3. D3D12 使用 waitable swapchain，而不是把 fence stall 当作 VSync

D3D12 factory 先查询 `DXGI_FEATURE_PRESENT_ALLOW_TEARING`。支持时，无论当前默认是否 VSync，swapchain 创建和 resize 都保存 `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING`，以便运行时切换 Immediate；实际 VSync Present 仍使用 flags 0。

swapchain 同时使用 `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`，取得 `IDXGISwapChain2::GetFrameLatencyWaitableObject()`，并按解析配置调用 `SetMaximumFrameLatency(1/2/...)`。每帧在正式生产工作前等待该对象，包括首帧；对象关闭和 swapchain 重建遵循 RAII。

后端映射：

| Intent | D3D12 Present | Frame latency |
|---|---|---|
| Synchronized | `Present(1, 0)` | preset/config 值 |
| LowLatencySynchronized | `Present(1, 0)` | 1 |
| Immediate + tearing supported | `Present(0, DXGI_PRESENT_ALLOW_TEARING)` | preset/config 值 |
| Immediate + no tearing support | `Present(0, 0)` | preset/config 值，记录 fallback |

运行时切换只改变 sync interval、Present flags 和 maximum frame latency；swapchain flags 由创建时能力决定并在 resize 保持。frame-resource fence wait 仍可能因 GPU 真正落后而发生，这应被单独报告而不是强行归零。

### 4. Vulkan 按 present mode 的真实语义映射

Vulkan 选择顺序为：

| Intent | Vulkan preference | 语义 |
|---|---|---|
| Synchronized | `FIFO` | 规范保证、无撕裂、显示驱动 |
| LowLatencySynchronized | `MAILBOX -> FIFO` | 最新帧替换；不可用时同步回退 |
| Immediate | `IMMEDIATE -> MAILBOX -> FIFO` | benchmark 优先不等待 VBlank；能力不足时回退 |

不再把 `MAILBOX` 描述为等价 uncapped/tearing。swapchain image count 按 surface capability 选择，并与 `maxQueuedFrames` 分开记录；FIFO 下至少比较双/三缓冲的 stall 和稳定性，但不在公共契约中硬编码所有设备必须使用相同数量。

若支持 present-wait/timing 能力，backend 用其形成 display admission 和实际 present 时间；否则使用受控的 acquire/in-flight semaphore/fence 数量近似限制 outstanding frames，并明确将 effective admission source 标为 fallback。

切换到需要不同 `VkPresentModeKHR` 的策略时，在 render lane 安全帧边界走已有 swapchain recreation 路径。Main 收到新的 effective generation 前继续显示 requested 与 pending 状态，不假装切换已立即生效。

### 5. Target FPS limiter 是独立阶段

Limiter 根据单调时钟和上一次 admission deadline 计算下一目标时间。长等待使用可中断 OS wait/sleep，末端只允许短时 yield/spin 校正，并记录各阶段时间；不得整帧 busy-spin。deadline 按计划时间推进，发生严重 hitch 时重置基准，避免连续追赶。

当 display synchronization 已形成更慢节拍时，limiter 不额外重复等待；当 target FPS 低于显示刷新率时，它可以在 display admission 之后延迟到目标 deadline。配置变更会重置 limiter history。

完成条件看 frame-interval error、P95/P99 jitter 和 CPU 占用，而不是只看平均 FPS。高精度平台扩展留在同一接口下，不写入 backend Present 实现。

### 6. Requested、effective 和 pending 状态必须值化发布

`PresentationState`/`FramePacingState` 至少包含：profile、requested/effective intent、target FPS、configured/effective max queued frames、native present mode、sync interval、tearing capability/use、swapchain image count、admission source、configuration generation 和 fallback/pending 原因。

Backend 将状态复制到 `RenderFrameFeedback`，Main/UI 不跨 lane 查询 live backend。运行时请求通过 execution target 的可靠控制命令发送，不能被普通 frame coalescing 丢弃。

Profiler 每帧分别保存：

- `displayAdmissionWaitMs`
- `frameLimiterWaitMs`
- `frameResourceWaitMs`
- `presentCpuMs`
- Main/Game/Render active CPU 时间
- GPU frame 时间
- input-to-present 或可获得的近似值
- submitted/outstanding queue depth

Overlay 显示 `Editor Loop FPS` 与 `Game View/Render/GPU`，并明确等待分类。FPS 始终由完整 loop interval 计算，不能由某个 GPU pass 倒数得出。

### 7. 空场景测试使用不同目的的基线，不用单一 FPS 判定

固定 Release、1280×800、validation off、native、`threaded+pool`，每项 30 warmup + 120 valid sample，并分别测试 Editor 双视图、Game-only 和 standalone：

- `interactive-smooth`：检查无撕裂语义、稳定帧时间、合理 display wait 和默认兼容。
- `low-latency`：与 smooth 比较 queue depth 1/2、input-to-present 和 hitch resilience。
- `benchmark`：确认 D3D12 sync interval 0/Vulkan effective mode、真实 CPU/GPU 吞吐和剩余非显示瓶颈。
- `custom 60/120/144`：检查 limiter interval error、P95/P99 jitter 与无持续 busy-spin。

300 FPS 和 frame-resource wait 小于 2 ms 仅是当前机器 benchmark 的诊断参考。硬门禁是配置实际生效、数据口径正确、必要 fence 保留、队列有界、fallback 可解释、无 device removal/validation regression。

## Risks / Trade-offs

- [显式 frame admission 增加 Main/Render lane 往返] → inline/threaded 共用接口并记录 admission overhead；若往返成为显著成本，再优化为可证明等价的预发布 token，不绕过所有权。
- [默认 VSync 继续限制表面 FPS] → 默认目标是交互稳定；Profiler 和显式 benchmark 提供真实吞吐，UI 必须同时显示 work 与 wait。
- [Queue depth 1 降低 hitch resilience] → 仅 low-latency 预设默认 1，interactive 默认 2，并通过 P95/P99 对比说明取舍。
- [D3D12 创建标志组合或 resize 不一致] → 单一 swapchain plan 保存 flags，覆盖 create/resize/recreate 测试。
- [Vulkan mode 变更需要重建] → 使用可靠控制命令、generation 和 pending/effective 状态，在安全边界重建。
- [软件 limiter 引入抖动或高 CPU] → 分阶段等待、禁止整帧 busy-spin，并以 jitter/CPU 数据验收。
- [Benchmark tearing/功耗升高] → 仅显式模式允许，overlay 持续提示 effective Immediate/tearing 状态。
- [更快提交暴露资源寿命问题] → 保持 frame-resource fence 和有界队列，运行 resize、120 帧采样和代表场景冒烟。

## Migration Plan

1. 登记现有 VSync 空场景数据并增加等待分类，先不改变默认行为。
2. 引入公共配置、预设、parser/state 和纯后端选择测试；将默认设为 `interactive-smooth`。
3. 接入 D3D12 waitable swapchain、最大帧延迟、tearing capability 和 resize flag 保持。
4. 接入 Vulkan 语义映射、outstanding frame 限制和 swapchain recreation。
5. 在 execution target/Main loop 接入 frame admission 和 target FPS limiter。
6. 接入可靠运行时配置命令、UI、feedback、日志和 profiler。
7. 执行 matched profile/queue-depth/target-FPS 测试和代表场景冒烟；只在硬门禁通过后完成 change。

出现兼容问题时，回退到 `interactive-smooth`；该回退不需要删除新诊断，也不改变 RenderGraph、TaskExecutor 或场景配置。
