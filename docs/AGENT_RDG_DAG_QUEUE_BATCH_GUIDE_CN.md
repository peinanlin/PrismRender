# PrismRender Stage 17-N：依赖 DAG 与独立 Queue Batch 调度

## 1. 阶段目标

Stage 17-M 已经能够测量 Graphics/Compute 的公共 GPU 时间轴，但当时的执行方式仍按 Pass 声明顺序切换队列：

```text
Graphics -> Compute -> Graphics
```

这会把所有队列段串成一条链。即使两个 Pass 没有资源依赖，后提交的队列也不能覆盖前一个队列的工作。

Stage 17-N 将 RDG 执行升级为依赖 DAG 驱动的独立 Queue Batch：

1. 为 Active Pass 建立完整执行依赖。
2. 把资源别名复用加入执行依赖。
3. 删除已被其他路径覆盖的传递依赖。
4. 按跨队列边界把 Pass 合并为 Queue Batch。
5. 对 Batch DAG 做拓扑调度。
6. D3D12 使用每队列 Fence 提交独立命令列表。
7. Vulkan 使用每队列 Timeline Semaphore 提交独立命令缓冲。
8. 最终恢复 Graphics continuation，继续截图、ImGui 和 Present。
9. 用 GPU 时间轴与串行/多队列 A/B 判断优化是否真正盈利。

本阶段的重点是建立正确、可观察、可回退的调度基础。A/B 结果显示当前 640×360 启动场景的 Pass 太轻，强制多队列的提交成本仍高于收益，因此默认 `auto` 暂时选择串行，`native` 用于明确启用 DAG 多队列实验。

## 2. 文件变更

### 2.1 新增文件

- `docs/AGENT_RDG_DAG_QUEUE_BATCH_GUIDE_CN.md`
- `examples/harness/rdg_dag_queue_batches.jsonl`

### 2.2 主要修改文件

- `src/RHI/ICommandContext.h`
- `src/RHI/D3D12/D3D12Context.h`
- `src/RHI/D3D12/D3D12Context.cpp`
- `src/RHI/D3D12/D3D12CommandContextAdapter.h`
- `src/RHI/D3D12/D3D12CommandContextAdapter.cpp`
- `src/RHI/Vulkan/VulkanContext.h`
- `src/RHI/Vulkan/VulkanContext.cpp`
- `src/Renderer/RenderGraph.h`
- `src/Renderer/RenderGraph.cpp`
- `src/Renderer/RenderGraphDiagnostics.cpp`
- `src/Automation/HarnessTools.cpp`
- `tests/RenderGraphTests.cpp`
- `tests/EngineHarnessTests.cpp`
- `docs/LEARNING_GUIDE_CN.md`
- `docs/AGENT_RDG_QUEUE_TIMELINE_GUIDE_CN.md`

## 3. 总体数据流

```text
Pass 声明
  -> Liveness Dependencies
  -> Pass Culling
  -> RAW/WAR/WAW + Same Queue Dependencies
  -> Transient Aliasing Dependencies
  -> Transitive Reduction
  -> Cross Queue Sync Plan
  -> Queue Batch 切分
  -> Batch DAG Topological Schedule
  -> BeginQueueBatch(queue, waits)
  -> 录制并提交 Batch
  -> EndQueueBatch() 返回 QueueSyncPoint
  -> ResumeGraphicsQueue(final waits)
  -> 截图 / ImGui / Present
```

RDG 负责回答“哪些工作可以并行”和“需要等待谁”。RHI 后端只负责把统一 Batch 与同步点翻译成具体 API，不理解 Shadow、Hi-Z 或 Bloom 算法。

## 4. Pass 执行依赖

### 4.1 资源危险

`BuildExecutionDependencies()` 为 Active Pass 建立：

- RAW：后续读取等待前一次写入。
- WAR：后续写入等待之前的读取。
- WAW：后续写入等待前一次写入。
- Same Queue Order：同一队列的 Pass 保持稳定声明顺序。

同一队列顺序使命令提交具有确定性，也避免拓扑排序随容器迭代顺序变化。

### 4.2 资源别名依赖

两个瞬态纹理可以复用同一块 D3D12 Heap 或 Vulkan Memory，但后一个资源必须等前一个资源的最后一次访问完成。

`BuildAliasingDependencies()` 按物理别名槽排序资源生命周期，并添加：

```text
PreviousResource.LastUsePass
  -> NextResource.FirstUsePass
```

这样异步计算不会让原本只在串行执行下安全的别名资源发生真实并发。

### 4.3 传递约简

假设依赖为：

```text
A -> B -> C
A ------> C
```

`A -> C` 不再提供额外约束。`ReduceTransitiveDependencies()` 会删除这条边，保留：

```text
A -> B -> C
```

在当前高级场景中，它删除了类似以下冗余关系：

```text
DeferredLighting -> BloomHorizontal
DeferredLighting -> BloomExtract -> BloomHorizontal
```

结果是跨队列依赖从 5 条减少为 3 条，Queue Batch 从 6 个减少为 5 个。

## 5. Queue Batch 编译

### 5.1 为什么不是一个 Pass 一个提交

命令队列提交、Fence/Timeline Signal、驱动调度都有成本。一个 Pass 一个提交虽然最灵活，但会制造大量小提交。

RDG 在以下位置切分 Batch：

- 当前 Pass 有跨队列输入依赖。
- 前一个同队列 Pass 是跨队列生产者。
- 队列类型发生变化。

没有同步边界的连续同队列 Pass 被合并到一个 Batch。

### 5.2 Batch 依赖

每个 `QueueBatchDescription` 包含：

```cpp
batchIndex
queue
passes
dependencies
firstExecutionIndex
lastExecutionIndex
```

Batch 依赖来自：

- Batch 内 Pass 的执行依赖。
- 同一队列上一个 Batch，保证队列提交顺序稳定。

同一个 Batch 内的依赖不需要 GPU 同步原语，因为命令本身已经按顺序录制。

### 5.3 拓扑调度

调度器计算 Batch 入度并执行 Kahn 拓扑排序。多个 Batch 同时 Ready 时，优先选择与上一个提交不同的队列，再按原执行索引稳定排序。

当前场景的计划为：

```text
G0: Shadow + GBuffer
G1: DeferredLighting
G2: Tonemap + SceneColorReady
C0: HiZBuild
C1: BloomExtract + BloomHorizontal + BloomVertical
```

核心依赖为：

```text
G0 -> G1 -> C1 -> G2
 |              ^
 +----> C0      |
```

`G1` 与 `C0` 之间没有路径，因此 `DeferredLighting` 可以与 `HiZBuild` 重叠。

## 6. 公共 RHI 契约

`ICommandContext` 新增：

```cpp
bool BeginQueueBatch(
    CommandQueueType queue,
    std::span<const QueueSyncPoint> waits);

QueueSyncPoint EndQueueBatch();

bool ResumeGraphicsQueue(
    std::span<const QueueSyncPoint> waits);
```

`QueueSyncPoint` 只包含：

```cpp
CommandQueueType queue;
std::uint64_t value;
```

它表示“某个队列已经 Signal 到哪个单调递增值”。RDG 不持有 D3D12 Fence 或 Vulkan Semaphore，从而保持 API 中立。

`CommandQueueCapabilities::independentBatchSubmission` 用于区分：

- 旧的顺序式 `SwitchCommandQueue`。
- 新的独立 Batch 提交。

## 7. D3D12 后端

### 7.1 独立命令列表

第一批 Graphics 工作复用 `BeginFrame()` 已打开的 Direct Command List。后续 Batch 创建独立：

- `D3D12_COMMAND_LIST_TYPE_DIRECT`
- `D3D12_COMMAND_LIST_TYPE_COMPUTE`

Command Allocator 与 Command List 由当前 FrameContext 持有，直到帧 Fence 完成后才释放。

### 7.2 Fence 关系

Graphics 与 Compute 各自维护单调递增 Fence Value：

```text
Graphics Batch End
  -> GraphicsQueue.Signal(GraphicsFence, value)

Compute Batch Begin
  -> ComputeQueue.Wait(GraphicsFence, value)
```

反方向同理。等待被提交到 GPU Queue，不会阻塞 CPU。

### 7.3 Graphics continuation

所有 RDG Batch 提交后，`ResumeGraphicsQueue()`：

1. 创建新的 Direct Command List。
2. 等待最后的 Compute Sync Point。
3. 恢复 Graphics 为 Active Queue。
4. 继续录制截图、ImGui、BackBuffer Present Barrier。

最终 Frame Fence 位于 Graphics continuation 之后，因此也覆盖它等待的 Compute 工作。

## 8. Vulkan 后端

### 8.1 为什么使用两个 Timeline Semaphore

Stage 17-L 的顺序队列切换使用一个公共 Timeline Semaphore。独立 DAG Batch 不能继续让两个 Queue 同时向同一个 Timeline 值流随意 Signal，否则很难表达每个队列独立的完成进度。

Stage 17-N 为 Graphics 与 Compute 分别创建：

```text
GraphicsBatchTimeline
ComputeBatchTimeline
```

每个 Semaphore 只由所属 Queue Signal，其他 Queue 只 Wait。这与 D3D12 的“每队列一个 Fence”语义一致。

### 8.2 Batch 提交

`EndQueueBatch()` 将以下内容一次提交给目标 Queue：

- 当前 Batch 的 Command Buffer。
- 跨队列 Timeline Wait Values。
- 当前队列的新 Timeline Signal Value。
- 首个 Graphics Batch 所需的 Swapchain Image Available Semaphore。

后续 Graphics continuation 等待最终 Compute Timeline Value，再由 `EndFrame()` 提交并关联 Frame Fence。

### 8.3 命令缓冲生命周期

每个 Batch Command Buffer 记录到 `retiredCommandBuffers`。只有当前 Frame Fence 完成后，下一次复用 FrameContext 时才释放，避免 CPU 提前回收 GPU 仍在执行的命令内存。

## 9. 资源状态与队列交接

### 9.1 跨队列纹理

生产 Batch 结束前，RDG 将需要交给其他队列的纹理转到 `Common`。消费 Batch 在自己的命令缓冲中转到声明状态，例如：

```text
DepthWrite
  -> Common
  -> ShaderResource
```

Fence 或 Timeline Wait 保证第二个 Barrier 不会早于第一个 Barrier。

### 9.2 Compute 终止资源

如果纹理最后一次访问发生在 Compute Queue，RDG 在 Compute Batch 末尾把它交回 `Common`，然后在 Graphics continuation 中恢复到最终声明状态。

这保证下一帧导入 `DepthBuffer`、`HiZ` 等持久纹理时，CPU 侧状态记录与真实 GPU 状态一致。

### 9.3 当前限制

跨队列共享只读资源目前采用保守依赖。后续可增加“生产者预转换到公共只读状态”，让多个队列在相同只读状态下并发访问，并为 Vulkan Queue Family Ownership 提供更精确的 Release/Acquire Barrier。

## 10. 执行模式与收益策略

三种模式含义：

| 模式 | 当前行为 |
| --- | --- |
| `serial` | 所有 Pass 在 Graphics Queue 顺序执行 |
| `native` | 明确启用独立 DAG Queue Batch |
| `auto` | 暂时选择串行，等待 GPU 成本模型完成 |

不能只凭“DAG 中存在可并行节点”判断异步计算必然更快。还必须比较：

```text
可覆盖的 Graphics 时间
Compute 工作时间
Queue Submit / Fence / Semaphore 成本
资源 Barrier 与缓存干扰
GPU 是否真的具有可并发执行能力
```

当前 `native` 是功能与性能实验入口，`auto` 是面向普通运行的保守入口。

## 11. RDG Report v6

报告新增：

```json
{
  "compilation": {
    "queueExecutionMode": "dag_multi_queue",
    "queueInfrastructure": {
      "independentBatchSubmission": true,
      "dagQueueBatchExecutionApplied": true
    },
    "queueBatchPlan": {
      "batchCount": 5,
      "crossQueueDependencyCount": 3,
      "overlapOpportunityCount": 1
    }
  },
  "queueBatches": []
}
```

`overlapOpportunityCount` 是静态 DAG 结论，`gpuTimings.timeline.overlapMilliseconds` 是实际硬件测量。两者必须分开理解。

Harness `engine.describe` 同时公开：

```text
rdgDagQueueBatches
independentQueueBatchSubmission
```

Agent 可以先检查能力，再请求 `rdg.describe(queueMode=native)`。

## 12. 自动化验证

### 12.1 编译与测试

```powershell
cmake --build build-windows-ci --config Debug --parallel 8
ctest --test-dir build-windows-ci -C Debug --output-on-failure
```

结果：8/8 测试通过。

`RenderGraphTests` 新增分叉与汇合图：

```text
RootProduce
  +-> GraphicsBranch -+
  +-> ComputeBranch  -+-> Join
```

测试验证：

- 4 个独立 Batch。
- Compute 与 Graphics Branch 无直接依赖。
- Compute Batch 等待 Root Graphics Sync Point。
- Join Graphics Batch 等待 Compute Sync Point。
- 最终 Graphics continuation 等待两队列最后进度。
- RDG Report 为 v6 和 `dag_multi_queue`。

### 12.2 双 API 实机场景

在 NVIDIA GeForce RTX 5060、Debug、640×360 启动场景下：

| 项目 | D3D12 | Vulkan |
| --- | ---: | ---: |
| RDG Batch 数 | 5 | 5 |
| 跨队列 Batch 依赖 | 3 | 3 |
| 静态重叠机会 | 1 | 1 |
| `rdg.describe` 进程退出 | 0 | 0 |
| 单次观察到的重叠 | 0 ms | 0.014816 ms |
| Vulkan 单次 Compute 覆盖率 | - | 20.96% |

跨 API Tonemap Golden Image 在 `queueMode=native` 下通过，D3D12/Vulkan 子进程退出码均为 0。

### 12.3 串行/多队列 A/B

3 个正式样本、1 个预热样本的中位数：

| API | Serial Renderer GPU | Native Renderer GPU | Native Compute Busy | Native Overlap |
| --- | ---: | ---: | ---: | ---: |
| D3D12 | 0.167840 ms | 3.472672 ms | 0.042496 ms | 0 ms |
| Vulkan | 0.157280 ms | 0.760992 ms | 0.062496 ms | 0 ms |

这组轻量场景中多队列未盈利。一次 Vulkan `rdg.describe` 能观察到短暂重叠，但多样本中位数仍为 0，因此不能宣称稳定性能收益。

报告保存在：

- `automation/reports/stage17n-d3d12-rdg.json`
- `automation/reports/stage17n-vulkan-rdg.json`
- `automation/reports/stage17n-d3d12-queue-ab.json`
- `automation/reports/stage17n-vulkan-queue-ab.json`

## 13. 设计取舍

### 已完成

- Pass DAG、别名依赖与传递约简。
- 独立 Queue Batch 编译和稳定拓扑调度。
- D3D12 每队列 Fence 提交。
- Vulkan 每队列 Timeline Semaphore 提交。
- 跨队列资源状态交接。
- Graphics continuation。
- v6 诊断、能力发现、双 API 实机与 Golden Image 验证。

### 未伪装成已完成

- 当前默认场景没有稳定异步计算收益。
- 本阶段尚未并行录制 Command List/Command Buffer；Stage 17-P 后续已完成。
- 本阶段尚未建立基于历史 GPU 时间的自动收益模型；Stage 17-O/P 后续已完成。
- 尚未实现共享只读资源的最小化 Release/Acquire。
- 尚未在重负载 Benchmark 场景中验证多队列收益。

上面的“本阶段”指 Stage 17-N 的历史边界。当前版本已经具备 P50/P95
历史成本、受控探索、D3D12/Vulkan 原生命令并行录制和重负载 A/B 入口；
共享只读资源的最小化 Handoff 仍属于进一步的同步优化，不影响正确性。

## 14. 推荐学习与实施顺序

1. 阅读 `RenderGraph::BuildExecutionDependencies()`，理解 RAW/WAR/WAW。
2. 阅读 `BuildAliasingDependencies()`，理解内存复用为什么也是执行依赖。
3. 阅读 `ReduceTransitiveDependencies()`，手工画出约简前后的 DAG。
4. 阅读 `BuildQueueBatches()`，观察 Batch 切分与拓扑排序。
5. 阅读 `ExecuteQueueBatches()`，跟踪 Wait、Signal 和 Graphics continuation。
6. 阅读 `ICommandContext.h`，理解 API 中立同步点。
7. 阅读 D3D12 的 `BeginQueueBatch/EndQueueBatch/ResumeGraphicsQueue`。
8. 阅读 Vulkan 对应实现，比较 Fence 与 Timeline Semaphore。
9. 打开 RDG v6 报告，对照 Pass、Batch、Dependency 和 Queue Sync。
10. 运行 `performance.compare_queue_modes`，理解“可并行”不等于“更快”。

## 15. 下一阶段

下一阶段应实现基于 GPU 历史成本的调度收益模型：

1. 为 Pass 保存稳定的 GPU 时间 EMA/P50/P95。
2. 估算每个异步分支可被 Graphics 覆盖的时间。
3. 估算 Queue Submit、同步和 Barrier 固定成本。
4. 只有预计净收益为正时，`auto` 才启用 DAG 多队列。
5. 支持并行录制 Queue Batch，并在录制完成后集中提交，减少轻量 Pass 间的 CPU 提交空洞。
6. 增加可调工作量的异步计算 Benchmark 场景，建立不同 GPU 的收益阈值。

完成这些后，`auto` 才能从当前的安全串行策略升级为硬件与场景自适应策略。
