# Stage 30：Agent 资产流送双 API 运行验证

## 1. 这一阶段解决什么问题

Stage 29 已经让 D3D12 和 Vulkan 能够异步读取 Cooked Asset、提交 GPU Upload、等待 Upload Ticket，并在依赖闭包完整后激活 Scene。但是此前完整验证仍依赖人工设置一组环境变量、分别启动两个渲染器、读取多份报告并执行图像比较。

本阶段把这套流程收敛为一条 Harness 命令：

```text
asset.streaming.validate
```

JSONL Harness 和 MCP Agent 都调用同一个实现。Agent 不需要了解渲染器进程的启动细节，也不需要根据 stdout 猜测资源是否加载完成。

## 2. 文件与职责

新增：

- `examples/harness/asset_streaming_runtime_validation.jsonl`
- `docs/AGENT_ASSET_STREAMING_RUNTIME_VALIDATION_GUIDE_CN.md`

修改：

- `src/Automation/HarnessTools.h/.cpp`
- `tests/EngineHarnessTests.cpp`
- `scripts/Validate-PrismRender.ps1`
- `.github/workflows/windows-ci.yml`
- `docs/LEARNING_GUIDE_CN.md`
- `docs/MCP_AGENT_ADAPTER_GUIDE_CN.md`

`HarnessTools` 负责参数校验、子进程环境隔离、报告解析、跨 API 一致性检查和 Golden Image 判断。渲染器仍然负责真正的 IO、GPU Upload、Scene 激活和截图。

## 3. 数据流

```text
JSONL 或 MCP prism.execute
  -> HarnessTools::ValidateAssetStreaming
  -> 校验 Asset Manifest 与源文件 Hash
  -> 启动 PrismRender.exe --api=d3d12
  -> 异步 Cooked IO -> Upload Ticket -> Scene 原子激活 -> 延迟一帧截图
  -> 读取 Residency / Scene / GPU Timing / CPU Trace / Identity
  -> 启动 PrismRender.exe --api=vulkan
  -> 执行相同流程
  -> 比较资源计数、Scene 绑定和 Tonemap 图像
  -> 返回一条 PrismAssetStreamingValidationResult
```

父进程为每个 API 设置独立的 Capture、Report、stdout、stderr、Crash、Minidump、CPU Trace、GPU Timing、Build Identity 和 Performance Identity 路径。`EnvironmentVariableGuard` 在每次运行结束后恢复父进程环境，防止连续 Agent 请求互相污染。

## 4. 命令参数

最小请求：

```json
{
  "requestId": "streaming-validation-001",
  "command": "asset.streaming.validate",
  "arguments": {}
}
```

完整示例：

```json
{
  "requestId": "streaming-validation-001",
  "command": "asset.streaming.validate",
  "arguments": {
    "width": 1600,
    "height": 900,
    "stage": "tonemap",
    "residentBudgetMb": 512,
    "maximumFrameCount": 120,
    "timeoutMilliseconds": 120000,
    "meanThreshold": 0.001,
    "rmseThreshold": 0.005,
    "changedThreshold": 0.001,
    "pixelTolerance": 4,
    "enforce": true
  }
}
```

可使用 `manifestPath` 指定项目内 Manifest。`residentBudgetMb` 与 `residentBudgetBytes` 互斥。输出路径可以通过 `d3d12Output`、`vulkanOutput`、`d3d12StreamingReport`、`vulkanStreamingReport`、`d3d12SceneReport` 和 `vulkanSceneReport` 覆盖。

## 5. 运行时验收规则

每个 API 必须同时满足：

1. Manifest 至少包含一个导入 Scene，且源文件 Hash 未过期。
2. `assetCount == residentCount`，没有 queued、loading、ready、uploading 或 failed 资产。
3. `completedIoCount` 与 `completedUploadCount` 大于零。
4. Upload Ring 已分配页面，上传字节覆盖 Resident 字节。
5. 至少提交一个 Upload Batch，`completedTicket >= lastSubmittedTicket`。
6. Upload Queue 没有未完成操作、字节或 Batch。
7. Scene 已尝试并成功激活，激活对象数大于零。
8. 每个流送对象都使用 `prism-asset://` Mesh/Material，且运行时资源已就绪。
9. 报告中的 Graphics API 身份与实际子进程一致。

随后还要满足：

- D3D12/Vulkan 的资产数、Resident 数、激活对象数、渲染对象数、流送绑定数和 Scene Asset ID 相同。
- 两张确定性截图通过严格 Golden Image 阈值。

任何一项失败时，`enforce=true` 返回 `asset_streaming_validation_failed`，并把两端完整报告放入 `error.details`，Agent 可以直接定位失败层。

## 6. 为什么不以 synchronousFlushCount 为成功条件

第一次实机验收曾要求设备级 `synchronousFlushCount == 0`。运行结果表明该计数也包含渲染器初始化期间的全局资源 Flush，不能归因于 Asset Streaming。

正确判据是流送资产自己的状态机和 Ticket 闭环：

```text
ReadyForUpload
-> Uploading(ticket)
-> IsUploadComplete(ticket)
-> Resident
```

因此命令使用 `completedUploadCount`、`submittedBatchCount`、`lastSubmittedTicket`、`completedTicket` 和 Queue Drain 状态判定。设备级同步 Flush 仍以 `globalSynchronousFlushCount` 返回，只作为诊断数据，不作为流送失败条件。

## 7. MCP 调用

MCP 不复制这套逻辑。调用稳定的 `prism.execute`：

```json
{
  "name": "prism.execute",
  "arguments": {
    "requestId": "mcp-streaming-validation-001",
    "command": "asset.streaming.validate",
    "arguments": {
      "enforce": true
    }
  }
}
```

结果位于 Tool Result 的 `structuredContent`。`isError` 与 Harness 的 `success` 对齐。同一个 `requestId` 会命中 Harness 幂等缓存，避免 Agent 重试时重复运行两个 GPU 进程。

## 8. 验证方式与本机结果

直接运行：

```powershell
.\build-windows-ci\Debug\PrismHarness.exe `
  --headless `
  --project-root D:\unity_project\PrismRender `
  --commands examples\harness\asset_streaming_runtime_validation.jsonl `
  --output automation\reports\asset-streaming-harness-validation.jsonl
```

统一验证：

```powershell
.\scripts\Validate-PrismRender.ps1 -IncludeGpu
```

本阶段实机结果：

```text
D3D12 validation: passed
Vulkan validation: passed
Resident assets: 8 / 8
Activated streamed objects: 1
Cross-API report identity: passed
Mean absolute error: 0.0000082934
RMSE: 0.0001874651
Changed pixel ratio: 0.0000034722
Harness invocation: passed
MCP prism.execute invocation: passed
CTest: 8 / 8 passed
```

该命令验证的是当前项目真实硬件和驱动上的完整运行闭环。普通云端 Windows CI 只做命令发现与 MCP 协议检查，不伪装成硬件 Golden Image；指定 GPU 机器通过 `-IncludeGpu` 执行完整门禁。
