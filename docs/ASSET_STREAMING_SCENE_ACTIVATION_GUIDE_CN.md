# Stage 29：资产流送场景激活与双 API 可见性验证

## 1. 本阶段解决的问题

Stage 24 和 Stage 28 已经实现异步 Cooked IO、专用上传队列、
`UploadTicket`、持久 Upload Ring 和 Residency 状态机。但仅有
“8 个资产全部 Resident”仍不能证明场景真的使用了这些资产：

```text
Asset Resident
  != AssetRegistry 已绑定运行时对象
  != RenderScene 已创建 RenderObject
  != GPU 已经绘制
  != D3D12/Vulkan 输出一致
```

本阶段把上述五层连接为一条可自动验证的数据流。

## 2. 文件

新增：

- `src/Scene/AssetStreamingSceneBridge.h/.cpp`
- `src/Scene/SceneFraming.h/.cpp`
- `examples/harness/asset_streaming_scene_validation.jsonl`
- `docs/ASSET_STREAMING_SCENE_ACTIVATION_GUIDE_CN.md`

修改：

- `src/Asset/AssetDatabase.cpp`
- `src/Asset/AssetStreamingManager.h/.cpp`
- `src/Scene/Transform.h/.cpp`
- `src/Core/Application.h/.cpp`
- `src/Core/VulkanApplication.h/.cpp`
- `src/Renderer/VulkanSceneRenderer.h/.cpp`
- `src/Renderer/Features/GpuDrivenVisibility.cpp`
- `src/RHI/D3D12/D3D12Context.h/.cpp`
- `src/RHI/Vulkan/VulkanContext.cpp`
- `tests/EngineHarnessTests.cpp`
- `CMakeLists.txt`

## 3. 导入阶段如何保存场景实例

`AssetDatabase::ImportGltf` 为 Scene 记录保存：

```json
{
  "instances": [
    {
      "name": "Node_2",
      "meshAssetId": "...",
      "materialAssetId": "...",
      "worldMatrix": [16 个 float]
    }
  ]
}
```

Scene 只保存稳定 Asset ID，不保存临时 Registry 下标或 C++ 指针。
重新导入时，源文件未变则 Mesh、Material 和 Scene 的 Asset ID
保持稳定，`importRevision` 增长。

## 4. 运行时激活流程

```text
Request(Scene Asset ID)
  -> 递归请求 Mesh、Material、Texture
  -> 后台读取 Cooked v2
  -> Copy/Transfer Queue
  -> UploadTicket 完成
  -> 全部依赖 Resident
  -> Scene Resident
  -> GetResidentSceneInstances
  -> AssetStreamingSceneBridge::Activate
  -> AssetRegistry 解析 Runtime Mesh/Material
  -> Transform::SetWorldMatrix
  -> RenderScene::AddRenderObject
```

Bridge 先在临时数组中构造所有对象。只有全部依赖与矩阵都有效时
才提交到 `RenderScene`，避免场景只激活一半。

## 5. 引用与 Pin 为什么必须对称

请求 Scene 会递归增加整个依赖闭包的引用。释放 Scene 也必须沿
同一依赖图递归释放，否则 Texture 会永久保持引用而无法被预算
淘汰。

`pinned` 已由单一布尔值改为 `pinReferenceCount`：

```text
Request(pin=true)  -> 每个依赖 pinReferenceCount + 1
Release(unpin=true)-> 每个依赖 pinReferenceCount - 1
```

多个所有者同时 Pin 同一资源时，一个所有者释放不会错误解除另一个
所有者的 Pin。

## 6. Vulkan 运行期场景变化

原来的 `VulkanSceneRenderer` 在初始化时按对象数创建：

- 每帧 Object Constant Buffer；
- 每对象 Material Buffer；
- 每对象每帧 DescriptorSet；
- Shadow DescriptorSet。

流送场景激活后对象数量变化，旧代码会直接报错。本阶段新增
`EnsureSceneResources`，每帧比较对象数量以及 Mesh/Material 指针。
绑定签名变化时重建上述资源；旧 Buffer、DescriptorSet 和 Vulkan
对象通过已有的帧 Fence 延迟回收。

`GpuDrivenVisibility` 也取消初始对象容量的静默截断。场景增长时，
Object Record 与 Indirect Argument Buffer 会按实际对象数重建。

## 7. D3D12 Descriptor 容量

运行验证发现原来的 Shader-visible Heap 仍是教程容量：

```text
SRV/CBV/UAV: 512
Sampler: 64
```

而渲染器支持 128 个对象，每个对象、每帧都有材质 DescriptorSet，
重建期间新旧表还会短暂共存。容量已调整为：

```text
SRV/CBV/UAV: 65,536
Sampler: 2,048
```

分配器仍使用区间分配、延迟回收、合并和高水位统计。这个修改解决
当前容量模型不一致；更大规模材质系统仍应继续演进到 Bindless。

## 8. 为什么截图要延迟一帧

Scene 激活发生在 `BeginFrame` 之后。Vulkan 同一帧还可能发生：

```text
Transfer 完成
-> Timeline Wait
-> Scene 激活
-> Descriptor/Buffer 重建
-> Graphics Submit
```

如果激活后立刻请求截图，捕获可能落在资源首次稳定提交之前。本阶段
在激活后先完整渲染一帧，再请求自动截图。截图前通过
`FrameCameraToRenderObjects` 按世界包围球构图，确保流送对象位于画面
中。这个延迟只用于自动捕获，不给正常交互渲染增加 GPU Wait。

## 9. Agent 可观察报告

新增环境变量：

```text
PRISM_RENDER_ASSET_STREAMING_SCENE_REPORT_PATH
```

报告包含：

- API 名称；
- 激活是否尝试、是否成功；
- 激活对象数和 RenderScene 对象总数；
- `streamedBindingCount`；
- 每个对象的 Mesh/Material Asset Path；
- Runtime Mesh/Material 是否有效；
- 结构化错误码与消息。

因此 Agent 可以区分“IO 成功但未激活”“已激活但路径错误”和
“已绑定且已绘制”。

## 10. 验证结果

Harness 重导入：

```text
Scene importRevision: 7
Scene instances: 1
Cooked v2 assets: 7
Cooked bytes: 1,158,448
```

D3D12 与 Vulkan：

```text
Resident: 8/8
Failed: 0
Activated objects: 1
Streamed bindings: 1
Mesh/Material runtime ready: true
```

截图中预览几何、太阳和流送 Duck 均可见。强制 Golden Image：

```text
MAE: 0.000008
RMSE: 0.000187
Changed pixel ratio: 0.000003
Maximum channel error: 0.039216
```

阈值：

```text
MAE <= 0.001
RMSE <= 0.005
Changed pixel ratio <= 0.001
Pixel tolerance = 4/255
```

自动测试为 `8/8 passed`，其中 `EngineHarnessTests` 还验证了 Scene
实例元数据以及整个依赖闭包的 Request/Release/Pin 对称性。

## 11. 设计边界

当前完成的是整 Scene 粒度的异步激活。尚未在本阶段实现：

- 按相机 Cell、Chunk 或距离自动请求；
- Mesh/Texture 分块与 Mip 分级流送；
- DirectStorage；
- Sparse/Reserved Resource；
- Virtual Texture。

这些扩展应建立在现有稳定 Asset ID、UploadTicket、依赖闭包和
结构化报告之上，不应绕过状态机直接操作后端资源。
