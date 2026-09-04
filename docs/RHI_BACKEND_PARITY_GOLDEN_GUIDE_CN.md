# Stage 15：后端输入统一与 Golden Image 学习指南

本文档记录 Stage 15 已经落地的代码，不使用伪代码代替实现。本阶段的目标不是让 D3D12 与 Vulkan 立刻得到逐像素相同的画面，而是先建立可靠的统一输入、统一资源状态声明和可量化验证链路。

> 后续状态：本文末尾列出的跨 API 数值对齐工作已经在 Stage 16 完成。当前实现与最终验证结果请继续阅读 `RHI_NUMERICAL_PARITY_GUIDE_CN.md`；本文件保留 Stage 15 的演进过程和历史基线。

## 1. 本阶段完成了什么

1. D3D12 的 Shadow、GBuffer、HDR、Bloom、Tonemap 纹理进入公共 RHI 资源模型。
2. D3D12 高级 Pass 使用 RenderGraph 声明读写状态，由公共命令上下文生成 Barrier。
3. D3D12 Mesh、IndexBuffer、InstanceBuffer 和 DrawIndexed 进入公共 ICommandContext 提交路径。
4. D3D12 与 Vulkan 使用相同的窗口尺寸、Camera、太阳、点光源、StartupScene.gltf 和根节点偏移。
5. SceneLoader 增加 IGraphicsDevice 重载，Vulkan 可以加载同一份 glTF Mesh、纹理和材质。
6. 公共 Texture 支持六层 Cubemap 初始上传，D3D12 与 Vulkan 可以读取相同环境六面图。
7. Vulkan Sky Pass 通过公共 DescriptorSet 采样 Cubemap，并读取 RenderSettings 的天空、太阳和网格参数。
8. 增加 Golden Image 比较器、单元测试和双 API 自动捕获脚本。

当前 `assets/environment/default` 只有 `.gitkeep`，没有 `px/nx/py/ny/pz/nz` 图片。因此两个后端会使用 `EnvironmentMapLoader` 创建的同一组回退 Cubemap 面色。将来放入六张同分辨率图片后，两端会自动读取真实环境图。

## 2. 文件清单

### 2.1 新增文件

- `src/Tools/GoldenImageComparator.h`
- `src/Tools/GoldenImageComparator.cpp`
- `src/Tools/GoldenImageMain.cpp`
- `tests/GoldenImageTests.cpp`
- `tools/CaptureGoldenImages.ps1`
- `docs/RHI_BACKEND_PARITY_GOLDEN_GUIDE_CN.md`

### 2.2 主要修改文件

- `src/RHI/D3D12/D3D12Resources.h/.cpp`
- `src/RHI/Vulkan/VulkanResources.cpp`
- `src/RHI/VertexBuffer.h/.cpp`
- `src/RHI/IndexBuffer.h/.cpp`
- `src/Asset/Mesh.cpp`
- `src/Asset/Texture.h/.cpp`
- `src/Asset/EnvironmentMapLoader.h/.cpp`
- `src/Scene/SceneLoader.h/.cpp`
- `src/Scene/DefaultSceneFactory.h/.cpp`
- `src/Renderer/D3D12SceneRenderer.h`
- `src/Renderer/D3D12SceneRenderer.cpp`
- `src/Renderer/VulkanSceneRenderer.h/.cpp`
- `src/Renderer/RenderSettings.h`
- `assets/shaders/RhiSky.hlsl`
- `src/Core/Application.cpp`
- `src/Core/VulkanApplication.cpp`
- `CMakeLists.txt`

## 3. D3D12 外部资源为什么需要 RHI 包装

旧 D3D12 高级渲染器已经创建了可用的 `ID3D12Resource`、RTV 和 DSV。如果为了接入 RHI 再创建一份纹理，会产生重复显存、重复上传和资源不同步。

Stage 15 给 `D3D12Buffer` 和 `D3D12Texture` 增加外部资源构造函数：

```cpp
D3D12Buffer(ID3D12Resource* resource, const BufferDescription& description);
D3D12Texture(ID3D12Resource* resource, const TextureDescription& description);
```

包装对象通过 `ComPtr` 共享同一个原生资源。它不复制 GPU 数据，只给原生对象补上公共 `BufferDescription` 或 `TextureDescription`。

`D3D12TextureView` 也增加“公共 Texture + 已存在 Descriptor”的构造路径。这样 `GetTexture()` 不再返回空指针，RenderGraph 才能把 HDR、Bloom、GBuffer 和 SceneColor 注册为真实纹理。

所有权关系是：

```text
SceneRenderer
  -> ID3D12Resource
  -> D3D12Texture 公共包装，共享同一原生资源
  -> D3D12TextureView，引用公共 Texture 和已有 RTV/DSV
  -> RenderGraph，只保存当前帧的非拥有指针与状态
```

## 4. D3D12 自动 Barrier 数据流

每帧开始时，D3D12 离屏纹理都以上一帧结束时的 `ShaderResource` 状态导入：

```cpp
m_renderGraph.ImportTexture(
    "HdrColor", *m_hdrTextureRhi, RHI::ResourceState::ShaderResource);
```

每个 Pass 只声明需求：

```text
Shadow             ShadowMap: ShaderResource -> DepthWrite
GBuffer            GBuffer0..3: ShaderResource -> RenderTarget
DeferredLighting   GBuffer0..3: RenderTarget -> ShaderResource
                   HdrColor: ShaderResource -> RenderTarget
BloomExtract       HdrColor: RenderTarget -> ShaderResource
                   BloomA: ShaderResource -> RenderTarget
BloomHorizontal    BloomA: RenderTarget -> ShaderResource
                   BloomB: ShaderResource -> RenderTarget
BloomVertical      BloomB: RenderTarget -> ShaderResource
                   BloomA: ShaderResource -> RenderTarget
Tonemap            HdrColor/BloomA -> ShaderResource
                   SceneColor: ShaderResource -> RenderTarget
SceneColorReady    SceneColor: RenderTarget -> ShaderResource
```

`RenderGraph::PrepareTextureBarriers` 比较当前状态和目标状态，再调用：

```cpp
commandContext.TextureBarrier({texture, before, after});
```

D3D12 后端把它转换为 `D3D12_RESOURCE_BARRIER`，Vulkan 后端把它转换为 `VkImageMemoryBarrier`。高级渲染算法只维护一份依赖图。

Bloom 必须拆成三个节点。若三个阶段藏在一个回调里，RenderGraph 看不到 A 和 B 之间的读写切换，也就不能正确自动生成 Barrier。

## 5. D3D12 对象提交迁移方式

旧 `Mesh` 使用 `VertexBuffer` 和 `IndexBuffer` 持有 D3D12 资源。现在初始化完成后，会为同一资源创建 `D3D12Buffer` 公共包装，并填充：

```cpp
m_rhiVertexBuffer
m_rhiIndexBuffer
m_rhiIndexCount
```

普通绘制变成：

```text
Mesh::Draw(ICommandContext&)
  -> BindVertexBuffer
  -> BindIndexBuffer
  -> DrawIndexed
```

实例绘制把完整 InstanceBuffer 绑定到 Slot 1，并用 `firstInstance` 选择批次起点。这样不需要为每批次创建新的 BufferView。

### 当前兼容层

D3D12 的成熟 PBR Shader 仍使用原来的 Root Signature 参数编号和材质 SRV Table。Stage 15 保留这部分原生 Root 参数绑定，但 Pipeline、Buffer 和 Draw 已通过公共命令上下文提交。

这是增量迁移，不是最终形态。下一阶段应让 D3D12 高级材质也使用公共 `IDescriptorSet`，届时 SceneRenderer 不再直接调用 `SetGraphicsRootConstantBufferView` 和 `SetGraphicsRootDescriptorTable`。

## 6. glTF 如何跨 API 加载

旧 SceneLoader 只接收 `D3D12Context`。新增重载接收：

```cpp
bool LoadFromGltf(
    const std::string& path,
    RHI::IGraphicsDevice& device,
    RenderScene& scene,
    ...);
```

数据流如下：

```text
StartupScene.gltf
  -> GltfLoader::SceneData
  -> MeshAsset / MaterialAsset / TextureAsset
  -> IGraphicsDevice::CreateBuffer/CreateTexture
  -> Mesh / Material / Texture 运行时对象
  -> RenderObject
  -> RenderScene
```

D3D12 编辑器路径继续使用 AssetRegistry，以保留可编辑资源句柄。Vulkan 运行路径直接创建 RHI 运行时对象。两条路径消费相同的解析结果和变换矩阵。

## 7. 公共 Cubemap 上传

`TextureInitialData::slicePitch` 表示一层图像的字节数。Cubemap 使用连续的六层像素：

```text
data
  -> +X face
  -> -X face
  -> +Y face
  -> -Y face
  -> +Z face
  -> -Z face
```

D3D12 使用 `GetCopyableFootprints` 为六个 Subresource 生成 Footprint，然后逐层 `CopyTextureRegion`。

Vulkan 创建带 `VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT` 的六层 Image，ImageView 使用 `VK_IMAGE_VIEW_TYPE_CUBE`，一次 `vkCmdCopyBufferToImage` 上传六层。

当前初始上传支持一个 Mip。生产级 IBL 还需要公共 Mip 链上传、GPU Mip 生成和 Prefilter Compute Pass。

## 8. RenderSettings 如何统一

`RenderSettings` 是两端共同的默认配置源。Vulkan 不再硬编码 Exposure、BloomThreshold、BloomIntensity、Ambient、天空色和太阳半径。

`DefaultSceneFactory::ConfigureEditorPreviewWorld` 统一设置：

- Camera FOV、Near/Far、LookAt
- DirectionalLight 方向、颜色和强度
- 两个 PointLight

Vulkan SkyConstants 还包含：

- InverseViewProjection
- CameraPosition
- Zenith/Horizon/Ground Color
- Atmosphere Density
- Sun Direction/Color/Angular Radius
- Grid Minor/Major Color、Scale、Fade Distance
- 公共 Cubemap

因此天空、太阳和编辑器网格都能从相同设置生成。D3D12 仍有更完整的 IBL 和 PBR，Vulkan 当前是基础 Deferred 光照，所以画面数值还不会完全一致。

## 9. Golden Image 的两种比较

### 9.1 跨 API 趋同报告

脚本依次捕获 D3D12 和 Vulkan，再直接比较两张图。它回答：两个后端当前差得有多远？

指标包括：

- `mean_absolute_error`：RGB 平均绝对误差
- `root_mean_square_error`：对大误差更敏感
- `changed_pixel_ratio`：超过单通道容差的像素比例
- `maximum_channel_error`：最大单通道误差

默认只生成报告，不强制失败。只有传入 `-EnforceThresholds` 才把跨 API 阈值作为门禁。

### 9.2 每后端回归基线

当前 D3D12 有 PBR、IBL、CSM 和编辑器合成，Vulkan 的光照模型更简单。因此现阶段可靠的 CI 方法是：

```text
D3D12 当前帧  vs D3D12 已批准基线
Vulkan 当前帧 vs Vulkan 已批准基线
```

第一次批准基线：

```powershell
.\tools\CaptureGoldenImages.ps1 `
  -BaselineDirectory tests\golden `
  -UpdateBaselines
```

后续严格回归：

```powershell
.\tools\CaptureGoldenImages.ps1 `
  -BaselineDirectory tests\golden
```

跨 API 趋同到可接受范围后，再启用：

```powershell
.\tools\CaptureGoldenImages.ps1 -EnforceThresholds
```

不要在没有评审截图的情况下自动更新基线，否则真正的视觉回归会被当成新标准。

## 10. 如何验证

### 10.1 构建

```powershell
cmake --build build-windows-ci --config Debug --parallel 8
```

### 10.2 自动测试

```powershell
ctest --test-dir build-windows-ci -C Debug --output-on-failure
```

Stage 15 实测结果：5/5 通过。

### 10.3 自动捕获与报告

```powershell
.\tools\CaptureGoldenImages.ps1
```

输出：

- `build-windows-ci/golden/d3d12.bmp`
- `build-windows-ci/golden/vulkan.bmp`
- `build-windows-ci/golden/comparison.txt`

首次实测两张图都是 `1600x900`，两个后端都能完成捕获并自动退出。加入统一网格前的跨 API 报告为：

```text
mean_absolute_error=0.583893
root_mean_square_error=0.636638
changed_pixel_ratio=1.000000
maximum_channel_error=0.890196
```

这组数据不是通过标准，而是阶段基线。它说明输入和构图已经统一，但光照、IBL、阴影和编辑器合成仍需继续趋同。

加入共享回退 Cubemap 和 Vulkan 编辑器网格后的最终报告改善为：

```text
mean_absolute_error=0.230762
root_mean_square_error=0.259711
changed_pixel_ratio=1.000000
maximum_channel_error=0.847059
```

构图和背景已经明显趋同；像素变化比例仍为 1，主要因为 Vulkan 基础 Deferred 与 D3D12 PBR/IBL 的亮度响应不同，因此跨 API 强制阈值仍保持关闭。

## 11. 推荐学习顺序

1. 阅读 `GraphicsResources.h`，理解 TextureInitialData 和公共资源接口。
2. 阅读 `D3D12Resources.cpp` 的外部包装与多层上传。
3. 阅读 `VulkanResources.cpp` 的 Cube Image、View 和多层 Copy。
4. 阅读 `SceneRendererStage4.cpp::Render`，画出完整 RenderGraph 状态图。
5. 阅读 `Mesh.cpp`，对比原生 Draw 与 ICommandContext Draw。
6. 阅读 `SceneLoader.cpp` 两个重载，理解编辑器资源句柄和纯运行时资源的差别。
7. 阅读 `VulkanSceneRenderer.cpp::UpdateConstants` 与 `RhiSky.hlsl`。
8. 阅读 `GoldenImageComparator.cpp`，手算一个两像素样例后再读测试。
9. 运行双 API 捕获脚本并观察报告。
10. 最后回到 `LEARNING_GUIDE_CN.md`，把本阶段放入完整路线。

## 12. 下一阶段

1. 用公共 DescriptorSet 替代 D3D12 高级 Pass 的原生 Root 参数绑定。
2. 让 D3D12 和 Vulkan 共用更接近的 PBR/IBL 数学与 Shadow Cascade 设置。
3. 增加公共 Cubemap Mip 链和 GPU Prefilter。
4. 为两个后端提交评审后的 Golden 基线，并接入 CI。
5. 增加 Vulkan ImGui 编辑器后端。
6. 将 RenderGraph 从整纹理状态推进到 Mip/ArrayLayer 子资源状态。

## 13. Stage 16 完成状态

上述第 1、2、3、4 项的公共 DescriptorSet、共享 PBR/IBL/CSM、公共 IBL 资源和严格 Golden Image 门禁已经完成。最终 Tonemap 平均绝对误差为 `0.000018`，完整实现、数据流与学习顺序见 `RHI_NUMERICAL_PARITY_GUIDE_CN.md`。

Vulkan ImGui 和 Mip/ArrayLayer 子资源状态仍属于后续工作。
