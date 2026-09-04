# Physical Atmosphere Lab

该场景用于观察 PrismRender 的物理大气渲染，包括透射率 LUT、Sky-View LUT、
Rayleigh/Mie 散射、低角度太阳以及 HDR 天空合成。

## 启动

在项目根目录执行：

```powershell
.\tools\RunRendererLab.ps1 -Scene atmosphere -Api d3d12
```

验证 Vulkan 路径：

```powershell
.\tools\RunRendererLab.ps1 -Scene atmosphere -Api vulkan
```

也可以正常启动 PrismRender，然后在 `PrismRender Debug` 面板的
`Demo Scene` 下拉框中选择 `Physical Atmosphere Lab`。

## 观察内容

- 右侧低空太阳附近的 Mie 前向散射和暖色光晕。
- 从天顶到地平线逐渐增加的 Rayleigh 光程和颜色变化。
- 远景方尖碑、拱门和地平线剪影形成的逆光轮廓。
- 观测台与金属球上的低角度直射光和长阴影。
- `Environment` 面板中参数变化对天空 LUT 的实时影响。

建议先保持默认参数，然后分别调节：

- `Atmosphere Height (km)`
- `Mie Anisotropy`
- `Multiple Scattering`

关闭 `Physical Atmosphere` 可以与原有程序化天空直接进行 A/B 对比。
