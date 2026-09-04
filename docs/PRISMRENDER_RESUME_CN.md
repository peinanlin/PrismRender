# PrismRender 简历项目描述

整理依据：2026-08-28 工作区源码及现有验证报告。本次只整理文字，未重新构建或运行测试。项目时间、仓库地址和个人职责范围请按实际经历填写。

## 可直接使用的版本

### PrismRender｜基于 D3D12 / Vulkan 的实时渲染器

**项目类型：** 个人图形渲染项目  
**技术栈：** C++20、Direct3D 12、Vulkan、HLSL / Slang、CMake、GLFW、ImGui

**项目简介：** 面向现代图形 API 与渲染架构开发实时渲染器，统一跨后端渲染流程，集成 PBR、RenderGraph、GPU 可见性处理及海洋/流体专项演示，配套参数编辑、性能分析和自动化图像回归工具。

- **跨 API 架构：** 设计公共 RHI，统一资源、命令、描述符、Pipeline 与帧生命周期，将 D3D12 / Vulkan 差异封装于后端；通过 Slang 将共享 Shader 编译为 DXIL / SPIR-V，并利用反射生成资源绑定布局，复用同一高层渲染流程。
- **RenderGraph 与同步：** 基于 Pass 资源读写构建依赖图，实现无效 Pass 裁剪、子资源状态转换和瞬态资源生命周期/别名复用；支持 Graphics / Compute 队列批次、跨队列同步与并行命令录制，并通过能力查询保留回退路径。
- **渲染管线：** 实现 Forward / Deferred PBR、IBL、级联阴影、GTAO、SSR、TAA 与 HDR / Bloom / Tonemapping；接入 GPU 视锥/Hi-Z 剔除及间接绘制，将可见性结果通过 RenderGraph 屏障衔接至图形绘制。
- **海洋与水体光学：** 实现四级联双 JONSWAP 频谱与 GPU IFFT，支持 128² / 256² / 512² 质量档、自适应海面几何、局部尾流和持久泡沫；在独立 HPWater Lab 中复用模拟结果，实现屏幕空间折射、RGB 吸收、焦散近似与低分辨率水下体积光，配合时域重投影和双边重建。
- **工程验证：** 搭建 ImGui 调试面板、分 Pass CPU / GPU 计时及确定性截图回归；覆盖参数切换、Resize、跨队列资源复用和双后端一致性验证。现有 HPWater 七组固定视角对照均通过门限，图像 SSIM 不低于 0.9998。

## 可按岗位替换的条目

不建议把所有条目同时放入简历；保留最能讲清设计、取舍与验证的 4–5 条。

### 偏图形算法：流体模拟

- 实现默认 32,768 粒子的 GPU PBF 流体演示，通过均匀网格邻域查询和迭代约束投影更新粒子，结合涡量约束与 XSPH 速度平滑；使用粒子深度/厚度重建、双边滤波和法线恢复完成屏幕空间流体渲染，并接入吸收、折射、反射和焦散近似。

### 偏引擎基础设施：资产流送

- 建立 CPU 资产、Cooked 数据与 GPU Runtime 资源分层，实现后台资产读取、GPU 上传和驻留发布；使用 Upload Ticket、持久上传环与延迟回收管理异步资源生命周期，结合驻留预算和 LRU 策略控制资源占用。

### 偏水体渲染：将海洋条目拆为两条

- 基于双 JONSWAP 谱实现四级联 GPU FFT 海洋，将频谱演化、IFFT、位移/梯度、泡沫与 Mip 生成拆分为 RenderGraph Pass；以折叠信号驱动泡沫生成，结合历史平流和耗散保持连续性，支持实时风速、风向、Fetch 与涌浪控制。
- 在共享海面模拟之上实现独立水光学子图，分离不透明场景深度与水面深度，避免水体自折射；实现屏幕空间折射回退、Beer–Lambert 吸收、均匀介质单次散射，以及带运动/深度拒绝的体积光历史和双边重建。

## 数字与表述边界

- **四级联、128² / 256² / 512²、32,768 粒子**是实现规模/默认配置，不是帧率保证。
- **SSIM ≥ 0.9998**来自现有 HPWater 七组固定视角 D3D12 / Vulkan 图像对照，仅代表这些测试输入下的一致性，不代表与 NVIDIA WaveWorks 的相似度或任意场景的一致性。
- 现有性能报告为 RTX 5060、RelWithDebInfo、固定视角和预热后的单个已完成帧快照，不宜直接写成平均 FPS、优化提升比例或整帧 GPU 时长。
- 不写“完全复现 WaveWorks”“达到 WaveWorks 200 FPS”“生产级完整游戏引擎”“完整 Bindless GPU Driven”“物理准确多次散射/光子追踪焦散”。
- 项目有可运行代码和验证记录，不自动等于个人熟练掌握全部模块；投递前应确保能解释所写条目的关键代码、失败案例与技术取舍。

## 核验入口

| 内容 | 代码或已有报告 |
| --- | --- |
| 公共 RHI 与后端 | `src/RHI/IGraphicsDevice.h`、`src/RHI/ICommandContext.h`、`src/RHI/IFrameContext.h`、`src/RHI/D3D12/`、`src/RHI/Vulkan/` |
| Shader 编译与绑定 | `src/Asset/SlangShaderCompiler.cpp`、`src/RHI/ShaderLayoutBuilder.cpp` |
| RenderGraph | `src/Renderer/RenderGraph.cpp`、`src/Renderer/RenderGraphCompiler.cpp`、`tests/RenderGraphTests.cpp` |
| 海洋模拟 | `src/Renderer/Features/Ocean/SpectralOceanSimulation.cpp`、`src/Renderer/Features/Ocean/OceanSpectrumMath.cpp`、`assets/shaders/Ocean/` |
| 水光学 | `src/Renderer/Features/Ocean/WaterOpticsFeature.cpp`、`assets/shaders/Ocean/WaterOptics.slang`、`assets/shaders/Ocean/WaterVolumetrics.slang` |
| GPU PBF | `src/Renderer/Features/Fluid/PbfFluidSimulation.cpp`、`src/Renderer/Features/Fluid/ScreenSpaceFluidRenderer.cpp` |
| 资产流送 | `src/Asset/AssetStreamingManager.cpp`、`docs/ASSET_STREAMING_GUIDE_CN.md` |
| 图像一致性与性能条件 | `artifacts/hpwater-validation/acceptance-summary.json`、`docs/HPWATER_OCEAN_LAB_CN.md` |
| 已有 CPU / GPU 测试记录 | `artifacts/hpwater-validation/cpu-tests.log`、`artifacts/hpwater-validation/gpu-tests.log` |
