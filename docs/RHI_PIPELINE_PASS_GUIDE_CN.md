# PrismRender RHI Pipeline 与共享 Pass 学习指南

本文档记录 Stage 11 的实际实现。目标不是立即把 D3D12 的全部高级效果复制到 Vulkan，而是先补齐渲染 Pass 共用的执行层，让后续 Shadow、Deferred、HDR、Bloom 和 Tonemap 只保留一份渲染算法。

## 1. 本阶段完成了什么

本阶段新增了四组能力：

1. API 中立的 Graphics/Compute Pipeline 描述与对象接口
2. API 中立的 Pipeline 绑定、Draw、Dispatch 和 TextureBarrier 命令
3. Vulkan 对上述接口的真实后端实现
4. 可同时被 Vulkan 与 D3D12 调用的 Geometry/Fullscreen Pass

Vulkan 场景不再在 `VulkanSceneRenderer` 内直接创建或绑定 `VkPipeline`。天空、太阳、纹理地面和 Mesh 已通过公共 RHI Pipeline 与共享 Pass 绘制。

D3D12 高级场景仍使用原有资源和 Root Binding，但 Deferred Lighting、Sky、Bloom、Tonemap 的全屏三角形提交已接入同一个 `FullscreenTrianglePass`。这是增量迁移，不会为了跨 API 而重写已经工作的 D3D12 后端。

## 2. 设计结论：哪些代码只写一次

最终结构分为三层：

```text
Renderer / RenderGraph
  ShadowPass, DeferredPass, BloomPass, TonemapPass
  只描述渲染算法、输入输出和 Draw/Dispatch
                    |
                    v
公共 RHI
  PipelineDescription, Resource, Descriptor, CommandContext
  表达 API 中立的 GPU 意图
                    |
          +---------+---------+
          v                   v
     D3D12 Backend        Vulkan Backend
     PSO/RootSignature    VkPipeline/VkPipelineLayout
     ResourceBarrier      VkImageMemoryBarrier
     DrawInstanced        vkCmdDraw
```

因此：

- Shadow、Deferred、Bloom 等算法应该只实现一次。
- D3D12、Vulkan、未来 Metal 各自实现一次 RHI 翻译。
- Shader 源码由 Slang 保持一份，分别生成 DXIL、SPIR-V，未来可生成 Metal 需要的目标代码。
- 只有无法抽象或需要厂商扩展的功能，才保留在特定后端中。

## 3. 新增文件

### 3.1 公共 RHI

- `src/RHI/PipelineState.h`
- `src/RHI/PipelineState.cpp`

定义：

- `GraphicsPipelineDescription`
- `ComputePipelineDescription`
- `IGraphicsPipeline`
- `IComputePipeline`
- `PrimitiveTopology`
- Vertex Binding 与 Attribute 描述
- `TextureBarrier`
- Pipeline 合法性校验

### 3.2 Vulkan 后端

- `src/RHI/Vulkan/VulkanPipeline.h`
- `src/RHI/Vulkan/VulkanPipeline.cpp`

负责把公共 Pipeline 描述转换为：

- `VkShaderModule`
- `VkPipelineLayout`
- `VkVertexInputBindingDescription`
- `VkVertexInputAttributeDescription`
- Raster/Depth/Blend/Multisample 状态
- `VkPipeline`

### 3.3 D3D12 增量适配

- `src/RHI/D3D12/D3D12PipelineView.h`
- `src/RHI/D3D12/D3D12PipelineView.cpp`
- `src/RHI/D3D12/D3D12CommandContextAdapter.h`
- `src/RHI/D3D12/D3D12CommandContextAdapter.cpp`

`D3D12PipelineView` 用 RAII `ComPtr` 包装现有 PSO 和 RootSignature。它是旧 D3D12 对象进入公共命令接口的桥梁，不重复创建 PSO。

`D3D12CommandContextAdapter` 把公共命令转发为：

```text
BindGraphicsPipeline -> SetGraphicsRootSignature + SetPipelineState + IASetPrimitiveTopology
BindComputePipeline  -> SetComputeRootSignature + SetPipelineState
Draw                 -> DrawInstanced
DrawIndexed          -> DrawIndexedInstanced
Dispatch             -> Dispatch
```

### 3.4 共享 Renderer Pass

- `src/Renderer/RhiPasses.h`
- `src/Renderer/RhiPasses.cpp`

包含：

- `RhiGeometryPass`：绑定 Pipeline、DescriptorSet、VertexBuffer、IndexBuffer 并提交 `DrawIndexed`
- `FullscreenTrianglePass`：提交无 VertexBuffer 的三顶点全屏绘制

### 3.5 Shader

- `assets/shaders/RhiSky.hlsl`

该 Shader 根据全屏三角形生成天空渐变和太阳。它让 Vulkan 天空验证也经过 Slang、公共 Pipeline 和共享 Pass，而不是使用后端硬编码清屏颜色。

## 4. 修改文件与职责

- `src/RHI/IGraphicsDevice.h`：增加 Graphics/Compute Pipeline 创建接口
- `src/RHI/ICommandContext.h`：增加 Pipeline 绑定、Draw、Dispatch、TextureBarrier
- `src/RHI/Vulkan/VulkanContext.*`：实现公共命令，并跟踪 Graphics/Compute Descriptor Bind Point
- `src/Asset/Mesh.*`：暴露公共 Vertex/Index Buffer 与 Index Count
- `src/Renderer/VulkanSceneRenderer.*`：移除 Renderer 内的原生 Vulkan Pipeline 生命周期
- `src/Renderer/D3D12SceneRenderer.h`：持有共享 `FullscreenTrianglePass`
- `src/Renderer/D3D12SceneRenderer.cpp`：让 D3D12 全屏绘制经过公共 Pass
- `src/Core/Application.*`：D3D12 截图完成后支持自动退出，便于双后端无人值守验证
- `tests/RhiTypeTranslationTests.cpp`：增加 Pipeline 描述校验
- `tests/RenderGraphTests.cpp`：增加共享 Pass 命令序列测试
- `tests/ShaderCompilerTests.cpp`：增加 `RhiSky` 的 DXIL/SPIR-V 编译覆盖
- `CMakeLists.txt`：注册新增源码和测试依赖

## 5. Graphics Pipeline 描述

`GraphicsPipelineDescription` 保存创建 Graphics Pipeline 所需的中立信息：

```cpp
struct GraphicsPipelineDescription
{
    ShaderBinary vertexShader;
    ShaderBinary pixelShader;
    std::shared_ptr<IDescriptorSetLayout> descriptorSetLayout;
    std::vector<VertexBufferBindingDescription> vertexBindings;
    std::vector<VertexAttributeDescription> vertexAttributes;
    PrimitiveTopology topology;
    RasterizerDescription rasterizer;
    DepthStencilDescription depthStencil;
    std::vector<BlendAttachmentDescription> blendAttachments;
    std::vector<Format> colorFormats;
    Format depthFormat;
    std::uint32_t sampleCount;
};
```

这里没有 `D3D12_GRAPHICS_PIPELINE_STATE_DESC` 或 `VkGraphicsPipelineCreateInfo`。Renderer 只表达“我要三角形、背面剔除、深度测试和这些输出格式”，后端决定如何转换。

公共校验会在调用驱动前拒绝以下错误：

- Vertex/Pixel Shader 阶段错误或二进制目标不同
- 有 Color Attachment 却没有 Pixel Shader
- 非法 Sample Count
- Color Format 与 Blend Attachment 数量不一致
- 把深度格式用作 Color Attachment
- 开启深度测试却没有深度格式
- 没有任何 Color 或 Depth Attachment
- 重复的 Vertex Binding/Location
- Attribute 超出 Vertex Stride

纯深度 Shadow Pipeline 是合法配置：清空 `colorFormats` 和 `blendAttachments`，省略 Pixel Shader，启用 Depth Test/Write，并提供 `D32Float` 等深度格式。

## 6. Vulkan Pipeline 创建流程

Vulkan 的真实创建流程如下：

```text
VulkanSceneRenderer
  -> SlangShaderCompiler 生成 SPIR-V ShaderBinary
  -> 填充 GraphicsPipelineDescription
  -> VulkanContext::CreateGraphicsPipeline
  -> VulkanGraphicsPipeline
       -> ValidateGraphicsPipelineDescription
       -> vkCreateShaderModule
       -> 公共 DescriptorSetLayout 转 VkDescriptorSetLayout
       -> vkCreatePipelineLayout
       -> 转换 Vertex/Raster/Depth/Blend 状态
       -> vkCreateGraphicsPipelines
       -> 销毁临时 ShaderModule
```

`VulkanGraphicsPipeline` 拥有 `VkPipeline` 和 `VkPipelineLayout`，析构时按 RAII 释放。DescriptorSetLayout 由共享指针在外部保持生命周期。

Compute 路径同样已经具备：

```text
ComputePipelineDescription
  -> VulkanComputePipeline
  -> vkCreateComputePipelines
  -> BindComputePipeline
  -> Dispatch(x, y, z)
```

本阶段对 Compute 做了创建代码编译和描述校验测试，尚未加入窗口内的实际 Compute 效果。

## 7. 公共命令如何落到 Vulkan

`VulkanContext` 实现 `IGraphicsDevice` 与 `ICommandContext`。关键映射为：

```text
BindGraphicsPipeline -> vkCmdBindPipeline(GRAPHICS)
BindComputePipeline  -> vkCmdBindPipeline(COMPUTE)
BindVertexBuffer     -> vkCmdBindVertexBuffers
BindIndexBuffer      -> vkCmdBindIndexBuffer
BindDescriptorSet    -> vkCmdBindDescriptorSets
DrawIndexed          -> vkCmdDrawIndexed
Draw                 -> vkCmdDraw
Dispatch             -> vkCmdDispatch
TextureBarrier       -> vkCmdPipelineBarrier
```

绑定 Graphics 或 Compute Pipeline 时会更新当前 Bind Point。后续 `BindDescriptorSet` 因此可以选择正确的 `VK_PIPELINE_BIND_POINT_GRAPHICS` 或 `VK_PIPELINE_BIND_POINT_COMPUTE`。

`TextureBarrier` 使用公共 `ResourceState`，由 `VulkanTypeConversions` 转换为：

- Pipeline Stage Mask
- Access Mask
- Image Layout

Renderer 不需要直接书写 `VkImageMemoryBarrier`。

## 8. 共享 Pass 的数据流

### 8.1 Vulkan 天空

```text
RenderGraph::SharedFullscreenSky
  -> VulkanSceneRenderer::RenderSkyPass
  -> BindGraphicsPipeline(skyPipeline)
  -> FullscreenTrianglePass::Execute
  -> ICommandContext::Draw(3)
  -> vkCmdDraw(3, 1, 0, 0)
```

`RhiSky.hlsl` 使用 Vertex ID 生成覆盖屏幕的三角形。Pixel Shader 输出天空渐变与太阳圆盘。

### 8.2 Vulkan Mesh

```text
RenderGraph::SharedOpaqueGeometry
  -> 组装 IndexedGeometryDraw
       DescriptorSet
       VertexBuffer
       IndexBuffer
       IndexCount
  -> RhiGeometryPass::Execute
  -> Bind Pipeline/Descriptor/Vertex/Index
  -> DrawIndexed
```

共享 Pass 会检查 CommandContext、Pipeline 和所有 Draw Resource 是否属于同一个 `GraphicsApi`，避免混用 D3D12 与 Vulkan 对象。

### 8.3 D3D12 全屏效果

```text
现有 D3D12 PSO + RootSignature
  -> D3D12GraphicsPipelineView
  -> D3D12CommandContextAdapter
  -> FullscreenTrianglePass::Execute
  -> DrawInstanced(3, 1, 0, 0)
```

Deferred Lighting、Sky、Bloom 和 Tonemap 已使用该路径。它证明同一个 Pass 算法可以被两个后端命令上下文调用。

## 9. 为什么 D3D12 使用 Adapter

D3D12 高级场景已经拥有大量可工作的 PSO、RootSignature、Descriptor Heap 和 Resource。一次性替换会扩大风险，也会遮蔽真正的 RHI 设计问题。

Adapter 的作用是把迁移拆成可验证的小步：

1. 先统一 Draw/Dispatch 调用
2. 再统一 Pipeline 描述和创建
3. 再迁移 Buffer/Texture/Descriptor
4. 最后让高级 Pass 完全脱离 D3D12 原生类型

Stage 11 完成时，Adapter 的限制是明确且主动失败的：

- `BindVertexBuffer`：D3D12 公共 Buffer 尚未迁移
- `BindIndexBuffer`：D3D12 公共 Buffer 尚未迁移
- `BindDescriptorSet`：D3D12 公共 DescriptorSet 尚未迁移
- `TextureBarrier`：D3D12 公共 Texture 包装尚未迁移

这些接口不会静默执行错误操作，而会通过 `Core::Check` 指出下一迁移阶段。

## 10. 当前仍未完成的 RHI 能力

本阶段完成的是 Pipeline 与命令执行基础，不等于高级 Pass 已全部跨后端。

仍需补齐：

- API 中立的 RenderTargetView/DepthStencilView
- `BeginRendering/EndRendering` 或等价 Rendering Attachment 接口
- Load/Store/Clear 操作描述
- 多 Color Attachment 和 Depth-only 的 Vulkan RenderPass/Dynamic Rendering 支持
- D3D12 公共 Buffer/Texture/Sampler/Descriptor 实现
- 自动资源状态跟踪与 Barrier 生成
- SwapChain 公共接口
- Reflection 自动合并 Pipeline Layout

以上是 Stage 11 的阶段边界。Stage 12 已完成公共 `TextureView`、`RenderingAttachment`、`BeginRendering/EndRendering`、Vulkan Dynamic Rendering，以及 D3D12 公共 Buffer/Texture/Sampler/DescriptorSet。`VulkanGraphicsPipeline` 现在通过 `VkPipelineRenderingCreateInfo` 声明 Attachment 格式，也可以创建无 Pixel Shader、无 Color Attachment 的 Depth-only Shadow Pipeline。

后续 Stage 16 已完成 Reflection Layout、共享高级 Shader 与双 API Golden；
Stage 20/27/28 已完成资源版本、子资源 Barrier、Native Transient Alias、
Upload Ring 和延迟回收。SwapChain 仍由平台 Application 与后端 Context
持有，这是当前所有权边界，不是高级 Pass 跨 API 的阻塞项。

## 11. Stage 12 实施结果与后续顺序

下列步骤中的 1 至 6 已在 Stage 12 完成，详细代码和数据流见 `docs/RHI_RENDERING_SHADOW_GUIDE_CN.md`：

1. 定义 `RenderingAttachmentDescription`、Load/Store/Clear 和 Rendering Scope
2. 定义公共 Texture View，区分 SRV、RTV、DSV、UAV 用途
3. Vulkan 使用 Dynamic Rendering 或 RenderPass Cache 支持离屏与多 Attachment
4. D3D12 实现公共 Buffer、Texture、Sampler、DescriptorSet
5. D3D12 实现公共 TextureBarrier 与 Rendering Scope
6. 先迁移纯深度 Shadow Pass，验证 Depth-only Pipeline
7. 迁移 GBuffer 与 Deferred Lighting：Stage 13 已完成 Vulkan 基础版
8. 迁移 HDR、Bloom、Tonemap：Stage 13 已完成 Vulkan 基础版
9. Vulkan 保持 Runtime/Headless 后端，Windows Editor 使用 D3D12 ImGui
10. 统一场景设置后执行 D3D12/Vulkan Golden Image：Stage 16 已完成

这个顺序的关键是：第 6 至 8 步只实现一次 Renderer Pass，第 3 至 5 步分别实现各 API 的底层翻译。

## 12. 验证方法

构建与自动测试：

```powershell
cmake --build build-windows-ci --config Debug
ctest --test-dir build-windows-ci -C Debug --output-on-failure
```

当前覆盖：

- `RhiTypeTranslation`：Pipeline 描述、Depth-only Shadow 配置与三套 API 类型转换
- `ShaderCompiler`：当前 29 个入口分别生成 DXIL/SPIR-V，共 58 个程序
- `VulkanRuntime`：Vulkan Loader、Instance 和 PhysicalDevice
- `RenderGraph`：依赖检查、共享 Geometry Pass 命令序列、Fullscreen Draw(3)

窗口回归产物：

```text
build-windows-ci/PrismRenderVulkanShadowStage12.bmp
build-windows-ci/PrismRenderD3D12RhiShadowStage12.bmp
```

Vulkan 截图应包含天空渐变、太阳、纹理地面、立方体、正确深度遮挡和方向光阴影。D3D12 截图应保持原有 Shadow、Deferred、HDR、Bloom、Tonemap 场景正常，并证明 Shadow Texture/View/Rendering Scope 已走公共 RHI。

两个后端都支持设置 `PRISM_RENDER_CAPTURE_PATH` 和 `PRISM_RENDER_EXIT_AFTER_CAPTURE=1`。截图写入后程序会等待 GPU 并正常退出，因此脚本不需要强制结束进程。

## 13. 推荐学习顺序

1. `src/RHI/PipelineState.h`：理解公共 Pipeline 输入
2. `src/RHI/PipelineState.cpp`：理解后端调用前的合法性边界
3. `src/RHI/IGraphicsDevice.h`：Pipeline 的创建入口
4. `src/RHI/ICommandContext.h`：一帧可提交的公共命令
5. `src/RHI/Vulkan/VulkanPipeline.cpp`：中立描述到 Vulkan 对象
6. `src/RHI/Vulkan/VulkanContext.cpp`：公共命令到 `vkCmd*`
7. `src/Renderer/RhiPasses.cpp`：与 API 无关的绘制算法
8. `src/Renderer/VulkanSceneRenderer.cpp`：RenderGraph 如何组装共享 Pass
9. `assets/shaders/RhiSky.hlsl`：全屏三角形与天空/太阳
10. `src/RHI/D3D12/D3D12PipelineView.cpp`：旧对象如何安全进入公共层
11. `src/RHI/D3D12/D3D12CommandContextAdapter.cpp`：增量迁移边界
12. `src/Renderer/D3D12SceneRenderer.cpp`：D3D12 高级 Pass 的第一批迁移点
13. `tests/RhiTypeTranslationTests.cpp` 与 `tests/RenderGraphTests.cpp`：接口契约如何被固定

学习时可以选一次 Vulkan Sky Draw 和一次 D3D12 Tonemap Draw，从 RenderGraph/Renderer 一直跟踪到原生 API。两条调用链在 `FullscreenTrianglePass` 汇合，又在各自 CommandContext 后端分开，这正是 RHI 的核心作用。
