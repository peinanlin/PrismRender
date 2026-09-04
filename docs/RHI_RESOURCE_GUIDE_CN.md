# PrismRender 公共 RHI 资源与 Vulkan 场景学习指南

本文档记录 Stage 10 已经落地的代码。重点不是罗列 Vulkan API，而是解释一份 `Mesh`、`Texture` 和 `Material` 数据怎样经过公共 RHI 接口，最终成为 Vulkan Buffer、Image、DescriptorSet 和 DrawIndexed 命令。

## 1. 本阶段目标与边界

本阶段完成：

- 公共 `IGraphicsResource`、`IBuffer`、`ITexture`、`ISampler`
- 公共 `IDescriptorSetLayout`、`IDescriptorSet`
- 最小 `IGraphicsDevice` 和 `ICommandContext`
- Vulkan Buffer、Texture2D、Sampler、DescriptorPool、DescriptorSetLayout、DescriptorSet
- GPU-only Buffer 的 Staging 上传
- Texture2D 的 Staging 上传与 Image Layout 转换
- Swapchain Framebuffer 的深度附件
- `Mesh`、`Texture`、`Material` 的公共 RHI 创建路径
- 接收 `ICommandContext` 的 RenderGraph Pass
- Vulkan 多物体、纹理、材质、光照和深度场景
- Vulkan 窗口截图回归

本阶段没有宣称完成：

- D3D12 完整场景内部全部改用公共资源接口
- Slang Reflection 自动生成 DescriptorSetLayout
- Vulkan ImGui、完整 glTF/环境资源统一和生产级资源管理；基础 RenderScene 已在 Stage 14 统一
- 自动 Barrier 已完成整纹理基础版；Transient Resource 和 Pass Culling 待实现
- D3D12/Vulkan 完整场景逐像素 Golden Image 对比

因此当前 Vulkan 是“可运行的纹理 Forward 场景后端”，而 D3D12 仍是高级渲染功能最完整的后端。

> 后续状态：以上是 Stage 10 当时的边界。Stage 16 已完成共享 Slang
> Shader、PBR/IBL/CSM 与逐 Pass 双 API 数值对齐；Stage 20 已完成强类型
> Handle、资源版本、Texture 子资源和 Buffer Range；Stage 27/28 已完成
> Native Transient Alias、Upload Ring、Upload Ticket 与延迟回收。阅读本章
> 时应把它视为演进起点，而不是当前能力清单。

## 2. 文件清单

### 2.1 新增文件

- `src/RHI/GraphicsResources.h/.cpp`：公共资源描述、接口和合法性检查
- `src/RHI/IGraphicsDevice.h`：资源创建入口
- `src/RHI/ICommandContext.h`：资源绑定和索引绘制入口
- `src/RHI/Vulkan/VulkanResources.h/.cpp`：Vulkan 资源与描述符实现
- `src/Renderer/VulkanSceneRenderer.h/.cpp`：公共资源驱动的 Vulkan 场景
- `assets/shaders/RhiScene.hlsl`：同一绑定约定下的 DXIL/SPIR-V 场景 Shader
- `tests/RenderGraphTests.cpp`：Context Pass、依赖和统计测试

### 2.2 修改文件

- `src/RHI/Vulkan/VulkanContext.*`：实现 Device/CommandContext，加入深度和 Immediate Submit
- `src/Asset/Mesh.*`：增加公共 Buffer 创建和绘制路径
- `src/Asset/Texture.*`：增加公共 Texture 创建路径
- `src/Asset/Material.*`：增加 API 无关的已解析纹理路径
- `src/Renderer/RenderGraph.*`：增加接收 `ICommandContext` 的 Pass
- `src/Core/VulkanApplication.*`：由三角形 Renderer 切换到场景 Renderer
- `tests/RhiTypeTranslationTests.cpp`：公共资源描述合法性测试
- `tests/ShaderCompilerTests.cpp`：加入 `RhiScene` 双目标编译
- `CMakeLists.txt`：注册源文件和 RenderGraph 测试目标

## 3. 三层职责

```text
Asset 层
  Mesh / Texture / Material
        |
        | 只表达资源和绘制意图
        v
公共 RHI 层
  IGraphicsDevice / ICommandContext / IBuffer / ITexture / IDescriptorSet
        |
        | 后端实现和原生类型转换
        v
Vulkan 层
  VkBuffer / VkImage / VkSampler / VkDescriptorSet / VkCommandBuffer
```

Asset 不应该包含 `VkBuffer`，RenderGraph Pass 不应该接收 `VkCommandBuffer`。需要 Vulkan 特有 Pipeline 操作时，由 `VulkanSceneRenderer` 留在 Vulkan Renderer 边界内处理。

## 4. 公共资源接口

`IGraphicsResource` 提供最小公共基类：

```cpp
class IGraphicsResource
{
public:
    virtual ~IGraphicsResource() = default;
    virtual GraphicsApi GetGraphicsApi() const = 0;
};
```

资源接口只公开上层真正需要的能力：

```text
IBuffer       -> GetDescription, Update
ITexture      -> GetDescription
ISampler      -> GetDescription
IDescriptorSetLayout -> GetDescription
IDescriptorSet -> WriteBuffer, WriteTexture, WriteSampler
```

公共接口不返回 Vulkan Handle。Vulkan Renderer 需要创建 PipelineLayout 时，可以在后端边界内向下转换为 `VulkanDescriptorSetLayout`；普通 Asset 和 RenderGraph 不允许这样做。

### 4.1 为什么资源使用 shared_ptr

DescriptorSet 必须保证被写入的 Buffer、Texture 和 Sampler 在 GPU 使用期间仍然存在。当前实现让 `VulkanDescriptorSet` 保存这些资源的 `shared_ptr`，避免上层临时变量销毁后产生悬空 Descriptor。

这不是最终的 GPU 延迟释放方案。生产版本还需要按 Fence Value 回收原生对象，但共享所有权先解决 CPU 侧对象依赖关系。

### 4.2 公共描述合法性

`ValidateBufferDescription` 会拒绝：

- 大小为零
- 没有 Usage
- Vertex/Index Buffer 没有 Stride
- CPU 只读内存配合初始上传数据

`ValidateDescriptorSetLayoutDescription` 会拒绝：

- 空 Shader Stage
- Descriptor Count 为零
- 同一 Layout 内 Binding 重复

验证发生在公共层，避免不同后端对同一个非法描述作出不同解释。

## 5. IGraphicsDevice 与 ICommandContext

`IGraphicsDevice` 负责创建长期资源：

```text
CreateBuffer
CreateTexture
CreateSampler
CreateDescriptorSetLayout
CreateDescriptorSet
```

`ICommandContext` 负责记录一帧内的绘制命令：

```text
BindVertexBuffer
BindIndexBuffer
BindDescriptorSet
DrawIndexed
```

把二者分开有两个原因：Device 创建资源不依赖某一帧；CommandContext 受当前 CommandBuffer 和 RenderPass 生命周期约束。后续增加 CopyContext 或 ComputeContext 时，也不必让所有资源对象知道队列细节。

## 6. Vulkan Buffer 实现

### 6.1 CPU 可见 Buffer

常量缓冲使用：

```text
MemoryAccess::CpuToGpu
  -> HOST_VISIBLE | HOST_COHERENT
  -> vkMapMemory 一次
  -> 每帧 IBuffer::Update memcpy
```

当前使用 Coherent Memory，因此 `Update` 后不需要手工 `vkFlushMappedMemoryRanges`。如果以后允许非 Coherent Memory，必须按 `nonCoherentAtomSize` 对齐 Flush 范围。

### 6.2 GPU-only Buffer

Mesh 顶点和索引使用：

```text
创建 DEVICE_LOCAL 目标 Buffer
  -> 创建 HOST_VISIBLE Staging Buffer
  -> memcpy 顶点或索引
  -> ExecuteImmediate(vkCmdCopyBuffer)
  -> 等待 Queue 完成
  -> 销毁 Staging Buffer
```

`ExecuteImmediate` 适合初始化阶段，代码简单但每次上传都会等待队列。后续应替换为持久 Upload Ring 和批量异步提交。

## 7. Vulkan Texture 实现

当前公共 Texture 路径支持单层、单 Mip 的 Texture2D。RGBA8 上传流程：

```text
创建 VkImage，初始布局 UNDEFINED
  -> 分配并绑定 DEVICE_LOCAL Memory
  -> 创建 Staging Buffer 并复制像素
  -> Barrier: UNDEFINED -> TRANSFER_DST_OPTIMAL
  -> vkCmdCopyBufferToImage
  -> Barrier: TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL
  -> 创建 VkImageView
```

深度纹理由 `VulkanContext` 使用同一公共 Texture 类创建，但没有初始数据，最终布局由 RenderPass 管理。当前格式为 `D32Float`，Framebuffer 同时绑定 Swapchain Color View 和 Depth View。

## 8. DescriptorSet 实现

### 8.1 Binding 约定

当前 Set 0 使用：

| Binding | 类型 | 内容 |
| --- | --- | --- |
| 0 | Uniform Buffer | FrameConstants |
| 1 | Uniform Buffer | ObjectConstants |
| 2 | Uniform Buffer | MaterialConstants |
| 16 | Sampled Image | Albedo Texture |
| 48 | Sampler | Linear Wrap Sampler |

D3D 的 `b0`、`t0`、`s0` 属于不同寄存器空间；Vulkan 同一 Set 只有一个 Binding 数字空间，所以 Texture 和 Sampler 分别从 16、48 开始。

### 8.2 分配与写入

```text
DescriptorSetLayoutDescription
  -> VkDescriptorSetLayoutBinding[]
  -> vkCreateDescriptorSetLayout
  -> VulkanDescriptorAllocator::Allocate
  -> vkAllocateDescriptorSets
  -> WriteBuffer/Texture/Sampler
  -> vkUpdateDescriptorSets
```

`VulkanDescriptorAllocator` 当前拥有一个固定容量 Pool，并允许单独释放 Set。资源销毁顺序必须是：Renderer 先释放 DescriptorSet，Allocator 再销毁 Pool，最后销毁 VkDevice。

## 9. Asset 迁移方法

为保持 D3D12 完整场景不回退，本阶段没有删除原有重载，而是增量增加公共路径。

### 9.1 Mesh

```text
MeshAsset CPU 顶点/索引
  -> Mesh::CreateFromAsset(IGraphicsDevice&)
  -> IGraphicsDevice::CreateBuffer(Vertex)
  -> IGraphicsDevice::CreateBuffer(Index)
  -> Mesh::Draw(ICommandContext&)
  -> BindVertexBuffer + BindIndexBuffer + DrawIndexed
```

这样 Vulkan Renderer 不需要读取 `MeshAsset` 的内部数组，也不需要知道 `VkBuffer`。

### 9.2 Texture

`Texture::InitializeRgba8` 保存 CPU 像素元数据，并创建公共 `ITexture`。`InitializeSolidColor` 通过 1x1 RGBA8 走同一入口，材质缺省纹理不需要后端特例。

### 9.3 Material

公共 `Material::Initialize` 保存参数和五张已经解析的 Texture。DescriptorSet 属于 Renderer，因为 Binding Layout 由 Pipeline 决定；Material 只提供资源，不直接拥有 Vulkan DescriptorSet。

## 10. RenderGraph Pass 迁移

旧接口仍保留，确保 D3D12 Pass 不被一次性重写：

```cpp
AddPass(name, reads, writes, [] { /* legacy */ });
```

新增接口为：

```cpp
AddContextPass(name, reads, writes,
    [](RHI::ICommandContext& context) { /* backend-neutral commands */ });
```

执行时使用：

```cpp
renderGraph.Execute(commandContext);
```

RenderGraph 仍会验证读资源在当前 Pass 前已经被 Import 或写入，并记录每个 Pass 的 CPU 时间。`RenderGraphTests` 使用 Mock CommandContext 验证上下文传递、执行顺序、统计和非法读取。

Stage 10 完成时，Vulkan 场景的完整 Pass 图为：

```text
Import BackBuffer
Import DepthBuffer
Import FrameConstants
Import MaterialResources
        |
        v
VulkanOpaqueGeometry
  reads:  FrameConstants, MaterialResources
  writes: BackBuffer, DepthBuffer
```

这里的“完整迁移”是指 Stage 10 Vulkan Forward 场景的所有绘制都由 Context Pass 执行，不再绕过 RenderGraph。Stage 12 随后增加 Shadow，Stage 13 又把这张图扩展为 GBuffer、Deferred、HDR、Bloom 和 Tonemap；最新流程见 `docs/RHI_DEFERRED_POST_PROCESS_GUIDE_CN.md`。

## 11. Vulkan 场景一帧数据流

初始化：

```text
Create DescriptorSetLayout/Sampler
  -> Create double-buffered FrameConstants
  -> Create cube Mesh GPU buffers
  -> Create Texture/Material resources
  -> Create per-object ObjectConstants and DescriptorSets
  -> Compile RhiScene.hlsl to SPIR-V
  -> Create PipelineLayout and GraphicsPipeline
```

每帧：

```text
Update FrameConstants
  -> Update each ObjectConstants
  -> Build RenderGraph
  -> Begin RenderPass with color/depth clear
  -> Bind Pipeline
  -> for each object:
       Bind DescriptorSet
       Bind Mesh buffers
       DrawIndexed
  -> EndFrame and Present/Capture
```

每帧常量和每物体常量都按 FrameCount 双缓冲，避免 CPU 改写 GPU 仍在读取的内存。Material 常量和纹理在场景初始化后不变，因此可以跨帧共享。

## 12. 矩阵约定与一次真实故障

项目统一约定：

```text
DirectXMath 使用行向量语义
CPU 上传前 XMMatrixTranspose
Slang Session 使用 Column Major
Shader 使用 mul(vector, matrix)
Vulkan 使用负高度 Viewport 处理 Y 方向
```

初版 `RhiScene.hlsl` 误写成 `mul(matrix, vector)`。代码和 Shader 编译都通过，Descriptor 也正常，但平移分量进入齐次坐标 `w`，截图出现从一个点向外发散的长三角。修复方式不是给 Vulkan 投影矩阵加补丁，而是把 Shader 恢复为项目统一的：

```hlsl
float4 worldPosition = mul(float4(input.position, 1.0f), worldMatrix);
output.position = mul(worldPosition, viewProjectionMatrix);
```

这个问题说明 Shader 编译测试不能替代真实 GPU 截图。跨 API 迁移时必须同时检查矩阵存储布局、乘法方向、NDC 深度范围和 Viewport Y 方向。

## 13. 验证方法与结果

构建与测试：

```powershell
cmake --build build-windows-ci --config Debug
ctest --test-dir build-windows-ci -C Debug --output-on-failure
```

当前四个测试：

- `RhiTypeTranslation`：公共 Buffer/Layout 合法性和三套 API 类型映射
- `ShaderCompiler`：当前 29 个入口、58 个 DXIL/SPIR-V 程序和 Reflection
- `VulkanRuntime`：Loader、Instance 和 PhysicalDevice
- `RenderGraph`：Context Pass、依赖验证和统计

窗口截图：

```powershell
$env:PRISM_RENDER_CAPTURE_PATH = 'D:\unity_project\PrismRender\build-windows-ci\PrismRenderVulkanStage10.bmp'
$env:PRISM_RENDER_EXIT_AFTER_CAPTURE = '1'
.\build-windows-ci\Debug\PrismRender.exe --api=vulkan
```

截图需要人工检查：物体轮廓、深度遮挡、棋盘纹理、材质颜色、光照方向和背景是否正常。只检查程序退出码无法发现矩阵方向错误。

## 14. 推荐学习顺序

1. `GraphicsResources.h`：先理解公共资源契约
2. `IGraphicsDevice.h`：资源从哪里创建
3. `ICommandContext.h`：一帧怎样提交绘制意图
4. `VulkanResources.cpp` 的 `VulkanBuffer`：内存类型和 Staging
5. `VulkanResources.cpp` 的 `VulkanTexture`：Image Layout 和复制
6. `VulkanDescriptorSetLayout` 与 `VulkanDescriptorSet`：绑定布局和实例
7. `Mesh.cpp`：Asset 如何不依赖 Vulkan 完成 DrawIndexed
8. `Texture.cpp` 和 `Material.cpp`：资源与 Pipeline Binding 的边界
9. `RenderGraph.cpp`：Context Pass 如何兼容旧 Pass
10. `VulkanSceneRenderer::CreateSceneResources`：资源组装
11. `VulkanSceneRenderer::Render`：Pass 声明
12. `VulkanSceneRenderer::RenderOpaquePass`：实际命令记录
13. `RhiScene.hlsl`：Binding 和矩阵约定
14. 四个测试目标与 Stage 10 截图：理解自动测试覆盖边界

## 15. 后续实施顺序

1. 为公共 RHI 增加 Pipeline 和共享 Pass 执行接口：Stage 11 已完成基础版
2. 增加 TextureView 与 Rendering Scope：Stage 12 已完成；SwapChain 继续由平台 Application/后端 Context 持有，不作为公共 Pass 资源暴露
3. 让 D3D12 完整场景逐步实现同一 `IGraphicsDevice/ICommandContext`：Stage 12 已完成资源后端并迁移 Shadow Attachment
4. 由 Slang Reflection 生成并校验 PipelineLayoutDescription
5. 增加 Upload Ring、批量异步上传和 Fence 延迟释放：Stage 28 已完成
6. 迁移 Vulkan Shadow Pass：Stage 12 完成基础版，Stage 16 完成统一 CSM
7. 迁移 Vulkan GBuffer、Deferred Lighting：Stage 13 已完成基础版
8. 迁移 HDR、Bloom、Tonemap：Stage 13 已完成基础版；整纹理自动 Barrier 已在 Stage 14 完成
9. Vulkan 独立运行路径保持 Headless/Runtime 定位；Windows Editor 当前使用 D3D12 ImGui 后端
10. 让 D3D12/Vulkan 使用同一 RenderScene、Camera、RenderSettings 和 Golden Image：Stage 16 完成

Stage 11 已完成部分见 `docs/RHI_PIPELINE_PASS_GUIDE_CN.md`；Stage 12 的 TextureView、Rendering Scope 和 Shadow 见 `docs/RHI_RENDERING_SHADOW_GUIDE_CN.md`；Stage 13 的 Deferred 与后处理见 `docs/RHI_DEFERRED_POST_PROCESS_GUIDE_CN.md`；Stage 14 的共享场景、动态 Offset 与自动 Barrier 见 `docs/RHI_SCENE_DYNAMIC_BARRIER_GUIDE_CN.md`。
