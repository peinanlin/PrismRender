# Stage 17：Agent Harness 基础闭环学习指南

本文档记录 PrismRender 向 Agent 友好开发环境演进的第一阶段真实实现。本阶段完成的是可独立运行、可验证、可回放的 Engine/Harness 数据闭环，不代表动画、物理、脚本、完整 RDG 或 MCP 已经完成。

核心原则是：LLM 或 Agent 位于引擎进程外，引擎内部只提供结构化、确定、可验证、可回滚的命令。

## 1. 本阶段完成了什么

1. 将原单体构建拆分为 `PrismRenderer`、`PrismEngine`、`PrismEditor`、`PrismRender` 和 `PrismHarness`。
2. 增加可持久化、可解析、按种子确定生成的 Entity UUID。
3. 增加基础 World/ECS 数据模型和稳定实体遍历顺序。
4. 增加组件 Reflection，Agent 可以先查询 Schema 再修改属性。
5. 增加版本 2 World 格式，并支持 Engine v1 与项目原有 `PrismRenderScene` v1 迁移。
6. 增加固定时间步、随机种子、Snapshot 和规范化 World Hash。
7. 增加结构化 Command Processor、requestId 幂等、事务、Undo/Redo。
8. 增加 Command Journal 导出和逐事务确定性 Replay。
9. 增加独立 Headless `PrismHarness.exe`，支持 JSON、JSONL、stdin/stdout。
10. 增加项目目录访问限制和 Engine Harness 自动测试。

## 2. 当前边界

Stage 17-B 已在保留现有 `Scene::RenderScene` 的同时，让 `Engine::World` 成为 D3D12 编辑器场景数据的权威来源。目前关系是：

```text
PrismHarness -> Engine::World                    已完成
PrismEditor -> CommandProcessor -> Engine::World 已完成 ECS 场景属性
Engine::World -> RenderScene 同步桥              已完成基础版
Harness -> Shader/Capture/Golden/RDG             已完成基础版
Harness -> Profile/Crash/Memory Budget           后续阶段
```

这样处理是为了先验证 Agent 数据协议、事务和回放，再改变正在工作的跨 API 渲染路径。直接把 ECS、编辑器和渲染器一次性改完，会让错误无法定位到具体层。

## 3. 构建目标怎么拆分

| 目标 | 类型 | 职责 |
| --- | --- | --- |
| `PrismRenderer` | Static Library | Platform、RHI、Asset、Scene、Renderer |
| `PrismEngine` | Static Library | UUID、World、Reflection、序列化、命令事务 |
| `PrismEditor` | Static Library | ImGui DebugPanel 和 EditorLayer |
| `PrismRender` | Executable | 组装编辑器、渲染器和图形后端 |
| `PrismHarness` | Executable | Agent Headless 入口；按命令选择纯 CPU 执行或隐藏 GPU 子进程 |

数据依赖保持单向：

```text
PrismHarness -> PrismRenderer -> PrismEngine
PrismEditor  -> PrismRenderer -> PrismEngine
PrismRender  -> PrismEngine + PrismEditor + PrismRenderer
```

Stage 17-B 后 `PrismHarness` 为了复用 Slang 编译器与公共渲染工具而链接 `PrismRenderer`，但 World 操作不会初始化窗口或 GPU。只有 `render.capture`、`render.compare_apis` 和 `rdg.describe` 会显式启动隐藏的 `PrismRender` 子进程。

## 4. 文件清单

### 4.1 新增 Engine 文件

- `src/Engine/EntityId.h/.cpp`
- `src/Engine/Components.h`
- `src/Engine/Reflection.h/.cpp`
- `src/Engine/World.h/.cpp`
- `src/Engine/WorldSerializer.h/.cpp`
- `src/Engine/CommandSystem.h/.cpp`

### 4.2 新增 Harness 与测试文件

- `src/Automation/HarnessRunner.h/.cpp`
- `src/Automation/HarnessMain.cpp`
- `tests/EngineHarnessTests.cpp`
- `examples/harness/create_scene.jsonl`
- `docs/AGENT_HARNESS_GUIDE_CN.md`

### 4.3 修改文件

- `CMakeLists.txt`
- `docs/LEARNING_GUIDE_CN.md`

## 5. Entity UUID 如何实现

`EntityId` 使用两个 `uint64_t` 保存 128 位标识，并以标准 UUID 字符串形式读写：

```text
127b8eac-46c2-4943-9812-c0a1c4173978
```

`EntityIdGenerator` 使用 `seed + counter` 经过稳定的 64 位混合函数生成 UUID，并设置 UUID Version/Variant 位。相同初始 Snapshot 和相同命令序列会生成相同 Entity ID，这是 Journal Replay 可验证的基础。

不能使用容器地址、数组下标或随机设备结果作为持久 Entity ID：

- 内存地址每次运行不同。
- 数组下标在删除后会改变。
- 非确定随机数会让 Replay Hash 不同。

## 6. World 和组件模型

当前 World 注册七种组件：

| 组件 | 必选 | 属性 |
| --- | --- | --- |
| `Name` | 是 | `value:string` |
| `Transform` | 是 | `position/rotation/scale:float3` |
| `Hierarchy` | 否 | `parent:entityReference` |
| `MeshRenderer` | 否 | `meshAsset/materialAsset:assetPath`、`visible:boolean` |
| `Camera` | 否 | `fieldOfViewY/nearPlane/farPlane:float` |
| `DirectionalLight` | 否 | `direction/color:float3`、`intensity:float` |
| `PointLight` | 否 | `color:float3`、`intensity/range:float` |

必选组件随 Entity 自动创建，不能删除。Hierarchy 会验证：

- Parent 必须存在。
- Entity 不能把自己设为 Parent。
- 沿 Parent 链检查，禁止产生循环。
- 删除 Parent 后，Child 的 Parent 自动清空。

World 使用按 UUID 排序的 `std::map` 保存 Entity。稳定顺序会让序列化文本、哈希和 Replay 不受哈希表遍历顺序影响。

## 7. Reflection 为什么是 Agent 的必要接口

`ReflectionRegistry` 提供组件、属性、类型、是否必选和是否可写。Agent 首先调用：

```json
{"requestId":"001","command":"engine.describe","arguments":{}}
```

返回结果包含所有命令和组件 Schema。Agent 不需要猜测 `Transform` 的字段名，也不能写入未注册属性。

当前 Reflection 是显式注册表。后续扩展可以加入：

- 数值最小值、最大值和步长
- Enum 候选项
- Asset 类型约束
- 编辑器显示名和分组
- C++ Getter/Setter 或代码生成

## 8. World 版本迁移与 Snapshot

World 文件当前格式为：

```text
format = PrismEngineWorld
version = 2
idGenerator = seed + counter
entities = UUID + components
```

Engine 版本 1 使用 Entity 顶层的 `name/position/rotation/scale/mesh/material`。加载时 `MigrateVersion1` 把这些字段转换成版本 2 的 Name、Transform 和 MeshRenderer，保存时只输出版本 2。

项目原有编辑器场景使用 `format=PrismRenderScene, version=1`。`MigrateRenderSceneVersion1` 会确定性生成 UUID，并迁移：

- Camera -> Transform + Camera
- DirectionalLight -> DirectionalLight
- PointLight -> Transform + PointLight
- RenderObject -> Transform + MeshRenderer

旧资源引用优先保留 Asset Path；只有旧场景缺少 Path 时才写成 `prism-handle://类型/句柄`，避免静默丢失引用信息。

运行时 Snapshot 在 World 外增加：

```json
{
  "format": "PrismEngineSnapshot",
  "version": 1,
  "simulation": {
    "fixedDeltaSeconds": 0.016666666666666666,
    "tick": 60,
    "randomSeed": 42
  },
  "world": {}
}
```

Hash 对规范化 Snapshot JSON 执行 FNV-1a 64。它覆盖 Entity、组件、ID 生成器、Tick、固定步长和随机种子。

## 9. Command 协议

请求格式：

```json
{
  "requestId": "task-104-step-7",
  "command": "component.set",
  "arguments": {
    "entity": "127b8eac-46c2-4943-9812-c0a1c4173978",
    "component": "Transform",
    "properties": {"position": [0.0, 2.0, 5.0]}
  }
}
```

成功结果：

```json
{
  "success": true,
  "requestId": "task-104-step-7",
  "command": "component.set",
  "transactionId": 8,
  "worldHash": "e92d6120595c2ad4",
  "data": {}
}
```

失败结果包含稳定错误码和消息。相同 `requestId` 再次提交时返回缓存结果并设置 `idempotentReplay=true`，不会再次创建 Entity 或重复推进模拟。

## 10. 已实现命令

```text
engine.describe
world.status / snapshot / restore / save / load
entity.create / delete / get / list
component.add / remove / get / set
simulation.configure / step
transaction.begin / commit / rollback / undo / redo
journal.list / export
```

文件命令只能访问 `--project-root` 内部。`../` 逃逸和项目外绝对路径会返回 `path_outside_project`，Harness 不提供任意 Shell 执行接口。

## 11. 事务、Undo 和原子失败

单个修改命令自动成为一个事务：

```text
Capture Before Snapshot
  -> Apply Command
  -> Capture After Snapshot
  -> Append Journal Entry
```

多命令事务使用 `transaction.begin/commit`。提交后整个事务只占一个 Undo 单元。`rollback` 恢复 Begin Snapshot。

每条修改命令执行前还会保存命令级 Snapshot。属性校验、路径检查或层级检查失败时自动恢复，因此不会出现“添加了组件，但第二个属性写入失败”的半完成状态。

Undo 恢复 Journal Entry 的 Before Snapshot，Redo 恢复 After Snapshot。Undo 后执行新命令会删除旧 Redo 分支，这是标准线性编辑历史语义。

## 12. Journal Replay

导出的 Journal 包含：

- Initial Snapshot
- 每个事务的结构化请求
- 每个事务完成后的 Expected Hash

Replay 从 Initial Snapshot 开始重新执行请求，并在每个事务后计算 Hash。任何 UUID、浮点状态、命令顺序或迁移结果不同都会立即报出 `Deterministic replay hash mismatch`。

Journal 不依赖 Undo Snapshot 直接跳到结果，而是重新执行命令。这才能验证引擎逻辑是否仍具有确定性。

## 13. Headless Harness 使用方法

执行示例命令并保存 JSONL 结果和 Journal：

```powershell
.\build-windows-ci\Debug\PrismHarness.exe `
  --headless `
  --project-root . `
  --commands examples\harness\create_scene.jsonl `
  --output build-windows-ci\harness\results.jsonl `
  --journal-out build-windows-ci\harness\journal.json
```

重放 Journal，并从标准输入查询状态：

```powershell
'{"requestId":"status","command":"world.status","arguments":{}}' | `
  .\build-windows-ci\Debug\PrismHarness.exe `
  --headless `
  --project-root . `
  --replay build-windows-ci\harness\journal.json
```

命令输入支持一个 JSON Object、JSON Array 或一行一个请求的 JSONL。输出始终是一行一个结构化结果，适合外部 Agent 流式读取。

## 14. 本阶段验证

```powershell
cmake --build build-windows-ci --config Debug --parallel 8
ctest --test-dir build-windows-ci -C Debug --output-on-failure
```

`EngineHarnessTests` 覆盖：

- UUID 生成、格式化和解析
- Reflection Schema
- requestId 成功/失败幂等
- 失败命令原子回滚
- 组件属性设置
- 多命令事务、Undo 和 Redo
- Journal 确定性 Replay
- Engine World v1 和现有 PrismRenderScene v1 到版本 2 迁移
- JSONL 输入输出
- 项目目录路径限制

Stage 17-B 实测 CTest 为 `7/7` 通过。示例执行后 Replay 状态为：

```text
entityCount=1
simulationTick=60
randomSeed=42
worldHash=e92d6120595c2ad4
```

## 15. 推荐学习顺序

1. 阅读 `EntityId.h/.cpp`，理解确定 UUID 的 seed/counter。
2. 阅读 `Components.h` 和 `Reflection.cpp`，对照 `engine.describe` 输出。
3. 阅读 `World.h/.cpp`，理解稳定实体顺序和所有权。
4. 阅读 `WorldSerializer.cpp`，手动跟踪版本 1 到版本 2 迁移。
5. 阅读 `CommandSystem::CaptureState/RestoreState`，理解事务基础。
6. 阅读 `ExecuteUncached`，画出自动事务和显式事务状态图。
7. 阅读 `ExportJournal/ReplayJournal`，理解为什么 Replay 必须执行命令。
8. 阅读 `HarnessRunner.cpp`，理解 JSON、JSON Array 和 JSONL 输入。
9. 运行示例，修改一个 Replay 请求并观察 Hash 验证失败。
10. 最后阅读 `CMakeLists.txt` 的目标依赖，区分无 GPU 的 World 命令与显式启动 GPU 子进程的渲染命令。

## 16. 后续实施顺序

1. ECS 到 RenderScene 同步桥、ImGui Command 路径：Stage 17-B 已完成基础版。
2. Render Capture、Golden Image、Shader 编译和基础 RDG 查询：Stage 17-B 已完成。
3. Asset Import/Reimport 已在 Stage 17-D 完成；Stage 17-E 已增加 Source Bundle 缓存，下一步实现 Cooked 中间格式。
4. 将基础 RenderGraph 升级为完整 RDG，并开放生命周期、显存和 GPU 时间报告。
5. Stage 17-E 已增加结构化日志、基础崩溃报告和进程性能基线；下一步增加 GPU Pass 预算、Minidump 和趋势数据库。
6. 增加动画、物理、音频、Prefab、脚本和游戏运行模式。
7. 最后增加 MCP/Agent Adapter、权限策略和长任务 Checkpoint 恢复。

Stage 17-B 的完整文件、数据流、JSON 命令和实测结果见：

- `docs/AGENT_RENDER_AUTOMATION_GUIDE_CN.md`

Stage 17-C 已让渲染工具真正使用当前 Harness World，并增加跨进程 Snapshot Hash 校验、统一 Asset Path 和结构化资源错误。完整说明见：

- `docs/AGENT_WORLD_RENDER_LOOP_GUIDE_CN.md`

Stage 17-D 已加入 Asset Manifest、稳定 Asset ID、依赖 Hash、资产查询/导入/重导入命令，以及 D3D12/Vulkan 运行时资产重建。完整说明见：

- `docs/AGENT_ASSET_PIPELINE_GUIDE_CN.md`

Stage 17-E 已加入结构化子进程日志、stdout/stderr 工件、Crash Report、性能基线和内容寻址 Asset Cache。完整说明见：

- `docs/AGENT_OBSERVABILITY_CACHE_GUIDE_CN.md`

后续每一阶段都必须保留本阶段的 CTest、Journal Replay Hash 和 D3D12/Vulkan Golden Image 通过，不能通过绕过事务或直接修改场景 JSON 来换取短期功能速度。
