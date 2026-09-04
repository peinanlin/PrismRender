# Position Based Fluids Lab 实施说明

## 目标与范围

Fluid Lab 在 PrismRender 现有 RHI、RenderGraph、FeatureRegistry 和 ImGui 架构上实现了一个可运行的 GPU Position Based Fluids 演示。它包含：

- 32,768 粒子的 GPU PBF 模拟；
- 基于均匀网格的邻居查询；
- 屏幕空间流体表面、厚度、法线和最终合成；
- Beer-Lambert 吸收、折射、环境反射；
- 卡通分段、轮廓、泡沫和调试视图；
- 图像空间焦散生成与双向高斯模糊；
- D3D12/Vulkan 共用的 Shader 和 RenderGraph 调度；
- ImGui 参数面板、GPU Pass 名称和模拟诊断统计。

该实现复用了 PrismRender 的跨后端抽象，没有引入只服务于流体的 D3D12 专用路径。

## 每帧数据流

```text
固定时间步 / 子步进
  -> Predict（重力、外力、速度积分）
  -> ClearGrid + BuildGrid（固定容量均匀网格）
  -> Lambda -> Delta -> Apply/Boundary，重复约束投影
  -> Velocity Update -> Vorticity -> XSPH
  -> StructuredBuffer<float4> particle positions
  -> Particle sphere impostor depth/mask + additive thickness
  -> Bilateral depth ping-pong
  -> View-space normal + foam mask
  -> Image-space caustics + horizontal/vertical blur
  -> Beer-Lambert / refraction / Fresnel / toon composite
  -> TAA（场景配置允许时）-> Bloom -> Tonemap
```

PBF 各 Compute 阶段作为独立 RenderGraph Pass 声明 SRV/UAV 状态，资源屏障由 RenderGraph 统一生成。首版使用 Graphics Queue 调度，避免同帧图形消费粒子位置时引入额外的跨队列同步。

## 关键模块

- `src/Renderer/Features/Fluid/PbfFluidSimulation.*`：模拟资源、常量、调度、重置和延迟 readback。
- `src/Renderer/Features/Fluid/ScreenSpaceFluidRenderer.*`：粒子表面、滤波、法线、泡沫和最终 HDR 合成。
- `src/Renderer/Features/Fluid/FluidCaustics.*`：焦散生成与模糊。
- `src/Renderer/Features/Fluid/FluidFeature.*`：三个子模块的所有权和 RenderGraph 接线。
- `src/Renderer/Features/Fluid/FluidSettings.h`：运行时配置。
- `src/Renderer/Features/Fluid/FluidStatistics.h`：粒子、网格、Dispatch、溢出和非法粒子统计。
- `src/UI/FluidLabPanel.*`：交互参数和诊断面板。
- `src/Scene/FluidLabSceneFactory.*`：与默认 PBF AABB 对齐的开放式水槽场景。
- `assets/shaders/Fluid/`：PBF、表面重建、泡沫、合成与焦散 Shader。

## 邻居结构的取舍

没有照搬 `particleCount × 256` 的显式邻居表。默认 294,912 粒子时，该表仅索引就约占 288 MiB，而且容易超过 Vulkan 设备的单 Storage Buffer 范围。Fluid Lab 使用固定容量的 dense grid bucket，在约束 Shader 中直接扫描相邻 27 个网格：

- 默认每格最多 64 个粒子；
- 超出容量的粒子会增加 overflow 诊断计数；
- 默认 32K 粒子以保证 D3D12/Vulkan 的保守兼容性；
- 增大粒子数、容器、平滑半径或 bucket 容量会触发持久 GPU 资源重建。

## 运行与交互

```powershell
cd D:\unity_project\PrismRender
.\tools\RunRendererLab.ps1 -Scene pbf -Api d3d12 -Build
.\tools\RunRendererLab.ps1 -Scene fluid-render -Api d3d12
.\tools\RunRendererLab.ps1 -Scene fluid-caustics -Api d3d12
.\tools\RunRendererLab.ps1 -Scene fluid-toon -Api vulkan
```

也可以设置 `PRISM_RENDER_DEMO_SCENE=fluid` 后直接运行 Renderer。Fluid Lab 场景默认打开流体，其余场景默认关闭。

- `J / L`：沿 `-X / +X` 施加外部加速度；
- `K / I`：沿 `-Y / +Y` 施加外部加速度；
- `U / O`：沿 `-Z / +Z` 施加外部加速度；
- Debug Panel：暂停、重置、约束迭代、子步进、黏性、涡旋、表面参数、卡通、泡沫、焦散及调试视图。

## 验证建议

1. 编译 `PrismRenderer` 和 `PrismEditor`。
2. 运行 `ShaderCompiler` 测试，确认所有 Fluid entry 同时通过 DXIL 与 SPIR-V。
3. 运行全量 CTest。
4. 分别以 D3D12 和 Vulkan 启动 `fluid` 场景至少数帧。
5. 观察 `Grid Overflow` 与 `Invalid Particles`；两者应保持为 0。
6. 切换 Depth、Thickness、Normals、Foam、Particle Mask 调试模式检查中间资源。

## 当前边界

- 碰撞体是轴对齐水槽边界，尚未实现任意场景 Mesh/SDF 碰撞；
- bucket 溢出时会舍弃超出容量的邻居；
- 网格每个 substep 构建一次，不会在每轮约束投影后重建；
- 焦散和泡沫是屏幕空间近似，不是完整 Photon Mapping 或气液两相模拟；
- 尚未实现与其他透明物体的 OIT、流体 motion vector 和 TAA reactive mask；流体开启时会自动绕过 TAA；
- GPU 原子插入顺序不同，因此 D3D12/Vulkan 结果按统计和图像容差验收，而不是逐位一致。
