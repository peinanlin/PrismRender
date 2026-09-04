# Stage 31：最终稳定性与发布闸门学习指南

## 1. 本阶段解决什么问题

前面的 RDG、RHI、双 API、GPU Driven、资产流送和 Agent Harness 已经形成
功能闭环，但“功能能跑”还不等于“构建可发布”。最终阶段把以下事实变成
机器可验证的发布契约：

1. Debug、Release、RelWithDebInfo 不会混用性能基线。
2. Debug 和 RelWithDebInfo 必定生成与 EXE 匹配的 PDB。
3. 崩溃报告可以从真实 Minidump 恢复故障帧和调用者。
4. 全功能与 Vulkan-only 构建都经过 Debug/RelWithDebInfo 回归。
5. D3D12/Vulkan 使用同一流送场景时，资源、场景和最终图像一致。
6. CI 失败时保留测试日志与自动化诊断工件。

## 2. 文件变更

新增：

- `docs/FINAL_STABILITY_RELEASE_GATE_GUIDE_CN.md`

修改：

- `CMakeLists.txt`
- `src/Renderer/PerformanceIdentity.cpp`
- `src/Core/MinidumpSymbolizer.cpp`
- `tests/MinidumpSymbolizerTests.cpp`
- `scripts/Validate-PrismRender.ps1`
- `.github/workflows/windows-ci.yml`
- `.github/workflows/linux-vulkan.yml`
- `docs/STABILITY_CI_GUIDE_CN.md`
- `docs/AGENT_OFFLINE_MINIDUMP_SYMBOLIZATION_GUIDE_CN.md`
- `docs/LEARNING_GUIDE_CN.md`
- `docs/MODERN_RENDERER_ROADMAP_COMPLETION_CN.md`

## 3. 真实构建配置身份

### 3.1 原问题

旧实现根据 `_DEBUG` 和 `NDEBUG` 推断配置：

```text
_DEBUG       -> Debug
NDEBUG       -> Release
otherwise    -> RelWithDebInfo
```

但 MSVC 的 `Release` 与 `RelWithDebInfo` 都定义 `NDEBUG`，所以
RelWithDebInfo 会被错误记录为 Release。其后果是不同优化级别的 GPU/CPU
性能样本可能进入同一条基线。

### 3.2 实现

`CMakeLists.txt` 在 `PrismRenderer` 上定义：

```cmake
PRISM_RENDER_BUILD_CONFIGURATION="$<CONFIG>"
```

`$<CONFIG>` 是 CMake 生成器表达式。在多配置生成器中，它会分别展开为
Debug、Release 或 RelWithDebInfo。`PerformanceIdentity.cpp` 优先使用该
值，仅在非 CMake 构建中回退到旧宏判断。

数据流：

```text
CMake selected configuration
  -> PRISM_RENDER_BUILD_CONFIGURATION
  -> CaptureRuntimePerformanceIdentity
  -> *.identity.json
  -> Harness asset.streaming.validate
  -> Validate-PrismRender.ps1 configuration gate
```

## 4. PDB 与 Build Identity

### 4.1 为什么使用目标级选项

依赖全局 `CMAKE_CXX_FLAGS_*` 容易受到旧 Cache 或外部工具链覆盖。函数
`prism_enable_ms_debug_symbols(target)` 对 Debug/RelWithDebInfo 目标明确
添加：

```text
compile: /Zi
link:    /DEBUG
```

目前应用于：

- `PrismRender`
- `PrismCrashFixture`

启动时 `BuildSymbolIdentity` 读取 EXE 的 CodeView 记录和 PDB GUID+Age，
生成 `PrismBuildSymbolIdentity`。只有 GUID 与 Age 一致，才允许把该 PDB
用于离线符号化。

## 5. Minidump 调用栈语义

调用栈帧现在明确区分来源：

| `unwindMethod` | 含义 | 可信度 |
| --- | --- | --- |
| `exception_context` | 异常线程寄存器中的故障地址 | 最高 |
| `stack_walk` | DbgHelp `StackWalk64` 标准展开 | 高 |
| `stack_scan` | 标准展开失败后的受约束地址扫描 | 辅助 |

自动测试制造真实访问冲突，并要求：

1. 故障帧包含 `TriggerTestCrash`。
2. 至少恢复一个 `stack_walk` 或回退 `stack_scan` 调用者。
3. 正确 PDB 被接受，错误 PDB 被拒绝。
4. 报告可以写入并重新读取。

本次 RelWithDebInfo 实测恢复：

```text
TriggerTestCrash -> exception_context
main             -> stack_walk
```

## 6. 最终验证矩阵

| 构建目录 | 配置 | CTest | 结果 |
| --- | --- | --- | --- |
| `build-windows-ci` | Debug | 8/8 | 通过 |
| `build-windows-ci` | RelWithDebInfo | 8/8 | 通过 |
| `build-windows-vulkan-ci` | Debug | 7/7 | 通过 |
| `build-windows-vulkan-ci` | RelWithDebInfo | 7/7 | 通过 |

四种组合均完成实际编译，且都生成 `PrismRender.pdb`。

完整构建还分别运行：

```powershell
.\scripts\Validate-PrismRender.ps1 `
  -BuildDirectory build-windows-ci `
  -Configuration Debug `
  -IncludeGpu `
  -SkipBuild

.\scripts\Validate-PrismRender.ps1 `
  -BuildDirectory build-windows-ci `
  -Configuration RelWithDebInfo `
  -IncludeGpu `
  -SkipBuild
```

脚本会拒绝以下任一情况：

- D3D12/Vulkan Runtime 配置与 `-Configuration` 不一致。
- EXE/PDB Build Identity 无效或不匹配。
- 任一资产未驻留、Upload Ticket 未完成或队列未排空。
- 流送 Scene 未激活，或 Mesh/Material 运行时绑定不完整。
- 两个 API 的结构化报告不一致。
- Golden Image 超过阈值。

## 7. 双 API 实机结果

测试 GPU：NVIDIA GeForce RTX 5060。

```text
D3D12 resident assets: 8 / 8
Vulkan resident assets: 8 / 8
Streamed scene objects: 1
MAE: 0.00000829339143064709
RMSE: 0.00018746513955815867
Changed pixel ratio: 0.00000347222222222222
```

Debug 和 RelWithDebInfo 均通过；两个后端分别报告正确配置，且 PDB 与 EXE
身份匹配。

关键工件：

- `automation/reports/validation-asset-streaming-debug.jsonl`
- `automation/reports/validation-asset-streaming-relwithdebinfo.jsonl`
- `automation/captures/asset-streaming-runtime-validation-d3d12-streaming.bmp`
- `automation/captures/asset-streaming-runtime-validation-vulkan-streaming.bmp`
- `automation/reports/relwithdebinfo-symbolized-final.json`

## 8. CI 如何使用这些能力

Windows CI 使用全功能与 Vulkan-only Matrix，关闭 `fail-fast`，并通过
concurrency 取消同分支过期任务。MCP Smoke Test 验证命令与能力发现。

Linux CI 在 Ubuntu 24.04 上使用固定 Slang SDK、Vulkan-only 构建、Xvfb
和 Mesa 软件 Vulkan。它负责证明 POSIX/Vulkan 编译与运行路径，不替代
真实 NVIDIA/AMD 硬件上的跨 API Golden Image。

失败时 Windows 上传 CTest 日志、`automation/reports` 和
`automation/tests`；Linux 上传 CTest 与 CMake Configure 日志。

## 9. 发布判断

本技术路线的工程基线已经闭环，可以称为现代跨平台渲染器基线：

- 算法只在共享前端和 Slang 中实现一次。
- D3D12/Vulkan 差异被限制在 RHI 后端。
- RDG 管理生命周期、Barrier、别名和多队列依赖。
- GPU Driven、资产流送、Harness/MCP 有真实运行入口。
- Windows 双 API 和 Vulkan-only 具备自动回归。
- Linux Vulkan 具备 CI 构建与 Mesa 运行定义。

这里不声称已实现 Unity/UE 的全部游戏引擎能力，也不声称本机已经原生
运行 Ubuntu Job。Metal、WebGPU、光追、Virtual Texture、完整编辑器与
游戏系统仍是后续独立产品路线，而不是当前验收缺口。

## 10. 推荐阅读顺序

1. `CMakeLists.txt` 的配置宏与 `prism_enable_ms_debug_symbols`
2. `src/Renderer/PerformanceIdentity.cpp`
3. `src/Core/BuildSymbolIdentity.cpp`
4. `src/Core/MinidumpSymbolizer.cpp`
5. `tests/MinidumpSymbolizerTests.cpp`
6. `scripts/Validate-PrismRender.ps1`
7. `.github/workflows/windows-ci.yml`
8. `.github/workflows/linux-vulkan.yml`

读完本篇后回到
`docs/MODERN_RENDERER_ROADMAP_COMPLETION_CN.md`，按其中顺序学习 RHI、
RDG、共享渲染前端、GPU Driven、资产流送和 Agent Harness。
