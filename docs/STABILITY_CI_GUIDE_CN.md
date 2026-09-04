# Stage 26：稳定性、验证分层与 CI 学习指南

## 1. 目标

现代渲染器不能只以“本机能运行”作为完成标准。最后阶段需要把构建、CPU 逻辑、GPU 运行时、跨 API 图像和 Agent 协议拆成可重复执行的验证层，并在失败时保留足够诊断信息。

## 2. 文件

新增：

- `.github/workflows/windows-ci.yml`
- `scripts/Validate-PrismRender.ps1`
- `docs/STABILITY_CI_GUIDE_CN.md`

修改：

- `.github/workflows/linux-vulkan.yml`
- `CMakePresets.json`
- `CMakeLists.txt`

## 3. 测试分层

CTest 测试使用 Label：

| Label | 示例 | 运行条件 |
| --- | --- | --- |
| `unit` | RHI Translation、RDG、Golden Metrics | 任意构建机 |
| `integration` | Engine Harness、Minidump | 对应 OS 和工具链 |
| `gpu` | Vulkan Runtime | 有图形运行时或 Mesa |
| `shader` | Slang 编译 | Slang Runtime 已安装 |
| `agent` | Harness/MCP | 构建 Harness |

所有测试都设置 Timeout。这样 Driver、子进程或协议异常不会无限占用 CI Runner。

无 GPU 的 Windows 提交检查使用 `-LE gpu`。Linux Vulkan Job 使用 Xvfb 与 Mesa 软件 Vulkan，执行包含 `gpu` 的完整测试。真实 D3D12/Vulkan Golden Image 仍是显式硬件回归，不应假装成普通云主机测试。

## 4. CMake Preset

新增：

- `windows-ci`
- `windows-vulkan-only-ci`

二者都使用 `Ninja Multi-Config`，避免把 CI 固定到某个 Visual Studio Generator 版本。第一个构建 D3D12、Vulkan、Editor、Harness 和 MCP；第二个关闭 D3D12、Editor 和 Harness，验证公共 Renderer 不会重新引入 Windows/D3D 类型依赖。

已有 `linux-vulkan-debug` 继续验证 Linux Vulkan。

## 5. Windows CI

Windows Job 对两个 Preset 使用 Matrix：

```text
Checkout
-> 初始化 MSVC x64 环境
-> 缓存 FetchContent 依赖
-> Configure
-> Build RelWithDebInfo
-> 运行 CPU-safe CTest
-> MCP stdio Smoke Test
-> 失败时上传 Test Log 和 automation/reports
```

`fail-fast: false` 保证一个配置失败时，另一个配置仍会运行，从而判断问题属于 D3D12/Editor 还是公共 Vulkan 路径。

## 6. 本地统一验证

在 Visual Studio Developer PowerShell 中运行：

```powershell
.\scripts\Validate-PrismRender.ps1
```

默认流程：

1. 增量构建 `build-windows-ci/Debug`。
2. 运行除 `gpu` 外的全部 CTest。
3. 启动真实 `PrismMcpServer.exe`。
4. 验证 initialize、tools/list 和 world.status。

包含本机 Vulkan Runtime 测试：

```powershell
.\scripts\Validate-PrismRender.ps1 -IncludeGpu
```

完整构建存在 `PrismHarness.exe` 时，`-IncludeGpu` 还会执行
`asset.streaming.validate`。该门禁真实启动 D3D12/Vulkan 渲染器，检查
Resident/Upload Ticket/Scene 激活报告并执行严格 Tonemap Golden Image
比较。Vulkan-only 构建没有 Harness，因此只执行其 GPU Runtime CTest。

验证其他目录：

```powershell
.\scripts\Validate-PrismRender.ps1 `
  -BuildDirectory build-windows-vulkan-ci `
  -Configuration Debug `
  -IncludeGpu
```

## 7. 为什么 CI 不能替代 Golden Image

CPU 单元测试负责算法和协议正确性；Mesa Vulkan 负责 Linux API 调用合法性；真正的 GPU Golden Image 负责 Shader、格式、深度约定、驱动与后端最终输出。

三者发现的问题不同：

```text
Unit/Integration -> 逻辑回归
Mesa Vulkan      -> 平台和 API 回归
Hardware Golden -> 数值与最终画面回归
```

因此最终发布门禁应同时保留：

- Windows CPU-safe CI
- Linux Mesa Vulkan CI
- 指定 GPU/驱动机器上的 D3D12/Vulkan Capture 和 Golden Image
- 性能身份匹配后的 Baseline 对比

## 8. 完成定义

一次渲染器改动只有在以下条件满足后才算验证完成：

1. 全量构建成功。
2. 相关 Unit/Integration 测试通过。
3. 修改 RHI 或 Shader 时，Vulkan-only 构建通过。
4. 修改最终画面时，双 API Golden Image 通过。
5. 修改 RDG 调度时，单队列/多队列 A/B 和性能身份一致。
6. 修改 Agent 接口时，JSONL Harness 与 MCP Smoke Test 都通过。
7. 文档记录数据流、权衡和复现命令。

Stage 30 以后，修改 Asset Streaming、运行时 Asset Registry 或 Scene 激活
路径时，还必须让 `asset.streaming.validate` 通过。普通云端 CI 只验证该
命令可被 `engine.describe`/MCP 发现；指定 GPU 机器负责执行完整双 API
硬件门禁。

## 9. 构建身份与符号发布闸门

`Release` 和 `RelWithDebInfo` 都会定义 `NDEBUG`，因此不能根据预处理宏
推断真实配置。CMake 使用
`PRISM_RENDER_BUILD_CONFIGURATION="$<CONFIG>"` 将生成器选择的配置写入
`PrismRenderer`，`PerformanceIdentity` 优先读取该值。

MSVC 目标级函数 `prism_enable_ms_debug_symbols` 为 `PrismRender` 和
`PrismCrashFixture` 的 Debug/RelWithDebInfo 显式设置 `/Zi` 与 `/DEBUG`。
这避免缓存中的全局 CMake flag 被清空时悄悄丢失 PDB。

`Validate-PrismRender.ps1 -IncludeGpu` 还会强制检查：

1. D3D12 与 Vulkan 的 `buildConfiguration` 等于脚本的 `-Configuration`。
2. 两个后端的 Build Identity 均有效。
3. 两个后端的 PDB GUID+Age 与 EXE 匹配。
4. 资产流送、场景激活和 Golden Image 同时通过。

最终发布验证与完整结果见
`docs/FINAL_STABILITY_RELEASE_GATE_GUIDE_CN.md`。
