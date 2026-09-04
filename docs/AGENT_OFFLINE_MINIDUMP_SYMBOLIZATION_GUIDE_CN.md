# PrismRender Stage 17-I：离线 Minidump 符号化与 Build/PDB 身份匹配

## 1. 阶段目标

Stage 17-F 已经能够在渲染器进程崩溃时生成：

- 结构化 Crash Report。
- Windows Minidump。
- 崩溃进程内的即时符号栈。
- Harness `crash.inspect` 查询结果。

但进程内符号化不能完整解决 CI 和用户机器上的崩溃分析：

- 崩溃机器可能没有 PDB。
- Crash Report 中的即时调用栈可能不完整。
- Dump、EXE 和 PDB 可能来自不同构建。
- Agent 不能仅凭同名文件判断符号是否可信。
- CI 下载 Dump 后需要一个不启动渲染器的离线分析入口。

Stage 17-I 完成：

1. 独立 `PrismDiagnostics` 静态库。
2. EXE/PDB CodeView GUID 与 Age 身份读取。
3. Build Identity JSON 工件。
4. Crash Report v2 内嵌 Build/PDB 身份。
5. Dump、EXE、PDB 三方身份强校验。
6. 离线 Minidump 模块、异常和调用栈解析。
7. `PrismMinidumpSymbolize.exe` 命令行工具。
8. Harness `crash.symbolize` Agent 命令。
9. 标准 `StackWalk64` 与受约束栈扫描回退。
10. 真实崩溃进程和错误 PDB 自动测试。

## 2. 文件变更

### 2.1 新增

- `src/Core/BuildSymbolIdentity.h`
- `src/Core/BuildSymbolIdentity.cpp`
- `src/Core/MinidumpSymbolizer.h`
- `src/Core/MinidumpSymbolizer.cpp`
- `src/Tools/MinidumpSymbolizeMain.cpp`
- `tests/CrashFixtureMain.cpp`
- `tests/MinidumpSymbolizerTests.cpp`
- `docs/AGENT_OFFLINE_MINIDUMP_SYMBOLIZATION_GUIDE_CN.md`
- `examples/harness/offline_minidump_symbolization.jsonl`

### 2.2 修改

- `src/Core/ProcessDiagnostics.cpp`
- `src/Automation/HarnessTools.h`
- `src/Automation/HarnessTools.cpp`
- `tests/EngineHarnessTests.cpp`
- `CMakeLists.txt`
- `docs/LEARNING_GUIDE_CN.md`
- `docs/AGENT_GPU_CRASH_COOKED_GUIDE_CN.md`

## 3. 总体数据流

```text
编译 PrismRender.exe + PrismRender.pdb
  -> PE/PDB 内写入同一份 CodeView GUID + Age
  -> ProcessDiagnostics::Initialize
  -> CaptureBuildSymbolIdentity
  -> 写入 *.build.json

未处理 SEH 异常
  -> SetUnhandledExceptionFilter
  -> MiniDumpWriteDump
  -> Crash Report v2
     - Dump 路径
     - Build Identity
     - 进程内即时符号栈

CI 或 Agent 获得 Dump + EXE + PDB
  -> MiniDumpReadDumpStream
  -> 读取异常、模块、线程、内存和系统信息
  -> 从目标模块 RSDS 记录读取 GUID + Age
  -> 校验 Dump == EXE == PDB
  -> DbgHelp 加载本地模块和 PDB
  -> StackWalk64
  -> 必要时执行受约束 stack_scan
  -> PrismMinidumpSymbolization JSON
```

## 4. 为什么必须匹配 Build/PDB 身份

PDB 名称相同不代表内容匹配。例如以下文件都可能叫
`PrismRender.pdb`：

- Debug 和 Release 构建。
- 不同 Git 提交。
- 不同编译器版本。
- 同一提交的重新链接结果。

MSVC 链接器会在 PE 的 CodeView 调试目录和 PDB 中写入身份：

```text
RSDS
GUID
Age
PDB Path
```

只有 GUID 和 Age 同时一致，PDB 才属于该二进制。当前实现检查：

```text
Dump 目标模块 CodeView == EXE CodeView
Dump 目标模块 CodeView == PDB CodeView
EXE CodeView == PDB CodeView
```

默认任何一项不一致都会返回：

```json
{
  "code": "symbol_identity_mismatch"
}
```

这样 Agent 不会使用错误 PDB 生成看似合理、实际错误的函数名和源码行。

## 5. BuildSymbolIdentity

`BuildSymbolIdentity` 记录：

- EXE 绝对路径、文件大小和 FNV-1a 64 位哈希。
- EXE CodeView GUID、Age、时间戳和映像大小。
- PDB 绝对路径、文件大小和哈希。
- PDB CodeView GUID 和 Age。
- `pdbMatchesExecutable`。
- 稳定 `buildId`。

核心入口：

```cpp
BuildSymbolIdentity CaptureBuildSymbolIdentity(
    const std::filesystem::path& executablePath = {},
    const std::filesystem::path& pdbPath = {});
```

实现使用 `SymSrvGetFileIndexInfoW` 读取 PE/PDB 索引信息，不需要手工解析完整 PDB 格式。

`buildId` 当前由以下信息组成：

```text
CodeView GUID + Age + Executable Hash
```

GUID/Age 用于符号身份，Executable Hash 用于区分二进制内容和构建工件。

## 6. Crash Report v2

`ProcessDiagnostics::Initialize` 新增环境变量：

```text
PRISM_RENDER_BUILD_IDENTITY_PATH
```

当 Harness 启动渲染子进程时，每次 Capture/RDG 运行都会产生：

```text
*.jsonl
*.stdout.log
*.stderr.log
*.crash.json
*.dmp
*.cpu.json
*.identity.json
*.build.json
```

Crash Report 从 version 1 升级到 version 2，并新增：

```json
{
  "format": "PrismCrashReport",
  "version": 2,
  "buildIdentityPath": "...build.json",
  "buildIdentity": {
    "format": "PrismBuildSymbolIdentity",
    "valid": true,
    "buildId": "...",
    "pdbMatchesExecutable": true
  }
}
```

`crash.inspect` 同时兼容 version 1 和 version 2。

Minidump 类型也由 `MiniDumpNormal` 扩展为：

```text
MiniDumpNormal
MiniDumpWithThreadInfo
MiniDumpWithUnloadedModules
```

这保留线程信息和卸载模块信息，同时避免完整内存 Dump 的巨大体积。

## 7. MinidumpSymbolizer 如何工作

### 7.1 映射 Dump

`MappedFile` 使用：

- `CreateFileW`
- `CreateFileMappingW`
- `MapViewOfFile`

将 Dump 只读映射到内存，并对所有 RVA 和长度做边界检查。

### 7.2 读取标准 Stream

`MiniDumpReadDumpStream` 读取：

- `ExceptionStream`
- `ModuleListStream`
- `SystemInfoStream`
- `MemoryListStream`
- `Memory64ListStream`
- `ThreadListStream`

异常 Stream 提供：

- Exception Code。
- Exception Address。
- 崩溃线程 ID。
- 崩溃线程 `CONTEXT`。

模块 Stream 提供：

- 模块基址和映像大小。
- 模块路径。
- CodeView RSDS 记录。

### 7.3 读取本地 PE 映像

精简 Dump 不一定保存所有模块映像页。离线展开可能需要 `.pdata`、`.xdata`
等 PE 可执行文件数据，因此符号化器还会读取本地 EXE/DLL：

```text
DOS Header
PE Signature
File Header
Optional Header
Section Headers
```

每个节按照：

```text
Dump Module Base + Section.VirtualAddress
```

映射到离线地址空间。带 `IMAGE_SCN_MEM_EXECUTE` 的节还会被记录为可执行地址范围。

### 7.4 DbgHelp 符号化

DbgHelp 是进程级全局状态，因此使用互斥锁保护：

```text
SymInitializeW
SymLoadModuleExW
StackWalk64
SymFromAddrW
SymGetLineFromAddrW64
SymCleanup
```

符号搜索路径默认包含：

- EXE 所在目录。
- PDB 所在目录。
- 命令提供的额外 `symbolPaths`。

### 7.5 两种调用栈来源

第一帧直接来自异常上下文：

```json
{
  "unwindMethod": "exception_context"
}
```

随后优先使用标准 `StackWalk64`。

某些精简 Dump 中，DbgHelp 可能因缺少地址页返回
`ERROR_INVALID_ADDRESS`。此时使用保守回退：

1. 从崩溃线程 RSP 开始扫描，最多 64 KiB。
2. 候选值必须落在已知模块范围。
3. 候选值必须落在本地 PE 的可执行节。
4. 候选值必须能解析出符号。
5. 重复地址不会再次加入。

回退帧明确标记：

```json
{
  "unwindMethod": "stack_scan",
  "stackOffset": 104
}
```

`stack_scan` 是保守恢复结果，不等价于标准 unwind 的严格调用关系。Agent
应优先信任 `exception_context` 和未来可能产生的标准 unwind 帧，并将扫描帧用于定位附近调用者。

## 8. 独立命令行工具

构建结果：

```text
build-windows-ci/Debug/PrismMinidumpSymbolize.exe
```

使用方式：

```powershell
.\build-windows-ci\Debug\PrismMinidumpSymbolize.exe `
  --dump automation\reports\process\renderer.dmp `
  --exe build-windows-ci\Debug\PrismRender.exe `
  --pdb build-windows-ci\Debug\PrismRender.pdb `
  --output automation\reports\symbolized\renderer.json `
  --max-frames 128
```

可重复指定额外符号目录：

```text
--symbol-path <directory>
```

只有人工诊断错误工件时才建议使用：

```text
--allow-mismatch
```

默认必须保持身份强校验。

## 9. Harness Agent 命令

命令：

```json
{
  "requestId": "symbolize-renderer-crash",
  "command": "crash.symbolize",
  "arguments": {
    "dumpPath": "automation/reports/process/renderer.dmp",
    "executablePath": "build-windows-ci/Debug/PrismRender.exe",
    "pdbPath": "build-windows-ci/Debug/PrismRender.pdb",
    "maximumFrames": 128,
    "requireIdentityMatch": true,
    "output": "automation/reports/symbolized/renderer.json"
  }
}
```

成功结果包含：

```text
data.reportPath
data.report.exception
data.report.buildIdentity
data.report.dumpTargetModule
data.report.identity
data.report.stack
data.report.modules
```

身份错误时，Harness 返回结构化失败：

```text
error.code = symbol_identity_mismatch
error.details.report
```

即使失败，也会先写出完整分析报告，便于 Agent 查看三方 GUID/Age。

`engine.describe` 新增能力：

```text
offlineMinidumpSymbolization
buildPdbIdentityMatching
```

## 10. JSON 报告关键字段

```json
{
  "format": "PrismMinidumpSymbolization",
  "version": 1,
  "success": true,
  "exception": {
    "threadId": 1234,
    "code": 3221225477,
    "addressHex": "0x..."
  },
  "identity": {
    "matches": true,
    "executableMatchesDump": true,
    "pdbMatchesDump": true,
    "requireIdentityMatch": true
  },
  "stack": [
    {
      "index": 0,
      "symbol": "TriggerTestCrash",
      "file": ".../CrashFixtureMain.cpp",
      "line": 10,
      "unwindMethod": "exception_context"
    }
  ]
}
```

模块列表还会记录：

- 模块路径和基址。
- Dump 内 CodeView。
- 是否成功加载符号。
- 符号加载错误。

## 11. 自动测试

`PrismCrashFixture` 是一个独立测试进程：

1. 初始化 `ProcessDiagnostics`。
2. 写入启动日志。
3. 在 `TriggerTestCrash` 中制造访问冲突。
4. 由顶层 SEH Filter 写出真实 Dump。

`MinidumpSymbolizerTests` 验证：

1. 崩溃进程返回非零。
2. Crash Report v2 存在。
3. Minidump 成功写入。
4. Build Identity JSON 有效。
5. 正确 EXE/PDB GUID + Age 匹配。
6. 离线报告包含故障函数、文件和行号。
7. 标准展开帧显式标记 `stack_walk`；标准展开失败时使用并标记 `stack_scan`。
8. 报告可以原子写入并重新读取。
9. 使用 `PrismRender.pdb` 替代 Fixture PDB 时返回
   `symbol_identity_mismatch`。

`EngineHarnessTests` 验证：

- `crash.symbolize` 被工具发现。
- 两个新能力被 `engine.describe` 暴露。
- `crash.inspect` 继续兼容旧版 Crash Report。

## 12. 本阶段验证结果

2026-07-17 Debug 验证：

- 完整构建成功。
- CTest 8/8 通过。
- 真实访问冲突 Dump 生成成功。
- Dump/Fixture EXE/Fixture PDB 三方身份匹配。
- 错误 `PrismRender.pdb` 被拒绝。
- 独立 CLI 符号化成功。
- Harness `crash.symbolize` 成功。
- 故障帧解析为 `TriggerTestCrash`。
- 离线报告至少恢复故障帧和调用者；`StackWalk64` 恢复的帧明确标记为
  `stack_walk`，仅在标准展开失败时才使用并标记 `stack_scan`。

验证命令：

```powershell
cmake --build build-windows-ci --config Debug --parallel 8
ctest --test-dir build-windows-ci -C Debug --output-on-failure
```

## 13. 设计取舍

### 13.1 为什么独立 PrismDiagnostics

符号化不依赖 D3D12、Vulkan、GLFW 或 ImGui。拆成独立库后：

- CLI 不需要链接完整渲染器。
- Harness 可以直接调用。
- 测试夹具可以保持很小。
- 未来 Crash Uploader、CI Worker 和 Symbol Server Client 可以复用。

### 13.2 为什么不默认联网下载符号

当前阶段强调确定性和本地工件身份。自动访问公网符号服务器会引入：

- 网络不稳定。
- 凭据和权限。
- 符号保留策略。
- CI 结果不可复现。

后续应实现受控 Symbol Store，而不是在核心符号化器中隐式联网。

### 13.3 为什么不写完整 PDB 解析器

PDB 格式复杂，DbgHelp 已提供稳定的 Windows 符号接口。当前项目的重点是渲染器和 Agent Harness，因此使用系统能力更合适。

## 14. 当前限制

- 当前离线上下文实现面向 Windows x64。
- 系统 DLL 没有私有 PDB 时通常只能得到导出符号。
- `stack_scan` 不是严格调用关系，报告已显式标记。
- 尚未接入企业 Symbol Store、Source Index 或 Source Link。
- 尚未建立 Dump/PDB/EXE 的自动归档与保留策略。
- 尚未实现 Dump 隐私扫描、脱敏和上传。

## 15. 推荐阅读顺序

1. `src/Core/BuildSymbolIdentity.h`
2. `src/Core/BuildSymbolIdentity.cpp`
3. `src/Core/ProcessDiagnostics.cpp`
4. `src/Core/MinidumpSymbolizer.h`
5. `src/Core/MinidumpSymbolizer.cpp`
6. `src/Tools/MinidumpSymbolizeMain.cpp`
7. `src/Automation/HarnessTools.cpp` 的 `SymbolizeCrash`
8. `tests/CrashFixtureMain.cpp`
9. `tests/MinidumpSymbolizerTests.cpp`

先理解身份，再理解 Dump Stream，最后理解符号加载和 Harness 协议。

## 16. 下一阶段

建议继续按以下顺序推进：

1. CI Build Artifact Manifest，将 EXE、PDB、Shader Revision、Build ID 和提交身份绑定。
2. 本地/远程 Symbol Store 与按 GUID+Age 查找 PDB。
3. Dump 归档、保留、压缩、隐私扫描和离线批处理。
4. RDG Pass Culling、Transient 生命周期、别名槽和 Queue 同步计划已在 Stage 17-J 完成，详见 `docs/AGENT_RDG_COMPILER_GUIDE_CN.md`。
5. D3D12 Placed Resource、Vulkan Alias Memory、原生 Aliasing Barrier 和 Compute Queue 提交已在 Stage 17-K 至 17-P 完成。
6. MCP Agent Adapter 已在 Stage 25 完成。

本地 Symbol Store、Dump 隐私处理与归档服务属于后续生产基础设施，不阻塞当前渲染器路线验收。
