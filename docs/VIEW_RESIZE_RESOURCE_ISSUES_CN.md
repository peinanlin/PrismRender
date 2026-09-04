# PrismRender 视图尺寸变化与资源重建问题记录

> 记录日期：2026-08-15  
> 状态：仅完成问题分析，尚未实施修改  
> 用途：供后续独立对话制定方案和实施改造

## 1. 结论

Game View、Scene View 或窗口尺寸改变时，重新创建尺寸相关 GPU 资源是正常且必要的。GPU Texture 创建之后不能直接改变宽高，因此新的渲染尺寸需要新的 Texture，或者从资源池取得一个尺寸兼容的 Texture。

当前 PrismRender 的主要问题不是“发生了资源重建”，而是：

1. 窗口、Game View 和 Scene View 的尺寸及资源生命周期耦合过紧。
2. 尺寸变化触发的重建范围过大。
3. 普通宽高变化会重新创建不依赖宽高的 Pipeline。
4. 连续拖动窗口或编辑器面板时缺少防抖、容量复用和延迟回收策略。
5. SwapChain Resize 与离屏 RenderView Resize 尚未形成清晰的职责边界。

UE 同样会在 View Extent 变化时重新取得 SceneColor、Depth、GBuffer、Velocity、HiZ/HZB 和时序历史等尺寸相关资源，但不会因此重新创建 Shader、Mesh、Material 等视图无关资源。其 RDG、Render Target Pool 和 transient allocator 会尽量复用底层显存。

## 2. 当前实现的调用链

窗口 Resize 事件目前由以下路径处理：

```text
Window::ConsumeResize
    -> IFrameContext::Resize
        -> D3D12Context::Resize
            -> WaitForGpu
            -> ReleaseSizeDependentResources
            -> IDXGISwapChain::ResizeBuffers
            -> 重建 BackBuffer RTV 和窗口 Depth
    -> Game SceneRenderer::RecreateSwapChainResources
    -> Scene View SceneRenderer::RecreateSwapChainResources
    -> 更新 ImGui 纹理及相机参数
```

相关代码入口：

- `src/Core/ApplicationHost.cpp:671`
- `src/Core/ApplicationHost.cpp:986`
- `src/Core/ApplicationHost.cpp:1159`
- `src/RHI/D3D12/D3D12Context.cpp:185`
- `src/RHI/Vulkan/VulkanContext.cpp:492`
- `src/Renderer/SceneRenderer.cpp:407`
- `src/Renderer/SceneRendererResources.cpp:58`

`SceneRenderer::RecreateSwapChainResources` 当前执行：

```text
释放 SwapChain/尺寸相关资源
    -> 重置 Depth、HiZ 状态
    -> 重新创建所有尺寸相关资源
    -> 重新创建 Pass DescriptorSet
    -> 重新创建 Pipeline
    -> 更新队列代价模型尺寸
```

## 3. 已确认的问题

### RV-01：Game View 与 Scene View 使用同一个 Frame Extent

严重程度：高（架构问题）

`SceneRenderer::CreateSizeDependentResources` 从 `IFrameContext::GetFrameWidth/Height` 取得尺寸。这意味着 SceneRenderer 的渲染尺寸实际上由窗口/SwapChain 尺寸决定，而不是由各自编辑器面板的实际尺寸决定。

影响：

- Game View 与 Scene View 无法真正拥有独立分辨率。
- 缩放其中一个面板时，不能只调整对应 View。
- 小面板仍可能按整个窗口分辨率渲染，浪费 fill rate、带宽和显存。
- 后续实现独立 DPI、动态分辨率、固定游戏分辨率或多视口会受到限制。

建议：

- 为每个 View 保存独立的 logical extent、allocated extent 和 output texture。
- SceneRenderer 不再从全局 FrameContext 查询离屏渲染尺寸，而是由 `RenderView` 或 Render 参数显式传入。

### RV-02：一次窗口变化同时重建 Game 与 Scene 两套资源

严重程度：高（性能与职责耦合）

`ApplicationHost` 在收到 Window Resize 后，无条件调用 Game Renderer 和 Scene View Renderer 的 `RecreateSwapChainResources`。

影响：

- 即使只有一个 View 的显示区域发生变化，另一个 View 也会被重建。
- 两套 GBuffer、HDR、HiZ、后处理和时序资源会在同一帧集中释放与创建。
- 编辑器拖动分隔条或窗口边缘时容易产生明显卡顿和显存峰值。

建议：

- Window Resize 只负责 SwapChain。
- Game/Scene 面板根据各自内容区域分别提交 `RequestResize(width, height)`。
- 每帧边界只对 pending extent 确实发生变化的 View 应用 Resize。

### RV-03：单纯宽高变化会重新创建 Pipeline

严重程度：中高（不必要的 CPU 开销和缓存扰动）

`SceneRenderer::RecreateSwapChainResources` 在重新创建纹理和描述符后调用 `CreatePipeline(backend)`。

普通 D3D12/Vulkan Pipeline 的兼容性由 Shader、Render Target Format、Depth Format、Sample Count、Blend/Raster/Depth State 等决定，不由 Render Target 的宽高决定。Viewport 和 Scissor 本身可以动态更新。

影响：

- Resize 时产生无必要的 PSO 查询或创建工作。
- 如果缓存未完全命中，可能发生 Shader/Pipeline 编译卡顿。
- 将“资源尺寸生命周期”与“渲染状态生命周期”混在同一接口中。

建议：

- Extent-only Resize 不调用 `CreatePipeline`。
- 只有 Format、MSAA Sample Count、渲染路径或其他 Pipeline 兼容条件变化时才更新对应 PSO variant。

### RV-04：SwapChain Resize 使用全 GPU 空闲，离屏资源也被绑定在该停顿之后重建

严重程度：中高（交互卡顿）

`D3D12Context::Resize` 和 `VulkanContext::Resize` 当前均调用 `WaitForGpu()`。对 SwapChain BackBuffer 做 Resize 时，先确保旧 BackBuffer 不再被 GPU 引用是正确且保守的实现；但当前 Game/Scene 离屏资源重建紧跟同一个全局流程，未利用项目已有的按帧资源退休机制。

影响：

- CPU 必须等待所有已提交 GPU 工作完成。
- 连续 Resize 会反复清空 CPU/GPU 并行流水。
- 离屏 View Resize 也无法独立采用 fence 延迟回收。

建议：

- 第一阶段保留 SwapChain Resize 的安全等待，先将其与 RenderView Resize 分离。
- 离屏纹理使用 frames-in-flight fence 或现有 retired-resources 机制延迟释放，避免全局 `WaitForGpu()`。
- 后续再评估 SwapChain Resize 是否需要更细粒度等待；不能在没有生命周期证明时直接删除同步。

### RV-05：连续 Resize 缺少防抖、尺寸量化和容量复用

严重程度：中（性能问题）

当前只要收到一个不同的非零窗口尺寸，就立刻按新宽高执行 Resize。编辑器面板或窗口被鼠标拖动时，尺寸可能连续逐像素变化。

影响：

- 每次鼠标移动都可能创建新纹理、Texture View 和 DescriptorSet。
- 资源频繁申请、释放，造成 CPU 开销、显存抖动和潜在碎片。
- 即使尺寸只改变少量像素，也会重建整套屏幕空间资源。

建议：

- 将分配尺寸向上对齐到 8、16、32 或 64 像素，具体粒度通过测试确定。
- 区分 logical extent 与 allocated extent；逻辑尺寸缩小时优先保留较大的已有纹理，只更新 viewport/scissor。
- 增长超过 allocated extent 时立即扩容；收缩在尺寸稳定若干帧后再执行。
- 合并同一帧或连续帧中的多个 Resize 请求，只应用最新尺寸。

### RV-06：Transient Texture Pool 随每次尺寸变化整体重建

严重程度：中（资源复用不足）

`CreateSizeDependentResources` 会调用 `CreateTransientTexturePool`，而释放尺寸资源时会 reset 当前 pool。因此每次 Resize 都按新尺寸重建整套 transient 请求和视图。

影响：

- 当前 transient alias 能优化单一尺寸下的帧内资源，但不能充分复用跨 Resize 的历史分配。
- 连续在相近尺寸之间变化时，仍会发生较大的资源创建压力。

建议：

- 保留现有帧内 alias 设计。
- 在更高层增加按资源描述键控的缓存/池：`width + height + format + usage + mipCount + sampleCount`。
- 或让 allocator 支持容量型 extent，逻辑 View Rect 小于等于物理 Texture Extent。
- 旧 pool 通过 fence 延迟退休，不要在仍可能被 GPU 引用时立即销毁。

### RV-07：`RecreateSwapChainResources` 命名与实际职责不一致

严重程度：中（可维护性问题）

该接口不仅处理 SwapChain 引用，还处理 SceneRenderer 的 GBuffer、HDR、Bloom、HiZ、各 Feature 资源、DescriptorSet、Pipeline 和代价模型。

影响：

- 调用者无法从接口名判断实际成本和副作用。
- Window/SwapChain 生命周期和 RenderView 生命周期难以独立演进。
- 后续增加多 View、离屏预览或独立动态分辨率时容易继续扩大耦合。

建议拆分：

```cpp
IFrameContext::ResizeSwapChain(width, height);
RenderView::RequestResize(width, height);
RenderView::ApplyPendingResize(IRenderBackend& backend);
SceneRenderer::ResizeViewResources(RenderView& view);
SceneRenderer::RebuildPipelines(const PipelineCompatibilityKey& key);
```

具体命名可在实施时根据现有类职责调整，不要求机械照搬以上接口。

## 4. Resize 时应重建和不应重建的内容

| 类别 | Extent 变化时的预期处理 |
|---|---|
| SwapChain BackBuffer | 仅窗口/交换链尺寸变化时 Resize |
| 窗口 Depth | 跟随 SwapChain Resize |
| View Final Output | 跟随对应 View Resize |
| GBuffer、HDR、Velocity、View Depth | 跟随对应 View Resize |
| HiZ/HZB | 重建并标记历史无效 |
| TAA/SSR 等历史 | 重建、失效，或未来实现兼容的历史缩放 |
| Bloom、SSAO、SSR、Fluid 等屏幕空间资源 | 按 Feature 的分辨率规则调整 |
| Viewport、Scissor、相机 Aspect | 更新数值，不需要重建 GPU Pipeline |
| DescriptorSet/View | 只更新引用了新纹理的 View 级描述符 |
| Shader、Root Signature/Pipeline Layout | 不因宽高变化重建 |
| PSO/Pipeline | 宽高变化不重建；兼容格式或 Sample Count 变化时才重建 |
| Mesh、Material、普通 Texture、Sampler | 不重建 |
| Shadow Map | 通常保持其独立配置尺寸，不跟随 View Resize |
| Exposure 等固定小纹理 | 尺寸无关时保留 |

## 5. 当前已经正确、后续必须保持的行为

以下内容不是问题，改造时不得丢失：

1. D3D12 Resize 会忽略零尺寸以及与当前完全相同的尺寸。
2. HiZ 在尺寸资源重建后设置为无效，避免使用旧分辨率的遮挡历史。
3. `TemporalAntiAliasing::Resize` 会重新创建 Motion/Resolved/History 纹理，并调用 `ResetHistory()`。
4. 新纹理创建后会重新建立对应 Texture View 和 DescriptorSet。
5. Viewport、相机宽高比以及 ImGui 展示纹理需要在 Resize 后同步更新。
6. 旧资源只有在 GPU 不再使用之后才能释放；优化同步不等于删除同步。

## 6. 建议的目标数据流

```text
GLFW Window Resize
    -> ResizeSwapChain
    -> 更新窗口 BackBuffer/Depth

ImGui Game 面板 ContentRegion 变化
    -> GameRenderView::RequestResize
    -> 帧边界 ApplyPendingResize
    -> 只调整 Game View 资源和历史

ImGui Scene 面板 ContentRegion 变化
    -> SceneRenderView::RequestResize
    -> 帧边界 ApplyPendingResize
    -> 只调整 Scene View 资源和历史

Shared Renderer Resources
    -> Shader / PSO Cache / Mesh / Material / Environment
    -> 不跟随单个 View Extent 重建
```

建议每个 View 至少保存：

```cpp
struct RenderViewExtent
{
    uint32_t logicalWidth = 0;
    uint32_t logicalHeight = 0;
    uint32_t allocatedWidth = 0;
    uint32_t allocatedHeight = 0;
    bool resizePending = false;
};
```

## 7. 后续实施顺序建议

1. 先增加测试/统计，记录 Resize 次数、等待耗时、纹理创建数和 PSO 创建数。
2. 将 `CreatePipeline` 从 extent-only 重建路径移除，并验证 D3D12/Vulkan 输出一致。
3. 将 SwapChain Resize 与 RenderView Resize 拆开。
4. 为 Game View 和 Scene View 引入独立 logical extent。
5. 只重建发生变化的 View，并正确更新 ImGui Texture、相机和历史状态。
6. 加入 pending resize 合并、尺寸对齐和 shrink 延迟。
7. 接入 fence 延迟退休，消除离屏 View Resize 的全 GPU 等待。
8. 最后评估跨 Resize 的资源池复用和容量型 Texture Extent。

## 8. 验收标准

改造完成后至少验证：

- 调整 Scene 面板尺寸时，Game View 资源创建计数不增加。
- 调整 Game 面板尺寸时，Scene View 资源创建计数不增加。
- 仅改变宽高时，PSO 创建计数不增加。
- 连续拖动面板时，实际资源重建次数明显少于 Resize 事件次数。
- Resize 后首帧不会采样旧 HiZ、TAA 或 SSR 历史。
- 0×0、最小化、恢复、DPI 改变和快速连续 Resize 不崩溃。
- D3D12 Debug Layer 无资源仍在使用、状态错误或描述符生命周期错误。
- Vulkan Validation Layer 无 SwapChain、Image View 或同步相关错误。
- Game/Scene 使用不同宽高比时，各自相机投影和画面显示正确。
- Resize 前后没有持续性显存增长。

## 9. 后续新对话可直接使用的任务描述

```text
请阅读 docs/VIEW_RESIZE_RESOURCE_ISSUES_CN.md，结合当前代码重新核实每个问题，
然后按文档第 7 节的顺序增量改造。先给出涉及文件、数据流、风险和验证方案，
再实施第一批改动。不要一次性重写渲染器，并保持 D3D12/Vulkan 行为一致。
```

