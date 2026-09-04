# Stage 17-O：历史 GPU 成本模型与并行命令录制

## 1. 本阶段解决什么问题

Stage 17-N 已经能把 RDG 编译成独立 Graphics/Compute Queue Batch，但“图上可以并行”不代表“运行起来更快”。

轻量场景中，多队列会增加：

- Command List/Command Buffer 切分成本；
- Queue Submit 与 Fence/Timeline Semaphore 成本；
- 跨队列 Barrier 和等待成本；
- CPU 逐 Batch 录制、提交造成的间隙。

Stage 17-O 完成两个闭环：

1. 保存相同 GPU、API、分辨率和 RDG 图的历史串行/多队列 GPU 成本；
2. 先并行生成 API 中立命令流，再集中提交原生命令批次。

`auto` 不再等同于固定串行。它只会在历史数据完整且预计净收益超过阈值时选择多队列。

## 2. 文件清单

### 2.1 新增文件

- `src/Renderer/QueueSchedulingCostModel.h`
- `src/Renderer/QueueSchedulingCostModel.cpp`
- `src/RHI/DeferredCommandContext.h`
- `src/RHI/DeferredCommandContext.cpp`
- `automation/tests/stage17o-rdg-commands.jsonl`
- `automation/tests/stage17o-parity-command.jsonl`
- `docs/AGENT_RDG_COST_MODEL_PARALLEL_RECORDING_GUIDE_CN.md`

### 2.2 主要修改文件

- `src/Renderer/RenderGraph.h/.cpp`
- `src/Renderer/RenderGraphDiagnostics.cpp`
- `src/Renderer/GpuProfiler.h/.cpp`
- `src/Renderer/D3D12SceneRenderer.h`
- `src/Renderer/D3D12SceneRenderer.cpp`
- `src/Renderer/VulkanSceneRenderer.h/.cpp`
- `src/RHI/ICommandContext.h`
- `src/RHI/D3D12/D3D12Context.h/.cpp`
- `src/RHI/D3D12/D3D12CommandContextAdapter.h/.cpp`
- `src/RHI/Vulkan/VulkanContext.h/.cpp`
- `src/Automation/HarnessTools.cpp`
- `tests/RenderGraphTests.cpp`
- `tests/EngineHarnessTests.cpp`
- `CMakeLists.txt`

## 3. 总体数据流

```text
GpuProfiler 完成一帧 Timestamp 解析
  -> Renderer 根据 resolved frame index 找到该帧的图签名和实际执行模式
  -> QueueSchedulingCostModel::Observe
  -> 写入 serial/native EMA 和各 Pass 历史
  -> 下一帧 RenderGraph::Compile
  -> 生成稳定 graphSignature
  -> QueueSchedulingCostModel::Evaluate
  -> RenderGraph::SetAutomaticQueueDecision
  -> auto 选择 serial 或 DAG multi-queue
```

多队列录制与提交路径：

```text
RenderGraph 主线程
  -> 按拓扑顺序规划资源状态、Aliasing Barrier 和 Queue Handoff
  -> parallelRecordable Pass 进入独立 DeferredCommandContext
  -> std::async 并行执行 Pass 的前端录制
  -> 等待所有前端命令流完成
  -> 按 Queue Batch 拓扑顺序回放到原生命令上下文
  -> D3D12/Vulkan EndQueueBatch 只关闭并保存批次
  -> FlushQueueBatches 集中提交全部批次
  -> ResumeGraphicsQueue 录制帧尾工作
```

## 4. 历史 GPU 成本模型

### 4.1 为什么使用运行时身份和图签名

同一套渲染代码在不同 GPU、分辨率和 Pass 配置下可能得到相反结论。因此历史 Key 为：

```text
stage17o-v1
| graphicsApi
| adapterName
| width x height
| graphSignature
```

`graphSignature` 使用活动 Pass 的以下信息生成 64 位 FNV-1a Hash：

- Pass 名；
- Graphics/Compute Queue；
- 执行依赖；
- 资源读写名称；
- 资源状态。

关闭 Bloom、切换 Forward/Deferred 或改变依赖图后，会使用新的历史分组，不会错误复用旧结论。

### 4.2 保存哪些数据

每个 Key 分别保存 `serial` 和 `native`：

- Renderer 总 GPU 时间 EMA；
- Compute Busy 时间 EMA；
- Graphics/Compute 实际重叠时间 EMA；
- 每个 RDG Pass 的 GPU 时间 EMA；
- 样本数；
- 上一次自动决策。

默认文件：

```text
automation/cache/rdg-queue-cost-model.json
```

可用环境变量覆盖：

```text
PRISM_RENDER_QUEUE_COST_MODEL_PATH
```

Stage 17-O 实机验证使用独立文件：

```text
automation/reports/stage17o-queue-cost-model.json
```

### 4.3 EMA

EMA 系数为 `0.25`：

```text
newEma = oldEma * 0.75 + sample * 0.25
```

EMA 能跟随驱动、场景负载和温度变化，又不会因单帧抖动立即反转调度策略。

### 4.4 auto 决策规则

至少需要：

```text
serial samples >= 2
native samples >= 2
static overlap opportunity > 0
```

然后计算：

```text
estimatedNetBenefit = serialRendererEma - nativeRendererEma
requiredBenefit = max(0.05 ms, serialRendererEma * 3%)
```

决策原因包括：

- `history_unavailable`
- `no_static_overlap_opportunity`
- `insufficient_history`
- `estimated_negative_benefit`
- `benefit_below_enable_threshold`
- `estimated_positive_benefit`
- `positive_benefit_hysteresis`
- `forced_serial`
- `forced_native`

因此：

- `serial/native` 是采样和调试用的强制模式；
- `auto` 在冷启动或负收益时保持串行；
- 只有净收益为正并超过启用阈值时，`auto` 才进入 DAG 多队列；
- 已启用后，只要收益仍为正就使用迟滞保持，避免阈值附近频繁切换。

## 5. GPU Timing 如何对应回原来的帧

GPU Timestamp 通常在后续帧才可读。只保存“最新 Timing”无法知道它属于哪个 RDG 图和执行模式。

`GpuProfiler::FrameData` 新增：

```cpp
std::uint64_t submissionGeneration;
```

解析完成后公开：

```cpp
GetLatestTimingGeneration();
GetLatestResolvedFrameIndex();
```

两个 Renderer 按 Frame Slot 保存：

```text
graphSignature
nativeMultiQueue
valid
```

只有新的 `submissionGeneration` 才会写入模型，从而避免同一帧被重复采样，也避免把旧 GPU 时间记录到新图上。

## 6. API 中立并行命令流

### 6.1 DeferredCommandContext 的职责

`DeferredCommandContext` 实现公共 `ICommandContext`，但不立刻调用 D3D12/Vulkan。它把命令保存为有序操作：

- Begin/EndRendering；
- Bind Graphics/Compute Pipeline；
- Bind Vertex/Index Buffer；
- Bind DescriptorSet 和动态 Offset；
- Draw/DrawIndexed；
- Dispatch；
- Texture、TextureView、Aliasing Barrier。

完成后使用：

```cpp
deferredContext.Replay(nativeContext);
```

回放前会验证：

- Graphics API 相同；
- 当前 Queue 类型相同；
- Rendering Scope 已正确闭合。

### 6.2 为什么需要显式 opt-in

`RenderGraph::PassOptions` 新增：

```cpp
bool parallelRecordable = false;
```

只有满足以下条件的 Pass 才能设为 `true`：

- 回调只通过传入的 `ICommandContext` 生成命令；
- 不修改 World、资源注册表或 RenderGraph；
- 不执行截图、文件 IO 或窗口操作；
- 读取的 Pipeline、DescriptorSet、Buffer、Texture 在录制期间保持有效；
- 不依赖另一个 Pass 回调产生的 CPU 副作用。

当前开放并行录制的高级 Pass：

- DeferredLighting；
- HiZBuild；
- BloomExtract；
- BloomHorizontal；
- BloomVertical。

Shadow、GBuffer、Tonemap 和 SceneColorReady 仍保持主线程录制，因为它们包含场景遍历、原生上下文依赖或帧尾副作用。

### 6.3 为什么 Barrier 仍由主线程规划

资源状态是全图共享状态。若多个工作线程同时修改 `m_textureResources`，结果会依赖线程调度。

所以执行顺序为：

1. 主线程按拓扑顺序执行读前写校验；
2. 主线程生成 Aliasing/Texture Barrier 前缀；
3. 工作线程只生成 Pass Body 命令流；
4. 主线程生成 Queue Handoff 尾部；
5. 最后按确定顺序回放。

这样并行的是 Pass 的 CPU 前端命令生成，资源状态机仍然是确定性的。

## 7. 集中原生 Batch 提交

### 7.1 D3D12

`EndQueueBatch` 现在只做：

1. Close Command List；
2. 预留 Fence Value；
3. 保存 Wait、Signal、Command List 和 Command Allocator；
4. 不立即调用 `ExecuteCommandLists`。

`FlushQueueBatches` 再按 DAG 拓扑顺序：

1. 应用跨队列 Wait；
2. `ExecuteCommandLists`；
3. Signal 对应 Graphics/Compute Fence；
4. 将 Command List 和临时 Command Allocator 保留到 FrameContext。

Command Allocator 必须与 Command List 一起保活。否则延迟提交期间分配器可能提前释放，产生 GPU Validation 错误或设备丢失。

### 7.2 Vulkan

`EndQueueBatch` 关闭并保存 VkCommandBuffer、Wait 和 Timeline Signal Value。

`FlushQueueBatches` 集中构造 `VkTimelineSemaphoreSubmitInfo` 与 `VkSubmitInfo`：

- 第一个 Graphics Batch 等待 `imageAvailable`；
- 跨队列依赖等待生产 Queue 的 Timeline Semaphore；
- 每个 Batch Signal 自己 Queue 的 Timeline Semaphore；
- 最终 Graphics continuation 等待 Compute 的最后进度。

### 7.3 当前并行录制的边界

Stage 17-O 并行的是 API 中立前端命令流，原生 API 调用在回放阶段保持确定顺序。这样能先安全并行场景遍历和命令描述生成，并共享 D3D12/Vulkan 实现。

更进一步的“原生并行录制”需要：

- D3D12 每个 Worker 独立 Command Allocator/Command List；
- Vulkan 每个 Worker Thread 独立 Command Pool/Command Buffer；
- Descriptor 分配器线程安全或使用线程局部分配器；
- Secondary Command Buffer/Bundle 的 Rendering 继承规则；
- 原生 Batch 合并与更细的 CPU Trace。

## 8. RDG Report v7

报告新增：

```text
graphSignature
queueInfrastructure.deferredBatchSubmission
queueInfrastructure.deferredBatchSubmissionApplied
queueInfrastructure.parallelCommandRecordingApplied
queueInfrastructure.parallelRecordedPassCount
queueInfrastructure.recordedCommandCount
automaticQueueDecision
passes[].parallelRecordable
```

`automaticQueueDecision` 包含样本数、串行/原生估值、净收益、提交成本、重叠时间、启用阈值、置信度和原因。

Harness `engine.describe` 新增能力：

```text
rdgHistoricalGpuCostModel
parallelCommandRecording
deferredQueueBatchSubmission
```

## 9. 自动化测试

`RenderGraphTests` 新增覆盖：

- Deferred Batch Flush；
- 两个可并行 Branch 的命令流录制与回放；
- `parallelRecordedPassCount` 和 `recordedCommandCount`；
- 默认 `auto` 在无历史时保持串行；
- 正收益历史决策能让 `auto` 进入 DAG 多队列；
- 成本模型 JSON 保存与重新加载；
- RDG Report v7 字段。

`EngineHarnessTests` 验证 Stage 17-O 能力发现字段。

构建与测试：

```powershell
cmake --build build-windows-ci --config Debug --parallel 8
ctest --test-dir build-windows-ci -C Debug --output-on-failure
```

结果：

```text
8/8 tests passed
```

## 10. 双 API 实机结果

环境：

```text
GPU: NVIDIA GeForce RTX 5060
Build: Debug
Resolution: 640 x 360
Scene: StartupScene.gltf
```

每个 API 执行两次 `serial`、两次 `native`，再执行一次 `auto`。

| API | Serial Renderer EMA | Native Renderer EMA | Native Overlap EMA | auto |
| --- | ---: | ---: | ---: | --- |
| D3D12 | 约 0.41 ms | 约 5.30 ms | 0 ms | serial |
| Vulkan | 约 0.93 ms | 约 1.91 ms | 0 ms | serial |

两个 `auto` 报告均为：

```text
reason = estimated_negative_benefit
historyAvailable = true
selectNative = false
```

这正是本阶段需要的行为：当前轻量场景中多队列没有收益，自动模式不会为了“使用异步计算”而强行启用异步计算。

强制 `native` 结果：

| 项目 | D3D12 | Vulkan |
| --- | ---: | ---: |
| Queue Batch | 5 | 5 |
| 并行前端录制 Pass | 5 | 5 |
| 录制命令 | 67 | 67 |
| Deferred Batch Flush | 成功 | 成功 |
| 子进程退出码 | 0 | 0 |

跨 API Tonemap 对比：

```text
passed = true
meanAbsoluteError = 0.000064026
rootMeanSquareError = 0.000512323
maximumChannelError = 0.0196078
changedPixelRatio = 0
```

主要产物：

- `automation/stage17o-results.jsonl`
- `automation/stage17o-parity-results.jsonl`
- `automation/reports/stage17o-queue-cost-model.json`
- `automation/reports/stage17o-d3d12-auto-rdg.json`
- `automation/reports/stage17o-vulkan-auto-rdg.json`
- `automation/captures/stage17o-native-d3d12.bmp`
- `automation/captures/stage17o-native-vulkan.bmp`

## 11. 设计取舍

### 已完成

- 按硬件、API、分辨率和图签名隔离历史；
- Serial/Native GPU 总成本和每 Pass EMA；
- 自动正收益阈值与迟滞；
- GPU Timing 到原提交帧的稳定关联；
- API 中立前端并行命令录制；
- D3D12/Vulkan 延迟集中 Batch 提交；
- v7 结构化诊断；
- 双 API 实机和图像一致性验证。

### 当前限制

- 冷启动不会主动试探 Native，必须先通过 `serial/native` 或 `performance.compare_queue_modes` 采样；
- 当前模型以实测 Renderer 总时间为最终收益依据，尚未按 DAG 逐分支预测未见过的图；
- 样本保存为 EMA，尚未保存 P50/P95 分布；
- 目前不是原生 Command List/Command Buffer 多线程录制；
- 默认场景过轻，无法证明 Async Compute 在重负载场景有正收益；
- JSON 文件还没有跨进程文件锁。

## 12. 推荐学习顺序

1. 阅读 `QueueSchedulingCostModel.h`，理解输入、历史身份和决策输出。
2. 阅读 `QueueSchedulingCostModel::Observe`，跟踪 Renderer、Pass、Compute Busy 和 Overlap。
3. 阅读 `Evaluate`，手算净收益、阈值和迟滞。
4. 打开 Stage 17-O 成本模型 JSON，对照两个 API 的真实数据。
5. 阅读 `GpuProfiler::FrameData::submissionGeneration`。
6. 阅读两个 Renderer 的 `ObserveCompletedQueueTimings`。
7. 阅读 `RenderGraph::BuildGraphSignature`。
8. 阅读 `DeferredCommandContext` 的 Record/Replay。
9. 阅读 `ExecuteQueueBatches` 的主线程规划、异步录制和确定性回放。
10. 阅读 D3D12 `EndQueueBatch/FlushQueueBatches`，重点理解 Allocator 生命周期。
11. 阅读 Vulkan 对应实现，比较 Fence 与 Timeline Semaphore。
12. 打开 RDG v7 报告，核对决策、并行 Pass、命令数和 Batch。
13. 运行 Stage 17-O JSONL 命令，观察 `auto` 如何根据历史改变结果。

## 13. 下一阶段

以下事项已经由 Stage 17-P 完成，详细实现与实机数据见
`docs/AGENT_RDG_ADAPTIVE_NATIVE_RECORDING_GUIDE_CN.md`：

1. 可调工作量的重负载 Async Compute Benchmark；
2. DAG 分支与逐 Pass 历史预测；
3. 受控 Native 探索；
4. P50/P95、历史过期和跨进程锁；
5. D3D12/Vulkan 原生命令并行录制；
6. 原生录制与 Queue Submit CPU Trace；
7. 仅由实测正收益提升 `auto` 决策。

Stage 17-P 的 A/B 结果也证明：当前图没有形成实际 GPU overlap，
所以强制 Native 仍然回归。后续优化必须以测量结果为准。
