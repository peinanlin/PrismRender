# PrismRender RHI 专用上传队列、Upload Ticket 与持久 Upload Ring

本文记录 Stage 28 的实际实现。目标是让资源上传不再依赖 Graphics Queue，也不再为每个 Mesh 或 Texture 单独创建 staging resource，并让资产流送通过真实 GPU 完成点决定资源何时进入 `Resident`。

实现日期：2026-07-19

## 1. 本阶段解决的问题

旧路径已经能把多个上传合并为批次，但仍有三个缺口：

1. 上传命令占用 Graphics Queue；
2. 每个资源各自创建、映射和销毁 staging resource；
3. `AssetStreamingManager` 无法知道某个上传批次是否真的由 GPU 完成。

新路径为：

```text
Cooked IO 完成
  -> Upload Ring 子分配
  -> 复制命令进入 Copy/Transfer Batch
  -> 分配稳定 UploadTicket
  -> 专用队列提交
  -> Graphics 首个消费者等待
  -> Ready Fence / Timeline 到达
  -> AssetStreamingManager 将资源改为 Resident
```

## 2. 主要文件

### 修改

- `src/RHI/DeviceCapabilities.h`
- `src/RHI/IGraphicsDevice.h`
- `src/RHI/D3D12/D3D12Context.h`
- `src/RHI/D3D12/D3D12Context.cpp`
- `src/RHI/D3D12/D3D12Resources.cpp`
- `src/RHI/Vulkan/VulkanContext.h`
- `src/RHI/Vulkan/VulkanContext.cpp`
- `src/RHI/Vulkan/VulkanResources.cpp`
- `src/Asset/AssetStreamingManager.h`
- `src/Asset/AssetStreamingManager.cpp`
- `src/Renderer/RenderGraphDiagnostics.cpp`
- `src/Automation/HarnessTools.cpp`
- `src/Core/Application.cpp`
- `src/Core/VulkanApplication.cpp`

### 新增验证产物

- `automation/reports/upload-ring-d3d12.json`
- `automation/reports/upload-ring-vulkan.json`
- `automation/reports/production-rhi-d3d12-rdg.json`
- `automation/reports/production-rhi-vulkan-rdg.json`

## 3. Upload Ticket

公共票据定义在 `DeviceCapabilities.h`：

```cpp
struct UploadTicket
{
    std::uint64_t value = 0;
    bool IsValid() const;
};
```

`IGraphicsDevice` 提供：

```cpp
UploadTicket GetPendingUploadTicket() const;
bool IsUploadComplete(UploadTicket ticket) const;
```

同一待提交批次内创建的资源共享一个 Ticket。Ticket 在首次录制上传命令时分配，提交后成为 `lastSubmittedTicket`，原生同步对象达到该值后成为 `completedTicket`。

资产状态机因此从推测式逻辑：

```text
全局批次数增加 -> 假设本资源完成
```

改为确定性逻辑：

```text
entry.uploadTicket 有效
  && device.IsUploadComplete(entry.uploadTicket)
  -> Uploading -> Resident
```

## 4. D3D12 实现

### 4.1 两段命令录制

COPY 类型 Command List 不允许执行完整的图形资源状态转换，因此 `QueueUpload` 接收两个回调：

```text
recordCopyCommands
  -> CopyBufferRegion / CopyTextureRegion

recordFinalizeCommands
  -> COPY_DEST 到最终状态的 ResourceBarrier
```

### 4.2 提交与同步

```text
Copy Queue
  -> Execute copy list
  -> Signal copyFence(ticket)

Graphics Queue
  -> Wait copyFence(ticket)
  -> Execute finalize list
  -> Signal uploadFence(ticket)
```

对外的 Ticket 绑定 `uploadFence`，所以“完成”不仅表示字节复制结束，也表示资源已经进入可供渲染读取的状态。

### 4.3 持久 Upload Ring

每个页默认 16 MiB，资源在页内按对齐要求做 bump allocation：

- Buffer：16 字节；
- Texture：`D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT`，即 512 字节；
- 超过默认页大小时创建按 64 KiB 向上对齐的大页。

页创建后只映射一次。提交时记录 `lastTicket`，只有该 Ticket 完成后才允许把页的 cursor 重置为 0。

## 5. Vulkan 实现

### 5.1 Transfer Family 选择

队列族选择优先级：

1. 仅支持 Transfer；
2. 支持 Transfer/Compute 但不支持 Graphics；
3. Graphics Queue Family 回退。

能力表通过 `copyQueue` 和 `dedicatedCopyQueue` 报告实际结果。

### 5.2 资源共享

上传目标 Buffer/Image 在 Graphics、Compute 和 Transfer 三个队列族之间采用去重后的 `VK_SHARING_MODE_CONCURRENT`。这样专用 Transfer Queue 不需要为每个资源维护额外的 queue-family ownership transfer。

纹理上传命令在 Transfer Queue 中完成：

```text
UNDEFINED
  -> TRANSFER_DST_OPTIMAL
  -> vkCmdCopyBufferToImage
  -> SHADER_READ_ONLY_OPTIMAL
```

最后一个 barrier 使用 Transfer 到 Bottom-of-Pipe；真正的消费者可见性由 Timeline Semaphore wait 建立。

### 5.3 Timeline 注入点

`pendingUploadWaitValue` 会进入所有可能成为首个消费者的提交路径：

- 普通 `EndFrame`；
- 传统 Graphics/Compute 队列切换；
- RDG 独立 Queue Batch 提交。

因此启用原生多队列 RDG 时，Compute 或 Graphics 都不会越过尚未完成的上传。

### 5.4 持久 Upload Ring

Vulkan 同样使用 16 MiB、HOST_VISIBLE/HOST_COHERENT、持久映射的页。Buffer copy 使用子分配 `srcOffset`，Texture copy 使用 `VkBufferImageCopy::bufferOffset`。

页通过 Upload Timeline 的 counter value 回收，不依赖 CPU 每帧等待。

## 6. 可观测性

`UploadQueueStatistics` 新增：

```text
pendingTicket
lastSubmittedTicket
completedTicket
outstandingBatchCount
stagingPageCount
stagingCapacityBytes
stagingHighWatermarkBytes
```

应用还支持：

```text
PRISM_RENDER_MAX_FRAMES
PRISM_RENDER_ASSET_STREAMING_REPORT_PATH
```

这两个入口让自动测试能够隐藏运行固定帧数，在 GPU 等待后泵一次状态机并导出完整资产驻留报告。

## 7. 验证

### 7.1 构建

```powershell
cmake --build build-windows-ci --config Debug --parallel 8
```

结果：成功。

### 7.2 D3D12

```text
Resident: 8/8
Failed: 0
Upload Ticket: 3/3 completed
Outstanding batch: 0
Upload pages: 1
Capacity: 16 MiB
High watermark: 1,049,904 bytes
```

### 7.3 Vulkan

测试同时启用 `PRISM_RENDER_RDG_QUEUE_MODE=native`：

```text
Resident: 8/8
Failed: 0
Upload Ticket: 4/4 completed
Outstanding batch: 0
Upload pages: 1
Capacity: 16 MiB
High watermark: 1,582,752 bytes
```

两端 RDG 报告均确认：

```text
copyQueue = true
dedicatedCopyQueue = true
timelineSynchronization = true
```

## 8. 设计边界

当前 Ring 是按页增长、按 Ticket 整页回收，不是可在页内任意回收洞的通用内存分配器。这个选择让生命周期简单且确定，适合批量资源上传。后续大规模开放世界流送可在此基础上增加：

- 每帧上传带宽预算；
- 多优先级上传队列；
- 大资源分块；
- DirectStorage 或 Vulkan 压缩传输扩展；
- 更细粒度的环形 head/tail 回收。

这些扩展不改变 `UploadTicket` 和资产状态机契约。
