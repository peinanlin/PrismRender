# PrismRender Showcase Scene

## 目标

Showcase Scene 是一个不依赖外部美术资源的程序化展示场景，用于录屏、渲染功能 A/B、性能统计和跨 API 验证。它不会替换 `StartupScene.gltf`，仅在显式启用时创建。

## 启动

构建 `PrismRender` 后运行：

```powershell
.\tools\RunShowcaseScene.ps1
```

切换 Vulkan：

```powershell
.\tools\RunShowcaseScene.ps1 -Api vulkan
```

生成一张确定性 D3D12 捕获并自动退出：

```powershell
.\tools\RunShowcaseScene.ps1 `
    -CapturePath build-windows-ci\showcase\d3d12.png
```

也可以直接设置环境变量：

```powershell
$env:PRISM_RENDER_SHOWCASE_SCENE = "1"
.\build-windows-ci\RelWithDebInfo\PrismRender.exe --api=d3d12
```

## 场景组成

- Hero Stage：金属球、双层基座和高反射地面，用于 PBR、IBL、SSR、阴影和 Tonemap。
- Material Gallery：15 个金属度/粗糙度样本，使用对象级材质参数覆盖。
- Lighting Runway：8 个暖冷点光源和 2 个投射阴影的聚光灯，用于 Clustered Lighting、局部光阴影和 Bloom。
- Instance Field：48 个共享 Mesh/Material 的几何体，用于 GPU Instancing、GPU Driven 和 Draw Call 对比。
- Occlusion Partitions：两块遮挡墙，用于 Hi-Z Occlusion Culling 验证。
- Architectural Frame：立柱、横梁、侧墙和背板，为 CSM、GTAO 和反射提供稳定的几何参照。

当前场景共 100 个 RenderObject，低于渲染器现有的 128 对象预算。

## 文件

新增：

- `src/Scene/ShowcaseSceneFactory.h`
- `src/Scene/ShowcaseSceneFactory.cpp`
- `tools/RunShowcaseScene.ps1`
- `docs/SHOWCASE_SCENE_GUIDE_CN.md`

修改：

- `src/Core/Application.cpp`
- `src/Core/VulkanApplication.cpp`
- `CMakeLists.txt`

## 数据流

1. `PRISM_RENDER_SHOWCASE_SCENE=1` 选择 Showcase 启动源。
2. `ShowcaseSceneFactory` 创建共享 Mesh、运行时 Material、RenderObject 和 Light。
3. Camera 和 Directional Light 由 `ConfigureWorld` 设置。
4. D3D12/Vulkan 继续使用现有 `RenderScene -> SceneRenderer -> RenderGraph` 路径。
5. ImGui Debug Panel 可以直接切换 PBR、IBL、阴影、TAA、GTAO、SSR、GPU Driven 等功能。

如果同时指定 `PRISM_RENDER_WORLD_PATH`，自动化 World Snapshot 在后续阶段具有更高优先级并会替换当前场景。Asset Streaming 也可能在资源就绪后激活其目标场景，因此录制 Showcase 时不应同时启用这两个入口。

## 验证

- 构建 `PrismRender` 的 `RelWithDebInfo` 配置。
- 运行 RenderGraph 与 WorldRenderSceneBridge 单元测试。
- 使用确定性捕获确认场景可完成 D3D12 首帧渲染。
- 录制前启用 D3D12 Debug Layer，确保无错误或资源状态告警。
- 使用相同相机、分辨率和设置分别捕获 D3D12/Vulkan，执行 Golden Image 对比。
