# Hi-Z 遮挡剔除实现与学习指南

## 1. 解决的问题

视锥剔除只能删除相机视野外的物体。视野内但被墙体或大型物体完全挡住的物体仍会提交 Draw。Hi-Z 遮挡剔除用上一帧深度判断包围球是否被完全遮挡，并把不可见物体的间接绘制参数 `instanceCount` 写成 0。

## 2. 数据流

```text
上一帧 Depth
  -> HiZBuild（逐级取最大深度）
  -> 持久 Hi-Z Mip 链
  -> 下一帧 GpuVisibility
       1. 包围球视锥测试
       2. 投影为屏幕矩形
       3. 按矩形尺寸选择 Hi-Z mip
       4. 比较物体最近深度与遮挡者最远深度
  -> IndirectArguments
  -> GBuffer / Forward ExecuteIndirect
```

项目使用普通 Z：0 是近处，1 是远处。因此 `HiZ.hlsl` 每级取 `max`。只有物体包围球的最近深度仍比这个最远遮挡深度更远时，才能剔除。

## 3. 如何实现

### 公共 GPU Culling

`GpuDrivenVisibility` 新增上一帧 Hi-Z 的纹理描述符，以及 View、ViewProjection、Viewport、投影缩放、功能开关和深度 Bias。Shader 位于 `assets/shaders/GpuCulling.hlsl`：先做包围球视锥测试，再投影包围球，按屏幕覆盖尺寸选择 mip，读取四角与中心五个样本。

首帧、Resize 后或 Hi-Z 无效时，C++ 自动把 `occlusionCullingEnabled` 传为 0，不读取未初始化历史。

### RDG 生命周期

Hi-Z 从“本帧才产生”的资源改为持久 `ImportTexture`。`GpuVisibility` 显式声明读取 Hi-Z，`HiZBuild` 在几何之后写入新版本。RDG 因此能表达跨帧历史并生成正确 Barrier。

### 双 API

算法和 Shader 只写一套：

```text
GpuCulling.hlsl
  -> Slang DXIL -> D3D12 Descriptor / Dispatch
  -> Slang SPIR-V -> Vulkan DescriptorSet / Dispatch
```

D3D12 与 Vulkan 只负责把各自的 Hi-Z RHI Texture 写入公共绑定 16，不复制剔除算法。

## 4. 文件与职责

- `src/Renderer/Features/GpuDrivenVisibility.h/.cpp`：CPU 常量、描述符和 Dispatch。
- `assets/shaders/GpuCulling.hlsl`：视锥和遮挡测试。
- `src/Renderer/SharedRenderGraphFrontend.cpp`：Hi-Z 读取和间接参数写入依赖。
- `src/Renderer/D3D12SceneRenderer.cpp`：D3D12 RHI 纹理接线。
- `src/Renderer/VulkanSceneRenderer.cpp`：Vulkan RHI 纹理接线。
- `src/Renderer/RenderSettings.h`、`src/UI/DebugPanel.cpp`：运行时开关。
- `tests/ShaderCompilerTests.cpp`：DXIL/SPIR-V 与 Reflection 契约。

## 5. 权衡

- 使用上一帧 Hi-Z 可避免额外 Depth Prepass，代价是高速相机运动存在历史误差。
- 包围球和保守 Bias 会少剔除一些物体，但优先避免错误删除可见物体。
- 五点采样是功能基线；生产级版本可增加矩形覆盖保守测试、两帧可见性滞后和相机突变检测。

## 6. 验证

```powershell
cmake --build build-windows-ci --config Debug --target PrismRender PrismShaderCompilerTests PrismRenderGraphTests
.\build-windows-ci\Debug\PrismShaderCompilerTests.exe
.\build-windows-ci\Debug\PrismRenderGraphTests.exe
```

验证结果：

- 11 个 Shader 入口共 22 个 DXIL/SPIR-V 程序编译通过。
- Reflection 确认绑定 16 是 SampledTexture，32/33 是 StorageBuffer。
- RDG 测试确认 Hi-Z 在读取前已作为持久资源导入。
- D3D12/Vulkan 主程序共享同一 C++ 与 Shader 路径并编译通过。
