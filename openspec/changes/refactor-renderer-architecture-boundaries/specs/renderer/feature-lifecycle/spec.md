## Purpose

为渲染功能的接入方提供可验证的生命周期、作用域、图数据交换及失败处理契约，使新增功能可以接入既有渲染阶段，同时保持共享模拟只执行一次、多视图历史隔离和已有输出兼容。

## ADDED Requirements

### Requirement: Scoped lifecycle registration

渲染系统 SHALL 接受具有稳定标识、设备共享或视图局部作用域、初始化、帧准备、图构建及释放操作的功能注册；重复标识、缺失必要依赖和依赖环 MUST 在提交相关 GPU 工作前被明确拒绝。未声明可选操作的功能 SHALL 不被要求提供无意义的实现。

#### Scenario: Register a valid rendering feature
- **WHEN** 接入方注册一个依赖完整且标识唯一的功能并启用对应渲染阶段
- **THEN** 系统在首次准备和执行前完成初始化，并只向该功能提供声明作用域内的输入

#### Scenario: Reject invalid registration
- **WHEN** 注册包含重复标识、未满足的必要依赖或依赖环
- **THEN** 系统报告涉及的功能标识与原因，且不提交该无效图的 GPU 工作

### Requirement: Deterministic stage participation

渲染系统 SHALL 根据已解析的有效设置和能力选择功能阶段，保持确定的阶段顺序与资源依赖；无关功能的注册不得改变现有有效功能的输入和执行语义。接入方 SHALL 能在已存在的扩展阶段声明新功能的类型化输入输出，而无需扩展所有功能共享的资源与回调字段全集。

#### Scenario: Add an independent feature
- **WHEN** 接入方在现有扩展阶段接入新的独立后处理功能
- **THEN** 系统接受其自有输入输出声明，既有功能的数据契约保持不变，声明之外的输出不受影响

#### Scenario: Disable HPWater optics
- **WHEN** 运行不启用 HPWater 光学的既有海洋场景
- **THEN** 不调度 HPWater 专用光学 Pass，且保留该场景既有的模拟与光学输出

### Requirement: Shared frame execution

对每个单调递增的逻辑帧标识，渲染系统 SHALL 为存在活跃消费者的已启用共享模拟执行一次帧准备及一次模拟工作链，并向该帧的消费者提供同一已排序、连贯的资源版本；帧槽索引回绕不得被解释为新旧逻辑帧相同。

#### Scenario: Render two views
- **WHEN** Game 与 Editor 两个视图在同一逻辑帧消费共享海洋模拟
- **THEN** FFT、泡沫及局部波模拟链各执行一次，两个视图消费同一模拟版本而不再次推进模拟时间

#### Scenario: Change active views
- **WHEN** Editor 视图隐藏、按需刷新或重新显示
- **THEN** 活跃渲染消费者获得有效共享输出，不重复执行模拟，不采样尚未完成生产的版本

#### Scenario: Suspend all consumers
- **WHEN** 当前逻辑帧没有任何需要该共享模拟的活跃视图
- **THEN** 系统不提交该模拟工作链，恢复消费时仍遵循既有时间推进政策

### Requirement: View-local state isolation

系统 SHALL 按稳定视图标识分别维护相机、尺寸、历史有效性和光学临时资源；视图局部的 Resize、相机切换或历史重置不得隐式清除其他视图的有效历史或设备共享模拟状态。

#### Scenario: Reset one view history
- **WHEN** 一个视图发生相机切换而另一个视图输入未变
- **THEN** 仅失效该视图相关的时域历史，共享海洋谱和另一个视图历史仍遵循各自原有有效性条件

### Requirement: Declared graph data exchange

功能输出 SHALL 带有资源类型、当前图代次/版本与生产者信息，并在消费者执行前满足声明的读写依赖。访问缺失的必要输出、过期图句柄、错误类型或冲突生产者 MUST 产生可归因的错误；可选输入 SHALL 使用显式声明的后备路径。

#### Scenario: Consume feature output
- **WHEN** 水光学功能向透明物体和后处理发布合成深度、HDR 和运动数据
- **THEN** 消费者获得本图的有效版本，折射仍读取未被水覆盖的 opaque 深度，临时资源不以过期句柄传入下一图

#### Scenario: Missing or stale input
- **WHEN** 功能请求不存在的必要输入或上一个图代次的资源
- **THEN** 系统在执行前报告输入与功能标识，不静默绑定空资源或前一帧句柄

### Requirement: Safe failure and resource retirement

初始化失败、功能禁用、视图销毁或应用退出时，系统 SHALL 对已成功取得的资源执行可重复调用的清理；仍被在途 GPU 工作引用的资源 MUST 保持有效，直到对应完成条件满足。CPU 对象析构或帧号增加本身不得替代 GPU 完成证明。

#### Scenario: Fail during initialization
- **WHEN** 某个功能在部分依赖已初始化后创建资源失败
- **THEN** 已完成的初始化按依赖的逆序清理，失败路径不访问未创建资源，也不重复销毁已有资源

#### Scenario: Resize with work in flight
- **WHEN** 功能因质量或尺寸变化替换 GPU 资源，旧资源仍被先前提交引用
- **THEN** 新帧使用新资源，旧资源在确认完成后才回收，两个后端均不出现无效描述符或资源引用

### Requirement: Execution lane affinity and isolated recording

系统 SHALL 为 Feature 可变状态、帧上下文、建图、GPU 上传和提交指定唯一渲染执行 lane；inline 模式和独立渲染线程模式 SHALL 使用相同生命周期实现。常驻 worker SHALL 只执行明确允许并行且具有独立上下文的录制任务，不得并发修改 Feature 共享状态、调用主线程 UI 或直接提交 GPU。

#### Scenario: Record passes with a persistent pool
- **WHEN** 多个允许并行的 Pass 被交给常驻 worker 且完成顺序与图顺序不同
- **THEN** 渲染 lane 等待任务组完成后按原编译计划收集结果，Pass/batch/fence 顺序、输入与共享模拟次数不变

#### Scenario: Fail a recording task
- **WHEN** 任一录制任务失败或无法创建独立上下文
- **THEN** 系统结束或取消同组任务并保活其引用直到退出，报告错误且不提交部分失败帧，不遗留阻塞等待者

### Requirement: Bounded task lifetime

工作线程数和待执行任务 SHALL 有明确上限；队列背压、任务异常、初始化失败及退出 MUST 不造成无限增长或同池嵌套等待死锁。停止接收后系统 SHALL 收拢任务并 join worker，再释放其 CPU 数据；GPU 退役仍遵循完成证明。

#### Scenario: Shut down with queued recording work
- **WHEN** 应用退出而 worker 仍持有帧输入或独立命令上下文
- **THEN** 所有任务得到完成或明确取消状态，等待方被唤醒，CPU 引用在任务结束后释放且 GPU 引用不会提前回收

### Requirement: Preserve all completed demo effects

线程模式切换 SHALL 保持全部既有 Demo 的默认设置、固定输入输出、模拟推进、每视图历史及控制动作效果。系统 MUST 保留 inline 参考路径，并在风险代表集的原始基线、inline、inline+pool 与 threaded+pool 同后端静态及必要连续帧验收通过后切换多线程默认值；不得通过降低画质、关闭既有功能、跳过已接受模拟帧或放宽跨后端门限实现通过。此架构 change 的自动代表集明确不运行 `pbf`、`fluid-render`、`fluid-caustics`、`fluid-toon` 四个粒子流体 Demo；若实现直接触及 Fluid 契约，以无窗口模块/图 fixture 验证其作用域与资源规则。

#### Scenario: Compare temporal demos across execution modes
- **WHEN** 验证工具向代表性的水体、TAA、阴影、流送和双视图场景提供相同时间序列与控制动作
- **THEN** 各模式使用相同采样帧及历史重置边界，风险代表 Demo 满足 design V2 的同后端门限且无新增黑帧、错误视图或冻结，四个粒子流体 Demo 不被启动

#### Scenario: Detect a mode-specific regression
- **WHEN** 任一被选入风险代表集的 Demo 图像、连续帧、生命周期或验证层结果超出批准基线
- **THEN** 当前阶段验收失败且不切换默认模式，保存差异证据，不覆盖 golden 或自动放宽门限
