# P0 Fluid 共享资源与 PBF 重放验证

对应 change `refactor-renderer-architecture-boundaries` 的 1.18/1.19，用户已批准排查修复。本文记录前置正确性修复，不代表 P1 架构拆分或多线程已完成。

最新状态：用户已回复“好的，请继续”，明确批准文末三组稳定样本作为新 P0 视觉参考；下文“候选/等待批准”描述的是批准前状态。正式登记见 `scripts/ArchitectureVisualBaselineRevisions.json`。原图和旧 V2 失败报告保持不动，门限未变。

新增 `scripts/ArchitectureVisualBaselines.Common.ps1`、`scripts/Compare-ArchitectureDemoRun.ps1` 和 `tests/scripts/ArchitectureVisualBaselineTests.ps1`；修改统一捕获驱动，仅补充 Editor 构建能力元数据。数据流为：已完成捕获索引与实际帧报告 → 核对精确输入及图像哈希 → 显式选择批准修订或旧参考 → 原同后端比较器 → 独立报告。未修改 C++、shader、求解参数或 Demo 设置。

批准严格限定为完整 Editor / RelWithDebInfo、单 Game view、native、1280×800、默认参数、确定性第 30 帧。输入不同不会复用该批准；仅 `-UseApprovedRevisions` 开关启用修订。9 张重复图像、3 张旧图、3 个失败报告及源码 manifest 已验证；7 项正反例通过，三组共 9 次显式修订比较通过。对应证据 `P0/comparisons/approved-catalog-{d3d12-pbf,vulkan-pbf,vulkan-toon}-01/`。1.19 可完成；其他质量/时域/性能矩阵继续独立验收。

## 修改与数据流

- `src/Renderer/SharedRenderGraphFrontend.{h,cpp}`：环境纹理导入为唯一 `Environment`，图形消费者和 Fluid 计算消费者使用同一 handle。
- `src/Renderer/SceneRenderer.cpp`：提供现有环境纹理，不新增环境资产。
- `src/Renderer/Features/Fluid/FluidFeature.{h,cpp}`：接收共享环境 handle，删除私有 `Fluid.Environment` 导入。
- `src/Renderer/Features/PlanarReflections.{h,cpp}`、`src/Renderer/Features/Ocean/WaterOpticsFeature.cpp`：仅补环境读声明，不改着色/模拟实现。
- `tests/RenderGraphTests.cpp`：forward/deferred + Fluid 的统一资源身份与读声明回归。
- `assets/shaders/Fluid/PbfGrid.hlsl`、`src/Renderer/Features/Fluid/PbfFluidSimulation.{h,cpp}`：BuildGrid → UAV barrier → 每桶按粒子 ID 排序 → UAV barrier → BuildNeighbors。每桶一个线程，桶之间无写冲突；不修改求解方程、时间步、邻域半径或迭代次数。
- 新增 `tests/PbfFluidGpuTests.cpp`、`tests/shaders/PbfSnapshot.hlsl`，在 `CMakeLists.txt` 注册两后端 GPU 重放测试。测试从真实模拟读取所有粒子位置和密度，不用最终图像代替数值验证。

共享纹理原来只在 Fluid 计算 Pass 中被图声明，但同一纹理已用于 Deferred 中的 Skybox、Forward/Transparent 材质和 Planar/WaterOptics。漏声明使 native graphics prologue 将其提前转为 Common（Vulkan GENERAL），与仍在采样的图形 descriptor 冲突。补齐资源身份/读依赖后由原 RenderGraph 队列算法处理；没有关闭 validation、强制 serial 或修改 RHI。

PBF 的 atomic bucket insertion 顺序取决于 GPU 调度，导致邻居浮点累加次序变化。排序只固定桶内遍历顺序。容量溢出仍报告，排序不恢复被截断粒子，也不承诺溢出时的确定性；测试必须检查诊断，不能把溢出误当正常可重放输入。

## 隔离证据

下列路径相对 `artifacts/architecture-refactor/20260828-hpwater-complete/`。

| 项目 | 证据及结果 |
| --- | --- |
| 共享资源修复，排序之前 | `P0/{vulkan,d3d12}/fluid-dependencies-{native,serial}-01/`：4 Demo × 2 API × 2 queue，共 16/16 capture/strict 通过 |
| 依赖 CPU 回归 | `P0/windows-ci/fluid-dependencies-cpu-01.log`：11/11 |
| Vulkan 原图比较，排序之前 | `P0/comparisons/fluid-dependencies-vulkan-*-native-01/`：fluid-render、fluid-caustics 通过；PBF changed=.002042、SSIM=.996745，Toon SSIM=.998741，未通过 |
| D3D12 上一修复版比较，排序之前 | `P0/comparisons/fluid-dependencies-d3d12-*-native-01/`：三表面 Demo 通过，PBF 未通过 |
| 原模拟数据重放负例 | `P0/gpu/pbf-replay-before-vulkan-02/`：初始化完全一致；首步 11801/11816 个 float word 不同，最大差 2.98023e-7；第 30 步 119755/119460 个 word 不同。显式校验无 error，fixture 因数值不稳定返回失败 |
| 排序后初次数据重放 | `P0/gpu/pbf-replay-sorted-{vulkan,d3d12}-01/`：三次 reset、0/1/30 步位置与密度逐位一致，两后端 strict 通过 |

数值中 `maxAbs` 包括带符号密度元数据，不应直接解释成米制位置误差。每次三轮共 90 个固定步；排序使内部 dispatch 总数从 1641 增至 1731，模拟步数仍相同。没有将增加的排序工作假称为零性能开销。

完整 Demo 的重复图像、对原图兼容、性能及无 Editor 最终验证仍需单独记录。数值重放通过不自动批准新 golden；旧图和失败报告保持不动，原 V2 门限不变。

构建/fixture 开发失败保留：首版 fixture 枚举名、GPU output buffer 缺 CopyDestination 已修正，`pbf-replay-before-vulkan-01` 是 fixture 初始化失败，不能当作模拟不稳定证据。Windows 重配固定 CMake 4.1.2；构建日志中的 LNK1168 属于输出占用，未删除 exe 或终止未知进程，后续链接重试通过。

## 最终数值与图像结果

以下为排序修复及 fixture 初始化/诊断断言补齐后的结果；没有重写旧 golden。

- 最终双视图开关严格矩阵：`P0/gpu/fluid-final-optional-strict-01/` **16/16**，`P0/gpu/fluid-final-no-editor-strict-01/` **8/8**；覆盖 default/optional/ocean/water、native/serial、全部帧槽和两次 resize。与下述 GPU 数据重放三配置合计 **27 项专项 strict**，不当作所有常规 GPU suites 或 P0 全矩阵已经完成。

- 完整 Windows 与 no-Editor 构建通过：`P0/windows-ci/pbf-final-{windows-ci,windows-vulkan-only-ci}-build-01.log`。两者 SceneRenderer 的 Ninja header deps 均为 87 VALID，CMAKE_COMMAND 固定 4.1.2。
- CPU 11/11 + 10/10：`P0/windows-ci/pbf-final-{windows-ci,windows-vulkan-only-ci}-cpu-01.log`。
- GPU 数据：`P0/gpu/pbf-replay-final-01/`，D3D12、Vulkan、no-Editor Vulkan 三轮 strict 均通过；每轮三次 reset 重放，0/1/30 帧的所有位置与带符号密度逐位一致，最终解析的诊断无 grid/neighbor overflow、无无效粒子。另有非零模拟推进及已初始化粒子的断言，不能靠不执行模拟通过。
- PBF 真正独立的三次 Demo 捕获：`P0/{d3d12,vulkan}/pbf-sorted-repeat-native-02/`，每 API 各三次 capture/strict 通过。四组重复比较 `P0/comparisons/pbf-sorted-{d3d12,vulkan}-repeat-r{2,3}-02/` 全为 MAE/RMSE/changed=0、SSIM=1。
- Vulkan Toon：`P0/vulkan/fluid-toon-sorted-repeat-native-01/` 三次 capture/strict 通过，两组 `P0/comparisons/fluid-toon-sorted-vulkan-repeat-r{2,3}-01/` 也完全相同。
- 相关 Demo：`P0/{d3d12,vulkan}/fluid-sorted-related-native-01/`，HPWater + 三个 Fluid 表面 Demo，各后端 4/4 capture/strict 通过。HPWater 默认视角对原始二进制的比较见 `P0/comparisons/fluid-sorted-hpwater-{d3d12,vulkan}-original-01/`：D3D12 SSIM=.999981、changed=.000042；Vulkan 逐像素相同。只覆盖默认视角、1280×800、native、f30，不替代七视角/质量/时域全矩阵。

旧图兼容与重复稳定是两个独立条件：

| 旧图对照 | MAE | RMSE | changed | SSIM | 原 V2 判定 |
| --- | ---: | ---: | ---: | ---: | --- |
| PBF D3D12，上一修复版 | .000206 | .004222 | .001870 | .996944 | 未通过 |
| PBF Vulkan，原始二进制 | .000201 | .004124 | .001966 | .996851 | 未通过 |
| Toon Vulkan，原始二进制 | .000076 | .004268 | .000871 | .998911 | 未通过 |

比较路径分别为 `P0/comparisons/pbf-sorted-d3d12-old-r1-01/`、`pbf-sorted-vulkan-original-01/`、`fluid-sorted-vulkan-fluid-toon-original-01/`。其余三项 D3D12 Fluid 表面图和两项 Vulkan realistic/caustics 图均满足旧 V2 门限，见 `fluid-sorted-d3d12-*-old-01/` 和 `fluid-sorted-vulkan-*-original-01/`。

稳定图像的 SHA-256：PBF D3D12 `18F553FD15B17FDA1AA2D99B0DA5D664672E1EAA7AD966197F6F4F47E9166ECA`；PBF Vulkan `FB88763D0E5A5E7843A11EBA8F62DC5DCDC3C222CFFFC5AD82F485AC27C74810`；Toon Vulkan `7F76EFEA6AB80F3F8C17AB0D2571616E5DA02C0819D27999FCB79564847572A0`。这些是候选稳定样本身份，不是已批准的新视觉基线。

保留的一次运行失败：`P0/d3d12/pbf-sorted-repeat-native-01/` 前两次截图相同，但第三次在初始化期间 exit 1，无截图、无 validation error，也无 crash report。没有查明原因，不能把这一批称作三次通过。未改代码后的 `-02` 独立三次均通过；不删除或隐藏该失败。

当前需明确的决定是：是否批准以确定性修复后的 PBF（双后端）及 Toon Vulkan 样本建立新的 P0 视觉基线，并保留旧样本及全部旧 V2 门限。不自动接受此例外，不把同后端失败改用跨后端宽门限通过。1.17–1.19 和 P0 整体仍未勾选；P1–P8 尚未开始。完整质量/时域、性能和 Linux 验收仍按原计划保留。

审阅样本（全部固定 native、1280×800、f30）：

| 场景 | 旧样本 | 确定性候选 |
| --- | --- | --- |
| PBF D3D12 | [上一修复版](../artifacts/architecture-refactor/20260828-hpwater-complete/P0/d3d12/optional-all-demos-native-02/pbf-game-f30-r1.bmp) | [三次一致的新结果](../artifacts/architecture-refactor/20260828-hpwater-complete/P0/d3d12/pbf-sorted-repeat-native-02/pbf-game-f30-r1.bmp) |
| PBF Vulkan | [原始二进制](../artifacts/architecture-refactor/20260828-hpwater-complete/P0/vulkan/optional-original-all-demos-native-01/pbf-game-f30-r1.bmp) | [三次一致的新结果](../artifacts/architecture-refactor/20260828-hpwater-complete/P0/vulkan/pbf-sorted-repeat-native-02/pbf-game-f30-r1.bmp) |
| Toon Vulkan | [原始二进制](../artifacts/architecture-refactor/20260828-hpwater-complete/P0/vulkan/optional-original-all-demos-native-01/fluid-toon-game-f30-r1.bmp) | [三次一致的新结果](../artifacts/architecture-refactor/20260828-hpwater-complete/P0/vulkan/fluid-toon-sorted-repeat-native-01/fluid-toon-game-f30-r1.bmp) |

## 批准后的闭环结果

- 最终全目录 `approved-final-all-demos-native-01` 双 API 各 20/20 capture/strict；同名 comparison 各 20/20 原 V2 通过，只有登记的三项更换参考，旧失败记录不改。
- 最终 `approved-final-fluid-serial-01` 双 API 各 4/4 strict。四个 Fluid 在各自 API 的当前 native 与 serial 八组图像对比全部逐像素一致，保存于 `approved-final-queue-diagnostic-*`。这不把 native 的批准范围扩成 serial 基线，只证明当前两种队列输出一致。
- 当前 CPU 11/11+10/10，工具 12/12+10/10。结合前次缺失依赖 CPU 回归、最终 GPU 数据与双视图/帧槽/resize strict，1.17–1.19 完成，进度 14/117。
- P0 仍未完成：新的常规 GPU 严格矩阵为 16/18 与 8/9，失败是独立 OceanTessellation fixture，详见 `OCEAN_TESSELLATION_P0_BLOCKER.md`。未改其实现、生产海洋 shader、V2 门限或默认 Demo；完整时域/性能/Linux 等门禁继续保留。
