# 连续图像采样（P0 1.7 / 1.9）

本实现为架构迁移提供同一进程内的多帧图像证据，不是渲染线程或 Feature 生命周期重构。默认不启用，不改 Demo 设置、海洋算法或 shader。任务是否完成仍以完整矩阵为准。

## 文件与所有权

- `src/Core/Application/FrameCaptureSequence.h/.cpp`：应用层采样计划和单个在途请求；不访问 SceneRenderer、GPU 或 UI。
- `src/Core/ApplicationHost.h/.cpp`：原 BeginFrame 成功后的请求接线；原 RecordTextureCapture / EndFrame / ResolveTextureCapture 顺序不变。
- `src/Renderer/FrameDiagnostics.h/.cpp`：逐请求诊断文件，拒绝覆盖在途请求，沿用原捕获帧冻结逻辑。
- `cmake/PrismApplicationSources.cmake`、`CMakeLists.txt`：应用归属及 CPU fixture 注册。
- `tests/FrameCaptureSequenceTests.cpp`、`tests/FrameDiagnosticsTests.cpp`：有序帧、相邻采样、完成粘滞、输入/路径错误、未完成退出及逐请求诊断。
- `scripts/Capture-ArchitectureSequence.ps1`、`ArchitectureSequences.Common.ps1`：一次进程采样、真实帧/视图/场景/尺寸检查、GPU 锁、源/二进制/证据哈希。
- `scripts/Compare-ArchitectureSequences.ps1`：同后端逐图 V2 与图结构比较，拒绝不同输入和被改写证据。
- `scripts/Validate-ArchitectureWaterBaselines.ps1`：每 API 七视角、三质量×两尺寸，各三次独立捕获，全部重复组合比较。
- `tests/scripts/ArchitectureSequenceTests.ps1`、`ArchitectureSequenceIntegrationTests.ps1`：工具正反例及两后端/无 Editor 的真实未完成退出。

## 数据流与约束

采样 JSON → FrameCaptureSequence → 原后端单请求截图 → 实际视图图像/帧诊断 → 原 GPU readback → 成功确认。各帧正常推进；不重启进程来伪装连续帧，不以新场景覆盖旧图身份。

`PRISM_RENDER_CAPTURE_SEQUENCE_PATH` 指向包含 version=1、frames 整数数组的 JSON；`PRISM_RENDER_CAPTURE_SEQUENCE_OUTPUT` 必须为不存在的新目录。输出为 `frame-N.bmp`、`frame-N.capture.json`、`frames.jsonl`。

- 需要 `PRISM_RENDER_DETERMINISTIC=1`；按已完成逻辑帧计数，时间为 `(N-1)/60`。帧列表严格递增、非零、最多 4096 个，帧号不超过 uint32。
- 与旧 CAPTURE_PATH、CAPTURE_DELAY_FRAMES、独立 FRAME_DIAGNOSTICS_PATH / CAPTURE_DIAGNOSTICS_PATH 互斥；旧接口本身仍可用。
- EXIT_AFTER_CAPTURE=1 等待最后一个样本成功，不在第一个样本后退出；设为 0 时可继续到 MAX_FRAMES。提前窗口关闭/max frames/报告退出不会把未完成序列视为成功。
- 不重用 Vulkan 的持久完成标志去确认未来样本。保存错误抛出，未产生的图像不会有成功 sidecar。
- 只保留有界帧号列表、一个在途捕获及原诊断的当前帧/最近视图，不在运行时累积全部 JSON。
- 沿用原 view 选择/可见性/按需刷新，未知或旧 Scene 源由工具拒绝；不为了诊断伪造新图。
- 截图沿用后端等待 GPU 的行为，可能影响墙钟和资源退役，因此不作为 V3 性能证据；固定模拟时间并与单张捕获对照。没有新增渲染/RHI 线程。
- Pass 次数仍是命令录制/回放次数，不是 GPU dispatch 完成数；InteractiveTerrain 原双视图空 Pass 的例外不改。

## 复现

```powershell
./scripts/Capture-ArchitectureSequence.ps1 -Backend vulkan -Scene preview -Frames 3,4,5 -EndFrame 7 -OutputDirectory artifacts/architecture-refactor/my-preview-sequence
./scripts/Validate-ArchitectureWaterBaselines.ps1 -OutputDirectory artifacts/architecture-refactor/my-water-matrix
./scripts/Compare-ArchitectureSequences.ps1 -ReferenceDirectory <reference> -CandidateDirectory <candidate> -OutputDirectory artifacts/architecture-refactor/my-sequence-compare
```

所有驱动要求新目录，GPU 锁与原架构驱动共用。可见性使用原 PrismImageVisibilityCheck 默认阈值，结构使用 PrismFrameDiagnosticsCompare 自校验；这些不是 golden 对照，后者单独运行原 V2 门限。图像/sidecar/连续帧/输入/日志封存 SHA-256，批次读取会校验；新增未知输入也参与等输入检查。

沿用 WaterValidationSequence，控制数字为已完成帧数，reset 首个结果在 61/73/85。固定采样 64 点：

```text
30 31 32 60 61 62 72 73 74 84 85 86
96 97 98 108 109 110 111 112 119 120 121 122
132 133 134 144 145 146 156 157 158
168 169 170 171 180 181 182 183
192 193 194 204 205 206 216 217 218
228 229 230 240 241 242 252 253 254
301 302 320 321 330
```

这是连续 330 帧内的 64 张图，不宣称其余 266 帧也有图像。JSONL 覆盖全部 330 帧；合法 debug 中间输出另属 details 矩阵，不用“非黑”规则误判其语义。

## 验证记录

固定水输入来源：`TerrainOceanSceneFactory.cpp` 的命名相机、`DemoSceneSettings.cpp` 的原参数解析。近景/foam 为 eye=(-20,5,36.0625)、target=(0,-3,39.0625)；horizon=(-30,10,36.0625)→(270,8,66.0625)；refraction=(-22,3,28)→(-8,-3,40)；caustics=(-19,10,31)→(-3,-8,39)；underwater=(-30,-3,36.0625)→(0,-4,39.0625)；waterline=(-30,-0.1,36.0625)→(0,-0.1,39.0625)。up 均为 (0,1,0)。诊断相机原来就带有封闭 receiver 几何，不是本轮新增；Demo 默认相机不启用该 fixture。

谱随机种子为原 ConfigureSpectrum 默认 `0x4f1bbcdc`，local emitter seed 为原 `0x13579bdf`，本工具不改种子。七视角默认 optical=High，三档质量矩阵只改变 optical quality，谱仍为 HPWater Demo 的 Extreme。foam 使用原 wake surface preset，其余 reference；RayMarch 请求开启、caustics 请求 rgb，实际降档规则不改。水矩阵没有异步资产流送，就绪条件为工厂同步创建及固定第 30 帧；流送 Demo 另验收。

证据根 `artifacts/architecture-refactor/20260828-hpwater-complete/P0/`。

- sequence-preview-01 两 API 和 sequence-no-editor-preview-01：3/4/5 帧三张图、1–7 帧无缺失，strict、可见性/结构通过。
- 各 API 的 sequence-water-controls-01/02/03：各 330 帧、64 张图，strict、reset/resize/三场景及原覆盖率断言通过；共 384 张图、1980 个逻辑帧。
- comparisons/sequence-water-{api}-r2-01、r3-01：四组各 64 个图像及结构比较通过。
- comparisons/sequence-vs-single-final-{api}-01：330 帧对上轮单张路径通过。D3D12 SSIM=.999780、changed=.000480；Vulkan 逐像素一致，原 V2 门限不变。
- gpu/sequence-incomplete-01、sequence-incomplete-no-editor-01：三条预期失败通过；计划 3/4/5、第 4 帧退出，保留前两张证据并明确失败，不产生第 5 帧成功诊断。
- CPU 12/12、11/11：artifacts/architecture-refactor/capture-sequence-cpu-{full,no-editor}-01.xml。新断言并入已有 FrameDiagnostics fixture，不虚增 CTest 项数。
- 工具 9 组通过：artifacts/architecture-refactor/tool-tests/sequences-1a8432a05f3d48a2a4d782477e025fcc/。首次 BMP 测试生成的 PowerShell 有符号常量错误已修复，失败输出保留，不是渲染错误。

后续补充验证与最终结论见下文；20 Demo 的完整控制/双视图/流送序列及 V3 性能仍分别验收，不由固定帧替代。

补充已完成：GPU 完整版 18/18、无 Editor 9/9（实际 strict 图形测试 17+8，两个 Runtime 单列），见 `d3d12/sequence-gpu-full-01`、`vulkan/sequence-gpu-no-editor-01`。两构建使用 CMake 4.1.2；完整/无 Editor 的 Host 头依赖分别 121/114，新序列/诊断各 2，均非零且 VALID，无须删除源文件或对象。完整二进制 SHA-256=`468697069B7AAD9E4E3966F0B617735DD792E6C69D1BA14F0EDBFF639378DADB`，无 Editor=`98DF88647EF0E269F883779E8749A38FBB57D76EC22F433852DAE1BD27A52A22`。

控制序列三次全部两两比较完成（包括 r2/r3），每 API 192 次图像+结构检查；Vulkan 全部逐像素一致，D3D12 最小 SSIM=.999816、最大 changed=.000440。汇总 `comparisons/sequence-control-repeat-summary.json`；所有原 V2 门限不变。

`comparisons/sequence-control-cross-api-01` 的 64 点跨 API 图像/结构检查通过，单独使用原 parity 门限；不代替上面的同 API 检查。`sequence-wave-motion-summary.json` 表明六次运行的 30/31/32 帧均为三个不同图像，未通过冻结波浪取得重复性。

D3D12 七视角和六个质量/尺寸的首个样本已额外对原 `artifacts/hpwater-validation/compare_hpwater-ocean_*` / `bench_*` 图片全部通过 V2，记录 `comparisons/sequence-legacy-water-d3d12-*`。旧图片只读、没有覆盖；旧批次的 queue/cache/构建元数据不完整，因此该补充图像检查不冒充完整的等输入结构或性能验收。

### 窗口尺寸干扰与工具修正

初次静态矩阵 `water-static-matrix-01` 在第 72 次捕获后的比较停止：Vulkan / High / 2560×1417 第三次实际为 **2556×1406**，且第 1–30 帧均如此。前两次正确尺寸的图片 SHA 相同；失败指标为 dimensions_match=false，不是同尺寸图像漂移。当前日志不足以确定具体窗口尺寸变化触发来源，不能归咎于海洋算法。

原失败 aggregate/index、错误尺寸 BMP/sidecar 和比较均保留。新增逐帧实际尺寸对请求值的检查；旧错误样本现在也会被批次读取器拒绝。lifecycle 仅允许原 1280×800、960×600 两个尺寸。工具不修改生产 Window 或强制干预桌面。

新增可选 `-Headless`，只传递引擎原有 PRISM_RENDER_HEADLESS=1；默认仍关闭，Editor 构建能力、Game/Scene 渲染策略不变。Vulkan 六个质量/尺寸在 `water-vulkan-quality-headless-01` 各三次全部通过。Normal/High 四配置还与原尺寸正确的显示窗口样本逐一通过图像及完整捕获结构对照，见 `comparisons/sequence-hidden-visible-*`。

矩阵驱动额外固定同一二进制、跨独立运行的源清单和实际 optical quality。工具正反例已增至 **10 组**：`artifacts/architecture-refactor/tool-tests/sequences-c1d399a7fb184b81b461810e86d579ab/`。

### 选定的可重放水基线

`P0/water-baseline-set-01.json` 逐项登记 26 个配置、78 个捕获、78 次全部两两比较的路径与 SHA；重新校验每个子运行所有封存文件、全部帧身份/尺寸、实际质量以及同一源码/二进制。

- D3D12 七视角及六个质量/尺寸、Vulkan 七视角：使用初次矩阵中各自已经完整通过的子配置，显示窗口。
- Vulkan 六个质量/尺寸：统一使用新的隐藏窗口批次，避免在同一配置内混用窗口模式。
- 失败 aggregate 没有改为通过，错误尺寸样本未纳入；新清单明确记录两批来源和隐藏窗口参数。初次 72 + 重测 18 共 90 次静态捕获中，78 张是选定基线，另 11 张正确尺寸补充样本及 1 张错误尺寸样本都保留。
- 两 API 共 26 个首样本对旧 HPWater 图片的 V2 检查全部通过；七视角与六个质量/尺寸的跨 API 图像检查全部通过，见 `comparisons/sequence-legacy-water-*` 和 `sequence-water-parity-*`。

水基线清单 SHA-256：`8CDC99B1A32FF172AC15D4322514CF4621B4D3CEB24154558CA80C2B6EA423FC`。

### 最终全目录与任务状态

- `P0/{d3d12,vulkan}/sequence-final-all-demos-native-01`：各 20/20 默认 Demo 捕获、实际 strict 校验及第 30 帧诊断通过；两个批次都没有启用多帧 sequence，覆盖旧单张入口的兼容性。
- `P0/comparisons/sequence-final-all-demos-{api}-01`：各 20/20 对上轮 `frame-diagnostics-all-demos-native-01` 的原 V2 图像比较通过，不调用新参考修订、不覆盖旧图。
- 最终工具回归 13+11+10 组通过，分别在 `tool-tests/7ceae1ecc5084c38b4f94ab09996ceaf`、`baselines-8a245871bbfd42fcaf7e195414822df6`、`sequences-78ad9e6dca594648bc9765793a97fda6`。这些路径相对 `artifacts/architecture-refactor/`。
- 60 个 assets 文件对上轮 843 文件恢复点全部哈希相同；没有改生产 shader、海洋算法或 Demo 默认值。

任务 **1.7 完成，18/118**。1.8 的 CPU/GPU/内存/PSO 性能分布与 1.9 的全 Demo 控制/双视图/流送序列仍未完成，未进入 P1 或线程迁移。序列截图的 GPU 等待不能被用作性能基线。

恢复点 `20260828-p0-capture-sequences` 已封存并验证 **853 文件**，manifest SHA-256=`3B1E86160072855D5938671248169436C39A02C88F5E94F00E897540D71851D6`。快照包含 1.7 勾选与上述验收；本条和总验收文档的哈希登记在封存后追加，代码未再改。旧 843 文件快照复核完好。恢复仅复制到新的空目录，不原地覆盖工作区；原 HPWater 图片和本轮失败样本均保留。
