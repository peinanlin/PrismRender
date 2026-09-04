# PrismRender 聚光灯与点光源阴影学习指南

## 1. 本阶段完成了什么

本阶段在同一套 RHI、RDG 和共享 Shader 上实现了：

- 最多 16 个场景聚光灯，其中最多 4 个同时投射阴影。
- 最多 128 个场景点光源，其中最多 4 个同时投射阴影。
- 聚光灯使用二维深度纹理数组。
- 点光源使用 CubeArray，每盏灯占 6 个深度面。
- 两类阴影都进入共享 Deferred PBR 光照。
- D3D12 编译为 DXIL，Vulkan 编译为 SPIR-V。
- `SpotLight`、`PointLight.castsShadow` 进入 ECS、JSON、Reflection 和 Harness 命令。
- RDG 显式追踪本地光常量、阴影绘制常量、两类阴影贴图和状态转换。

实现仍然只有一套算法：

```text
World / Harness JSON
        |
        v
RenderScene PointLight + SpotLight
        |
        v
LocalLightShadows::Update
        |
        +--> SpotLightConstants + 4-layer depth array
        |
        +--> PointShadowConstants + 4 x 6 CubeArray faces
        |
        v
SharedRenderGraphFrontend
        |
        +--> SpotShadows
        +--> PointShadows
        +--> DeferredLighting
        |
        v
Deferred.hlsl
        |
        +--> DXIL --> D3D12
        +--> SPIR-V --> Vulkan
```

## 2. 主要文件

### 新增

- `src/Renderer/Features/LocalLightShadows.h`
- `src/Renderer/Features/LocalLightShadows.cpp`
- `assets/shaders/LocalLightShadow.hlsl`
- `examples/harness/local_light_shadow_validation.jsonl`
- `docs/LOCAL_LIGHT_SHADOWS_GUIDE_CN.md`

### 修改

- `assets/shaders/Deferred.hlsl`
- `src/Renderer/SharedRenderGraphFrontend.h/.cpp`
- `src/Renderer/D3D12SceneRenderer.h`
- `src/Renderer/D3D12SceneRenderer.cpp`
- `src/Renderer/VulkanSceneRenderer.h/.cpp`
- `src/RHI/Vulkan/VulkanResources.cpp`
- `src/Scene/Light.h`
- `src/Scene/RenderScene.h/.cpp`
- `src/Scene/WorldRenderSceneBridge.cpp`
- `src/Engine/Components.h`
- `src/Engine/World.h`
- `src/Engine/Reflection.cpp`
- `src/Engine/CommandSystem.cpp`
- `src/Engine/WorldSerializer.cpp`
- `src/Renderer/GpuProfiler.h`

## 3. 聚光灯阴影如何实现

聚光灯和相机很像，只是视锥是灯照亮的锥体：

1. 灯的位置作为相机位置。
2. 灯的 `direction` 作为观察方向。
3. `outerAngleRadians * 2` 作为完整垂直 FOV。
4. `range` 作为远平面。
5. 从灯的视角把场景深度写入二维数组的一层。

`SpotLightData` 保存：

```text
viewProjection
position + range
direction + outerCos
color + intensity
innerCos + arrayLayer + shadowEnabled + bias
```

Deferred 阶段先判断像素是否位于聚光锥中：

```text
cone = smoothstep(outerCos, innerCos, dot(lightDirection, surfaceDirection))
```

然后把世界坐标乘灯的 ViewProjection，得到阴影 UV 和深度。最终用
`SampleCmpLevelZero` 做 3 x 3 PCF 比较采样。

简单理解：

```text
聚光灯阴影 = 从灯的位置拍一张深度照片
当前像素比照片里的深度更远 = 它被别的物体挡住
```

## 4. 点光源阴影如何实现

点光源向所有方向发光，一张普通二维图无法覆盖 360 度，因此每盏灯需要
6 个方向：

```text
+X, -X, +Y, -Y, +Z, -Z
```

每个方向使用 90 度 FOV，6 张深度图组成一个 Cubemap。4 盏带阴影点光源
合并为一个 `TextureCubeArray`，总共 24 个数组层。

Deferred 阶段使用：

```hlsl
pointShadowMap.SampleCmpLevelZero(
    shadowSampler,
    float4(normalize(lightToSurface), cubeIndex),
    referenceDepth);
```

Cubemap 深度不是线性距离。采样前根据主轴距离恢复与投影矩阵一致的设备深度：

```text
depth = far / (far - near)
      - near * far / ((far - near) * majorAxisDistance)
      - bias
```

这样 D3D12 与 Vulkan 都比较同一种 0 到 1 深度。

## 5. 为什么使用数组资源

如果每盏灯各建一张纹理和一个 Descriptor，灯数增加时绑定会频繁变化。
数组方案让 Deferred Pass 始终只绑定：

- 一个 `Texture2DArray`。
- 一个 `TextureCubeArray`。
- 一个本地光常量 Buffer。

每盏灯只需在常量中保存 `layer` 或 `cubeIndex`。这更接近现代渲染器的
批量资源组织方式，也方便以后升级为 Bindless。

当前限制是最多 4 个带阴影的聚光灯和 4 个带阴影的点光源。场景灯可以更多，
但需要以后用光照重要度选择最值得生成阴影的灯，而不是简单取前 4 个。

## 6. RDG 中的数据流

新增逻辑资源：

```text
LocalLightConstants
LocalShadowRenderConstants
SpotShadowMap
PointShadowMap
```

新增 Pass：

```text
ClusteredLightBuild
        |
        +--> SpotShadows  --> SpotShadowMap
        |
        +--> PointShadows --> PointShadowMap
                               |
GBuffer + GTAO + Cluster Data --+
                               |
                               v
                       DeferredLighting
```

`DeferredLighting` 声明读取两张阴影图，因此 RDG 自动生成：

```text
DepthWrite -> ShaderResource
```

后端不需要手工猜测 Barrier。D3D12 映射为 Resource Barrier，Vulkan 映射为
Image Layout/Access/Stage 转换。

## 7. D3D12 与 Vulkan 后端分别做什么

共享层负责：

- 灯光选择。
- ViewProjection 计算。
- 每物体 WVP 上传。
- RDG Pass 和资源依赖。
- Shader 阴影比较与 PBR 光照。

D3D12 后端只负责：

- D32 Array/CubeArray 资源。
- 每层 DSV 和数组 SRV。
- Descriptor、Pipeline 和 Draw 命令翻译。

Vulkan 后端只负责：

- `VkImage`、二维层 View、CubeArray View。
- Dynamic Rendering 深度 Attachment。
- DescriptorSet、Pipeline 和 Draw 命令翻译。

`VulkanResources.cpp` 特别区分了：

```text
采样 Cube View       -> VK_IMAGE_VIEW_TYPE_CUBE/CUBE_ARRAY
写入单个深度面 View -> VK_IMAGE_VIEW_TYPE_2D
```

Cube 兼容图片不能用 Cube View 直接作为单个深度 Attachment，这是本阶段很重要的
后端细节。

## 8. Agent 如何创建和调试灯光

聚光灯已经注册为可反射组件：

```json
{
  "command": "component.add",
  "arguments": {
    "entity": "实体 UUID",
    "component": "SpotLight",
    "properties": {
      "direction": [0.0, -1.0, 0.0],
      "color": [0.35, 0.55, 1.0],
      "intensity": 8.0,
      "range": 20.0,
      "innerAngleRadians": 0.3,
      "outerAngleRadians": 0.58,
      "castsShadow": true
    }
  }
}
```

位置来自同一实体的 `Transform.position`。AI 可以继续调用：

```text
component.get SpotLight
component.set SpotLight
world.snapshot
render.compare_apis
rdg.describe
```

因此 Debug 不再依赖“猜场景里有什么灯”，而可以直接读取 JSON：

- 灯的位置与方向是否正确。
- 角度是否合法。
- `castsShadow` 是否打开。
- RDG 是否存在 `SpotShadows`、`PointShadows`。
- 双 API 最终截图是否通过。

## 9. 验证方法与结果

### 编译和单元测试

```powershell
cmake --build build-windows-ci --config Debug --target PrismRender PrismHarness PrismShaderCompilerTests PrismRenderGraphTests PrismEngineHarnessTests PrismWorldRenderSceneBridgeTests
.\build-windows-ci\Debug\PrismShaderCompilerTests.exe
.\build-windows-ci\Debug\PrismRenderGraphTests.exe
.\build-windows-ci\Debug\PrismEngineHarnessTests.exe
.\build-windows-ci\Debug\PrismWorldRenderSceneBridgeTests.exe
```

结果：

- Slang 编译 16 个入口、共 32 个 DXIL/SPIR-V 程序。
- Deferred Reflection 找到 `LocalLightConstants`、`spotShadowMap` 和
  `pointShadowMap`。
- RDG 测试确认 16 个 GPU Driven Pass，包含两类局部阴影。
- ECS Command、JSON 往返和 World 到 RenderScene 同步测试通过。

### 双 API 实机运行

```powershell
$env:PRISM_RENDER_MAX_FRAMES='4'
.\build-windows-ci\Debug\PrismRender.exe --api=d3d12
.\build-windows-ci\Debug\PrismRender.exe --api=vulkan
```

两端都连续运行 4 帧并以退出码 0 结束。

### 有几何体的严格像素比较

```powershell
.\build-windows-ci\Debug\PrismHarness.exe `
  --commands examples\harness\local_light_shadow_validation.jsonl `
  --output automation\reports\local-light-shadow-world-validation.jsonl
```

固定场景含地面、立方体、高柱、点光源和聚光灯。结果：

```text
dimensionsMatch   = true
MAE               = 0.0000235844
RMSE              = 0.000309149
changedPixelRatio = 0
maximumError      = 0.0235294
passed            = true
```

截图：

- `automation/captures/local-shadow-d3d12.bmp`
- `automation/captures/local-shadow-vulkan.bmp`

## 10. 本阶段修复的工程问题

新增 Pass 后，默认 GPU Driven 图达到 16 个 Pass。旧 Profiler 只有 32 个 Query：

```text
Frame Begin/End 2 个
每个 Pass Begin/End 2 个
总需求 = 2 + 16 * 2 = 34
```

因此两端都被 `GPU profiler query budget is exhausted` 主动保护拦截。
现在使用：

```text
MaxProfiledPasses = 31
MaxQueriesPerFrame = 2 + MaxProfiledPasses * 2 = 64
```

这个修复说明新增 RDG Pass 时不能只看渲染逻辑，还要同步检查性能工具容量。

## 11. 推荐阅读顺序

1. 先读 `Scene/Light.h`，理解灯光数据。
2. 读 `LocalLightShadows::Update`，跟踪矩阵和常量上传。
3. 读 `LocalLightShadow.hlsl`，理解纯深度 Pass。
4. 读 `Deferred.hlsl` 的 `ComputeSpotShadow`。
5. 再读 `ComputePointShadow`，重点理解 CubeArray 与设备深度。
6. 读 `SharedRenderGraphFrontend.cpp`，画出写深度到读阴影的依赖。
7. 对照 D3D12/Vulkan TextureView 实现。
8. 最后运行 Harness 示例，修改灯光位置、颜色和 `castsShadow` 观察 JSON 与画面。

## 12. 后续扩展

- 按屏幕贡献度选择最重要的 4 盏阴影灯。
- 为点光源增加多采样 PCF，降低硬边。
- 增加 Shadow Atlas，减少固定数组浪费。
- 增加阴影缓存，只重绘发生变化的灯和物体。
- 增加 Spot/Point Shadow 独立捕获 Stage。
- 将局部灯也并入 Clustered Light 列表，统一光照遍历。
- 增加光线追踪阴影作为可选高质量路径。
