# PrismRender 光线追踪 RHI 功能基线学习指南

## 1. 本阶段完成了什么

本阶段不是只增加一个 `supportsRayTracing` 布尔值，而是打通了以下真实链路：

1. 公共 RHI 能描述 BLAS、TLAS、三角形几何、实例和构建偏好。
2. D3D12 查询 DXR Tier，Vulkan 查询并启用光追扩展与 Feature。
3. 两个后端都能向驱动查询 Result、Build Scratch 和 Update Scratch 大小。
4. 两个后端都能创建并实际构建一个三角形 BLAS 和一个单实例 TLAS。
5. 公共 Buffer Usage 能表达 AS Storage、Build Input、Scratch 和 Shader Binding Table。
6. 公共资源状态能表达 Acceleration Structure 读写同步。
7. Slang 能将同一套 RayGen、Miss、ClosestHit Shader 编译为 DXIL 和 SPIR-V。
8. Slang Reflection 能识别 TLAS Descriptor 和输出 UAV。
9. 程序能输出结构化 JSON 探针报告，供 Agent 和 CI 验证。

## 2. 先理解几个名词

### 2.1 BLAS

BLAS 是 Bottom-Level Acceleration Structure。

可以简单理解为：

```text
一个 Mesh 的三角形
    -> 驱动构建空间索引
    -> 光线可以快速判断碰到了哪个三角形
```

BLAS 主要描述几何，不描述物体在世界中的多个摆放位置。

### 2.2 TLAS

TLAS 是 Top-Level Acceleration Structure。

它保存 BLAS 实例：

```text
同一个 Cube BLAS
    -> 实例 A：放在左边
    -> 实例 B：放在右边并旋转
    -> TLAS 统一管理这些实例
```

光线通常先查询 TLAS，再进入命中的 BLAS。

### 2.3 Scratch Buffer

驱动构建 BLAS/TLAS 时需要临时工作区，这就是 Scratch Buffer。

它不是最终加速结构，只在构建或更新过程中使用。所需大小不能靠引擎猜测，必须先向驱动查询。

### 2.4 Shader Binding Table

Shader Binding Table，简称 SBT，用于把 RayGen、Miss、Hit Group 与局部参数组织起来。

本阶段已经加入 SBT Buffer Usage 和 Shader 阶段，但还没有实现完整的公共 Ray Tracing Pipeline 与 `DispatchRays`。

## 3. 公共 RHI 是怎样设计的

核心文件：

- `src/RHI/RayTracing.h`
- `src/RHI/RayTracing.cpp`
- `src/RHI/IGraphicsDevice.h`
- `src/RHI/GraphicsResources.h`
- `src/RHI/GraphicsTypes.h`

### 3.1 公共构建描述

`AccelerationStructureBuildDescription` 只描述规模和策略：

```cpp
AccelerationStructureBuildDescription description;
description.type = AccelerationStructureType::BottomLevel;
description.geometries.push_back({
    vertexCount,
    vertexStride,
    indexCount,
    IndexFormat::UInt32,
    RayTracingGeometryFlags::Opaque});
description.flags =
    AccelerationStructureBuildFlags::PreferFastTrace
    | AccelerationStructureBuildFlags::AllowUpdate;
```

这份描述可以先用于查询驱动内存大小。

### 3.2 公共构建请求

`AccelerationStructureBuildRequest` 在描述之外增加真实 Buffer：

```text
BuildDescription
    + Vertex Buffer
    + Index Buffer
    + Buffer Offset
    + TLAS Instance
    -> IGraphicsDevice::CreateAccelerationStructure()
```

公共接口不会暴露 `ID3D12Resource*` 或 `VkBuffer`。

### 3.3 公共加速结构对象

`IRayTracingAccelerationStructure` 提供：

- `GetDescription()`：原始构建描述；
- `GetBuildSizes()`：驱动返回的内存大小；
- `GetDeviceAddress()`：BLAS/TLAS 的 GPU 地址；
- `GetGraphicsApi()`：对象所属后端。

TLAS 构建时通过公共 GPU 地址引用 BLAS，因此前端不用知道 DXR 和 Vulkan 的实例结构差异。

### 3.4 严格校验

`ValidateAccelerationStructureBuildDescription()` 检查：

- BLAS 至少有一个三角形几何；
- TLAS 至少有一个实例；
- 索引数或非索引顶点数必须组成 Triangle List；
- `PreferFastTrace` 与 `PreferFastBuild` 不能同时开启。

`ValidateAccelerationStructureBuildRequest()` 继续检查：

- Buffer 必须属于当前图形 API；
- Vertex/Index Buffer 必须带 `AccelerationStructureBuildInput`；
- 几何数量和预构建描述必须一致；
- TLAS 只能引用 BLAS；
- Instance ID 和 Hit Group Offset 不得超过 24 位。

这些错误会在进入驱动前被发现。

## 4. D3D12 / DXR 后端如何实现

核心文件：

- `src/RHI/D3D12/D3D12Context.cpp`
- `src/RHI/D3D12/D3D12GraphicsDevice.cpp`
- `src/RHI/D3D12/D3D12Resources.cpp`
- `src/RHI/D3D12/D3D12TypeConversions.cpp`

### 4.1 能力查询

初始化时调用：

```text
ID3D12Device::CheckFeatureSupport
    D3D12_FEATURE_D3D12_OPTIONS5
        -> RaytracingTier
```

映射规则：

- DXR Tier 1.0：支持 AS 和 Ray Tracing Pipeline；
- DXR Tier 1.1：再开放公共 `rayQuery`；
- 不支持时保持 `RayTracingTier::Unsupported`。

### 4.2 查询构建大小

公共描述被翻译为：

```text
D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS
    + D3D12_RAYTRACING_GEOMETRY_DESC[]
    -> ID3D12Device5::GetRaytracingAccelerationStructurePrebuildInfo()
```

结果映射回公共 `AccelerationStructureBuildSizes`。

### 4.3 实际构建

D3D12 后端执行：

```text
创建 Result Buffer
    State = RAYTRACING_ACCELERATION_STRUCTURE

创建 Scratch Buffer
    State = UNORDERED_ACCESS

填入 Vertex/Index GPU Virtual Address
    -> ID3D12GraphicsCommandList4::BuildRaytracingAccelerationStructure()
    -> UAV Barrier
```

TLAS 会先把公共实例转换成 `D3D12_RAYTRACING_INSTANCE_DESC`，再上传到 CPU-To-GPU Buffer。

## 5. Vulkan 后端如何实现

核心文件：

- `third_party/glad/include/glad/vulkan.h`
- `src/RHI/Vulkan/VulkanContext.cpp`
- `src/RHI/Vulkan/VulkanResources.cpp`
- `src/RHI/Vulkan/VulkanTypeConversions.cpp`

### 5.1 GLAD 为什么需要更新

旧 GLAD 只包含 Vulkan 1.3 Core、Surface 和 Swapchain，完全没有 Vulkan 光追扩展的类型和函数。

本阶段重新生成 GLAD，加入：

- `VK_KHR_acceleration_structure`
- `VK_KHR_deferred_host_operations`
- `VK_KHR_ray_tracing_pipeline`
- `VK_KHR_ray_query`

如果 Loader 不包含这些扩展，仅在 C++ 中写扩展名仍然无法调用光追函数。

### 5.2 扩展和 Feature 链

Vulkan 先枚举扩展，再按支持情况组成 `pNext` 链：

```text
VkPhysicalDeviceVulkan12Features
    -> VkPhysicalDeviceVulkan13Features
    -> VkPhysicalDeviceAccelerationStructureFeaturesKHR
    -> VkPhysicalDeviceRayTracingPipelineFeaturesKHR
    -> VkPhysicalDeviceRayQueryFeaturesKHR
```

创建逻辑设备时只启用实际支持的扩展和 Feature，非光追显卡仍能继续运行普通 Vulkan 渲染。

### 5.3 Device Address

AS 输入、Scratch 和 SBT Buffer 都需要 GPU 地址。

因此 Vulkan Buffer 会加入：

```text
VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
```

再通过 `vkGetBufferDeviceAddress()` 得到地址。

### 5.4 实际构建

Vulkan 后端执行：

```text
创建 AS Storage Buffer
    -> vkCreateAccelerationStructureKHR()

创建 Scratch Buffer
    -> 取得 Device Address

准备 VkAccelerationStructureGeometryKHR
    -> vkCmdBuildAccelerationStructuresKHR()
    -> AS Write 到 AS/Shader Read 的 Memory Barrier
```

TLAS 实例会转换为 `VkAccelerationStructureInstanceKHR`。

## 6. Slang Shader 跨 API 流程

共享 Shader：

- `assets/shaders/RayTracingBaseline.hlsl`

入口：

- `RayGenerationMain`
- `MissMain`
- `ClosestHitMain`

编译流：

```text
RayTracingBaseline.hlsl
    -> Slang
        -> DXIL，Shader Model 6.3
        -> SPIR-V 1.5
```

公共 `ShaderStage` 新增：

- RayGeneration
- AnyHit
- ClosestHit
- Miss
- Intersection
- Callable

Reflection 会把：

```text
RaytracingAccelerationStructure
    -> DescriptorType::AccelerationStructure

RWTexture2D
    -> DescriptorType::StorageTexture
```

这样 Pipeline Layout 不需要为 D3D12 和 Vulkan 维护两份人工绑定表。

## 7. 自动验证是怎样做的

### 7.1 纯逻辑测试

`PrismRhiTranslationTests` 验证：

- BLAS/TLAS 描述合法性；
- 错误 Triangle List 被拒绝；
- 相互冲突的 Build Flag 被拒绝；
- AS Buffer Usage；
- Vulkan RT Shader Stage；
- Vulkan AS Barrier 映射。

### 7.2 Shader 测试

`PrismShaderCompilerTests` 当前编译 38 个 DXIL/SPIR-V 程序，并验证 RayGen Reflection 的 TLAS 与 UAV 类型。

### 7.3 双 API 实机探针

PowerShell：

```powershell
$env:PRISM_RENDER_MAX_FRAMES = "1"
$env:PRISM_RENDER_RAY_TRACING_REPORT_PATH = "automation/reports/ray-tracing-d3d12.json"
.\build-windows-ci\Debug\PrismRender.exe --api=d3d12

$env:PRISM_RENDER_RAY_TRACING_REPORT_PATH = "automation/reports/ray-tracing-vulkan.json"
.\build-windows-ci\Debug\PrismRender.exe --api=vulkan
```

探针实际执行：

```text
创建三角形 Vertex/Index Buffer
    -> 查询 BLAS 大小
    -> 构建 BLAS
    -> 查询 TLAS 大小
    -> 构建 TLAS
    -> 验证两个 GPU 地址非零
    -> 输出 JSON
```

本机 NVIDIA GeForce RTX 5060 实测：

| 项目 | D3D12 | Vulkan |
|---|---:|---:|
| Tier | 1.1 | 1.1 |
| BLAS Result | 2944 B | 2944 B |
| BLAS Scratch | 1408 B | 1408 B |
| TLAS Result | 2304 B | 2304 B |
| TLAS Scratch | 1792 B | 1792 B |
| Scratch Alignment | 256 B | 128 B |
| BLAS/TLAS Built | true | true |

对齐不同是 API 规范允许的后端差异，不能强行写死为相同值。

## 8. 当前边界

本阶段完成的是“可运行的 AS 与 Shader 基线”，不是完整可见光追效果。

尚未完成：

- 公共 Ray Tracing Pipeline State；
- Hit Group 和 Shader Binding Table Builder；
- DescriptorSet 写入 TLAS；
- 公共 `DispatchRays`；
- RDG Ray Tracing Pass；
- 光追阴影、反射或 Path Tracing 最终画面；
- BLAS Compaction、Update/Refit 和动态实例增量更新。

正确的下一阶段顺序是：

```text
AccelerationStructure Descriptor 写入
    -> Ray Tracing Pipeline / Hit Group
    -> SBT Builder
    -> DispatchRays
    -> RDG RayTracedShadow 或 RayTracedReflection Pass
    -> 双 API Golden Image
```

## 9. 推荐阅读顺序

1. 先读 `RayTracing.h`，理解公共描述。
2. 再读 `RayTracing.cpp`，理解为什么要在驱动调用前校验。
3. 阅读 D3D12 `QueryAccelerationStructureBuildSizes()` 和 `CreateAccelerationStructure()`。
4. 阅读 Vulkan `CreateLogicalDevice()` 的 Feature 链。
5. 阅读 Vulkan `CreateAccelerationStructure()` 的 Device Address 和 Barrier。
6. 阅读 `RayTracingBaseline.hlsl` 与 `SlangShaderCompiler.cpp`。
7. 运行双 API 探针，对照 JSON 中的 Size、Alignment 和 Built 字段。

