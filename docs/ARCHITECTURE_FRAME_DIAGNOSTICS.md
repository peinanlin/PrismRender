# P0 捕获帧诊断（任务 1.5）

## 边界与数据流

这是默认关闭的诊断接线，不是 P1 职责拆分或渲染线程迁移。未修改 HPWater/Fluid 求解、shader、Demo 默认参数、双视图调度或队列选择。

`ApplicationHost` 在获得场景 mailbox 帧后调用 `BeginFrame`，每个实际完成 `SceneRenderer::Render` 的视图提交值快照。图执行的 end marker 在调用/回放线程记录已完成命令录制的 Pass；原生并行录制的工作已 join/append，不从 worker 修改计数。截图录制后冻结当前帧、所选视图及图像路径；`EndFrame` 返回后写帧日志，仅在 readback 成功后写 capture JSON。后续帧、切场景、resize 不会把已经录制的截图诊断替换成新的状态。

- `PRISM_RENDER_FRAME_DIAGNOSTICS_PATH`：每个完成提交的逻辑帧一行 JSONL。
- `PRISM_RENDER_CAPTURE_DIAGNOSTICS_PATH`：一次成功捕获对应的 JSON。
- 两者可独立启用，输出拒绝覆盖既有文件；图像、日志、capture JSON 不允许同一路径。
- 内存只保留当前帧、Game/Scene 各一个最新快照、一个待完成捕获，不持有 GPU 资源。
- 旧 `PRISM_RENDER_RDG_REPORT_PATH` 仍在首个报告点写一次；没有把旧报告冒称为截图帧报告。仅补充不丢组合状态信息的 `stateBits` 字段。
- 无 Editor 的 Vulkan 由 `EndFrame` 直接捕获交换链，诊断也绑定这条既有路径；`outputTarget` 明确区分 `swapchain` 与 `view-texture`，不新增一次渲染或拷贝。
- Vulkan 的 capture complete 是持续状态，且保存失败也会置位；诊断只在 complete 且无错误时发布一次。D3D12 的一次性成功返回保持原样。捕获后继续运行不会重复写报告，保存失败不产生成功报告。

## 字段的准确含义

`frameId` 为 Host 已提交帧的一基序号；`frameSlot` 是 RHI 的循环槽，不能作为帧身份。`sceneGeneration`、`scene`、模拟时间、view 和输出尺寸绑定实际 Render 调用。Game/Scene 各保留自己的历史状态和图。

`recordedPasses` / `recordedPassCounts` 是完成命令录制或回放的次数，**不是 GPU 硬件完成次数或单个 Pass 内 dispatch 数**。`sharedSimulationPassCounts` 汇总两个实际渲染视图中的 spectral/legacy FFT、local wave、PBF Simulate、InteractiveTerrain BrushAndErosion Pass；完整 Pass 列表仍单列。它可发现第二视图重复添加整条模拟链，不把现有 estimated dispatch 统计当作实测。

`history` 在 Render 结束、capture 前观察：TAA valid/readIndex/resetCallCount，WaterOptics version/lastInvalidationBits/完整 key，volumetric version/valid，以及 Ocean 显式 reset 请求序号。TAA resetCallCount 包括本来就在禁用时每帧调用的 ResetHistory，**不是有效历史失效事件数**；lastInvalidation 是最近一次历史状态更新的分类，不保证本帧发生。连续 JSONL 可用于观察版本和请求变化，但不能替代连续图像验收。

`graph` 是 RDG 的最终逻辑资源状态摘要、带 mip/layer/buffer range 的访问声明、Pass 依赖和队列批次；不是逐屏障 driver trace，也不声称导出每个最终子资源状态。必须独立运行实际 D3D12/Vulkan validation。

隐藏/按需 Scene 视图不强制刷新。若所选源不是本帧渲染，写 `sourceFresh=false`；尚未渲染的视图为 `sourceKnown=false`。比较工具拒绝两种情况，即使拿同一份旧图与自身比较；不会通过更改调度让记录看起来正常。

实测普通双视图 Demo 的 `InteractiveTerrain.BrushAndErosion` 可能记录两次，但 `InteractiveTerrain::Execute` 在 revision 已消费时直接返回，所以不能据此宣称 GPU 地形模拟执行两次。保持这一既有基线，海洋链的每帧一次断言不套用到地形空操作。旧 water benchmark 的 `activeViews` 字段也不重新解释为 Host 实际视图数量；新诊断直接列出真实渲染的 Game/Scene。

## 入口与验证

```powershell
./scripts/Validate-ArchitectureRefactor.ps1 -BaselineId 20260828-hpwater-complete `
  -Suite demos -Backend d3d12 -Scenes hpwater-ocean -Repeats 2 `
  -Validation -FrameDiagnostics -RunId unique-run

./scripts/Compare-ArchitectureFrameDiagnostics.ps1 `
  -Reference <reference.capture.json> -Candidate <candidate.capture.json> `
  -OutputDirectory artifacts/architecture-refactor/<unique-comparison>
```

结构比较从文件中的 API 身份选择策略：同后端严格比较稳定的图访问/状态、Pass/执行顺序、队列、模拟计数、历史和帧视图身份；跨后端保留语义检查，忽略队列/回放顺序/帧槽和未使用导入资源的原生初态。两者都排除计时、地址、物理分配等非语义噪声。返回 JSON diff，错误输入/不一致身份/计数不自洽均失败。跨后端不豁免活动资源状态、历史或模拟差异。

图像仍使用 `Compare-ArchitectureImages.ps1` / `Compare-ArchitectureDemoRun.ps1`，原 V2 同后端与跨后端门限独立、未放宽。结构比较不自动判定像素、driver validation 或性能通过。开启完整 JSON 诊断存在序列化/I/O 开销，不用于 V3 性能测量。旧基线没有此诊断，不能追补其历史状态证据。

CPU 测试覆盖延迟 resolve、双视图冻结、每帧日志、错序/重复视图、输出冲突与不覆盖、隐藏/未渲染视图、错帧/错视图/资源状态/历史/重复模拟负例、计时和地址噪声正例，以及独立的同/跨后端队列策略。脚本测试覆盖 opt-in 计划、输出配对、无效 Suite、无封存诊断不允许冒充批准基线。

## 本次文件清单

新增：

- `src/Renderer/FrameDiagnostics.h` / `.cpp`：有界 recorder 与输出。
- `src/Renderer/SceneRendererDiagnostics.cpp`：只读视图诊断构建。
- `src/Tools/FrameDiagnosticsComparison.h` / `.cpp`、`FrameDiagnosticsMain.cpp`：结构比较及 CLI。
- `tests/FrameDiagnosticsTests.cpp`：不依赖 GPU 的正反例。
- `tests/scripts/FrameDiagnosticsIntegrationTests.ps1`：双后端持续完成通知、写图失败和 330 帧实际历史/resize/场景序列验证。
- `scripts/Compare-ArchitectureFrameDiagnostics.ps1`：隔离输出及比较来源哈希。
- 本文档。

修改：

- `src/Core/ApplicationHost.h` / `.cpp`、`ApplicationHostScene.cpp`：opt-in 生命周期和 request/record/resolve 接线。
- `src/Renderer/SceneRenderer.h` / `.cpp`：实际 Pass 完成记录。
- `src/Renderer/Features/TemporalAntiAliasing.h` / `.cpp`：只读状态及 reset 调用计数。
- `src/Renderer/Features/Ocean/WaterOpticsFeature.h`：只读历史访问器。
- `src/Renderer/RenderGraphDiagnostics.cpp`：资源/访问的数值状态位。
- `cmake/PrismRendererSources.cmake`、`CMakeLists.txt`：编译与 CPU CTest 注册。
- `scripts/Validate-ArchitectureRefactor.ps1`、`ArchitectureVisualBaselines.Common.ps1`：捕获诊断配对和封存校验。
- `tests/scripts/ArchitectureValidationTests.ps1`、`ArchitectureVisualBaselineTests.ps1`：脚本保护回归。
- `tests/RenderGraphTests.cpp`：组合资源状态位及实际 TAA reset 计数回归。
- `docs/ARCHITECTURE_REFACTOR_ACCEPTANCE.md`、本 change 的 `tasks.md`：实际验收与进度。

## 已完成验证与构建身份

证据根：`artifacts/architecture-refactor/20260828-hpwater-complete/P0/`。

- 初版诊断完整 Editor 二进制 `9429F72376CE1F39770B960164A4DA0D405428E5E7CDB289F59F0F64E4177F47`：`{d3d12,vulkan}/frame-diagnostics-all-demos-native-01/` 各 20/20 capture/strict，全部与上一 `tessellation-fix-all-demos-native-01` 的原 V2 图像比较通过。D3D12 18 张逐像素一致，其余仍满足原门限。
- 对应结构自比较各 20/20、跨 API 全目录语义比较 20/20，通过；`comparisons/frame-diagnostics-self-all-{api}-01/` 和 `frame-diagnostics-cross-all-01/`。每 API 的 20 份 JSONL 各有连续的 30 帧，共 1200 个逻辑帧记录，按需 Scene 缺席不填造数据。
- HPWater 每 API 两次独立 capture：同后端结构一致、四张图像对照通过；跨 API 结构与独立图像门限均通过（图像 SSIM=.999796）。路径为 `frame-diagnostics-hpwater-native-01` 与相关 comparisons。
- 最终边界修正后，完整二进制 `A98D22D4BB13F39CED0C4D199A181FB62E23F81B4E17D2F2DC8D2EB6B38896F0`，无 Editor `7FBC8807D316759ED1B50976AE466F3DA7790E2A1A13C7969177DF290278C4D2`。差异仅诊断的重复完成/错误判断、swapchain 捕获接线与输出目标字段；**不把前述 40 次截图冒称由最终哈希生成**。
- 最终 CPU **12/12 + 11/11**，JUnit 位于 `artifacts/architecture-refactor/` 下的 `frame-diagnostics-cpu-full-02.xml`、`frame-diagnostics-cpu-no-editor-02.xml`；包含 TAA 实际 reset 调用计数、组合 state bits 和所有诊断正反例。
- 最终边界 GPU 集成 **4+2 项**：`gpu/frame-diagnostics-boundaries-final-01/`（两 API）、`gpu/frame-diagnostics-no-editor-boundaries-02/`（Vulkan）。capture 第 3 帧成功后继续到第 7 帧；故意把图像路径设为新目录时，按预期在第 3 帧保存失败，严格校验无错误且没有成功诊断文件。
- 最终 HPWater 连续序列 **两 API 各 330 帧**：`gpu/frame-diagnostics-lifecycle-final-01/`。实际记录显式历史/local/full reset、960×600 resize、至少三个场景标签和海洋链每逻辑帧至多一次；已有第 320 帧覆盖率恢复断言通过。这是连续诊断和最终截图，不代替每个控制点的连续图像矩阵。
- 60 个 assets 文件全部与上一 833 文件恢复点哈希相同；没有生产 shader 或 Demo 参数变化。

实现期间的失败证据保留：重复完成通知的 CPU 负例 `frame-diagnostics-sticky-regression-before.xml`；组合状态 CPU fixture 未先 Compile 导致数组为空的两份 `frame-diagnostics-final-cpu-*.xml`；无 Editor swapchain 初次接线遗漏的 `gpu/frame-diagnostics-no-editor-boundaries-01/`。上述均属本次诊断/测试实现问题，已经修复并重新验证，不归咎于既有引擎。

构建缓存曾由系统 CMake 改写为错误的中文依赖前缀，导致 10 个对象的 `#deps=0`。已以固定 CMake 4.1.2 重配并通过 Ninja 清理这 10 个可重建对象，源码/旧证据未删除；清单 `artifacts/architecture-refactor/frame-diagnostics-rebuilt-empty-deps.json`。最终两构建使用正确前缀，SceneRenderer 的 87 个依赖及新增诊断对象依赖非零且 VALID。

最终 GPU 套件已通过 **18/18 + 9/9**：`d3d12/frame-diagnostics-final-gpu-01/`、`vulkan/frame-diagnostics-final-no-editor-gpu-01/`。其中真实校验层启用的图形测试分别 17、8 项，各自另有一个 VulkanRuntime，不能把 Runtime 项也计为 strict 图形测试。

最终 Game/Scene 捕获完成：`{d3d12,vulkan}/frame-diagnostics-final-smoke-01/` 各 preview/HPWater 两项，图像对上一验收版、结构对本轮初版诊断均通过；`frame-diagnostics-final-scene-01/` 各一个真实 Scene 捕获，frameId=30/view=scene/sourceFresh=true，自比较与两 API 语义比较通过。最终 HPWater 默认视角直接对原始图像、330 帧最终图对上一 lifecycle 结果的原 V2 比较均通过，见 `comparisons/frame-diagnostics-final-*`。

最终工具测试 **13+11** 通过，记录在 `artifacts/architecture-refactor/tool-tests/fbc8780943024b6b89ddf25206fe3666/` 和 `baselines-9ca696cc20a44f9f85f84fe2ec7ec516/`。原始 809 文件与上一 833 文件快照再次验证完好。

1.5 完成，进度 **17/118**。恢复点 `20260828-p0-frame-diagnostics` 已封存并验证 **843 文件**，manifest SHA-256：`B1EDC9C513B8782DF478789C0D317B5536E9A3403FD8392EFFADF042F762B826`。快照已含 1.5 勾选，本条哈希登记与总验收文档的对应说明在封存后追加；代码没有再改。恢复仅复制到新的空目录，不自动覆盖工作区。

任务 1.7–1.9 的完整质量/尺寸/连续图像/性能，以及后续 Linux 验收仍未由这些结果替代，未进入 P1 或渲染线程迁移。
