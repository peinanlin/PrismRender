# HPWater Ocean Lab

## 目标与运行

`hpwater-ocean` 在 PrismRender 中复用 WaveWorks Lab 的四级 Spectral FFT、局部波动、泡沫和自适应海面几何，另建屏幕空间水光学管线。不是把远处换成 FFT、近处换成另一张水面：整片海面共享连续的 FFT 位移，近、中、远距离只影响光学成本与局部效果权重。

在仓库根目录运行：

```powershell
.\build-windows-ci\RelWithDebInfo\PrismRender.exe --api=d3d12 --scene=hpwater-ocean
.\build-windows-ci\RelWithDebInfo\PrismRender.exe --api=vulkan --scene=hpwater-ocean
```

原来的 `--scene=waveworks-ocean` 和 `--scene=ocean` 入口保留，不调度 WaterOptics pass。旧 Lab 与新 Lab 的默认相机、几何、光照和 Spectral 设置相同；命名测试视角会为两者同时增加棋盘接收面、水下方块和穿水柱，方便检查遮挡与折射。

## 数据流与责任边界

```text
共享 Spectral FFT + foam + local waves（每帧只运行一次）
                      ↓
Opaque GBuffer / Lighting → 原始 OpaqueDepth / Hi-Z / HDR
                      ↓
Water DepthCopy → Water Visibility（3 张水 GBuffer + water motion）
                      ↓
Caustics / Refraction → Underwater Accumulate → Temporal → Bilateral
                      ↓
Water Composite → Publish HDR + CompositeDepth + water motion
                      ↓
SSR / transparency / TAA / Bloom / Tonemap
```

- `Renderer/Features/Ocean/WaterOpticsFeature` 拥有水专用 PSO、描述符、每帧常量、scratch/history、RenderGraph 子图及延迟释放队列；不拥有或重复 FFT。
- `WaterMediumState` 管理水上/水下分类与迟滞；使用相机 XZ 的异步 GPU 水面高度查询，查询不可用、过期或相机跳跃时回退平均海平面。
- `WaterVolumetricHistory` 将相机切换、介质转换、尺寸、质量、光学与海面历史版本纳入 key；写入完成后才提交有效历史。
- `WaterCoverageReadback` 在 GPU 按 tile 统计真实水像素覆盖率，使用完成 fence 对应的 readback；不是用相机朝向估算。
- `WaterBenchmarkReport` 汇总有效模式、分辨率、历史、覆盖率、draw/dispatch、CPU recording 和 GPU 时间，供 UI 与机器可读报告使用。

水可见性写入 normal/roughness、absorption/foam、scattering/mask；IOR 在光学常量中。原始 opaque 深度不被水覆盖，透明物体与后处理使用独立 CompositeDepth。折射只读取 opaque 场景，避免把水自身反复折射。深度先通过逆 VP 重建为米制线性深度，不能直接拿 device-depth 差计算 Beer-Lambert 衰减；折射输出 alpha 的绝对值保留厚度，负号标记 fallback。

## 控制与质量

HPWater 面板保留共享 FFT/local 控制，并新增 Material、Refraction、Caustics、Volumetrics、Distance、Quality、History Reset 与 Debug View 分组。

- 光学编辑只使光学历史失效，不重建 H0、不清空局部波动；显式 local/full reset 仍走原模拟生命周期。
- Normal/High/Extreme 是光学质量，独立于 Spectral FFT 质量。UI 显示有效纹理尺寸和 sample 数。
- 高精度折射可关闭；远处平滑退回有界近似折射与反射/吸收。FFT 几何不因光学远距策略消失。
- RGB 焦散仅在 High/Extreme 生效，Normal 请求 RGB 时明确显示单通道回退。
- 体积光只在水下执行低分辨率积分、运动/深度拒绝、ping-pong 历史与全分辨率 joint bilateral 重建。
- Debug 包括水 mask、深度、法线/粗糙度、吸收、散射、泡沫、厚度、折射命中、距离分层、焦散能量/级联、体积光和历史拒绝；切换 debug 不重置模拟或光学历史。

## 可复现验证

```powershell
# 单次确定性截图、JSON 报告与日志
.\scripts\Capture-HpWater.ps1 -Api d3d12 -Camera underwater -RayMarch -Caustics rgb
# 连续切换、reset、resize、穿水线、场景切换和全部 debug
.\scripts\Validate-HpWater.ps1 -Mode lifecycle
# 七组原水面 / HPWater 等输入对照，各跑两个后端
.\scripts\Validate-HpWater.ps1 -Mode views
# 两种分辨率 × 三档光学质量 × 两个后端
.\scripts\Validate-HpWater.ps1 -Mode benchmark
# 前景/水线运动、历史拒绝、焦散级联及 calm/strong-wind 泡沫诊断
.\scripts\Validate-HpWater.ps1 -Mode details -Apis d3d12
# 汇总性能并以固定阈值检查双后端图像差异
.\scripts\Summarize-HpWater.ps1 -EnforceParity
ctest --test-dir build-windows-ci -C RelWithDebInfo --output-on-failure -j4 -LE gpu
ctest --test-dir build-windows-ci -C RelWithDebInfo --output-on-failure -j1 -L gpu
```

所有输出位于 `artifacts/hpwater-validation/`。截图默认第 30 帧；模拟使用固定 1/60 秒输入。GPU 时间是预热后最近已完成帧，并非跨多次运行的统计平均或 FPS。批量脚本串行运行 GPU 程序，避免互相抢占影响比较。`-Sequence -Frames N` 可在自动操作序列指定帧取证。

生命周期矩阵强制 `-QueueMode native`，覆盖独立 graphics/compute 队列，避免自动成本模型选用串行时漏掉交接错误。普通截图使用默认 auto；也可显式指定 `native` 或 `serial`。近景、侧向折射和俯视焦散使用同一组接收物；泡沫对照使用双方一致的 wake 模拟 preset。诊断接收物采用独立双面索引网格，避免水下视角受通用单面网格剔除方向干扰。

机器可读报告保留通用 GPU timing schema，并在 `capture` 字段附带本次水面元数据。`waterMemoryMiB` 表示当前水光学 transient 纹理池的物理分配，不包含共享 FFT、驱动开销及短期退役资源峰值；不应把它解释成进程总显存。

Vulkan validation 需要 Khronos 校验层。测试脚本优先使用项目内 `artifacts/vulkan-validation-tools/sdk/Bin`，仅对本次子进程设置 `VK_LAYER_PATH`。校验 SDK 使用 [LunarG 官方 copy-only 安装方式](https://vulkan.lunarg.com/doc/view/latest/windows/getting_started.html)，不要求修改系统 PATH。测试时发现并修复的呈现信号量复用方式遵循 [Khronos 的每交换链图像方案](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html)。

## 主要文件

相对于仓库根目录，完整实现位于以下源文件，而非 `main.cpp`：

| 路径 | 职责 |
| --- | --- |
| `src/Renderer/Features/Ocean/WaterOpticsFeature.{h,cpp}` | 管线、资源、子图、退休与发布 |
| `src/Renderer/Features/Ocean/WaterOpticsSettings.{h,cpp}` | 参数验证、质量、dirty scopes |
| `src/Renderer/Features/Ocean/WaterOpticsTransientLayout.{h,cpp}` | 格式、extent 与 transient 布局 |
| `src/Renderer/Features/Ocean/WaterOpticsHistory.{h,cpp}` | 光学历史 key |
| `src/Renderer/Features/Ocean/WaterVolumetricHistory.{h,cpp}` | 体积历史提交与拒绝 |
| `src/Renderer/Features/Ocean/WaterMediumState.{h,cpp}` | 相机介质迟滞与回退 |
| `src/Renderer/Features/Ocean/WaterCaustics.{h,cpp}` | 相机中心级联和有界能量模型 |
| `src/Renderer/Features/Ocean/WaterOpticalModel.{h,cpp}` | CPU 解析光学参考 |
| `src/Renderer/Features/Ocean/WaterRefractionModel.{h,cpp}` | CPU 折射验证 |
| `src/Renderer/Features/Ocean/WaterCoverageReadback.{h,cpp}` | 覆盖率统计和生命周期 |
| `src/Renderer/Features/Ocean/WaterBenchmarkReport.{h,cpp}` | 机器可读诊断 |
| `assets/shaders/Ocean/WaterOptics.slang` | 可见性、折射、BSDF、合成与调试 |
| `assets/shaders/Ocean/WaterCaustics.slang` | 焦散生成与解析 fixture |
| `assets/shaders/Ocean/WaterVolumetrics.slang` | 水下累积、时域和重建 |
| `assets/shaders/Ocean/WaterCoverage.slang` | GPU 水像素归约 |
| `src/UI/WaterOpticsPanel.{h,cpp}` | Lab 控件和有效模式反馈 |
| `src/Core/Application/WaterValidationSequence.cpp` | opt-in 连续操作验收驱动 |
| `scripts/Capture-HpWater.ps1`, `scripts/Validate-HpWater.ps1`, `scripts/Summarize-HpWater.ps1` | 可复现截图、验证矩阵与性能/图像误差汇总 |

集成修改包括 SceneRenderer、共享 RenderGraph、Ocean GPU Query、LocalWave GPU 资源、OceanLabPanel、场景目录/工厂、GPU timing 报告、CMake 和测试。双后端联调还修复了原有纹理状态、shader 输入接口、Vulkan 设备能力和交换链同步问题；不引入后端专属的水算法 shader。

## 边界与来源

这是项目自有的 HPWater-style 实时近似，不是 Unity 工程搬运。参考技术来源：[HPWater 项目](https://github.com/AshenOneArt/HPWater) 与[作者文章](https://zhuanlan.zhihu.com/p/2010582216581862517)。光学方程、Slang shader、RHI 集成和测试为 PrismRender 内独立实现；不依赖原项目未提供的 Unity 场景或运行时。

- 折射是屏幕空间方法：离屏、前景交叉或无有效命中时有界回退，不包含屏幕外几何。
- 焦散是以代表性浅水接收面生成的有界聚焦近似，再在有效水下 receiver 应用深度衰减；不是完整光子追踪或严格能量守恒的光线路径求解。
- 体积光使用均匀介质、主方向光和现有阴影；不模拟多次散射、任意封闭水体或海底多层介质。
- 当前海平面约定为世界 Y=0，局部域有限；大范围海面来自 FFT 级联，不来自无限范围局部波动方程。
- 异步介质查询有延迟，UI 明示 fallback；水线使用迟滞抑制噪声，不能解释为逐像素多介质路径追踪。

## 验收结果

OpenSpec `add-hpwater-ocean-optics-lab` 的 38 项任务已完成，未自动归档。以下是 2026-08-28 本机验收记录。

### 等输入图像索引（D3D12）

| 视图 | 原 WaveWorks 光学 | HPWater 光学 |
| --- | --- | --- |
| 近景 | [原水面](../artifacts/hpwater-validation/compare_waveworks-ocean_d3d12_near.bmp) | [新水面](../artifacts/hpwater-validation/compare_hpwater-ocean_d3d12_near.bmp) |
| 地平线 | [原水面](../artifacts/hpwater-validation/compare_waveworks-ocean_d3d12_horizon.bmp) | [新水面](../artifacts/hpwater-validation/compare_hpwater-ocean_d3d12_horizon.bmp) |
| 折射 | [原水面](../artifacts/hpwater-validation/compare_waveworks-ocean_d3d12_refraction.bmp) | [新水面](../artifacts/hpwater-validation/compare_hpwater-ocean_d3d12_refraction.bmp) |
| 泡沫 | [原水面](../artifacts/hpwater-validation/compare_waveworks-ocean_d3d12_foam.bmp) | [新水面](../artifacts/hpwater-validation/compare_hpwater-ocean_d3d12_foam.bmp) |
| 焦散 | [原水面](../artifacts/hpwater-validation/compare_waveworks-ocean_d3d12_caustics.bmp) | [新水面](../artifacts/hpwater-validation/compare_hpwater-ocean_d3d12_caustics.bmp) |
| 水下 | [原水面](../artifacts/hpwater-validation/compare_waveworks-ocean_d3d12_underwater.bmp) | [新水面](../artifacts/hpwater-validation/compare_hpwater-ocean_d3d12_underwater.bmp) |
| 水线 | [原水面](../artifacts/hpwater-validation/compare_waveworks-ocean_d3d12_waterline.bmp) | [新水面](../artifacts/hpwater-validation/compare_hpwater-ocean_d3d12_waterline.bmp) |

Vulkan 对照使用相同文件名，将 `d3d12` 替换为 `vulkan`。每张 BMP 同目录均有同名 JSON 与日志。截图是有限数量场景的验收证据，不代表任意场景都已穷尽测试。

### 双后端一致性

七组 HPWater 图像全部通过固定门限：MAE ≤ 0.03、RMSE ≤ 0.10、变化像素比例 ≤ 0.25、SSIM ≥ 0.90（单像素容差 8/255）。本次实测 SSIM 为 **0.999801–0.999957**，最大平均绝对误差 **0.000077**。边缘少量像素仍有栅格化/浮点差异，因此不是逐位相同。

完整数据见 [acceptance-summary.json](../artifacts/hpwater-validation/acceptance-summary.json) 和各 `parity_*.txt`。

专项图：焦散 [接收物能量](../artifacts/hpwater-validation/detail_d3d12_caustic_energy.bmp) 在水上部分为零，[级联视图](../artifacts/hpwater-validation/detail_d3d12_caustic_cascades.bmp) 显示近/中范围与覆盖外的衰减；CPU snapping/weight 测试验证亚 texel 相机移动的稳定性。[关闭体积光](../artifacts/hpwater-validation/detail_d3d12_volume_off.bmp) 可与上面的水下成图对照，确认前景遮挡与介质积分的区别。

运动序列在 [100 帧（水下）](../artifacts/hpwater-validation/detail_d3d12_motion_100.bmp)、[112 帧（水线振荡）](../artifacts/hpwater-validation/detail_d3d12_motion_112.bmp)、[121 帧（回到水上）](../artifacts/hpwater-validation/detail_d3d12_motion_121.bmp) 取样；所测前景边缘没有保留上一视角的雾色拖影。121 帧报告确认 medium=false、volume active=false、volume history=false。静止水下 [历史调试](../artifacts/hpwater-validation/detail_d3d12_volume_history.bmp) 以绿/青表示接受，局部变化点拒绝；邻域颜色约束与深度拒绝共同限制残影。

180 帧泡沫诊断：[calm](../artifacts/hpwater-validation/detail_d3d12_foam_calm.bmp) 水面 mask 为黑，[wake](../artifacts/hpwater-validation/detail_d3d12_foam_wake.bmp) 仅局部扰动区域出现泡沫，[strong-wind](../artifacts/hpwater-validation/detail_d3d12_foam_strong-wind.bmp) 在此时间/阈值下没有明显白沫。强风不等于强制覆盖泡沫；shader 不在模拟 mask 为零处凭空添加细节。

### 性能快照

测试设备 NVIDIA GeForce RTX 5060，RelWithDebInfo，单个 active view，underwater 命名视角，第 30 帧，固定 1/60 模拟步长，RayMarch/RGB 请求开启，默认 auto 队列调度。水表面 mask 覆盖约 45.2–45.5%；水下介质积分仍作用于整个视图。

| 输出尺寸 | 光学档位 | D3D12 水阶段合计 ms | Vulkan 水阶段合计 ms | D3D12 水池 MiB | Vulkan 水池 MiB |
| --- | --- | ---: | ---: | ---: | ---: |
| 1280×800 | Normal | 3.55 | 4.03 | 51.63 | 52.13 |
| 1280×800 | High | 3.69 | 3.96 | 59.00 | 59.94 |
| 1280×800 | Extreme | 4.05 | 4.07 | 63.06 | 64.00 |
| 2560×1417 | Normal | 9.11 | 7.24 | 167.13 | 167.13 |
| 2560×1417 | High | 9.64 | 7.92 | 193.38 | 193.38 |
| 2560×1417 | Extreme | 7.91 | 7.33 | 206.50 | 206.50 |

这是最近已解析帧各 WaterOptics pass 的时长之和，包含 visibility，不包含共享 FFT，也不是整帧 GPU critical path/FPS。单帧波动与自动队列选择会导致档位之间非单调，不能据此认定 Extreme 比 High 更快。需要稳定性能排名时应固定队列并采集多帧分布。

例如 D3D12 High / 1280×800：visibility 3.14 ms、折射 0.044 ms、体积三阶段合计 0.251 ms。当前主要成本在海面可见性，而不是屏幕空间折射。Normal 的 RayMarch 有效样本为 0、RGB 回退单通道；High/Extreme 本次有效样本为 8。体积 extent 分别为 Normal 四分之一宽高、High/Extreme 二分之一宽高，奇数尺寸向上取整。显存数字仅指水 transient 池当前物理分配。

### 秒退与同步修复记录

- D3D12：RenderGraph 跨队列资源释放不再依赖经过传递约简的依赖边；复用的 Bloom/Hi-Z 等纹理在首次 compute 使用前由 graphics prologue 释放，并增加 fence 等待。恢复图形状态时同时更新每个 mip 的状态表。
- D3D12：upload/readback 堆保持固定原生状态；Ocean.Query 拷贝后恢复声明的 UAV 状态；避免输出原生前后状态相同的 transition。
- Vulkan：独立 compute family 的 barrier 去掉不受支持的 graphics shader stages；交换链呈现信号量按 image 分配；水纹理 capture 读取真正的 Game Output，而不是编辑器窗口。
- 两个后端的 330 帧强制多队列序列均退出码 0。Vulkan validation 无错误；D3D12 无错误，但仍有原有 optimized-clear-value 性能警告 820，不影响清除结果。
- 不启用截图/测试场景覆盖的正常 `--api=d3d12 --scene=hpwater-ocean` 入口另跑 120 帧（1600×900），退出码 0；日志为 `interactive-smoke.*.log`。
- `cpu-tests.log`：11/11 通过，含 ShaderCompiler 双目标编译、跨队列导入/复用纹理与传递依赖约简回归。
- `gpu-tests.log`：5/5 通过，含 Vulkan runtime、两后端 FFT/查询和 water visibility/optics/caustics/volume GPU fixtures。
