# PrismRender 跨 API Deferred 与后处理实现指南

本文档记录 Stage 13 的实际实现。目标是让 Vulkan 运行完整的 GBuffer、Deferred、HDR、Bloom 和 Tonemap 链，同时把 D3D12 高级目标的 RTV/DSV、Clear 和 Barrier 迁入公共 Rendering Scope。

## 1. 本阶段完成边界

已经完成：

- 公共 `RhiGeometryRenderingPass`
- 公共 `RhiFullscreenRenderingPass`
- 一个 Rendering Scope 内提交多个全屏 Draw
- Vulkan 四目标 RGBA16F GBuffer
- Vulkan Deferred Lighting 与 Shadow Sampling
- Vulkan RGBA16F HDR Scene Color
- Vulkan Bright Extract、横向 Blur、纵向 Blur
- Vulkan ACES Tonemap 到 Swapchain
- Vulkan Resize 时重建尺寸相关 Texture、View 和 DescriptorSet
- D3D12 HDR、GBuffer、Bloom、SceneColor 和主深度接入公共 Rendering Scope
- D3D12 Shadow PSO/RootSignature 通过公共 Pipeline Adapter 绑定
- 29 个 Shader 入口的 DXIL/SPIR-V 双目标测试，共 58 个程序
- D3D12/Vulkan 独立 GPU 截图回归
- Stage 14：公共 `RenderScene`、Camera、动态 Buffer Offset 和 RenderGraph 自动 Barrier

尚未完成：

- 两后端完全统一 glTF、环境贴图、编辑器 UI 和 RenderSettings
- D3D12 CSM 几何提交完全改用 `RhiShadowPass`
- Vulkan CSM Texture2DArray
- Slang Reflection 自动生成 Pipeline Layout
- Vulkan ImGui Renderer Backend
- 同一场景下的 Golden Image 像素差异测试

> 后续状态：Stage 16 已完成 glTF/环境/RenderSettings、共享 Slang
> Shader、Vulkan CSM、Reflection Layout 与同场景逐 Pass Golden。Vulkan
> 继续定位为 Runtime/Headless 后端，因此不复制 Windows D3D12 Editor UI。

这里要区分两种“共享”：

```text
已共享：Pass 执行结构、RenderingInfo、Attachment 状态、Draw/Fullscreen 命令
Stage 14 已共享：预览 Scene 数据、Camera、动态对象常量和 Vulkan 高级链 Barrier
Stage 16 已共享：完整 glTF/环境资源、RenderSettings 与高级对象提交
```

因此 Stage 13 消除了每个 API 各自手写 RTV/DSV 和 RenderPass 控制流；
Stage 14 统一基础场景，Stage 16 最终统一高级输入和数值规则。当前可以并且
已经执行严格逐像素双 API 对比。

## 2. 一帧 RenderGraph

Vulkan 当前一帧为：

```text
SharedShadowMap
  writes ShadowMap
        |
        v
SharedGBuffer
  writes GBuffer[0..3] + Depth
        |
        v
SharedDeferredLighting
  reads GBuffer + ShadowMap
  writes HdrColor
        |
        v
SharedBloomExtract
  reads HdrColor
  writes BloomA
        |
        v
SharedBloomHorizontal
  reads BloomA
  writes BloomB
        |
        v
SharedBloomVertical
  reads BloomB
  writes BloomA
        |
        v
SharedTonemap
  reads HdrColor + BloomA
  writes BackBuffer
```

RenderGraph 当前负责顺序和读前写检查；真正的 Vulkan Layout 与 D3D12 Resource State 仍由每个 `RenderingAttachment` 的 `stateBefore/stateAfter` 显式声明。

## 3. 为什么增加两个公共 Pass

### 3.1 RhiGeometryRenderingPass

它把一次 MRT 几何渲染表达为：

```text
BeginRendering(GBuffer[4] + Depth)
  -> Bind GraphicsPipeline
  -> Bind Descriptor/Vertex/Index
  -> DrawIndexed
EndRendering
```

`RhiShadowPass` 现在复用这项能力。GBuffer 与 Shadow 的区别只在 `RenderingInfo`、Pipeline 和 Draw 列表，不在 API 控制流。

### 3.2 RhiFullscreenRenderingPass

全屏 Pass 接收 `FullscreenDraw` 数组：

```cpp
struct FullscreenDraw
{
    const RHI::IGraphicsPipeline* pipeline;
    const RHI::IDescriptorSet* descriptorSet;
};
```

Deferred HDR Scope 会连续提交：

```text
Draw 0: Sky Pipeline，不需要 DescriptorSet
Draw 1: Deferred Pipeline，绑定 GBuffer/Shadow DescriptorSet
```

Bloom 和 Tonemap 只提交一个 Draw。这样天空和 Deferred Lighting 可以写同一张 HDR Texture，不需要额外 Load/Store 往返。

## 4. GBuffer 数据布局

当前 Vulkan GBuffer 使用四张 `Rgba16Float`：

| Attachment | RGB | A |
| --- | --- | --- |
| GBuffer 0 | World Position | Roughness |
| GBuffer 1 | Encoded World Normal | Metallic |
| GBuffer 2 | Albedo | Occlusion |
| GBuffer 3 | Emissive | Alpha/Occupancy |

选择 RGBA16F 是为了先验证架构和精度，不是最终带宽方案。后续可以把 Position 改为 Depth Reconstruction，把 Normal 压缩为 2 通道，并把材质参数打包到 R8/RG8。

`RhiScene.hlsl::GBufferPS` 只写材质与几何信息，不计算最终光照。`RhiDeferred.hlsl::DeferredLightingPS` 再读取 GBuffer、方向光和 Shadow Map。

## 5. Vulkan 尺寸相关资源

`VulkanSceneRenderer::CreateSizeDependentResources` 创建：

```text
4 x GBuffer Texture + RTV View + Sampled View
1 x HDR Texture     + RTV View + Sampled View
1 x Bloom A         + RTV View + Sampled View
1 x Bloom B         + RTV View + Sampled View
```

Texture Usage 均为：

```text
RenderTarget | ShaderResource
```

Resize 的销毁顺序必须是：

```text
Pipeline
  -> DescriptorSet
  -> TextureView
  -> Texture
```

DescriptorSet 会持有 View，View 会持有 Texture。反向销毁可以避免 Vulkan 对象仍被上层对象引用。

## 6. Descriptor 布局

### 6.1 Deferred Set

```text
binding 0     FrameConstants
binding 16    GBuffer Position/Roughness
binding 17    GBuffer Normal/Metallic
binding 18    GBuffer Albedo/Occlusion
binding 19    GBuffer Emissive/Alpha
binding 20    Shadow Map
binding 48    Linear Clamp Sampler
binding 49    Comparison Shadow Sampler
```

### 6.2 PostProcess Set

```text
binding 0     PostProcessConstants
binding 16    Source Texture
binding 17    Auxiliary Texture
binding 48    Linear Clamp Sampler
```

每个 Frame-in-flight 都有自己的 ConstantBuffer 和 DescriptorSet。Texture View 可以共享，CPU 每帧只更新当前帧常量，避免覆盖 GPU 尚未消费的数据。

## 7. Attachment 状态流

Vulkan 第一帧的离屏 Texture 从 `Undefined` 开始，后续帧从 `ShaderResource` 开始：

| Pass | Target Before | Pass State | Target After |
| --- | --- | --- | --- |
| GBuffer | Undefined/ShaderResource | RenderTarget | ShaderResource |
| Deferred | Undefined/ShaderResource | RenderTarget | ShaderResource |
| Bloom Extract A | Undefined/ShaderResource | RenderTarget | ShaderResource |
| Bloom Horizontal B | Undefined/ShaderResource | RenderTarget | ShaderResource |
| Bloom Vertical A | ShaderResource | RenderTarget | ShaderResource |
| Tonemap BackBuffer | Present | RenderTarget | RenderTarget |

最后一项保持 `RenderTarget`，由 `VulkanContext::EndFrame` 转换为 Present 或 CopySource。截图开启时，EndFrame 会先复制，再恢复 Present。

## 8. HDR、Bloom 与 Tonemap

### 8.1 HDR

Deferred Lighting 输出不执行 `saturate`，允许颜色超过 1。HDR Texture 使用 `Rgba16Float`，保存太阳高光和镜面反射能量。

### 8.2 Bloom

当前基础版执行：

```text
HDR -> BrightExtract -> BloomA
BloomA -> HorizontalBlur -> BloomB
BloomB -> VerticalBlur -> BloomA
```

这是全分辨率双缓冲验证版。后续应改为降采样 mip chain，提高性能并获得更自然的大范围辉光。

### 8.3 Tonemap

Tonemap 合并 HDR 与 Bloom：

```text
color = HDR + Bloom * intensity
color = ACESFilm(color * exposure)
```

输出格式使用实际 Swapchain Format，而不是写死 RGBA/BGRA，保证 Vulkan Surface 格式变化时 Pipeline 仍匹配 Dynamic Rendering。

## 9. D3D12 如何迁移

D3D12 没有重写成熟的 PBR、IBL、CSM 和编辑器 Shader。本阶段给现有资源建立公共 `D3D12TextureView`，然后把以下原生序列：

```text
ResourceBarrier
RSSetViewports/RSSetScissorRects
OMSetRenderTargets
ClearRenderTargetView/ClearDepthStencilView
Draw
ResourceBarrier
```

替换为：

```text
D3D12CommandContextAdapter::BeginRendering(RenderingInfo)
Draw
D3D12CommandContextAdapter::EndRendering()
```

完成迁移的目标：

- HDR Render Target
- 四张 GBuffer
- Bloom A/B
- SceneColor
- 主场景 Depth
- CSM Shadow Cascade DSV

Shadow 的 PSO 和 RootSignature 也通过 `D3D12GraphicsPipelineView` 与公共 `BindGraphicsPipeline` 绑定。动态 Object CBV Offset、Cascade Root Constant 和 Instancing Vertex Stream 仍是原生提交；公共 Descriptor 模型补齐这些能力后，才适合让 D3D12 完整调用 `RhiShadowPass`。

## 10. Shader 文件

- `assets/shaders/RhiScene.hlsl`：Forward 与 GBuffer Geometry
- `assets/shaders/RhiShadow.hlsl`：纯深度 Shadow
- `assets/shaders/RhiDeferred.hlsl`：全屏 Deferred Lighting
- `assets/shaders/RhiPostProcess.hlsl`：Bright、Blur、Tonemap
- `assets/shaders/RhiSky.hlsl`：HDR 天空与太阳

Slang 对这些入口同时生成 DXIL 和 SPIR-V。当前测试覆盖 29 个入口，共 58 个程序，并检查 SPIR-V Binding 不重叠。

## 11. 新增与修改文件

新增：

- `assets/shaders/RhiDeferred.hlsl`
- `assets/shaders/RhiPostProcess.hlsl`
- `docs/RHI_DEFERRED_POST_PROCESS_GUIDE_CN.md`

主要修改：

- `src/Renderer/RhiPasses.h/.cpp`
- `src/Renderer/VulkanSceneRenderer.h/.cpp`
- `src/Renderer/D3D12SceneRenderer.h`
- `src/Renderer/D3D12SceneRenderer.cpp`
- `src/RHI/D3D12/D3D12Context.h/.cpp`
- `assets/shaders/RhiScene.hlsl`
- `tests/ShaderCompilerTests.cpp`
- `tests/RhiTypeTranslationTests.cpp`
- `tests/RenderGraphTests.cpp`

## 12. 验证方法

```powershell
cmake --build build-windows-ci --config Debug
ctest --test-dir build-windows-ci -C Debug --output-on-failure
```

窗口回归：

```text
build-windows-ci/PrismRenderVulkanDeferredStage13.bmp
build-windows-ci/PrismRenderD3D12AttachmentsStage13.bmp
```

Vulkan 需要检查：

- GBuffer 物体边界和深度遮挡正确
- Deferred 光照与 Shadow 方向一致
- 天空写入 HDR，不被空 GBuffer 像素覆盖
- Bloom 只增强高亮区域
- Tonemap 后没有黑屏、NaN 或整体过曝

D3D12 需要检查：

- CSM、PBR、IBL、HDR、Bloom、Tonemap 没有回退
- 编辑器网格、太阳和 SceneColor 正常
- 自动截图完成后进程正常退出

## 13. 推荐学习顺序

1. `src/Renderer/RhiPasses.h/.cpp`
2. `assets/shaders/RhiScene.hlsl::GBufferPS`
3. `assets/shaders/RhiDeferred.hlsl`
4. `assets/shaders/RhiPostProcess.hlsl`
5. `VulkanSceneRenderer::CreateSizeDependentResources`
6. `VulkanSceneRenderer::CreatePassDescriptorSets`
7. `VulkanSceneRenderer::CreatePipeline`
8. `VulkanSceneRenderer::Render`
9. `RenderGBufferPass` 与 `RenderDeferredPass`
10. 三个 Bloom Pass 与 `RenderTonemapPass`
11. `D3D12CommandContextAdapter::BeginRendering/EndRendering`
12. `SceneRendererStage4.cpp` 的 D3D12 高级 Pass
13. 三组自动测试与两张 Stage 13 截图

## 14. 下一阶段

1. 让 D3D12/Vulkan 消费同一个 RenderScene、Camera 和 RenderSettings
2. 增加公共动态 Buffer Offset或 Push Constants
3. 让 D3D12 CSM 完整进入 `RhiShadowPass`
4. Vulkan Shadow 扩展为 CSM Texture2DArray
5. RenderGraph 根据读写声明自动生成 Barrier
6. Slang Reflection 自动生成 DescriptorSetLayout/RootSignature
7. Bloom 改为降采样 mip chain
8. 接入 ImGui Vulkan Renderer Backend
9. 使用同一场景执行 Golden Image 差异测试

完成第 1、3 和 9 项后，才能把“两个后端都能运行高级链”进一步升级为“完整场景可以可靠地跨 API 切换”。
