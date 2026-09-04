# Terrain VT 与 FFT Ocean 实施说明

## 大地图 Tile

`terrain-vt` 不再用单个 Cube 或单个根地形演示。场景生成一张 4096 m × 4096 m 的程序化山地地图，并把地图划分为 4 × 4 个根 Tile。每个根 Tile 可继续选择 2 × 2 子 Tile，因此一帧共有 16 个根节点和 64 个子节点、共 80 个 GPU 候选 Patch。

```text
4096 m procedural height field
  -> 4 x 4 root map tiles
  -> root/child projected-size LOD selection
  -> frustum + optional Hi-Z culling
  -> GPU indirect draw arguments
  -> visible terrain patches
```

每个 Tile 使用 33 × 33 顶点网格。`GpuCulling.hlsl` 根据父子关系、投影尺寸和视锥选择根块或子块，再沿用 Indirect Draw 路径输出可见地形。Debug Panel 中的 `Terrain Tile Boundaries` 会把根块边界显示为橙色、子块边界显示为青色；`Terrain Tile Border` 控制边界宽度。关闭该开关即可查看正常材质。

### Scene View / Game View 双相机剔除演示

Terrain Lab 使用两台互相独立的相机：

```text
Editor Camera（Scene View 中鼠标/WASD 控制）
  -> Scene View 独立颜色与深度目标
  -> 16 个根 Tile 编辑器代理
  -> 游戏相机细黑线视锥

Game Camera（Final Output）
  -> FrameConstants / GBuffer / Sky / 最终画面
  -> GPU 四叉树投影尺寸 LOD
  -> 六平面视锥测试
  -> 上一帧 Game Camera 深度的 Hi-Z 测试
  -> Indirect Draw instanceCount
```

编辑器现在与 Unity/UE 的常见工作流一致：`Scene` 页签由 Editor Camera 渲染，`Final Output` 页签由 Game Camera 渲染。移动 Scene View 不会改变 Game View 的画面、LOD 或剔除结果。Game Camera 的 12 条视锥边使用真正的 `LineList` 绘制为细黑线，并标记为 `editorOnly`，因此不会出现在最终画面或阴影中。`Game Camera Output` 可以临时让 Final Output 改用 Editor Camera，但仍会过滤编辑器辅助对象。

两个视图由独立 `SceneRenderer` 实例和独立离屏 Depth/Hi-Z 资源渲染。Game View 的 Hi-Z 始终来自上一帧 Game Camera 深度，不再混用 Editor Camera 深度。Scene View 将完整四叉树折叠为 16 个根 Tile 代理并关闭 GPU Driven，以避免为编辑器重复创建全部运行时描述符；Game View 仍消费完整 80 个候选 Patch。

虚拟纹理仍使用跨 D3D12/Vulkan 的软件页表方案：16 × 16 虚拟页、8 × 8 物理 Atlas，CPU 根据相机位置请求 7 × 7 邻域并用 LRU 决定驻留。它具备虚拟地址、物理页、按需驻留、淘汰和 Shader 映射，但不是 D3D12 Reserved Resource 或 Vulkan Sparse Image。

## 相机跟随 Ocean Clipmap

`ocean` 每帧继续执行完整的 128 × 128 FFT 链：

```text
Phillips + Gaussian spectrum
  -> time spectrum
  -> 7 horizontal butterfly stages
  -> 7 vertical butterfly stages
  -> displacement + normal/Jacobian foam
```

水面不再是一张固定在场景原点的有限平面，而是三个以相机为中心的方形 Clipmap 环带：

| 环带 | 覆盖范围 | 网格 |
| --- | ---: | ---: |
| Inner | 0–270 m | 129 × 129 |
| Middle | 250–1050 m | 129 × 129，中心挖空 |
| Outer | 1000–3200 m | 129 × 129，中心挖空 |

相邻环带保留少量重叠并使用很小的高度偏移，避免不同分辨率边缘产生 T-junction 裂缝。相机 XZ 位置以 `Ocean Patch Length / FFT Resolution` 为步长对齐；默认是 `320 / 128 = 2.5 m`。网格跟随对齐后的相机位置，而 Shader 使用对齐后的世界原点计算 FFT UV，因此相机移动时波形不会在网格上滑动。

```text
camera absolute XZ
  -> snap to 2.5 m FFT texel grid
  -> move Inner/Middle/Outer rings
  -> derive stable world-space FFT UV
  -> vertex displacement
  -> PBR + IBL + SSR water
```

`Ocean Camera Follow` 可用于对比固定海面和相机跟随模式。Clipmap 模式覆盖 3.2 km 半径，场景远裁剪面为 2.8 km，所以旋转或移动相机不会再因为走到原有限平面的边缘而失去海面。

## 大气亮度

`Atmosphere Brightness` 只缩放物理大气 Sky-View LUT 的 HDR 结果；它不会改变地形或模型材质。Lab 默认值已从 1.0 提高到 1.35（大气专项场景为 1.45）。建议先调到 1.3–1.8。

`Exposure` 会同时放大天空和整个 HDR 场景，建议在大气亮度合适后再以 1.1–1.5 小幅调整。太阳方向光强度影响太阳光盘和受光物体；Mie、Rayleigh 和多次散射参数主要控制散射形态，不适合作为单纯的亮度旋钮。

## 运行

```powershell
cd D:\unity_project\PrismRender
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
.\tools\RunRendererLab.ps1 -Scene terrain-vt -Api d3d12 -Build
.\tools\RunRendererLab.ps1 -Scene ocean -Api d3d12
.\tools\RunRendererLab.ps1 -Scene terrain-vt -Api vulkan
.\tools\RunRendererLab.ps1 -Scene ocean -Api vulkan
```

关键实现文件：

- `src/Scene/TerrainOceanSceneFactory.cpp`：大地图 Tile、地形 Patch 与三环 Ocean Clipmap 网格。
- `src/Renderer/SceneRenderer.cpp`：GPU Feature 常量、海洋网格对齐和世界空间采样原点。
- `assets/shaders/GpuCulling.hlsl`：地形 LOD、视锥/Hi-Z 和 Indirect Draw。
- `assets/shaders/FftOcean.hlsl`：频谱、Butterfly、位移、法线和泡沫。
- `assets/shaders/Mesh.hlsl`：Tile 可视化、VT 采样、海洋顶点位移与水面材质。
- `assets/shaders/PostProcess.hlsl`：独立的大气亮度倍率。
- `src/Renderer/Features/VirtualTextureCache.*`：页表、物理 Atlas 和 LRU 驻留。
- `src/Renderer/Features/FftOcean.*`：FFT 资源与 Compute 调度。
