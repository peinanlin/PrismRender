# 独立 RHI Thread 决策

结论：本 change **不实现独立 RHI thread，也不建议立即新建实现 change**。

## 对照条件

- Windows Release、D3D12、Preview、确定性输入、双视图、同一 1280×800/Headless 条件；
- 每种模式 60 帧，去除前 10 帧；功能/图像门禁另由双后端 HPWater 和代表 Demo 覆盖；
- 模式为显式 `inline+inline`、`inline+pool`、`threaded+pool`；证据位于 `artifacts/architecture-refactor/p7-mode-matrix-20260831/profile_*.jsonl`；
- 这些是 matched profiler 样本，不是 GPU 锁频后的绝对 FPS 宣称。

## 代表中位数

| 模式 | Producer/Editor Loop | Main active | Render active | Main completion wait | Input→Present |
|---|---:|---:|---:|---:|---:|
| inline + inline | 17.20 ms | 7.40 ms | 16.71 ms（Main 子区间） | ~0 ms | 17.19 ms |
| inline + pool | 17.43 ms | 7.00 ms | 17.07 ms（Main 子区间） | ~0 ms | 17.41 ms |
| threaded + pool | 17.69 ms | 约 0.2–0.6 ms | 约 6.5 ms active + 11.0 ms wait | 约 17.17 ms | 约 34.9 ms |

threaded 模式将 ownership 和工作从 Main 分离成功，但当前 Preview 的 Render lane 仍由视图渲染及 GPU/frame pacing 主导。代表帧的 `BeginFrame` 约 10.89 ms，Game/Scene render 约 0.63/5.66 ms；真正接近 API submit/Present 的 CPU 区间仅约 0.24 ms，其中 native Present 约 0.14 ms、submit 约 0.07 ms。RenderGraph build/execute 约 5.63/0.41 ms，前者是 renderer graph construction，不是可单独搬到 RHI thread 的 API 翻译队列。

## 决策依据

独立 RHI thread 只有在“API 命令翻译/提交是可分离的主 CPU 瓶颈”时才可能抵消额外 command packet、同步、错误传播和一帧队列延迟。当前约 0.24 ms 的 submit/Present CPU 区间不满足该条件；主要等待属于 GPU/frame pacing，主要 active 工作属于 Scene render/graph build。再加一条线程不会缩短这些路径，却会扩大帧身份、descriptor/PSO/cache、capture、resize 与 device-lost 协议面。

后续只有在代表场景中连续观察到以下信号，才应另立 OpenSpec change：Render lane 的 API translation/submit 持续占关键路径、GPU 非饱和、Main/worker 已有空闲、拆分后的排队延迟预算可接受。新 change 必须重新定义 command packet ownership、RHI resource lifetime、device error/resize/drain 协议及低延迟 fallback。
