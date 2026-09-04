# 架构重构视觉基线修订

`scripts/ArchitectureVisualBaselineRevisions.json` 是用户批准的精确视觉参考修订表，不是自动更新 golden 的工具。原图、失败报告、三次独立的新捕获均保留原路径，通过 SHA-256 检查完整性。

当前唯一修订为 `20260828-fluid-determinism-approved`：PBF D3D12、PBF Vulkan、Fluid Toon Vulkan。原因是固定原子网格插入后的粒子遍历顺序；求解方程、默认参数与时间步不变。批准仅覆盖完整 Editor 的 RelWithDebInfo 构建、单 Game view、1280×800、native、默认相机/画质、确定性第 30 帧，不覆盖其他配置。

## 使用

先运行 `tests/scripts/ArchitectureVisualBaselineTests.ps1`，校验登记表、9 张新图、3 张旧图及保留的失败报告。执行实际捕获仍使用 `scripts/Validate-ArchitectureRefactor.ps1`，源输入/二进制记录和 GPU 串行锁保持不变。

将下面三个目录参数替换为已完成的实际批次；输出必须是尚不存在的 architecture-refactor 子目录：

```powershell
./scripts/Compare-ArchitectureDemoRun.ps1 `
  -ReferenceRunDirectory artifacts/architecture-refactor/20260828-hpwater-complete/P0/d3d12/optional-all-demos-native-02 `
  -CandidateRunDirectory artifacts/architecture-refactor/20260828-hpwater-complete/P0/d3d12/pbf-sorted-repeat-native-02 `
  -OutputDirectory artifacts/architecture-refactor/20260828-hpwater-complete/P0/comparisons/review-new-run `
  -UseApprovedRevisions
```

不传 `-UseApprovedRevisions` 时完全不使用新参考，旧图超门限仍失败。传入时也只有精确匹配已批准配置的样本使用新参考；其余样本必须找到等输入旧参考。不同 API 直接拒绝，跨后端 parity 继续使用独立图像比较入口。

每个比较保存实际参考路径、候选路径、使用的 revision，以及原比较器的哈希和指标；整批有任一失败即返回失败，不覆盖已有输出。未知附加参数/画质 override、损坏图像哈希、失败批次、错误帧身份均拒绝。

旧 v1 批次没有 Editor capability 字段：只有与批准时完全相同的二进制 SHA 才能恢复该事实。新驱动补充此字段；普通旧参考若无法确定该能力会明确保留未知状态，并继续核对真实 activeViews、尺寸、帧号和构建配置，不把未知配置套用到批准项。

这只是视觉比较入口。strict validation、连续控制序列、历史/模拟次数和性能是独立门禁；一次或三次图片比较不能代替这些验收。特别是旧图通过的传递关系不能冒充每一张图都直接对原始二进制验收。

捕获驱动的 `-Validation` 现会在 Demo 子进程成功退出后继续检查 stderr，要求实际启用相应校验层，并拒绝既有 D3D12 820 以外的校验诊断。GPU CTest 批次会显式传递 validation 请求；每项是否真正启用仍需检查其结果日志，不能仅凭开关推断。普通捕获默认行为和图像门限未改。

基线自测最终为 10 项；另有原验证工具扩充后的 12 项自测。测试覆盖修改 metadata seal、重复批准项和未知 Editor 能力，不修改任何真实 baseline 文件；负例只写入独立 tool-tests 目录。
