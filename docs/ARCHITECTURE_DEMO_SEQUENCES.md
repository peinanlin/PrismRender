# 全 Demo 连续序列门禁（P0 1.9）

任务 1.9 已建立可执行、默认关闭的 20 Demo 连续帧目录。它补充原固定帧 V2 基线，不改变普通启动、Demo 参数、HPWater 算法、shader、队列或 Game/Scene 默认渲染策略。采集包含 GPU readback 等待，因此只用于功能/图像回归，不作为 FPS 或 V3 性能证据。

## 文件与职责

新增文件：

- `scripts/ArchitectureDemoSequences.json`：20 个 `DemoSceneCatalog` key 的唯一连续序列清单，包含采样帧、结束帧、view、控制动作、流送/lifecycle 开关、覆盖标签和不适用说明。
- `scripts/ArchitectureDemoSequences.Common.ps1`：严格校验目录形状、20 key 精确相等、Ocean/Fluid 覆盖和 TAA/阴影/流送/双视图控制要求。
- `scripts/Validate-ArchitectureDemoSequences.ps1`：每个场景连续运行两至三次，封存运行后做同后端逐采样 V2 图像与结构比较，并生成总索引。

修改文件：

- `src/Core/Application/FrameCaptureSequence.h/.cpp`：保持 version 1 兼容，新增 version 2 的有序动作计划；动作仅允许 `set-taa`、`set-shadows`、`refresh-scene`、`activate-streaming`。
- `src/Core/ApplicationHost.cpp`：只在显式 capture sequence 中、对应逻辑帧渲染前消费动作；无动作时走原路径。流式场景可被门控到指定帧，之后仍使用原就绪/激活逻辑。
- `src/Renderer/SceneRendererDiagnostics.cpp`：逐 view 追加只读 `settings.taaEnabled/shadowsEnabled`，用于证明动作真正生效；旧比较忽略新增字段。
- `scripts/Capture-ArchitectureSequence.ps1`、`scripts/ArchitectureSequences.Common.ps1`：传递/封存动作与流送输入，校验实际场景、view、设置和输出；流送报告路径作为输出位置忽略，但流送开关仍是严格视觉输入。
- `tests/FrameCaptureSequenceTests.cpp`、`tests/scripts/ArchitectureSequenceTests.ps1`：覆盖 version 1/2、动作顺序/范围/字段、20 key 缺失、控制覆盖、trace 状态和输入一致性正反例。
- `cmake/PrismApplicationSources.cmake`：补齐应用诊断头的源归属。

## 数据流

```text
20 Demo 严格目录
  -> matrix runner（每场景、每后端、两个独立进程）
  -> Capture-ArchitectureSequence（固定 1/60、帧 1..endFrame）
  -> FrameCaptureSequence（采样 + 有序动作）
  -> ApplicationHost 在目标逻辑帧渲染前应用动作
  -> 原 SceneRenderer / 原截图 readback
  -> BMP + capture diagnostics + 全帧 JSONL
  -> 可见性/结构校验 + 输入和产物 SHA-256 封存
  -> 同后端 repeat 1 vs repeat 2 逐采样 V2 比较
```

普通配置继续使用 version 1 `{frames}`；只有存在控制动作的序列使用 version 2 `{version,frames,actions}`。动作严格递增，同帧按类型唯一，不能晚于最后采样帧；未消费动作会使序列无法完成。未知字段、未知动作、错误 `enabled` 类型、缺少目录项或新增 Demo 未配置都会失败。

## 覆盖策略

- 三个 Ocean：`ocean`、`waveworks-ocean`、`hpwater-ocean`；四个 Fluid：`pbf`、`fluid-render`、`fluid-caustics`、`fluid-toon`，均包含相邻帧样本。
- `post-process` 在 45/60 帧关闭/恢复 TAA；`shadows` 在 45/60 帧关闭/恢复阴影，并在动作前后采样。
- `streaming` 从启动执行原异步 decode/upload，激活门控到第 30 帧，必须在 120 帧前实际进入 `Streamed Asset Scene:*`。
- `waveworks-ocean` 使用 Scene View，并在每个采样帧显式请求原按需刷新；没有把 Scene View 改成普通运行时每帧渲染。
- HPWater 沿用原 330 帧 lifecycle，覆盖 history/local/full reset、两次 resize、WaveWorks/Ocean/HPWater 切换、水体关闭/恢复及最终 coverage 恢复。采样 301 帧观察 debug-view sweep 退出后的 Demo 画面；第 300 帧是合法的近均匀 debug buffer，不使用通用最终画面可见性门限误判。
- 没有专用 P0 控制的场景明确记录 `applicability`，仍在一个进程中从第 1 帧连续推进到结束帧并采样，不能退化为多次单帧启动。

## 最终证据（2026-08-29）

统一目录 SHA-256：`ADBB9B14320231C1F121E398F0C46E68034D56E1D97EDF4E3E998288030DE51B`；完整 Editor 二进制 SHA-256：`CE4A90EF71455A55EE70D03FF54DC1E4BA2E178B7F3588139A040576F8B65B66`。

- Vulkan：`artifacts/architecture-refactor/demo-sequences-vulkan-full-20260829-r3/index.json`，`status=passed`，20 scenes、40 runs、20 comparisons；index SHA-256 `32B041233F53C70636F75A284AC9317742CC9C41DD4DA4D6A806109A42D4F98C`。
- D3D12：`artifacts/architecture-refactor/demo-sequences-d3d12-full-20260829/index.json`，`status=passed`，20 scenes、40 runs、20 comparisons；index SHA-256 `D5AFD5C091F3F083A36DEA11B203E0420A912AF4C0DAF2A943F966C6FC6C9EFE`。
- 代表性 TAA/阴影/流送/双视图矩阵已先在两后端各自通过；HPWater 301 帧调整后也在两后端各自独立重复通过。初始流送输出路径误分类、空动作 PowerShell 折叠和第 300 帧 debug buffer 失败目录全部保留，没有覆盖为通过。
- 完整/无 Editor `PrismRender` 和 `PrismFrameDiagnosticsTests` 构建通过；完整 CTest 29/31 首跑通过，两个缺少本地 Vulkan layer path 的细分项随后在项目规定 `VK_LAYER_PATH` 且仍开启校验时 2/2 通过。脚本正反例 12 组、目录 dry-run 和两个构建的 FrameDiagnostics 测试通过。

## 复现

```powershell
./scripts/Validate-ArchitectureDemoSequences.ps1 -Backend vulkan -Repeats 2 -OutputDirectory artifacts/architecture-refactor/my-vulkan-demo-sequences
./scripts/Validate-ArchitectureDemoSequences.ps1 -Backend d3d12 -Repeats 2 -OutputDirectory artifacts/architecture-refactor/my-d3d12-demo-sequences
./scripts/Validate-ArchitectureDemoSequences.ps1 -Backend vulkan -Scenes post-process,shadows,streaming,waveworks-ocean -Repeats 2 -OutputDirectory artifacts/architecture-refactor/my-control-sequences
```

每次必须使用不存在的新输出目录。完整矩阵的成功不替代任务 1.8 的 V3 性能分布；P0 仍因该独立门禁未完成而不能进入 P1 或线程迁移。

恢复点 `20260829-p0-demo-sequences` 已创建并重复验证 **888 文件**，manifest SHA-256 `57CE3BE2BD69A1830F25B80CADBB42694B102D7E7E36B811C2FB04CC96B9E441`。本说明在封存后追加；恢复只复制到新的空目录，不覆盖工作区、旧 HPWater 证据或任何失败批次。
