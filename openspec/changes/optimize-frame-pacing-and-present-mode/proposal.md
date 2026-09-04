## Why

当前 D3D12 后端固定使用 `Present(1, 0)`，Release 空场景中约 1–3 ms 的实际 CPU 渲染工作最终表现为约 15–16 ms 的 frame-resource fence 等待，Editor 双视图和 Game-only 因此都只有约 56 FPS。该数据能证明应用受显示节拍反压，但不能推出“默认关闭 VSync”是正确产品策略：显示同步、软件限帧、CPU/GPU 排队深度、资源复用 fence 和性能测试模式是不同问题。

Unity 和 Unreal 的共同工程实践是将这些维度独立配置，并为日常交互、低延迟和性能测试提供不同策略。PrismRender 需要同样明确的帧节拍契约，使空场景分析既能测出真实渲染吞吐，又不以默认撕裂、更高功耗或删除必要同步换取 FPS。

## What Changes

- 新增跨后端 `FramePacingConfiguration`，独立描述 presentation intent、可选 target FPS 和最大排队帧数，禁止用单个 VSync 开关隐式控制全部节拍行为。
- 提供 `interactive-smooth`、`low-latency`、`benchmark` 三个预设和 `custom` 配置；默认保持无撕裂的 `interactive-smooth`，`benchmark` 必须显式启用。
- D3D12 使用 flip-model frame-latency waitable object 和最大帧延迟控制帧准入；现有 GPU fence 继续只负责 frame resource 生命周期。支持时预创建 tearing-capable swapchain，使运行时切换到 benchmark 不需要非法 flag 组合。
- Vulkan 按语义映射 present mode：平滑模式使用 `FIFO`，低延迟无撕裂模式优先 `MAILBOX`，benchmark 优先 `IMMEDIATE`；能力不足时确定性回退并报告实际模式。
- 新增统一帧准入边界，使 threaded/inline 路径在采样输入和构建下一帧前等待显示系统或目标帧率 deadline，而不是把后续 frame-resource fence stall 当作正常的 VSync 实现。
- 支持在 Editor/Game 调试 UI 和启动配置中选择预设；运行时变更通过 render execution target 传递，Vulkan 在安全帧边界重建 swapchain，D3D12 在兼容创建标志下切换 Present 参数。
- Profiler、日志和 overlay 分别报告显示准入等待、软件限帧等待、frame-resource fence 等待、Present CPU 时间、requested/effective 模式和队列深度。
- 空场景验收同时覆盖默认平滑、低延迟和 benchmark；300 FPS 仅为当前硬件 benchmark 参考，不是完成门禁。

## Capabilities

### New Capabilities

- `rhi/presentation-pacing`: 规定跨 D3D12/Vulkan 的帧节拍配置、预设、帧准入、能力回退、运行时变更、诊断和空场景验收。

### Modified Capabilities

无。

## Impact

- 主要影响 `src/RHI` 的 swapchain 创建、resize、present 和 frame-admission 接口，`src/Renderer` 的 execution target/feedback，`src/Core/ApplicationHost.cpp` 的帧开始边界，以及 `src/UI` 的配置与诊断。
- 默认呈现行为保持同步、无撕裂；显式 benchmark 才允许 D3D12 tearing 或 Vulkan `IMMEDIATE`。
- 不修改 RenderGraph、Feature、shader、场景数据、默认画质或模拟算法；空场景中无有效工作却仍执行的 shadow/GTAO/SSR/bloom/HiZ pass 另行分析，不混入本 change。
- swapchain image count、最大排队帧数和 frame-resource slot 数量保持为独立概念；实现不得通过取消必要 fence 或无限堆积工作提高表面 FPS。
- 验收使用完整 application loop interval，并同时保存 CPU work、GPU work、各类 wait、输入至呈现延迟和 effective presentation state。
