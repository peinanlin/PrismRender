# PrismRender Rendering Attachment 与共享 Shadow 实现指南

本文档记录 Stage 12 已经落地的代码。它解释公共 TextureView、Rendering Scope、Vulkan Dynamic Rendering、D3D12 公共资源后端，以及 Shadow 如何先完成公共 Attachment、再逐步迁移算法提交。

## 1. 本阶段目标

Stage 11 已经统一了 Pipeline、Draw、Dispatch 和基础资源，但还不能表达：

- 一张纹理的某个 mip 或 array layer
- 当前 Pass 使用哪些 RTV/DSV
- Attachment 是 Load、Clear 还是 Discard
- Pass 完成后资源进入 ShaderResource 还是继续作为 RenderTarget
- 纯深度 Shadow 和多个颜色输出的 GBuffer

Stage 12 先补这些 RHI 能力，再迁移 Shadow。渲染算法不按 API 复制。

## 2. 完成边界

已经完成：

- 公共 `ITextureView` 和 `TextureViewDescription`
- Sampled、RenderTarget、DepthStencil、Storage 四种 View 用途
- mip/layer 范围合法性检查
- 公共 Load/Store/Clear 和 `RenderingInfo`
- `ICommandContext::BeginRendering/EndRendering`
- Vulkan 1.3 Dynamic Rendering
- Vulkan 离屏、纯深度、多颜色 Attachment 的 Pipeline 基础
- Vulkan Texture2DArray View
- D3D12 Buffer、Texture、Sampler、TextureView
- D3D12 DescriptorSetLayout、DescriptorSet
- D3D12 Graphics/Compute Pipeline 创建
- D3D12 公共 Vertex/Index/Descriptor 绑定和 TextureBarrier
- Vulkan 单层方向光 Shadow Map 与可见阴影
- D3D12 CSM Shadow Texture/View 和 Rendering Scope 迁移；常量与几何提交仍保留成熟路径
- 双后端截图自动退出与回归检查

本阶段尚未完成：

- 两后端统一 RenderScene/Camera/Material 后的完整共享 Shader 数据路径；Stage 13 已完成高级 Pass 执行层
- Vulkan CSM Texture2DArray
- RenderGraph 自动推导 ResourceState 和 Barrier
- Transient Attachment、Alias 和 Pass Culling
- D3D12 Descriptor Heap 回收与长期分配器
- Slang Reflection 自动生成 Pipeline Layout
- 通用 Vertex Semantic 描述

> 后续状态：Stage 16 已完成统一 RenderScene/Camera/Material、Vulkan CSM、
> Slang Reflection 和双 API Golden；Stage 20 已完成子资源 Barrier；
> Stage 27/28 已完成 Transient Alias、Descriptor/Resource 延迟回收与异步
> Upload。上面的清单是 Stage 12 的历史边界。

## 3. 新增文件

- `src/RHI/Rendering.h/.cpp`：公共 Attachment 和 Rendering Scope 描述、校验
- `src/RHI/D3D12/D3D12Resources.h/.cpp`：D3D12 公共资源、View 和 Descriptor
- `src/RHI/D3D12/D3D12GraphicsDevice.h/.cpp`：D3D12 `IGraphicsDevice` 后端和 Pipeline 创建
- `assets/shaders/RhiShadow.hlsl`：公共 Vulkan Shadow Depth Shader
- `docs/RHI_RENDERING_SHADOW_GUIDE_CN.md`：本学习文档

## 4. 为什么 Texture 与 TextureView 必须分开

Texture 是显存分配，TextureView 是对这块显存某部分、某用途的解释。

一张三级联 Shadow Array 可以拥有：

```text
Shadow Texture: D32Float, 2048 x 2048, 3 layers
  |
  +-> DSV layer 0
  +-> DSV layer 1
  +-> DSV layer 2
  +-> SRV layers 0..2
```

如果 `ITexture` 直接等于 DSV 或 SRV，就无法同时表达这些视图，也无法在 Bloom 中针对不同 mip 创建 UAV/SRV。

公共描述：

```cpp
struct TextureViewDescription
{
    TextureViewType type;
    Format format;
    uint32_t baseMipLevel;
    uint32_t mipLevelCount;
    uint32_t baseArrayLayer;
    uint32_t arrayLayerCount;
};
```

`ValidateTextureViewDescription` 会检查：

- mip/layer 是否越界
- Sampled View 是否具有 ShaderResource Usage
- RTV 是否来自颜色 RenderTarget Texture
- DSV 是否来自 DepthStencil Texture
- Storage View 是否具有 UnorderedAccess Usage
- View Format 是否与资源兼容

## 5. Rendering Scope

`RenderingAttachment` 描述一次 Pass 对 View 的使用：

```cpp
struct RenderingAttachment
{
    const ITextureView* view;
    LoadOperation loadOperation;
    StoreOperation storeOperation;
    ClearColorValue clearColor;
    ClearDepthStencilValue clearDepthStencil;
    ResourceState stateBefore;
    ResourceState stateAfter;
};
```

`stateBefore/stateAfter` 是当前阶段的显式状态契约。例如 Shadow：

```text
ShaderResource
  -> BeginRendering
  -> DepthWrite
  -> Draw Shadow Geometry
  -> EndRendering
  -> ShaderResource
```

显式状态使 D3D12 旧资源也能逐步迁移。后续 RenderGraph 会根据 Pass 读写声明自动生成这两个状态。

## 6. Vulkan Dynamic Rendering

旧实现固定为：

```text
BeginFrame
  -> vkCmdBeginRenderPass(Swapchain + Depth)
Renderer 只能在这个 RenderPass 内绘制
EndFrame
  -> vkCmdEndRenderPass
```

这无法在一帧内切换 Shadow、GBuffer、HDR 和 Swapchain。

新实现要求 Vulkan 1.3，并启用：

```cpp
VkPhysicalDeviceVulkan13Features features{};
features.dynamicRendering = VK_TRUE;
```

现在调用流是：

```text
BeginFrame
  -> Acquire Image
  -> Begin CommandBuffer

Shadow Pass
  -> Barrier ShaderResource -> DepthWrite
  -> vkCmdBeginRendering(depth only)
  -> DrawIndexed
  -> vkCmdEndRendering
  -> Barrier DepthWrite -> ShaderResource

Main Scene
  -> Barrier Present -> RenderTarget
  -> vkCmdBeginRendering(color + depth)
  -> Draw Sky and Geometry
  -> vkCmdEndRendering

EndFrame
  -> RenderTarget -> Present/CopySource
  -> Submit and Present
```

`VulkanGraphicsPipeline` 使用 `VkPipelineRenderingCreateInfo`：

```text
GraphicsPipelineDescription.colorFormats
  -> VkPipelineRenderingCreateInfo.pColorAttachmentFormats

GraphicsPipelineDescription.depthFormat
  -> VkPipelineRenderingCreateInfo.depthAttachmentFormat
```

所以 Pipeline 可以是：

- 0 Color + D32：Shadow
- 1 Color + D32：Forward/HDR
- 4 Color + D32：GBuffer
- 1 Color + no Depth：Bloom/Tonemap

## 7. Vulkan TextureView

`VulkanTextureView` 把公共范围转换为：

```text
baseMipLevel      -> VkImageSubresourceRange.baseMipLevel
mipLevelCount     -> levelCount
baseArrayLayer    -> baseArrayLayer
arrayLayerCount   -> layerCount
Depth Format      -> VK_IMAGE_ASPECT_DEPTH_BIT
Color Format      -> VK_IMAGE_ASPECT_COLOR_BIT
```

Swapchain Image 不是普通 `ITexture` 分配，因此 VulkanContext 用 External View 包装已有的 `VkImage/VkImageView`。View 不拥有 Swapchain Image，只提供公共 Rendering 接口。

## 8. D3D12 公共资源后端

`D3D12GraphicsDevice` 实现 `IGraphicsDevice`：

```text
CreateBuffer              -> ID3D12Resource Buffer
CreateTexture             -> ID3D12Resource Texture2D/Array
CreateTextureView         -> RTV/DSV/SRV/UAV Descriptor
CreateSampler             -> Sampler Description
CreateDescriptorSetLayout -> Binding 到 Heap Offset
CreateDescriptorSet       -> Shader-visible Descriptor Range
CreateGraphicsPipeline    -> RootSignature + Graphics PSO
CreateComputePipeline     -> RootSignature + Compute PSO
```

GPU-only Buffer/Texture 初始数据通过 `D3D12Context::ExecuteImmediate` 上传：

```text
Create Upload Resource
  -> Map and Copy CPU Data
  -> CopyBufferRegion/CopyTextureRegion
  -> ResourceBarrier to final state
  -> Execute and Wait Fence
```

CPU-to-GPU Buffer 保持映射，可用于 ConstantBuffer 每帧更新。

### 8.1 Depth + Sampled 格式

D3D12 中一张 Shadow Texture 同时作为 DSV 和 SRV 时，底层资源使用：

```text
Resource: DXGI_FORMAT_R32_TYPELESS
DSV:      DXGI_FORMAT_D32_FLOAT
SRV:      DXGI_FORMAT_R32_FLOAT
```

公共层仍把资源意图表示为 `Format::D32Float + DepthStencil + ShaderResource`。格式别名细节留在 D3D12 后端。

### 8.2 DescriptorSet 到 RootSignature

当前 Binding 约定：

```text
0..15   -> b0..b15  CBV
16..31  -> t0..t15  SRV
32..47  -> u0..u15  UAV
48..63  -> s0..s15  Sampler
```

`D3D12DescriptorSetLayout` 为资源 Heap 与 Sampler Heap 分别计算连续 Offset。创建 Pipeline 时，这些 Binding 被翻译为 Descriptor Range 和 Root Descriptor Table。

绑定时：

```text
BindDescriptorSet
  -> SetDescriptorHeaps(CBV_SRV_UAV, SAMPLER)
  -> SetGraphicsRootDescriptorTable
     或 SetComputeRootDescriptorTable
```

当前 Heap 使用线性分配，适合验证架构；生产级实现还需要 Fence 延迟回收和 Descriptor Arena。

## 9. 共享 Shadow Pass

`RhiShadowPass` 只表达算法，目前由 Vulkan Shadow 路径完整使用：

```text
BeginRendering(depth attachment)
  -> Bind Pipeline
  -> for each draw:
       Bind DescriptorSet
       Bind VertexBuffer
       Bind IndexBuffer
       DrawIndexed
  -> EndRendering
```

它不包含 `VkImageView`、`D3D12_CPU_DESCRIPTOR_HANDLE` 或原生 Barrier。D3D12 已先复用同一套 `RenderingInfo`，但其三级联常量、Instancing 和 Draw 尚未改成 `RhiShadowPass::Execute`；这是下一次增量迁移，而不是复制一份 Pass。

### 9.1 Vulkan Shadow

Vulkan 当前实现单层方向光 Shadow：

- 1536 x 1536 D32Float
- `RhiShadow.hlsl` 只有 Vertex Shader
- Depth-only Graphics Pipeline，无 Pixel Shader
- Light ViewProjection ConstantBuffer
- 每个对象使用自己的 Object ConstantBuffer
- 主场景通过 Comparison Sampler 执行硬件 PCF

主 Shader 计算：

```text
World Position
  -> Light Clip Position
  -> NDC
  -> Shadow UV
  -> SampleCmpLevelZero(depth - bias)
  -> Direct Lighting * Shadow
```

### 9.2 D3D12 Shadow

D3D12 已有三级 CSM 算法和 Shader，本阶段不重写它。改造点是：

- Shadow Texture 改由 `D3D12GraphicsDevice::CreateTexture` 创建
- 每个 Cascade DSV 改由 `CreateTextureView` 创建
- 每层通过公共 `BeginRendering/EndRendering` 完成状态、清理和绑定
- 现有 Root CBV、Instancing 与 Draw 逻辑保持工作

这是增量迁移：资源和 Attachment 已公共化；Stage 13 又把 Shadow PSO/RootSignature 绑定接入公共 Pipeline Adapter。动态常量 Offset 与 Instancing 几何仍需后续迁到 `RhiShadowPass`。

## 10. 修改文件

- `src/RHI/GraphicsTypes.*`：增加 BGRA Swapchain Format
- `src/RHI/GraphicsResources.*`：增加 TextureView 与描述符 View 写入
- `src/RHI/IGraphicsDevice.h`：增加 `CreateTextureView`
- `src/RHI/ICommandContext.h`：增加 Rendering Scope
- `src/RHI/Vulkan/VulkanContext.*`：Dynamic Rendering、Attachment Barrier、Swapchain View
- `src/RHI/Vulkan/VulkanPipeline.cpp`：Dynamic Rendering Pipeline Compatibility
- `src/RHI/Vulkan/VulkanResources.*`：Texture2DArray/View 和 Descriptor View
- `src/RHI/D3D12/D3D12Context.*`：Immediate Upload、Sampler Heap、Descriptor Capacity
- `src/RHI/D3D12/D3D12CommandContextAdapter.*`：公共绑定、Barrier 和 Rendering Scope
- `src/RHI/D3D12/D3D12PipelineView.*`：公共 Pipeline Binding 基类
- `src/Renderer/RhiPasses.*`：共享 Shadow Pass
- `src/Renderer/VulkanSceneRenderer.*`：Shadow RenderGraph 和主场景采样
- `src/Renderer/D3D12SceneRenderer.cpp`：D3D12 Shadow Resource/Scope 迁移
- `assets/shaders/RhiScene.hlsl`：Shadow Comparison Sample
- `tests/RhiTypeTranslationTests.cpp`：View/Attachment 合法性
- `tests/RenderGraphTests.cpp`：共享 Shadow 命令顺序
- `tests/ShaderCompilerTests.cpp`：新增 Shadow Shader 双目标编译

## 11. 验证方法

```powershell
cmake --build build-windows-ci --config Debug
ctest --test-dir build-windows-ci -C Debug --output-on-failure
```

Stage 12 完成时 Shader 测试覆盖 21 个入口、42 个程序；Stage 13 已扩展为 29 个入口、58 个 DXIL/SPIR-V 程序。

窗口回归：

```text
build-windows-ci/PrismRenderVulkanShadowStage12.bmp
build-windows-ci/PrismRenderD3D12RhiShadowStage12.bmp
```

Vulkan 截图需要检查：

- 物体在地面上的阴影是否可见
- 阴影方向是否与太阳/方向光一致
- 自阴影是否没有大面积 Acne
- 天空、纹理和深度遮挡是否保持正常

D3D12 截图需要检查原有 CSM、Deferred、HDR、Bloom、Tonemap 和场景构图没有回退。

## 12. Stage 13 实施结果

1. 把公共 Rendering Scope 接入 D3D12 HDR/GBuffer/Bloom Target：已完成
2. 定义共享 GBuffer Pass 和四 Attachment 输出：已完成
3. Vulkan 创建 GBuffer Texture/View 并运行 Geometry Pass：已完成
4. 迁移 Deferred Lighting Fullscreen Pass：已完成 Vulkan 基础版
5. 迁移 HDR SceneColor 和 Tonemap：已完成 Vulkan 基础版
6. 迁移 Bloom Bright/Blur：已完成双缓冲基础版；mip chain 待后续
7. Vulkan Shadow 扩展为 CSM Texture2DArray
8. RenderGraph 根据读写用途自动生成 stateBefore/stateAfter
9. 把 D3D12 Shadow 常量、几何与 Descriptor 绑定迁入 `RhiShadowPass`
10. Slang Reflection 自动生成 DescriptorSetLayout/RootSignature
11. 对两后端执行完整 Golden Image 差异测试

Stage 13 逐文件实现见 `docs/RHI_DEFERRED_POST_PROCESS_GUIDE_CN.md`。新增 Metal 时可以复用公共 Rendering Scope 与 Pass 控制流，只需实现 Metal Resource、TextureView、Pipeline 和 CommandContext 翻译；完整材质 Shader 共享仍要先统一两后端的 Scene 数据。

## 13. 推荐阅读顺序

1. `src/RHI/GraphicsResources.h`
2. `src/RHI/Rendering.h/.cpp`
3. `src/RHI/ICommandContext.h`
4. `src/RHI/Vulkan/VulkanResources.cpp` 的 `VulkanTextureView`
5. `src/RHI/Vulkan/VulkanContext::BeginRendering/EndRendering`
6. `src/RHI/Vulkan/VulkanPipeline.cpp`
7. `src/RHI/D3D12/D3D12Resources.cpp`
8. `src/RHI/D3D12/D3D12GraphicsDevice.cpp`
9. `src/RHI/D3D12/D3D12CommandContextAdapter.cpp`
10. `src/Renderer/RhiPasses.cpp`
11. `src/Renderer/VulkanSceneRenderer::RenderShadowPass`
12. `assets/shaders/RhiShadow.hlsl`
13. `assets/shaders/RhiScene.hlsl`
14. `src/Renderer/D3D12SceneRenderer.cpp` 的 D3D12 Shadow 路径
15. 三组 RHI/RenderGraph/Shader 测试和两张 Stage 12 截图
