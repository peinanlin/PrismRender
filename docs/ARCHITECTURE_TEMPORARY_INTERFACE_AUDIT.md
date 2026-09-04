# 架构重构临时接口审计

P8 9.1 审计结论：生产运行时没有 Host 回指、live renderer/backend 跨 lane 访问、旧 RenderScene mailbox frame overload、逐 Pass `std::async` 或第二套 inline 渲染实现。

## 已删除或已收口

- Host 的 runtime backend/coordinator/renderer/target alias 在 `Initialize` 完成前清空；Run、resize、scene switch、asset work、capture、report、drain 和 Shutdown 均通过 `RenderExecutionService`。
- SceneRenderer 只消费 `RenderFramePacket/RenderSceneView`；旧逐帧 mutable RenderScene render overload 与 mailbox legacy envelope 已删除。
- RenderGraph compiler/executor 的 facade 迁移别名和执行状态重复项已清理；worker recording 不再使用 `std::async/std::future`。
- UI backend 不读取 `ImGui::GetDrawData()`；Render lane 只消费复制且带纹理 lease 的 `UiDrawPacket`。

## 有意保留

- `ApplicationHost` 的 startup-only 非拥有 alias：只用于 target inline 初始化、renderer 资源创建和 UI callback 组合；进入 Run 前全部置空。把这些构造参数再包装成新 builder 不减少运行时耦合，留作组合根实现细节。
- `PRISM_RENDER_SCENE_PUBLICATION_MODE=full-rebuild`：版本化发布的诊断/恢复路径，与默认 versioned 共用 packet/renderer 实现。
- `PRISM_RENDER_EXECUTION_MODE=inline` 和 `PRISM_RENDER_TASK_EXECUTOR=inline`：低延迟、故障隔离及兼容回退；不是 legacy 渲染路径。
- `RenderSettings` 的 legacy Ocean 字段同步：旧场景/配置文件兼容边界，WaveWorks/Ocean migration 仍是独立 change，不能在本架构 change 中删除。
- `RenderScene::EditRenderObjectsForFullRebuild`：导入/工厂的显式保守逃生入口，会推进 raw-mutation stamp，不是无跟踪可写数组。

主入口、HPWater/WaveWorks/Fluid 算法、shader 和 Demo 默认值没有因清理而修改。
