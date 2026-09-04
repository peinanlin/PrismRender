## 1. 基线与公共契约

- [x] 1.1 登记当前 Release D3D12 `threaded+pool` 空场景双视图/Game-only 的 30 帧预热、120 帧样本；验证：保存完整 loop、Main/Render/GPU、display/resource fence/Present、active mask、queue depth、input-to-present 和测试配置，不使用单 Pass 倒数。
- [x] 1.2 新增 `FramePacingProfile`、`PresentationIntent`、`FramePacingConfiguration`、`FramePacingState` 和严格 parser/string conversion；验证：默认 interactive-smooth、三个预设、custom 合法边界、非法值与字符串往返测试通过，公开头可自包含。
- [x] 1.3 将 swapchain image count、configured/effective max queued frames 和 frame-resource slot count 建模为不同字段；验证：类型/序列化测试证明三者不会互相覆盖或被 UI 混用。

## 2. 帧准入和 execution target

- [x] 2.1 在 inline/threaded `RenderRuntimeExecutionTarget` 增加统一 frame-admission 契约和可靠配置控制命令；验证：Main 在 FrameTimer/输入采样前获得 admission，native backend 对象不泄露跨 lane，配置请求不被普通帧合并丢弃。
- [x] 2.2 以 configured max queued frames 限制 Main 对 Render/backend 的领先量，并保留所有 frame-resource fence；验证：queue depth 1/2 的并发测试证明队列有界，GPU 真正落后时 resource fence 仍正确保护 allocator/back buffer/retirement。
- [x] 2.3 实现单调时钟 target FPS limiter 和 hitch recovery；验证：60/120/144 可达目标的 interval error、P95/P99 jitter 与 CPU 占用测试通过，无整帧 busy-spin，display 已晚于 deadline 时无重复等待。

## 3. D3D12 backend

- [x] 3.1 增加 tearing capability、frame-latency waitable object 和可测试 swapchain/present plan；验证：Synchronized、LowLatencySynchronized、Immediate supported/unsupported 精确产生 create/resize flags、sync interval、Present flags、maximum frame latency 和 effective state。
- [x] 3.2 将 waitable object 接入首帧及逐帧 admission，并用 RAII 管理 create/resize/recreate/shutdown 生命周期；验证：display wait 不再延后统一记入 frame-resource wait，resize 后 handle 有效且无泄漏/device removal。
- [x] 3.3 接入运行时 profile 切换；验证：interactive-smooth 使用 `Present(1,0)`，low-latency 保持同步并使用 effective latency 1，benchmark 支持时使用 `Present(0,ALLOW_TEARING)`、不支持时合法 fallback，切换和 resize 后状态一致。

## 4. Vulkan backend

- [x] 4.1 扩展纯 present-mode 选择函数；验证：Synchronized=`FIFO`、LowLatencySynchronized=`MAILBOX→FIFO`、Immediate=`IMMEDIATE→MAILBOX→FIFO` 的组合测试通过，诊断不把 MAILBOX 标成 Immediate。
- [x] 4.2 接入 max outstanding frame 控制和可用的 present wait/timing 能力；验证：支持时记录原生 admission source，不支持时以 acquire/in-flight 同步安全 fallback，queue depth 始终有界。
- [x] 4.3 通过安全 swapchain recreation 支持运行时 mode 变更；验证：rebuild 前后 requested/pending/effective generation 正确，只有 FIFO 的设备可以完成所有 profile fallback，resize/退出无 validation error。

## 5. 状态、Profiler 和 UI

- [x] 5.1 将完整 `FramePacingState` 值化到 backend state、`RenderFrameFeedback`、FrameProfiler metadata/report 和启动日志；验证：两后端、fallback、pending、generation 和 custom limiter 序列化测试通过，Main/UI 不查询 live backend。
- [x] 5.2 新增 `displayAdmissionWaitMs`、`frameLimiterWaitMs`、`frameResourceWaitMs`、`presentCpuMs` 和 submitted/outstanding depth；验证：逐帧总账口径明确，display、limiter、resource stall 不重复计数，历史报告版本兼容或显式升级。
- [x] 5.3 在 Game 调试 overlay/Editor 配置中显示并切换 profile，展示 requested/effective native mode、target FPS、queue depth、tearing/fallback/pending；验证：默认仍为 interactive-smooth，benchmark 有明显撕裂/功耗提示，无 Editor 构建不引入 ImGui。

## 6. 定向构建与验收

- [x] 6.1 构建 `windows-ci` 与 `windows-vulkan-only-ci` 受影响目标并运行 parser、present-plan、frame-admission、limiter、FramePacing、FrameProfiler、RenderFrameQueue、公开头和模块边界测试；验证：两个配置均通过，不运行 GPU 全套、完整 Demo、四个粒子 Demo或整仓哈希。
- [x] 6.2 使用同一 Release D3D12 二进制运行 interactive-smooth、low-latency、benchmark 的空场景 Editor 双视图、Game-only 和 standalone matched 样本；验证：每项 30 warmup/120 valid、footer complete、active mask 正确，保存平均/P95/P99、Main/Render/GPU、各 wait、queue depth 和 input-to-present。
- [x] 6.3 对 low-latency 比较 queue depth 1/2，对 custom 验证设备可区分的 60/120/144 FPS；验证：报告延迟与 hitch/吞吐取舍、limiter jitter 和 CPU 占用，不以平均 FPS 单独判定。
- [x] 6.4 验证 benchmark 在 D3D12 实际 sync interval 0、Vulkan 使用 IMMEDIATE 或明确 fallback，且显示节拍 wait 相对同步基线显著下降；验证：300 FPS和 2 ms resource wait 仅作参考，未达到时保存真实分项，不取消必要 fence或改画质。
- [x] 6.5 运行 D3D12/Vulkan 的 Preview、WaveWorks Ocean、profile 切换和 resize 短冒烟；验证：正常退出，无 validation error/device removal/非有限 pacing 值，RenderGraph、Feature、场景、shader 和模拟步长未改。
- [x] 6.6 更新帧节拍/架构文档与证据索引；验证：说明 profile 语义、线程所有权、FPS 口径、等待分类、fallback 和 benchmark 取舍，`openspec validate optimize-frame-pacing-and-present-mode --strict` 通过且只在全部门禁真实通过后报告完成。
