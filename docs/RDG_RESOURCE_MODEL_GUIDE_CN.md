# PrismRender 完整 RDG 资源模型

本文记录架构路线 Stage 20 的实际实现。目标是把 RenderGraph 从“用字符串连接 Pass 的执行列表”提升为“能够编译资源版本、子资源状态和生命周期的渲染图”。这为下一阶段只编写一套 D3D12/Vulkan 高级渲染前端提供基础。

实现日期：2026-07-19

## 1. 本阶段解决的问题

旧接口 `AddResourceContextPass()` 可以描述一个资源的大致读写状态，但存在以下限制：

1. 字符串无法在编译期区分 Texture、Buffer 和 TextureView；
2. 同名资源被重复写入后，没有版本号判断旧数据是否仍可读取；
3. Texture 只能按整个资源切换状态，无法表达某个 mip 或 array layer；
4. Buffer 没有 Byte Range 状态；
5. Temporal AA、历史曝光等跨帧资源没有统一模型；
6. Pass 回调需要从渲染器成员中捕获原生资源，RDG 无法验证声明与实际使用是否一致。

Stage 20 为这些问题建立了公共模型。现有字符串接口继续保留，后续 Pass 可以逐个迁移，不需要一次重写整个渲染器。

## 2. 文件范围

### 2.1 新增文件

- `src/Renderer/RenderGraphResources.h`
- `src/Renderer/RenderGraphResources.cpp`
- `docs/RDG_RESOURCE_MODEL_GUIDE_CN.md`
- `examples/harness/rdg_resource_model_validation.jsonl`

### 2.2 主要修改文件

- `src/Renderer/RenderGraph.h`
- `src/Renderer/RenderGraph.cpp`
- `src/Renderer/RenderGraphDiagnostics.cpp`
- `src/RHI/PipelineState.h`
- `src/RHI/D3D12/D3D12CommandContextAdapter.cpp`
- `src/RHI/Vulkan/VulkanContext.cpp`
- `src/RHI/Vulkan/VulkanParallelCommandRecording.cpp`
- `tests/RenderGraphTests.cpp`
- `CMakeLists.txt`

## 3. 强类型句柄和资源版本

公共句柄包括：

```cpp
TextureHandle { index, generation, version }
BufferHandle  { index, generation, version }
TextureViewHandle { index, generation, texture }
```

- `index` 定位 RDG 注册表；
- `generation` 在每次 `Reset()` 后递增，旧帧句柄会被拒绝；
- `version` 表示资源内容版本。读取必须使用当前版本，写入必须产生 `current + 1`。

例如：

```cpp
auto parameters = graph.CreatePassParameters();
TextureHandle hdrV1 = parameters.WriteTexture(
    hdrV0,
    RHI::ResourceState::RenderTarget);
graph.AddParameterPass(
    "Lighting",
    std::move(parameters),
    [](RHI::ICommandContext& context,
       const RenderGraphPassResources& resources)
    {
        // resources.GetTexture(hdrV1) 返回本帧解析后的 RHI Texture。
    });
```

`AddParameterPass()` 使用版本事务。若一个 Pass 的后续访问非法，函数会恢复此前已经推进的 Texture/Buffer 版本，再抛出错误。Agent 因错误命令重试时不会继承半更新状态。

## 4. 参数化 Pass

`RenderGraphPassParameters` 显式收集：

- `ReadTexture()` / `WriteTexture()`；
- `ReadBuffer()` / `WriteBuffer()`；
- `ReadTextureView()` / `WriteTextureView()`。

数据流如下：

```text
Renderer 声明 Handle
  -> PassParameters 声明读写、状态和范围
  -> AddParameterPass 校验 generation/version/range
  -> RDG 编译依赖、生命周期、队列和别名
  -> Execute 前自动生成 Barrier
  -> RenderGraphPassResources 解析公共 RHI 对象
  -> Pass 回调录制图形命令
```

这使 Pass 的资源契约与执行回调放在同一个声明中。下一阶段迁移高级渲染前端后，D3D12 和 Vulkan 将消费相同的 Pass 参数，而不是维护两张图。

## 5. Texture 子资源与 Buffer Range

Texture 使用：

```cpp
TextureSubresourceRange {
    baseMipLevel,
    mipLevelCount,
    baseArrayLayer,
    arrayLayerCount
}
```

`0` 个数表示从起点到末尾。RDG 为每个 `mip * arrayLayer` 保存状态，只为实际访问范围生成 Barrier。Hi-Z mip chain、Cubemap 六个面和分层 Shadow 因此可以独立调度。

Buffer 使用：

```cpp
BufferRange { offset, size }
```

Vulkan 后端使用精确 `offset/size`，RDG 通过区间分割与合并维护状态。D3D12 当前公共层仍使用传统 `ResourceBarrier`，它只能切换整个 Buffer，因此采用保守整资源 Barrier；接口保留 Range，后续启用 Enhanced Barriers 时不需要修改 Pass。

同一 Pass 若对重叠范围声明冲突状态，编译执行会明确拒绝；互不重叠的范围可以使用不同状态。相同 UAV 状态的连续写入会生成排序 Barrier。

## 6. History 与 Blackboard

`ImportTextureHistory()` 一次注册：

- `previous`：上一帧只读结果；
- `current`：当前帧写入目标。

两者使用独立物理纹理和句柄，并在诊断中标记 `history=true`。后续 TAA、SSR、自动曝光和 Temporal Upscale 可以共享这一模式。

`RenderGraphBlackboard` 使用类型作为键保存帧内共享结构：

```cpp
graph.GetBlackboard().Set(SceneTextures{...});
const SceneTextures& textures =
    graph.GetBlackboard().Get<SceneTextures>();
```

它用于在多个建图函数之间传递句柄，不替代 GPU 资源所有权。`Reset()` 会清空 Blackboard。

## 7. 临时 Buffer 与别名

`DeclareTransientBuffer()` 将 Buffer 纳入与临时 Texture 相同的生命周期分析：

1. 编译器计算 first/last use；
2. 只让生命周期不重叠且描述兼容的资源共享逻辑 allocation；
3. 报告 logical/physical/aliased bytes。

逻辑别名规划之后已继续完成原生内存落地：D3D12 使用
`Heap + Placed Resource`，Vulkan 使用多个 `VkBuffer` 绑定同一段
`VkDeviceMemory`，执行器会在逻辑资源切换时记录 Buffer Aliasing Barrier。
完整实现和验证见
[RDG_TRANSIENT_BUFFER_ALIASING_GUIDE_CN.md](RDG_TRANSIENT_BUFFER_ALIASING_GUIDE_CN.md)。

## 8. RDG v11 诊断

`rdg.describe` 报告新增：

- 资源 `kind/history/currentVersion`；
- 资源的 `textureSubresourceCount/bufferStateRangeCount`；
- 每个 Pass 访问的 `version/textureRange/bufferRange`；
- `compilation.resourceModel`，统计注册 Texture、Buffer、View、History、版本化资源、参数化 Pass 和状态范围。

Agent 可以先查询报告，再定位第一个状态、版本或范围不一致的 Pass，不需要只根据最终黑屏猜测原因。

## 9. 验证方法与结果

构建和测试：

```powershell
cmake --build build-windows-ci --config Debug --parallel 8
ctest --test-dir build-windows-ci -C Debug --output-on-failure
```

结果：完整构建成功，8/8 CTest 通过。`RenderGraphTests` 覆盖：

- Texture/Buffer 句柄解析；
- 旧 generation 与旧 version 拒绝；
- 非法 Pass 的版本回滚；
- mip/layer Texture Barrier；
- Byte Range Buffer Barrier；
- History 与 Blackboard；
- 临时 Buffer 生命周期别名；
- RDG v11 JSON 序列化。

真实双 API 验证：

```powershell
.\build-windows-ci\Debug\PrismHarness.exe --headless `
  --project-root . `
  --commands examples\harness\rdg_resource_model_validation.jsonl `
  --output automation\stage20-results.jsonl
```

结果为 3/3 成功。D3D12/Vulkan 报告均为 v11；Tonemap 对比 MAE `0.0000640262`、RMSE `0.000512323`、最大通道误差 `0.0196078`、变化像素比例 `0`。

真实报告中的 `parameterPassCount` 暂为 `0`，它准确表示旧高级渲染前端还在使用兼容接口。Stage 21 将逐 Pass 迁移到参数化资源模型，并以该计数和双 API 图像对比作为验收门槛。

## 10. 设计取舍与下一步

- 保留字符串接口：降低迁移风险，但它只能获得整资源保守依赖；
- 使用索引句柄而非裸指针：增加注册步骤，换来代际、版本和诊断能力；
- D3D12 Buffer 暂时整资源切换：可能增加 Barrier，但状态正确且为 Enhanced Barriers 保留升级路径；
- History 由外部提供双缓冲纹理：RDG 管理使用关系，资源池仍管理跨帧物理生命周期。

下一阶段是统一 D3D12/Vulkan 高级渲染前端：建立共享 `SceneTextures` Blackboard，将 Shadow、GBuffer、Hi-Z、Deferred、Bloom 和 Tonemap 迁为参数化 Pass，使两端共用同一套建图代码。
