# P0 可选 Feature 资源绑定审计

所属 change：`refactor-renderer-architecture-boundaries`，任务 1.17。用户已授权统一处理关闭状态的同类绑定问题；本项不改变算法、shader、Demo 默认设置，也不代表后续架构重构已经完成。

## 约束与检查方法

纹理已分配不等于当前视图/当前帧有有效输出。先核对 producer 条件，再核对所有静态 descriptor consumer 及图中访问。关闭分支可采用已初始化后备，也可保留已有显式 SRV 状态访问并由 shader 开关禁止语义读取；不能仅修改 CPU 标志后仍绑定 Undefined 图像。每帧只改 fence 已允许复用的当前帧槽，resize 遵守已有 GPU idle 契约。

## Producer / consumer / 图访问清单

| Feature | 有效生产条件与消费者 | 关闭路径 / 本轮处理 |
| --- | --- | --- |
| GTAO | deferred && gtao；DeferredLighting binding 24、SSR binding 20 | **修复**：ScreenSpaceEffects owner 上传全白 1×1 AO 后备，两个消费者在当前帧槽选择；启用仍走原 GTAO Pass。重建 SSR 集合先放后备。 |
| Spectral Ocean | fftOcean && renderWater && SpectralOcean；普通材质/海洋材质 binding 36–39 | **修复**：初始化与当前帧更新均包含 renderWater，关闭时用已有 ZeroArray / UpNormalArray；不修改共享模拟或 FFT。 |
| Local waves | 上述 spectral 条件 && local.enabled && 已初始化；binding 40–41 | **修复**：不再仅以 IsInitialized 选择真实纹理。隐藏海面/关闭 local 使用已有初始化后备；保留共享首视图生产。 |
| Ocean GPU query 的 disabled local 输入 | local.enabled && GPU ready；OceanGpuQuery binding 18 | **修复**：原有零值 1×1 后备从首次帧中查询时的惰性创建提前至 SceneRenderer 初始化，保证 BeginFrame 刷新上传后才被查询采样。零值/格式/查询算法均不变。 |
| Legacy FFT | fftOcean && renderWater && LegacyFft；binding 30–31 | 初始化/更新选择同步补充 renderWater，关闭沿用已初始化 VT atlas；没有改 FFT 数值、资源格式或模拟步骤。 |
| PlanarReflection | deferred && planar；SSR binding 21 | 1.16 已修复黑色后备；本轮继续与 GTAO 独立开关和 resize 回归。 |
| SkyAtmosphere | physicalAtmosphere && skybox；材质、Deferred、Sky、Tonemap、HPWater | 1.13 已修复黑色后备、当前帧槽；本轮保留完整消费者测试。 |
| 方向/局部/方差阴影 | 各阴影开关；Opaque、Deferred、Transparent、Water | SharedRenderGraphFrontend 对 Opaque/Deferred 静态反射到的阴影纹理已有无条件 SRV 访问，先于 Transparent/Water；shader 标志避免关闭分支语义读取。保留该布局策略，不新增强制生产 Pass。 |
| SSR composite | deferred && (SSR || planar) | 后处理/ TAA 按同条件选 composite，否则选 HDR；本轮 optional fixture 开关验证。 |
| TAA | deferred && TAA && !fluid | 后处理跳过关闭输出；启用时图声明 history 读取，reset 标志控制历史使用；不修改历史算法。 |
| Bloom | bloom；Tonemap binding 17 | 正常 Tonemap 关闭时绑定当前有效 scene color，图不执行 Bloom；调试 capture 也按当前 producer 条件选择。 |
| 中间纹理 capture | GBuffer 要求 deferred、Bloom 要求 bloom、Water 要求有效 HPWater producer | **修复**：关闭或保留旧资源均不再选择无 producer 的中间纹理，沿用原先 Water 尚未分配时的当前 scene color 后备。不强制启用效果；这种后备图不应标为有效的该中间阶段 golden。 |
| Clustered lighting | clustered；Forward/Deferred/Water 的 buffer 输入 | 原开关和 buffer 图访问保持；optional fixture 首次关闭、往返和 resize。不能由图像布局通过推导未启用 buffer 的内容有效。 |
| Fluid / foam / caustics | fluid 及子开关；Fluid composite | 关闭整个 Fluid 时后处理绕过其输出；Fluid composite 对 foam/caustics 保留显式 SRV 访问，EndFrame 保留实际读状态。**但全目录实测 PBF/native 暴露另一处 General→SRV 错误，不能据此宣称整个 Fluid 路径安全，详见下文。** |
| HPWater / caustics / volumetrics | HpWater && SpectralOcean && fftOcean && renderWater | 主 composite 对存在的 caustics/volumetric 输出已有 SRV 访问，即使对应计算关闭；子开关控制语义采样。原光学 Pass/历史不改。 |
| InteractiveTerrain / VT | terrain 开关；材质/地形绘制 | 关闭材质绑定使用已上传 VT atlas；启用依赖原 brush/erosion 图访问。保留原策略，全 Demo 捕获覆盖现有地形配置。 |
| GPU visibility / HiZ | visibility/HiZ 条件及历史有效位 | HiZReady 将 mip 链转为 SRV，消费使用既有有效位和图访问；不改变剔除、readback 或历史。 |

## 文件与数据流

- 修改 `src/Renderer/Features/ScreenSpaceEffects.{h,cpp}`：每视图后备 owner、AO 选择接口、SSR 当前帧槽绑定。
- 修改 `src/Renderer/SceneRenderer.cpp`：Deferred AO、材质海洋纹理及中间纹理 capture 的有效生产条件。所有动态选择在原安全点、原 descriptor 集合中完成。
- 修改 `tests/SkyAtmosphereGpuTests.cpp`、`CMakeLists.txt`：沿用真实双视图 fixture，增加 optional/ocean 模式；8 阶段，每阶段覆盖所有帧槽，320×200→400×250→320×200。
- 新增 `scripts/Test-OptionalFeatureBindings.ps1`：可重放 main/capture 严格矩阵，独立输出、GPU 锁、显式校验层及实际启用检查、二进制 hash 与结果索引。`-Suite main` 是 default/optional/ocean/water 四类 fixture × native/serial；`-Suite capture` 是 Bloom 关闭/forward GBuffer/HPWater 隐藏及尺寸重建，默认两后端。无 Editor 可指定其 BinaryPath 和 `-Apis vulkan`。CTest 注册 optional/ocean/water 各后端测试。
- 新增本文档；同步 proposal/design/tasks 和验收记录。无新增生产模块。
- 数据流：有效设置 → 原 producer 条件 / 当前安全帧槽 descriptor → 原渲染 Pass；关闭输出指向初始化后备，不改变共享资源 owner 或增加模拟。

## 证据与限制

证据根：`artifacts/architecture-refactor/20260828-hpwater-complete/`，所有运行独立目录，旧 golden 和恢复点不覆盖。

- `P0/gpu/ocean-bindings-before-01/`：修复前 hidden water + allocated local 在 Vulkan phase 0 严格校验失败，保留原日志。
- `P0/gpu/capture-bindings-before-01/`：Bloom 关闭和 forward GBuffer capture 分别在 phase 0 复现 Undefined→SRV 校验失败，保留两份日志。
- `P0/gpu/optional-capture-strict-01/`：5/6 通过，Vulkan water 在首次启用 phase 1 失败；`water-enable-isolation-01` 不启用 capture 时 native/serial 也失败。`water-enable-trace-01` 精确定位 `0xb2a0000000b2a` 为 OceanGpuQuery binding 18 的 1×1 后备，不是水 GBuffer。临时绑定追踪已删除，VulkanResources.cpp 恢复到上一快照同 hash；修复仅移动 SceneRenderer 中该后备的创建时机。
- `P0/gpu/optional-bindings-strict-01/`：GTAO 初次修复后的 default/optional 双后端 native 共四轮通过；不是最后 Ocean 修复的完整验收。
- `P0/gpu/optional-bindings-strict-02/`：default/optional/ocean × 两后端 × native/serial，12 轮通过（capture 补充修复之前）。Vulkan 显式 Khronos validation，无 validation 消息；D3D12 仅既有 820 性能 warning。
- `P0/gpu/optional-capture-strict-02/`：最后 query fallback 修复后，三类 capture × 双后端 **6/6 通过**；包括原先 phase 1 失败的 water 模式。
- `P0/gpu/optional-final-strict-01/`：最后生产二进制四类 fixture × 双后端 × native/serial **16/16 通过**，二进制 hash 和环境逐项留存。开启算法、shader 及数值门限未修改。
- 最终 CPU 为完整版 11/11、无 Editor 10/10；常规 GPU 为 16/16、8/8。Vulkan 常规 CTest 不强制 layer，因此显式 strict 结论只引用上述专门矩阵。
- 最终 D3D12 20 Demo capture/strict 通过，对上一修复基线 19/20 图像通过，PBF 超门限；HPWater 对原始二进制的最终同后端门限两 API 通过。详见 `docs/ARCHITECTURE_REFACTOR_ACCEPTANCE.md` 的逐项结果；完整 P0 与 1.17 仍不勾选。

严格校验不等于完整视觉/时域/性能验收；Linux 环境、完整 P0 矩阵和 P1–P8 保持未完成。

### 全目录检查发现的独立阻碍（未修改其实现）

`P0/vulkan/optional-all-demos-native-01/` 前 16 场景 capture/strict 通过，PBF 第 17 项失败，剩余三项没有在此轮执行。前 16 项对原始二进制同队列图像均通过严格门限；其中 HPWater 默认视角本次逐像素一致。

PBF 的六层图像 `0x4e000000004e` 实际为 General、descriptor 要求 ShaderReadOnly，不再是关闭 producer 的 Undefined 输出。`optional-pbf-serial-01` 同场景 serial strict 通过。源码显示 Fluid.Environment 首次被图声明在 Compute，而同一 environment 还由普通材质/Deferred 的静态 descriptor 采样；`RenderGraph::ExecuteQueueBatches` 会在 graphics prologue 将首用 Compute 的已初始化纹理转为 Common（Vulkan General）。这指向未完整声明的共享只读输入/队列交接问题；尚未逐原生句柄追踪，不把候选图像身份当作已独立证明。

此问题涉及已开启 Fluid 的图依赖/跨队列状态，不属于本轮关闭状态绑定修复；没有修改 RenderGraph、Fluid 算法或切换默认 serial 来放行。需另行明确纳入 P0 后处理，1.17 的全目录验收因此保持未完成。

原始 native 与修复后 serial 的 PBF 图像诊断也未过同后端门限：MAE=0.000195、RMSE=0.004161、changed=0.001955、SSIM=0.997015，见 `P0/comparisons/optional-vulkan-pbf-serial-diagnostic-01/`。这是混合队列条件的诊断，不是等输入回归验收，也不能用它证明新绑定修复改变了 PBF 算法；门限未放宽。

最后无 Editor strict 8/8 通过，连同主矩阵 16 项和 capture 6 项共 30 项。最终 Vulkan 四个 Fluid/native 均失败；D3D12 PBF 同一二进制三次重复 capture/strict 通过，但两组图像对比均超过门限，直接复现运行间波动。完整数值、路径和恢复点见验收记录。1.17 仍未闭环，不把本审计当作全 Demo 视觉/时域/性能验收通过。

后续用户已批准将两项阻碍纳入 1.18/1.19。现已补齐共享 Environment 图依赖，四个 Fluid × 双 API × native/serial 严格校验通过；PBF 桶内 ID 排序后 GPU 数据和真实 PBF 截图三次重放均一致，Vulkan Toon 三次截图也一致。HPWater 默认视角原图两 API 通过，但 PBF 双 API 和 Toon Vulkan 对旧图仍超原门限，尚未批准替换基线。以上旧失败描述为历史记录，最新证据见 `docs/FLUID_P0_VALIDATION.md`；1.17 全目录验收仍不勾选。

最新闭环：用户已明确批准三项固定配置的新参考，并保留全部旧图/失败记录。最终生产二进制 `C42D75E6...DCF4` 的 `P0/{d3d12,vulkan}/approved-final-all-demos-native-01/` 各 20/20 capture/strict 通过；对应 `approved-final-all-demos-{d3d12,vulkan}-01` 比较各 20/20 原 V2 通过，仅三项使用登记的批准参考。D3D12 HPWater 单独对原始二进制 SSIM=.999998、changed=.000020，通过；Vulkan HPWater 在全目录中直接对原始图通过。结合最终 16+8 双视图开关/帧槽/resize strict，1.17 已完成；这是前置绑定修复闭环，不是 P0 全时域/性能或架构迁移完成。
