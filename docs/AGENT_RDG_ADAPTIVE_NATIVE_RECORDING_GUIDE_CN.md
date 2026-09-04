# Stage 17-P：自适应队列成本模型与原生命令并行录制

## 1. 本阶段解决什么问题

Stage 17-O 已经具备：

- 基于历史 GPU 总时间的 Serial/Native 选择；
- API 中立 `DeferredCommandContext` 并行录制；
- D3D12/Vulkan 独立 Queue Batch 提交。

但当时工作线程只生成公共命令描述，真正的
`ID3D12GraphicsCommandList` 或 `VkCommandBuffer` 仍由主线程回放。
成本模型也只有 EMA，无法处理分位数、过期历史、冷启动预测和多进程写入。

Stage 17-P 完成以下闭环：

1. 成本模型 v2：P50/P95、14 天过期、DAG 预测、受控探索、文件锁和原子保存；
2. D3D12 工作线程直接录制独立 Command List；
3. Vulkan 工作线程直接录制独立 Command Buffer；
4. Queue Batch 可以按确定顺序拼接多个原生命令流；
5. Harness 提供可调 Async Compute 重负载；
6. RDG v9 明确报告原生并行录制状态；
7. 双 API 编译、单元测试、实机 RDG、图像一致性和 A/B 全部自动验证。

重要结论：原生并行录制已经可运行，但当前测试图没有形成 GPU
Graphics/Compute overlap。强制 Native 的 GPU 总时间反而更高，所以
`auto` 保持保守是正确结果，不是功能失败。

## 2. 文件变化

### 2.1 新增文件

- `src/RHI/Vulkan/VulkanParallelCommandRecording.h`
- `src/RHI/Vulkan/VulkanParallelCommandRecording.cpp`
- `examples/harness/rdg_adaptive_native_recording.jsonl`
- `docs/AGENT_RDG_ADAPTIVE_NATIVE_RECORDING_GUIDE_CN.md`

### 2.2 主要修改文件

- `src/RHI/ICommandContext.h`
- `src/RHI/D3D12/D3D12CommandContextAdapter.h`
- `src/RHI/D3D12/D3D12CommandContextAdapter.cpp`
- `src/RHI/D3D12/D3D12Context.h`
- `src/RHI/D3D12/D3D12Context.cpp`
- `src/RHI/Vulkan/VulkanContext.h`
- `src/RHI/Vulkan/VulkanContext.cpp`
- `src/Renderer/RenderGraph.h`
- `src/Renderer/RenderGraph.cpp`
- `src/Renderer/RenderGraphDiagnostics.cpp`
- `src/Renderer/QueueSchedulingCostModel.h`
- `src/Renderer/QueueSchedulingCostModel.cpp`
- `src/Renderer/RenderCapture.h`
- `src/Renderer/D3D12SceneRenderer.cpp`
- `src/Renderer/VulkanSceneRenderer.cpp`
- `src/Automation/HarnessTools.cpp`
- `tests/RenderGraphTests.cpp`
- `tests/EngineHarnessTests.cpp`
- `CMakeLists.txt`

## 3. 公共 RHI 契约

`ICommandContext` 新增两个扩展点：

```cpp
virtual std::unique_ptr<IParallelCommandRecording>
    CreateParallelCommandRecording(CommandQueueType queue);

virtual bool AppendParallelCommandRecording(
    std::unique_ptr<IParallelCommandRecording> recording);
```

`IParallelCommandRecording` 只公开：

```cpp
virtual ICommandContext& GetCommandContext() = 0;
virtual bool Close() = 0;
```

设计含义：

- RDG 不知道后端使用 Command List 还是 Command Buffer；
- 工作线程仍调用同一套 `ICommandContext`；
- 后端负责命令分配器、命令池和 GPU 完成前的生命周期；
- 不支持此能力的后端返回空对象，RDG 继续使用 Deferred 回退路径；
- `CommandQueueCapabilities::nativeParallelCommandRecording` 用于能力发现。

这不是把原生 API 暴露给 Renderer，而是让后端提供“可并行录制的公共命令上下文”。

## 4. RDG 执行数据流

```text
RenderGraph::Compile
  -> 生成 Active Pass、依赖 DAG 和 Queue Batch
  -> 主线程规划每个 Pass 的 Barrier/Aliasing Prefix
  -> 为 parallelRecordable Pass 创建后端原生 Recording
  -> std::async 工作线程调用 Pass::contextExecute
  -> 工作线程关闭原生命令流
  -> 主线程等待所有 Recording
  -> BeginQueueBatch
       -> Replay Prefix
       -> Begin GPU Timestamp
       -> Append 原生 Pass 命令流
       -> End GPU Timestamp
       -> Replay Queue Handoff Tail
  -> EndQueueBatch
  -> FlushQueueBatches
  -> ResumeGraphicsQueue
```

为什么 Barrier 仍由主线程规划：

- RDG 全局资源状态是共享状态；
- 多线程同时修改资源状态会让结果依赖调度时序；
- Prefix/Tail 在主线程按编译顺序生成，可以保持确定性；
- Pass 主体通常占场景遍历、绑定和 Draw/Dispatch 录制的大部分 CPU 成本，
  适合并行。

## 5. D3D12 如何实现

### 5.1 每个工作项拥有独立对象

`D3D12ParallelCommandRecording` 为每个 Pass 创建：

- 一个 `ID3D12CommandAllocator`；
- 一个匹配队列类型的 `ID3D12GraphicsCommandList`；
- 一个绑定显式 Command List 的 `D3D12CommandContextAdapter`。

工作线程只访问自己的 Allocator 和 Command List。D3D12 要求同一个
Command Allocator 不能被多个线程同时录制，因此“一条录制一个 Allocator”
是必要条件。

### 5.2 Batch 不再只有一个 Command List

`PendingQueueBatch` 保存有序数组：

```text
[Prefix 0] [Pass 0 Native] [Tail 0 / Prefix 1]
[Pass 1 Native] [Batch Tail]
```

`AppendParallelCommandRecording` 的步骤：

1. 关闭当前主线程 Segment；
2. 把它加入 Batch 数组；
3. 加入已经关闭的工作线程 Command List；
4. 创建新的主线程 Segment，继续记录 Timestamp、Barrier 和后续 Pass；
5. Batch 提交时一次 `ExecuteCommandLists` 按数组顺序提交。

Allocator 和 Command List 被保留在当前 FrameContext 中，只有对应 Frame
Fence 完成后才释放。

## 6. Vulkan 如何实现

### 6.1 为什么每个线程需要独立 Command Pool

Vulkan 对 `VkCommandPool` 有外部同步要求。即使两个线程分配不同
`VkCommandBuffer`，也不能无锁地同时操作同一个 Pool。

因此 `VulkanParallelCommandRecording` 每次创建：

- 一个 `VK_COMMAND_POOL_CREATE_TRANSIENT_BIT` Command Pool；
- 一个 Primary Command Buffer；
- 一个只保存局部 Pipeline/Rendering 状态的公共命令上下文。

工作线程不修改 `VulkanContext` 的当前帧 Pipeline 状态，也不操作 Swapchain。

### 6.2 为什么使用 Primary Command Buffer

本实现需要兼容：

- Dynamic Rendering；
- Graphics 与 Compute Pass；
- 独立 Queue Batch；
- Prefix/Body/Tail 的简单确定顺序。

Primary Command Buffer 可以直接组成一次 `vkQueueSubmit` 的
`pCommandBuffers` 数组，不需要为 Secondary Command Buffer 额外维护
Render Pass Inheritance。代价是命令缓冲数量更多，但实现边界清晰。

### 6.3 生命周期

`AppendParallelCommandRecording` 把工作线程 Command Pool 转移给当前
FrameContext。该 Pool 不能在提交后立即销毁，因为它拥有的 Command Buffer
仍可能被 GPU 使用。

下一次复用这个 FrameContext 时：

1. 等待 `inFlight` Fence；
2. 销毁上次保留的独立 Command Pool；
3. 释放共享 Pool 中分配的 Segment Command Buffer；
4. 开始新一帧。

这保证了 CPU RAII 与 GPU 异步生命周期同时正确。

## 7. 成本模型 v2

### 7.1 身份键

历史按以下字段隔离：

```text
stage18-v2
  + graphicsApi
  + adapterName
  + width x height
  + graphSignature
```

不能把不同 GPU、API、分辨率或 RDG 拓扑的结果混在一起。

### 7.2 保存的数据

每个 Serial/Native 模式保存：

- Renderer 总 GPU 时间；
- Compute Busy；
- Graphics/Compute Overlap；
- 每个 Pass 的 GPU 时间；
- EMA；
- 最近 64 个样本；
- P50/P95；
- 最后更新时间；
- 图的 Pass 队列和依赖；
- 决策次数与迟滞状态。

### 7.3 三种决策来源

`exact_measurement`

- Serial 和 Native 都至少有两个新鲜样本；
- 以实测 P50 计算净收益；
- 只有净收益超过最小阈值才启用 Native。

`dag_pass_history`

- 当前图缺少完整 A/B，但相同 Pass 有历史；
- 在两个虚拟队列时间线上模拟依赖 DAG；
- 估算关键路径、潜在 overlap 和提交成本；
- 预测只能给出诊断，不能直接长期提升为 Native。

`controlled_native_probe`

- Serial 历史新鲜但 Native 样本不足；
- 每 120 次评估允许一次 Native 探针；
- 用于避免旧硬件/驱动结论永久固化；
- 探针结果仍需进入实测样本后才能形成稳定决策。

### 7.4 过期、锁和原子保存

- Renderer 样本超过 14 天视为过期；
- `<cost-model>.lock` 目录作为跨进程互斥锁；
- 5 分钟以上的锁视为崩溃遗留锁；
- 持锁后重新加载，再合并 Observe/Evaluate；
- 先写 `.tmp`，成功后 rename，避免半写 JSON；
- 未配置持久化路径时保留纯内存历史。

## 8. 重负载基准

Harness 参数：

```json
{
  "asyncWorkloadMultiplier": 8
}
```

有效范围为 1 到 64。它通过
`PRISM_RENDER_ASYNC_WORKLOAD_MULTIPLIER` 传给渲染进程，并重复 Bloom
Compute 工作，用来放大异步计算负载。

该参数被加入：

- `render.capture`
- `rdg.describe`
- `performance.measure`
- `performance.compare_queue_modes`

结果 JSON 会回显 multiplier，确保测试身份可追踪。

## 9. RDG v9 诊断

`compilation.queueInfrastructure` 新增：

```json
{
  "parallelCommandRecordingApplied": true,
  "nativeParallelCommandRecordingApplied": true,
  "parallelRecordedPassCount": 5,
  "nativeParallelRecordedPassCount": 5
}
```

`recordedCommandCount` 只统计 Deferred 中间命令数量。使用原生录制时它可以是
0，不能据此判断“没有录制命令”；应查看 `nativeParallelRecordedPassCount`。

CPU Trace 新增聚合 Span：

- `rdg.record/NativeCommandRecord`
- `rdg.record/DeferredCommandRecord`
- `rdg.submit/QueueBatchRecord`
- `rdg.submit/QueueBatchSubmit`

## 10. 自动验证

### 10.1 构建与单元测试

```powershell
cmake --build build-windows-ci --config Debug --parallel 8
ctest --test-dir build-windows-ci -C Debug --output-on-failure
```

结果：

```text
8/8 passed
```

测试覆盖：

- 成本模型实测正收益；
- P50/P95；
- 持久化重载；
- DAG Pass 历史预测不直接提升；
- 120 次受控探索；
- 多写入者合并；
- 无文件路径时的纯内存历史；
- RDG Deferred 回退路径；
- RDG v9 诊断字段。

### 10.2 双 API 实机验证

```powershell
.\build-windows-ci\Debug\PrismHarness.exe `
  --headless `
  --project-root . `
  --commands examples\harness\rdg_adaptive_native_recording.jsonl `
  --output automation\stage18-results.jsonl
```

本机验证环境：

- GPU：NVIDIA GeForce RTX 5060；
- D3D12 与 Vulkan；
- Debug；
- 640x360；
- `asyncWorkloadMultiplier=8`。

两端 RDG 报告：

```text
version                         = 9
queueExecutionMode              = dag_multi_queue
nativeParallelRecording         = true
nativeParallelRecordedPassCount = 5
queueBatchCount                 = 5
overlapOpportunityCount         = 1
```

图像比较：

```text
passed                   = true
meanAbsoluteError        = 0.0000640262
rootMeanSquareError      = 0.000512323
maximumChannelError      = 0.0196078
changedPixelRatio        = 0
```

CPU Trace 最大值：

```text
D3D12 NativeCommandRecord = 2.163 ms
D3D12 QueueBatchRecord    = 1.968 ms
D3D12 QueueBatchSubmit    = 2.052 ms
Vulkan NativeCommandRecord = 1.114 ms
Vulkan QueueBatchRecord    = 0.187 ms
Vulkan QueueBatchSubmit    = 0.113 ms
```

### 10.3 A/B 结论

重负载 A/B 确认两端都真正进入 Compute Queue，但采样中：

```text
overlapMilliseconds = 0
overlapRatio        = 0
```

Native 相比 Serial 的 Renderer GPU 中位数发生回归，因此
`comparison.passed=false`。这表明当前 DAG 的依赖与提交位置尚未允许
Graphics 工作覆盖 Compute 区间，增加工作量本身不会自动创造 overlap。

因此本阶段没有把预测结果强行当成成功收益；`auto` 只有在相同硬件、API、
分辨率和图签名下观测到实测正收益后才长期选择 Native。

## 11. 设计取舍与限制

### 已完成

- API 中立的原生并行录制契约；
- D3D12 独立 Allocator/Command List；
- Vulkan 独立 Command Pool/Command Buffer；
- Prefix/Body/Tail 确定顺序拼接；
- GPU Fence 前的对象保留；
- 成本模型 v2 与并发安全持久化；
- 重负载 A/B、CPU Trace、RDG v9 和双 API 图像回归。

### 仍有限制

- 当前使用 `std::async` 一 Pass 一任务，尚未使用固定渲染 Worker Pool；
- 原生 Recording 对象仍由主线程逐个创建，只有命令内容并行录制；
- 当前测试图没有实际 Graphics/Compute overlap；
- RHI 尚缺统一 Buffer/UAV Barrier、Copy Queue、上传环和延迟销毁队列；
- Descriptor 分配器还不是生产级并发、分代和回收模型；
- Native Command Recording 的收益目前只观测 CPU Span，尚未形成独立 CPU
  收益决策维度；
- Linux Vulkan 尚未验证。

## 12. 推荐学习顺序

1. 阅读 `ICommandContext.h` 的新契约和能力位。
2. 阅读 `RenderGraph::ExecuteQueueBatches`，区分 Prefix、Body、Tail。
3. 阅读 `D3D12ParallelCommandRecording` 的所有权。
4. 阅读 `D3D12Context::AppendParallelCommandRecording`。
5. 对照 Vulkan 的独立 Command Pool 实现。
6. 跟踪 Frame Fence 后如何销毁 Vulkan Pool。
7. 阅读 `QueueSchedulingCostModel::Observe`。
8. 阅读 `EvaluateUnlocked` 的实测、预测和探索分支。
9. 手算 `PredictFromPassHistory` 的双队列关键路径。
10. 查看 `stage18-*-native-rdg.json` 的 v9 字段。
11. 查看 CPU Trace 中不同线程的 NativeCommandRecord。
12. 运行 JSONL 并比较 Serial/Native GPU 时间轴。

## 13. 下一阶段

下一阶段进入生产级 RHI，优先顺序：

1. 统一 Buffer Barrier、UAV Barrier 和更完整的 Texture Subresource Barrier；
2. 增加 Copy Queue、Upload Ring、Readback Ring 和异步资源上传；
3. 增加 Fence/Timeline 驱动的延迟资源销毁；
4. 建立分代 Descriptor 分配、回收和容量诊断；
5. 统一 Device Feature/Limit 查询与错误传播；
6. 增加 Device Lost 诊断和后端恢复边界；
7. 用 Mock、D3D12、Vulkan 三层验证公共 RHI 契约。
