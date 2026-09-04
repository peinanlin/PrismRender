## Context

See `proposal.md` for motivation. The renderer currently selects `LegacyFft` or `SpectralOcean` through `OceanImplementation`, and the ocean surface is rasterized inside the ordinary GBuffer/forward geometry path. The spectral implementation already publishes four-cascade displacement, gradients, moments, foam history, bounded local-wave maps, adaptive quadtree geometry, queries, and stage timings. The shared RenderGraph already manages opaque GBuffer, depth/Hi-Z, HDR, motion vectors, TAA histories, shadows, atmosphere, SSR, bloom, and feature-owned subgraphs.

HPWater's relevant architectural contract is downstream of wave generation: a water-specific surface GBuffer, separate opaque/water depth, screen-space refraction, water BSDF/composite, camera-centered caustics, and low-resolution temporally reconstructed volumetrics. World-space ocean extent remains owned by the spectral maps and adaptive geometry. The implementation must be clean-room and use no Unity HDRP or HPWater source files.

## Goals / Non-Goals

**Goals:**

- Keep spectral/local simulation results identical between WaveWorks and HPWater Labs.
- Add an independent optics axis and dedicated Lab without duplicating simulation ownership.
- Preserve opaque depth/color before water so refraction and thickness have unambiguous inputs.
- Scale expensive optics by water-pixel coverage, quality, and camera-centered distance rather than total ocean area.
- Make history, reset, timing, and backend behavior explicit and testable.

**Non-Goals:**

- Replacing the spectral FFT with the HPWater finite-domain wave equation.
- Reproducing Unity HDRP's VBuffer, light loop, shadow atlas layout, or public API.
- Pixel-identical output or copied source/assets from HPWater.
- Shoreline topology, spray particles, multi-layer water bodies, planar fluid volumes, or general-purpose participating media in this change.
- Renaming the existing `OceanImplementation` public enum during the initial integration.

## Decisions

### 1. Add an independent optics selector

Introduce `OceanOpticsModel { Current, HpWater }` inside `OceanSettings`. `OceanImplementation` continues to select legacy versus spectral simulation for compatibility. The HPWater preset selects `SpectralOcean + HpWater`; existing presets select their current simulation plus `Current` optics.

Alternative: add `OceanImplementation::HpWater`. Rejected because HPWater is not a wave source and this would either duplicate FFT ownership or make the source of displacement implicit.

### 2. Reuse one simulation and geometry instance

The new Lab reuses `SpectralOceanSimulation`, `LocalWaveSimulation`, `OceanSurfaceRenderer`, query services, and their published resources. No second FFT dispatch, resource set, quadtree, or emitter state is created for HPWater optics. The Lab changes only scene identity, optical preset, pass scheduling, and UI.

Alternative: copy the WaveWorks scene and simulation objects into a new feature. Rejected because it prevents deterministic A/B comparison and doubles maintenance and GPU work.

### 3. Split opaque and water visibility before optical composition

When HPWater optics are active, the ordinary opaque GBuffer draw excludes ocean objects. Opaque deferred lighting produces `OpaqueHdr` and preserves `OpaqueDepth`/opaque Hi-Z. A dedicated water visibility pass copies opaque depth into a composite depth target, depth-tests and writes the spectral surface into it, and writes a water mask plus water material/motion targets. Downstream transparency consumes the composite depth, while refraction always reads the untouched opaque depth/Hi-Z.

The water targets use project-supported formats:

- `WaterGBuffer0`: `Rgba16Float`, encoded normal xyz and perceptual roughness.
- `WaterGBuffer1`: `Rgba8Unorm`, absorption rgb and foam.
- `WaterGBuffer2`: `Rgba8Unorm`, scattering rgb and water mask/flags.
- `WaterMotion`: the existing `Rg16Float` motion target at water pixels.
- `WaterCompositeDepth`: `D32Float`, initialized from opaque depth and updated by visible water.

Alternative: keep water in the ordinary GBuffer and reconstruct the background depth later. Rejected because the nearest depth is then water, so refracted scene intersection and thickness cannot be recovered robustly.

### 4. Own HPWater work in a feature subgraph

Add a `WaterOpticsFeature` under `Renderer/Features/Ocean/` that owns optical settings validation, history resources, pipelines, descriptors, timings, and an `AddPasses`/callback boundary. Its subgraph is inserted after opaque HDR/Hi-Z and before SSR/TAA/bloom. It consumes the shared spectral/local resources and returns the new HDR and composite depth versions. The shared frontend names only the feature boundary and authoritative dependencies; internal caustic and volumetric scratch resources remain private.

Alternative: add every optical scratch texture and callback directly to `SharedRenderGraphFrontend`. Rejected because it repeats the coupling already avoided by the fluid feature callback and makes the shared graph a feature-specific resource registry.

### 5. Use screen-space refraction with an explicit quality ladder

Default refraction uses water normal, opaque depth, water depth, and a bounded thickness-aware UV offset. High quality enables jittered depth-guided exponential stepping with a configurable finite sample count. Invalid/off-screen/foreground hits fall back to the original opaque color rather than sampling unrelated pixels. Near, middle, and far distances select ray-marched, approximate, and disabled/reflective modes respectively, with smooth transitions.

Alternative: make ray marching mandatory or use a Hi-Z traversal immediately. Rejected because screen-filling water makes mandatory traversal expensive and the reference architecture also treats high-precision marching as optional.

### 6. Evaluate water energy and medium response in a dedicated composite

The composite reconstructs water world position from water depth, evaluates Fresnel reflection using the existing environment/atmosphere resources, and applies Beer-Lambert transmittance and single-scattering response from non-negative absorption/scattering coefficients and refracted path length. Thin-layer side lighting and backlit transmission are bounded approximations controlled by thickness and phase. Existing simulated foam raises roughness, suppresses refraction, and adds diffuse/aerated response; procedural detail remains gated by simulated foam energy.

The composite writes HDR only at water-mask pixels and never invokes ordinary mesh material or clustered opaque BRDF code.

### 7. Limit caustics by camera-centered coverage, not ocean extent

Caustics consume the combined spectral/local normal field and directional light. Two camera-centered world-space cascades cover configurable near and middle receiver ranges. A compute accumulation buffer uses deterministic jitter and bounded atomics or additive accumulation, followed by depth/normal-aware reconstruction. The default single-channel mode computes one energy field; a high tier may accumulate RGB wavelengths. Contribution fades at cascade edges and with water path extinction.

Alternative: allocate caustic resolution proportional to the full ocean extent. Rejected because kilometer-scale uniform texel density is wasteful and visually relevant receivers are camera-local.

### 8. Reconstruct underwater volumetrics from low resolution with owned history

The camera-medium state uses the most recent coherent ocean height query at camera XZ with a hysteresis band and mean-sea-level fallback while query data is unavailable. When underwater, a half- or quarter-resolution pass integrates extinction, phase-weighted primary lighting, shadowing, and caustic contribution between camera and opaque/water boundaries. Motion/depth rejection precedes temporal accumulation, and a joint bilateral/À-trous reconstruction prevents foreground bleeding.

Optical history versions include resolution, optics quality, camera cut, scene identity, surface-history version, and explicit reset serial. A mismatch clears history before sampling it.

Alternative: run full-resolution volumetric marching. Rejected because cost scales poorly when the entire camera is underwater and existing motion/depth infrastructure supports reconstruction.

### 9. Keep settings, diagnostics, and resets orthogonal

Add nested reflection/refraction, medium, caustic, volumetric, distance-tier, and debug settings under an HPWater optics settings block. Optical-only edits invalidate constants or optical histories but never rebuild H0 or local-wave resources. Quality changes recreate only affected optical resources and retire old resource sets after frames in flight. Statistics report water coverage and every new GPU stage separately.

### 10. Validate equal-input behavior and backend parity

CPU tests prove preset isolation, wave/optics independence, validation, dirty scopes, tier transitions, and history versions. RenderGraph tests prove opaque depth is preserved, water passes are absent for existing Labs, and history dependencies are ordered. Shader/pipeline tests cover D3D12 DXIL and Vulkan SPIR-V reflection. Deterministic captures use identical wave time/camera for current and HPWater optics and report resolution and active views.

## Files to Add

- `src/Renderer/Features/Ocean/WaterOpticsSettings.h/.cpp`: HPWater optical settings, validation, dirty scopes, distance tiers, and history keys.
- `src/Renderer/Features/Ocean/WaterOpticsFeature.h/.cpp`: feature-owned GPU resources, pipelines, descriptors, subgraph registration, execution, and statistics.
- `assets/shaders/Ocean/WaterOptics.slang`: dedicated water GBuffer, refraction, BSDF/composite, and debug entries.
- `assets/shaders/Ocean/WaterCaustics.slang`: camera-centered caustic accumulation and reconstruction.
- `assets/shaders/Ocean/WaterVolumetrics.slang`: reduced-resolution accumulation, temporal rejection, and edge-aware reconstruction.
- `tests/WaterOpticsTests.cpp`: settings, tier, medium-state, history, graph, and shader-contract regression coverage.
- `docs/HPWATER_OCEAN_LAB_CN.md`: architecture, controls, captures, profiling, limitations, and clean-room reference note.

## Files to Modify

- `src/Renderer/Features/Ocean/OceanSettings.*` and `OceanStatistics.h`: independent optics selection, HPWater preset composition, dirty scopes, and stage statistics.
- `src/Renderer/SharedRenderGraphFrontend.*`, `SceneRenderer*`, `DeferredTransientLayout.*`, and scene pipeline planning: opaque/water pass split, feature subgraph boundary, resources, callbacks, and timings.
- `src/Scene/DemoSceneCatalog.*`, `TerrainOceanSceneFactory.*`, `src/Renderer/DemoSceneSettings.cpp`, and application scene handling: `hpwater-ocean` identity and shared scene/preset behavior.
- `src/UI/OceanLabPanel.*`, `DebugPanel.cpp`, and editor scene selection: HPWater controls, debug views, reset commands, and diagnostics.
- `assets/shaders/Ocean/OceanSurface.hlsl` or its Slang surface entry, shared shader bindings, and deferred/post-process shaders as required to exclude HPWater water from the ordinary material path and composite its optical result.
- `CMakeLists.txt` and existing scene, settings, RenderGraph, GPU shader, and bridge tests for build/test registration and compatibility fixtures.

## Data Flow

```text
shared OceanSettings
  -> SpectralOceanSimulation + LocalWaveSimulation
  -> coherent displacement / gradient / moments / foam maps
  -> OceanSurfaceRenderer adaptive geometry

opaque objects
  -> opaque GBuffer -> opaque deferred lighting
  -> OpaqueHdr + OpaqueDepth + OpaqueHiZ

spectral water geometry + coherent ocean maps
  -> dedicated water visibility/GBuffer
  -> WaterGBuffer[0..2] + WaterMask + WaterCompositeDepth + WaterMotion

OpaqueHdr/Depth/HiZ + water buffers + atmosphere/environment/shadows
  -> thickness/refraction
  -> HPWater BSDF composite
  -> optional camera-centered caustics
  -> optional underwater low-res accumulation/history/reconstruction
  -> composed HDR + WaterCompositeDepth
  -> SSR/TAA/bloom/tonemap/output
```

## Validation Method

- Run CPU unit tests for settings normalization, presets, dirty scopes, distance transitions, medium hysteresis, history invalidation, and scene isolation.
- Run RenderGraph tests for pass absence in existing Labs, opaque-depth preservation, water resource versions, composite-depth publication, pass culling, and history ordering.
- Compile/reflection-test every new shader entry as DXIL and SPIR-V and run D3D12 debug-layer plus Vulkan validation matrices through resize, scene switch, quality edit, camera cut, reset, and shutdown.
- Capture deterministic above-water near/horizon/refraction/foam/caustic and underwater/waterline images at equal simulation time and resolution; emit machine-readable stage timings and resource memory.
- Benchmark water-pixel coverage and per-stage GPU costs at 1280x800 and 2560x1417 with approximation versus high-quality paths, verifying distance tiers reduce work without removing the far FFT surface.

## Risks / Trade-offs

- **Full-screen water adds GBuffer and composite bandwidth** → use compact material targets, water-mask early exits, distance tiers, and separately report water-pixel coverage.
- **Depth-copy and water-depth ownership can break later transparency** → publish an explicit composite depth version and test opaque, water, and transparent occlusion ordering.
- **Ray-marched refraction can shimmer or hit foreground geometry** → default to bounded approximation, use deterministic temporal jitter, thickness rejection, and safe fallbacks.
- **Caustic atomics can overflow or become backend-dependent** → bound input energy, use named fixed-point scale when integer atomics are selected, and compare backend tolerance fixtures.
- **Underwater detection can oscillate near the surface** → use coherent query versions plus configurable hysteresis and reset temporal history on medium transitions.
- **Multiple histories increase resize/scene-switch risk** → centralize history versioning and retire resource sets only after frames in flight.
- **Scope can obscure the original renderer roadmap** → keep all code under the existing Ocean feature and expose each stage as an independently disableable Lab capability.

## Migration Plan

1. Add the optics enum/settings, Lab identity, preset isolation, and CPU/catalog tests with HPWater passes disabled by default.
2. Add water visibility resources and pass splitting, preserving byte-for-byte scheduling for existing Labs.
3. Add refraction and the HPWater BSDF/composite, then make the Lab opt in.
4. Add caustics, underwater medium detection, volumetrics, histories, controls, and profiling incrementally behind feature toggles.
5. Run D3D12/Vulkan validation and deterministic capture matrices before declaring the Lab complete.

Rollback is selecting `OceanOpticsModel::Current` or removing the `hpwater-ocean` catalog entry; existing Labs and their resources remain independent throughout migration.
