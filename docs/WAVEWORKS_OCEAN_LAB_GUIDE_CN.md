# WaveWorks-like Ocean Lab

PrismRender 的 WaveWorks-like Ocean Lab 是项目内的 clean-room 实现：不链接
NVIDIA WaveWorks SDK、运行时或资产，只复现公开可观察的频谱海洋工作流。
旧的 FFT Ocean Lab 保留为对照场景；两条路径由 `OceanImplementation` 选择，
同一帧只允许一条路径提交模拟工作。

## 数据流

```text
Wind/Swell settings
        -> deterministic JONSWAP h0 (four band windows)
        -> absolute-time spectrum evolution
        -> horizontal/vertical array IFFT
        -> displacement + gradient + slope moments + foam/mips
        -> OceanSurface shader + optional local-wave composition
```

频谱坐标采用 XZ 水平、Y 向上。四个默认 patch 长度为 15.625、62.5、250、
1000 m；每级波长窗口由 patch、分辨率、稳定 Nyquist margin 和相邻级 texel
尺度计算，再以归一化对数 overlap 保证可解析波长不会漏级。Normal/High/Extreme
分别使用 128/256/512² 每级资源。所有演化从
绝对时间重放，初始高斯资源按随机种子和频谱设置缓存。泡沫使用 ping-pong
历史；历史纹理同时保存上一帧水平/垂直位移，并以相邻帧水平位移差估算表面
速度，在每个级联的周期域内执行有界半拉格朗日反向平流。新破碎源由折叠矩阵
最小主伸缩、局部波峰尖锐度和波峰上升速度共同门控，再叠加耗散与 falloff；
因此白沫会跟随波面运输，而不是固定在原纹理坐标或凭程序噪声生成。
水平位移按 PrismRender IFFT 的相位约定使用 `+i * k/|k| * h`；这样正的
Lateral Multiplier 会在正高度波峰处形成主方向压缩，而不是旧实现中把波峰拉宽、
把波谷压紧。折叠矩阵和白帽触发因此与可见尖峰处于同一相位。
风、Fetch、peaking 或幅度编辑使用 pending/recorded/active 三阶段版本；录制 H0
dispatch 不会立即发布，只有完整 H0→演化→IFFT→地图链到达帧提交边界后才提升
活动版本。较旧的录制版本不能清除拖动过程中产生的较新 dirty 版本。
Base/Swell Amplitude 按 WaveWorks 头文件定义为最终波幅倍率；频谱内部因此把该值
平方后写入能量，使 UI 从 1 调到 2 时 h0 与最终位移确实变为 2 倍，而不是旧实现
的 `sqrt(2)` 倍。

局部波是 128–2048 网格的阻尼高度/速度求解器，支持批量扰动、雨滴、航迹和
域边缘衰减。默认移动航迹复现参考示例的正弦/余弦船体路径：每帧在船体位置
提交 10x10 个正扰动样本；基础船体能量为 `0.13 * gridSpeed * dt`，单样本再乘采样密度归一化系数，避免 100 个重叠样本把能量放大为整片泡沫池。这些样本组成随速度方向旋转、首尾收窄的不可见船体轮廓；船体以 Wake Speed 指定的世界速度在参考相机前方沿椭圆轨迹从近处驶向远处再返回。船体本身不参与绘制，补偿波谷、V 形尾流与白沫由局部波方程及后续上升压缩波峰判定产生，而不是预先绘制泡沫轨迹。

可见泡沫不会把历史能量直接映射成纯白。当前材质将其分为三个连续阶段：正在上升和压缩的破碎波峰使用较亮、较粗糙的厚泡沫；稳定尾流使用偏蓝灰的多孔泡沫；能量衰减时逐渐过渡为透明度更低、接近水体散射色的湿润薄膜。三者共享世界空间气泡细节，因此浪起浪落时亮度和覆盖率连续变化，而不是整片同时出现或消失。
由高度方程传播自然形成，不预先绘制 V 形或圆形泡沫。只有传播后同时满足压缩、
正高度与上升速度的波峰才会生成泡沫历史。位移、梯度、坡度矩和
泡沫在 RenderGraph 中发布，并在 OceanSurface 中按域边缘 fade 合成。没有扰动
且历史衰减完成后，不再提交 local-wave dispatch。

局部波梯度是频谱梯度的增量，不能替换频谱法线；位移、梯度和泡沫统一从未位移
世界 XZ 计算 local UV，并共享 12% 吸收边界权重。细频段使用约 30 个 patch 周期
的连续距离衰减，最粗级联始终覆盖地平线；位移、法线、坡度矩、泡沫和级联调试图
使用同一权重。泡沫历史先经过能量门槛，再由米制 0.04/0.15/0.30 密度层与
亚米级、域扭曲的填充型气泡团簇侵蚀成连续湿膜、稀疏厚芯和微泡簇；不会从
噪声等值线提取 cellular wall，也不使用屏幕导数给泡沫增加白色描边。程序噪声本身
不能生成白沫。局部泡沫还会根据高度场速度梯度和移动扰动方向执行有界反向
平流。传播阶段只把上升中的正高度压缩波峰作为破碎候选，不使用会同时选中波谷
与回弹纹波的 `abs(Laplacian)`。最终材质将能量拆成带水色的薄膜、较白的厚核心和
衰减中的湿润间隙；三层都由模拟能量门控，平静水面严格保持无白沫。
局部位移图使用下一时间步的预测梯度，使水平位移、法线和新高度处于同一时刻；
水平位移按受限的两网格主波长尺度由坡度转换，不再使用会把船尾浪峰压圆的
`0.35 * cellSize` 弱位移近似。

## UI 与默认值

WaveWorks Lab 的 General、Wind Waves、Local Waves、Geometry、Statistics、
Debug 分组来自 `OceanSettings::WaveWorksReference()`。默认 Base Wind 使用
Beaufort（同时显示换算后的 m/s），关闭 Beaufort 后直接使用 m/s；fetch
单位 km，patch/domain/位移单位 m，方向输入为度并规范到 [0,360)。质量切换
会安全替换资源并清空不兼容的泡沫历史；sun/shading 只更新常量，不重建频谱。
Statistics 显示后端、分辨率、级联发布版本、内存、dispatch、阶段计时、
几何节点、tessellation 能力、查询 pending/completed/expired 和读回延迟。

当前对照截图预设的 Base Wind 为：方向 31.9 度、9.9 Beaufort（26.04 m/s）、
fetch 0.001 km、dependency 1、spectrum peaking 13.664、cutoff length 5.10 m、
cutoff power 0.248、amplitude 2.058。Swell 为：方向 90 度、速度 29.62 m/s、
fetch 9.502 km、dependency 1、spectrum peaking 12.275、cutoff length 7.23 m、
cutoff power 0.007、amplitude 0.477。Spectral Foam 为 0.500/0.370/0.120/
0.600/0.985（whitecaps threshold、generation threshold、generation amount、
dissipation speed、falloff speed）。Load WaveWorks Reference 会恢复这些值。

颜色预设为深水 `RGB(0,51,102)`、散射 `RGB(128,160,180)`、泡沫
`RGB(184,199,194)`、水下泡沫 `RGB(153,153,153)`；water/scattering intensity
保持 `(0.040, 0.040, 0.025, 0.380)`。

参考场景的主 `DirectionalLight` 强度为 4；新建方向光组件的引擎默认值和编辑器
Create 菜单也统一为 4。参考水体的深水强度为 `(0.04, 0.04, 0.025)`、散射强度
为 `0.38`，场景曝光为 `1.16`；这些默认值提高暗部可读性，但不改变太阳高光的
物理门控。UV warping 保持示例的 amplitude 0.03、frequency 2，
其中 frequency 的单位是每级联 UV 的弧度而不是 cycles：`U += A*cos(V*F)`、
`V += A*sin(U*F)`。位移、梯度、坡度矩和泡沫共享该坐标。旧实现额外乘以
`2*pi` 并把偏移归一化，低视角会出现密集的指纹状/旋涡状机械纹路。

## RHI 与后端差异

- D3D12 使用 DXIL、HS/DS 字节码、patch-list topology 和根签名；GPU 时间戳
  来自 graphics/compute timestamp query。
- Vulkan 使用 SPIR-V tessellation-control/evaluation stage、patch topology 和
  `VkPipelineTessellationStateCreateInfo`；descriptor stage mask 同时覆盖
  vertex/hull/domain/pixel。
- 两端都通过 Slang 反射生成同一 canonical binding：CBV 0–15、SRV 16–31、
  UAV 32–47、sampler 48–63。没有 compute queue 或 tessellation capability 时，
  自动回退到 graphics queue/clipmap，模拟和 shading 不会被关闭。

## 性能与验证

RenderGraph 将初始谱、演化、水平/垂直 FFT、输出 map、foam、mip、local-wave
和可选 Ocean.Query 分成独立 pass。`asyncComputeEnabled` 只有在设备报告
compute queue 时才启用，队列成本模型记录 overlap 与 synchronization cost。
默认查询不分配资源；调用 `SceneRenderer::QueueOceanDisplacementQuery` 后才
创建有界批次缓冲并在下一帧交付 tagged 结果。

编辑器的 Game View 和 Scene View 共用同一个 `SpectralOceanSimulation` 与
`LocalWaveGpuResources`。Game View 是唯一 producer，负责频谱更新、FFT、泡沫
历史交换和局部波推进；Scene View 只导入已经发布的纹理用于采样。因此打开两个
Viewport 不会再重复分配四级联资源，也不会在同一帧提交两套模拟 dispatch。

支持 tessellation 的设备使用相机/视锥驱动的自适应四叉树：CPU 按最小 patch、
屏幕空间边长、最大 LOD 和保守位移范围选叶节点，做相邻 LOD 平衡并上传实例数据；
Slang vertex/hull/domain shader 完成 edge folding、geomorph 和曲面细分。不支持
tessellation 或关闭 `Prefer Tessellation` 时保留三环 camera-following clipmap，
频谱模拟、泡沫和专用水面 shading 不会被关闭。选择器有 4096 节点硬预算。

生产 IFFT 使用每个 workgroup 完整处理一行或一列的共享内存 radix-2 内核，
把 Extreme 512 的 18 次全纹理 butterfly dispatch 缩减为水平、垂直各一次。
旧 stage-by-stage 内核仍由数值 fixture 覆盖；该优化不降低质量档，也不改变公开图。

RTX 5060、1280x800、RelWithDebInfo、D3D12、单 Game 输出自动采样如下：

| Quality | Resolution | IFFT GPU | Renderer GPU |
|---|---:|---:|---:|
| Normal | 128² x 4 | 0.02 ms | 2.68 ms |
| High | 256² x 4 | 0.06 ms | 2.61 ms |
| Extreme | 512² x 4 | 0.35 ms | 4.57 ms |

2026-08-26 最终默认尾流样本在 1280x800 为 5.63 ms Renderer GPU，其中
ForwardGeometry 2.77 ms、水平/垂直 IFFT 合计 0.68 ms、LocalWave 0.010 ms；
2560x1417 为 8.38 ms，其中 ForwardGeometry 5.07 ms。Vulkan Extreme 在
1280x800 为 4.82 ms。优化前同一
1280x800 D3D12 样本为 12.47 ms，其中 IFFT 7.65 ms。分辨率、活动 View、质量档
和是否包含编辑器 UI 必须同时报告，不能把 2560 编辑器 FPS 与 WaveWorks 的
1280x800 独立窗口直接比较。

WaveWorks Lab 的编辑器 Scene View 使用场景级 on-demand 更新策略：进入场景、
窗口缩放、World 编辑、Scene 相机/Gizmo/地形交互或显式 Scene capture 时刷新一次；
空闲时保留最后一张 Scene 快照，只让 Game View 连续渲染海面。该策略不会改变
四级联频谱和局部波模拟的单一共享生产者，也不会应用到其他 Demo；其他场景仍按
原有 Continuous 策略连续渲染独立 Scene/Game 视口。该 Lab 不包含可编辑地形，
因此同时关闭默认 RenderSettings 中无消费者的 InteractiveTerrain 计算 pass。

局部波生产路径只在 CPU 生成确定性的雨滴/船体扰动批次，512² 高度、速度和泡沫
网格由 GPU RenderGraph pass 唯一推进。原先生产路径还会同步执行同尺寸的 CPU
oracle 网格，随后丢弃其输出，单帧约消耗 53 ms；该 oracle 现在只保留给单元测试
和显式 CPU 验证。1280x800、D3D12、RelWithDebInfo 的 90 帧隐藏采样中，Frame
中位数由约 56.6 ms 降为 5.0 ms，SceneRenderer 中位数为 0.84 ms，Renderer GPU
保持约 4.6 ms。90 个 Game 帧只发生 91 次 SceneRenderer 调用（90 次 Game 加首次
Scene 快照），且没有 Terrain GPU pass。窗口模式仍使用同步 Present，最终显示 FPS
会受显示器刷新率限制。

频谱白沫以最小主伸长量表示的水平折叠为权威破碎条件，并由离散高度曲率和逐帧
上升速度调节生成强度；后两项不作硬剔除，以免不同级联分辨率的数值尺度误删真实
破碎波峰。只有中、大尺度两个频带可以生成可见白沫，历史随后按水平位移速度反向
追踪、双线性平流并指数耗散。泡沫与 folding 的 mip 均以覆盖率平均为主，只弱保留
峰值，避免把稀疏波峰递归扩散成远景白膜。

水面阶段不再用最粗 1000 m 周期在 shader 内反推固定数组。CPU 每帧从当前频谱版本
发布四级联 patch 长度、上下波长界、UV 比例/偏移、纹理层顺序和距离淡化起止值；
顶点位移、法线、坡度矩、泡沫和级联调试视图统一读取这份尾随 FrameConstants
元数据。普通 Mesh shader 不声明这些尾随字段，因此原有材质资源布局保持不变。

OceanSurface 使用独立反射布局和独立 descriptor set。它只保留帧/对象常量、阴影、
环境与大气、四级联位移/梯度/坡度矩/泡沫、局部波、patch 实例和采样器；普通材质
纹理、材质常量、聚簇点光、普通实例数据与地形资源不会进入海洋布局。DXIL 与
SPIR-V 测试逐 binding 比较类型和数组长度，普通 Mesh pipeline 继续使用原布局。

交互运行应使用 `RelWithDebInfo`，避免 Debug 的 `/Od`、运行时检查和 D3D12
验证层开销：

```powershell
.\build-windows-ci\RelWithDebInfo\PrismRender.exe `
    --api=d3d12 `
    --scene=waveworks-ocean
```

自动验收可在启动前设置：

```powershell
$env:PRISM_RENDER_OCEAN_QUALITY='extreme' # normal/high/extreme
$env:PRISM_RENDER_OCEAN_PRESET='wake'     # calm/no-swell/spectral-only/local-only/
                                          # strong-wind/rough-water/whitecap/
                                          # whitecap-energy/
                                          # low-sun/wake/rain/cascade/foam/
                                          # normal/moments/geometry-lod/wireframe
```

验证入口：

- `RhiTypeTranslation`：RHI stage、descriptor merge、patch pipeline validation。
- `ShaderCompiler`：DXIL/SPIR-V 共用 shader matrix，另验证 SPIR-V hull/domain。
- `RenderGraph`：四级联资源依赖、版本发布、历史/队列 barrier。
- `OceanSettings`：参数规范化、查询 FIFO、patch topology、frustum/LOD。
- `OceanFftGpuD3D12` / `OceanFftGpuVulkan`：128/256/512 FFT、四级联、地图、
  foam、质量切换和数值容差。

限制：GPU fence/timeline 的公开 RHI 接口尚未覆盖 Ocean.Query 的完全异步读回，
目前用 renderer frame boundary 完成 handoff。fallback 三环几何使用 4:1 共点
边界并按物理 cell size 过滤顶点位移，但低视角仍可能出现少量三角反射折线；
完全消除需要让自适应四叉树成为所有后端的唯一绘制路径或提高曲面细分密度。
Vulkan 的 `PRISM_RENDER_CAPTURE_VIEW=game` 当前仍可能抓到完整编辑器合成而不是
独立 Game 纹理，数值、计时与 GPU fixture 已通过，但该 capture 路径需单独修复。
本文所有 WaveWorks 比较均为 clean-room 定性参考，不宣称像素一致。
