# PrismRender Stage 17-K：RDG 原生瞬态资源与多队列基础设施

## 1. 阶段目标

Stage 17-J 已经让 RDG 能够计算：

- 活跃 Pass 与被裁剪 Pass。
- RAW、WAR、WAW 依赖。
- 瞬态纹理的 `FirstUse/LastUse`。
- 逻辑资源到物理槽的别名规划。
- Graphics/Compute Pass 之间的同步边。

但当时的 GBuffer、HDR 和 Bloom 纹理仍然分别分配显存，所有 Pass 也仍然在主 Graphics Command Context 中顺序执行。

Stage 17-K 完成以下内容：

1. 公共 `ITransientTexturePool`。
2. 稳定的瞬态纹理请求与原生分配元数据。
3. D3D12 `ID3D12Heap + CreatePlacedResource`。
4. Vulkan `VK_IMAGE_CREATE_ALIAS_BIT + VkDeviceMemory` 共享绑定。
5. D3D12 Aliasing Barrier。
6. Vulkan 别名切换内存依赖。
7. RDG 对真实物理槽和生命周期重叠的自动校验。
8. RDG Report v3 的原生分配与屏障统计。
9. D3D12 Compute Queue、Compute Fence 和真实提交探针。
10. Vulkan Compute Queue、Timeline Semaphore 和真实提交探针。
11. D3D12/Vulkan 高级场景使用真实瞬态池。
12. 双 API 截图与 Golden Image 回归。

需要准确区分：

```text
原生显存别名                 已真实应用
Compute Queue 提交与同步探针 已真实运行
RDG 高级 Pass 多队列并行     尚未应用
```

因此当前报告是：

```text
physicalAliasingApplied = true
nativeComputeQueueAvailable = true
nativeMultiQueueSubmissionApplied = false
queueExecutionMode = serial_fallback
```

## 2. 文件变更

### 2.1 新增文件

- `src/RHI/TransientResources.h`
- `src/RHI/TransientResources.cpp`
- `src/RHI/D3D12/D3D12TransientResources.h`
- `src/RHI/D3D12/D3D12TransientResources.cpp`
- `src/RHI/Vulkan/VulkanTransientResources.h`
- `src/RHI/Vulkan/VulkanTransientResources.cpp`
- `src/Renderer/DeferredTransientLayout.h`
- `src/Renderer/DeferredTransientLayout.cpp`
- `docs/AGENT_RDG_NATIVE_RESOURCE_QUEUE_GUIDE_CN.md`
- `examples/harness/rdg_native_resources.jsonl`

### 2.2 主要修改文件

- `src/RHI/GraphicsResources.h`
- `src/RHI/IGraphicsDevice.h`
- `src/RHI/ICommandContext.h`
- `src/RHI/D3D12/D3D12Context.h`
- `src/RHI/D3D12/D3D12Context.cpp`
- `src/RHI/D3D12/D3D12GraphicsDevice.h`
- `src/RHI/D3D12/D3D12GraphicsDevice.cpp`
- `src/RHI/D3D12/D3D12Resources.h`
- `src/RHI/D3D12/D3D12Resources.cpp`
- `src/RHI/D3D12/D3D12CommandContextAdapter.h`
- `src/RHI/D3D12/D3D12CommandContextAdapter.cpp`
- `src/RHI/Vulkan/VulkanContext.h`
- `src/RHI/Vulkan/VulkanContext.cpp`
- `src/RHI/Vulkan/VulkanResources.h`
- `src/RHI/Vulkan/VulkanResources.cpp`
- `src/Renderer/RenderGraph.h`
- `src/Renderer/RenderGraph.cpp`
- `src/Renderer/RenderGraphDiagnostics.cpp`
- `src/Renderer/D3D12SceneRenderer.h`
- `src/Renderer/D3D12SceneRenderer.cpp`
- `src/Renderer/VulkanSceneRenderer.h`
- `src/Renderer/VulkanSceneRenderer.cpp`
- `src/Automation/HarnessTools.cpp`
- `tests/RenderGraphTests.cpp`
- `tests/EngineHarnessTests.cpp`
- `CMakeLists.txt`
- `docs/LEARNING_GUIDE_CN.md`

## 3. 总体数据流

```text
DeferredTransientLayout
  -> 生成 7 个逻辑纹理请求和 5 个物理槽
  -> IGraphicsDevice::CreateTransientTexturePool
       -> D3D12TransientTexturePool
            -> ID3D12Heap
            -> CreatePlacedResource
       -> VulkanTransientTexturePool
            -> VkDeviceMemory
            -> vkCreateImage(VK_IMAGE_CREATE_ALIAS_BIT)
            -> vkBindImageMemory
  -> Renderer 创建 TextureView 和 DescriptorSet
  -> 每帧向 RenderGraph 声明真实 ITexture
  -> RenderGraph::Compile
       -> 计算理论生命周期和槽位
       -> 读取原生分配元数据
       -> 校验真实共享槽没有生命周期重叠
  -> RenderGraph::Execute
       -> 第一次激活共享槽中的逻辑资源
       -> TextureAliasingBarrier
       -> 普通 TextureBarrier
       -> 执行 Pass
  -> RDG Report v3
       -> 输出规划数据、原生数据和屏障计数
```

## 4. 公共瞬态资源接口

### 4.1 `TransientTextureRequest`

每个请求包含：

```cpp
struct TransientTextureRequest
{
    std::string name;
    TextureDescription description;
    std::size_t allocationIndex;
};
```

其中：

- `name` 是 RDG 逻辑资源名。
- `description` 是跨 API 纹理描述。
- `allocationIndex` 是希望共享的物理槽。

### 4.2 `ITransientTexturePool`

公共池负责：

- 创建一组逻辑纹理。
- 保存共享物理分配的所有权。
- 按名称返回稳定 `ITexture`。
- 报告逻辑字节、物理字节和节省字节。

Renderer 不需要知道底层使用 D3D12 Heap 还是 Vulkan Device Memory。

### 4.3 原生分配元数据

`ITexture::GetTransientAllocationInfo()` 返回：

```text
poolId
allocationIndex
logicalBytes
allocationBytes
poolPhysicalBytes
```

普通纹理返回 `nullptr`。

RDG 通过这些信息判断当前纹理是否来自真实瞬态池。

## 5. 共享延迟渲染布局

`BuildDeferredTransientTextureRequests` 当前生成：

| 逻辑资源 | 物理槽 | 生命周期 |
| --- | ---: | --- |
| GBuffer0 | 0 | Pass 1 到 2 |
| BloomA | 0 | Pass 3 到 6 |
| GBuffer1 | 1 | Pass 1 到 2 |
| BloomB | 1 | Pass 4 到 5 |
| GBuffer2 | 2 | Pass 1 到 2 |
| GBuffer3 | 3 | Pass 1 到 2 |
| HdrColor | 4 | Pass 2 到 6 |

槽 0 和槽 1 可以安全复用，因为：

```text
GBuffer0.lastUse = 2 < BloomA.firstUse = 3
GBuffer1.lastUse = 2 < BloomB.firstUse = 4
```

当前使用精确描述匹配：

- 相同宽高。
- 相同格式。
- 相同 Mip 和 Array Layer。
- 相同 Sample Count。
- 相同 Usage。
- 相同 MemoryAccess。

这比按“大于等于尺寸”复用更保守，但便于验证两套原生 API 的要求。

## 6. D3D12 实现

### 6.1 创建 Heap

每个物理槽调用：

```cpp
ID3D12Device::GetResourceAllocationInfo
ID3D12Device::CreateHeap
```

Heap 使用：

```text
D3D12_HEAP_TYPE_DEFAULT
D3D12_HEAP_FLAG_NONE
```

物理槽大小采用驱动返回的 `SizeInBytes`，而不是只使用宽高乘像素字节。

这样会正确包含：

- D3D12 资源对齐。
- GPU Tile 对齐。
- 驱动要求的额外空间。

### 6.2 创建 Placed Resource

同一槽中的多个逻辑纹理都使用：

```cpp
CreatePlacedResource(heap, 0, ...)
```

例如：

```text
Heap Slot 0
  Offset 0 -> GBuffer0 ID3D12Resource
  Offset 0 -> BloomA   ID3D12Resource
```

它们是不同 `ID3D12Resource` 对象，但底层引用同一块 Heap 内存。

### 6.3 Heap 生命周期

Placed Resource 不能比 Heap 活得更久。

`D3D12Texture` 因此额外保存共享 Heap Owner。

即使 Renderer、TextureView 或 DescriptorSet 暂时持有纹理，Heap 也不会提前释放。

### 6.4 Aliasing Barrier

切换逻辑资源时记录：

```cpp
D3D12_RESOURCE_BARRIER_TYPE_ALIASING
```

第一次激活一个共享槽时允许：

```text
pResourceBefore = nullptr
pResourceAfter  = 当前逻辑纹理
```

同一帧后续切换时使用已知前驱资源：

```text
GBuffer0 -> BloomA
GBuffer1 -> BloomB
```

Aliasing Barrier 后，RDG 将新资源状态重置为 `Undefined`，随后普通 Texture Barrier 把它转换为 RenderTarget 或其他目标状态。

## 7. Vulkan 实现

### 7.1 创建可别名 Image

每个逻辑纹理独立调用：

```cpp
vkCreateImage
```

并带有：

```text
VK_IMAGE_CREATE_ALIAS_BIT
```

Cube Texture 还会保留：

```text
VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT
```

### 7.2 合并 Memory Requirements

同一槽内所有 Image 的要求必须满足：

- `memoryTypeBits` 存在公共交集。
- 分配大小不小于最大 `requirements.size`。
- 绑定 Offset 满足 Alignment。

当前精确描述匹配使这些条件通常一致。

### 7.3 共享 `VkDeviceMemory`

每个物理槽只调用一次：

```cpp
vkAllocateMemory
```

同槽多个 Image 都绑定到 Offset 0：

```cpp
vkBindImageMemory(image, memory, 0)
```

Image 和 ImageView 由 `VulkanTexture` 销毁。

`VkDeviceMemory` 由共享 Memory Owner 销毁，确保 Memory 晚于所有 Image 释放。

### 7.4 别名切换同步

Vulkan 没有与 D3D12 Aliasing Barrier 完全同名的对象。

当前实现使用全命令阶段的 `VkMemoryBarrier`：

```text
srcStage  = ALL_COMMANDS
dstStage  = ALL_COMMANDS
srcAccess = MEMORY_WRITE
dstAccess = MEMORY_READ | MEMORY_WRITE
```

然后把新 Image 当作 `VK_IMAGE_LAYOUT_UNDEFINED`，再由普通 Image Barrier 转换到目标 Layout。

这样明确表示：

- 旧逻辑资源的写入已经结束。
- 新逻辑资源不继承旧内容。
- 新 Image 可以开始使用共享内存。

## 8. RDG 原生别名校验

`BuildNativeTransientAliasingPlan` 会读取每张真实纹理的：

```text
poolId + allocationIndex
```

然后按真实物理槽分组。

对每个槽中的活跃资源检查：

```text
previous.lastUse < next.firstUse
```

如果出现重叠，RDG 立即失败：

```text
Native transient textures with overlapping lifetimes share one allocation.
```

这比等待 GPU 随机闪烁或 Device Lost 更容易定位。

### 8.1 为什么保留理论槽和原生槽

报告同时保留：

```text
physicalAllocation
nativePoolId
nativeAllocation
```

前者是 RDG 编译器的理论结果。

后两者是后端实际执行的结果。

两组数据分开后，Agent 可以发现：

- 规划已经节省显存，但后端没有应用。
- 后端错误地让生命周期重叠资源共享内存。
- 原生分配器因为对齐导致实际字节高于理论估算。

## 9. Aliasing Barrier 执行顺序

`PrepareTextureAliasingBarriers` 在普通状态 Barrier 前运行：

```text
Pass 开始
  -> 检查资源是否在本 Pass 第一次使用
  -> 检查真实物理槽是否包含多个活跃逻辑资源
  -> 找到同槽最近的前驱资源
  -> TextureAliasingBarrier
  -> 将新资源状态设为 Undefined
  -> PrepareTextureBarriers
  -> 执行 Pass Callback
```

当前场景每帧预计并执行 4 次：

```text
激活 GBuffer0
激活 GBuffer1
GBuffer0 -> BloomA
GBuffer1 -> BloomB
```

前两次使用空前驱，是为了覆盖上一帧最后使用该共享槽的资源。

## 10. 多队列基础设施

### 10.1 公共能力结构

`ICommandContext::GetQueueCapabilities()` 返回：

```text
graphics
compute
dedicatedCompute
timelineSynchronization
```

RDG Report 会记录实际运行设备的能力。

### 10.2 D3D12

`D3D12Context` 新增：

- `ID3D12CommandQueue`，类型为 `COMPUTE`。
- 独立 `ID3D12Fence`。
- 独立 Fence Event。
- `ExecuteComputeImmediate`。

初始化时提交一张空 Compute Command List，并等待 Compute Fence。

只有提交和等待成功后：

```text
compute = true
dedicatedCompute = true
timelineSynchronization = true
```

D3D12 Fence 在这里承担跨提交单调时间线同步的角色。

### 10.3 Vulkan

Queue Family 选择增加 Compute：

- 优先选择不带 Graphics 能力的 Dedicated Compute Family。
- 没有独立 Family 时可回退到 Graphics Family。

逻辑设备启用：

```text
VkPhysicalDeviceVulkan12Features::timelineSemaphore
VkPhysicalDeviceVulkan13Features::dynamicRendering
```

并创建：

- Compute Queue。
- Compute Command Pool。
- Timeline Semaphore。
- `ExecuteComputeImmediate`。

初始化探针会：

1. 分配空 Compute Command Buffer。
2. 提交到 Compute Queue。
3. Signal Timeline Semaphore。
4. 等待 Fence。
5. 查询 Timeline Counter。
6. 确认计数达到提交值。

## 11. 为什么渲染帧仍是串行

当前 Bloom 仍然是 Fullscreen Graphics Pipeline：

```text
BrightExtractPS
BlurHorizontalPS
BlurVerticalPS
```

D3D12 高级 Renderer 的部分回调也仍然直接持有主 `D3D12Context`。

如果此时只把 Pass 标签改成 Compute：

- Graphics Pipeline 不能在 Compute Queue 上执行。
- 回调仍会写主 Graphics Command List。
- 跨队列资源所有权和 Fence Wait/Signal 没有真正发生。

因此当前保持：

```text
nativeMultiQueueSubmissionApplied = false
queueExecutionMode = serial_fallback
```

这不是遗漏报告，而是避免把“队列创建成功”误报成“高级渲染已经并行”。

## 12. RDG Report v3

资源新增：

```text
nativePoolId
nativeAllocation
nativeAllocationBytes
```

编译摘要新增：

```text
transient.native.planValid
transient.native.physicalAliasingApplied
transient.native.allocationCount
transient.native.logicalBytes
transient.native.physicalBytes
transient.native.aliasedBytes
transient.native.expectedAliasingBarrierCount
transient.native.executedAliasingBarrierCount
```

队列新增：

```text
queueInfrastructure.nativeComputeQueueAvailable
queueInfrastructure.dedicatedComputeQueueAvailable
queueInfrastructure.timelineSynchronization
queueInfrastructure.nativeMultiQueueSubmissionApplied
```

## 13. Harness 能力

`engine.describe` 当前包含：

```text
rdgPassCulling = true
rdgResourceLifetimes = true
rdgTransientAliasingPlan = true
rdgQueueSchedule = true
nativeMultiQueueInfrastructure = true
nativeTransientAliasing = true
d3d12PlacedResources = true
vulkanAliasMemory = true
nativeAliasingBarriers = true
nativeAsyncCompute = false
```

`nativeAsyncCompute` 继续为 `false`，因为还没有高级 Compute Pass 通过 RDG 分段提交。

## 14. 自动测试

`RenderGraphTests` 新增验证：

1. 两个 Mock 原生纹理共享同一真实槽。
2. 生命周期不重叠。
3. 原生逻辑字节与物理字节正确。
4. `nativeTransientAliasingApplied=true`。
5. 预计 2 次 Aliasing Barrier。
6. 实际执行 2 次 Aliasing Barrier。
7. RDG Report v3 包含原生分配块。

`EngineHarnessTests` 验证新的能力字段。

真实 Harness 验证两套后端：

- D3D12 Placed Resource。
- Vulkan Alias Memory。
- Compute Queue 提交探针。
- RDG Report v3。
- 双 API Tonemap Golden Image。

## 15. 真实验证结果

验证环境：

```text
GPU: NVIDIA GeForce RTX 5060
分辨率: 640 x 360
构建: Debug
```

D3D12 与 Vulkan 结果一致：

```text
逻辑纹理数                 7
理论物理槽                 5
原生物理分配               5
原生逻辑字节        13,762,560
原生物理字节         9,830,400
原生节省字节         3,932,160
原生计划有效               true
原生别名已应用             true
预计 Aliasing Barrier      4
实际 Aliasing Barrier      4
Compute Queue 可用          true
Dedicated Compute 可用      true
Timeline 同步可用           true
高级多队列提交已应用       false
```

理论字节为 `12,902,400`，原生逻辑字节更大，是因为驱动分配要求包含 GPU 对齐。

Golden Image：

```text
passed                  true
meanAbsoluteError       0.0000640262
rootMeanSquareError     0.000512323
maximumChannelError     0.0196078
changedPixelRatio       0
```

## 16. 验证命令

```powershell
cmake --build build-windows-ci --config Debug
ctest --test-dir build-windows-ci -C Debug --output-on-failure

.\build-windows-ci\Debug\PrismHarness.exe `
  --headless `
  --project-root . `
  --commands examples\harness\rdg_native_resources.jsonl `
  --output automation\stage17k-results.jsonl
```

输出：

- `automation/reports/stage17k-d3d12-rdg.json`
- `automation/reports/stage17k-vulkan-rdg.json`
- `automation/captures/stage17k-d3d12.bmp`
- `automation/captures/stage17k-vulkan.bmp`
- `automation/stage17k-results.jsonl`

## 17. 实现时遇到的问题

第一次 D3D12 真实运行在初始化阶段失败：

```text
D3D12 transient textures require valid allocation metadata.
```

原因是 D3D12 池采用两阶段元数据回填：

1. 创建 Heap 和 Placed Resource。
2. 统计整个池的最终物理字节数。
3. 回填每张纹理的 `poolPhysicalBytes`。

初版在步骤 1 就要求最终池字节非零，因此主动抛出异常。

修正后，初次包装先使用当前已知池字节，全部槽建立后再回填最终值。

这个问题说明：

- 单元测试只能验证编译器逻辑。
- 原生资源生命周期必须通过真实 GPU 初始化验证。
- 结构化 Crash Report 能快速定位初始化阶段错误。

## 18. 设计取舍

### 18.1 为什么池按尺寸创建

当前 TextureView 和 DescriptorSet 在尺寸资源创建阶段绑定稳定纹理对象。

池与窗口尺寸一起创建，Resize 时整体释放和重建，最符合现有所有权模型。

### 18.2 为什么没有每帧重新创建纹理

每帧重建 Image/Resource 会导致：

- DescriptorSet 全量更新。
- CPU 创建开销。
- 资源销毁延迟管理。
- 更复杂的 Frame In Flight 生命周期。

当前池保留稳定逻辑纹理，只复用底层物理内存。

### 18.3 为什么实际字节使用驱动值

宽高乘像素大小只是算法估算。

真正显存分配必须使用：

- D3D12 `GetResourceAllocationInfo`。
- Vulkan `vkGetImageMemoryRequirements`。

Harness 同时显示理论和原生数据，避免错误的显存预算。

## 19. 当前边界

- 资源仍通过字符串名称标识，没有强类型 RDG Resource Handle。
- 物理槽布局由共享延迟渲染布局提供，尚未由 RDG 自动实例化。
- 生命周期粒度仍是整张纹理。
- 没有 Mip/Array Layer 子资源区间。
- 没有 UAV Barrier 和 D3D12 Split Barrier。
- 没有 Vulkan Queue Family Ownership Transfer。
- 没有 RDG Graphics/Compute 分段 Command Buffer。
- 没有高级 Compute Bloom 或 Hi-Z。
- 没有多队列 GPU Timestamp 合并。
- 没有 Copy Queue 和异步上传。

## 20. 推荐阅读顺序

1. `src/RHI/TransientResources.h`
2. `src/Renderer/DeferredTransientLayout.cpp`
3. `src/RHI/D3D12/D3D12TransientResources.cpp`
4. `src/RHI/Vulkan/VulkanTransientResources.cpp`
5. `RenderGraph::BuildNativeTransientAliasingPlan`
6. `RenderGraph::PrepareTextureAliasingBarriers`
7. `D3D12CommandContextAdapter::TextureAliasingBarrier`
8. `VulkanContext::TextureAliasingBarrier`
9. `D3D12Context::ExecuteComputeImmediate`
10. `VulkanContext::ExecuteComputeImmediate`
11. `src/Renderer/RenderGraphDiagnostics.cpp`
12. `tests/RenderGraphTests.cpp`
13. `automation/reports/stage17k-d3d12-rdg.json`
14. `automation/reports/stage17k-vulkan-rdg.json`

## 21. 下一阶段

Stage 17-L 已完成其中的 Compute Bloom、Hi-Z、Graphics/Compute Command 分段、D3D12 Fence 和 Vulkan Timeline Semaphore 接入。

完整实现与验证请继续阅读：

```text
docs/AGENT_RDG_ASYNC_COMPUTE_GUIDE_CN.md
```

Stage 17-K 当时建议进入 Stage 17-L：

1. 新增强类型 `RdgTextureHandle` 和资源版本。
2. 让 RDG 根据编译结果自动创建 Transient Pool，不再由固定布局手工指定槽。
3. 将 Bloom 改写为公共 Compute Shader。
4. 增加 Graphics/Compute 类型化 Command Context。
5. 按 Queue 切分 Command List/Command Buffer。
6. 将跨队列依赖转换为 D3D12 Fence Wait/Signal。
7. 将跨队列依赖转换为 Vulkan Timeline Semaphore Wait/Signal。
8. 必要时执行 Vulkan Queue Family Ownership Transfer。
9. 合并 Graphics/Compute GPU Timestamp。
10. 测量真实 Graphics/Compute Overlap。

完成后才能把：

```text
nativeMultiQueueSubmissionApplied
```

改为 `true`，并进一步判断异步计算是否真正缩短 GPU Frame Time。
