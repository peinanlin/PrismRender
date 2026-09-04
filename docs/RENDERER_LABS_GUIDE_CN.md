# PrismRender 渲染实验场景

## 启动

Debug 构建完成后，可直接运行：

```powershell
.\tools\RunRendererLab.ps1 -Scene reflections -Api d3d12
```

可用场景键：`reflections`、`shadows`、`lights`、`gpu-driven`、
`post-process`、`render-graph`、`materials`、`streaming`、`atmosphere`、
`large-world`、`terrain-vt`、`ocean`。程序运行后也可在
Debug Panel 的 `Demo Scene` 下拉框中切换；Asset Streaming 最好以
`-Scene streaming` 启动，使异步管理器从初始化阶段开始工作。

## 各 Lab 的观察方法

### Reflection Lab

- `IBL` 是所有未命中区域的稳定回退。
- `Screen Space Reflections` 展示屏幕内高频反射。
- `Planar Reflections` 启用半分辨率镜像相机 pass；地板区域优先使用它，
  SSR 自动降权。
- `Reflection Plane Y` 必须与镜面顶面一致，内置场景为 `0.0`。

### Shadow Lab

`Shadow Filter` 可切换 Hard、PCF 3x3、PCF 5x5、PCSS、VSM 和 EVSM。
PCSS 包含 blocker search 与接触硬化半影；VSM/EVSM 使用 1024² 三层
moments、横纵 compute blur 和概率上界测试。`Cascade Debug Tint` 用
红/绿/蓝显示三个 CSM 区间。

### Forward+ Lab

该场景默认使用 Forward+。保持 `Clustered Lighting` 开启，在
`Deferred Rendering` 开关两侧对比时，两条路径都读取同一份
16x16 tile、16 个对数深度 slice 的 light list。关闭
`Forward+ (when Forward)` 后会回到仅 4 个点光源的兼容路径。

### GPU Driven Stress Lab

场景包含 120 个重复对象和两面遮挡墙。组合切换 `Frustum Culling`、
`Hi-Z Occlusion Culling`、`GPU Instancing` 与 `GPU Driven`，观察
Visible/Culled Objects、Draw Calls、Indirect Draw 和 GPU 时间。

### Post Process Lab

发光体按强度递增。依次切换 HDR、Bloom、Tonemapping、Temporal AA、
GTAO，并调整 Exposure、Bloom Threshold 和 Bloom Intensity，能够得到
稳定的 A/B 结果。

### RenderGraph Lab

此场景同时启用 Shadow、Cluster、Geometry、GTAO、SSR、TAA、Bloom 和
Tonemap。可用环境变量比较调度：

```powershell
$env:PRISM_RENDER_RDG_QUEUE_MODE='serial'    # 或 native / automatic
.\tools\RunRendererLab.ps1 -Scene render-graph
```

Debug Panel 的 GPU Profiling 与 RenderGraph 报告用于检查跨队列同步和
时间线；GBuffer/HDR/Bloom 来自 transient texture pool，可用于观察
瞬态资源复用。

### Material Lab

5x5 球体矩阵中，行方向增加 metallic，列方向增加 roughness。切换 PBR、
IBL、Normal Mapping、Occlusion 和 Emissive，可分离材质输入的贡献。

### Asset Streaming Lab

以该场景启动会自动开启现有 Cooked Asset 异步 IO、上传 ticket、显存预算
和 LRU 淘汰路径。Debug Panel 显示 Queued/Resident/Evicted/Failed、驻留
MiB、完成上传数和淘汰数。可限制预算：

```powershell
$env:PRISM_RENDER_ASSET_STREAMING_BUDGET_MB='64'
.\tools\RunRendererLab.ps1 -Scene streaming
```

### Terrain & Virtual Texture Lab

该场景显示真实高度场山谷、山脊和雪线，不使用 Cube 代替地形。GPU Driven
Compute 同时完成四叉树屏幕尺寸 LOD、父子叶节点选择、视锥/Hi-Z 剔除与
Indirect Draw。`Virtual Terrain` 和 `Terrain Virtual Texture` 可独立切换；
统计区显示物理页驻留、请求、命中、缺页和 LRU 淘汰数量。

### FFT Ocean Lab

128x128 Phillips 频谱经过 14 个 Butterfly 阶段生成位移、法线和 Jacobian
泡沫。可调整 `Ocean Wind Speed`、`Ocean Spectrum` 和 `Ocean Choppiness`。
水面使用 PBR、IBL 与 SSR，场景中的岩石用于观察水位、波峰和反射变化。

## 数据流

```text
DemoSceneCatalog -> Scene Factory -> RenderScene
RenderSettings -> SharedFrameConstants / Lab-specific constants
RenderScene -> Shadow / Cluster / Planar / Geometry passes
RenderGraph -> GTAO / SSR / TAA / Bloom / Tonemap -> Scene Color
Debug Panel <- RendererStatistics / GPU timeline / Streaming statistics
```

新增功能保持在 `Renderer/` 与 `Scene/` 模块中，`main.cpp` 不包含场景或
渲染逻辑。D3D12 与 Vulkan 共用场景、HLSL/Slang shader 和 RenderGraph
frontend。
