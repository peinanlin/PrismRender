# Stage 25：Editor、Harness 与 MCP Agent Adapter 学习指南

## 1. 目标

此前 Agent 需要启动 `PrismHarness.exe`，自行写入 JSON/JSONL，再解析一行一个结果。该协议适合 CI，但支持 MCP 的 Agent 宿主还需要标准 JSON-RPC 适配。

本阶段新增 `PrismMcpServer.exe`。它不绕过 Harness，而是复用同一个执行链：

```text
MCP Client
  -> McpServer
  -> HarnessRunner::Execute
  -> HarnessTools 或 CommandProcessor
  -> Engine::World / Renderer 子进程
```

因此幂等、事务、Undo/Redo、World Hash、路径限制、截图、RDG、性能和崩溃诊断都只有一份实现。

## 2. 文件

新增：

- `src/Automation/McpServer.h/.cpp`
- `src/Automation/McpMain.cpp`
- `docs/MCP_AGENT_ADAPTER_GUIDE_CN.md`

修改：

- `src/Automation/HarnessRunner.h/.cpp`
- `tests/EngineHarnessTests.cpp`
- `CMakeLists.txt`

## 3. MCP 工具

`tools/list` 返回：

- `prism.describe`
- `prism.execute`

`prism.describe` 用于发现当前构建真实支持的命令、组件 Reflection、图形 API 和自动化能力。

`prism.execute` 接收：

```json
{
  "requestId": "stable-idempotency-key",
  "command": "component.set",
  "arguments": {}
}
```

没有把几十个 Engine Command 固化成几十个 MCP Tool，是因为命令 Schema 会持续演进，而 `engine.describe` 已是权威 Reflection 入口。两层结构可以保持 MCP 接口稳定，同时让引擎能力动态增长。

## 4. 协议流程

```text
initialize
notifications/initialized
ping
tools/list
tools/call
```

stdio 每行是一条 JSON-RPC 消息。Notification 没有 `id`，Server 不输出响应。Tool Result 包含：

- `content`：兼容只读取文本结果的客户端。
- `structuredContent`：保留完整 Harness JSON。
- `isError`：映射引擎命令是否成功。

JSON 解析错误、无效请求、未知 Method 和无效 Tool 参数都会返回 JSON-RPC 错误对象。

## 5. 持久 World

`McpMain` 在进程生命周期内只创建一个 `CommandProcessor`。连续 Tool Call 操作同一个 World：

```text
entity.create
-> component.add
-> component.set
-> render.capture
```

如果每次请求都重建 World，Agent 就无法完成长任务，也无法使用事务、Journal 和 Undo/Redo。

## 6. 启动

```powershell
.\build-windows-ci\Debug\PrismMcpServer.exe `
  --project-root D:\unity_project\PrismRender
```

MCP 客户端配置应指向该可执行文件，并传入 `--project-root`。Server 的 stdout 只输出协议消息；致命启动错误写入 stderr，避免破坏 JSON-RPC 数据流。

## 7. 验证

自动测试在进程内依次发送 `initialize`、`tools/list` 和 `tools/call(prism.describe)`，检查协议版本协商、工具发现、结构化 Harness 成功结果和 `isError=false`。

此外使用真实 `PrismMcpServer.exe` stdin/stdout 验证了 `world.status`。后续增加 MCP Resources、分页或长任务 Job 时，仍应保持 MCP 只做协议适配，不复制引擎业务逻辑。

## 8. 资产流送长任务

Stage 30 新增 `asset.streaming.validate`。MCP 继续通过 `prism.execute` 调用 Harness，不在 `McpServer` 中复制 D3D12/Vulkan 子进程、报告解析或 Golden Image 逻辑。

一次调用会完成 Manifest 校验、双 API 资产依赖闭包加载、Upload Ticket 等待、Scene 激活、截图和严格图像比较。完整实现与验证见：

- [AGENT_ASSET_STREAMING_RUNTIME_VALIDATION_GUIDE_CN.md](AGENT_ASSET_STREAMING_RUNTIME_VALIDATION_GUIDE_CN.md)
