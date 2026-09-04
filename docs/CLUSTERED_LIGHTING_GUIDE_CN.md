# Clustered Lighting 实现和学习指南

## 1. 为什么不能继续在 Shader 中循环所有灯光

旧实现把 4 个点光源直接放入 Frame Constant Buffer，Deferred Shader 的每个像素
都循环这 4 盏灯。灯光增加到几十或几百盏后，每个像素遍历全部灯光会浪费大量 GPU
计算。

Clustered Lighting 把相机视锥划分为许多小的三维区域。每个 Cluster 只保存真正
影响该区域的灯光索引，像素只遍历所属 Cluster 的短列表。

## 2. 数据流

```text
RenderScene PointLights（最多 128）
  -> 每飞行帧 GPU Light StructuredBuffer
  -> ClusteredLightBuild Compute
       X/Y：16x16 像素 Tile
       Z：16 个对数深度切片
       对每个 Cluster 测试灯光球体
  -> ClusterLightCounts
  -> ClusterLightIndices（每 Cluster 最多 64）

GBuffer Pixel
  -> 根据 SV_POSITION 找 X/Y Tile
  -> 根据像素世界位置距离找 Z Slice
  -> 取得 Cluster Index
  -> 只遍历该 Cluster 的灯光
  -> PBR 点光源累加
```

## 3. Cluster 如何划分

### 3.1 屏幕 X/Y

屏幕按 `16x16` 像素分 Tile：

```text
tileCountX = ceil(width / 16)
tileCountY = ceil(height / 16)
```

### 3.2 深度 Z

近处分得更细、远处分得更粗，因此使用 16 个对数切片：

```text
sliceNear = near * pow(far / near, slice / sliceCount)
sliceFar  = near * pow(far / near, (slice + 1) / sliceCount)
```

这种分布更符合透视相机中“近处变化大、远处变化小”的特点。

## 4. GPU 如何建立灯光列表

`BuildLightClustersCS` 为每个 Cluster 启动一个线程：

1. 从 Cluster Index 解出 Tile X、Tile Y 和 Depth Slice。
2. 把每个灯光中心从世界空间转换到 View Space。
3. 用灯光 Range 判断球体是否与深度切片重叠。
4. 把灯光球投影到 NDC，判断是否与屏幕 Tile 重叠。
5. 把命中的灯光索引写入 `ClusterLightIndices`。
6. 把命中数量写入 `ClusterLightCounts`。

每个 Cluster 最多保存 64 盏灯。超出的灯会被截断，这是显式的性能和内存上限。

## 5. Deferred Shader 如何使用

Deferred Pixel Shader 使用像素坐标计算 X/Y Tile，使用世界位置到相机的距离计算
Z Slice，然后读取：

```text
count = ClusterLightCounts[cluster]
lightIndex = ClusterLightIndices[cluster * 64 + i]
light = PointLights[lightIndex]
```

接着继续使用项目已有的 GGX PBR 函数，不复制另一套光照模型。

关闭 Clustered Lighting 时，Deferred 回退到旧的 4 灯循环；Forward Rendering
也保留旧 4 灯常量路径。这样新架构可以增量接入而不破坏兼容模式。

## 6. 每飞行帧资源

每个飞行帧拥有独立的：

- Cluster Constants
- Point Light Buffer
- Cluster Count Buffer
- Cluster Index Buffer
- DescriptorSet

如果两帧共用同一份可写 Cluster Buffer，CPU/GPU 和两个队列可能同时覆盖数据。
按飞行帧分配可以让 Frame Fence 自然管理生命周期。

## 7. 新增的 RHI 能力

Slang 对以下两种资源给出不同 Reflection：

```hlsl
StructuredBuffer<T>   // 只读
RWStructuredBuffer<T> // 读写
```

公共 RHI 因此新增：

- `DescriptorType::ReadOnlyStorageBuffer`
- `BufferUsage::ShaderResource`

后端映射：

```text
ReadOnlyStorageBuffer
  -> D3D12 Structured Buffer SRV
  -> Vulkan VK_DESCRIPTOR_TYPE_STORAGE_BUFFER（Shader 只读）

StorageBuffer
  -> D3D12 UAV
  -> Vulkan VK_DESCRIPTOR_TYPE_STORAGE_BUFFER（Shader 读写）
```

这使读写权限、D3D12 Descriptor Range 和资源状态都能正确表达，而不是把只读
StructuredBuffer 伪装成 UAV。

## 8. RDG 如何表达

新增 `ClusteredLightBuild` Compute Pass：

- 读 Cluster Constants
- 读 Point Lights
- 写 Cluster Counts
- 写 Cluster Indices

`DeferredLighting` 声明读取这四个 Buffer。RDG 自动生成 Compute 写入到 Graphics
读取之间的 Buffer Barrier 和跨队列同步。

## 9. 主要文件

- `src/Renderer/Features/ClusteredLighting.h/.cpp`：资源、数据上传、Pipeline 和 Dispatch
- `assets/shaders/ClusteredLighting.hlsl`：Cluster 灯光相交测试
- `assets/shaders/Deferred.hlsl`：像素 Cluster 查找与灯光循环
- `src/Scene/RenderScene.h`：场景灯光容量从 4 扩展到 128
- `src/RHI/GraphicsResources.h`：只读 Buffer 公共契约
- `src/RHI/ShaderLayoutBuilder.cpp`：Slang Reflection 映射
- `src/RHI/D3D12/D3D12Resources.cpp`：Structured Buffer SRV
- `src/RHI/Vulkan/VulkanResources.cpp`：Vulkan Storage Buffer
- `src/Renderer/SharedRenderGraphFrontend.cpp`：RDG Pass 与 Buffer 依赖

## 10. 权衡与后续升级

- 当前每个 Cluster 顺序测试全部活动灯光。128 灯规模可用；数千灯时应改为
  并行灯光投影、前缀和或分层 Bin。
- 当前 Cluster 上限 64，溢出时直接截断。生产版本应增加 Overflow Counter 和
  Debug Heatmap。
- 当前 Z Slice 查找使用世界距离，Build 使用 View Z，视野边缘可能选择相邻
  Slice。可把 GBuffer 增加 Linear View Depth 以完全统一。
- 当前只包含 Point Light。Spot Light 可使用锥体包围球做初筛，再做锥体测试。
- Forward 路径仍是 4 灯兼容模式；Forward+ 或透明物体应复用 Cluster Buffer。

## 11. 验证

```powershell
cmake --build build-windows-ci --config Debug --target PrismRender PrismHarness PrismShaderCompilerTests PrismRenderGraphTests PrismRhiTranslationTests PrismWorldRenderSceneBridgeTests
.\build-windows-ci\Debug\PrismShaderCompilerTests.exe
.\build-windows-ci\Debug\PrismRenderGraphTests.exe
.\build-windows-ci\Debug\PrismRhiTranslationTests.exe
.\build-windows-ci\Debug\PrismWorldRenderSceneBridgeTests.exe
.\build-windows-ci\Debug\PrismHarness.exe --commands examples\harness\gpu_driven_validation.jsonl --output automation\reports\stage-clustered-validation.jsonl
```

本阶段结果：

- 15 个 Shader 入口共 30 个 DXIL/SPIR-V 程序通过。
- Reflection 验证只读灯光 Buffer 与读写 Cluster Buffer 类型正确。
- RHI D3D12/D3D11/Vulkan 类型转换测试通过。
- RenderScene 可设置 96 盏灯，并在 128 容量处正确 Clamp。
- D3D12/Vulkan 真实多帧运行和 Harness 跨 API 对比通过。

## 12. 推荐阅读顺序

1. 阅读第 2 节数据流。
2. 阅读 `ClusteredLighting::Update()` 的相机参数。
3. 阅读 `BuildLightClustersCS` 的 Tile 与 Slice 相交。
4. 阅读 Deferred Shader 的 Cluster Index 计算。
5. 阅读 RHI 的只读/读写 StructuredBuffer 映射。
6. 最后看 RDG 如何安排 Compute 到 Graphics 的依赖。
