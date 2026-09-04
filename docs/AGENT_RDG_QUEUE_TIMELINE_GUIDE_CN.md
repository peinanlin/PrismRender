# PrismRender Stage 17-M：跨队列 GPU 时间轴与 A/B 性能基线

> 后续状态：Stage 17-N 已实现依赖 DAG、传递约简、独立 Queue Batch，以及 D3D12 每队列 Fence 和 Vulkan 每队列 Timeline Semaphore 提交。当前场景已识别一个静态重叠窗口，Vulkan 单次验证观察到重叠；多样本 A/B 仍表明轻量场景的提交成本高于收益，因此默认 `auto` 暂时保持串行。详见 `docs/AGENT_RDG_DAG_QUEUE_BATCH_GUIDE_CN.md`。

## 1. 阶段目标

Stage 17-L 已经能把 Hi-Z 和 Bloom 提交到原生 Compute Queue，但当时只能回答：

```text
Compute Queue 是否被使用：是
Graphics/Compute 是否真的重叠：不知道
多队列是否比单队列更快：不知道
```

Stage 17-M 补齐测量和自动比较闭环：

1. D3D12 分别校准 Graphics/Compute Queue 的 GPU 时钟。
2. Vulkan 依据设备时间戳和队列族有效位建立公共时间轴。
3. GPU Timing Report 记录每个 Pass 的队列、开始时间和结束时间。
4. 计算 Graphics Busy、Compute Busy、Overlap 和 Overlap Ratio。
5. RDG 支持强制 `serial`、`native` 和默认 `auto` 模式。
6. Harness 支持相同场景和构建身份下的串行/多队列 A/B 采样。
7. 输出机器可读的性能比较报告，供 Agent、CI 和开发者判断多队列是否有效。

本阶段不把“使用了 Compute Queue”误写成“已经获得异步计算收益”。真实结果表明，当前依赖图仍然严格串行，两个后端的 Overlap 都是 0。

## 2. 文件变更

### 2.1 新增文件

- `docs/AGENT_RDG_QUEUE_TIMELINE_GUIDE_CN.md`
- `examples/harness/rdg_queue_ab.jsonl`

### 2.2 主要修改文件

- `src/Renderer/GpuProfiler.h`
- `src/Renderer/GpuProfiler.cpp`
- `src/Renderer/GpuTimingReport.h`
- `src/Renderer/GpuTimingReport.cpp`
- `src/Renderer/RenderGraph.h`
- `src/Renderer/RenderGraph.cpp`
- `src/Renderer/RenderGraphDiagnostics.cpp`
- `src/Renderer/D3D12SceneRenderer.h`
- `src/Renderer/D3D12SceneRenderer.cpp`
- `src/Renderer/VulkanSceneRenderer.h`
- `src/Renderer/VulkanSceneRenderer.cpp`
- `src/Core/Application.cpp`
- `src/Core/VulkanApplication.cpp`
- `src/Automation/HarnessTools.h`
- `src/Automation/HarnessTools.cpp`
- `tests/RenderGraphTests.cpp`
- `tests/EngineHarnessTests.cpp`
- `docs/LEARNING_GUIDE_CN.md`
- `docs/AGENT_RDG_ASYNC_COMPUTE_GUIDE_CN.md`

## 3. 总体数据流

```text
Harness JSONL
  -> queueMode = serial/native
  -> PRISM_RENDER_RDG_QUEUE_MODE
  -> SceneRenderer / VulkanSceneRenderer
  -> RenderGraph::SetQueueExecutionMode
  -> RDG Pass 在 Graphics 或 Compute Queue 执行
  -> GpuProfiler 写入每个 Pass 的 Begin/End Timestamp
  -> 后端把两个 Queue 的 Timestamp 转到同一帧时间轴
  -> PrismGpuTimingReport v2
  -> Harness 聚合多次样本
  -> PrismQueueModeComparison v1
```

A/B 的两组样本固定以下身份：

- Graphics API
- GPU、驱动和 API 版本
- CPU
- Debug/Release 构建配置
- 编译器和架构
- 可执行文件 Hash
- Shader Revision
- World Hash
- Asset Manifest Hash
- 分辨率和捕获阶段

只有身份一致，串行与多队列结果才可比较。

## 4. RDG 队列执行模式

`RenderGraph` 新增：

```cpp
enum class QueueExecutionMode
{
    Automatic,
    Serial,
    Native
};
```

含义如下：

| 模式 | 行为 |
| --- | --- |
| `auto` | Stage 17-N 起暂时选择串行，等待基于历史 GPU 成本的收益模型 |
| `serial` | 所有 Pass 都在 Graphics Queue 执行 |
| `native` | 请求使用原生队列计划；硬件能力不足时仍安全回退 |

渲染子进程通过环境变量选择模式：

```text
PRISM_RENDER_RDG_QUEUE_MODE=auto
PRISM_RENDER_RDG_QUEUE_MODE=serial
PRISM_RENDER_RDG_QUEUE_MODE=native
```

`serial` 是 A/B 的控制组。Compute Pass 的 Shader 和 Dispatch 不变，只改变提交 Queue，因此不会混入算法和画质差异。

RDG Report 升级到 v5，并新增：

```json
{
  "requestedQueueExecutionMode": "serial|native|auto",
  "queueExecutionMode": "serial_fallback|native_multi_queue"
}
```

前者表示调用者请求，后者表示最终实际执行结果。

## 5. D3D12 跨队列时钟校准

### 5.1 为什么不能直接相减

Graphics Queue 和 Compute Queue 可能具有不同 Timestamp Frequency，也不能假设两个计数器的零点相同。

如果把 Compute Timestamp 直接除以 Graphics Frequency，单个 Pass 耗时和跨队列起止位置都可能错误。

### 5.2 初始化

`GpuProfiler::Initialize(D3D12Context&)` 分别读取：

```cpp
graphicsQueue->GetTimestampFrequency(...)
computeQueue->GetTimestampFrequency(...)
QueryPerformanceFrequency(...)
```

### 5.3 每帧校准

每帧开始时分别调用：

```cpp
graphicsQueue->GetClockCalibration(&gpuTimestamp, &cpuTimestamp);
computeQueue->GetClockCalibration(&gpuTimestamp, &cpuTimestamp);
```

`cpuTimestamp` 位于同一个 QPC 时钟域。任意 Queue Timestamp 可以转换为 QPC：

```text
gpuDelta = sampleGpu - calibrationGpu

sampleQpc =
    calibrationQpc
    + gpuDelta * qpcFrequency / queueGpuFrequency
```

最后减去本帧 Graphics 起始 QPC，得到统一的 `startMilliseconds` 和 `endMilliseconds`。

### 5.4 单 Pass 耗时

单 Pass 时长仍使用该 Pass 所属 Queue 自己的频率：

```text
durationMs =
    (endGpu - beginGpu) * 1000 / queueGpuFrequency
```

这样“Pass 自身耗时”和“Pass 在整帧中的位置”分别采用最合适的计算方式。

## 6. Vulkan 跨队列时间轴

Vulkan 使用物理设备的：

```text
VkPhysicalDeviceLimits::timestampPeriod
VkQueueFamilyProperties::timestampValidBits
```

Graphics 与 Compute Queue Family 的公共有效位取较小值。时间戳差值按有效位 Mask 展开：

```text
delta = (end - begin) & timestampMask
milliseconds = delta * timestampPeriod / 1,000,000
```

每个 Pass 相对帧起点的位置也使用相同设备时间戳域计算。

报告中的校准方法为：

```text
vulkan_device_timestamp
```

当前实现用于同一物理设备上的 Graphics/Compute Queue。以后若要把 GPU 时间与 CPU Trace 做绝对时间关联，可继续接入 `VK_EXT_calibrated_timestamps`。

## 7. GPU Timing Report v2

每个 Pass 现在包含：

```json
{
  "name": "BloomExtract",
  "gpuMilliseconds": 0.0065,
  "queue": "Compute",
  "startMilliseconds": 1.8947,
  "endMilliseconds": 1.9011,
  "calibrated": true
}
```

`timeline` 包含：

```json
{
  "available": true,
  "crossQueueCalibrated": true,
  "calibrationMethod": "d3d12_get_clock_calibration_qpc",
  "frameGpuMilliseconds": 3.7351,
  "graphicsBusyMilliseconds": 0.1241,
  "computeBusyMilliseconds": 0.0414,
  "overlapMilliseconds": 0.0,
  "overlapRatio": 0.0,
  "computeOverlapRatio": 0.0
}
```

计算过程：

1. 排除总计用的 `Renderer` 区间。
2. 分别收集 Graphics 和 Compute Pass 区间。
3. 合并同一 Queue 内重叠或相连的区间。
4. 两组区间求交集，得到 `overlapMilliseconds`。

两个比例的含义：

```text
overlapRatio =
    overlapMilliseconds / frameGpuMilliseconds

computeOverlapRatio =
    overlapMilliseconds / computeBusyMilliseconds
```

第一个回答“整帧有多少比例发生双队列重叠”，第二个回答“Compute 工作中有多少被 Graphics 工作覆盖”。

## 8. Harness 命令

### 8.1 单模式性能采样

原有 `performance.measure` 新增：

```json
{
  "queueMode": "serial"
}
```

结果新增：

```text
queueTimelineStatistics.frameGpuMilliseconds
queueTimelineStatistics.graphicsBusyMilliseconds
queueTimelineStatistics.computeBusyMilliseconds
queueTimelineStatistics.overlapMilliseconds
queueTimelineStatistics.overlapRatio
queueTimelineStatistics.computeOverlapRatio
```

### 8.2 自动 A/B

新增命令：

```json
{
  "requestId": "queue-ab-d3d12",
  "command": "performance.compare_queue_modes",
  "arguments": {
    "api": "d3d12",
    "width": 640,
    "height": 360,
    "useCurrentWorld": false,
    "warmupCount": 1,
    "sampleCount": 5,
    "maximumRegressionPercent": 5.0,
    "enforce": false,
    "reportPath": "automation/reports/queue-ab-d3d12.json"
  }
}
```

命令内部执行：

```text
同一 World/Asset/Build Identity
  -> performance.measure(queueMode=serial)
  -> performance.measure(queueMode=native)
  -> 比较 Renderer GPU median/P95
  -> 汇总 native Compute Busy 和 Overlap
  -> 写入 PrismQueueModeComparison
```

主要结果字段：

```text
identityMatches
crossQueueCalibrated
nativeMultiQueueObserved
serialMedianGpuMilliseconds
nativeMedianGpuMilliseconds
medianRegressionPercent
p95RegressionPercent
nativeComputeBusyMilliseconds
nativeOverlapMilliseconds
nativeOverlapRatio
nativeComputeOverlapRatio
passed
```

`enforce` 默认关闭，因为多队列是否更快取决于 RDG 依赖、工作粒度、GPU 和驱动。CI 在建立稳定样本后可以打开它。

## 9. Agent 能力声明

`engine.describe` 现在返回：

```text
crossQueueGpuTimestamps true
queueModeComparison     true
```

并公开：

```text
performance.compare_queue_modes
```

Agent 不需要解析控制台文本，可以直接根据结构化字段判断：

- 是否真的切换到 Compute Queue。
- 两个 Queue 的时间戳是否可比。
- 是否产生工作重叠。
- native 相对 serial 是提升还是回退。

## 10. 自动测试

`RenderGraphTests` 验证：

- RDG Report v5。
- `auto` 模式能执行 Graphics/Compute/Graphics 三段。
- `serial` 模式不会切换 Queue。
- `serial` 模式下 Compute Pass 实际在 Graphics Queue 执行。
- 串行模式不会错误声明多队列 Timestamp。

`EngineHarnessTests` 验证：

- GPU Timing Report v2。
- Pass Queue 和校准字段。
- Graphics/Compute 区间交集计算。
- Harness 新命令与能力声明。

完整测试：

```powershell
ctest --test-dir build-windows-ci -C Debug --output-on-failure
```

结果：

```text
8/8 tests passed
```

## 11. 真实双 API 验证结果

验证硬件：

```text
GPU  NVIDIA GeForce RTX 5060
CPU  Intel Core i5-13600KF
配置 Debug
尺寸 640 x 360
样本 每种模式 1 次
```

单样本只用于验证链路，不应作为发布性能结论。

### 11.1 D3D12

```text
crossQueueCalibrated          true
nativeMultiQueueObserved      true
serial Renderer GPU           0.169 ms
native Renderer GPU           3.735 ms
native Compute Busy           0.041 ms
native Overlap                0.000 ms
native Compute Overlap Ratio  0
```

### 11.2 Vulkan

```text
crossQueueCalibrated          true
timestampValidBits            64
nativeMultiQueueObserved      true
serial Renderer GPU           0.153 ms
native Renderer GPU           0.528 ms
native Compute Busy           0.062 ms
native Overlap                0.000 ms
native Compute Overlap Ratio  0
```

报告路径：

- `automation/reports/queue-ab-d3d12-startup.json`
- `automation/reports/queue-ab-vulkan-startup.json`

## 12. 如何理解当前结果

当前帧图是：

```text
Graphics:
Shadow -> GBuffer -> Deferred
                         |
                         v
Compute:
                    Hi-Z -> Bloom
                                  |
                                  v
Graphics:
                              Tonemap
```

Compute 同时依赖 GBuffer Depth 和 Deferred HDR；Tonemap 又依赖 Compute Bloom。因此队列边界是：

```text
Graphics 完全完成
  -> Compute 才开始
  -> Compute 完全完成
  -> Graphics 才继续
```

这会产生两次同步和命令提交开销，却没有可覆盖这些开销的并行 Graphics 工作。D3D12 的空隙尤其明显。

因此本阶段结论是：

```text
多队列基础设施：正确运行
跨队列测量：可信
当前调度的异步收益：没有
```

这不是测量失败，而是测量系统成功发现了当前优化无效。

## 13. 设计取舍

### 13.1 为什么控制组是真实串行执行

控制组不能删除 Hi-Z 或 Bloom，也不能换 Shader。否则结果同时包含功能差异。

`serial` 只把相同命令录制到 Graphics Queue，所以比较的是 Queue 调度成本。

### 13.2 为什么比较 GPU Renderer 而不是进程墙钟

每个 Harness 样本会重新创建窗口、设备、Pipeline 和 Shader。进程墙钟主要由初始化占据，无法反映小于 1 ms 的队列变化。

A/B 主指标因此使用 `Renderer` GPU Timestamp。进程墙钟和 CPU Trace仍保留用于定位初始化或提交开销。

### 13.3 为什么不自动宣布 native 失败

新命令默认 `enforce=false`，先记录事实。稳定基线应使用：

- Release 构建。
- 至少 1 到 3 次 Warmup。
- 至少 5 到 20 个样本。
- 固定 GPU 电源状态和后台负载。

之后再启用回归阈值。

## 14. 推荐阅读顺序

1. `src/Renderer/RenderGraph.h`
2. `RenderGraph::ParseQueueExecutionMode`
3. `RenderGraph::Execute`
4. `src/Renderer/GpuProfiler.h`
5. `GpuProfiler::BeginFrame(D3D12Context&)`
6. `GpuProfiler::ReadCompletedD3D12Frame`
7. `GpuProfiler::Initialize(VulkanContext&)`
8. `GpuProfiler::ReadCompletedVulkanFrame`
9. `src/Renderer/GpuTimingReport.cpp`
10. `HarnessTools::MeasurePerformance`
11. `HarnessTools::CompareQueueModes`
12. `tests/RenderGraphTests.cpp`
13. `tests/EngineHarnessTests.cpp`
14. `examples/harness/rdg_queue_ab.jsonl`

## 15. 下一阶段

下一阶段不应继续增加 Queue 数量，而应让 RDG 找到真正可重叠的工作：

1. 将队列计划从“按 Pass 顺序切换”升级为依赖 DAG 调度。
2. 生成每个 Queue 的独立 Command Batch。
3. 只在具体跨队列资源依赖点执行 Signal/Wait。
4. 让与 Bloom 无关的 Graphics 工作覆盖 Compute 区间。
5. 评估把 Hi-Z 移到下一帧消费，减少当前帧关键路径。
6. 增加帧间资源版本和双缓冲，避免读写冲突。
7. 在 Release 构建上用 5 到 20 个样本重新建立 A/B 基线。
8. A/B 证明有效后，再进入 Compute Culling、ExecuteIndirect 和 GPU Driven Draw。

只有 `nativeOverlapMilliseconds > 0`，并且 `Renderer` GPU median/P95 不回退，才能把当前状态升级为“异步计算优化有效”。
