# PrismRender Render Graph 面试复习笔记

本文面向面试复习，按当前项目实际代码说明 `Renderer -> Render Graph -> RHI -> D3D12/Vulkan` 的关系，不把理论能力和当前实现混为一谈。

## 1. 一句话架构

```text
Scene 提供要渲染的数据
Renderer 决定如何渲染
Render Graph 调度 Pass 和资源
RHI 提供跨 API 接口
D3D12/Vulkan 后端记录原生命令
GPU 执行 Draw、Dispatch 和 Shader
```

## 2. Render Graph 一帧流程

```text
SceneRenderer::Render
  -> 更新 Frame/Object/Material 数据
  -> 创建 SharedRenderGraphResources
  -> 创建 SharedRenderGraphCallbacks
  -> SharedRenderGraphFrontend::Build
       -> 声明 Pass 读写资源、目标状态、队列和回调
  -> RenderGraph::Compile
       -> RAW/WAR/WAW 依赖
       -> Pass Culling
       -> FirstUse/LastUse
       -> Transient Aliasing
       -> Graphics/Compute Queue Batch
  -> RenderGraph::Execute
       -> ValidateReads
       -> Aliasing Barrier
       -> Resource Barrier
       -> Debug Label / CPU Trace / GPU Timestamp
       -> 调用 Pass 回调
            -> Renderer 绑定 Pipeline/Descriptor/Attachment
            -> Draw 或 Dispatch
            -> RHI 转换为 D3D12/Vulkan 命令
```

## 3. Pass 回调是什么

回调就是“在 Build 阶段保存、在 Execute 阶段才调用的函数对象”。

`SceneRenderer::Render()` 中先创建具体回调：

```cpp
graphCallbacks.gbuffer =
    [this, &frame, &scene, depthView](
        RHI::ICommandContext& commandContext,
        const RenderGraphPassResources&)
    {
        RenderGBufferPass(
            commandContext,
            *depthView,
            frame.GetFrameWidth(),
            frame.GetFrameHeight(),
            scene);
    };
```

`SharedRenderGraphFrontend::Build()` 再把回调和资源声明注册到同一个 Pass：

```cpp
graph.AddParameterPass(
    "GBuffer",
    std::move(gbufferParameters),
    callbacks.gbuffer);
```

二者关系：

```text
资源状态声明：描述这个 Pass 需要哪些资源、以什么状态访问
Pass 回调：描述轮到这个 Pass 时具体记录哪些 GPU 命令
```

状态声明负责调度和 Barrier；回调负责真正的 Draw/Dispatch。只有声明没有回调，Pass 不知道做什么；只有回调没有声明，Render Graph 不知道怎样正确同步资源。

## 4. 绘制场景对象与全屏三角形

### 4.1 绘制场景对象

GBuffer、Forward、Shadow、Transparent 等几何 Pass 要处理真实 Mesh。

```text
遍历 RenderObject
  -> 过滤不可见对象和错误渲染队列
  -> 选择 Material/PSO
  -> 绑定 Object/Material Descriptor
  -> 绑定 Mesh Vertex Buffer
  -> 绑定 Mesh Index Buffer
  -> DrawIndexed
```

每个对象有自己的顶点、索引、Transform 和材质。GBuffer Pass 的结果不是最终屏幕颜色，而是世界位置/粗糙度、法线/金属度、Albedo/AO、自发光/Alpha、Motion Vector 和 Depth。

### 4.2 绘制全屏三角形

Deferred Lighting 和 Tonemap 不需要重新绘制全部 Mesh。它们处理的是“上一阶段已经生成的整张屏幕纹理”。项目调用：

```cpp
commandContext.Draw(3);
```

Vertex Shader 根据 `SV_VertexID` 直接生成覆盖屏幕的三个顶点，不需要 Vertex Buffer。Pixel Shader 对屏幕上的每个像素采样 GBuffer/HDR/Bloom 等纹理。

区别：

```text
场景对象绘制：Mesh -> 顶点变换 -> 光栅化 -> 生成表面数据
全屏三角形：已有屏幕纹理 -> 每像素后处理/光照 -> 新屏幕纹理
```

全屏三角形用一个三角形覆盖屏幕，避免全屏四边形的中间对角线和重复顶点，也便于用 `SV_VertexID` 无 VB 绘制。

## 5. Deferred Lighting 为什么是全屏三角形

GBuffer 已经保存了屏幕上每个可见像素的表面数据。Deferred Lighting 只需要对每个屏幕像素执行：

```text
读取 GBuffer
  -> 恢复世界位置、法线、材质参数
  -> 读取灯光、阴影、IBL、AO
  -> 计算 PBR 光照
  -> 输出 HDR Color
```

因此不需要再次提交每个场景 Mesh，只需要执行一次覆盖全屏的 Pixel Shader。

## 6. Compute Dispatch 是什么

`Dispatch(x, y, z)` 是启动 Compute Shader 的命令，对应 Graphics Pipeline 中的 `Draw`，但它不生成三角形，也不经过光栅化。

Compute Shader 用线程组描述并行工作量：

```hlsl
[numthreads(8, 8, 1)]
void BrightExtractCS(
    uint3 dispatchThreadId : SV_DispatchThreadID)
```

一个线程组处理 8×8 像素。假设目标为 1920×1080：

```text
groupCountX = ceil(1920 / 8) = 240
groupCountY = ceil(1080 / 8) = 135
Dispatch(240, 135, 1)
```

RHI 最终转换为：

```text
D3D12：ID3D12GraphicsCommandList::Dispatch
Vulkan：vkCmdDispatch
```

## 7. Bloom 的当前实现

Bloom 用于让超过阈值的高亮区域产生向周围扩散的辉光。当前项目使用三个 Compute Pass：

```text
HDR/PostProcessColor
  -> BloomExtract
       提取 brightness > bloomThreshold 的像素
       输出 BloomA
  -> BloomHorizontal
       水平方向模糊 BloomA
       输出 BloomB
  -> BloomVertical
       垂直方向模糊 BloomB
       输出 BloomA
  -> Tonemap
       HDR + BloomA * bloomIntensity
```

Bright Extract：

```hlsl
float brightness = max(max(color.r, color.g), color.b);
outputTexture[pixel] =
    brightness > bloomThreshold
    ? float4(color, 1.0f)
    : float4(0.0f, 0.0f, 0.0f, 1.0f);
```

模糊拆成水平和垂直两次，是可分离卷积思路，比直接做二维大卷积采样次数更少。

选择 Compute 的原因：

- 每个输出像素可以独立并行处理；
- 直接通过 `RWTexture2D` 写结果；
- 不需要顶点、三角形、深度测试和 Blend；
- 可以安排到 Compute Queue，并在硬件与调度条件允许时和 Graphics 工作重叠。

### 7.1 面试回答

> Bloom 从 HDR 场景颜色中提取超过阈值的高亮像素，在宽高各为原图一半的纹理上，通过水平和垂直两个 Compute Pass 做可分离模糊，最后乘以 Bloom 强度加回 HDR，并交给 Tonemap。Bloom 表现的是大范围、低频的柔和辉光，本来就不需要保留物体边缘和高亮区域的精细像素，因此可以在半分辨率下计算。宽高各减半后像素数量约为原来的四分之一，可以显著减少临时纹理显存、纹理采样量和内存带宽；Tonemap 按归一化 UV 采样 Bloom 纹理时，再由线性过滤将其放大到输出分辨率。代价是小而细的高亮可能变弱或闪烁，因此更完整的实现通常会在降采样时增加预过滤，并使用多级 Bloom 金字塔。

## 8. Tonemap 是什么

场景光照输出 HDR 颜色，数值可能明显大于 1，例如太阳高光可能为 10、50 或更高。普通 SDR 输出不能直接显示这些范围。

Tonemap（色调映射）负责把 HDR 高动态范围压缩到显示器可显示范围，同时尽量保留暗部和高光层次。

当前 `TonemapPS` 的顺序：

```text
采样 HDR Color
  -> 采样并叠加 Bloom
  -> 乘 Exposure
  -> ACESFilm 曲线压缩到 0~1
  -> LinearToSrgb
  -> 写入 Output Color
```

核心代码：

```hlsl
float3 color = hdrColor + bloomColor;
color = ACESFilm(color * exposure);
return float4(LinearToSrgb(color), 1.0f);
```

Tonemap 使用全屏三角形，是因为它要处理整张 HDR 纹理的每个像素，而不是重新处理场景 Mesh：

```text
Draw(3)
  -> FullscreenVS 生成覆盖屏幕的三角形
  -> TonemapPS 对每个屏幕像素采样 HDR/Bloom
  -> 输出 SwapChain 或 FinalOutput Texture
```

这里的“再次绘制”是再次提交一个全屏后处理 Draw，不是再次绘制全部场景对象。

## 9. CPU/GPU Profiling 记录什么

### 9.1 CPU Profiling

Render Graph 在调用 Pass 回调前记录 `steady_clock::now()`，回调结束后计算：

```text
pass.info.cpuMilliseconds
```

它包含 CPU 为该 Pass 录制命令所花的时间，例如：

- 构建 Draw 列表；
- 绑定 Pipeline/Descriptor；
- 向 CommandList/CommandBuffer 记录 Draw、Dispatch 和 Barrier；
- Pass 回调中的 CPU 逻辑。

它不代表 GPU 真正执行该 Pass 的时间。

项目还使用 `CpuTraceSpan` 记录：

```text
SceneRenderer
SceneConstantsUpdate
RenderGraphBuild
RenderGraphExecute
每个 Render Graph Pass
Queue Batch Record/Submit
```

### 9.2 GPU Profiling

每个 Pass 前后写入 GPU Timestamp Query：

```text
BeginPass -> begin timestamp
EndPass   -> end timestamp
GPU duration = end - begin
```

D3D12 使用 Timestamp Query Heap 和 Readback Buffer；Vulkan 使用 `VkQueryPool` 与 `vkCmdWriteTimestamp`。

每条样本至少记录：

```text
Pass 名称
Begin Query
End Query
执行队列 Graphics/Compute
换算后的 GPU 毫秒
```

当前统计会读取例如：

```text
Shadow GPU ms
GBuffer GPU ms
BloomExtract GPU ms
Tonemap GPU ms
Renderer 总 GPU ms
```

CPU/GPU 时间的区别：

```text
CPU 时间：CPU 录制命令用了多久
GPU 时间：GPU 实际执行命令用了多久
```

## 10. Render Graph 与回调的执行关系

```text
Build 阶段
  -> 声明 GBuffer 写 GBuffer/Depth
  -> 保存 callbacks.gbuffer

Compile 阶段
  -> 根据读写关系建立依赖、Barrier 和队列计划

Execute 阶段
  -> 检查资源是否可用
  -> 插入 Aliasing/Resource Barrier
  -> 开始 CPU/GPU Profiling
  -> 调用 callbacks.gbuffer
       -> RenderGBufferPass
       -> BuildOpaqueGeometryDraws
       -> DrawIndexed
  -> 结束 CPU/GPU Profiling
```

## 11. 30 秒面试回答

> Render Graph 是一个基于资源读写关系的渲染任务调度系统。Renderer 为每个 Pass 声明读取和写入的 Texture、Buffer、目标资源状态、执行队列以及执行回调。Compile 阶段根据这些声明建立 RAW、WAR、WAW 依赖，裁剪不影响最终输出的 Pass，计算 transient 资源生命周期和显存复用，并规划 Graphics、Compute 队列同步。Execute 阶段在每个 Pass 前完成资源可用性检查、Aliasing Barrier 和 Resource Barrier，然后调用回调，由 Renderer 记录具体 Draw 或 Dispatch，最后通过 RHI 映射到 D3D12 或 Vulkan。

## 12. 两分钟面试回答

> 在 PrismRender 中，`SceneRenderer` 先更新 Frame、Object、Material 等 GPU 数据，再通过 `SharedRenderGraphFrontend` 注册 Shadow、GBuffer、Deferred Lighting、Bloom、Tonemap 等 Pass。每个 Pass 会声明资源读写和需要的状态，同时保存一个延迟执行的回调。例如 GBuffer Pass 写入 GBuffer 和 Depth，它的回调最终调用 `RenderGBufferPass` 绘制场景 Mesh；Deferred Lighting 读取 GBuffer 并通过全屏三角形生成 HDR；Bloom 使用三个 Compute Dispatch 完成高亮提取、水平模糊和垂直模糊；Tonemap 再通过全屏三角形把 HDR 与 Bloom 合成，并使用曝光和 ACES 曲线转换到最终输出。
>
> Render Graph 在 Compile 阶段根据读写声明建立 RAW、WAR、WAW 依赖，从最终输出反向裁剪无用 Pass，计算资源的 First Use 和 Last Use，用于 transient 资源复用，并建立 Graphics/Compute Queue Batch 和跨队列同步。Execute 阶段会先检查资源是否已被导入或由前置 Pass 产生，再插入 Aliasing Barrier 和 Resource Barrier，添加调试标签和 CPU/GPU 性能采样，最后调用 Pass 回调。回调内部才真正绑定 Pipeline、Descriptor、Render Target 并提交 Draw 或 Dispatch。
>
> 所以 Renderer 负责具体渲染内容，Render Graph 负责 Pass 调度、资源生命周期和同步，RHI 负责将通用命令映射到 Vulkan 或 D3D12。

## 13. 最简记忆版

```text
Renderer 声明：
这个 Pass 读什么、写什么、在哪个队列、执行哪个回调。

Render Graph 计算：
谁先谁后、是否裁剪、何时 Barrier、资源活多久、如何跨队列同步。

Pass 回调执行：
绑定 Pipeline/Descriptor/Attachment，提交 Draw 或 Dispatch。

RHI 完成：
转换为 D3D12 或 Vulkan 命令。
```

## 14. 代码阅读入口

- `src/Renderer/SceneRenderer.cpp`：资源和回调组装、Compile/Execute、Profiling 接线。
- `src/Renderer/SharedRenderGraphFrontend.cpp`：Pass 读写声明和拓扑。
- `src/Renderer/RenderGraph.cpp`：依赖、裁剪、生命周期、队列、Barrier 和执行。
- `src/Renderer/SceneRendererPasses.cpp`：GBuffer、Bloom、Tonemap 等具体回调实现。
- `src/Renderer/RhiPasses.cpp`：几何绘制和全屏三角形共享实现。
- `src/RHI/Profiling/GpuProfiler.cpp`：D3D12/Vulkan Timestamp Query。
- `assets/shaders/PostProcess.hlsl`：Bloom Compute Shader、FullscreenVS 和 TonemapPS。
