# PrismRender Slang 与跨 API Shader 实现指南

本文档记录 Stage 8 的实际实现。目标不是只说明 Slang 的概念，而是让后续学习者能够沿着代码还原：Shader 如何读取、编译、反射、缓存，最后进入 D3D12 PSO；同一份源码又如何生成 Vulkan 所需的 SPIR-V。

## 1. 当前完成状态

已经完成：

- 固定使用 Slang `2026.8.1`
- 用 Slang 替换 `D3DCompileFromFile`/FXC 运行时编译
- API 无关的 `ShaderStage`、`ShaderBinaryFormat` 和 `ShaderBinary`
- `IShaderCompiler` 编译器边界
- `SlangShaderCompiler` 的 DXIL、SPIR-V、DXBC 和 Metal Source 目标映射
- Slang Reflection 到 Prism `ShaderReflection`
- D3D12 PSO 使用 Slang 生成的 Shader Model 6 DXIL
- Vulkan Triangle、纹理 Forward 与 Sky Pipeline 使用 Slang 生成的 SPIR-V
- 29 个入口的 DXIL/SPIR-V 双目标自动测试，包含 Shadow、GBuffer、Deferred 和 PostProcess
- Vulkan Descriptor Binding 唯一性约定
- Vulkan 场景 DescriptorSetLayout 与实际 DescriptorSet 绑定

还没有完成：

- 两后端统一 RenderScene 后的完整高级材质数据路径；Vulkan Deferred/PostProcess Pipeline 已在 Stage 13 完成基础版
- 根据 Reflection 自动生成 D3D12 Root Signature
- 根据 Reflection 自动生成 Vulkan DescriptorSetLayout
- Shader 热重载、磁盘缓存和 PSO 失效
- 原生 Metal Backend

所以当前状态是“D3D12 完整高级场景使用 Slang DXIL，Vulkan Deferred + PostProcess 场景使用 Slang SPIR-V”。Vulkan Shadow、GBuffer、Deferred、HDR、Bloom 和 Tonemap 已运行，但两后端仍需统一 RenderScene 与完整材质参数。

## 2. 为什么分成 Shader 层和 RHI 层

两层解决的问题不同：

```text
Shader 层：HLSL/Slang -> DXIL/SPIR-V/MSL + Reflection
RHI 层：   Texture/Buffer/State/Command -> D3D12/Vulkan/Metal 原生对象
```

Slang 不创建 D3D12 Resource，也不提交 Vulkan CommandBuffer。RHI 不应该解析 Shader 语言。二者在 Pipeline 创建阶段汇合：Backend 同时需要中立 Pipeline 描述、目标 Shader 字节码和资源绑定布局。

## 3. 文件职责

新增文件：

- `src/RHI/ShaderTypes.h/.cpp`：API 无关 Shader 数据
- `src/Asset/IShaderCompiler.h`：编译器接口和输入请求
- `src/Asset/SlangShaderCompiler.h/.cpp`：Slang Compilation API 适配器
- `assets/shaders/ShaderBindings.hlsli`：跨 D3D/Vulkan Binding 规则
- `tests/ShaderCompilerTests.cpp`：DXIL/SPIR-V/Reflection/Cache 测试

修改文件：

- `src/Asset/ShaderLoader.*`：只负责读取 Shader 源码文本
- `src/Asset/ShaderManager.*`：拥有编译器并缓存 `ShaderBinary`
- `src/RHI/D3D12/D3D12TypeConversions.*`：把 `ShaderBinary` 转为 `D3D12_SHADER_BYTECODE`
- `src/Renderer/TriangleRenderer.cpp`：改用通用 ShaderBinary
- `src/Renderer/D3D12SceneRenderer.cpp`：改用通用 ShaderBinary
- `src/Renderer/D3D12SceneRenderer.cpp`：所有实际 PSO 改用 Slang DXIL
- `CMakeLists.txt`：Slang SDK、运行时 DLL、DXC DLL 和测试目标

## 4. 核心数据结构

`ShaderCompileRequest` 表达编译意图：

```cpp
struct ShaderCompileRequest
{
    std::filesystem::path filePath;
    std::string entryPoint;
    RHI::ShaderStage stage;
    RHI::ShaderBinaryFormat format;
    bool debug;
};
```

`ShaderBinary` 保存与具体 COM/Vulkan 对象无关的结果：

```cpp
struct ShaderBinary
{
    ShaderStage stage;
    ShaderBinaryFormat format;
    std::string entryPoint;
    std::vector<std::uint8_t> bytecode;
    ShaderReflection reflection;
    std::string diagnostics;
};
```

旧实现返回 `ComPtr<ID3DBlob>`，这会让 Asset 层绑定 Direct3D。现在 Blob 被复制到普通字节数组，D3D12 和未来 Vulkan 都可以消费。

## 5. Slang 编译过程

`SlangShaderCompiler::Compile` 按以下顺序工作：

1. `ShaderLoader` 读取源码
2. 根据目标选择 `SLANG_DXIL`、`SLANG_SPIRV`、`SLANG_DXBC` 或 `SLANG_METAL`
3. 创建 `slang::ISession`
4. 设置 Profile：DXIL 使用 `sm_6_0`，SPIR-V 使用 `spirv_1_5`
5. 设置矩阵为 Column Major，保持原项目 HLSL/CPU 转置约定
6. 用 `loadModuleFromSourceString` 加载源码
7. 用 `findAndCheckEntryPoint` 显式校验入口名称和阶段
8. 合成 Module 与 EntryPoint
9. Link Program
10. 用 `getEntryPointCode` 生成目标代码
11. 用 `getLayout` 提取 Reflection
12. 把结果复制到 `ShaderBinary`

编译器内部由互斥量保护。Slang Session 不是默认可重入对象，不能在多个线程中无保护地共享编译状态。

## 6. 矩阵布局为什么是 Column Major

旧 FXC 路径采用 HLSL 默认 Column Major。CPU 侧在上传前使用 `XMMatrixTranspose`。如果切换 Slang 时改成 Row Major，Shader 可以编译成功，但场景中的相机、阴影和物体变换会全部错误。

迁移原则是先保持原语义：

```text
DirectXMath Row-Major 数据
        -> CPU XMMatrixTranspose
        -> Shader Column-Major 常量
```

未来可以统一改为 Row Major，但必须同时修改 C++ 常量上传和所有 Shader，不能只改编译器参数。

## 7. D3D 与 Vulkan Binding 约定

D3D 的 `b0`、`t0`、`s0` 属于不同寄存器类别，可以同时存在。Vulkan 的同一 Descriptor Set 中只有一个 Binding 数字空间，因此直接转换会重叠。

当前 Set 0 约定：

| 资源 | D3D 寄存器 | Vulkan Binding |
| --- | --- | --- |
| ConstantBuffer | `b0...` | `0...` |
| Texture/SRV | `t0...` | `16...` |
| UAV | `u0...` | `32...`，预留 |
| Sampler | `s0...` | `48...` |

Shader 使用：

```hlsl
PRISM_VK_BINDING(0)  cbuffer FrameConstants : register(b0) { /* ... */ };
PRISM_VK_BINDING(16) Texture2D albedoTexture : register(t0);
PRISM_VK_BINDING(48) SamplerState linearSampler : register(s0);
```

这些 `vk::binding` 属性影响 SPIR-V，D3D12 仍然使用原有 `register`。测试会检查 Slang 诊断中不存在 `explicit binding overlap`。

## 8. Reflection 当前做了什么

Reflection 会记录：

- 参数名称
- ConstantBuffer、ShaderResource、UAV、Sampler 等类别
- Binding Index
- Binding Space/Descriptor Set
- 可获得时的常量字节大小

当前 Root Signature 仍由 D3D12 Renderer 手工创建，Reflection 用于测试和下一阶段输入。下一阶段应增加 `PipelineLayoutDescription`，由 Reflection 生成中立布局，再分别转换为：

```text
D3D12 -> Root Parameter + Descriptor Table + Static Sampler
Vulkan -> DescriptorSetLayout + PipelineLayout
Metal -> Argument Buffer / Resource Index
```

## 9. D3D12 如何消费 DXIL

Renderer 不直接读取 Slang Blob。调用链是：

```text
SceneRenderer
  -> ShaderManager::LoadShader(..., ShaderStage::Pixel)
  -> SlangShaderCompiler::Compile(..., ShaderBinaryFormat::Dxil)
  -> ShaderBinary
  -> D3D12::ToNativeShaderBytecode
  -> D3D12_GRAPHICS_PIPELINE_STATE_DESC
  -> CreateGraphicsPipelineState
```

`ToNativeShaderBytecode` 会拒绝空字节码和 SPIR-V，防止把错误目标提交给 D3D12。

Vulkan 使用 `ShaderBinary::emittedEntryPoint` 创建 Pipeline。Slang 的逻辑入口可以是 `VSMain`、`PSMain`，当前单入口 SPIR-V 模块实际导出名是 `main`。`entryPoint` 用于缓存和诊断，`emittedEntryPoint` 才是提交给原生 Pipeline 的名字。

## 10. ShaderManager 缓存

缓存键包含：

```text
文件路径 | 入口名称 | Shader Stage | Binary Format
```

因此同一个 `Mesh.hlsl` 可以同时缓存 VS/PS，也可以同时缓存 DXIL/SPIR-V。当前缓存只在进程内有效；后续热重载需要额外记录文件时间、Include 依赖和 PSO 反向依赖。

## 11. CMake 与运行时依赖

固定 SDK 位于：

```text
third_party/slang
```

运行时需要复制到可执行文件目录：

- `slang.dll`
- `slang-compiler.dll`
- `slang-glslang.dll`
- `dxcompiler.dll`
- `dxil.dll`

Slang 负责前端和 SPIR-V，DXIL 目标会调用 Windows SDK 的 DXC。CMake 会查找 Windows SDK `Redist/D3D/x64`，缺失时在配置阶段直接失败，而不是等到程序启动后秒退。

## 12. 自动验证

执行：

```powershell
cmake --build build-windows-ci --config Debug
ctest --test-dir build-windows-ci -C Debug --output-on-failure
```

`ShaderCompiler` 测试会：

1. 遍历当前 29 个 Vertex/Pixel 入口
2. 每个入口分别生成 DXIL 和 SPIR-V，共 58 个程序
3. 检查 DXIL Container Magic
4. 检查 SPIR-V Magic `0x07230203`
5. 检查 Vulkan Binding 不重叠
6. 检查 Reflection 能找到 `FrameConstants`、`albedoTexture` 和 `linearWrapSampler`
7. 检查 `ShaderManager` 缓存复用

编译测试通过后仍要实际运行 D3D12 场景，因为矩阵、颜色空间和资源绑定错误可能只在 GPU 执行时出现。

## 13. 推荐学习顺序

1. 阅读 `RHI/ShaderTypes.h`，理解为什么不能返回 `ID3DBlob`
2. 阅读 `Asset/IShaderCompiler.h`，理解接口边界
3. 阅读 `SlangShaderCompiler::Compile`，按 12 个步骤跟踪一次编译
4. 阅读 `ShaderManager`，理解缓存键
5. 阅读 `ShaderBindings.hlsli`，理解 D3D/Vulkan Binding 差异
6. 阅读 `D3D12::ToNativeShaderBytecode`
7. 阅读 `SceneRendererStage4.cpp` 中的 PSO 创建
8. 阅读 `ShaderCompilerTests.cpp`，对照每个断言
9. 手动破坏一个 Binding，观察 SPIR-V 测试失败
10. 手动改错一个入口阶段，观察 `findAndCheckEntryPoint` 诊断

## 14. 下一阶段实施顺序

1. 定义中立 `PipelineLayoutDescription`：手工 DescriptorSetLayout 基础版已进入公共 Pipeline
2. 把 Reflection 合并规则从测试代码提升到运行时代码
3. 校验 Reflection 与手写 D3D12 Root Signature 一致
4. 建立 Vulkan `Format`、State、Stage、Access、ImageLayout 转换：已完成
5. 实现 Vulkan Device/Queue/CommandPool/SwapChain：已完成
6. 用 SPIR-V 创建 Vulkan 公共 Pipeline：Triangle、纹理 Forward 与 Sky 已完成
7. 迁移 Descriptor 与 Forward：已完成手工布局基础版
8. 迁移 Shadow：Stage 12 已完成；Deferred 和 PostProcess：Stage 13 已完成 Vulkan 基础版
9. 加入 D3D12/Vulkan 完整场景截图回归

Vulkan 基础后端的详细实现见 `docs/VULKAN_RHI_GUIDE_CN.md`，公共资源与 DescriptorSet 实现见 `docs/RHI_RESOURCE_GUIDE_CN.md`，公共 Pipeline 与共享 Pass 见 `docs/RHI_PIPELINE_PASS_GUIDE_CN.md`，公共 Rendering Scope、D3D12 资源后端和共享 Shadow 见 `docs/RHI_RENDERING_SHADOW_GUIDE_CN.md`。在第 9 步完成之前，不应删除 D3D12 手工 Root Signature，也不应宣称完整 Vulkan 高级场景后端已经完成。
