# Stage 16：D3D12/Vulkan 数值一致性实现与学习指南

本文档记录 PrismRender 从“两个 API 都能运行”推进到“两个 API 使用同一套渲染算法，并能自动验证结果”的完整实现。

本阶段不是把 D3D12 调用翻译成 Vulkan 调用。正确的分层是：

```text
RenderScene / RenderSettings / Material
                |
                v
共享 RenderGraph Pass + 共享 Shader + 共享 GPU 数据结构
                |
                v
公共 RHI：Resource / Pipeline / DescriptorSet / CommandContext
           |                              |
           v                              v
      D3D12 后端                       Vulkan 后端
```

上层算法只实现一次，D3D12 和 Vulkan 后端只负责实现 RHI 对应的资源创建、状态转换、Descriptor 和命令提交。

## 1. 本阶段完成了什么

1. Frame、Object、Material、Shadow 和 PostProcess 常量改为公共 C++ 数据结构。
2. Slang Reflection 生成公共 DescriptorSet Layout，DXIL 和 SPIR-V 共用同一绑定约定。
3. D3D12 高级 Pass 从原生 Root 参数提交迁移到公共 Pipeline、DescriptorSet 和动态 Buffer Offset。
4. D3D12 与 Vulkan 共用 Mesh、Shadow、Deferred、Sky、Bloom 和 Tonemap Shader 源文件。
5. Vulkan 补齐完整材质通道、三层 CSM、IBL、PBR 和后处理输入。
6. 两个后端共用 Cascade 切分、稳定阴影矩阵、环境卷积和 BRDF LUT 数据。
7. 统一线性 HDR 到 sRGB 的输出规则，并优先使用 UNORM Swapchain。
8. 增加固定相机、固定设置、逐 Pass 捕获和严格 Golden Image 门禁。

最终自动捕获结果从 Stage 15 的平均绝对误差 `0.230762` 降到 `0.000018`，在默认 8/255 单通道容差下变化像素比例为 `0`。

## 2. 文件清单

### 2.1 新增文件

- `src/Renderer/SharedRenderData.h`
- `src/Renderer/ShadowCascades.h`
- `src/Renderer/ShadowCascades.cpp`
- `src/Renderer/RenderCapture.h`
- `src/RHI/ShaderLayoutBuilder.h`
- `src/RHI/ShaderLayoutBuilder.cpp`
- `assets/shaders/DebugView.hlsl`
- `docs/RHI_NUMERICAL_PARITY_GUIDE_CN.md`

### 2.2 主要修改文件

- `src/Renderer/D3D12SceneRenderer.h`
- `src/Renderer/D3D12SceneRenderer.cpp`
- `src/Renderer/VulkanSceneRenderer.h`
- `src/Renderer/VulkanSceneRenderer.cpp`
- `src/RHI/GraphicsPipeline.h`
- `src/RHI/D3D12/D3D12GraphicsPipeline.cpp`
- `src/Asset/SlangShaderCompiler.cpp`
- `src/Asset/IblEnvironmentBuilder.h/.cpp`
- `src/Asset/Texture.h/.cpp`
- `src/Scene/DefaultSceneFactory.cpp`
- `assets/shaders/Mesh.hlsl`
- `assets/shaders/Shadow.hlsl`
- `assets/shaders/Deferred.hlsl`
- `assets/shaders/PostProcess.hlsl`
- `tests/ShaderCompilerTests.cpp`
- `tools/CaptureGoldenImages.ps1`
- `CMakeLists.txt`

原来仅供 Vulkan 简化路径使用的 `RhiScene.hlsl`、`RhiDeferred.hlsl`、`RhiShadow.hlsl`、`RhiSky.hlsl` 和 `RhiPostProcess.hlsl` 已删除。继续保留两套 Shader 会让两边的光照公式再次分叉。

## 3. 统一 GPU 数据契约

`SharedRenderData.h` 是 CPU 到 GPU 的公共数据契约。两个后端都使用以下结构：

| C++ 结构 | Shader cbuffer | 大小 |
| --- | --- | ---: |
| `SharedFrameConstants` | `FrameConstants` | 496 字节 |
| `SharedObjectConstants` | `ObjectConstants` | 128 字节 |
| `SharedMaterialConstants` | `MaterialConstants` | 80 字节 |
| `SharedShadowPassConstants` | `ShadowPassConstants` | 16 字节 |
| `SharedPostProcessConstants` | `PostProcessConstants` | 208 字节 |

`static_assert` 同时检查结构大小和关键成员偏移。这样一旦 C++ 新增字段但 Shader 没同步，构建阶段就会先暴露一部分布局错误。

这里需要区分两个概念：

- 结构逻辑大小必须与 cbuffer 布局一致。
- D3D12 CBV 分配仍必须满足 256 字节地址对齐；对齐后的上传跨度不等于结构本身大小。

公共常量的数据流为：

```text
RenderScene + Camera + RenderSettings
  -> SharedFrameConstants
RenderObject::Transform
  -> SharedObjectConstants
MaterialAsset + MaterialParameters
  -> SharedMaterialConstants
  -> 公共 Buffer
  -> IDescriptorSet
  -> D3D12 CBV / Vulkan Uniform Buffer
```

## 4. Slang Reflection 如何生成 DescriptorSet Layout

Shader 仍使用熟悉的 HLSL 寄存器写法。`ShaderLayoutBuilder` 把 Reflection 结果映射到公共、无冲突的绑定空间：

| Shader 资源 | 公共 binding |
| --- | ---: |
| `bN` ConstantBuffer | `N` |
| `tN` ShaderResource | `16 + N` |
| `uN` UnorderedAccess | `32 + N` |
| `sN` Sampler | `48 + N` |

例如 `b1` 保持 binding 1，`t0` 变成 binding 16，`s0` 变成 binding 48。D3D12 后端据此创建 Root Signature，Vulkan 后端据此创建 DescriptorSetLayout。

每个 Pipeline 会合并 Vertex/Pixel 阶段的 Reflection：同一 binding 的类型必须一致，Shader Stage Flags 则取并集。Object 和 Shadow Cascade 等每次 Draw 改变的 CBV 被标记为 `DynamicConstantBuffer`，提交时通过 `DynamicBufferOffset` 选择当前对象。

Slang 的 SPIR-V 反射可能把顶层参数报告为 Descriptor Table 类别，因此 `SlangShaderCompiler` 还会检查反射类型本身：

- `ConstantBuffer` -> ConstantBuffer
- `SamplerState` -> Sampler
- 只读 Resource -> ShaderResource
- 可写 Resource -> UnorderedAccess

这一步避免 Pipeline Layout 因目标格式差异漏掉 `b0` 或纹理绑定。

## 5. D3D12 为什么也要使用公共 DescriptorSet

在迁移前，D3D12 的高级 Pass 直接调用 `SetGraphicsRootConstantBufferView` 和 `SetGraphicsRootDescriptorTable`，而 Vulkan 使用公共 DescriptorSet。即使 Shader 算法相同，两边仍是两套绑定逻辑，容易出现纹理槽位、Sampler 或动态偏移不一致。

现在 `SceneRenderer` 在初始化阶段完成：

```text
CreateSharedRhiPipelines
  -> Slang 编译 DXIL
  -> Reflection 构建 DescriptorSet Layout
  -> D3D12 RHI 创建 GraphicsPipeline

CreateSharedRhiSamplers
  -> 创建公共线性、阴影 Sampler

EnsureSharedRhiDescriptorSets
  -> 每帧/每对象创建完整材质 DescriptorSet

RebuildSharedRhiPassDescriptorSets
  -> 创建 Shadow、Deferred、PostProcess、Debug DescriptorSet
```

运行时 Shadow、Forward、GBuffer、Deferred Lighting、Sky、Bloom、Tonemap 均通过 `IGraphicsPipeline`、`IDescriptorSet` 和 `ICommandContext` 提交。原生 D3D12 对象可继续作为底层实现细节存在，但高级 Pass 不再依赖固定 Root 参数编号。

实例数据的语义也改为公共 Pipeline 描述显式提供 `INSTANCEWORLD0..3` 和 `INSTANCEWVP0..3`，防止 D3D12 后端根据位置猜测语义。

## 6. 一套 Shader 如何输出两个 API

共享 Shader 文件及职责如下：

| 文件 | 主要入口与职责 |
| --- | --- |
| `Mesh.hlsl` | Forward、GBuffer、完整 glTF 材质、PBR、IBL、CSM 采样 |
| `Shadow.hlsl` | 三层方向光 Cascade Shadow 深度写入 |
| `Deferred.hlsl` | GBuffer 解码、PBR 光照、点光源、IBL、阴影 |
| `PostProcess.hlsl` | Sky/Grid/Sun、Bloom、ACES Tonemap、sRGB 输出 |
| `DebugView.hlsl` | ShadowMap 可视化捕获 |

Slang 对同一入口分别生成：

```text
同一 Shader 源文件
  -> Slang target=dxil   -> D3D12 Pipeline
  -> Slang target=spirv  -> Vulkan Pipeline
```

Shader 共享并不代表 API 原生命令相同；它代表材质、光照、阴影和后处理数学只有一份实现。

## 7. 材质、CSM 和 IBL 如何对齐

### 7.1 材质

两个后端都消费相同的：

- Base Color 与 Base Color Texture
- Metallic-Roughness Texture
- Normal Texture 与 Normal Scale
- Occlusion Texture 与 Strength
- Emissive Texture、Color 与 Strength
- Alpha Mode 与 Alpha Cutoff

预览场景曾在 D3D12 使用“彩色常量且关闭 Base Color Texture”，Vulkan 使用“白色常量乘彩色纹理”。两种写法视觉接近，但 GBuffer2 不相同。`DefaultSceneFactory` 现在为两边生成同样的材质参数和纹理路径。

### 7.2 Cascade Shadow Map

`BuildCascadeShadowData` 是两后端唯一的 Cascade 构建函数，统一：

- Cascade 数量：3
- ShadowMap 分辨率：2048
- Split Lambda 与最大阴影距离
- Light View/Projection
- Texel Snapping
- Depth Bias 和 PCF 参数来源

Vulkan Shadow Texture 使用三层 Array，逐层创建 Depth View，并通过动态 Shadow Pass 常量选择 Cascade。

### 7.3 IBL

`IblEnvironmentBuilder::Build(IGraphicsDevice&, const Texture&)` 通过公共设备创建：

- Environment Cubemap
- Irradiance Cubemap
- 多粗糙度 Prefiltered Specular Cubemap Array
- BRDF LUT

D3D12 和 Vulkan 使用同一 CPU 烘焙算法与同一输入 Cubemap，再通过各自 RHI Texture 上传。这样 Golden Image 测试不会把“IBL 预计算算法不同”误认为 API 差异。

## 8. 输出颜色为什么必须显式统一

跨 API 常见的整屏差异来自 sRGB 转换被执行零次或两次。本阶段采用明确规则：

```text
Lighting/Bloom 在 Linear HDR 空间计算
  -> ACES Tonemap
  -> PostProcess.hlsl::LinearToSrgb
  -> UNORM Swapchain
```

Vulkan Swapchain 优先选择 `B8G8R8A8_UNORM`，其次选择其他 UNORM 格式。D3D12 与 Vulkan 因此都由共享 Shader 只执行一次 Linear-to-sRGB 转换。

同时固定 FrontFace、Viewport、深度范围和矩阵约定，防止画面看似相同但深度、剔除方向或投影结果不同。

## 9. 确定性与逐 Pass 捕获

`RenderCapture.h` 定义两组环境变量：

```text
PRISM_RENDER_DETERMINISTIC=1
PRISM_RENDER_CAPTURE_STAGE=shadow|gbuffer0|gbuffer1|gbuffer2|gbuffer3|hdr|bloom|tonemap
```

确定性模式会固定预览相机和渲染设置、关闭相机输入，并避免 D3D12 独有优化路径影响对比。捕获时两个后端使用相同窗口大小、场景、材质、灯光和帧序列。

执行完整逐 Pass 严格回归：

```powershell
.\tools\CaptureGoldenImages.ps1 -CapturePasses -EnforceThresholds
```

严格门槛为：

- `mean_absolute_error <= 0.001`
- `root_mean_square_error <= 0.005`
- `changed_pixel_ratio <= 0.01`

输出包括：

```text
build-windows-ci/golden/d3d12-shadow.bmp
build-windows-ci/golden/vulkan-shadow.bmp
build-windows-ci/golden/shadow-comparison.txt
...
build-windows-ci/golden/d3d12.bmp
build-windows-ci/golden/vulkan.bmp
build-windows-ci/golden/comparison.txt
```

逐 Pass 排查顺序必须从前往后：

1. Shadow 不同，检查 Cascade 矩阵、Bias、深度格式和 PCF。
2. GBuffer 不同，检查顶点输入、材质、纹理色彩空间和编码。
3. HDR 不同，检查 BRDF、灯光、IBL 和阴影采样。
4. Bloom 不同，检查阈值、采样核和中间纹理。
5. Tonemap 不同，检查曝光、ACES 与 sRGB 转换。

本阶段正是通过 `GBuffer2` 报告定位到预览材质表达不同，而不是在最终画面里盲目调整曝光。

## 10. 自动测试覆盖

`ShaderCompilerTests.cpp` 对代表性共享入口同时编译 DXIL 和 SPIR-V，并检查：

- Reflection 能找到 Frame、Object、纹理和 Sampler。
- 资源被映射到 binding 0、1、16..25、48 和 49。
- Object 常量是动态 Buffer。
- 公共常量结构大小保持不变。

完整验证命令：

```powershell
cmake --build build-windows-ci --config Debug --parallel 8
ctest --test-dir build-windows-ci -C Debug --output-on-failure
.\tools\CaptureGoldenImages.ps1 -CapturePasses -EnforceThresholds
```

最近一次最终 Tonemap 报告：

```text
dimensions_match=true
mean_absolute_error=0.000018
root_mean_square_error=0.000270
changed_pixel_ratio=0.000000
maximum_channel_error=0.015686
```

同一次严格逐 Pass 回归结果：

| 阶段 | MAE | RMSE | Changed Ratio | Max Error |
| --- | ---: | ---: | ---: | ---: |
| Shadow | 0.000000 | 0.000026 | 0.000000 | 0.003922 |
| GBuffer0 | 0.000000 | 0.000000 | 0.000000 | 0.000000 |
| GBuffer1 | 0.000000 | 0.000000 | 0.000000 | 0.000000 |
| GBuffer2 | 0.000000 | 0.000000 | 0.000000 | 0.000000 |
| GBuffer3 | 0.000000 | 0.000000 | 0.000000 | 0.000000 |
| HDR | 0.000018 | 0.000270 | 0.000000 | 0.015686 |
| Bloom | 0.000000 | 0.000000 | 0.000000 | 0.000000 |
| Tonemap | 0.000018 | 0.000270 | 0.000000 | 0.015686 |

当前 `assets/environment/default` 没有完整的六张环境图片，运行日志中的 `Environment cubemap face is missing: px` 表示两端都进入公共回退 Cubemap 路径，不是捕获失败。加入真实环境图后应重新生成并评审 Golden 基线。

最大单通道误差仍可能来自不同驱动对浮点插值、过滤和舍入的实现细节，因此目标是严格容差内一致，而不是要求所有 GPU 上每个字节绝对相同。

## 11. 推荐学习与实施顺序

1. 阅读 `SharedRenderData.h`，对照 `Mesh.hlsl` 的 cbuffer 手算偏移。
2. 阅读 `SlangShaderCompiler.cpp`，理解 DXIL/SPIR-V 编译与资源类型反射。
3. 阅读 `ShaderLayoutBuilder.cpp`，画出 b/t/u/s 到公共 binding 的映射表。
4. 阅读 `D3D12GraphicsPipeline.cpp` 和 Vulkan Pipeline 实现，理解同一布局怎样落到两种 API。
5. 从 `SceneRenderer::CreateSharedRhiPipelines` 跟到 D3D12 Pipeline 创建。
6. 阅读 `EnsureSharedRhiDescriptorSets`，理解每帧、每对象和每材质资源所有权。
7. 阅读 `VulkanSceneRenderer::CreateSizeDependentResources` 与 Descriptor 更新路径，对照 D3D12。
8. 阅读 `ShadowCascades.cpp`，理解 Split、稳定投影和 Texel Snapping。
9. 阅读 `IblEnvironmentBuilder.cpp`，理解 Irradiance、Prefilter 和 BRDF LUT 的来源。
10. 按 Shadow、GBuffer、HDR、Bloom、Tonemap 顺序阅读 RenderGraph Pass。
11. 阅读 `PostProcess.hlsl` 的 Tonemap 和 `LinearToSrgb`，理解输出颜色链路。
12. 运行逐 Pass 捕获，故意改动一个材质值，观察第一个失败报告，再恢复改动。

## 12. 当前边界与下一步

本阶段已经完成 D3D12 与 Vulkan 的高级渲染算法共用和自动数值验证，但不等于 RHI 已覆盖现代渲染器的全部能力。后续仍应增量实现：

1. Mip/ArrayLayer 子资源级 RenderGraph 状态跟踪。
2. 公共 Cubemap Mip 链和 GPU Compute IBL 烘焙。
3. Vulkan ImGui 编辑器后端。
4. Bindless Descriptor、Upload Ring 与延迟资源释放。
5. CI 中保存经评审的每后端基线，并运行严格跨 API 门禁。
6. 在保持本阶段 Golden Image 通过的前提下增加 TAA、GTAO 和 Clustered Lighting。

新增 Metal 或其他图形后端时，应实现公共 RHI 后端并复用本阶段的场景、Shader 和 Pass，不能再复制一套高级渲染算法。
