## Why

The completed WaveWorks-like ocean migration has a visible and functional acceptance gap. The current WaveWorks Lab is dominated by coordinated swell, omits the short wind-wave peak, rarely generates spectral foam, does not deliver demo wake disturbances to the GPU, and exposes an obsolete wind-speed field that does not control the active spectral simulation. The dedicated ocean pixel entry also executes the generic mesh pixel shader before its ocean work, contributing unnecessary cost and an appearance that differs substantially from the reference sample.

The gaps are caused by concrete implementation defects rather than missing high-level architecture. They should be fixed in a focused follow-up while preserving the legacy FFT Ocean Lab and PrismRender-owned Slang/RHI implementation.

## What Changes

- Replace patch-centered spectral windows with resolution-aware wavelength bands that cover the FFT-resolvable range and retain the WaveWorks reference base-wind and swell peaks.
- Make cascade ordering, UV scale, offset, warping, and distance fade explicit runtime data shared by displacement, gradient, moment, and foam sampling.
- Provide one authoritative WaveWorks Lab settings path so every visible wind control updates `OceanSettings`; remove or disable spectral-scene legacy controls that currently have no effect.
- Repair local-wave demo disturbance publication, adopt reference-compatible `rain off / moving wake on` defaults, and make the GPU local solver honor physical spacing, absorbing boundaries, lateral displacement, foam thresholds, generation, dissipation, and falloff.
- Rework spectral/local foam composition and project-owned multi-scale foam detail so wind whitecaps and moving wakes are visible only where simulated folding or persistent energy exists.
- Turn the ocean pixel entry into a genuinely independent shading path instead of calling the generic mesh pixel shader, while keeping shared frame lighting data and Slang DXIL/SPIR-V parity.
- Add visual, control-response, D3D12/Vulkan, and resolution-normalized performance acceptance gates.

## Capabilities

### Modified Capabilities

- `renderer/spectral-ocean-simulation`: Correct resolution-aware wavelength partitioning and live spectrum updates.
- `renderer/ocean-surface-rendering`: Correct per-cascade mapping/fades, detailed foam composition, and an independent ocean pixel path.
- `renderer/local-wave-simulation`: Visible reference wake defaults, correct GPU disturbance delivery, bounded physical integration, and local foam.
- `renderer/ocean-query-and-profiling`: Comparable-resolution performance reporting and ocean-stage budgets.
- `ui/ocean-lab-controls`: One authoritative settings source and complete WaveWorks-style grouped controls.

## Impact

- Renderer: ocean settings lifecycle, spectrum generator, local-wave emitter publication, GPU local resources, object/ocean constants, and ocean surface descriptors.
- Shaders: initial spectrum, spectral foam, local waves, displacement/domain sampling, and ocean pixel shading compiled through Slang for D3D12 and Vulkan.
- UI: WaveWorks Lab panel discoverability, legacy-control gating, live apply status, reference defaults, and diagnostics.
- Tests: wavelength coverage, dirty scopes, emitter delivery, foam evolution, shader reflection, deterministic captures, and performance reports.
- Compatibility: the legacy FFT Ocean Lab remains unchanged; no NVIDIA library, shader, or media asset is linked or redistributed.

