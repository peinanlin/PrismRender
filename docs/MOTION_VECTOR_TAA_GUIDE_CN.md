# Motion Vector 与 TAA 实现和学习指南

## 1. 这两个技术解决什么问题

普通抗锯齿只看当前帧，很难同时保留细线、远处物体和缓慢移动的亚像素细节。
TAA（Temporal Anti-Aliasing，时间抗锯齿）会复用上一帧已经计算过的颜色，相当于
让多帧共同为当前像素提供采样。

复用历史前必须知道“当前像素在上一帧位于哪里”。Motion Vector（运动向量）
记录的就是当前像素相对上一帧的屏幕位移。没有运动向量，移动物体会直接拖出残影。

## 2. 数据流

```text
Camera Halton Jitter
  -> 当前帧 ViewProjection

上一帧 Object WorldViewProjection ----+
                                      +-> GBuffer Vertex/Pixel
当前帧 Object WorldViewProjection ----+     -> MotionVectors(RG16F)

当前 HDR + MotionVectors + HistoryRead
  -> TemporalResolve Compute
       1. 用运动向量反投影到上一帧
       2. 读取历史颜色
       3. 用当前帧 3x3 邻域限制历史颜色
       4. 按速度降低历史权重
  -> TemporalResolved(RGBA16F)
  -> HistoryWrite(RGBA16F)
  -> Bloom -> Tonemap

帧结束：交换 HistoryRead / HistoryWrite
```

## 3. Motion Vector 如何实现

### 3.1 保存两套变换

`SharedObjectConstants` 同时上传：

- 当前帧 `worldViewProjection`
- 上一帧 `previousWorldViewProjection`

普通绘制和 GPU Instancing 都使用相同数据约定。实例输入新增
`INSTANCEPREVWVP0..3`，所以共享 GBuffer Shader 不需要为 D3D12 和 Vulkan
维护两套算法。

### 3.2 在 GBuffer 写运动向量

顶点着色器输出当前和上一帧的 Clip Position。像素着色器完成透视除法并转换为
UV，然后写出：

```text
motion = currentUv - previousUv
```

Motion Vector 使用 `RG16Float`。两个通道足够保存屏幕二维位移，而且比
`RGBA16Float` 节省一半带宽。

### 3.3 相机抖动

`Camera::SetProjectionJitterNdc()` 把 Halton 2/3 序列写入投影矩阵。连续帧在
同一像素内部选择不同采样位置，TAA 累积后才能获得真正的亚像素信息。

本项目使用 8 个样本循环。Resize、历史重置或首帧时不读取未初始化历史。

## 4. TAA 如何实现

### 4.1 公共计算模块

`TemporalAntiAliasing` 负责：

- 创建 Motion Vector Render Target
- 创建 Temporal Resolved 输出
- 创建两张 History Texture
- 为每个飞行帧维护常量 Buffer 和 DescriptorSet
- 计算 Halton Jitter
- 交换历史读写纹理

接口只使用公共 RHI。D3D12 编译 DXIL，Vulkan 编译 SPIR-V，两个后端执行同一
`TemporalAA.hlsl`。

### 4.2 历史重投影

Shader 对每个像素计算：

```text
previousUv = currentUv - motion
```

若坐标仍在屏幕内，就从 `HistoryRead` 读取上一帧颜色。运动越快，历史权重越低，
从而减少移动边缘的拖影。

### 4.3 邻域 Clamp

历史颜色可能来自已经消失的物体。Shader 统计当前颜色 3x3 邻域的最小值和最大值，
并把历史颜色限制在这个范围内。这是当前实现抑制 Ghosting 的主要手段。

### 4.4 双缓冲历史

同一帧不能一边读取历史纹理、一边覆盖它，因此使用两张纹理：

```text
Frame N:   History[0] -> Read,  History[1] -> Write
Frame N+1: History[1] -> Read,  History[0] -> Write
```

`EndFrame()` 只在 RDG 执行成功后交换索引。

## 5. RDG 如何表达

公共 RenderGraph 新增：

- `MotionVectors`
- `TemporalResolved`
- `TemporalHistoryRead`
- `TemporalHistoryWrite`
- `TemporalResolve` Compute Pass

`GBuffer` 写 Motion Vector；`TemporalResolve` 读取 HDR、Motion 和历史，写
Resolved 与新历史；Bloom 和 Tonemap 改为读取 Resolved。RDG 据此生成资源状态
转换和 Graphics/Compute Queue 同步。

历史纹理是跨帧资源，因此使用 Import，而不是帧内 Transient Texture。

## 6. D3D12 生命周期修复

多帧验证暴露出一个原有 GPU Driven 问题：命令上下文适配器每帧创建并销毁
`ID3D12CommandSignature`，但 GPU 可能仍在执行 `ExecuteIndirect`。

现在 Command Signature 由 `D3D12Context` 按 stride 缓存，生命周期与设备一致，
并使用互斥锁兼容并行命令录制。这个修复不是 TAA 算法的一部分，但它是可靠跨帧
渲染的必要条件。

## 7. 主要文件

- `src/Renderer/Features/TemporalAntiAliasing.h/.cpp`：TAA 公共模块和资源所有权
- `assets/shaders/TemporalAA.hlsl`：历史重投影、Clamp 和混合
- `assets/shaders/Mesh.hlsl`：当前/上一帧位置与 Motion Vector 输出
- `src/Scene/Camera.h/.cpp`：投影抖动
- `src/Renderer/SharedRenderGraphFrontend.h/.cpp`：RDG 资源和 Pass
- `src/Renderer/D3D12SceneRenderer.cpp`：D3D12 公共 RHI 接线
- `src/Renderer/VulkanSceneRenderer.cpp`：Vulkan 公共 RHI 接线
- `src/RHI/D3D12/D3D12Context.h/.cpp`：持久 Command Signature 缓存
- `tests/ShaderCompilerTests.cpp`：双目标 Shader 与 Reflection 契约
- `tests/RenderGraphTests.cpp`：资源依赖、版本和 Pass 数量

## 8. 设计权衡

- 当前使用简单 RGB Min/Max Clamp，容易理解；生产版本可升级到 YCoCg、Variance
  Clip 和 Reactive Mask。
- 当前没有 Skinned Mesh，因此只记录刚体 Object Transform；加入骨骼动画后还需
  保存上一帧骨骼矩阵。
- 当前 TAA 输出与渲染分辨率相同；TAAU/TSR 还需要输出分辨率、重建滤波和更完整
  的遮挡显露处理。
- TAA 开关关闭时仍执行 Copy-like Resolve，以保持后续 Bloom/Tonemap 的 RDG
  输入拓扑稳定。

## 9. 验证

```powershell
cmake --build build-windows-ci --config Debug --target PrismRender PrismHarness PrismShaderCompilerTests PrismRenderGraphTests
.\build-windows-ci\Debug\PrismShaderCompilerTests.exe
.\build-windows-ci\Debug\PrismRenderGraphTests.exe
.\build-windows-ci\Debug\PrismHarness.exe --commands examples\harness\gpu_driven_validation.jsonl --output automation\reports\stage-hiz-taa-validation.jsonl
```

本阶段验证结果：

- 12 个 Shader 入口共 24 个 DXIL/SPIR-V 程序通过编译与 Reflection 检查。
- RenderGraph 测试确认 Temporal Resolve 的读写依赖和版本数量。
- D3D12 GPU Driven + 原生多队列连续运行 16 帧，退出码为 0。
- Harness 的 D3D12 RDG、Vulkan RDG 和跨 API 对比全部成功。

## 10. 推荐阅读顺序

1. 先看本页第 2 节的数据流。
2. 阅读 `Camera::SetProjectionJitterNdc()`。
3. 阅读 `Mesh.hlsl` 的当前/上一帧 Clip Position。
4. 阅读 `TemporalAA.hlsl` 的重投影和 Clamp。
5. 阅读 `TemporalAntiAliasing::Resize/Update/EndFrame()` 的资源生命周期。
6. 最后看 `SharedRenderGraphFrontend` 如何让 RDG 自动安排状态和队列同步。
