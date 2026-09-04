# PrismRender 生产级 RHI：能力、同步与资源生命周期

本文记录架构路线 Stage 19 的实际实现。目标不是增加一个画面特效，而是把公共 RHI 从“能够画出结果”提升为“能够长期、动态并且可诊断地运行”。

实现日期：2026-07-19

## 1. 本阶段解决的问题

此前公共 RHI 已经支持 Buffer、Texture、DescriptorSet、Graphics/Compute Pipeline、Rendering Scope、Barrier、Transient Resource 和多队列提交，但仍有四个生产环境问题：

1. 上层无法查询设备能力，只能假设功能存在；
2. 屏障只完整覆盖 Texture，Buffer 和全局 UAV 顺序缺少公共表达；
3. D3D12 Shader-visible Heap 只增不减，Vulkan 只有一个固定 DescriptorPool；
4. 每创建一个 GPU-only 资源都会单独提交并等待 GPU，资源析构也可能早于 GPU 完成。

这些问题在静态演示场景中不一定暴露，但在编辑器热重载、资产流送、场景切换和 Agent 长时间自动化中会逐渐演变为 Heap 耗尽、初始化卡顿或 GPU use-after-free。

## 2. 文件范围

### 2.1 新增文件

- `src/RHI/DeviceCapabilities.h`
- `docs/PRODUCTION_RHI_GUIDE_CN.md`
- `examples/harness/production_rhi_validation.jsonl`

### 2.2 主要修改文件

- `src/RHI/IGraphicsDevice.h`
- `src/RHI/ICommandContext.h`
- `src/RHI/PipelineState.h`
- `src/RHI/DeferredCommandContext.*`
- `src/RHI/D3D12/D3D12Context.*`
- `src/RHI/D3D12/D3D12GraphicsDevice.*`
- `src/RHI/D3D12/D3D12CommandContextAdapter.*`
- `src/RHI/D3D12/D3D12Resources.*`
- `src/RHI/Vulkan/VulkanContext.*`
- `src/RHI/Vulkan/VulkanResources.*`
- `src/RHI/Vulkan/VulkanPipeline.cpp`
- `src/RHI/Vulkan/VulkanParallelCommandRecording.cpp`
- `src/Renderer/RenderGraphDiagnostics.*`
- `src/Core/Application.cpp`
- `src/Core/VulkanApplication.cpp`
- `tests/RenderGraphTests.cpp`

## 3. 设备能力契约

公共入口是：

```cpp
const GraphicsDeviceCapabilities& GetCapabilities() const;
```

`GraphicsDeviceCapabilities` 分为三部分：

- `graphicsApi` 和 `adapterName`：报告实际后端与适配器；
- `DeviceLimits`：纹理尺寸、数组层数、颜色附件数、对齐和描述符容量；
- `DeviceFeatures`：Compute/Copy Queue、Timeline、Dynamic Rendering、Descriptor Indexing、Buffer Device Address、Indirect Count 等。

D3D12 从 Feature Support 和资源绑定等级构建能力表；Vulkan 从 `VkPhysicalDeviceProperties`、核心 Feature 和 Vulkan 1.2 Feature 构建能力表。上层以后不能因为“编译的是 Vulkan”就默认某项功能存在，而应依据 Capability 选择路径或明确拒绝。

数据流为：

```text
Native Device Query
  -> D3D12Context / VulkanContext
  -> GraphicsDeviceCapabilities
  -> IGraphicsDevice::GetCapabilities()
  -> Renderer feature selection
  -> RDG v10 / Harness diagnostics
```

## 4. 公共 Buffer 与 Global Barrier

本阶段加入：

```cpp
struct BufferBarrier
{
    IBuffer* buffer;
    ResourceState before;
    ResourceState after;
    std::size_t offset;
    std::size_t size;
};

struct GlobalBarrier
{
    ResourceState before;
    ResourceState after;
};
```

### 4.1 D3D12 映射

- Buffer 状态变化映射为 `D3D12_RESOURCE_BARRIER_TYPE_TRANSITION`；
- UAV 到 UAV 的同状态依赖映射为 UAV Barrier；
- Global Barrier 当前用于表达全局 UAV 顺序。

### 4.2 Vulkan 映射

- Buffer Barrier 映射为 `VkBufferMemoryBarrier`；
- Global Barrier 映射为 `VkMemoryBarrier`；
- `ResourceState` 同时转换成 Pipeline Stage、Access Mask 和必要的 Layout。

`DeferredCommandContext` 和 Vulkan 原生并行录制 Context 都实现了相同接口，因此串行、延迟并行和原生并行路径不会丢失屏障。

## 5. D3D12 描述符区间分配器

原实现使用两个单调递增下标：

```text
allocate -> next += count
destroy  -> 不回收
```

新实现维护有序空闲区间：

```text
Free ranges: [first, count]
Allocate:     first-fit，切掉区间前部
Retire:       放入当前 FrameContext
Fence done:   放回空闲表，排序并合并相邻区间
```

SRV Heap 的第 0 个描述符仍保留给 ImGui，因此公共资源容量为 `ShaderVisibleSrvCapacity - 1`。Sampler Heap 从 0 开始。

为什么不是在 `D3D12DescriptorSet` 析构时立即归还：

```text
CPU shared_ptr 归零
  !=
GPU 已经执行完引用该 Descriptor Table 的命令
```

立即复用会使下一份材质覆盖仍在飞行帧使用的描述符。现在析构只执行 `RetireShaderVisible*Range`，真正回收发生在对应 Frame Fence 完成后的 `BeginFrame`。

## 6. Vulkan 可扩容 DescriptorPool

原实现只有一个固定 512 Set 的 Pool。新分配器包含：

- 多个 `PoolState`；
- 初始容量 256；
- Pool 耗尽或碎片化后按 2 倍增长；
- 单个 Pool 最大增长到 4096，之后可以继续创建同容量 Pool；
- 每个 Set 记录来源 Pool；
- 每帧独立退休列表；
- Fence 完成后调用 `vkFreeDescriptorSets`。

分配流程：

```text
Allocate(layout)
  -> 依次尝试现有 Pool
  -> OUT_OF_POOL / FRAGMENTED
  -> CreatePool(grow capacity)
  -> Allocate from new Pool
```

销毁流程：

```text
VulkanDescriptorSet::~VulkanDescriptorSet
  -> Retire(pool, set, currentFrame)
  -> vkWaitForFences(frame)
  -> ReclaimFrame(frame)
  -> vkFreeDescriptorSets
```

分配器使用 Mutex 保护 Pool、统计和退休队列，为后续资产线程创建资源保留并发安全基础。

## 7. 批量上传

### 7.1 旧路径

```text
Create resource A
  -> Create staging A
  -> Submit
  -> Wait GPU
Create resource B
  -> Create staging B
  -> Submit
  -> Wait GPU
```

资源越多，CPU/GPU 往返等待越严重。

### 7.2 新路径

```text
Create resource A -> record copy A
Create resource B -> record copy B
Create resource C -> record copy C
BeginFrame / immediate dependency
  -> close one upload command buffer
  -> submit one batch
  -> retain all staging objects
```

D3D12 使用专用 Copy Queue 和分离的 Graphics Finalize List；Vulkan 优先使用专用 Transfer Queue。两端都把待上传资源合并到当前批次。

正常首帧路径不做 CPU Wait。上传命令与首帧命令提交到同一 Graphics Queue，队列顺序保证渲染读取发生在 Copy 之后，而首帧 Fence 同时保护 Staging 生命周期。

确实存在立即依赖时，例如初始化阶段马上执行 IBL Immediate Command，调用同步 Flush。该路径仍然正确，但同步次数会进入统计，便于继续优化。

Stage 28 已在此基础上补齐专用 Copy/Transfer Queue、持久映射 Upload Ring 和真实 Upload Ticket。完整提交、回收和验证过程见 `RHI_UPLOAD_QUEUE_RING_GUIDE_CN.md`。带宽预算和大资源分块仍属于后续开放世界资产流送扩展。

## 8. GPU 资源退休队列

新增统一统计：

```cpp
struct ResourceRetirementStatistics
{
    uint64_t totalRetiredObjectCount;
    uint64_t totalReclaimedObjectCount;
    uint32_t pendingObjectCount;
    uint32_t pendingObjectHighWatermark;
};
```

### 8.1 D3D12

公共 Buffer 和 Texture 在析构时把 `ComPtr<ID3D12Resource>` 移入当前 `FrameContext`。Fence 完成后清空该帧队列。

### 8.2 Vulkan

`VulkanContext::RetireGpuObject` 接收只依赖 `VkDevice` 的销毁回调。以下对象已接入：

- Buffer 与 DeviceMemory；
- Image、ImageView 与 DeviceMemory；
- 独立 TextureView；
- Sampler；
- DescriptorSetLayout；
- Graphics/Compute Pipeline 与 PipelineLayout。

Transient Texture 的回调还会捕获 Allocation Owner，保证别名内存在所有 Image 真正销毁前不会释放。

上下文关闭时先 `vkDeviceWaitIdle`，再执行仍未轮转到的退休回调，因此退出路径也不会泄漏。

## 9. RDG v10 可观测性

`rdg.describe` 的 `rhi` 节点包含：

```json
{
  "rhi": {
    "deviceCapabilities": {},
    "descriptorAllocator": {},
    "uploadQueue": {},
    "resourceRetirement": {}
  }
}
```

Agent 可以据此判断：

- 当前机器是否支持下一项渲染功能；
- Descriptor Set 或描述符是否持续增长；
- 上传是否被合并，是否发生过多同步 Flush；
- 退休对象是否长期只增不减。

常见诊断规则：

```text
allocated 高且长期不降
  -> DescriptorSet 或材质生命周期泄漏

submittedBatchCount 接近资源数量
  -> 上传没有有效合并

synchronousFlushCount 持续逐帧增长
  -> 热路径发生 GPU 等待

pendingObjectCount 跨多个 FrameCount 不下降
  -> Fence 轮转或资源所有权异常
```

## 10. 自动验证

### 10.1 构建与测试

```powershell
cmake --build build-windows-ci --config Debug --parallel 8
ctest --test-dir build-windows-ci -C Debug --output-on-failure
```

结果：完整 Debug 构建成功，8/8 测试通过。

`RenderGraphTests` 新增两类覆盖：

- Deferred Command 对 Buffer/Global Barrier 的无损回放；
- RDG v10 对设备、描述符、上传和退休统计的 JSON 序列化。

### 10.2 双 API 真实运行

```powershell
.\build-windows-ci\Debug\PrismHarness.exe `
  --headless `
  --project-root . `
  --commands examples\harness\production_rhi_validation.jsonl `
  --output automation\stage19-results.jsonl
```

三条命令全部成功：

- D3D12 `rdg.describe`；
- Vulkan `rdg.describe`；
- D3D12/Vulkan Tonemap Golden Image 对比。

本机为 NVIDIA GeForce RTX 5060。图像结果：

| 指标 | 结果 | 阈值 |
| --- | ---: | ---: |
| MAE | 0.0000640262 | 0.001 |
| RMSE | 0.000512323 | 0.005 |
| 最大通道误差 | 0.0196078 | 像素容差 8/255 |
| 变化像素比例 | 0 | 0.01 |

Vulkan 报告显示约 2.81 MB 初始化数据合并为一个上传批次。D3D12 高级渲染前端目前仍有一部分旧的原生资源创建路径，所以该场景的公共 Upload Queue 统计为 0；这不是上传管理器失效，而是 Stage 21“统一双 API 渲染前端”需要继续消除的旧路径。

## 11. 设计取舍

### 11.1 为什么暂时不是 Bindless

本阶段先解决有界 DescriptorSet 的正确回收和扩容。Bindless 需要稳定索引、代际验证、更新并发策略以及 Shader 访问模型，属于 GPU Driven 阶段，不能用一个超大 Heap 代替生命周期设计。

### 11.2 为什么上传需要 Copy/Transfer Queue 与 Graphics 收尾

复制工作与图形工作分离后，大批量 Mesh/Texture 上传不再占用 Graphics Queue。D3D12 在 Copy Fence 后由 Graphics Queue 执行最终状态转换；Vulkan 使用并发队列族共享、最终 Layout 转换和 Timeline Wait。对外 Ticket 只在资源真正可消费后完成，上层资源构造与资产状态机不需要了解原生同步对象。

### 11.3 为什么退休回调属于 Context

只有 Context 知道 Frame Fence 何时完成。资源对象只声明“我不再被 CPU 所有”，不能自行判断 GPU 生命周期。

## 12. 下一阶段

下一阶段是完整 RDG 资源模型，重点包括：

1. Buffer、Texture、View 的统一 RDG Handle；
2. Mip、Array Layer、Buffer Range 子资源追踪；
3. 自动生成 Texture、Buffer 和 Global Barrier；
4. Imported、Persistent、Transient、History 资源；
5. Blackboard 与 Pass Parameter；
6. 资源版本、写后读依赖和无效 Handle 校验；
7. Capture/Inspect 接口与资源生命周期图；
8. 在双 API 后端验证相同 Barrier Plan。

完成后，高级 Pass 才能只声明资源读写，而不再手工维护跨 API 状态细节。
