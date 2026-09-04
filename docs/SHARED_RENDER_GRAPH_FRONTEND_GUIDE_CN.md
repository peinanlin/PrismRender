# PrismRender 统一 D3D12/Vulkan 渲染前端

本文记录架构路线 Stage 21 的实际实现。目标是让 D3D12 与 Vulkan 只实现底层命令和资源对象，高级渲染流程只建一次图。

实现日期：2026-07-19

## 1. 为什么需要统一前端

此前两个后端虽然使用同一套 Slang Shader 和相同 PBR 参数，但 `SceneRendererStage4.cpp` 与 `VulkanSceneRenderer.cpp` 分别声明 Shadow、GBuffer、Deferred、Hi-Z、Bloom 和 Tonemap。新增 Pass、修改依赖或调整队列时必须改两次，容易出现以下分叉：

- 一端漏读某个 GBuffer；
- 一端状态为 UAV，另一端仍是 SRV；
- Pass 顺序和 Culling 根不同；
- Agent 只能看到两份不同的 RDG 报告。

Stage 21 新增 `SharedRenderGraphFrontend`。它负责算法级资源和 Pass，两个后端只注入执行回调。

## 2. 文件范围

### 2.1 新增文件

- `src/Renderer/SharedRenderGraphFrontend.h`
- `src/Renderer/SharedRenderGraphFrontend.cpp`
- `docs/SHARED_RENDER_GRAPH_FRONTEND_GUIDE_CN.md`
- `examples/harness/shared_render_frontend_validation.jsonl`

### 2.2 主要修改文件

- `src/Renderer/D3D12SceneRenderer.h`
- `src/Renderer/D3D12SceneRenderer.cpp`
- `src/Renderer/VulkanSceneRenderer.h`
- `src/Renderer/VulkanSceneRenderer.cpp`
- `src/Renderer/RenderGraph.cpp`
- `src/RHI/Vulkan/VulkanResources.h/.cpp`
- `src/RHI/Vulkan/VulkanContext.h/.cpp`
- `tests/RenderGraphTests.cpp`
- `CMakeLists.txt`

## 3. 公共前端结构

公共输入分为三组：

```cpp
SharedRenderGraphResources // 实际 RHI Buffer/Texture 和初始状态
SharedRenderGraphOptions   // Shadow、Deferred、Bloom、输出终态
SharedRenderGraphCallbacks // 每个 Pass 的后端命令录制函数
```

`Build()` 固定生成以下默认图：

```text
Shadow
  -> GBuffer
  -> DeferredLighting
  -> HiZBuild (Compute)
  -> BloomExtract (Compute)
  -> BloomHorizontal (Compute)
  -> BloomVertical (Compute)
  -> Tonemap
  -> OutputReady
```

当配置关闭 Deferred 或 Bloom 时，公共前端分别切换到 `ForwardGeometry` 或 `BloomClear`。当前 Vulkan 产品路径仍固定使用 Deferred，因为 Vulkan Forward Pipeline 尚未作为独立功能开放；默认路径与 D3D12 完全一致。

## 4. 数据流

```text
D3D12/Vulkan Renderer
  -> 填写本帧公共 RHI 资源
  -> 填写后端 Pass 回调
  -> SharedRenderGraphFrontend::Build()
  -> 强类型 Handle + Version
  -> Parameter Pass + Blackboard
  -> RDG 编译依赖、生命周期、队列、Barrier
  -> D3D12CommandContextAdapter / VulkanContext
  -> 后端回调录制真实命令
```

公共图使用统一名称：

- `FrameConstants`
- `ShadowMap`
- `DepthBuffer`
- `GBuffer0..3`
- `HiZ`
- `HdrColor`
- `BloomA/B`
- `OutputColor`

每个写入都产生下一版本。例如 `BloomA v1` 由 Extract 写入，Vertical 再生成 `BloomA v2`，Tonemap 只能读取 v2。两个后端因此不能悄悄读取旧 Bloom。

构建结果 `SharedSceneGraphHandles` 会写入 RDG Blackboard。后续模块可以通过类型查询场景纹理，不依赖字符串和具体渲染器类。

## 5. 回调仍然保留的原因

公共前端负责“做什么”和“资源何时可用”，后端回调暂时负责“如何录制该 Pass”：

```text
公共层：Pass、Handle、Version、State、Queue、Culling
后端层：RenderingInfo、Pipeline、DescriptorSet、Draw/Dispatch
```

这是 RHI 的正常边界，不代表每个 API 重写渲染算法。Shadow/PBR/Bloom 算法来自共享 Shader 和公共图；D3D12/Vulkan 回调只是把公共操作映射到各自命令缓冲。

`AddParameterPass()` 执行前会自动解析所有声明的 Texture、Buffer 和 View。即使一个回调暂时仍从渲染器成员读取对象，失效句柄也会在录制前被发现。

## 6. Vulkan Swapchain 外部纹理

旧 Vulkan BackBuffer 只有 `ITextureView`，`GetTexture()` 返回空指针，因此 RDG 只能把它记为字符串 `BackBuffer`。

本阶段为 Swapchain Image 增加非拥有式 `VulkanTexture`：

- 包装 `VkImage` 和公共 `TextureDescription`；
- 不创建、不销毁 Swapchain Image；
- 外部 `VulkanTextureView` 持有包装对象；
- `VulkanContext::GetCurrentBackBufferTexture()` 将它暴露给 RDG。

生命周期顺序为：

```text
清空 RDG/TextureView 引用
  -> 清空非拥有式 Texture 包装
  -> 销毁 VkImageView
  -> 销毁 VkSwapchainKHR
```

这样 BackBuffer 可以参与版本、Barrier、输出根和诊断，同时不会重复销毁系统拥有的 Image。

## 7. 输出终态为什么不同

公共算法和 Pass 名称一致，但 `OutputReady` 的状态是后端契约：

- D3D12：`ShaderResource`，因为编辑器随后采样 SceneColor；
- Vulkan：`RenderTarget`，因为 `VulkanContext::EndFrame()` 统一执行 Color Attachment 到 Present/CopySource 的最终转换。

因此两个报告的 Pass/资源集合完全一致，Graph Signature 会因最后一个状态不同而不同。这是平台输出所有权差异，不是 Lighting 或 Post Process 算法分叉。

## 8. 验证

构建和单测：

```powershell
cmake --build build-windows-ci --config Debug --parallel 8
ctest --test-dir build-windows-ci -C Debug --output-on-failure
```

结果：完整构建成功，8/8 CTest 通过。公共前端单测验证：

- 9 个 Pass 都执行；
- 9 个 Pass 都是 Parameter Pass；
- 11 个 Texture 和 1 个 Buffer 注册；
- 11 个资源版本推进；
- `Shadow` 到 `OutputReady` 顺序稳定；
- `SharedSceneGraphHandles` 已写入 Blackboard。

真实 Harness：

```powershell
.\build-windows-ci\Debug\PrismHarness.exe --headless `
  --project-root . `
  --commands examples\harness\shared_render_frontend_validation.jsonl `
  --output automation\stage21-results.jsonl
```

结果为 3/3 成功。D3D12 与 Vulkan 报告均为：

- `declaredPassCount=9`
- `activePassCount=9`
- `parameterPassCount=9`
- `registeredTextureCount=11`
- `registeredBufferCount=1`
- `versionedResourceCount=11`
- Pass 名称和资源名称集合完全相同

Tonemap Golden Image：MAE `0.0000640262`、RMSE `0.000512323`、最大通道误差 `0.0196078`、变化像素比例 `0`。

## 9. 设计取舍与下一步

- 公共前端不直接持有 Pipeline：避免把后端资源所有权塞进建图器；
- 保留后端回调：迁移风险低，但 Rendering Attachment 和 Descriptor 绑定仍可继续收敛；
- Vulkan Forward 暂未开放：默认现代 Deferred 路径完整，后续可补公共 Forward 分支测试；
- Hi-Z 内部仍自行循环 mip：Stage 22 会拆成可独立调度的 mip Pass，并接入 GPU Driven 可见性。

下一阶段进入 GPU Driven：公共可见性 Buffer、对象/材质 GPU 表、Frustum/Hi-Z Occlusion Compute、Indirect Argument、D3D12 ExecuteIndirect 与 Vulkan DrawIndirectCount。
