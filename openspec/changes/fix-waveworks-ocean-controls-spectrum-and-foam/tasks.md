## 1. Baseline and Regression Fixtures

- [ ] 1.1 Capture the current WaveWorks Lab at 1280x800 and 2560x1417 on D3D12 with the reference camera, settings, active views, Normal/High/Extreme simulation timings, ocean surface timing, total GPU/CPU time, memory, dispatches, foam-energy view, and cascade view; preserve the images and machine-readable reports as pre-fix artifacts
- [x] 1.2 Add CPU regression fixtures that calculate the reference effective 4.7 Beaufort wind speed and the expected approximate 0.54 m base-wind and 48 m swell JONSWAP peaks; verify the current and replacement band metadata can be evaluated without GPU execution
- [x] 1.3 Add settings-path tests proving the spectral implementation reads nested `OceanSettings` and the legacy implementation reads its compatibility fields; include a fixture demonstrating that an edit to a visible spectral control cannot terminate in the obsolete flat field only

## 2. Authoritative WaveWorks Lab Controls

- [x] 2.1 Make the WaveWorks Lab panel a discoverable editor window or docked section with the reference group names `General settings`, `Wind waves`, `Local waves`, and `Quadtree / geometry parameters`, followed by Statistics and Debug; preserve the legacy FFT Lab panel separately
- [x] 2.2 Hide or disable the EditorLayer flat ocean wind/spectrum/choppiness/color controls while `SpectralOcean` is active, bind all visible spectral controls directly to nested `OceanSettings`, and verify legacy controls remain functional in the FFT Ocean Lab
- [x] 2.3 Complete the WaveWorks control matrix: simulation API/quality, CPU/GPU query/readback toggles, timers, wind/swell parameters, foam parameters, manual local disturbance, rain, moving wake, domain/grid, geometry, resets, diagnostics, and feature gating; label PrismRender-specific mappings rather than claiming undocumented WaveWorks equivalence
- [x] 2.4 Change reference local defaults to local enabled, demo emitters enabled, rain disabled, and moving wake enabled; derive displayed direction degrees from canonical stored vectors and verify loading the preset cannot show a degree value that differs from the active direction
- [x] 2.5 Correct dirty-scope ownership so time scale, lateral multiplier, foam, geometry, and shading-only changes do not claim or schedule H0 rebuilds; expose pending and active settings/resource versions and verify each representative UI change reports the expected scope

## 3. Resolution-Aware Four-Cascade Spectrum

- [x] 3.1 Replace patch-centered `L/2.5..L*2.5` bands with resolution-aware wavelength limits derived from patch length, FFT resolution, and adjacent-cascade texel scales; document the stable Nyquist margin and overlap width as named constants rather than magic values
- [x] 3.2 Implement normalized logarithmic overlaps for CPU `BandWeights` and Slang `CascadeBandWeight` from one matching metadata contract; verify weights sum to one across the supported range and CPU/GPU sampled weights agree within tolerance at Normal, High, and Extreme
- [x] 3.3 Verify the reference approximately 0.54 m base-wind peak and 48 m swell peak receive non-zero ownership and finite energy, disabling swell does not perturb the deterministic base realization, and no resolvable wavelength falls through every band
- [x] 3.4 Publish explicit cascade patch lengths, wavelength bounds, UV scales, UV offsets, ordering, and distance-fade data to the surface path; remove spectral hard-coded patch arrays from shared mesh/ocean sampling and verify debug views use the same metadata
- [x] 3.5 Add coherent live spectrum replacement or an equivalent frame-safe commit that keeps the previous complete spectrum visible while a new wind/fetch/peaking/amplitude version is generated; verify rapid wind slider edits never expose mixed cascade versions, stale foam, or resource retirement errors

## 4. Correct Cascade Mapping and Surface Sampling

- [x] 4.1 Compute each cascade UV directly from undisplaced world XZ and apply sinusoidal warping after that mapping in cascade UV units; verify a 0.03 warp remains 0.03 for every cascade and zero warp reproduces unwarped world-stable sampling
- [x] 4.2 Implement fine-to-coarse distance/projected-footprint fading using the common metadata while preserving the coarsest horizon contribution; apply identical weights to displacement, gradients, moments, foam, and debug visualization and verify camera motion produces no rings, pops, or swimming
- [x] 4.3 Replace fixed ring mip assumptions with a validated displacement density strategy and derivative-aware pixel sampling that avoids both under-sampled high-frequency displacement and premature loss of near-field detail; verify near, middle, and horizon captures retain distinct scales without aliasing
- [x] 4.4 Add deterministic cascade/normal captures and analytic fixtures confirming short wind waves vary independently over longer swell, cascade boundaries remain energy-continuous, and the current coordinated swell-only appearance is rejected by image or numerical metrics

## 5. Spectral and Local Foam

- [x] 5.1 Validate Jacobian/folding semantics and thresholds between map generation, spectral foam history, and surface wave-hat shading; ensure reference wind/choppiness can produce bounded whitecap candidates while calm water remains zero
- [x] 5.2 Update spectral foam accumulation/composition so four cascade histories are combined with scale-aware weights rather than only a maximum sample, and verify a generated crest persists then decays monotonically under dissipation and falloff
- [x] 5.3 Fix `LocalWaveSimulation::Advance` so demo and externally queued disturbances are finalized before the GPU upload snapshot; verify a reference wake frame contains a non-empty deterministic GPU batch and reset clears pending and published disturbances
- [x] 5.4 Make the GPU local solver cell-size aware with bounded stable integration, absorbing/faded boundaries, finite guards, and lateral displacement derived from the local surface; verify impulse propagation, long-frame stability, energy decay, and absence of wraparound on D3D12 and Vulkan
- [x] 5.5 Pass local whitecap threshold, generation threshold, generation amount, dissipation, and temporal falloff to the GPU solver and generate local foam from folding/breaking candidates plus persistent history; verify disabling local foam leaves wake displacement visible and disabling the emitter lets existing foam decay
- [x] 5.6 Provide a manual Add Disturbance action and a deterministic moving-wake path that remains observable inside the default 200 m domain/reference camera; verify the default scene produces a V-shaped wake without requiring hidden setup
- [x] 5.7 Replace the incorrectly double-scaled low-frequency foam noise with project-owned multi-octave density/bubble detail expressed directly in world meters; gate it by spectral/local energy or active breaking and verify it adds near-field structure without creating foam on calm water
- [x] 5.8 Advect spectral foam history with the bounded per-cascade surface velocity inferred from consecutive displacement maps, gate new breaking energy by folding and rising crests, and verify D3D12/Vulkan shader compilation plus default/strong-wind/wake captures remain finite and localized
- [x] 5.9 Refine breaking with principal compression and local crest sharpness, advect local-wave foam with bounded lateral/wake flow, and shade persistent energy as layered thin film, thick foam, and world-space cellular/ridged bubbles; verify calm water stays clear and default/strong-wind/wake captures remain localized within the accepted surface budget
- [x] 5.10 Replace pre-stamped circular/Kelvin foam sources with the reference moving 10x10 positive hull-disturbance block, generate the wake through the local wave equation, and restrict foam generation to subsequent rising compressed crests; retain sub-0.02 ms local simulation cost in the 1280x800 Extreme acceptance capture

## 6. Independent Ocean Pixel Pipeline

- [x] 6.1 Refactor ocean shader declarations/helpers so `OceanPSMain` no longer calls the generic `PSMain`; keep shared frame, atmosphere, lighting, and binding definitions cleanly reusable without executing generic mesh material sampling
- [x] 6.2 Implement the complete independent ocean optical result: moment-filtered primary and auxiliary directional specular, effective Fresnel/environment reflection, deep-water/scattering/crest response, atmosphere attenuation, foam diffuse/roughness/underwater contribution, and existing frame output encoding
- [x] 6.3 Update `OceanSurfaceRenderer` layouts/descriptors and pipeline tests for the reduced ocean-only resource set; verify D3D12 DXIL and Vulkan SPIR-V reflection agree and ordinary mesh pipelines remain bytecode/layout compatible with their pre-change contract
- [x] 6.4 Re-run low-sun, rough-water, whitecap, wake, normal, moments, cascade, geometry LOD, and wireframe visual checks; verify debug modes do not mutate simulation and the reference scene no longer exhibits broad uniform smear or missing foam

## 7. Performance and Cross-Backend Acceptance

- [x] 7.1 Add ocean surface GPU timing and benchmark metadata for output resolution, active rendered views, quality, local grid/emitter state, simulation stages, surface, total renderer GPU/CPU time, memory, and dispatches; ensure a hidden editor Scene viewport contributes no render work
- [x] 7.2 Profile 1280x800 and 2560x1417 separately on D3D12, identify the remaining dominant simulation/surface stages after correctness fixes, and remove inactive local dispatch, unnecessary distant samples/mips, duplicate material work, or descriptor churn without changing accepted output
- [x] 7.3 If the portable FFT still exceeds the approved simulation budget, add a separately tested optimization such as FP16 working data, fused/shared-memory stages, or reduced-rate fine-cascade publication behind capability/quality policy; retain deterministic and D3D12/Vulkan tolerance gates and do not silently lower the selected quality
- [ ] 7.4 Run D3D12 debug-layer and Vulkan validation matrices across live wind edits, quality changes, pause/resume, wake/rain/manual disturbance, foam resets, resize, scene switching, and shutdown; verify no descriptor, barrier, non-finite, device-removal, or in-flight retirement errors
- [x] 7.5 Produce post-fix equal-resolution captures and reports for calm wind, default reference, swell disabled, strong wind whitecaps, moving wake, rain, cascade view, and foam-energy view; compare against the WaveWorks reference qualitatively without claiming pixel identity
- [x] 7.6 Verify final acceptance: wind controls visibly affect the running ocean, time scale zero freezes spectral motion, swell zero removes coordinated long ridges, default moving wake produces persistent detailed foam, legacy FFT Lab remains unchanged, and measured Normal/High/Extreme budgets are displayed accurately
- [x] 7.7 Update ocean technical documentation with corrected wavelength bands, live settings lifecycle, cascade metadata/mapping, local disturbance flow, foam model, independent shader path, benchmark methodology, limitations, and clean-room reference note
