## Context

See `proposal.md` for motivation. The current Ocean Lab owns one fixed 128x128 Phillips spectrum, recomputes its deterministic initial spectrum every frame, performs fourteen butterfly passes, and builds one displacement plus one normal/foam mip chain. Three camera-following mesh rings sample that single 320 m periodic result. Simulation dispatches are hidden inside one graphics-queue RenderGraph pass, and ocean shading is embedded in the general mesh shader.

The WaveWorks sample exposes its rendering, UI, quadtree integration, and D3D11/D3D12/Vulkan entry points, but the wind-wave FFT, map update, foam, and local-wave implementations are closed binary code. Its public rendering contract exposes four-slice RGBA16F displacement, gradient, and moment arrays. PrismRender must therefore reproduce the behavior from documented concepts rather than integrate or copy the proprietary implementation.

PrismRender already has Slang DXIL/SPIR-V compilation, texture arrays and subresource views, persistent RenderGraph resources, native graphics/compute queues, atmosphere data, ImGui, and backend profiling. Its RHI does not currently expose hull/domain stages or patch pipelines.

## Goals / Non-Goals

**Goals:**

- Preserve PrismRender ownership of algorithms, GPU resources, scheduling, and shaders.
- Reach WaveWorks-like visual structure through four spectral scales, swell, persistent foam, slope moments, local interaction, specialized shading, and adaptive geometry.
- Keep D3D12 and Vulkan behavior aligned through Slang and RHI abstractions.
- Make every major simulation and rendering stage independently observable, testable, and profileable.
- Develop the new WaveWorks Lab incrementally while keeping the existing FFT Ocean Lab as a stable, directly selectable visual and performance baseline.

**Non-Goals:**

- Pixel-identical reproduction of WaveWorks or undocumented replication of its internal spectrum-band allocation.
- Linking NVIDIA WaveWorks libraries or redistributing NVIDIA binaries.
- Copying WaveWorks sample shader source verbatim.
- A general-purpose fluid engine, shallow-water shoreline solver, breaking waves with topology changes, spray particles, or full volumetric underwater rendering in this change.
- Requiring tessellation for correctness; unsupported adapters retain a clipmap geometry path.
- Guaranteeing bit-identical floating-point output across D3D12 and Vulkan. Cross-backend validation uses numerical and image tolerances.

## Decisions

### 1. Evolve `FftOcean` into a modular native feature

Create an `Ocean` feature family rather than growing `FftOcean` and `Mesh.hlsl` further:

- `OceanSettings` and `OceanStatistics` own stable configuration and observable state.
- `SpectralOceanSimulation` owns four cascade resources and simulation passes.
- `OceanSpectrumGenerator` owns JONSWAP construction, directional spreading, band partitioning, deterministic random values, and cached initial spectra.
- `OceanFft` owns portable inverse FFT setup and passes.
- `OceanMapBuilder` owns displacement/gradient/moment output and mip generation.
- `OceanFoamSimulation` owns spectral foam history.
- `LocalWaveSimulation` owns the bounded time-domain solver and local foam.
- `OceanSurfaceRenderer` owns geometry, descriptors, and the dedicated forward surface pipeline.
- `OceanQueryService` owns optional sample batches, readback FIFO, and bounds.
- `OceanLabPanel` owns ocean-specific ImGui controls.

Alternative: keep all behavior in `FftOcean` and the general mesh path. Rejected because resource rebuilds, history, query lifetime, local integration, and backend capability branching would produce unclear ownership and make isolated validation difficult.

### 2. Use a project-defined four-cascade JONSWAP model

The combined base-wind and swell spectra use JONSWAP energy with directional spreading. The default largest simulation period is 1000 m. Four cascade patch lengths are derived by a constant 4:1 ratio: 15.625 m, 62.5 m, 250 m, and 1000 m. Smooth logarithmic band windows partition energy so adjacent cascades overlap only through a normalized transition region; summing their weights preserves the intended total spectrum.

The default project quality mapping is Normal=128, High=256, and Extreme=512 texels per cascade. These are PrismRender definitions because WaveWorks does not publish its corresponding resolutions. All four slices use one resolution within a quality preset so they can share texture arrays and dispatch dimensions.

Base-wind and swell random amplitudes are generated from a fixed seed and cached as initial spectra. Spectrum-affecting changes rebuild the cache; absolute time only runs phase evolution.

Alternative: retain Phillips and add four arbitrary frequency filters. Useful as a migration checkpoint but rejected as the final model because it lacks fetch and spectrum peaking controls and cannot represent the requested wind/swell behavior.

### 3. Prefer portable FFT correctness before subgroup optimization

The first implementation generalizes the existing butterfly FFT for runtime power-of-two resolutions and four array slices. Twiddle and index data are precomputed or generated once per quality configuration. RenderGraph exposes separate evolution, horizontal FFT, vertical FFT, map, foam, and mip stages even when an implementation batches multiple stages internally.

Spectrum working textures initially use 32-bit float channels where accumulation requires them; published displacement, gradient, moment, and foam arrays use RGBA16F with complete mip chains. After correctness and parity tests exist, an FP16 working-path optimization may be enabled per quality/device if it remains within tolerance.

Alternative: start with wave/subgroup intrinsics and shared-memory fused kernels. Deferred because subgroup width and compiler behavior increase early D3D12/Vulkan parity risk. The ownership boundary permits later replacement without changing consumers.

### 4. Publish three principal spectral arrays plus explicit foam history

The surface contract follows the useful public WaveWorks shape without depending on its implementation:

- Displacement array: horizontal and vertical displacement, plus an auxiliary folding value.
- Gradient array: reconstructed normal/slope data and current foam energy or folding metadata.
- Moment array: first- and second-order slope moments used by filtered microfacet shading.
- Foam-history ping-pong arrays: previous and next persistent foam energy, kept separate when necessary to make temporal dependencies explicit.

All arrays contain four slices and full mips. Frame-coherent output is promoted only after every required producer pass completes. Settings that change dimensions allocate a replacement resource set, initialize it, swap it at a frame boundary, and retire the old set through the existing retirement mechanism.

Alternative: expose separate textures per cascade. Rejected as the default because arrays simplify descriptors and surface sampling, although a temporary separate-texture implementation is acceptable during migration if an RHI view limitation is discovered.

### 5. Implement foam as temporal simulation data

Map generation computes surface derivatives, Jacobian/folding, and whitecap candidates. Foam update reads previous history, adds thresholded generation, applies time-scaled falloff and dissipation, and clamps finite energy. Spectral foam is periodic per cascade; local foam follows local-domain boundary behavior. Surface shading adds foam/bubble detail textures but does not use those textures as simulation history.

Alternative: retain the current single-frame Jacobian mask. Rejected because temporal continuity is a primary visual gap and the existing result visibly flickers or disappears immediately after a crest unfolds.

### 6. Use a bounded height/velocity local-wave solver

Local waves use ping-pong height and velocity fields with a damped two-dimensional wave equation, fixed maximum substep, central spatial differences, and an absorbing/faded boundary. Derived displacement, gradients, and foam are built after integration. Disturbance batches are uploaded once and applied in a compute pass; rain and moving-wake demos are producers of the same disturbance record format.

The domain defaults to 200 m and 512x512. The surface maps world XZ into local UV and fades local contribution near the domain edge. Relocating or resizing the domain is a controlled reset in the first version.

Alternative: use a second FFT for local waves. Rejected because an FFT domain is poorly suited to localized, continuously injected disturbances and naturally wraps energy across boundaries.

### 7. Move ocean optics into a dedicated forward surface pipeline

The ocean remains a forward-rendered surface but no longer uses the generic mesh material branch. The dedicated shader consumes cascade displacement in the vertex/domain stage and gradient, moment, foam, atmosphere, sun, and environment resources in the pixel stage. It implements:

- distance-weighted cascade composition and configurable sinusoidal UV warping;
- filtered slope-moment roughness, microfacet distribution, Smith masking, effective Fresnel, sun specular, and environment reflection;
- deep-water and scattering colors, crest-oriented transmission approximation, foam diffuse/roughness, and aerial transmittance;
- existing PrismRender exposure, bloom, and tonemapping ownership rather than an extra private tonemapper;
- normals, moments, foam, cascade, geometry LOD, and wireframe diagnostics.

The active physical atmosphere remains the authoritative sun and sky source. Shader constants use PrismRender's XZ-horizontal/Y-up convention; WaveWorks' XY-horizontal/Z-up sample convention is converted only during reference analysis, never leaked into runtime data.

Alternative: extend `Mesh.hlsl` further. Rejected because the ocean bindings and slope-statistics model are not general mesh-material concepts and would enlarge already complex forward/deferred variants.

### 8. Deliver geometry in two compatible steps

The surface renderer first supports the current camera-following clipmap using the new four-cascade shader. This isolates simulation and material correctness. The final adaptive path adds a quadtree patch selector with frustum culling, conservative displacement inflation, neighbor LOD balancing, geomorph factors, and a finite set of edge-compatible patch topologies suitable for instancing.

On tessellation-capable D3D12/Vulkan devices, patch vertices flow through Slang vertex/hull/domain shaders and use screen-space edge length for dynamic tessellation. `ShaderStage`, shader reflection masks, pipeline descriptions, cache keys, validation, D3D12 PSO construction, Vulkan tessellation state, and capabilities are extended together. On unsupported backends or when disabled, the existing clipmap path remains available and must render the same simulation/shading resources.

Alternative: require tessellation and remove the clipmap. Rejected because tessellation is optional on some Vulkan devices and a fallback is necessary for compatibility and staged rollout.

### 9. Expose scheduling through RenderGraph and enable async compute only after validation

Persistent ocean resources are imported into RenderGraph with per-subresource state. Each logical stage declares SRV/UAV dependencies, allowing D3D12 UAV barriers and Vulkan memory/layout transitions to be generated by the backend. The initial path stays on the graphics queue. After deterministic output and state validation pass, spectrum evolution, FFT, map, foam, local waves, and mips may move to the native compute queue, with the graph creating the compute-to-graphics synchronization before surface sampling.

The renderer consumes the newest completely published result and may render the previous result while a new async update is in flight. It never samples partially updated cascade slices.

Alternative: keep all dispatches inside one opaque graph pass. Rejected because it prevents scheduling analysis, precise profiling, resource lifetime optimization, and safe async migration.

### 10. Make queries explicitly asynchronous and demand driven

GPU query batches sample spectral and local displacement into a result buffer. Readback copies occur only for submitted CPU consumers and enter a finite FIFO tagged by simulation time and kick/sequence ID. Polling is non-blocking by default; an explicit diagnostic blocking mode is allowed but not used by frame rendering. Conservative displacement bounds derive from active spectrum energy plus configured local disturbance limits and are refreshed after relevant setting changes.

Alternative: sample the ocean synchronously on CPU. Rejected because duplicating four FFT fields on CPU is expensive and synchronous GPU readback would stall the render loop.

### 11. Model UI settings by ownership and dirty scope

`RenderSettings` gains a nested `OceanSettings` instead of more flat fields. It contains simulation, base wind, swell, foam, local waves, geometry, shading, query, debug, and quality groups. The WaveWorks Reference preset uses documented sample defaults; PrismRender explicitly labels its own quality-resolution mapping and implementation-specific limits.

UI edits produce one of: constants dirty, initial spectrum dirty, resource layout dirty, geometry dirty, local reset, or history reset. Expensive transitions occur at a safe frame boundary. `DebugPanel` keeps only the global feature toggle and a shortcut/focus action; `OceanLabPanel` owns detailed controls and diagnostics.

Alternative: call resource updates directly from ImGui widgets. Rejected because UI-time destruction or reallocation can race in-flight frame resources and makes automated setting tests difficult.

### 12. Preserve separate legacy and WaveWorks demonstration scenes

The existing FFT Ocean Lab remains a first-class scene with its original `FftOcean` defaults and visible result. A new WaveWorks Lab owns the WaveWorks Reference defaults and selects the native spectral implementation. The scenes may share renderer infrastructure, but scene initialization must not copy WaveWorks values into the legacy Lab or legacy compatibility values into the WaveWorks Lab. Only the active scene schedules ocean work. Temporary same-scene migration selectors and adapters may be removed after validation, but the legacy scene and the code required to render it remain available for direct visual and performance comparison.

## Planned File Layout

Principal files to add:

- `src/Renderer/Features/Ocean/OceanSettings.h`
- `src/Renderer/Features/Ocean/OceanStatistics.h`
- `src/Renderer/Features/Ocean/SpectralOceanSimulation.h/.cpp`
- `src/Renderer/Features/Ocean/OceanSpectrumGenerator.h/.cpp`
- `src/Renderer/Features/Ocean/OceanFft.h/.cpp`
- `src/Renderer/Features/Ocean/OceanMapBuilder.h/.cpp`
- `src/Renderer/Features/Ocean/OceanFoamSimulation.h/.cpp`
- `src/Renderer/Features/Ocean/LocalWaveSimulation.h/.cpp`
- `src/Renderer/Features/Ocean/OceanSurfaceRenderer.h/.cpp`
- `src/Renderer/Features/Ocean/OceanQuadtree.h/.cpp`
- `src/Renderer/Features/Ocean/OceanQueryService.h/.cpp`
- `src/UI/OceanLabPanel.h/.cpp`
- `assets/shaders/Ocean/OceanSpectrum.slang`
- `assets/shaders/Ocean/OceanFft.slang`
- `assets/shaders/Ocean/OceanBuildMaps.slang`
- `assets/shaders/Ocean/OceanFoam.slang`
- `assets/shaders/Ocean/OceanLocalWaves.slang`
- `assets/shaders/Ocean/OceanSurface.slang`
- focused tests under `tests/` for spectrum, FFT, settings, local waves, geometry, queries, RenderGraph, Slang stages, and RHI pipelines

Principal files or areas to modify:

- root/source CMake definitions for the new modules, shaders, and tests
- `src/Renderer/RenderSettings.h` and `src/Renderer/DemoSceneSettings.cpp`
- `src/Renderer/SceneRenderer*`, shared RenderGraph resources/callbacks, feature registration, and queue scheduling integration
- `src/Scene/TerrainOceanSceneFactory.cpp` and ocean render-surface declarations
- `src/UI/DebugPanel.cpp` and UI panel registration
- `src/Asset/SlangShaderCompiler.cpp` and shader-stage/reflection helpers
- `src/RHI/ShaderTypes.h`, `PipelineState.h`, `DeviceCapabilities.h`, pipeline validation/cache code, and D3D12/Vulkan pipeline/type-conversion implementations
- `assets/shaders/Mesh.hlsl` only to retire the new-ocean branch after the dedicated surface path is active
- Ocean Lab documentation and render-validation fixtures

Runtime data flows from validated `OceanSettings` into spectrum/resource dirty decisions, then through cached initial spectra, absolute-time evolution, inverse FFT, derived maps, temporal foam and optional local-wave passes. A coherent published resource version feeds either clipmap or quadtree/tessellated geometry and the dedicated forward ocean surface; optional queries and statistics observe that same version without becoming required rendering dependencies.

## Risks / Trade-offs

- [Closed WaveWorks core prevents exact numerical matching] -> Treat the sample as a visual/behavioral reference, document project-defined cascade bands and quality resolutions, and validate internal physical consistency rather than claiming equivalence.
- [Four cascades plus Extreme quality create high FFT and memory cost] -> Provide three presets, expose memory/timing, cache initial spectra, support reduced update rates later, and keep Extreme opt-in if reference hardware cannot meet the budget.
- [RGBA16F working FFT may accumulate visible error] -> Start critical working fields in FP32, add FP16 only after spectral and image tolerance tests.
- [Persistent histories complicate resize and frame overlap] -> Version resource sets, initialize histories explicitly, publish coherent results atomically, and retire old resources through frame-safe ownership.
- [Async compute can reduce rather than improve performance] -> Start on graphics, use existing queue scheduling telemetry, and enable native compute only when measured overlap exceeds synchronization cost.
- [Tessellation expands RHI and pipeline-cache surface area] -> Implement it as an optional isolated capability with validation and keep the clipmap fallback until backend tests pass.
- [Quadtree CPU selection may become a bottleneck] -> Group finite patch topologies for instancing, profile selection separately, and leave room for GPU-driven node generation after functional parity.
- [Local-wave 2048 grids are expensive and may exceed devices] -> Capability-check requested sizes, use 512 by default, and report clamping in UI.
- [Large controls panel can become difficult to use] -> Match WaveWorks' major groups, hide advanced/readback options by default, add reset/preset controls, and show units/tooltips.
- [Reference shader license constraints] -> Reimplement equations and architecture from public concepts, keep original code out of PrismRender, and record independent tests and source attribution where required.

## Migration Plan

1. Introduce nested settings, statistics, feature interfaces, and a legacy/new implementation selector without changing the existing FFT Ocean Lab result.
2. Generalize and test the existing FFT, add cached spectra, then run one new cascade through the current clipmap as a correctness checkpoint.
3. Add four cascade arrays and Phillips band partitioning as an architectural checkpoint; compare legacy displacement and seams.
4. Replace the final spectrum with base-wind plus swell JONSWAP, add the reference preset, and establish deterministic captures.
5. Add derived gradients, moments, persistent foam, mips, and the dedicated forward surface pipeline while the clipmap remains active.
6. Add local waves, disturbances, reset behavior, and composition.
7. Add query/readback and complete profiling/diagnostic UI.
8. Extend Slang/RHI tessellation, implement quadtree patch selection and adaptive rendering, retaining clipmap fallback.
9. Validate D3D12 and Vulkan, measure Normal/High/Extreme, fix validation findings, and keep the new implementation as the WaveWorks Lab default.
10. Remove temporary migration coupling only after image, correctness, compatibility, and performance acceptance gates pass. Preserve the original FFT Ocean Lab and its renderer path as the permanent comparison baseline.

## Open Questions

- The exact performance acceptance numbers will be recorded against the project's chosen reference GPU and resolution once a baseline capture is available; this does not alter architecture or required observability.
- The final foam and bubble detail textures may be authored assets or deterministic procedural assets, provided their license, repeatability, and required shading behavior satisfy the specifications.
