# PrismRender 共享场景、动态 Buffer Offset 与自动 Barrier 指南

本文档记录 Stage 14 的实际实现。本阶段解决三个架构问题：D3D12 与 Vulkan 使用同一种 `RenderScene` 数据模型；公共 RHI 能表达每次 Draw 的动态常量缓冲区偏移；RenderGraph 根据 Pass 的资源状态声明自动插入纹理 Barrier。

## 1. 完成边界

本阶段已经完成：

- `DefaultSceneFactory` 用一组公共描述生成编辑器预览场景
- `VulkanApplication` 持有公共 `RenderScene`、`Camera` 和 `CameraController`
- Vulkan Renderer 不再保存硬编码位置、旋转和缩放
- 一块每帧对象常量缓冲区供所有 Draw 复用
- 公共 `DynamicConstantBuffer` 和 `DynamicBufferOffset`
- Vulkan 映射到动态 Uniform Buffer Descriptor
- D3D12 映射到 Root CBV
- RenderGraph 登记真实 `ITexture`、当前状态和 Pass 期望状态
- Shadow、GBuffer、HDR、Bloom 的 Vulkan Barrier 由 RenderGraph 生成
- 单元测试验证动态 Offset 转发和自动 Barrier 顺序
- D3D12、Vulkan 均完成真实 GPU 截图回归

仍需后续完成：

- D3D12 高级主路径的全部对象常量改用公共动态 Buffer
- D3D12 原生高级目标全部替换为公共 `ITexture` 后，接入同一套自动 Barrier
- RenderGraph 子资源、UAV Hazard、跨队列和队列所有权跟踪
- 运行时增删物体后的 GPU 场景资源重建
- 两后端完全相同的 glTF、环境贴图、编辑器 UI 和设置
- Golden Image 像素差异阈值测试

## 2. 文件变更

新增：

- `docs/RHI_SCENE_DYNAMIC_BARRIER_GUIDE_CN.md`

主要修改：

- `src/RHI/GraphicsResources.h`
- `src/RHI/ICommandContext.h`
- `src/RHI/D3D12/D3D12Resources.h/.cpp`
- `src/RHI/D3D12/D3D12GraphicsDevice.cpp`
- `src/RHI/D3D12/D3D12CommandContextAdapter.h/.cpp`
- `src/RHI/Vulkan/VulkanResources.h/.cpp`
- `src/RHI/Vulkan/VulkanContext.h/.cpp`
- `src/Renderer/RenderGraph.h/.cpp`
- `src/Renderer/RhiPasses.h/.cpp`
- `src/Renderer/VulkanSceneRenderer.h/.cpp`
- `src/Scene/DefaultSceneFactory.h/.cpp`
- `src/Core/VulkanApplication.h/.cpp`
- `tests/RenderGraphTests.cpp`
- `tests/RhiTypeTranslationTests.cpp`

## 3. 共享场景数据

### 3.1 之前的问题

D3D12 Application 已经拥有 `RenderScene`，但 Vulkan Renderer 内部另有一组 `SceneObject`，其中直接保存位置、旋转、缩放、Mesh 和 Material。结果是修改公共场景并不会影响 Vulkan，两个后端也无法确认自己渲染的是同一组输入。

### 3.2 当前所有权

```text
VulkanApplication
  |-- RenderScene
  |     |-- Camera
  |     |-- DirectionalLight
  |     `-- RenderObject[]
  |            |-- Transform
  |            |-- Mesh
  |            `-- Material
  |-- CameraController
  `-- VulkanSceneRenderer
          `-- 只保存后端 GPU 绑定资源
```

`DefaultSceneFactory` 现在有两个入口：

```cpp
PopulateEditorPreviewScene(AssetRegistry&, D3D12Context&, RenderScene&);
PopulateEditorPreviewScene(IGraphicsDevice&, RenderScene&);
```

两者消费相同的 `PreviewMaterials` 和 `PreviewObjects` 描述。D3D12 入口同时注册 Asset Handle；公共 RHI 入口直接创建后端 Mesh、Texture 和 Material。这样共享的是场景语义与布局，而 GPU 对象仍由各后端独立创建。

### 3.3 相机交互

Vulkan 现在复用现有 `CameraController`：

- 右键拖动：观察；配合 `W/A/S/D/Q/E` 移动
- `Alt + 左键`：环绕
- 中键：平移
- 滚轮：推拉

Controller 修改 `RenderScene::Camera`，Renderer 每帧读取 `GetViewProjectionMatrix()` 和相机位置，不再使用硬编码矩阵。

## 4. 动态 Buffer Offset

### 4.1 公共接口

Descriptor Layout 使用：

```cpp
{1, DescriptorType::DynamicConstantBuffer, 1, ShaderStageFlags::Vertex}
```

每次 Draw 使用：

```cpp
draw.dynamicBufferOffsets.push_back({1, objectIndex * 256});
commandContext.BindDescriptorSet(*draw.descriptorSet, draw.dynamicBufferOffsets);
```

对象常量数据按以下方式排列：

```text
Frame 0 Object Buffer
  offset   0: ObjectConstants[0]
  offset 256: ObjectConstants[1]
  offset 512: ObjectConstants[2]
  ...

Frame 1 Object Buffer
  使用相同布局，避免 CPU 覆盖 GPU 正在读取的数据
```

当前统一使用 256 字节步长。它满足 D3D12 CBV 对齐要求，也满足本机 Vulkan 的 `minUniformBufferOffsetAlignment`；Vulkan 后端仍会读取设备限制并在绑定时校验。

### 4.2 Vulkan 映射

```text
DynamicConstantBuffer
  -> VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC

DynamicBufferOffset[]
  -> vkCmdBindDescriptorSets(dynamicOffsetCount, pDynamicOffsets)
```

`WriteBuffer` 写入基础 Buffer、基础 Offset 和 Range。每次 Bind 再把 Draw Offset 传给 Vulkan。后端会检查：Binding 是否为动态类型、是否重复、是否满足设备对齐、最终 Range 是否越界。

### 4.3 D3D12 映射

D3D12 没有与 Vulkan 动态 Uniform Descriptor 完全相同的对象。本项目将动态常量绑定生成为 Root Signature 中的 Root CBV：

```text
普通 CBV/SRV/UAV -> Descriptor Table
Sampler           -> Sampler Descriptor Table
动态 CBV          -> D3D12_ROOT_PARAMETER_TYPE_CBV
```

Bind 时调用：

```cpp
SetGraphicsRootConstantBufferView(rootParameter, baseGpuAddress + dynamicOffset);
```

这种方案不会在同一命令列表内反复覆盖一个描述符，也不会让多个 Draw 最终都看到最后一次写入的 CBV。代价是每个动态 CBV 占一个 Root Parameter，因此大量动态绑定应在后续改为更完整的 Frame/Object 数据布局或 Bindless 方案。

## 5. RenderGraph 自动 Barrier

### 5.1 资源登记

内部渲染目标先登记真实纹理和初始状态：

```cpp
graph.DeclareTexture("HdrColor", *hdrTexture, initialState);
```

外部已经可读资源使用 `ImportTexture`；只有逻辑依赖、Swapchain 外部 View 或尚未公共化的资源仍可使用 `ImportResource`。

### 5.2 Pass 声明

```cpp
graph.AddResourceContextPass(
    "SharedBloomExtract",
    {{"HdrColor", ResourceState::ShaderResource}},
    {{"BloomA", ResourceState::RenderTarget}},
    execute);
```

执行每个 Pass 前，RenderGraph 做四件事：

1. 检查 Read 是否已经 Import 或由前序 Pass 产生。
2. 检查同一个 Pass 是否为同一纹理声明了冲突状态。
3. 比较纹理当前状态和期望状态。
4. 状态不同时调用 `ICommandContext::TextureBarrier` 并更新跟踪状态。

例如 BloomA 的状态序列是：

```text
Undefined/ShaderResource
  -> RenderTarget       Bloom Extract 写入
  -> ShaderResource     Horizontal Blur 读取
  -> RenderTarget       Vertical Blur 写回
  -> ShaderResource     Tonemap 读取
```

Rendering Scope 内部的 Attachment 现在对这些纹理声明 `RenderTarget -> RenderTarget` 或 `DepthWrite -> DepthWrite`，因此它只负责绑定、Clear、Load/Store 和绘制，不再重复决定跨 Pass 状态。

### 5.3 当前限制

当前状态跟踪以整张纹理为粒度。它尚不理解 Mip、Array Layer、UAV 写后写、Async Compute 或跨 Queue 所有权。加入这些功能时，应扩展资源访问结构，而不是在 Pass 回调里重新散落手写 Barrier。

## 6. 一帧数据流

```text
CameraController
  -> RenderScene::Camera
  -> VulkanSceneRenderer::UpdateConstants

RenderScene::RenderObject[]
  -> World Matrix
  -> 每帧 Object Buffer 的 256 字节 Slot
  -> DynamicBufferOffset

RenderGraph 状态声明
  -> 自动 TextureBarrier
  -> Shadow
  -> GBuffer
  -> Deferred Lighting + Sky/Sun
  -> HDR
  -> Bloom Extract/H/V
  -> Tonemap
  -> Swapchain
```

## 7. 验证方法与结果

完整构建：

```powershell
cmake --build build-windows-ci --config Debug --parallel 8
```

自动测试：

```powershell
ctest --test-dir build-windows-ci -C Debug --output-on-failure
```

结果为 4/4 通过：

- RHI 类型与布局校验
- Slang Shader 双目标编译
- Vulkan Runtime
- RenderGraph 与共享 RHI Pass

真实 GPU 回归：

- `build-windows-ci/PrismRenderVulkanSharedSceneStage14.bmp`
- `build-windows-ci/PrismRenderD3D12SharedSceneStage14.bmp`

Vulkan 截图验证了五个公共预览对象使用不同动态 Offset，且完整高级 Pass 链可执行；D3D12 截图验证公共接口扩展没有破坏现有主路径。

## 8. 建议学习顺序

1. 读 `RenderScene.h` 和 `RenderObject.h`，先理解场景数据。
2. 读 `DefaultSceneFactory.cpp`，找到两后端共用的描述数组。
3. 读 `GraphicsResources.h` 中的动态 Descriptor 类型。
4. 读 `ICommandContext.h` 和 `RhiPasses.cpp`，理解 Offset 如何进入 Draw。
5. 对照 Vulkan 与 D3D12 后端，理解同一抽象的不同原生映射。
6. 读 `RenderGraph.h/.cpp`，手工推演 BloomA 的状态序列。
7. 读 `VulkanSceneRenderer::Render`，把每个资源声明与对应 Pass 对上。
8. 读 `RenderGraphTests.cpp`，理解自动 Barrier 的最小可验证行为。
9. 运行两个后端并观察相机控制与截图。

这一阶段的核心不是“减少几行 Barrier”，而是让场景、Draw 参数和资源状态各自只有一个可信来源。后续新增 Metal、更多 Pass 或 GPU Driven Rendering 时，渲染算法才不需要为每个 API 再复制一份控制流。
