# P0 Ocean Tessellation 严格校验修复记录

初次诊断未修改 tessellation fixture、其 shader、RHI 或海洋算法。用户随后明确批准最小修复范围，P0 task 1.20 已完成；下方原失败结果仍保留，不作为修复后结果。当前计划进度 16/118，完整 P0 尚未通过。

## 复现与影响范围

完整 Windows 构建显式 `-L gpu -j1` 并传入 `PRISM_RENDER_GPU_VALIDATION=1`：18 项中 16 项通过，`OceanTessellationGpuD3D12` 与 `OceanTessellationGpuVulkan` 失败。无 Editor Vulkan 同条件 9 项中 8 项通过，失败仍为 `OceanTessellationGpuVulkan`。

证据相对 `artifacts/architecture-refactor/20260828-hpwater-complete/`：

- `P0/d3d12/approved-final-gpu-01/ctest.xml`、`ctest.stdout.log`、`index.json`。
- `P0/vulkan/approved-final-no-editor-gpu-01/ctest.xml`、`ctest.stdout.log`、`index.json`。

两个批次均标记 failed，不通过删测试或关闭 validation 放行。完整版通过项中 15 项确认输出校验层已启用；另一个 `VulkanRuntime` 通过但没有该启用标记，不将其冒充 strict 渲染 fixture。无 Editor 对应为 7 项启用校验的通过项加 1 项 Runtime 通过。

当前 `PrismRender.exe` 的 20 Demo × 双 API 固定帧 capture/strict 和 V2 图像对照均已通过，只有三项使用用户批准的新参考。该结果不消除本 fixture 阻碍，也不意味着 HPWater 的所有质量/时域矩阵通过。

## 已定位的代码对应关系

1. D3D12 报五条 validation 527，barrier 的 before=COMMON 与实际 UNORDERED_ACCESS 不符。`tests/OceanTessellationGpuTests.cpp` 的 refraction frame recording 将 Storage 输入/输出缓冲 before 声明为 Undefined；`D3D12Resources.cpp::GetBufferReadyState` 对 Storage 返回 UAV，初始上传也退到此状态。日志没有资源名，未声称已逐原生句柄映射五条诊断；声明与真实初态的冲突已明确。
2. Vulkan `vkCmdBeginRendering` 报 backbuffer 实际 PRESENT_SRC_KHR、请求 COLOR_ATTACHMENT_OPTIMAL。fixture 把 `color.stateBefore` 固定为 RenderTarget，没有表达 Vulkan 呈现后状态。原已修复的 FFT fixture 在相同边界按后端明确 Present/RenderTarget，不能改 RHI 全局规则迁就错误测试声明。
3. Vulkan draw 报 Hull 的 SpacingFractionalOdd 与 Domain 的 SpacingEqual 冲突。独立 `OceanHull.slang` 声明 `fractional_odd`，独立 `OceanDomain.slang` 只有 domain，没有 matching partitioning。生产 `assets/shaders/OceanSurface.hlsl` 的 Hull/Domain 两端已显式匹配；独立 shader 的调用者检索仅发现 `ShaderCompilerTests` 与本 GPU fixture，不是默认 Demo 的生产曲面路径。

以上 fixture、两个独立 shader 与生产 OceanSurface 均逐文件对原始 `20260828-hpwater-complete/snapshot/` 核对，哈希不变。本轮没有重建或重跑原始 fixture 二进制，因此不把源码相同说成已经完成原始二进制 strict 复现。

## 已批准的最小修复

- 修改 `tests/OceanTessellationGpuTests.cpp`，校正缓冲初态和 backbuffer 呈现/绘制边界，保留所有光学、折射、焦散和体积数值断言及容差。
- 修改仅供上述测试使用的 `assets/shaders/Ocean/OceanDomain.slang`，补齐与 Hull 一致的 spacing 声明，不改生产 OceanSurface 或细分算法。
- 必要时修改 CMake 测试属性，固定本 fixture 的严格校验请求，防止仅因默认 validation 关闭而“通过”。

验收：两种 Windows 构建、CPU/ShaderCompiler、三个 tessellation GPU 配置与完整 GPU 套件严格校验，保留数值断言；再次核对生产 Demo/HPWater 图像、生产 shader 哈希和输出隔离。任何超出上述范围的新问题应独立报告，不扩展成海洋算法重写。

上述诊断时 1.6 和 P0 整体尚未完成，P1 架构拆分及后续线程阶段不得越过该门禁；修复后的当前状态见下文。

## 1.20 实施：仅修测试契约

修改文件：

- `tests/OceanTessellationGpuTests.cpp`：五组输入在 D3D12 从 UAV、Vulkan 从上传的 CopyDestination 转为 ShaderResource；五组输出在 D3D12 已是 UAV（同态 UAV barrier）、Vulkan 无前序访问（Undefined）后进入 UAV。backbuffer 按 BeginFrame 的真实边界选择 RenderTarget/Present，EndFrame 继续负责最终呈现。
- `assets/shaders/Ocean/OceanDomain.slang`：补一行 `[partitioning("fractional_odd")]`，与测试 Hull 一致；三角形重心插值、细分因子及生产曲面 shader 不变。
- `CMakeLists.txt`：两个 tessellation CTest 固定 `PRISM_RENDER_GPU_VALIDATION=1`；Vulkan validation 诊断使测试失败。请求开关不等于环境实际可用，验收另核对 layer enabled 日志。

数据流仍是：固定解析输入 → 五个原 compute fixture → UAV/CopySource → 原 readback 数值断言，再加独立三角形 draw。初始化/质量/history/退役检查和整个数值 readback 尾段与上一恢复点逐字一致；没有改变任何断言或容差。后端差异只在测试入口表达，不为迁就 fixture 改 RHI 状态契约。

对 `20260828-p0-approved-visual-baselines/snapshot/` 核对：370/370 个 `src/` 文件不变；60 个 assets 文件中只有独立 Domain shader 变化。生产 `OceanSurface.hlsl` SHA-256 为 `C5B9746DF3632B81E35D045EE083A6E6377CF6F7DFF77964538FE8C488AB11A8`，包装 `Ocean/OceanSurface.slang` 为 `1A171B9334FB0A2A09CA309B80B1FCD1F1B249948350B0DFAF8C70B64008B86B`，均与原记录一致。

已完成的定向验证：`P0/gpu/tessellation-fix-strict-01/` 的 full-d3d12、full-vulkan、no-editor-vulkan 三项均退出 0、实际层启用、无 validation 诊断，所有原数值断言通过。两种 Windows 构建通过，见 `P0/builds/tessellation-fix-01/`；完整构建发现缓存 CMake 指向 VS 版本后，显式用固定 4.1.2 重新 configure/build，最终 `SceneRenderer.cpp.obj` 的 87 个头依赖 VALID。没有清理工作区或覆盖旧二进制快照。

## 完整测试结果

| 检查 | 结果 | 证据（上述 P0 下） |
| --- | --- | --- |
| 完整 Windows CPU | 11/11，含 ShaderCompiler；40.76 s | `d3d12/tessellation-fix-cpu-01/` |
| 无 Editor CPU | 10/10，含 ShaderCompiler；22.48 s | `vulkan/tessellation-fix-no-editor-cpu-01/` |
| 完整 GPU suite | 18/18；136.48 s | `d3d12/tessellation-fix-gpu-01/` |
| 无 Editor GPU suite | 9/9；65.81 s | `vulkan/tessellation-fix-no-editor-gpu-01/` |

GPU suites 显式传入 validation、串行执行并逐项检查 JUnit：完整构建 17 项图形测试、无 Editor 8 项图形测试确认 layer enabled 且无新增诊断；各自余下的 `VulkanRuntime` 为单独运行时检查，不冒称 strict 图形测试。原 `approved-final-gpu-01` 的 16/18 和 `approved-final-no-editor-gpu-01` 的 8/9 仍为失败批次。

1.6 的当前可用平台构建/测试及环境登记完成。Linux 本机注册 WSL 发行版为 0，没有可用执行环境，仍是未验证；本次没有安装环境，也没有将 Windows 结果替代 Linux 的后续验收。工具自测 12/12（`tool-tests/bae2981e545f425a9775cd1928592d0b/`）和基线工具 10/10（`tool-tests/baselines-d1c576b6f2c74b1181f5a1e56b9dbe22/`）通过，路径均相对 `artifacts/architecture-refactor/`。

构建依赖补查：首次完整 fixture 对象由于原缓存的本地化 include 前缀不匹配，只记录了 0 个头依赖。固定 CMake 后增加一行说明输入/输出 initialData 的注释并定向重编（无逻辑变化），两种 fixture 对象现在都记录 36 个头依赖且 VALID。该重编没有改动生产 `PrismRender.exe`。最终通过 CTest 的 tessellation 标签再次运行三配置，不从外层环境提供 validation 开关，验证新 CTest 属性实际启用校验；`P0/gpu/tessellation-fix-final-ctest-01/{full,no-editor}/` 的 2+1 项全部通过，逐项检查 layer enabled 与无诊断。

最终 fixture 二进制 SHA-256：完整 `FD1EBEF902168B0763E39174B7679FD992A15D96B95B90019128BA61A9E50815`；无 Editor `0BEF18774F6E5CD8C0B52A0E4418AF6108663E9E5D3E92E54F032C93FE0AC455`。完整 GPU 18+9 套件对应此前同逻辑版本；最终定向 CTest 对应仅补注释、依赖记录恢复后的这两个二进制，不混淆运行身份。

## Demo / HPWater 图像保护

- `P0/{d3d12,vulkan}/tessellation-fix-all-demos-native-01/`：各 20/20 capture/strict，输入仍为默认设置、单 Game view、native、1280×800、确定性 f30。两 API 的生产源码和 Demo 设置未修改。
- `P0/comparisons/tessellation-fix-all-demos-{d3d12,vulkan}-01/`：分别对上一轮 `approved-final-all-demos-native-01` 同配置截图，均 20/20 满足原 V2。D3D12 19 张、Vulkan 18 张逐像素一致；没有替换旧 golden 或启用新批准范围。
- HPWater 另对最初完成版直接比较：`P0/comparisons/tessellation-fix-hpwater-{d3d12,vulkan}-original-01/`。D3D12 MAE=.000003、RMSE=.000325、changed=.000020、SSIM=.999998；Vulkan 逐像素一致。只代表默认视角固定帧，不代替完整质量/尺寸/连续帧矩阵。
- 本轮生产程序 SHA-256：完整 `A78B1666CBCAEC3A505EAD500516DF1F6506EDFD766DCF6239450A23F2FC8528`（CMake 重配置后重新链接，非生产源改动）；无 Editor 保持 `52C011849A34A803B6F8EF677D50EB9FD0650D659A87FCEEFF64B55D8514B7E4`。各捕获索引保留实际二进制身份，不声称完整程序字节未变。

## 额外的 P0 生命周期复核

`P0/{d3d12,vulkan}/tessellation-fix-hpwater-lifecycle-01/` 复用已有 330 帧水验证序列，两 API 均成功。逐份 stderr 确认实际校验层启用、无新增错误；stdout 含 frame 320 的覆盖率恢复断言完成。报告 `completedFrames=330`、`validationSequence=true`、最终尺寸 1280×800、`waterCoverage.available=true`，覆盖率两 API 均为 0.7886064648628235。

原序列覆盖 local 开关、history/local/full reset、水线相机、三质量、两次 resize、WaveWorks/Ocean/HPWater 切换及水体关闭/开启。这里只确认序列运行、现有断言和最终截图/报告；并未增加每个控制点的连续图像捕获、共享模拟计数或历史身份诊断，不能据此勾选整个 1.7/1.9。V3 性能矩阵也尚未执行。

最终两种完整 build 再次执行均 `ninja: no work to do`，见 `P0/builds/tessellation-fix-01/{windows-ci,no-editor}-final.log`。工作区相对上一 833 文件恢复点只有上述三个代码/构建文件与五个本 change 的计划/验收文件变化；生产 C++、其他资产、用户布局和其他文档未改。原始恢复点 809 文件再次验证通过。

恢复点 `20260828-p0-tessellation-fixture` 已创建并重复验证 833 文件；manifest SHA-256：`FD893CD10EB5D4A488EAE85540DE333D89840FA3C9144F3A62557E6E4CD6B7CA`。封存后仅追加完成状态/恢复点说明，没有修改已验证代码。1.6、1.20 完成，原始失败日志保留；1.5、1.7–1.9 与 P1–P8 不由本次结果代替。
