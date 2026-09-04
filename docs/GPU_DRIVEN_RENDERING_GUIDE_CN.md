# PrismRender GPU Driven Rendering 学习与实施指南

## 1. 本阶段目标

本阶段把 PrismRender 从 CPU 决定所有可见对象并直接调用
`DrawIndexed`，推进到下面的数据流：

```text
RenderScene
  -> 稳定 ObjectRecord
  -> Slang Compute Frustum Culling
  -> DrawIndexedIndirectArguments
  -> RDG UAV -> IndirectArgument Barrier
  -> D3D12 ExecuteIndirect / Vulkan vkCmdDrawIndexedIndirect
```

这里的“GPU Driven”指 GPU 决定每个候选对象最终是否产生几何工作。
CPU 仍负责遍历候选对象并绑定该对象当前的 Mesh 与 Material；完全
Bindless 的单次 Multi-Draw 提交需要后续建立 GPU Scene 与全局资源表，
不应和本阶段的跨 API 基础契约混在一次改动中。

## 2. 新增与修改文件

新增：

- `assets/shaders/GpuCulling.hlsl`
- `src/Renderer/Features/GpuDrivenVisibility.h`
- `src/Renderer/Features/GpuDrivenVisibility.cpp`
- `examples/harness/gpu_driven_validation.jsonl`
- `docs/GPU_DRIVEN_RENDERING_GUIDE_CN.md`

主要修改：

- `src/RHI/ICommandContext.h`
- `src/RHI/GraphicsResources.h`
- `src/RHI/GraphicsTypes.h`
- `src/RHI/DeferredCommandContext.*`
- `src/RHI/D3D12/D3D12CommandContextAdapter.*`
- `src/RHI/Vulkan/VulkanContext.*`
- `src/RHI/Vulkan/VulkanParallelCommandRecording.cpp`
- `src/RHI/D3D12/D3D12Resources.cpp`
- `src/RHI/D3D12/D3D12TypeConversions.cpp`
- `src/RHI/Vulkan/VulkanResources.cpp`
- `src/RHI/Vulkan/VulkanTypeConversions.cpp`
- `src/RHI/ShaderTypes.h`
- `src/RHI/ShaderLayoutBuilder.cpp`
- `src/Asset/SlangShaderCompiler.cpp`
- `src/Renderer/SharedRenderGraphFrontend.*`
- `src/Renderer/SceneRenderer*`
- `src/Renderer/VulkanSceneRenderer.*`
- `src/Renderer/RhiPasses.*`

## 3. 公共 Indirect RHI

### 3.1 标准参数布局

公共结构 `DrawIndexedIndirectArguments` 固定为 20 字节：

```cpp
uint32_t indexCount;
uint32_t instanceCount;
uint32_t firstIndex;
int32_t  vertexOffset;
uint32_t firstInstance;
```

它同时匹配：

- `D3D12_DRAW_INDEXED_ARGUMENTS`
- `VkDrawIndexedIndirectCommand`

`static_assert(sizeof(...) == 20)` 防止 C++ 对齐变化悄悄破坏 GPU 数据。

### 3.2 公共命令

`ICommandContext::DrawIndexedIndirect` 支持：

- 参数 Buffer 与 Offset；
- 多条命令与 Stride；
- 可选 Count Buffer；
- Deferred Command Stream 回放；
- 原生并行命令录制。

D3D12 后端按 Stride 缓存 `ID3D12CommandSignature`，最终调用
`ExecuteIndirect`。Vulkan 后端调用 `vkCmdDrawIndexedIndirect`，提供
Count Buffer 时使用 `vkCmdDrawIndexedIndirectCount`。

### 3.3 Usage 与状态

新增：

```text
BufferUsage::Indirect
ResourceState::IndirectArgument
```

后端映射：

| 公共语义 | D3D12 | Vulkan |
| --- | --- | --- |
| Indirect Usage | Buffer 本身无需额外 Flag | `VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT` |
| Indirect State | `D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT` | `DRAW_INDIRECT` Stage + `INDIRECT_COMMAND_READ` Access |

GPU 写入的参数 Buffer 同时带有 `Storage | Indirect | CopyDestination`。
D3D12 创建时增加 `ALLOW_UNORDERED_ACCESS`，初始状态保持 UAV；不能把
UAV 和 Indirect 两个互斥状态直接 OR 在一起。

## 4. Slang StructuredBuffer Reflection

旧 Reflection 只知道资源属于 SRV 或 UAV，无法区分 Buffer 与 Texture，
导致 `RWStructuredBuffer` 被错误映射成 StorageTexture。

本阶段新增 `ShaderResourceShape`：

```text
Unknown
Buffer
Texture
```

`SlangShaderCompiler` 根据 `SlangResourceShape` 识别：

- Structured Buffer；
- Byte Address Buffer；
- Texture Buffer；
- 普通纹理。

`ShaderLayoutBuilder` 因此会把 GPU 剔除 Shader 的 `u0/u1` 正确映射成
`DescriptorType::StorageBuffer`。Shader 单测同时编译 DXIL 与 SPIR-V，
并检查 canonical binding 32、33。

## 5. GPU 可见性模块

### 5.1 ObjectRecord

每个对象使用 48 字节记录：

```text
float4 centerRadius
uint   indexCount
uint   firstIndex
int    vertexOffset
uint   firstInstance
uint   enabled
uint3  padding
```

对象槽位直接使用 `RenderScene` 中的稳定索引。即使对象被剔除，也不会
重新压缩索引，因此 Object Constant、Material Descriptor 与 Indirect
Argument 始终对应同一个对象。

### 5.2 每帧资源

每个 in-flight frame 独立拥有：

- `CullingConstants`
- `IndirectArguments`
- `DescriptorSet`

共享的 ObjectRecord 仅在对象数量、Transform、Mesh 或可见标记变化时
重建。每帧独立参数缓冲避免上一帧 Compute 与下一帧 Graphics 互相覆盖。

### 5.3 Compute 算法

CPU 从相机 ViewProjection 提取并归一化六个裁剪平面。Compute Shader
对每个包围球执行：

```text
dot(plane, float4(center, 1)) < -radius
```

若任一平面满足条件，则写：

```text
instanceCount = 0
```

否则写 `instanceCount = 1`。后端无需读取 GPU 结果；0 实例的 Indirect
Draw 不会产生顶点或像素工作。

## 6. RDG 接入

公共前端新增：

- `GpuObjectRecords`
- `IndirectArguments`
- `GpuVisibility` Parameter Pass

关键版本关系：

```text
GpuVisibility:
  Read  GpuObjectRecords v0 as UAV
  Write IndirectArguments v1 as UAV

GBuffer / Forward:
  Read  IndirectArguments v1 as IndirectArgument
```

RDG 因此自动建立 Compute -> Graphics 依赖，并生成 UAV 写入到 Indirect
读取的 Barrier。参数 Buffer 在首次使用前是 UAV；某个 frame slot 再次
使用时，其初始状态是上一轮留下的 IndirectArgument，RDG 会先转回 UAV。

## 7. D3D12 与 Vulkan 前端

D3D12 的 GBuffer 和 Forward 回调不再忽略 RDG 提供的
`ICommandContext`，因此在 Native Multi-Queue 下，Barrier 与 Draw 会
记录到正确的 Queue Batch。

两个后端都采用同一流程：

```text
绑定对象 Descriptor
绑定该对象 Mesh 顶点/索引 Buffer
按 objectIndex * 20 读取间接参数
提交 DrawIndexedIndirect
```

Shadow 暂时继续绘制全部候选 Shadow Caster。相机视锥之外的物体仍可能
把阴影投进视野，直接复用相机剔除结果会错误漏掉阴影。

## 8. Harness 自动验证

普通 Golden 捕获默认设置 `PRISM_RENDER_DETERMINISTIC=1`，过去会关闭
所有 GPU 动态路径。现在 Harness 支持：

```json
{
  "command": "rdg.describe",
  "arguments": {
    "api": "vulkan",
    "gpuDriven": true
  }
}
```

`gpuDriven: true` 通过受控 Override 在固定相机下开启 GPU Frustum
Culling，并在结果 JSON 中回显 `gpuDriven`。

运行：

```powershell
.\build-windows-ci\Debug\PrismHarness.exe `
  --headless `
  --project-root . `
  --commands examples/harness/gpu_driven_validation.jsonl `
  --output automation/reports/stage22-results.jsonl
```

## 9. 2026-07-19 本机验证

构建成功，CTest 8/8 通过。

真实 NVIDIA GeForce RTX 5060 验证：

| 项目 | D3D12 | Vulkan |
| --- | ---: | ---: |
| 子进程退出码 | 0 | 0 |
| RDG Active Pass | 10/10 | 10/10 |
| `GpuVisibility` Queue | Compute | Compute |
| 单帧 GPU 时间 | 0.00688 ms | 0.00650 ms |
| GBuffer 参数版本 | v1 | v1 |
| GBuffer 参数状态 | IndirectArgument | IndirectArgument |

双 API Tonemap：

```text
MAE                 0.0000752655
RMSE                0.000553027
Changed Pixel Ratio 0
Max Channel Error   0.0196078
Result              PASS
```

报告：

- `automation/reports/stage22-d3d12-gpu-driven-rdg.json`
- `automation/reports/stage22-vulkan-gpu-driven-rdg.json`
- `automation/reports/stage22-results.jsonl`

## 10. 当前边界与下一层演进

本阶段已经完成真实 GPU Frustum Culling 和跨 API Indirect Draw，但还没有
宣称以下能力已经完成：

- 基于上一帧 Hi-Z 的遮挡剔除；
- GPU 参数压缩与 Count Buffer Multi-Draw；
- Bindless Mesh/Material/Texture 全局表；
- GPU LOD 选择；
- Mesh Shader 或 Work Graph 路径。

合理的后续顺序是先完成 Linux Vulkan 平台解耦，再建立 Bindless GPU
Scene。原因是 Bindless 描述符策略必须同时接受 Windows Vulkan 与 Linux
Vulkan 验证，否则容易把平台假设写死在新架构里。
