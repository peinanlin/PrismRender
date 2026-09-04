# GTAO 与 SSR 实现和学习指南

## 1. 两项技术分别做什么

GTAO（Ground-Truth Ambient Occlusion）近似计算物体缝隙、接触面和墙角中有多少
环境光被周围几何体挡住。它不生成新的光源阴影，而是让间接光的空间层次更清楚。

SSR（Screen Space Reflections）用当前屏幕已经渲染出的深度、法线和 HDR 颜色查找
反射命中点。它能低成本增加地面、金属和光滑表面的反射，但不能反射屏幕外物体。

## 2. 总数据流

```text
GBuffer0: WorldPosition + Roughness
GBuffer1: WorldNormal + Metallic
             |
             +-> GTAO Compute -> AmbientOcclusion
             |                      |
Shadow ------+-> Deferred Lighting <-+
                                    |
Depth -> HiZBuild ------------------+
                                    |
HDR + GBuffer0/1 + Hi-Z -> SSR Compute
                              |
                              +-> ScreenSpaceColor
                                      |
                                      +-> TAA -> Bloom -> Tonemap
```

## 3. GTAO 如何实现

`GtaoCS` 对每个有效 GBuffer 像素执行以下步骤：

1. 从 GBuffer 读取世界坐标和法线。
2. 在屏幕上选择 8 个方向。
3. 每个方向读取 4 个逐渐变远的样本。
4. 用“中心到样本”的世界空间向量估计该方向的最高遮挡角。
5. 汇总所有方向并转换为 0 到 1 的可见度。

当前实现属于 GTAO 的工程基线：核心是方向性地平线搜索，而不是随机半球 SSAO。
它使用确定性采样，因此 Agent 截图和双 API Golden Image 不会受到随机噪声影响。

输出在 RDG 中名为 `AmbientOcclusion`。Deferred Shader 把它与材质 Occlusion 相乘，
只作用于 Ambient 与 IBL，不会错误地把太阳直射光整体变暗。

## 4. SSR 如何实现

`ScreenSpaceReflectionsCS` 对每个像素执行：

1. 用相机位置、世界坐标和法线计算世界空间反射方向。
2. 沿反射方向逐步前进。
3. 使用 ViewProjection 把每个候选点投影回屏幕 UV。
4. 根据步数选择 Hi-Z mip，比较射线深度与场景深度。
5. 找到交点后读取该位置的 HDR 颜色。
6. 使用屏幕边缘衰减、距离衰减、粗糙度、金属度和 Fresnel 控制反射权重。

如果关闭 SSR，或切换到 Forward Rendering，同一个 Pass 会把 HDR 复制到
`ScreenSpaceColor`。这样 TAA 后续输入始终稳定，不需要运行时重建 DescriptorSet。

## 5. 公共 RHI 模块

`ScreenSpaceEffects` 统一拥有：

- GTAO 与 SSR Compute Pipeline
- 每飞行帧 Constant Buffer 和 DescriptorSet
- Ambient Occlusion Texture
- Screen Space Composite Texture
- D3D12/Vulkan 共同的初始化状态和 Dispatch

Shader 只写一套：

```text
ScreenSpaceEffects.hlsl
  -> Slang DXIL -> D3D12 Compute
  -> Slang SPIR-V -> Vulkan Compute
```

后端没有各自实现 GTAO 或 SSR 算法。

## 6. RDG 如何安排

公共 RDG 新增两个 Pass：

- `GTAO`：读取 GBuffer0/1，写 AmbientOcclusion。
- `ScreenSpaceReflections`：读取 HDR、Hi-Z、GBuffer0/1 和 AO，写
  ScreenSpaceColor。

依赖关系让 RDG 自动生成：

- Graphics GBuffer -> Compute GTAO
- Compute GTAO -> Graphics Deferred
- Graphics Deferred + Compute Hi-Z -> Compute SSR
- Compute SSR -> Compute TAA

因此 D3D12 Fence 和 Vulkan Timeline Semaphore 使用现有多队列执行器完成同步。

## 7. 主要文件

- `src/Renderer/Features/ScreenSpaceEffects.h/.cpp`：资源、Pipeline、Descriptor 与 Dispatch
- `assets/shaders/ScreenSpaceEffects.hlsl`：GTAO 地平线搜索和 SSR 射线步进
- `assets/shaders/Deferred.hlsl`：把 GTAO 应用于环境光和 IBL
- `src/Renderer/SharedRenderGraphFrontend.h/.cpp`：公共 Pass 和依赖
- `src/Renderer/D3D12SceneRenderer.cpp`：D3D12 RHI 接线
- `src/Renderer/VulkanSceneRenderer.cpp`：Vulkan RHI 接线
- `src/Renderer/RenderSettings.h`、`src/UI/DebugPanel.cpp`：运行时开关
- `tests/ShaderCompilerTests.cpp`：DXIL/SPIR-V 与 Reflection
- `tests/RenderGraphTests.cpp`：资源、版本和 Pass 图

## 8. 当前权衡与升级方向

- AO 当前使用 `RGBA16F`，因为两个 Compute 入口共享一个 `RWTexture2D<float4>`
  输出契约；后续可拆分 Shader 模块并改为 `R8/R16`，降低带宽。
- GTAO 当前是全分辨率、无时域降噪。生产版本可改为半分辨率、深度感知滤波和
  Motion Vector 时间累积。
- SSR 当前最多 56 步，只有首次深度穿越判断。生产版本可增加 Hi-Z
  粗到细遍历、二分细化、法线命中验证和去噪。
- SSR 无法看到屏幕外、背面或被遮挡的物体。后续应使用 Reflection Probe 或
  Ray Tracing 作为 Miss Fallback。
- 当前 AO 和 SSR 参数为模块内保守默认值；下一步可加入 Debug Panel Slider 和
  性能质量档位。

## 9. 验证

```powershell
cmake --build build-windows-ci --config Debug --target PrismRender PrismHarness PrismShaderCompilerTests PrismRenderGraphTests
.\build-windows-ci\Debug\PrismShaderCompilerTests.exe
.\build-windows-ci\Debug\PrismRenderGraphTests.exe
.\build-windows-ci\Debug\PrismHarness.exe --commands examples\harness\gpu_driven_validation.jsonl --output automation\reports\stage-gtao-ssr-validation.jsonl
```

本阶段结果：

- 14 个 Shader 入口共 28 个 DXIL/SPIR-V 程序通过编译和 Reflection。
- RDG 单元测试确认 17 张纹理、12 个默认 Pass 和 16 个资源版本。
- D3D12 与 Vulkan GPU Driven + 原生多队列均完成真实多帧运行。
- Harness 的 D3D12 RDG、Vulkan RDG 和跨 API 图像对比全部成功。

## 10. 推荐阅读顺序

1. 先画出第 2 节的数据流。
2. 阅读 `GtaoCS` 的方向和步进循环。
3. 阅读 `ScreenSpaceReflectionsCS` 的投影与深度命中。
4. 阅读 `ScreenSpaceEffects::Resize()` 的资源所有权。
5. 阅读 Deferred Shader 如何组合材质 AO 与 GTAO。
6. 最后查看 RDG 中 Graphics/Compute 之间的依赖。
