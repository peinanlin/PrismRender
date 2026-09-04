# PrismRender Stage 17-L：Compute Bloom、Hi-Z 与原生多队列 RDG

> 后续状态：Stage 17-M 已完成 D3D12/Vulkan 跨队列 GPU Timestamp 时间轴、Overlap 统计和串行/原生多队列 A/B 自动比较。真实验证表明当前严格依赖链的 Overlap 为 0，多队列尚未产生性能收益。详见 `docs/AGENT_RDG_QUEUE_TIMELINE_GUIDE_CN.md`。

## 1. 阶段目标

Stage 17-K 已经具备：

- RDG Pass Culling、资源生命周期和队列同步计划。
- D3D12 Heap/Placed Resource 与 Vulkan Alias Memory。
- D3D12 Compute Queue/Fence 和 Vulkan Compute Queue/Timeline Semaphore。
- 用空提交探针验证 Compute Queue 可以运行。

但 Stage 17-K 的高级渲染 Pass 仍全部录制到 Graphics 队列。Stage 17-L 把规划结果接入真实帧：

1. Bloom 三个 Pass 改为公共 Compute Shader。
2. 创建可采样深度缓冲。
3. 生成完整 mip-chain Hi-Z 深度金字塔。
4. RDG 按 `Graphics -> Compute -> Graphics` 切分命令段。
5. D3D12 使用 Fence `Signal/Wait` 完成跨队列依赖。
6. Vulkan 使用 Timeline Semaphore 完成跨队列依赖。
7. D3D12/Vulkan 继续使用同一份 HLSL 和同一套 RDG Pass。
8. RDG Report v4 暴露实际队列切换结果。
9. Harness 自动执行双 API RDG 查询和 Golden Image 对比。

当前状态应准确描述为：

```text
Compute Bloom                    已实现
完整 mip-chain Hi-Z              已实现
原生 Compute Queue 提交          已实现
跨队列 GPU 依赖同步              已实现
Graphics/Compute 工作重叠测量    Stage 17-M 已完成
跨队列 GPU Timestamp 时钟校准    Stage 17-M 已完成
```

## 2. 文件变更

### 2.1 新增文件

- `assets/shaders/HiZ.hlsl`
- `examples/harness/rdg_async_compute.jsonl`
- `docs/AGENT_RDG_ASYNC_COMPUTE_GUIDE_CN.md`

### 2.2 主要修改文件

- `assets/shaders/PostProcess.hlsl`
- `src/RHI/ICommandContext.h`
- `src/RHI/D3D12/D3D12Context.h`
- `src/RHI/D3D12/D3D12Context.cpp`
- `src/RHI/D3D12/D3D12CommandContextAdapter.h`
- `src/RHI/D3D12/D3D12CommandContextAdapter.cpp`
- `src/RHI/Vulkan/VulkanContext.h`
- `src/RHI/Vulkan/VulkanContext.cpp`
- `src/RHI/Vulkan/VulkanResources.cpp`
- `src/RHI/Vulkan/VulkanTransientResources.cpp`
- `src/Renderer/DeferredTransientLayout.h`
- `src/Renderer/DeferredTransientLayout.cpp`
- `src/Renderer/RenderGraph.h`
- `src/Renderer/RenderGraph.cpp`
- `src/Renderer/RenderGraphDiagnostics.cpp`
- `src/Renderer/D3D12SceneRenderer.h`
- `src/Renderer/D3D12SceneRenderer.cpp`
- `src/Renderer/VulkanSceneRenderer.h`
- `src/Renderer/VulkanSceneRenderer.cpp`
- `src/Automation/HarnessTools.cpp`
- `tests/RenderGraphTests.cpp`
- `tests/ShaderCompilerTests.cpp`
- `tests/EngineHarnessTests.cpp`
- `docs/LEARNING_GUIDE_CN.md`

## 3. 总体数据流

```text
GBuffer(Graphics)
  -> DepthBuffer
  -> GBuffer0..3

DeferredLighting(Graphics)
  -> HdrColor

RDG 分析跨队列依赖
  -> DepthBuffer: Graphics -> Compute
  -> HdrColor:    Graphics -> Compute
  -> BloomA:      Compute  -> Graphics

Graphics Queue
  -> Shadow
  -> GBuffer
  -> DeferredLighting
  -> Signal

Compute Queue
  -> Wait Graphics
  -> HiZBuild
       -> CopyDepthCS
       -> DownsampleDepthCS x N
  -> BloomExtractCS
  -> BlurHorizontalCS
  -> BlurVerticalCS
  -> Signal

Graphics Queue
  -> Wait Compute
  -> Tonemap
  -> SceneColorReady / Present
```

Hi-Z 和 Bloom 被安排在同一个 Compute 段，因此不会为了两个功能重复执行队列切换。

## 4. 公共 RHI 队列接口

`ICommandContext` 新增：

```cpp
enum class CommandQueueType
{
    Graphics,
    Compute
};

struct CommandQueueCapabilities
{
    bool graphics;
    bool compute;
    bool dedicatedCompute;
    bool timelineSynchronization;
    bool nativeQueueSwitching;
};
```

关键接口为：

```cpp
GetActiveCommandQueue()
SwitchCommandQueue(queue)
TextureViewBarrier(view, before, after)
```

`TextureViewBarrier` 与整纹理 `TextureBarrier` 的区别是：

- `TextureBarrier` 管理整张纹理，适合 RDG Pass 边界。
- `TextureViewBarrier` 管理一个 mip 或 array layer，适合 Hi-Z 内部逐级生成。

公共 Renderer 不直接调用 D3D12 `ResourceBarrier` 或 Vulkan `vkCmdPipelineBarrier`。

## 5. RDG 如何执行多队列

### 5.1 编译阶段

`RenderGraph::BuildQueueSchedule` 从执行依赖中提取跨队列边：

```text
producerPass
consumerPass
producerQueue
consumerQueue
resources[]
```

当前真实场景得到三条逻辑依赖：

```text
GBuffer          -> HiZBuild     : DepthBuffer
DeferredLighting -> BloomExtract : HdrColor
BloomVertical    -> Tonemap      : BloomA
```

### 5.2 执行阶段

当后端声明支持原生切换时，`RenderGraph::Execute`：

1. 从 Graphics 开始。
2. 找到第一个 Compute Pass。
3. 将需要跨队列传递的纹理转换到 `Common`。
4. 调用 `SwitchCommandQueue(Compute)`。
5. 连续录制 Hi-Z 和 Bloom。
6. 将返回 Graphics 所需的纹理转换到 `Common`。
7. 调用 `SwitchCommandQueue(Graphics)`。
8. 继续 Tonemap 和最终输出。

`Common` 是公共 RHI 的队列交接状态。真正的 Wait/Signal 由后端实现。

### 5.3 为什么不是每个 Pass 都切一次队列

队列切换会增加：

- Command List/Command Buffer 提交。
- Fence/Semaphore 信号。
- GPU 调度和 CPU 提交开销。

因此相邻 Compute Pass 合并成一个队列段。当前帧固定为：

```text
3 queue segments
2 queue switches
```

## 6. D3D12 后端

### 6.1 命令段生命周期

每次切换队列时：

1. `Close` 当前 Command List。
2. 提交到当前 Queue。
3. 当前 Queue 对 Fence 执行 `Signal`。
4. 目标 Queue 对同一 Fence 值执行 `Wait`。
5. 保留旧 Command Allocator 和 Command List，直到帧 Fence 完成。
6. 为目标队列创建新的 Allocator 和 Command List。

Graphics 到 Compute：

```text
GraphicsQueue.Execute
GraphicsQueue.Signal(GraphicsFence, value)
ComputeQueue.Wait(GraphicsFence, value)
```

Compute 到 Graphics：

```text
ComputeQueue.Execute
ComputeQueue.Signal(ComputeFence, value)
GraphicsQueue.Wait(ComputeFence, value)
```

这不是 CPU 阻塞等待。Wait 被写入 GPU Queue 时间线。

### 6.2 队列感知资源状态

公共 `ShaderResource` 在 Graphics 队列映射为：

```text
PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE
```

Compute Command List 不允许 Pixel Shader 状态，因此 Compute 队列映射为：

```text
NON_PIXEL_SHADER_RESOURCE
```

如果不做这一层转换，D3D12 会在 Compute Command List `Close()` 时返回 `E_INVALIDARG`。这个差异由 `D3D12CommandContextAdapter` 吸收，RDG 不需要知道。

### 6.3 Hi-Z 子资源 Barrier

每个 mip 使用独立 `D3D12TextureView`。生成一级时执行：

```text
mip N: ShaderResource -> UnorderedAccess
Dispatch
mip N: UnorderedAccess -> ShaderResource
```

下一次 Dispatch 可以安全读取刚完成的 mip N，并写 mip N+1。

## 7. Vulkan 后端

### 7.1 Timeline Semaphore

Graphics 到 Compute：

```text
vkQueueSubmit(Graphics)
  signal timeline = N

vkQueueSubmit(Compute)
  wait timeline = N
  signal timeline = N + 1
```

Compute 到 Graphics：

```text
vkQueueSubmit(Graphics continuation)
  wait timeline = N + 1
  signal renderFinished
```

Timeline Semaphore 避免为每条依赖创建一次性 Binary Semaphore。

### 7.2 Command Buffer 生命周期

每个队列段使用与 Queue Family 匹配的 Command Pool：

- Graphics Command Pool。
- Compute Command Pool。

已提交的 Command Buffer 保留在当前 Frame Context 中，等帧 Fence 完成后统一释放。

### 7.3 Queue Family

可能跨 Graphics/Compute 使用的 Buffer、普通 Texture 和 Transient Texture，在两个 Queue Family 不同时使用：

```text
VK_SHARING_MODE_CONCURRENT
```

当前实现因此不需要显式 Queue Family Ownership Transfer。代价是驱动可能放弃部分独占资源优化。

### 7.4 mip 级图像屏障

`TextureViewBarrier` 使用 View 的：

```text
baseMipLevel
mipLevelCount
baseArrayLayer
arrayLayerCount
```

只转换当前输出 mip：

```text
SHADER_READ_ONLY_OPTIMAL
  -> GENERAL
  -> SHADER_READ_ONLY_OPTIMAL
```

其他 mip 保持可读状态。

## 8. Compute Bloom

`PostProcess.hlsl` 保留原 Pixel Shader，并新增：

- `BrightExtractCS`
- `BlurHorizontalCS`
- `BlurVerticalCS`

公共资源布局为：

```text
b0  PostProcessConstants
t0  sourceTexture
u0  outputTexture
s0  linearClampSampler
```

Slang 分别输出：

- D3D12 DXIL。
- Vulkan SPIR-V。

每个线程组为 `8 x 8`。Dispatch 数量为：

```cpp
(width + 7) / 8
(height + 7) / 8
```

线程内部检查输出边界，因此窗口尺寸不需要是 8 的倍数。

## 9. Hi-Z 深度金字塔

### 9.1 资源

Hi-Z 使用：

```text
Format       R32Float
Usage        ShaderResource | UnorderedAccess
MipLevels    floor(log2(max(width, height))) + 1
```

640 x 360 会生成 10 个 mip：

```text
640x360
320x180
160x90
80x45
40x22
20x11
10x5
5x2
2x1
1x1
```

### 9.2 深度缓冲可采样

D3D12 主深度资源改为：

```text
Resource Format : R32_TYPELESS
DSV Format      : D32_FLOAT
SRV Format      : R32_FLOAT
```

Vulkan 深度 Image 增加：

```text
VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
VK_IMAGE_USAGE_SAMPLED_BIT
```

GBuffer 的 Depth Store 从 `Discard` 改为 `Store`，否则 Hi-Z 读取的深度未定义。

### 9.3 生成算法

`CopyDepthCS` 把主深度复制到 mip 0。

`DownsampleDepthCS` 对每个输出像素读取上一层 2×2 深度，并保存最大值：

```text
output = max(d00, d10, d01, d11)
```

PrismRender 使用标准深度范围，0 表示近处、1 表示远处。保存最大值可为后续遮挡测试提供保守的远深度。

当前阶段只生成 Hi-Z；Compute Culling 尚未消费它。

## 10. RDG Report v4

`rdg.describe` 新增或更新：

```text
nativeQueueSwitching
nativeMultiQueueSubmissionApplied
multiQueueGpuTimestamps
queueSwitchCount
queueSegmentCount
```

本阶段真实结果：

```text
nativeQueueSwitching              true
nativeMultiQueueSubmissionApplied true
multiQueueGpuTimestamps           false
queueSwitchCount                  2
queueSegmentCount                 3
```

Pass 报告中：

```text
HiZBuild        Compute
BloomExtract    Compute
BloomHorizontal Compute
BloomVertical   Compute
```

## 11. Harness 能力

`engine.describe` 现在声明：

```text
nativeAsyncCompute      true
computeBloom            true
computeHiZ              true
crossQueueGpuTimestamps false
```

这里的 `nativeAsyncCompute=true` 表示高级 Compute Pass 已经提交到原生 Compute Queue，并使用 GPU 端同步。

它不表示已经证明 Graphics 与 Compute 在时间上发生重叠，也不表示一定降低了帧时间。

## 12. 自动验证

### 12.1 编译

```powershell
cmake --build build-windows-ci --config Debug
```

### 12.2 单元测试

```powershell
ctest --test-dir build-windows-ci -C Debug --output-on-failure
```

测试覆盖：

- RDG 队列切换顺序和报告字段。
- DXIL/SPIR-V Compute Bloom 编译。
- DXIL/SPIR-V Hi-Z 编译与 Reflection。
- Harness 能力声明。
- 既有 Asset、Crash、性能和 World 同步回归。

### 12.3 真实双 API 验证

```powershell
.\build-windows-ci\Debug\PrismHarness.exe `
  --headless `
  --project-root . `
  --commands examples\harness\rdg_async_compute.jsonl `
  --output automation\stage17l-results.jsonl
```

验证内容：

1. D3D12 `rdg.describe`。
2. Vulkan `rdg.describe`。
3. D3D12/Vulkan Tonemap Golden Image。

本机验证结果：

```text
D3D12 active passes  9
Vulkan active passes 8
cross queue syncs    3
queue switches       2
queue segments       3
HiZBuild queue       Compute
aliasing barriers    4

dimensionsMatch      true
changedPixelRatio    0
meanAbsoluteError    0.0000640262
RMSE                 0.000512323
maximumChannelError  0.0196078
comparisonPassed     true
```

D3D12 比 Vulkan 多一个 `SceneColorReady` Side Effect Pass，这是捕获路径差异，不是光照算法差异。

## 13. 设计取舍

### 13.1 为什么保留 Pixel Bloom

旧 Pixel Pipeline 暂时保留，方便：

- 对比迁移前后的图像。
- 出现驱动问题时快速定位。
- 后续清理前保留回滚参考。

正常高级场景已经使用 Compute Bloom。

### 13.2 为什么 Hi-Z 是 Side Effect Pass

当前没有 Compute Culling 消费 Hi-Z。如果不标记 Side Effect，Pass Culling 会正确地删除它。

本阶段需要验证真实生成路径，因此暂时标记 Side Effect。Stage 21 接入 Compute Culling 后，应由资源依赖自然保持活跃。

### 13.3 为什么当前不宣称性能提升

当前调度是：

```text
Graphics 完成 -> Compute 完成 -> Graphics 继续
```

这是正确的多队列执行，但依赖边界仍然串行。真正的异步重叠需要：

- 把无依赖 Graphics Pass 与 Compute Pass 并行提交。
- 减少过宽的队列 Wait。
- 校准两个 Queue 的 Timestamp 时钟域。
- 对比单队列和多队列 GPU Frame Time。

## 14. 当前边界

以下列表保留 Stage 17-L 当时的边界，并标注后续状态：

- RDG 强类型 Handle 与资源版本已由 Stage 20 完成。
- Texture mip/layer 与 Buffer Range 状态跟踪已由 Stage 20 完成。
- Hi-Z 尚未接入 Frustum/Occlusion Culling。
- Bloom 仍是全分辨率双纹理模糊，不是多级 Bloom。
- D3D12/Vulkan 跨队列 Timestamp 与 Overlap 已由 Stage 17-M 完成。
- DAG Queue Batch、成本模型与受控探索已由 Stage 17-N 至 17-P 完成。
- Vulkan 跨 Queue Family 资源采用 Concurrent Sharing，尚未做 Exclusive Ownership Transfer 优化。
- Copy/Transfer Queue、Upload Ticket 与持久 Upload Ring 已由 Stage 28 完成。

## 15. 推荐阅读顺序

1. `assets/shaders/PostProcess.hlsl`
2. `assets/shaders/HiZ.hlsl`
3. `src/RHI/ICommandContext.h`
4. `RenderGraph::BuildQueueSchedule`
5. `RenderGraph::PrepareQueueHandoff`
6. `RenderGraph::Execute`
7. `D3D12Context::SwitchCommandQueue`
8. `D3D12CommandContextAdapter::TextureViewBarrier`
9. `VulkanContext::SwitchCommandQueue`
10. `VulkanContext::TextureViewBarrier`
11. `SceneRenderer::RenderHiZPass`
12. `VulkanSceneRenderer::RenderHiZPass`
13. `src/Renderer/RenderGraphDiagnostics.cpp`
14. `tests/RenderGraphTests.cpp`
15. `tests/ShaderCompilerTests.cpp`
16. `examples/harness/rdg_async_compute.jsonl`

## 16. 下一阶段

建议按以下顺序继续：

1. 为 D3D12 Graphics/Compute Queue 分别读取 Timestamp Frequency。
2. 使用 Clock Calibration 或统一时钟域合并跨队列 GPU 时间。
3. 让 RDG 调度器识别可重叠的 Graphics/Compute 子图。
4. 输出 Queue Segment 起止时间和 Overlap Ratio。
5. 建立单队列与多队列 A/B 性能基线。
6. 引入强类型 `RdgTextureHandle` 和资源版本。
7. 让 Compute Culling 消费 Hi-Z。
8. 实现 ExecuteIndirect 与 GPU Driven Draw。

只有在 A/B 数据证明 GPU Frame Time 下降后，才能把“多队列可运行”升级为“异步计算优化有效”。
