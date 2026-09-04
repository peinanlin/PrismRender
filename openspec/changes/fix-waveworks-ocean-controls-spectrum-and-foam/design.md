## Context

The current spectral implementation owns four 512/256/128 texture-array slices with patch lengths 15.625, 62.5, 250, and 1000 meters. However, each slice is filtered to a band centered on its patch length. At Extreme quality the smallest slice can resolve wavelengths near 0.061 meters, but its current lower band edge is 6.25 meters. With the reference 4.7 Beaufort base wind and 0.1 km fetch, the JONSWAP peak is approximately 0.54 meters and is therefore discarded. The approximately 48 meter swell peak remains, producing the coordinated appearance.

The surface applies UV warp to the common 1000 meter UV before multiplying it for smaller cascades, all cascade distance weights return one, and cascade metadata is hard-coded in multiple shaders. Foam consequently receives little folding energy. The local CPU emitter builds wake disturbances after copying the list intended for GPU upload, so the rendered local-wave textures receive no demo wake. The reference sample enables its boat by default and disables rain.

Finally, `OceanPSMain` calls the generic mesh `PSMain` and then performs ocean moment/foam work. This prevents the ocean path from owning a clear optical and performance contract.

## Goals / Non-Goals

**Goals:**

- Make reference wind, swell, wake, and foam parameters visibly and continuously controllable.
- Recover multi-scale short wind-wave structure without losing deterministic absolute-time evolution.
- Produce reference-like localized moving-wake foam and physically gated wind whitecaps.
- Reduce avoidable ocean pixel cost and publish comparable timing at both reference and editor resolutions.
- Preserve backend-neutral Slang sources and current RHI/RenderGraph ownership.

**Non-Goals:**

- Copying NVIDIA's closed FFT/local-wave algorithms or proprietary shader/media source.
- Pixel-identical WaveWorks output or an undocumented recreation of its internal Beaufort lookup tables.
- Shoreline breaking, spray particles, volumetric underwater rendering, or topology-changing waves.
- Changing the legacy FFT Ocean Lab defaults or renderer path.

## Decisions

### 1. Partition wavelengths by resolvable sampling range

For a patch length `L` and FFT resolution `N`, the shortest stable gravity wavelength begins near `2L/N`; the longest non-duplicated wavelength is bounded by `L`. The four cascades SHALL use normalized logarithmic windows derived from these limits and adjacent-cascade texel scales rather than from `L / 2.5` and `L * 2.5`.

The default bands will continuously cover the resolvable range of the 15.625 through 1000 meter patches. Adjacent bands overlap across a small logarithmic transition whose normalized weights sum to one. Tests explicitly verify that the approximately 0.54 meter base-wind peak and 48 meter swell peak receive non-zero ownership at every quality.

Alternative: raise base-wind amplitude until its long-wave tail becomes visible. Rejected because it hides the missing spectrum and produces incorrect energy.

### 2. Publish one cascade metadata contract

CPU code will generate patch length, UV scale, UV offset, minimum/maximum wavelength, and distance-fade data in a stable cascade order. Vertex/domain and pixel stages consume that same constant payload. The shader will compute each cascade UV from world XZ, then apply sinusoidal warp in that cascade's own UV space. Fine cascades fade according to projected footprint/distance; the coarsest displacement remains available at the horizon.

Hard-coded patch arrays in `Mesh.hlsl` and `OceanSurface.hlsl` will be removed from the spectral path. Debug visualizations consume the same weights as rendering.

### 3. Treat UI settings as staged renderer state

When the spectral scene is active, every ocean widget binds the nested `OceanSettings` object. Flat compatibility fields remain only for the legacy scene and are not displayed as editable spectral controls.

Changes are classified as:

- shading/evolution constants: apply on the next frame;
- spectrum parameters: rebuild the inactive initial spectrum, then publish coherently;
- foam parameters: change subsequent history evolution without reallocating FFT resources;
- quality/local-grid changes: allocate replacement resources and retire old resources safely;
- geometry changes: rebuild selection/instance data only.

The panel displays the effective Beaufort conversion and pending/active settings version. Spectrum updates preserve the last coherent visible result and must not expose partially rebuilt slices.

### 4. Make the reference wake a real GPU producer

The reference preset enables demo emitters with rain disabled and moving wake enabled. Demo disturbances are generated before the upload snapshot is finalized. CPU validation and GPU execution consume the same final per-frame disturbance batch.

The GPU local solver uses cell-size-aware spatial derivatives, a stable bounded step, an absorbing edge region, and derived lateral displacement. Local foam uses folding/velocity candidates plus all configured threshold, amount, dissipation, and falloff controls. Reset clears every ping-pong field and published map.

The demonstration wake follows a deterministic bounded path inside the local domain. The reference camera/preset keeps the wake observable without making camera tracking a simulation requirement.

### 5. Compose whitecaps, persistent energy, and detail separately

Spectral and local map generation publish folding candidates. History stores persistent turbulent energy. Surface shading combines weighted cascade energy, current wave-hat folding, and local energy. Project-owned foam-density and bubble detail is sampled at multiple world-space frequencies only after physical energy or a current breaking candidate exists; it never creates foam on calm water.

The implementation may use deterministic procedural detail initially, but its spatial frequencies must be expressed directly in meters and tested for near-field variation. NVIDIA foam DDS assets are reference-only and are not copied.

### 6. Own an independent ocean pixel path

The ocean pixel entry will no longer call the generic mesh `PSMain`. It may reuse small pure helper functions and shared frame/light constant declarations, but it owns water material evaluation, atmosphere/environment lookup, moment-filtered sun lighting, auxiliary fills, foam, and output encoding. Ordinary mesh reflection/layout remains unchanged.

This reduces redundant material texture samples and avoids evaluating generic direct lighting that the ocean later replaces. D3D12 and Vulkan reflection tests verify identical logical bindings.

### 7. Validate performance at equal resolutions

WaveWorks' approximately 5 ms reference frame is captured at 1280x800 while the reported PrismRender frame is 2560x1417, approximately 3.54 times as many pixels. Reports SHALL contain resolution, quality, active views, simulation stages, surface GPU time, total GPU time, and CPU time.

The first optimization pass removes duplicate pixel work, inactive local work, and unnecessary high-frequency sampling at distance. More invasive FFT replacement is performed only if stage timing still exceeds the selected budget after correctness fixes.

## Files to Add

- Focused regression tests and deterministic capture metadata under `tests/` and the existing test-artifact layout.
- Project-owned foam detail asset metadata only if an authored texture replaces deterministic shader detail.

## Files to Modify

- `src/Renderer/Features/Ocean/OceanSettings.*`
- `src/Renderer/Features/Ocean/OceanSpectrumGenerator.*`
- `src/Renderer/Features/Ocean/SpectralOceanSimulation.*`
- `src/Renderer/Features/Ocean/LocalWaveSimulation.*`
- `src/Renderer/Features/Ocean/LocalWaveGpuResources.*`
- `src/Renderer/SceneRenderer.cpp`
- `src/UI/OceanLabPanel.cpp`
- `src/UI/EditorLayer.cpp`
- `assets/shaders/Ocean/OceanInitialSpectrum.slang`
- `assets/shaders/Ocean/OceanFoam.slang`
- `assets/shaders/Ocean/LocalWave.slang`
- `assets/shaders/OceanSurface.hlsl` and shared binding declarations as needed
- CMake/test registration files for new regression coverage

## Data Flow

```text
WaveWorks Lab UI
  -> validated OceanSettings + settings version/dirty scope
  -> spectrum metadata/H0 rebuild or immediate constants
  -> evolution -> IFFT -> displacement/gradient/moments
  -> spectral foam history

Reference wake emitter
  -> final per-frame disturbance batch
  -> GPU local height/velocity integration
  -> local displacement/gradient/foam history

Cascade metadata + coherent spectral/local maps
  -> independent ocean vertex/domain/pixel pipeline
  -> atmosphere/sun/foam output
  -> frame tonemapping and bloom
```

## Risks / Mitigations

- **Spectrum correction changes displacement scale:** establish CPU/GPU sampled-spectrum fixtures and finite conservative bounds before visual tuning.
- **Live wind edits can reset visible features repeatedly:** keep the previous coherent spectrum visible and commit at a frame boundary; report pending state.
- **Wake simulation can become unstable:** use cell-size-aware CFL limits, bounded substeps, absorbing boundaries, and finite-output tests.
- **Independent ocean PS can diverge from frame lighting conventions:** reuse authoritative frame/atmosphere inputs and test sun-direction response and output encoding.
- **Visual improvements can regress performance:** capture stage and surface timings before and after each group at equal resolution and quality.

