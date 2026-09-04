# Stage 24：资产流送与驻留管理学习指南

## 1. 本阶段解决什么问题

原来的 `AssetRuntimeLoader` 会同步读取整个 Manifest、读取全部 Cooked Asset 并立即创建全部 GPU 资源。它适合确定性测试和小场景，但场景变大后会产生启动停顿，并让显存占用随内容总量增长。

本阶段保留同步 Loader 作为兼容路径，同时增加 `AssetStreamingManager`，把资产加载拆成可调度的生命周期。

## 2. 文件

新增：

- `src/Asset/AssetStreamingManager.h`
- `src/Asset/AssetStreamingManager.cpp`
- `docs/ASSET_STREAMING_GUIDE_CN.md`

修改：

- `src/Core/Application.h/.cpp`
- `src/Core/VulkanApplication.h/.cpp`
- `src/Automation/HarnessTools.h/.cpp`
- `tests/EngineHarnessTests.cpp`
- `CMakeLists.txt`

## 3. 状态机

```text
Unloaded
  -> Queued
  -> LoadingIo
  -> ReadyForUpload
  -> Uploading
  -> Resident
  -> Evicted

Material/Scene:
LoadingIo -> WaitingForDependencies -> ReadyForUpload/Resident

任意读取或上传错误 -> Failed
```

| 状态 | 所属线程 | 含义 |
| --- | --- | --- |
| `Queued` | 请求线程 | 已进入优先级队列 |
| `LoadingIo` | 后台线程 | 正在校验和解析 Cooked Asset |
| `ReadyForUpload` | 渲染线程可见 | CPU 数据完整，可创建 RHI 资源 |
| `Uploading` | GPU 队列 | staging 数据已排队，等待后端提交批次 |
| `Resident` | 渲染线程 | 运行时资源可以被材质或 Draw 使用 |
| `Evicted` | 驻留管理器 | Runtime 引用已释放，可重新请求 |

## 4. 数据流

```text
请求稳定 Asset ID
  -> 根据 AssetRecord.dependencies 递归展开依赖
  -> 优先级队列
  -> IO Worker 读取 Cooked v2
  -> Completion Queue
  -> Render Thread PumpIoCompletions
  -> 每帧 maxUploadsPerTick
  -> IGraphicsDevice::CreateBuffer/CreateTexture
  -> D3D12/Vulkan Upload Queue
  -> submittedBatchCount 确认提交
  -> Resident
```

Cooked 文件解析不会创建 GPU 对象。`IGraphicsDevice` 只在 `TickUploads` 中被调用，避免后台线程并发修改命令列表、Descriptor Allocator 或 Frame Context。

## 5. 依赖处理

请求 Scene 时会递归请求 Mesh 和 Material，Material 又会递归请求五类 Texture。Material 只有在所有 Texture 都处于 `Resident` 后才允许创建运行时对象；Scene 是逻辑聚合资产，所有直接依赖驻留后自身进入 `Resident`。

请求使用稳定 Asset ID 或 `prism-asset://` 路径，不依赖 Registry 的临时数组下标。

## 6. 为什么 CreateTexture 返回不等于上传完成

两个后端都会先记录 Copy，再在帧边界批量提交。资源创建返回只表示上传操作已排队，不表示 GPU 已执行。Manager 在创建资源前后读取：

- `pendingOperationCount`
- `submittedBatchCount`

若新增了待上传操作，资产进入 `Uploading` 并记录目标 Batch。后续帧发现 Pending 已清空且提交编号到达目标值后，才标记为 `Resident`。

## 7. 驻留预算和淘汰

`residentBudgetBytes` 限制 Manager 负责的资源估算值。超预算时：

1. 排除 `pinned` 和仍有请求引用的资产。
2. 优先释放 Material，让其 Texture 引用先消失。
3. 再按 `lastTouchedSerial` 从旧到新释放 Mesh/Texture。
4. 若对象仍被 RenderScene 等外部所有者持有，则跳过，禁止制造悬空引用。

当前估算包含 Mesh 顶点/索引和 Texture 像素主体；驱动分配粒度、Mip 和 Descriptor 开销后续可由 RHI 的真实显存预算接口校正。

## 8. Vulkan 运行时接入

```powershell
$env:PRISM_RENDER_API = "vulkan"
$env:PRISM_RENDER_ASSET_STREAMING = "1"
$env:PRISM_RENDER_ASSET_STREAMING_BUDGET_MB = "512"
.\build-windows-ci\Debug\PrismRender.exe
```

D3D12 和 Vulkan 应用都在 `BeginFrame` 后调用 `TickUploads` 和 `EvictToBudget`。D3D12 复用 `SceneRenderer` 已有的 `D3D12GraphicsDevice` 公共 RHI 包装，Vulkan 直接使用 `VulkanContext` 的 `IGraphicsDevice`，没有复制后端专用资产算法。

确定性 Harness 截图默认继续走同步 Loader，因为 Golden Image 必须固定资产完成时刻；交互运行时才通过环境变量显式启用异步路径。

## 9. Agent 可观察入口

```json
{
  "requestId": "stream-plan-1",
  "command": "asset.streaming.plan",
  "arguments": {
    "assetId": "scene-asset-id",
    "priority": 100,
    "pin": false,
    "residentBudgetBytes": 536870912
  }
}
```

返回每个依赖的状态、优先级、引用数、字节数和错误，并给出 IO 总量。该命令只执行后台读取，不在 Headless Harness 中伪造 GPU 驻留。

## 10. 验证

`EngineHarnessTests` 导入 StartupScene、请求 Scene 稳定 ID、等待后台 IO，并验证七个 Mesh/Material/Texture 依赖已完成读取、Scene 正确等待上传且没有失败项。测试还检查 Harness 暴露 `asset.streaming.plan`。

这一阶段完成的是异步 IO、RHI 上传衔接和驻留状态机。虚拟纹理、分块 Mesh、DirectStorage 和按相机距离自动请求属于更高层内容虚拟化，不应混入基础状态机。

## 11. Stage 29 更新

Stage 29 已将本章的基础状态机闭环到真实 `RenderScene`：

- Scene Manifest 保存 Mesh/Material 稳定 ID 和实例世界矩阵；
- `AssetStreamingSceneBridge` 在全部依赖 Resident 后原子激活对象；
- Request/Release/Pin 对整个依赖闭包保持对称；
- D3D12/Vulkan 都输出结构化场景绑定报告；
- 自动截图延迟到激活后的稳定帧，并执行双 API Golden Image。

本章第 6 节早期使用批次计数推断完成状态的描述，已由 Stage 28 的
显式 `UploadTicket` 取代。完整实现与验证结果见：

- [RHI_UPLOAD_QUEUE_RING_GUIDE_CN.md](RHI_UPLOAD_QUEUE_RING_GUIDE_CN.md)
- [ASSET_STREAMING_SCENE_ACTIVATION_GUIDE_CN.md](ASSET_STREAMING_SCENE_ACTIVATION_GUIDE_CN.md)
