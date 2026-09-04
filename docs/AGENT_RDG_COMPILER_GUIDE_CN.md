# PrismRender Stage 17-J：RDG 编译、Pass Culling、资源生命周期与队列计划

> 当前状态更新：Stage 17-K 将别名规划接入原生后端；Stage 17-L 至
> 17-P 又完成 Compute Pass、跨队列 Timestamp、DAG Queue Batch、历史
> 成本模型和 D3D12/Vulkan 原生命令并行录制；Stage 20/27 完成强类型资源
> 模型与 Texture/Buffer Native Alias。本文保留 Stage 17-J 的编译器演进
> 过程，当前实现还应结合后续 `AGENT_RDG_*` 与
> `RDG_RESOURCE_MODEL_GUIDE_CN.md` 阅读。

## 1. 阶段目标

Stage 17-J 将原来的顺序 `RenderGraph` 升级为可编译 RDG 基础版。

原实现已经能够：

- 声明 Pass 读写资源。
- 检查读前是否已导入或写入。
- 自动生成整纹理 Barrier。
- 记录 CPU/GPU Pass 时间。
- 通过 Harness 查询 Pass 和资源。

但它仍然会按照添加顺序执行所有 Pass，也不知道资源何时开始和结束使用。
因此无法回答：

- 哪些 Pass 对最终输出没有贡献。
- 哪些资源可以被裁剪。
- GBuffer、HDR 和 Bloom 的生命周期是否重叠。
- 哪些逻辑纹理未来可以共享物理显存。
- Graphics 和 Compute Pass 之间需要哪些同步。

本阶段完成：

1. 显式输出根 `MarkOutput`。
2. Pass Side Effect 和禁止裁剪标记。
3. 生产者依赖分析。
4. 从输出根反向执行 Pass Culling。
5. 活跃 Pass 执行索引。
6. RAW、WAR、WAW 执行依赖。
7. 瞬态纹理 `FirstUse/LastUse` 生命周期。
8. 兼容资源的别名槽规划。
9. Graphics/Compute Queue 调度和跨队列同步计划。
10. RDG Report v2。
11. Harness 能力声明。
12. D3D12/Vulkan 真实高级渲染链接入。

需要明确以下内容描述的是 Stage 17-J 完成时的状态：

- Pass Culling 已经真实影响执行，裁剪的 Pass 不会提交命令。
- 生命周期和别名槽已经对真实 D3D12/Vulkan 纹理计算。
- 当时的别名结果只是规划，尚未创建 D3D12 Placed Resource 或 Vulkan Alias Memory；该项已在 Stage 17-K 完成。
- 当前 Queue 结果是调度计划，仍通过单一 Graphics Context 顺序执行。

报告会分别使用：

```text
physicalAliasingApplied = false
queueExecutionMode = serial_fallback
```

避免把规划能力误报为原生后端能力。

## 2. 文件变更

### 2.1 主要修改

- `src/Renderer/RenderGraph.h`
- `src/Renderer/RenderGraph.cpp`
- `src/Renderer/RenderGraphDiagnostics.cpp`
- `src/Renderer/D3D12SceneRenderer.cpp`
- `src/Renderer/VulkanSceneRenderer.cpp`
- `src/Automation/HarnessTools.cpp`
- `tests/RenderGraphTests.cpp`
- `tests/EngineHarnessTests.cpp`
- `CMakeLists.txt`
- `docs/LEARNING_GUIDE_CN.md`

### 2.2 新增

- `docs/AGENT_RDG_COMPILER_GUIDE_CN.md`
- `examples/harness/rdg_compilation.jsonl`

## 3. 编译流程

每帧仍由 Renderer 声明资源和 Pass，但 `Execute` 前新增 `Compile`：

```text
Reset
  -> Import / Declare Resource
  -> Add Pass
  -> MarkOutput / SideEffect
  -> Compile
       1. BuildLivenessDependencies
       2. CullPasses
       3. 分配 Active Execution Index
       4. BuildExecutionDependencies
       5. BuildResourceLifetimes
       6. BuildTransientAliasingPlan
       7. BuildQueueSchedule
  -> Execute Active Passes
```

`Compile` 是幂等的。图结构变化时，`InvalidateCompilation` 会清除旧编译结果。

## 4. 输出根与副作用

### 4.1 输出资源

最终需要保留的资源通过：

```cpp
graph.MarkOutput("SceneColor");
graph.MarkOutput("BackBuffer");
```

当前 D3D12 使用 `SceneColor` 作为输出根；Vulkan 使用 `BackBuffer`。

### 4.2 Side Effect

有些 Pass 不写 GPU 资源，但会产生外部行为，例如：

- 截图。
- CPU Readback。
- Timestamp Resolve。
- Query。
- 调试日志。

这类 Pass 使用：

```cpp
RenderGraph::PassOptions{
    RenderGraph::QueueClass::Graphics,
    true,
    false
}
```

字段分别表示：

```text
queue
sideEffect
allowCulling
```

D3D12 `SceneColorReady` 会触发截图并更新外部状态，因此被标记为 Side Effect。

### 4.3 兼容旧调用

如果图没有任何显式输出、Side Effect 或禁止裁剪 Pass，RDG 会保留全部 Pass。

这样旧测试图和逐步迁移代码不会突然变成空图。正式渲染链已经显式声明输出，因此会启用裁剪。

## 5. Pass Culling 如何实现

### 5.1 构建生产者依赖

按照声明顺序扫描 Pass，并维护：

```text
lastWriter[resource] = passIndex
```

当 Pass 读取资源时，它依赖该资源最近的生产者。

例如：

```text
GBuffer writes GBuffer0
DeferredLighting reads GBuffer0
```

得到：

```text
DeferredLighting -> GBuffer
```

### 5.2 找到根 Pass

根包括：

- 写入 `MarkOutput` 最终版本的 Pass。
- `sideEffect=true` 的 Pass。
- `allowCulling=false` 的 Pass。

### 5.3 反向遍历

从根 Pass 开始，沿生产者依赖反向遍历。被访问到的 Pass 标记为 Active；
其他 Pass 标记：

```text
culled = true
cullReason = not_reachable_from_output
```

执行阶段直接跳过 Culled Pass，因此不会产生：

- Marker。
- Barrier。
- Draw/Dispatch。
- CPU/GPU 时间记录。

### 5.4 为什么 Liveness 不使用 WAW

Pass Culling 只沿“读取哪个版本”的生产者关系传播，不把所有 Write After
Write 都视为保活依赖。

例如：

```text
PassA writes DebugTexture
PassB overwrites DebugTexture
Output reads PassB result
```

如果没有其他 Pass 读取 PassA 的版本，PassA 应当被裁剪。

## 6. 执行依赖

裁剪完成后，活跃 Pass 重新建立完整 Hazard：

- RAW：读依赖前一次写。
- WAR：写依赖此前读者。
- WAW：写依赖前一次写。

这些依赖当前有两个用途：

1. 验证原始声明顺序。
2. 为未来多队列生成 Fence/Semaphore 计划。

当前尚未进行自动拓扑重排，活跃 Pass 保持原始声明顺序，便于渐进迁移和调试。

## 7. 资源生命周期

每个活跃 Pass 获得连续 `executionIndex`。

资源生命周期定义为：

```text
FirstUse = 第一次被活跃 Pass 读或写
LastUse  = 最后一次被活跃 Pass 读或写
```

被裁剪 Pass 的访问不会扩展生命周期。

`ResourceDescription` 新增：

```text
transient
active
firstUse
lastUse
physicalAllocation
estimatedBytes
```

### 7.1 瞬态候选

有两种声明：

```cpp
DeclareTransientTexture(name, textureDescription);
DeclareTransientTexture(name, existingTexture, initialState);
```

第一种用于纯 RDG 逻辑资源和测试。

第二种用于当前迁移阶段：真实纹理仍由 Renderer 创建并被 DescriptorSet 引用，
但 RDG 可以根据其 `TextureDescription` 计算生命周期和别名计划。

当前 D3D12/Vulkan 已把以下纹理登记为瞬态候选：

- `GBuffer0..3`
- `HdrColor`
- `BloomA`
- `BloomB`

## 8. 别名槽规划

### 8.1 兼容条件

两个纹理只有在以下字段完全一致时才允许共享槽：

- Dimension。
- Width/Height。
- Array Layers。
- Mip Levels。
- Sample Count。
- Format。
- Usage。
- Memory Access。

这是保守规则，优先保证未来物理实现的正确性。

### 8.2 生命周期条件

只有：

```text
旧资源 LastUse < 新资源 FirstUse
```

才允许复用。

如果两个资源在同一个 Pass 中分别被读和写，则生命周期重叠，不能别名。

### 8.3 分配算法

瞬态资源按 `FirstUse` 排序。对每个资源：

1. 查找生命周期已经结束的兼容槽。
2. 找到则复用槽。
3. 找不到则创建新槽。

本阶段使用确定性的 First Fit，便于测试和报告稳定。

### 8.4 真实场景结果

640×360 的 D3D12 和 Vulkan 高级链均报告：

```text
Transient Resources: 7
Planned Allocations: 5
Logical Bytes: 12,902,400
Planned Physical Bytes: 9,216,000
Planned Aliased Bytes: 3,686,400
```

实际槽映射包括：

```text
GBuffer0 -> Allocation 0
BloomA   -> Allocation 0

GBuffer1 -> Allocation 1
BloomB   -> Allocation 1
```

GBuffer 生命周期在 Deferred Lighting 后结束，而 Bloom 随后开始，因此描述兼容时可以规划复用。

当前真实纹理仍是独立分配，所以这 3,686,400 字节是预计可节省值，不是已经释放的显存。

## 9. Queue 调度计划

Pass 可以声明：

```cpp
PassOptions{
    QueueClass::Compute,
    false,
    true
}
```

编译器检查每条执行依赖：

```text
Producer Queue != Consumer Queue
```

满足时生成 `QueueSyncDescription`：

```text
producerPass
consumerPass
producerQueue
consumerQueue
resources
```

测试图：

```text
GraphicsProduce
  -> ComputeProcess
  -> GraphicsConsume
```

会生成两条同步边：

```text
Graphics -> Compute : GraphicsData
Compute  -> Graphics: ComputeData
```

### 9.1 为什么还不是原生 Async Compute

当前公共 RHI 只有一个 `ICommandContext`：

```text
D3D12: Direct Command Queue
Vulkan: Graphics Command Buffer
```

尚缺：

- Queue 类型化 Command Context。
- Compute Command Allocator/Command Buffer。
- 跨 Queue Fence 或 Timeline Semaphore。
- Queue Ownership Transfer。
- 多队列提交批次。

因此当前执行模式是：

```text
queueExecutionMode = serial_fallback
```

Compute Pass 的 Queue 声明会参与依赖和同步计划，但仍通过当前上下文顺序执行。

## 10. RDG Report v2

报告从 version 1 升级为 version 2：

```json
{
  "format": "PrismRenderGraphReport",
  "version": 2,
  "compilation": {
    "passCullingEnabled": true,
    "declaredPassCount": 8,
    "activePassCount": 8,
    "culledPassCount": 0,
    "queueExecutionMode": "serial_fallback",
    "crossQueueSyncCount": 0,
    "transient": {
      "resourceCount": 7,
      "allocationCount": 5,
      "logicalBytes": 12902400,
      "physicalBytes": 9216000,
      "aliasedBytes": 3686400,
      "physicalAliasingApplied": false
    }
  }
}
```

Pass 新增：

```text
originalIndex
executionIndex
culled
cullReason
sideEffect
queue
dependencies
```

Resource 新增：

```text
transient
active
firstUse
lastUse
physicalAllocation
estimatedBytes
```

顶层新增：

```text
queueSync
```

## 11. Harness 能力

`engine.describe` 新增：

```text
rdgPassCulling = true
rdgResourceLifetimes = true
rdgTransientAliasingPlan = true
rdgQueueSchedule = true
nativeTransientAliasing = false
nativeAsyncCompute = false
```

Agent 可以通过 `rdg.describe` 查询两个后端的完整编译结果。

明确区分“规划能力”和“原生执行能力”非常重要，否则 Agent 可能错误判断显存或并行优化已经生效。

## 12. 自动测试

`RenderGraphTests` 新增：

1. 不可达 Pass 被真实跳过。
2. 输出生产者和依赖生产者保持活跃。
3. Culled Pass 带有稳定原因。
4. Side Effect Pass 保活自身和生产者。
5. 两个不重叠兼容纹理复用一个别名槽。
6. 生命周期顺序正确。
7. 逻辑字节、物理槽字节和预计节省量正确。
8. Graphics/Compute/Graphics 生成两条同步边。
9. 当前执行模式明确为串行回退。
10. RDG Report v2 保留编译和 Queue 信息。

`EngineHarnessTests` 新增能力发现测试。

## 13. 验证结果

2026-07-17 Debug 验证：

- 完整构建成功。
- CTest 8/8 通过。
- RDG 专项测试通过。
- D3D12 `rdg.describe` 成功。
- Vulkan `rdg.describe` 成功。
- 两个后端均生成 RDG Report v2。
- D3D12：8/8 Pass Active。
- Vulkan：7/7 Pass Active。
- 两端瞬态生命周期和 7 -> 5 槽规划一致。
- D3D12/Vulkan Tonemap Golden Image 对比通过。
- MAE：`0.0000640262`。
- RMSE：`0.000512323`。

验证命令：

```powershell
cmake --build build-windows-ci --config Debug --parallel 8
ctest --test-dir build-windows-ci -C Debug --output-on-failure
```

Harness 示例：

```powershell
Get-Content examples/harness/rdg_compilation.jsonl |
  .\build-windows-ci\Debug\PrismHarness.exe --headless
```

## 14. 设计取舍

### 14.1 为什么先保留声明顺序

当前 Pass 已按正确顺序建立，并且 GPU Timestamp、截图和 Golden Image 都依赖稳定顺序。

先完成依赖、裁剪和生命周期，再增加拓扑排序，可以缩小回归范围。

### 14.2 为什么使用精确描述兼容

更激进的别名可以允许 Usage 合并或大小级别复用，但 D3D12 Resource Flags、
Vulkan Memory Requirements 和图像创建标志有额外约束。

本阶段使用精确匹配，便于未来安全映射到真实后端。

### 14.3 为什么没有直接创建临时纹理

当前 DescriptorSet 和 TextureView 在 Renderer 初始化阶段持有具体纹理。
如果 RDG 在每帧替换物理纹理，Descriptor 生命周期也必须同时迁移。

因此 Stage 17-J 先建立正确的逻辑模型和规划结果。Stage 17-K 已增加公共 Transient Pool，并让 D3D12/Vulkan 高级场景使用原生别名资源。

## 15. 当前边界

- 没有自动拓扑排序。
- 没有 Resource Handle 和资源版本句柄。
- 生命周期仍以整张纹理为粒度。
- 没有 Mip/Array Layer 子资源区间。
- 没有 UAV Barrier 和 Split Barrier。
- Stage 17-J 当时没有真实 Alias Heap 或 Alias Memory；Stage 17-K 已完成。
- 没有真实 Compute Queue 并行提交。
- 没有 Copy Queue。
- 没有跨队列 GPU Timestamp 合并。

## 16. 推荐阅读顺序

1. `src/Renderer/RenderGraph.h`
2. `BuildLivenessDependencies`
3. `CullPasses`
4. `BuildExecutionDependencies`
5. `BuildResourceLifetimes`
6. `BuildTransientAliasingPlan`
7. `BuildQueueSchedule`
8. `src/Renderer/RenderGraphDiagnostics.cpp`
9. `SceneRendererStage4.cpp` 的输出根和瞬态声明
10. `VulkanSceneRenderer.cpp` 的输出根和瞬态声明
11. `tests/RenderGraphTests.cpp`

先手工画出资源生产者图，再对照 Active Pass 和生命周期索引。

## 17. Stage 17-K 后续进展

Stage 17-K 已将以下规划转换成真实后端能力：

1. 公共 `ITransientTexturePool`。
2. 共享的延迟渲染瞬态资源布局。
3. D3D12 Heap + Placed Resource + Aliasing Barrier。
4. Vulkan Alias Image/Memory Binding 与内存依赖。
5. D3D12 Compute Queue、Fence 和真实空提交探针。
6. Vulkan Compute Queue、Timeline Semaphore 和真实空提交探针。
7. 原生分配统计、生命周期重叠校验与 Report v3。

Stage 17-K 当时仍待实现、但后续已经完成：

1. 强类型 RDG Resource Handle 与资源版本：Stage 20 完成。
2. RDG 根据编译结果实例化 Native Transient Pool：Stage 27 完成。
3. Graphics/Compute 公共 Queue Context：Stage 17-L/N 完成。
4. D3D12 Fence Wait/Signal 驱动的跨队列 Pass 提交：Stage 17-N 完成。
5. Vulkan Timeline Semaphore Wait/Signal 和 Queue Handoff：Stage 17-N 完成。
6. 多 Queue GPU Timestamp 合并：Stage 17-M 完成。
7. Texture 子资源与 Buffer Range 状态跟踪：Stage 20 完成。

Stage 17-L 随后选择 Compute Bloom 与 Hi-Z 作为真实工作负载；Stage 17-O/P
又加入历史成本模型和原生命令并行录制。当前 `auto` 会在预测净收益为负时
保持串行，这是调度结果，不是能力缺失。

Stage 17-K 的完整实现与验证见 `docs/AGENT_RDG_NATIVE_RESOURCE_QUEUE_GUIDE_CN.md`。
