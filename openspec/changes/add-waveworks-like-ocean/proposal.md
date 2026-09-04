## Why

PrismRender's Ocean Lab currently demonstrates a single 128x128 Phillips-spectrum FFT surface, but it lacks the multi-scale simulation, persistent foam, local interaction, adaptive geometry, and ocean-specific shading required to approach the WaveWorks sample visually and architecturally. Expanding it as a native PrismRender feature preserves Slang/RHI/RenderGraph ownership, avoids a proprietary NVIDIA runtime dependency, and creates a stronger Direct3D 12 and cross-backend renderer showcase.

## What Changes

- Add a separate WaveWorks Lab backed by a modular, native WaveWorks-like spectral ocean while preserving the existing FFT Ocean Lab, its defaults, and its `FftOcean` rendering path as a permanent comparison scene.
- Add deterministic base-wind and swell JONSWAP spectra distributed across four independently simulated world-space cascades.
- Cache initial spectra, evolve them over absolute simulation time, run portable GPU inverse FFT passes, and produce displacement, gradient, slope-moment, persistent-foam, and mip-chain outputs.
- Add a bounded local-wave simulation that accepts disturbances and blends its displacement, gradients, and foam with the spectral ocean.
- Add an ocean-specific surface renderer with cascade blending, UV warping, slope-statistics microfacet lighting, atmospheric reflection/scattering, detailed foam, and cascade/wireframe debug views.
- Add camera-dependent ocean geometry with crack-free LOD and geomorphing; extend the Slang/RHI graphics pipeline for hull/domain stages where supported and retain a clipmap fallback otherwise.
- Add optional asynchronous-compute scheduling, GPU timing, resource/memory statistics, and non-blocking displacement query/readback facilities.
- Add a dedicated WaveWorks Lab UI modeled on the WaveWorks grouping and defaults while retaining the legacy FFT Ocean Lab controls required to reproduce the original scene.
- Keep the implementation independent of NVIDIA WaveWorks binaries and do not copy proprietary sample shader code verbatim.

## Capabilities

### New Capabilities

- `renderer/spectral-ocean-simulation`: Four-cascade wind-and-swell JONSWAP simulation, portable inverse FFT, derived maps, persistent foam, quality presets, and deterministic time evolution.
- `renderer/ocean-surface-rendering`: Multi-cascade displacement and shading, ocean-specific microfacet optics, atmospheric integration, adaptive geometry selection, fallback geometry, and visual debug modes.
- `renderer/local-wave-simulation`: Bounded interactive wave simulation with disturbances, local gradients and foam, reset controls, and composition with the spectral ocean.
- `renderer/ocean-query-and-profiling`: Non-blocking displacement queries, conservative displacement bounds, simulation latency reporting, GPU timing, and memory/dispatch statistics.
- `ui/ocean-lab-controls`: Dedicated WaveWorks-inspired controls, defaults, dirty-state handling, presets, statistics, and debug visualization controls.
- `rhi/tessellation-pipeline`: Slang hull/domain compilation and portable tessellation pipeline support for capable D3D12 and Vulkan devices, with explicit capability reporting.

### Modified Capabilities

None. The repository does not currently contain main OpenSpec capability specifications; all behavior introduced by this change is specified as new capability deltas.

## Impact

- Renderer: `FftOcean`, scene feature registration, RenderGraph pass construction, queue scheduling, frame resources, profiling, and the forward ocean surface path.
- RHI: shader-stage enums and reflection, graphics pipeline descriptions, D3D12/Vulkan pipeline creation, capability reporting, and validation for optional tessellation stages.
- Shaders: spectral initialization/evolution, inverse FFT, derived maps, foam history, local waves, ocean surface stages, and debug visualization, all compiled through Slang to DXIL or SPIR-V.
- Scene/UI: a new WaveWorks Lab scene with independent defaults and a dedicated ImGui panel, while the existing FFT Ocean Lab remains available as an unchanged comparison baseline.
- Tests: deterministic spectrum tests, FFT correctness tests, resource-state and RenderGraph tests, D3D12/Vulkan shader and pipeline tests, image regression captures, and performance budgets.
- Dependencies: no NVIDIA WaveWorks runtime or new third-party ocean library; NVIDIA's sample is used only as behavioral and visual reference under its applicable license.
