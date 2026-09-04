# Render Execution 跨线程协议

## 所有权

- Main lane 独占 GLFW 事件、Editor 状态、World/SceneSession、相机输入、ImGui frame build 与自动化状态。
- Render lane 通过 `RenderRuntimeExecutionTarget` 独占 backend、frame context、`RenderFrameCoordinator`、两个 `SceneRenderer`、GPU 上传安全点、Present、capture resolve 与 GPU 报告快照。
- Worker pool 仅录制通过 `GraphParallelRecordingContract` 审计的独立 command context；descriptor、PSO/cache、动态上传、Feature 可变状态与提交仍留在 Render lane。
- 本 change 没有独立 RHI thread。Graphics API 翻译、提交和 Present 都属于 Render lane。

## 每帧数据流

```text
Main N+1                                         Render N
Poll GLFW / build ImGui packet                   dequeue acceptance N
apply completed asset binding feedback           apply reliable controls
update World / camera / settings                 BeginFrame
extract or reuse immutable RenderSceneData       Game + optional Scene render
build immutable RenderFramePacket                draw copied UiDrawPacket
freeze FrameEnvelope                             capture / EndFrame / Present
wait only before submitting N+1   <----------    publish completion + latest stats
submit N+1                       ---------->      dequeue N+1
```

队列默认至多一项 waiting，加上一项 executing。接受后的 frame/control 严格 FIFO、恰好执行一次；队满施加背压，不用 latest-wins 丢帧。Main 可以在 Render N 执行时准备 N+1，但不能同时准备第二个未提交帧。

`FrameEnvelope` 冻结：

- `LogicalFrameId`、scene/data revision、scene/view epoch、settings revision；
- simulation time/step、Game/Scene camera 与 previous-camera 输入；
- 每视图 active 状态和完整 `RenderSettings` 值；
- `shared_ptr<const RenderFramePacket>`、复制后的 `UiDrawPacket` 及 texture leases；
- profiling/capture/report 请求和 Main 输入采样时间戳。

Render lane 完成后发布两条不同语义的通道：

- `RenderFrameCompletion`：可靠、有序，驱动 capture、max-frames 和退出；
- `RenderFrameFeedback`：容量一的 latest-only 统计，供 UI 使用，不能代替可靠控制 ack。

## 可靠控制

Resize、history reset、quality/settings revision、scene switch、capture、asset render work/import、报告、drain 和 stop 使用 `RenderControlCommand`。每条命令具有 command ID、目标逻辑帧、scene/view epoch 和 `BeforeFrame`/`AfterFrame` 边界；ack 必须逐字段匹配。过期、跳跃、错边界或执行异常在接受/等待端显式失败，不能被统计覆盖。

## UI 与资源保活

Main 在 `ImGui::Render` 后复制顶点、索引、draw commands 和纹理 lease；Render lane 不访问 Main 的可变 ImGui draw data。GLFW 回调、BeginFrame/packet build、Render draw、viewport descriptor replacement 和 Shutdown 共享 ImGui backend mutex。FramePacket、SceneData、asset binding、texture view 和 graph callback 都以 const shared lease 保活至对应 execution/completion 释放。

## 关闭与失败

正常关闭顺序为：停止接受新工作 → drain 已接受 frame/control → Render lane GPU wait → UI backend shutdown → Coordinator/renderer shutdown → backend 释放 → Main 释放 Scene/Editor/Window。队列失败保存首异常、取消未执行项并唤醒 frame/control producer、completion waiter 和 shutdown waiter。重复或并发 Shutdown 汇合到同一次完成状态。

## 配置、诊断与回退

默认配置为：

```text
PRISM_RENDER_EXECUTION_MODE=threaded
PRISM_RENDER_TASK_EXECUTOR=pool
```

低输入延迟排障或兼容路径：

```text
PRISM_RENDER_EXECUTION_MODE=inline
PRISM_RENDER_TASK_EXECUTOR=inline
```

两种模式共用同一 FrameEnvelope、RenderExecutionService target 和 GraphExecutor，不是两套渲染实现。FrameProfiler 分别记录 Main/Render/Worker、queue wait/depth、completion wait、N+1 overlap、完整帧身份和 input-to-Present。threaded 允许一帧 producer lag，可能提高吞吐但会增加约一帧输入延迟；不得只以线程数量或推算 FPS 宣称收益。

## P7 恢复检查点

P7 的可重放检查点由当前 OpenSpec tasks、本文协议、`ARCHITECTURE_REFACTOR_ACCEPTANCE.md`、Release 定向构建，以及 `artifacts/architecture-refactor/p7-mode-matrix-20260831/` 的模式/图像/Profiler 证据共同组成。按用户要求不再生成整仓源码副本或逐文件 SHA-256；恢复使用显式 `inline+inline` 环境后重新构建，不能覆盖用户工作区或既有 HPWater 证据。
