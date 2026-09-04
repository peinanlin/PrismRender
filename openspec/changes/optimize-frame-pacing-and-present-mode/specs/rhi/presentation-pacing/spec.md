## Purpose

为 PrismRender 提供跨图形后端一致、可诊断、可运行时选择且保持资源安全的帧呈现与节拍控制，使默认交互、低延迟和吞吐测试各自具有明确语义。

## ADDED Requirements

### Requirement: Orthogonal frame-pacing configuration

系统 SHALL 使用类型化配置独立表达 presentation intent、可选 target FPS 和最大排队帧数。系统 SHALL 提供 `interactive-smooth`、`low-latency`、`benchmark` 三个预设及 `custom` 配置；未提供配置时 MUST 使用 `interactive-smooth`。Target FPS 为空 MUST 表示不增加软件帧率 deadline，而不是自动关闭或启用显示同步。最大排队帧数 MUST NOT 被当作 swapchain image count 或 frame-resource slot 数量的别名。

#### Scenario: Default interactive startup
- **WHEN** 用户没有提供 frame-pacing 配置
- **THEN** 系统以 `interactive-smooth`、同步呈现和最大排队帧数 2 请求初始化

#### Scenario: Explicit benchmark
- **WHEN** 用户显式选择 `benchmark`
- **THEN** 系统请求 Immediate presentation、无 target FPS 和有界排队深度，并清楚标识可能撕裂和更高功耗

#### Scenario: Independent custom frame limit
- **WHEN** 用户选择 custom、同步呈现并配置有效 target FPS
- **THEN** 系统保留同步呈现意图并独立应用目标帧率 deadline

#### Scenario: Invalid configuration
- **WHEN** profile、target FPS 或最大排队帧数超出支持范围
- **THEN** 系统在交换链变更前拒绝配置并报告具体允许值，不得静默改成 benchmark 或无限排队

### Requirement: Explicit frame admission and bounded lead

系统 SHALL 在 Main lane 采样输入和生产下一帧之前执行统一 frame admission。Admission SHALL 综合 display readiness、target FPS deadline 和 configured max queued frames，并返回分类等待时间。Threaded 与 inline execution MUST 使用同一语义；native wait handle MUST 保持由 backend execution lane 所有。现有 GPU fence MUST 继续保护 frame resource、back buffer 和 deferred retirement，不得为提高 FPS 而删除。

#### Scenario: Threaded frame admission
- **WHEN** threaded 模式准备生产下一帧且显示系统或队列尚未允许新帧
- **THEN** Main 通过 execution target 等待值化 admission 完成，并在完成后采样输入，不直接访问 native backend 对象

#### Scenario: GPU genuinely behind
- **WHEN** display admission 已完成但 GPU 尚未完成待复用 frame resource
- **THEN** backend 继续等待对应资源 fence，并把该时间报告为 frame-resource wait 而不是 display wait

#### Scenario: Queue depth reached
- **WHEN** submitted frame 数量已达到 configured max queued frames
- **THEN** 新帧 production 被 admission 阻止，队列不得继续无界增长

### Requirement: D3D12 presentation and latency behavior

D3D12 后端 SHALL 查询 windowed tearing capability，并使用 flip-model frame-latency waitable swapchain。支持 tearing 时，swapchain create/resize flags SHALL 保留该能力，即使当前 effective intent 为同步呈现；实际 Present 只有在 Immediate 且能力与创建 flag 都满足时才能传入 tearing flag。系统 SHALL 按配置设置 maximum frame latency，并在生产每帧工作前等待 frame-latency object。Resize/recreate MUST 保持兼容 flags 和 RAII handle 生命周期。

#### Scenario: D3D12 interactive smooth
- **WHEN** effective intent 为 Synchronized
- **THEN** D3D12 使用 `Present(1, 0)`，按配置限制最大排队帧数，并通过 waitable object 形成 display admission

#### Scenario: D3D12 low latency
- **WHEN** effective intent 为 LowLatencySynchronized
- **THEN** D3D12 保持同步无撕裂 Present，并将 effective maximum frame latency 设为 1

#### Scenario: D3D12 benchmark with tearing support
- **WHEN** requested intent 为 Immediate 且 capability、swapchain flag 均支持 tearing
- **THEN** D3D12 使用 sync interval 0 和 `DXGI_PRESENT_ALLOW_TEARING`，同时保持有界帧延迟

#### Scenario: D3D12 benchmark without tearing support
- **WHEN** requested intent 为 Immediate 但 tearing 不受支持
- **THEN** D3D12 使用合法的 `Present(0, 0)` fallback，并在 effective state 中报告未使用 tearing

#### Scenario: D3D12 resize
- **WHEN** 任一 profile 触发 swapchain resize/recreate
- **THEN** 系统保持创建时兼容 flags、重新取得有效 waitable object，并继续使用当前 requested configuration

### Requirement: Vulkan presentation and outstanding-frame behavior

Vulkan 后端 SHALL 将 Synchronized 映射为 `FIFO`，将 LowLatencySynchronized 按 `MAILBOX -> FIFO` 选择，将 Immediate 按 `IMMEDIATE -> MAILBOX -> FIFO` 选择。系统 MUST 记录 requested/effective intent、原生 present mode 和 fallback 原因，不得把 `MAILBOX` 报告为与 Immediate 相同的语义。系统 SHALL 通过可用的 present wait/timing 能力或受控的 acquire/in-flight 同步限制 outstanding frames。

#### Scenario: Vulkan interactive smooth
- **WHEN** requested intent 为 Synchronized
- **THEN** effective native present mode 为规范保证支持的 `FIFO`

#### Scenario: Vulkan low-latency preferred mode
- **WHEN** requested intent 为 LowLatencySynchronized 且 surface 支持 `MAILBOX`
- **THEN** effective mode 为 `MAILBOX`，诊断将其标记为无撕裂 latest-frame 策略

#### Scenario: Vulkan benchmark preferred mode
- **WHEN** requested intent 为 Immediate 且 surface 支持 `IMMEDIATE`
- **THEN** effective native present mode 为 `IMMEDIATE`

#### Scenario: Vulkan deterministic fallback
- **WHEN** requested intent 的首选 mode 不受 surface 支持
- **THEN** 系统按规定顺序选择受支持模式、继续初始化并报告 fallback；只有 `FIFO` 时不得失败

#### Scenario: Vulkan runtime mode change
- **WHEN** 配置变更需要不同 `VkPresentModeKHR`
- **THEN** render lane 在安全边界重建 swapchain，并在完成前报告 pending generation、完成后发布新的 effective generation

### Requirement: Independent target frame limiter

当 target FPS 存在时，系统 SHALL 使用单调时钟形成稳定 deadline，并将 limiter wait 与 display/resource wait 分开。实现 MUST 使用可中断长等待并把 yield/spin 限制在短校正窗口，MUST NOT 以整帧 busy-spin 实现限帧。Display admission 已经晚于 deadline 时 MUST 不增加重复等待；严重 hitch 或配置变更后 SHALL 重置 deadline history。

#### Scenario: Target below refresh
- **WHEN** custom profile 请求同步呈现且 target FPS 低于当前显示节拍
- **THEN** limiter 将新帧 production 延迟到 target deadline，并报告实际 interval error 和 limiter wait

#### Scenario: Display is slower than target
- **WHEN** display admission 完成时间已晚于 target deadline
- **THEN** limiter 不再额外等待，帧被分类为 display-bound

#### Scenario: Hitch recovery
- **WHEN** 单帧严重超出多个 target interval
- **THEN** limiter 重建未来 deadline，不连续提交追赶帧

### Requirement: Runtime configuration transition

系统 SHALL 允许启动配置和 Editor/Game 调试 UI 请求 frame-pacing profile。请求 MUST 通过可靠 execution control command 传入 backend，不能被普通 frame coalescing 丢弃。系统 SHALL 发布 requested、pending 和 effective configuration generation；变更失败 MUST 保持最后一个有效配置并给出错误，不得留下部分应用状态。

#### Scenario: D3D12 runtime profile switch
- **WHEN** D3D12 从 interactive-smooth 切换到 benchmark
- **THEN** backend 在兼容 swapchain flags 下原子更新 Present 参数和 maximum frame latency，并发布新 effective generation

#### Scenario: Vulkan runtime profile switch
- **WHEN** Vulkan profile 变更需要 swapchain rebuild
- **THEN** UI 在 rebuild 完成前显示 pending，完成后显示实际 native mode 和新的 effective generation

#### Scenario: Failed transition
- **WHEN** 新配置无法安全应用
- **THEN** 系统继续使用最后一个有效配置、报告失败原因且不破坏当前 swapchain

### Requirement: Frame-pacing diagnostics and FPS semantics

系统 SHALL 在启动日志、FrameProfiler metadata/逐帧数据、RenderFrameFeedback 和 Game 调试 overlay 中报告 profile、requested/effective intent、target FPS、configured/effective max queued frames、native present mode、sync interval、tearing capability/use、swapchain image count、admission source、configuration generation 和 fallback/pending 原因。逐帧统计 SHALL 分离 display admission、frame limiter、frame-resource fence 和 Present CPU 时间。FPS MUST 表示完整 application loop interval，不得使用 GPU pass 倒数替代。

#### Scenario: Synchronized diagnostics
- **WHEN** 任一后端以 interactive-smooth 完成一帧
- **THEN** 诊断明确显示同步模式、display wait、资源 wait、Render/GPU work 和完整 loop FPS

#### Scenario: Benchmark diagnostics
- **WHEN** benchmark 完成一帧
- **THEN** 诊断能证明 target FPS 未启用、实际 native mode/sync interval、是否使用 tearing以及队列仍然有界

#### Scenario: Editor view attribution
- **WHEN** 分别采集双视图和 Game-only
- **THEN** 报告分别保存 Editor Loop、Game/Render/GPU 和等待分项，不把 Scene View/Editor 工作误记为 Game GPU 时间

### Requirement: Matched empty-workload acceptance

验收 SHALL 在同一 Release 构建、D3D12、1280×800、validation off、native、`threaded+pool`、30 帧预热和 120 个有效样本下，分别测试 Editor 双视图、Game-only 和 standalone 的 interactive-smooth、low-latency 与 benchmark；custom SHALL 至少验证 60/120/144 FPS 中设备可有效区分的目标。报告 MUST 保存平均值、P95/P99 interval、Main/Render/GPU work、各类 wait、队列深度和 input-to-present。300 FPS 仅为当前硬件 benchmark 参考，不是绝对门禁。

#### Scenario: Interactive baseline
- **WHEN** 运行默认 interactive-smooth 空场景
- **THEN** 实际模式保持同步无撕裂语义，数据把显示等待与渲染工作分开，并可重放当前约 56 FPS 的显示受限基线

#### Scenario: Low-latency comparison
- **WHEN** 在相同条件运行 low-latency
- **THEN** 报告比较 queue depth 1/2 的 input-to-present、吞吐和 P95/P99 hitch，不以平均 FPS 单独宣称延迟改善

#### Scenario: Benchmark comparison
- **WHEN** 运行 benchmark
- **THEN** D3D12 实际使用 sync interval 0 或 Vulkan 报告其 effective fallback，显示节拍 wait 相对同步基线显著下降，并保留真实剩余瓶颈

#### Scenario: Reference FPS not reached
- **WHEN** benchmark 正确生效但完整 loop 未达到 300 FPS
- **THEN** 系统依据 Main/Render/GPU/queue/wait 分项继续定位，不取消必要 fence、不改变画质，也不把参考值当作唯一失败条件

### Requirement: Rendering compatibility

Frame-pacing 变更 MUST NOT 修改场景内容、RenderGraph/Feature 选择、shader、模拟步长或默认画质。验证 SHALL 包含 D3D12/Vulkan 的 Preview、WaveWorks Ocean 和真实 resize 短冒烟；空场景 pass 裁剪、四个粒子 Demo 和完整 Demo 矩阵不属于本 change 门禁。

#### Scenario: Representative render paths
- **WHEN** 各后端运行代表场景、切换 profile 并执行 resize
- **THEN** 进程正常完成，无新增 validation error、device removal、非有限诊断值或资源生命周期错误，且默认场景设置未改变
