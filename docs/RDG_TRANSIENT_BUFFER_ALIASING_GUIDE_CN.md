# RDG 瞬态 Buffer 原生别名学习指南

## 1. 本阶段解决的问题

此前 `RenderGraph::DeclareTransientBuffer()` 只会计算逻辑生命周期和复用槽。
真正的 GPU Buffer 仍由普通 committed/allocation 路径创建，因此报告中的
`transientAliasedBytes` 只是编译器规划值，并没有减少原生显存分配。

本阶段把这条链路补完整：

```text
RDG first/last use
  -> physicalAllocation
  -> TransientBufferRequest
  -> IGraphicsDevice::CreateTransientBufferPool
  -> D3D12 Heap + Placed Resource
     或 Vulkan VkDeviceMemory + 多个 VkBuffer
  -> IBuffer::GetTransientAllocationInfo
  -> RDG 原生别名组
  -> 首次使用前 Buffer Aliasing Barrier
```

## 2. 公共 RHI 如何实现

`src/RHI/TransientResources.h` 新增：

- `TransientBufferRequest`
- `TransientBufferPoolStatistics`
- `ITransientBufferPool`
- `ValidateTransientBufferRequests()`
- `RunTransientBufferPoolValidation()`

`IGraphicsDevice` 新增 `CreateTransientBufferPool()`。上层只提交资源描述和
RDG 分配槽，不接触 `ID3D12Heap`、`VkDeviceMemory` 等平台对象。

`IBuffer` 新增只读的 `TransientBufferAllocationInfo`，包含：

- Pool ID
- Allocation Index
- 单资源逻辑字节数
- 当前槽的物理字节数
- 整个 Pool 的物理字节数

普通 Buffer 返回空指针，只有瞬态池创建的 Buffer 才带有这组元数据。

## 3. D3D12 后端

`D3D12TransientBufferPool` 为每个物理槽创建一个
`D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS` Heap，然后为共享该槽的每个逻辑
Buffer 调用 `CreatePlacedResource()`，Offset 固定为 0。

共享槽内的描述必须一致，RDG 已保证这些 Buffer 的活跃生命周期不重叠。
Buffer 包装器持有 Heap Owner，确保 Placed Resource 存活期间 Heap 不会被释放。

切换逻辑资源时，D3D12 后端记录
`D3D12_RESOURCE_BARRIER_TYPE_ALIASING`，其中 Before/After 指向两个
Placed Resource。

## 4. Vulkan 后端

`VulkanTransientBufferPool` 先创建所有 `VkBuffer`，收集
`VkMemoryRequirements`，再对同一槽求：

- 最大 allocation size
- 可共同使用的 memoryTypeBits 交集

每个槽只分配一次 Device Local `VkDeviceMemory`，该槽内的多个 `VkBuffer`
都通过 `vkBindBufferMemory(..., 0)` 绑定到同一段内存。

Vulkan 没有 D3D12 同名的 Buffer Aliasing Barrier。切换资源时记录覆盖全部
命令阶段的 `VkMemoryBarrier`，保证前一个逻辑 Buffer 的写入在后一个逻辑
Buffer 读写前完成。

延迟销毁 Lambda 会持有 Memory Owner，最后一个 Buffer 真正销毁后才释放
共享 `VkDeviceMemory`。

## 5. RDG 编译和执行

`BuildResourceLifetimes()` 会读取 Buffer 的原生分配元数据。
`BuildNativeTransientAliasingPlan()` 现在统一处理 Texture 和 Buffer，不再把
活跃瞬态 Buffer 直接判定为“未原生化”。

`PrepareAliasingBarriers()` 在资源第一次使用时：

1. 找到同一 Pool/Allocation 的活跃逻辑资源；
2. 找到生命周期最近结束的前一个资源；
3. 记录 Texture 或 Buffer 对应的别名屏障；
4. 将新资源状态重置为 `Undefined`；
5. 再由普通自动 Barrier 转换到 Pass 请求的目标状态。

因此别名屏障负责“物理内存换了逻辑身份”，状态屏障负责“新身份以什么方式使用”。

## 6. 验证

单元测试新增两个带原生元数据的 Mock Buffer，验证：

- 两个逻辑 Buffer 只有一个原生 Allocation；
- Logical Bytes 为 8192，Physical Bytes 为 4096；
- RDG 执行两次 Buffer Aliasing Barrier；
- 生命周期依赖和普通 Buffer Barrier 仍然成立。

运行时验证使用：

```powershell
$env:PRISM_RENDER_VALIDATE_TRANSIENT_BUFFERS = '1'
$env:PRISM_RENDER_CAPTURE_PATH = 'automation/captures/transient.bmp'
$env:PRISM_RENDER_EXIT_AFTER_CAPTURE = '1'
.\build-windows-ci\Debug\PrismRender.exe --api=d3d12
.\build-windows-ci\Debug\PrismRender.exe --api=vulkan
```

该开关会在当前后端真实创建两个共享槽的 GPU Buffer，并校验 Pool 统计和
分配元数据。2026-07-19 的验证结果：

```text
Full Debug build: passed
RenderGraphTests: passed
D3D12 native transient-buffer validation and capture: passed
Vulkan native transient-buffer validation and capture: passed
Cross-API MAE: 0.000064
Cross-API RMSE: 0.000512
Changed pixel ratio: 0.000009
```

报告位于 `automation/reports/audit-transient-buffer-parity.json`。

## 7. 设计边界

瞬态 Buffer Pool 解决的是 RDG 帧内资源内存复用，不是资产上传系统。
Copy Queue、持久 Upload Ring、逐资源 Upload Ticket 和带宽调度仍属于
生产级 RHI/资产流送的后续工作，不能用本阶段结果替代。
