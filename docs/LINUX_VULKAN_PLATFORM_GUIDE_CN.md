# PrismRender Linux Vulkan 平台化学习与实施指南

本文记录 Stage 23 的实际实现。目标不是在 Windows 工程外再复制一份
Linux 工程，而是让同一个 Renderer、同一个 Vulkan RHI、同一套 Slang Shader
和同一套 RDG 前端在 Windows 与 Linux 上构建。

## 1. 为什么先做平台解耦

Stage 22 已实现跨 API GPU Frustum Culling 和 Indirect Draw，但此前工程仍有
四类 Windows 隐式依赖：

1. CMake 无条件查找 Windows SDK、DXIL DLL、D3D12 和 Win32 ImGui。
2. `Window.cpp` 无条件包含 `glfw3native.h` 并取得 `HWND`。
3. 公共 Asset 类型在头文件中暴露 D3D12 Descriptor 和 Resource。
4. CPU Trace、性能身份和进程诊断直接调用 Win32 API。

如果不先处理这些依赖，后续 Bindless GPU Scene、资产流送和 CI 会继续把
Windows 假设写入公共模块。

## 2. 目标划分

CMake 新增三个开关：

```cmake
PRISM_RENDER_ENABLE_D3D12
PRISM_RENDER_BUILD_EDITOR
PRISM_RENDER_BUILD_HARNESS
```

Windows 默认全部启用。Linux 默认只构建：

```text
PrismEngine
PrismRenderer common + Vulkan
PrismRender Vulkan application
跨平台测试和 Golden Image 工具
```

D3D12 RHI、D3D11 转换器、Win32 Editor、Minidump/PDB 工具不会进入 Linux
编译图。

## 3. 文件变化

新增文件：

```text
src/Core/Environment.h
src/Core/Environment.cpp
src/Core/ProcessDiagnosticsPosix.cpp
CMakePresets.json
.github/workflows/linux-vulkan.yml
docs/LINUX_VULKAN_PLATFORM_GUIDE_CN.md
```

主要修改文件：

```text
CMakeLists.txt
src/Platform/Window.cpp
src/Core/Assert.h
src/Core/CpuTrace.cpp
src/Core/ApplicationLauncher.cpp
src/Core/VulkanApplication.cpp
src/Renderer/PerformanceIdentity.cpp
src/Renderer/GpuProfiler.h/.cpp
src/Asset/Mesh.h/.cpp
src/Asset/Texture.h/.cpp
src/Asset/Material.h/.cpp
src/Asset/AssetRuntimeLoader.h/.cpp
src/Asset/EnvironmentMapLoader.h/.cpp
src/Asset/IblEnvironmentBuilder.h/.cpp
src/Scene/DefaultSceneFactory.h/.cpp
src/Scene/SceneLoader.h/.cpp
tests/RhiTypeTranslationTests.cpp
tests/ShaderCompilerTests.cpp
```

## 4. 构建数据流

```text
CMake host platform
  |
  +-- Windows + D3D12 enabled
  |     +-- common RHI/RDG/Asset
  |     +-- D3D12 backend
  |     +-- Vulkan backend
  |     +-- Editor/Harness/Minidump
  |
  +-- Linux
        +-- common RHI/RDG/Asset
        +-- Vulkan backend
        +-- POSIX diagnostics
        +-- PrismRender defaults to Vulkan
```

`PRISM_RENDER_HAS_D3D12` 是后端存在性宏，不能用 `_WIN32` 替代。原因是
Windows 也必须能够构建 Vulkan-only 配置，这可以检测公共对象文件是否仍
暗中引用 D3D12 符号。

## 5. 公共 Asset 如何脱离 D3D12

`Mesh`、`Texture` 和 `Material` 保留旧 D3D12 入口以兼容现有 Renderer，
但这些入口只在 `PRISM_RENDER_HAS_D3D12` 下声明和编译。

跨平台路径只依赖：

```text
IGraphicsDevice::CreateBuffer/CreateTexture
ICommandContext::BindVertexBuffer/BindIndexBuffer
ICommandContext::DrawIndexed/DrawIndexedIndirect
IDescriptorSet
```

因此 Vulkan Asset 上传不需要包含 `d3d12.h`，Linux 链接器也不会看到
`D3D12Context` 的未解析符号。

## 6. 平台服务

### 6.1 环境变量

`Core/Environment` 统一封装：

```cpp
ReadEnvironmentVariableValue(name);
IsEnvironmentVariableEnabled(name);
```

Windows 使用 `_dupenv_s`，POSIX 使用 `std::getenv`。Window、CPU Trace、
Render Capture 和 Vulkan Application 不再各自复制平台代码。

### 6.2 Window

GLFW Window 本身跨平台。只有 D3D12 Swapchain 需要 `HWND`，所以
`GetNativeHandle()` 在 Windows 返回 Win32 Handle，在 Linux 返回空；
Vulkan 始终通过 `glfwCreateWindowSurface` 建立 Surface。

### 6.3 CPU Trace 与性能身份

CPU Trace 使用 `std::thread::id` 的稳定进程内 Hash 作为 Thread ID：

```text
Windows process id -> GetCurrentProcessId
Linux process id   -> getpid
```

性能身份在 Linux 使用 `/proc/self/exe`、`/proc/cpuinfo`、GCC/Clang 宏和
Linux 架构宏，报告格式继续保持 `PrismRuntimePerformanceIdentity`。

### 6.4 POSIX 进程诊断

`ProcessDiagnosticsPosix.cpp` 保持与 Windows 相同的 JSONL Log 和 Crash
Report Schema。Linux 不声称生成 Windows Minidump，而是记录
`backtrace()` 地址与符号文本。后续稳定性阶段可继续接入 Core Dump 和
`llvm-symbolizer`。

## 7. Slang 和 DirectXMath

仓库中的 Slang SDK 是 Windows 二进制，不能复制到 Linux 使用。Linux 配置
必须让 `PRISM_SLANG_ROOT` 指向同版本 Linux SDK：

```bash
export PRISM_SLANG_ROOT=/opt/slang-2026.8.1
```

CMake 在 Windows 查找 `.lib + .dll`，在 Linux 查找 `libslang.so`。
Shader 仍由相同的 `SlangShaderCompiler` 输出 SPIR-V。

DirectXMath 是跨平台 Header Library。Linux 优先查找系统 Header，缺失时
CMake FetchContent 固定到 `may2026` 标签。这里继续保留 DirectXMath，
避免为了平台化重写全部矩阵和场景数学。

## 8. Linux 构建与运行

安装编译和 Vulkan 运行依赖，并准备 Slang SDK 后：

```bash
export PRISM_SLANG_ROOT=/opt/slang-2026.8.1
cmake --preset linux-vulkan-debug
cmake --build --preset linux-vulkan-debug
ctest --preset linux-vulkan-debug
./build-linux-vulkan/PrismRender --api=vulkan
```

无窗口 CI 使用 Xvfb 和 Mesa Vulkan：

```bash
LIBGL_ALWAYS_SOFTWARE=1 \
  xvfb-run -a ctest --test-dir build-linux-vulkan --output-on-failure
```

## 9. 自动验证

`.github/workflows/linux-vulkan.yml` 执行：

1. 安装 GCC、CMake、Ninja、GLFW、Mesa Vulkan 和 Xvfb。
2. 下载固定版本 Slang `2026.8.1` Linux x86_64 SDK。
3. 配置 D3D12/Editor/Harness 全关闭的 Vulkan-only Build。
4. 编译 `PrismRender` 与跨平台测试。
5. 在 Mesa Software Vulkan 上执行 CTest。
6. 失败时上传 CMake 和 CTest Log。

本机 2026-07-19 验证：

```text
Windows D3D12 + Vulkan Debug Build: PASS
Windows full CTest:                8/8 PASS
Windows Vulkan-only Build:        PASS
Windows Vulkan-only CTest:        7/7 PASS
Harness D3D12/Vulkan regression:  3/3 PASS
```

当前机器没有 WSL 或 Docker，因此 Linux 原生结果由新增 CI 负责，不能把
Windows Vulkan-only 测试误写成 Linux 实机测试。

## 10. 设计取舍

- 没有重写数学库：DirectXMath 本身支持 Linux，重写只会扩大风险。
- 没有在 Linux 构建 Editor：当前 Editor 使用 ImGui DX12/Win32 Backend，
  应在产品化阶段接入 ImGui Vulkan Backend 后再开放。
- 没有在 Linux 生成 Minidump：Minidump/PDB 是 Windows 诊断格式，Linux
  应使用 Core Dump 和 ELF/DWARF 工具链。
- 没有复制 Vulkan Renderer：平台差异停留在 CMake 与 Core/Platform，
  Vulkan RHI、RDG 和高级渲染前端保持一套。

## 11. 下一阶段

Stage 24 进入资产流送，重点是：

1. Asset Residency 状态和稳定 Handle。
2. 后台 IO、Cooked Chunk 与优先级请求。
3. RHI Async Upload Queue 与 Fence 完成回调。
4. 有预算的 GPU 内存 Residency 和 LRU Eviction。
5. Placeholder 资源与 RenderScene 无阻塞切换。
6. Harness 结构化查询、压力测试和确定性验证。

平台化完成后，这些能力才能同时服务 Windows D3D12、Windows Vulkan 与
Linux Vulkan，而不需要三套资产加载器。
