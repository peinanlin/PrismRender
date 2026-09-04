# 现代跨平台渲染器技术路线总验收

## 1. 结论

下列路线已经完成可编译、可运行、可自动验证的工程基线：

```text
RDG 调度收敛
-> 生产化公共 RHI
-> 完整 RDG 资源模型
-> D3D12/Vulkan 统一渲染前端
-> GPU Driven
-> Linux Vulkan 平台化
-> 资产流送
-> Editor/Harness/MCP 产品化
-> 稳定性与 CI
```

这里的“完成”指路线中的架构闭环和验证入口完成，不表示 PrismRender 已拥有 Unity/UE 的全部编辑器、内容生态、平台和游戏系统。项目定位仍是现代实时渲染器，不是完整游戏引擎。

## 2. 分阶段验收

| 路线 | 主要交付 | 学习文档 | 验证 |
| --- | --- | --- | --- |
| RDG 调度收敛 | Pass Culling、DAG Queue Batch、真实多队列、成本模型、并行录制 | `AGENT_RDG_*_GUIDE_CN.md` | RenderGraph Tests、Queue A/B |
| 生产化 RHI | 公共资源、Descriptor、Pipeline、专用 Copy/Transfer Queue、Upload Ticket、持久 Upload Ring、Retirement、能力查询 | `PRODUCTION_RHI_GUIDE_CN.md`、`RHI_UPLOAD_QUEUE_RING_GUIDE_CN.md` | RHI Translation、双 API RDG、双 API Resident/Ticket 报告 |
| RDG 资源模型 | Attachment、Load/Store、Barrier、Texture/Buffer Transient Alias、Placed/Alias Memory | `RDG_RESOURCE_MODEL_GUIDE_CN.md`、`RDG_TRANSIENT_BUFFER_ALIASING_GUIDE_CN.md` | RenderGraph Tests、双 API 原生 Pool 自检、RDG Report |
| 统一渲染前端 | Shared Pass、Slang、PBR/IBL、Shadow、Deferred、HDR/Bloom/Tonemap | `SHARED_RENDER_GRAPH_FRONTEND_GUIDE_CN.md` | 双 API 分 Pass Capture |
| GPU Driven | GPU Frustum Culling、Indirect Draw、GpuVisibility Pass | `GPU_DRIVEN_RENDERING_GUIDE_CN.md` | GPU Driven RDG + Golden |
| Linux Vulkan | 后端条件编译、POSIX 层、Preset、Mesa CI | `LINUX_VULKAN_PLATFORM_GUIDE_CN.md` | Windows Vulkan-only + Linux CI |
| 资产流送 | 异步 Cooked IO、依赖、RHI Upload、驻留预算和 LRU | `ASSET_STREAMING_GUIDE_CN.md` | IO Test + 双 API Headless Capture |
| Agent 产品化 | JSONL Harness、确定性 World、诊断、MCP stdio、双 API 资产流送门禁 | `AGENT_HARNESS_GUIDE_CN.md`、`MCP_AGENT_ADAPTER_GUIDE_CN.md`、`AGENT_ASSET_STREAMING_RUNTIME_VALIDATION_GUIDE_CN.md` | EngineHarness + MCP Smoke + Streaming Golden |
| 稳定性与 CI | Test Label/Timeout、Windows Matrix、Linux Mesa、真实构建身份、PDB/Minidump、本地统一脚本 | `STABILITY_CI_GUIDE_CN.md`、`FINAL_STABILITY_RELEASE_GATE_GUIDE_CN.md` | Full/Vulkan-only 双配置 CTest + GPU Golden + 符号化 |

## 3. 最终数据流

```text
Editor / JSONL / MCP
        |
        v
CommandProcessor -> Engine::World -> RenderScene Bridge
        |                                  |
        v                                  v
Asset Database -> Streaming Manager -> Shared Render Frontend
                                          |
                                          v
                                  Render Dependency Graph
                                  /                     \
                          D3D12 RHI                 Vulkan RHI
                              |                         |
                    DXIL from Slang          SPIR-V from Slang
```

Agent、编辑器和自动化不直接调用图形 API；高级 Pass 不为每个 API 复制算法；RHI 后端不决定场景语义；RDG 负责编译资源生命周期、Barrier、别名和队列依赖。

## 4. 本轮最终验证结果

Windows 全功能 Debug 与 RelWithDebInfo：

```text
Build: both succeeded
CTest: 8/8 passed in each configuration
MCP stdio: initialize/tools.list/prism.describe passed
Runtime build identity: Debug / RelWithDebInfo matched
EXE/PDB identity: matched for both APIs
```

Windows Vulkan-only Debug 与 RelWithDebInfo：

```text
Build: both succeeded
CTest: 7/7 passed in each configuration
```

真实 GPU：

```text
D3D12 GPU Driven RDG: passed
Vulkan GPU Driven RDG: passed
Cross API Tonemap Golden: passed
Mean absolute error: 0.0000082934
RMSE: 0.0001874651
Changed pixel ratio: 0.0000034722
```

资产流送：

```text
Dependency Cooked IO: passed
D3D12 runtime validation: passed (8 / 8 resident)
Vulkan runtime validation: passed (8 / 8 resident)
Scene activation: passed (1 streamed object)
Cross API report identity: passed
Harness command: passed
MCP prism.execute: passed
```

离线崩溃诊断：

```text
Real access-violation Minidump: generated
Dump/EXE/PDB GUID + Age: matched
Fault frame: TriggerTestCrash
Caller frame: main (stack_walk)
```

报告：

- `automation/reports/asset-streaming-harness-validation.jsonl`
- `automation/reports/validation-asset-streaming-debug.jsonl`
- `automation/reports/validation-asset-streaming-relwithdebinfo.jsonl`
- `automation/captures/asset-streaming-runtime-validation-d3d12-streaming.bmp`
- `automation/captures/asset-streaming-runtime-validation-vulkan-streaming.bmp`
- `automation/reports/relwithdebinfo-symbolized-final.json`

## 5. 明确未声称完成的内容

以下是未来产品能力，不属于本路线基线：

- Metal、WebGPU、主机平台后端。
- Virtual Texture、Sparse Resource、Mesh 分块和 DirectStorage。
- 完整 Bindless 材质系统、Mesh Shader 和 Nanite 类几何虚拟化。
- 光追、路径追踪和硬件 RT。
- Unity/UE 级动画、物理、音频、脚本、Prefab、打包和多人协作。
- 完整资源浏览器、材质图、Timeline、Sequencer 和可视化 Shader Debugger。
- 在当前电脑上原生执行 Linux Job；该部分由 Ubuntu CI 定义验证。

这些边界不破坏当前架构闭环。新增 Metal 时实现 Metal RHI 和 Slang Target，不应重写 Shadow、Deferred 或 Tonemap；新增更高层游戏系统时应位于 Engine/Editor，而不是反向侵入 RHI。

## 6. 推荐学习顺序

1. `PRODUCTION_RHI_GUIDE_CN.md`
2. `RDG_RESOURCE_MODEL_GUIDE_CN.md`
3. `SHARED_RENDER_GRAPH_FRONTEND_GUIDE_CN.md`
4. `SLANG_RHI_GUIDE_CN.md`
5. `GPU_DRIVEN_RENDERING_GUIDE_CN.md`
6. `LINUX_VULKAN_PLATFORM_GUIDE_CN.md`
7. `ASSET_STREAMING_GUIDE_CN.md`
8. `ASSET_STREAMING_SCENE_ACTIVATION_GUIDE_CN.md`
9. `AGENT_HARNESS_GUIDE_CN.md`
10. `MCP_AGENT_ADAPTER_GUIDE_CN.md`
11. `AGENT_ASSET_STREAMING_RUNTIME_VALIDATION_GUIDE_CN.md`
12. `STABILITY_CI_GUIDE_CN.md`
13. `FINAL_STABILITY_RELEASE_GATE_GUIDE_CN.md`

学习每一层时先看输入输出和所有权，再看具体 API。修改代码后使用 `scripts/Validate-PrismRender.ps1`，涉及 Shader/RHI 时再执行双 API GPU 回归。
