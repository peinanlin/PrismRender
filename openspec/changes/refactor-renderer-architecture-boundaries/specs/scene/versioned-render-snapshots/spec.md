## Purpose

为场景生产者和渲染消费者建立版本化的不可变数据发布契约，在保留编辑、资产流送、双视图和 GPU 可见性行为的同时复用未改变的对象数据，避免相机更新触发完整场景复制。

## ADDED Requirements

### Requirement: Reusable immutable scene data

场景发布服务 SHALL 将已发布对象与绑定数据保持为只读版本，并在相关输入未改变时跨逻辑帧复用同一数据版本；消费端不得通过共享的可变材质或网格对象改变已发布版本的可观察绑定或参数。

#### Scenario: Render an unchanged scene
- **WHEN** 一个已发布的静态场景连续渲染且没有对象或绑定变更
- **THEN** 场景数据版本和对象存储身份保持不变，预热后不再完整复制对象数组或重建场景数据快照

#### Scenario: Hold an older frame
- **WHEN** 消费者仍持有旧帧，而生产者发布了新场景数据版本
- **THEN** 旧帧的对象、参数及资源绑定仍有效且不随新版本被原地改变

### Requirement: Independent per-view inputs

系统 SHALL 在共享场景数据之外提供每视图的相机、逻辑帧时间、视图标识、尺寸、历史失效信号、对象选择及可见性反馈。只修改这些视图输入不得导致完整对象数据复制；所有渲染功能 SHALL 消费与其当前视图一致的动态输入。

#### Scenario: Move only the editor camera
- **WHEN** 用户仅移动 Editor 相机
- **THEN** Editor 输出相应改变，Game 相机与共享场景数据版本保持不变，不发生完整对象数据复制

#### Scenario: Advance water with unchanged objects
- **WHEN** 场景对象不变但逻辑帧时间推进且水模拟未暂停
- **THEN** 海洋模拟与各视图水光学继续更新，场景数据复用不冻结波浪、运动向量或水下介质判定

### Requirement: Complete mutation invalidation

对象增删、层级/变换/可见性修改、材质参数或运行时绑定替换、资产驻留变化、场景加载/切换、undo/redo 和事务提交 SHALL 在下一次有效渲染消费前发布匹配的新数据版本。连续一批编辑 SHALL 在提交边界合并，未提交事务不得发布混合状态。

#### Scenario: Edit material and transform in one transaction
- **WHEN** 用户提交同时改变材质参数和变换的事务
- **THEN** 下一次发布同时包含两项修改，不展示仅含其中一项的中间状态

#### Scenario: Replace a streamed asset
- **WHEN** GPU 上传完成后运行时资产绑定被替换，或资产被驱逐为后备资源
- **THEN** 下一次有效消费使用新的有效绑定，已有旧帧仍可安全持有旧绑定

#### Scenario: Restore a scene state
- **WHEN** undo、redo、journal replay、文件加载或场景切换恢复对象状态
- **THEN** 系统产生相应的新场景版本并应用正确的视图历史失效信号，不复用不兼容的旧数据

### Requirement: Stable object identity and selection

快照 SHALL 为对象、层级关系、实例和间接绘制提供一致的标识及索引映射；每视图隐藏编辑器辅助物或选择地形节点不得改变共享数据中已有对象的身份。GPU 反馈 SHALL 标记来源视图和场景数据版本。

#### Scenario: Compare game and editor terrain selection
- **WHEN** 两个视图对同一地形层级使用不同显示策略
- **THEN** 各自选择映射到同一快照中的正确节点，不因筛选改变父子索引、实例偏移或间接绘制绑定

#### Scenario: Receive late visibility feedback
- **WHEN** 场景已切换而 GPU 返回旧版本的可见性结果
- **THEN** 系统拒绝将结果应用到新场景的对象或错误视图，不污染当前编辑器显示

### Requirement: Safe publication and bounded retention

发布操作 SHALL 交付一个自洽的数据版本与视图输入组合；图执行和异步录制引用的 CPU 数据 MUST 保活到引用结束，GPU 资源 MUST 继续遵循后端完成条件。发布服务不得无限保存已经没有消费者的历史版本。

#### Scenario: Switch scenes while earlier work is retained
- **WHEN** 生产者切换场景而先前帧仍有 CPU 或 GPU 消费者
- **THEN** 新消费者只获得新场景，旧消费者安全完成后旧版本可被释放，不产生悬空捕获或无界历史积累

### Requirement: Measurable reuse and correctness fallback

系统 SHALL 公开快照构建次数、复用次数、对象复制量及对应场景版本的诊断数据，并支持迁移验收使用完整重建路径进行等输入对照。对于无法确定失效范围的变更，系统 MUST 保守地重建而非复用可能过期的数据；后备路径的使用 SHALL 可被诊断发现。

#### Scenario: Validate the reuse path
- **WHEN** 验证工具以相同场景、相机、时间和设置分别运行完整重建与版本复用路径
- **THEN** 两条路径产生等价对象/绑定/视图输入与兼容的渲染结果，诊断准确报告两者复制量差异

#### Scenario: Encounter an unclassified mutation
- **WHEN** 迁移中的调用方报告了无法细分的场景变更
- **THEN** 下一次发布执行保守重建并记录原因，不静默沿用旧版本

### Requirement: Ordered bounded frame handoff

跨线程帧交接 SHALL 使用有界 FIFO，对每个已接受的 LogicalFrameId 恰好执行一次且保持接受顺序；队列满时 SHALL 背压而非替换旧帧。时间、步长、视图设置及历史信号 MUST 来自包内不可变值，不得取消费时的最新可变场景或墙钟。

#### Scenario: Producer overtakes the render consumer
- **WHEN** 主线程准备下一帧而渲染线程尚未完成前帧且队列已满
- **THEN** 生产者受背压且仍能处理必要窗口事件，不丢弃已接受帧、不重复推进模拟、不无限积累数据

#### Scenario: Render a retained frame after a new camera edit
- **WHEN** 主线程已修改相机并发布新帧而消费者仍处理旧帧
- **THEN** 旧帧使用自己的相机/设置，历史引用前一实际渲染帧，新相机不原地污染旧输入

### Requirement: Reliable frame-bound controls and completion

resize、场景切换、reset、quality、capture 和停机命令 SHALL 具有有序标识、目标帧/epoch 及成功或失败确认，不能通过可丢弃的最新统计通道传送。捕获报告和完成反馈 SHALL 标记实际渲染帧及场景/视图版本；仅生产帧或退出 main loop 不得伪造渲染完成。

#### Scenario: Capture following a resize and reset
- **WHEN** 用户按顺序请求 resize、history reset 和目标帧捕获
- **THEN** 消费者按约定帧边界执行并确认控制，截图/报告来自正确尺寸和历史 epoch 的目标帧

#### Scenario: Reject stale completion
- **WHEN** 场景或视图已替换后收到旧 epoch 的 GPU 反馈或捕获完成
- **THEN** 系统仅将其归属原请求，不更新新视图对象或将新请求误判成功

### Requirement: Immutable UI and asset boundary

跨线程 UI 绘制数据 SHALL 以独立只读副本及纹理保活凭证交接，不得借用可变 ImGui context 的临时内存；UI 数据类型 MUST 不引入 Scene 对 UI 的逆向依赖。资产上传 SHALL 在渲染 lane 的安全点完成，再通过绑定 revision 提交影响后续场景帧，不得就地覆盖已发布帧绑定。

#### Scenario: Edit UI while a previous draw is retained
- **WHEN** 主线程开始下一次 UI 构建而旧帧 UI 尚未消费
- **THEN** 旧 UI 顶点/索引/命令与纹理仍有效，渲染线程不读取被主线程修改的上下文

#### Scenario: Publish a completed streamed binding
- **WHEN** IO worker 完成解码且渲染 lane 完成上传
- **THEN** 主线程收到有序完成消息后发布新的有效绑定 revision，旧消费者继续持有旧绑定直到其 CPU/GPU 消费结束

### Requirement: Failure-aware shutdown

关闭或渲染失败 SHALL 唤醒等待帧空间、帧完成及控制确认的所有参与方。正常退出 SHALL drain 已接受工作；失败取消的未提交帧 MUST 明确失败，不能产生成功截图确认。工作线程、GPU、渲染 owner 和窗口 SHALL 按各自完成及亲和约束释放。

#### Scenario: Renderer fails while producer is blocked
- **WHEN** 生产者因满队列等待且渲染线程发生初始化、录制或设备错误
- **THEN** 生产者和命令等待者收到失败并退出等待，所有已启动任务被收拢，不因等待互相依赖的主线程回调而死锁
