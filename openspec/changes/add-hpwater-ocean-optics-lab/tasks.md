## 1. Optics Contract and Lab Identity

- [x] 1.1 Add `OceanOpticsModel`, nested HPWater optics settings, validation/normalization, reference defaults, and optical dirty scopes; verify new WaterOptics settings tests cover finite clamping and prove optical-only edits do not rebuild H0 or local-wave resources
- [x] 1.2 Add optical history keys/versions and statistics fields for visibility, refraction, composite, caustics, and volumetrics; verify unit tests cover resize, quality, camera-cut, scene, surface-history, and explicit-reset invalidation
- [x] 1.3 Add the `hpwater-ocean` scene id, catalog entry, parser aliases, application routing, and editor selection; verify scene catalog and world-render bridge tests launch the new id from `--scene=hpwater-ocean`
- [x] 1.4 Compose an HPWater Lab preset from `OceanSettings::WaveWorksReference()` plus HPWater optical defaults and reuse the WaveWorks scene geometry/camera/light factory; verify tests compare spectral, local-wave, adaptive-geometry, camera, and light inputs between both Labs while confirming existing Lab defaults remain isolated

## 2. Opaque and Water Pass Separation

- [x] 2.1 Extend the scene pipeline plan so HPWater optics excludes ocean objects from ordinary opaque GBuffer/forward draws but leaves existing paths byte-for-byte scheduled as before; verify pipeline-plan tests cover deferred, forward, disabled water, and all three ocean Labs
- [x] 2.2 Add project-owned water material, water mask, composite depth, and optical scratch/history texture descriptions with named formats and scaled extents; verify transient-layout/resource tests cover allocation, resize, alias compatibility, and zero/one-pixel dimensions
- [x] 2.3 Add a dedicated water visibility pipeline that consumes existing spectral/local descriptors, writes finite water GBuffer data and motion, and depth-tests against an opaque-depth-initialized composite depth; verify shader reflection and GPU fixtures cover opaque occlusion, water mask, normal/roughness, coefficients, foam, and displaced motion
- [x] 2.4 Publish untouched opaque depth/Hi-Z and the updated composite depth as distinct RenderGraph versions, then route downstream transparency to composite depth; verify graph tests assert copy/write/read ordering, pass culling, and absence of stale-version or read/write conflicts

## 3. Water Optics Feature Infrastructure

- [x] 3.1 Add `WaterOpticsFeature` ownership for pipelines, descriptors, constant buffers, private scratch/history resources, resize/recreation, frames-in-flight retirement, resets, and statistics; verify lifecycle tests initialize, resize, reset, scene-switch, and destroy without leaked retired sets
- [x] 3.2 Add the feature subgraph callback/result boundary after opaque lighting/Hi-Z and before SSR/TAA/bloom, importing coherent spectral/local maps and shared lighting resources without duplicating FFT dispatch; verify RenderGraph tests show exactly one spectral simulation chain and one ordered water-optics subgraph
- [x] 3.3 Add water optical constants and descriptor updates for both indexed/adaptive and tessellated surface variants; verify D3D12 DXIL and Vulkan SPIR-V reflection agree on logical bindings and ordinary ocean/mesh pipeline layouts remain compatible
- [x] 3.4 Add non-mutating resource capture hooks for water depth/mask and all three water GBuffer targets; verify capture tests can request each stage without changing simulation or optical history versions

## 4. Thickness-Aware Refraction

- [x] 4.1 Implement bounded approximate refraction from water normal, opaque depth, water depth, and scene color with off-screen, foreground, zero-thickness, and non-finite fallbacks; verify analytic shader fixtures and deterministic captures cover valid hits and each rejection path
- [x] 4.2 Implement optional jittered exponential depth-guided ray marching with validated sample count, step bounds, thickness offset, and deterministic frame noise; verify CPU parameter tests and GPU captures show improved disjoint-object alignment without self-intersection or NaN output
- [x] 4.3 Integrate refraction resources, pass execution, quality fallback, debug hit/thickness output, and GPU timing into `WaterOpticsFeature`; verify graph/timing tests prove the ray-march pass is absent when disabled and high-quality mode reports its effective samples and cost

## 5. HPWater-Style BSDF and Composite

- [x] 5.1 Implement clean-room water optical helpers for Fresnel energy partition, Beer-Lambert transmittance, single-scattering albedo/phase response, thin-layer side response, and bounded backlit transmission; verify CPU/shader agreement fixtures cover zero coefficients, increasing thickness, grazing angles, and finite extreme inputs
- [x] 5.2 Implement the dedicated water composite using opaque/refraction color, environment/atmosphere, directional light/shadows, slope moments, deep-water coefficients, and water mask; verify no ordinary mesh material textures or generic opaque BRDF entry executes for HPWater pixels
- [x] 5.3 Integrate spectral/local foam as roughness, diffuse aeration, and refraction suppression while retaining project-owned world-space detail only where simulated foam exists; verify calm/default/wake/strong-wind captures keep calm water clear and foam localized
- [x] 5.4 Publish composed HDR and composite depth to downstream SSR/TAA/bloom/tonemap, including water motion vectors and output encoding; verify deterministic near/horizon captures and TAA motion tests show continuous large-area water without foreground overwrite or swimming

## 6. Large-Area Distance Quality

- [x] 6.1 Implement validated near/middle/far optical ranges and smooth normalized tier weights independent of FFT cascade distances; verify unit tests cover ordering repair, boundary continuity, camera-relative movement, and a far tier that never removes spectral geometry
- [x] 6.2 Apply tier weights consistently to local-wave sampling, ray marching, approximate refraction, caustics, volumetric participation, and far reflection/absorption; verify debug-tier captures show smooth transitions with local effects bounded and the horizon continuously shaded
- [x] 6.3 Add water-pixel early-outs and resolution/sample policies for Normal/High/Extreme optics quality without silently altering spectral simulation quality; verify graph dispatch sizes and benchmark metadata reflect selected optical quality and current water coverage

## 7. Camera-Centered Caustics

- [x] 7.1 Add two configurable camera-centered caustic cascade transforms, snapping, coverage fades, resources, and dirty/history rules; verify unit tests cover stable snapping, edge-weight normalization, resize, and camera jumps
- [x] 7.2 Implement bounded single-channel caustic accumulation from combined spectral/local normals, primary-light direction, underwater receiver depth, and extinction; verify GPU fixtures show near receivers respond to changing normals while above-water/out-of-range receivers remain zero
- [x] 7.3 Add high-quality RGB dispersion and depth/normal-aware reconstruction behind quality/capability policy with deterministic fixed-point or additive bounds; verify backend tolerance tests prevent overflow and compare single/RGB outputs on D3D12 and Vulkan
- [x] 7.4 Composite caustic energy into valid opaque underwater receivers and underwater volumetrics with debug cascade/energy views and stage timings; verify deterministic caustic captures remain camera-stable and fade cleanly outside coverage

## 8. Underwater Medium and Volumetrics

- [x] 8.1 Add camera-medium classification from the coherent ocean query at camera XZ with configurable hysteresis, mean-sea-level fallback, and transition/history events; verify unit tests cover above, below, oscillating waterline, unavailable query, scene switch, and full reset
- [x] 8.2 Implement reduced-resolution underwater accumulation for extinction, phase-weighted directional lighting, shadows, and optional caustics between camera and valid scene/water bounds; verify compute tests cover half/quarter extents, finite integration, disabled mode, and empty pixels
- [x] 8.3 Implement motion/depth/medium rejection and ping-pong temporal accumulation keyed by optical/surface history versions; verify disocclusion, camera cut, resize, quality, surface reset, and waterline transition tests reject stale history
- [x] 8.4 Implement joint bilateral/À-trous reconstruction and HDR composition with a stable waterline boundary; verify underwater, foreground-edge, moving-camera, and waterline captures have no persistent halo, foreground bleed, or ghosting

## 9. HPWater Lab Controls and Diagnostics

- [x] 9.1 Add the HPWater Ocean Lab panel with shared spectral/local controls plus grouped material, refraction, caustic, volumetric, distance, quality, and reset controls; verify UI/settings tests route spectral edits through existing dirty scopes and optical edits only through optical scopes
- [x] 9.2 Add effective-mode feedback for quality, resolution, samples, distance ranges, capability fallbacks, history validity, and camera medium state; verify unsupported/disabled fixtures display the effective fallback rather than the requested unavailable mode
- [x] 9.3 Add non-mutating debug views for water visibility/materials, thickness/refraction, distance tier, caustics, and volumetric history rejection; verify selecting each debug view leaves spectrum, local-wave, foam, and optical history versions unchanged
- [x] 9.4 Add per-stage CPU/GPU timings, memory, water coverage, dispatch/draw counts, active views, and capture metadata; verify a machine-readable benchmark report contains all required fields at 1280x800 and 2560x1417

## 10. Compatibility, Validation, and Documentation

- [x] 10.1 Extend settings, scene, pipeline-plan, RenderGraph, shader, and world-bridge regression suites and CMake registration; verify the complete CPU test suite passes and `ocean`/`waveworks-ocean` schedule no HPWater resources or passes
- [x] 10.2 Run the D3D12 debug-layer matrix through approximation/ray-march quality, caustic modes, above/underwater transitions, live settings, resets, resize, camera cuts, scene switching, and shutdown; verify no descriptor, barrier, non-finite, device-removal, or in-flight retirement errors
- [x] 10.3 Run the Vulkan validation matrix for the same states and compare logical bindings plus deterministic numerical/image tolerances against D3D12; verify no validation errors or backend-only shader source paths
- [x] 10.4 Produce equal-input current-versus-HPWater captures for near, horizon, refraction, foam, caustics, underwater, and waterline views, benchmark each optical quality, and document architecture, controls, data flow, limitations, clean-room provenance, and launch commands in `docs/HPWATER_OCEAN_LAB_CN.md`
