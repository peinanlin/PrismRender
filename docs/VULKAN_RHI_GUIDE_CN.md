# PrismRender Vulkan RHI 实现指南

本文档记录 Stage 9 到 Stage 14 的实际代码，不用伪代码代替实现。目标是让学习者能够从启动参数开始，沿着 Vulkan 对象生命周期和一帧命令流，理解 PrismRender 如何把 Slang SPIR-V、公共场景与资源、Dynamic Rendering 和共享高级 Pass 真正提交给 GPU。

## 1. 当前完成边界

已经完成：

- `--api=vulkan` 运行时后端选择
- 不依赖 Vulkan SDK 的动态函数加载
- Vulkan Instance、Surface、PhysicalDevice 和 LogicalDevice
- Graphics/Present Queue Family 选择
- Swapchain、ImageView，以及仅供旧 Triangle 路径兼容的 RenderPass/Framebuffer
- CommandPool、CommandBuffer、Semaphore 和 Fence
- 公共 Graphics/Compute Pipeline 与 Vulkan 原生实现
- Vulkan Buffer、Texture2D、Sampler 和 Staging 上传
- Vulkan DescriptorPool、DescriptorSetLayout 和 DescriptorSet
- Swapchain Framebuffer 深度附件
- 公共 Mesh、Texture、Material 资源路径
- Context RenderGraph Pass 与索引场景绘制
- 共享 Fullscreen Sky 与 Opaque Geometry Pass
- Vulkan 1.3 Dynamic Rendering 与公共 TextureView
- 离屏纯深度 Shadow Pass 与主场景阴影采样
- 四目标 GBuffer、Deferred Lighting 和 RGBA16F HDR
- Bright Extract、双向 Blur、Bloom 合成和 ACES Tonemap
- 多物体、棋盘纹理、材质和方向光
- 天空渐变与太阳
- Resize/Out-of-date Swapchain 重建
- Swapchain 图像回读和 BMP 自动截图
- Vulkan 类型转换测试和无窗口 Runtime 测试

还没有完成：

- 公共 SwapChain 接口；TextureView 与 Rendering Scope 已完成
- Slang Reflection 自动生成 DescriptorSetLayout
- 完整 D3D12 PBR/IBL 材质模型在 Vulkan 的统一实现
- ImGui Vulkan Renderer Backend
- D3D12/Vulkan 完整场景 Golden Image 对比

因此当前应称为“Vulkan Deferred + PostProcess 场景后端”。它已经具备现代高级渲染链，并在 Stage 14 消费公共 `RenderScene` 和 Camera；但 D3D12 仍是完整编辑器和复杂 PBR/IBL 场景的默认路径，两端完全统一 glTF、环境资源和设置前不能做可靠的逐像素对比。

## 2. 文件职责

新增文件：

- `third_party/glad/include/glad/vulkan.h`：固定版本的 Vulkan 1.3 函数加载头
- `src/RHI/Vulkan/VulkanLoader.*`：通过 GLFW 获取 Vulkan 函数地址
- `src/RHI/Vulkan/VulkanTypeConversions.*`：Prism 中立枚举到 Vulkan 原生类型
- `src/RHI/Vulkan/VulkanContext.*`：设备、交换链、命令、同步和截图
- `src/RHI/GraphicsResources.*`：公共资源描述和接口
- `src/RHI/IGraphicsDevice.h`、`ICommandContext.h`：公共设备和命令边界
- `src/RHI/PipelineState.*`：公共 Graphics/Compute Pipeline 描述和校验
- `src/RHI/Vulkan/VulkanResources.*`：Buffer、Texture、Sampler 和 Descriptor
- `src/RHI/Vulkan/VulkanPipeline.*`：公共描述到 VkPipeline/VkPipelineLayout
- `src/Renderer/RhiPasses.*`：共享 Geometry 与 Fullscreen Pass
- `src/Renderer/VulkanTriangleRenderer.*`：SPIR-V Pipeline 和三角形资源
- `src/Renderer/VulkanSceneRenderer.*`：公共资源驱动的纹理 Forward 场景
- `assets/shaders/RhiScene.hlsl`：Vulkan 场景 Shader
- `assets/shaders/RhiSky.hlsl`：共享全屏天空和太阳 Shader
- `src/Core/VulkanApplication.*`：Vulkan 主循环
- `src/Core/ApplicationLauncher.*`：解析 `--api`
- `tests/VulkanRuntimeTests.cpp`：无窗口 Loader/Instance/PhysicalDevice 验证
- `tests/RenderGraphTests.cpp`：Context Pass 和依赖验证

修改文件：

- `src/main.cpp`：只保留异常边界并调用 Launcher
- `src/RHI/ShaderTypes.h`：区分逻辑入口和目标模块导出入口
- `src/Asset/SlangShaderCompiler.cpp`：记录 SPIR-V 实际导出名 `main`
- `tests/RhiTypeTranslationTests.cpp`：加入 Vulkan 映射断言
- `tests/ShaderCompilerTests.cpp`：检查 SPIR-V 导出入口
- `CMakeLists.txt`：加入 Vulkan 源码、GLAD include 和测试目标

## 3. 为什么没有强制安装 Vulkan SDK

当前机器有显卡驱动安装的系统 Vulkan Runtime：

```text
C:\Windows\System32\vulkan-1.dll
```

但没有 `VULKAN_SDK`。PrismRender 没有直接链接 `vulkan-1.lib`，而是使用 GLFW 的：

```text
glfwGetInstanceProcAddress
```

加载流程为：

```text
GLFW 初始化
  -> LoadGlobalFunctions(VK_NULL_HANDLE)
  -> vkCreateInstance
  -> LoadInstanceFunctions(instance, VK_NULL_HANDLE)
  -> 枚举 PhysicalDevice
  -> LoadInstanceFunctions(instance, physicalDevice)
  -> 创建 Device 和 Swapchain
```

这样重新构建只需要仓库里的 GLAD 头和系统显卡驱动。Vulkan SDK 仍然推荐用于 Validation Layer、`vulkaninfo`、RenderDoc 集成和离线工具，但不再是编译的硬依赖。

## 4. 启动和后端选择

`main.cpp` 调用：

```text
ApplicationLauncher::RunApplication(argc, argv)
```

Launcher 支持：

```powershell
PrismRender.exe
PrismRender.exe --api=d3d12
PrismRender.exe --api=vulkan
PrismRender.exe --api vulkan
```

默认值为 D3D12。D3D11 目前只有类型转换器，选择 `--api=d3d11` 会返回清晰错误，不会伪装成可运行后端。

## 5. VulkanContext 初始化顺序

`VulkanContext::Initialize` 按以下顺序执行：

1. 检查 `glfwVulkanSupported`
2. 加载 Global Vulkan Functions
3. 创建 `VkInstance`，声明 Vulkan 1.3
4. 加载 Instance Functions
5. 由 GLFW 创建 `VkSurfaceKHR`
6. 枚举并评分 `VkPhysicalDevice`
7. 选择 Graphics 和 Present Queue Family
8. 检查 `VK_KHR_swapchain`
9. 启用 Dynamic Rendering，创建 `VkDevice` 和两个 Queue Handle
10. 创建 DescriptorAllocator
11. 创建可单独 Reset CommandBuffer 的 CommandPool
12. 创建 Swapchain、公共 BackBuffer/Depth View 和兼容性 RenderPass
13. 创建双帧 CommandBuffer、Semaphore 和 Fence

析构顺序与依赖方向相反：

```text
DeviceWaitIdle
  -> Capture Buffer
  -> Renderer 的 DescriptorSet/Texture/Buffer
  -> TextureView/Framebuffer/Depth/兼容性 RenderPass/ImageView/Swapchain
  -> DescriptorPool
  -> Semaphore/Fence/CommandPool
  -> Device
  -> Surface
  -> Instance
```

Vulkan 不会替应用自动管理父子对象生命周期。销毁 Device 后再销毁它创建的 Buffer 属于错误，因此 Renderer 必须先于 Context 销毁。

## 6. 物理设备和 Queue Family

设备必须同时满足：

- 存在 Graphics Queue
- 存在支持当前 Surface 的 Present Queue
- 支持 `VK_KHR_swapchain`
- Surface 至少有一个 Format 和 PresentMode

离散 GPU 获得更高评分。Graphics 和 Present 可能属于不同 Queue Family；此时 Swapchain 使用 `VK_SHARING_MODE_CONCURRENT`，否则使用更轻量的 `EXCLUSIVE`。

当前机器验证选择：

```text
NVIDIA GeForce RTX 5060
```

## 7. Swapchain 选择

Surface Format 优先级：

1. `VK_FORMAT_B8G8R8A8_SRGB + VK_COLOR_SPACE_SRGB_NONLINEAR_KHR`
2. `VK_FORMAT_B8G8R8A8_UNORM`
3. 驱动返回的第一个 Format

PresentMode 优先使用 `MAILBOX`，不可用时回退到规范保证存在的 `FIFO`。图像数量取 `minImageCount + 1`，并受 `maxImageCount` 限制。

Swapchain Usage 至少包含：

```text
VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
```

Surface 支持时还加入：

```text
VK_IMAGE_USAGE_TRANSFER_SRC_BIT
```

后者用于自动截图，不支持时渲染仍可运行，但 Capture 会报告明确错误。

## 8. 一帧 Vulkan 命令流

`BeginFrame`：

```text
Wait current frame Fence
  -> AcquireNextImage(imageAvailable Semaphore)
  -> Wait image previous Fence
  -> Reset Fence
  -> Reset/Begin CommandBuffer
```

Renderer：

```text
Update double-buffered Frame/Object constants
  -> Build RenderGraph
  -> SharedShadowMap
       BeginRendering(depth only)
       RhiShadowPass -> DrawIndexed
       EndRendering -> ShaderResource
  -> SharedMainScene
       BeginRendering(backbuffer + depth)
       FullscreenTrianglePass + RhiGeometryPass
       EndRendering
```

`EndFrame`：

```text
ColorAttachment -> Present 或 TransferSrc Barrier
  -> EndCommandBuffer
  -> QueueSubmit(wait imageAvailable, signal renderFinished, signal Fence)
  -> QueuePresent(wait renderFinished)
```

Semaphore 表达 GPU 队列和呈现系统之间的依赖；Fence 用于 CPU 等待某一帧资源可以复用。两者不能互相替代。

## 9. Slang SPIR-V 如何进入 Pipeline

调用链：

```text
VulkanSceneRenderer
  -> ShaderManager::LoadShader(..., ShaderBinaryFormat::SpirV)
  -> SlangShaderCompiler
  -> ShaderBinary
  -> GraphicsPipelineDescription
  -> VulkanContext::CreateGraphicsPipeline
  -> VulkanGraphicsPipeline
       -> vkCreateShaderModule
       -> VkPipelineShaderStageCreateInfo
       -> VkDescriptorSetLayout + VkPipelineLayout
       -> vkCreateGraphicsPipelines
```

当前 Slang Target Profile 为 `spirv_1_5`；Stage 12 为使用核心 Dynamic Rendering，Vulkan Instance 和设备要求提升到 Vulkan 1.3。

需要特别区分：

```text
逻辑入口：VSMain / PSMain
SPIR-V 模块导出入口：main
```

`ShaderBinary::entryPoint` 保存逻辑入口，供缓存和诊断使用；`ShaderBinary::emittedEntryPoint` 保存提交给原生 API 的名字。第一次窗口验证失败在 `vkCreateGraphicsPipelines`，原因正是把 `VSMain` 传给了实际只导出 `main` 的 SPIR-V 模块。现在 `ShaderCompiler` 测试会固定检查这一约定。

## 10. Pipeline 状态

Vulkan Scene Pipeline 由公共 `GraphicsPipelineDescription` 创建，当前包含：

- `POSITION`：Location 0，`R32G32B32_SFLOAT`
- `COLOR`：Location 1，`R32G32B32A32_SFLOAT`
- `NORMAL`、`TEXCOORD` 和 `TANGENT`
- Triangle List
- 无背面剔除
- 单采样
- 一个 Color Attachment 和一个 D32 Depth Attachment
- 动态 Viewport 和 Scissor
- 一个 PipelineLayout 和 Set 0 DescriptorSet
- Frame、Object、Material 三个 Uniform Buffer
- Albedo Texture 与独立 Sampler

Viewport 使用负 Height：

```text
y = framebufferHeight
height = -framebufferHeight
```

这会把 Vulkan Framebuffer 的 Y 方向调整为与当前 D3D/HLSL 场景约定一致。后续迁移 Camera 和投影矩阵时，必须把 Viewport 翻转与投影矩阵翻转视为二选一，不能重复处理。

## 11. Resize 和 Out-of-date

窗口 Resize、`vkAcquireNextImageKHR` 或 `vkQueuePresentKHR` 都可能报告 Swapchain 已过期。重建顺序为：

```text
DeviceWaitIdle
  -> Renderer destroy old Pipeline
  -> Context destroy TextureView/Framebuffer/兼容性 RenderPass/ImageView/Swapchain
  -> Context create new Swapchain resources
  -> Renderer recreate compatible Pipeline
```

VertexBuffer 和 PipelineLayout 不依赖窗口尺寸，因此不会重复创建。

## 12. 自动截图

设置：

```powershell
$env:PRISM_RENDER_CAPTURE_PATH='D:\temp\vulkan.bmp'
$env:PRISM_RENDER_EXIT_AFTER_CAPTURE='1'
PrismRender.exe --api=vulkan
```

Capture 路径：

1. Swapchain Image 从 ColorAttachment 转为 TransferSrc
2. `vkCmdCopyImageToBuffer` 复制到 Host Visible/Coherent Buffer
3. 图像转为 Present
4. 等待该帧 Fence
5. Map Buffer
6. 根据 BGRA/RGBA Format 调整通道
7. 翻转行顺序并写入 32-bit BMP

这条路径同时验证了 Barrier、Copy、GPU/CPU 同步和资源内存类型。

## 13. 自动测试

执行：

```powershell
cmake --build build-windows-ci --config Debug
ctest --test-dir build-windows-ci -C Debug --output-on-failure
```

当前四个测试：

- `RhiTypeTranslation`：公共资源描述和 D3D12、D3D11、Vulkan 类型转换
- `ShaderCompiler`：29 个入口、58 个 DXIL/SPIR-V 程序、Reflection、缓存和入口名
- `VulkanRuntime`：无窗口创建 Instance、枚举 PhysicalDevice
- `RenderGraph`：Context Pass、依赖验证、共享 Geometry/Fullscreen Pass 命令序列和统计

窗口级回归产物：

```text
build-windows-ci/PrismRenderVulkanDeferredStage13.bmp
```

## 14. 推荐学习顺序

1. `ApplicationLauncher.cpp`：后端如何选择
2. `VulkanApplication.cpp`：最小主循环
3. `VulkanLoader.cpp`：为什么不直接链接 Vulkan SDK
4. `VulkanTypeConversions.cpp`：中立意图如何映射
5. `VulkanContext::Initialize`：对象创建依赖
6. `VulkanContext::BeginFrame/EndFrame`：同步和命令流
7. `GraphicsResources.h`：公共资源契约
8. `PipelineState.h/.cpp`：公共 Pipeline 契约与校验
9. `VulkanResources.cpp`：Buffer、Texture 和 Descriptor 生命周期
10. `VulkanPipeline.cpp`：公共描述到 VkPipeline
11. `VulkanSceneRenderer::CreateSceneResources`：Asset 到 DescriptorSet
12. `VulkanSceneRenderer::Render`：RenderGraph 与共享 Pass
13. `RhiPasses.cpp`：后端无关的 Draw 算法
14. `VulkanContext::RecordPresentTransition`：Image Layout
15. `VulkanContext::SaveCapture`：GPU 回读
16. `tests/RenderGraphTests.cpp` 和 Stage 14 双后端截图：理解自动测试边界

## 15. Stage 14 结果与下一阶段

1. 把公共 Rendering Scope 接入 D3D12 HDR/GBuffer/Bloom：已完成
2. 迁移共享 GBuffer 与 Deferred Lighting：已完成 Vulkan 基础版
3. 迁移共享 HDR、Bloom 和 Tonemap：已完成 Vulkan 基础版
4. Vulkan Shadow 扩展为 CSM Texture2DArray
5. 从 Slang Reflection 合并 PipelineLayoutDescription
6. 增加 Upload Ring、异步批量上传和 Fence 延迟释放
7. RenderGraph 自动生成 Barrier：Vulkan 高级链已完成整纹理状态跟踪
8. 加入 ImGui Vulkan Renderer Backend
9. 让两后端消费同一 RenderScene 和 Camera：基础预览场景已完成；完整资源与 RenderSettings 待统一
10. 对完整场景做 Golden Image 差异测试

Stage 10 的公共资源见 `docs/RHI_RESOURCE_GUIDE_CN.md`，Stage 11 的公共 Pipeline 见 `docs/RHI_PIPELINE_PASS_GUIDE_CN.md`，Stage 12 的 Dynamic Rendering 和 Shadow 见 `docs/RHI_RENDERING_SHADOW_GUIDE_CN.md`，Stage 13 的完整高级链见 `docs/RHI_DEFERRED_POST_PROCESS_GUIDE_CN.md`，Stage 14 的共享场景、动态 Offset 与自动 Barrier 见 `docs/RHI_SCENE_DYNAMIC_BARRIER_GUIDE_CN.md`，Stage 15 的 glTF/Cubemap/Settings 统一和 Golden Image 见 `docs/RHI_BACKEND_PARITY_GOLDEN_GUIDE_CN.md`。当前已经能对每个后端执行可靠视觉回归；跨 API 强制阈值需要继续统一 PBR/IBL、CSM 和编辑器合成。
