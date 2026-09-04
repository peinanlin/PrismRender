# PrismRender 高级图形场景

## 场景组织

高级效果采用“专项 Lab + 最终 World Showcase”的结构。专项 Lab 隔离技术依赖，便于切换参数、抓帧和定位问题；各项技术稳定后再组合到综合场景。

| 场景 | 技术 | 状态 |
| --- | --- | --- |
| Physical Atmosphere Lab | Transmittance/Sky-View LUT、Rayleigh/Mie、多次散射 | 已实现 |
| Large World / RTE Lab | 双精度绝对坐标、相机相对 GPU 坐标 | 已实现 |
| Terrain & Virtual Texture Lab | 4 × 4 大地图 Tile、GPU LOD/视锥/Hi-Z、Indirect Draw、软件 VT | 已实现 |
| Ocean Lab | 128² FFT、三环相机跟随 Clipmap、世界空间稳定采样、PBR/SSR | 已实现 |
| Capture & IBL Lab | Scene/Cube Capture、反射探针、IBL 卷积 | 计划中 |
| Geo / 3D Tiles Lab | ECEF、Tileset 遍历、误差 LOD、异步资产 | 计划中 |
| World Showcase | 大气、海洋、地形、VT、反射和大世界坐标 | 最终阶段 |

## Large World / Relative-to-Eye

场景键为 `large-world` 或 `rte`。CPU Transform 与相机使用双精度绝对坐标，提交 GPU 前先在双精度中减去相机渲染原点，再转换为 float 相对坐标。这让数千公里绝对坐标处的亚米级物体间距仍能稳定显示。

## Terrain & Virtual Texture

场景键为 `terrain-vt`。4096 m 山地被划分为 4 × 4 根 Tile，每个根块有四个子块，共 80 个候选 Patch。GPU Compute 根据父子 LOD、投影尺寸、视锥和上一帧 Hi-Z 生成 Indirect Draw。`Scene` 使用可移动 Editor Camera 和 16 个根 Tile 编辑器代理，并用细黑 `LineList` 显示 Game Camera 视锥；`Final Output` 使用 Game Camera 和完整 GPU 四叉树。两个视图具有独立颜色、Depth 与 Hi-Z 资源，因此移动编辑相机不会改变最终画面或剔除结果。Tile 的橙色/青色边界用于观察根块与子块选择，可从 `Terrain Tile Boundaries` 关闭。

高度生成使用可按世界坐标独立求值的 Advanced Terrain Erosion Filter。基础高度函数先输出高度与解析坡度；四层 Phacelle 方向噪声沿最陡下降方向叠加沟壑，并通过 stacked fading、部分归一化和 straight-gully 伪坡度让小沟壑从大沟壑分叉。生成器同时输出高度、坡度与 ridge map：高度和坡度用于 Patch 顶点/法线，ridge map 经顶点属性插值后为材质增加沟底湿痕和山脊岩色。相同世界坐标的结果与 Tile、LOD 无关，因此相邻 Patch 不需要交换侵蚀状态。

这是一种视觉侵蚀滤镜，不模拟水量、泥沙守恒或完整河网。当前 Demo 在 CPU 创建 33 × 33 Patch；生成器保留 1～8 层参数，未来可迁移到 Compute Shader 或 GPU 位移路径以增加近景高频细节，而无需改动四叉树和 VT 所有权边界。

VT 使用软件页表和 LRU Atlas，D3D12/Vulkan 共用。Reserved Resource/Sparse Image 仍应作为 RHI 后端优化加入，不能把当前实现描述成硬件稀疏资源。

## FFT Ocean

场景键为 `ocean`。海面由 Inner、Middle、Outer 三个相机跟随环带组成，最远覆盖 3.2 km。相机 XZ 按 2.5 m FFT texel 对齐，Shader 用世界空间原点生成稳定 UV。这样右键旋转或 WASD 移动不会触及固定海面边界，同时近处保持高网格密度、远处降低密度。

FFT Compute 链每帧生成位移、法线和泡沫纹理，然后由三个环带共同采样。水面继续进入延迟 PBR、IBL 和 SSR 路径。

## 物理大气亮度

Environment 或 Debug Panel 提供 `Atmosphere Brightness`。推荐 1.3–1.8；它仅影响物理天空。`Exposure` 影响整个 HDR 画面，推荐在天空倍率调好后再小幅调整。三个相关 Lab 的默认天空倍率已提高。

## 启动命令

```powershell
cd D:\unity_project\PrismRender
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
.\tools\RunRendererLab.ps1 -Scene atmosphere -Api d3d12 -Build
.\tools\RunRendererLab.ps1 -Scene terrain-vt -Api d3d12
.\tools\RunRendererLab.ps1 -Scene ocean -Api d3d12
```

Vulkan 验证时把 `d3d12` 改为 `vulkan`。
