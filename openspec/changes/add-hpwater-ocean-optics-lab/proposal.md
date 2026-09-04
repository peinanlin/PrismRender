## Why

PrismRender already produces kilometer-scale spectral ocean displacement and bounded interactive local waves, but its current water path couples wave-source selection to one forward/deferred optical model. A dedicated HPWater-style Lab is needed to demonstrate that the same large-area FFT surface can drive a water-specific GBuffer, refraction, absorption/scattering, caustics, and underwater volumetrics without duplicating or replacing the ocean simulation.

## What Changes

- Add a `hpwater-ocean` Lab that reuses the existing Spectral FFT, adaptive ocean geometry, foam history, and Local Wave Solver while selecting a new HPWater-style optics model.
- Separate ocean wave-source selection from water-optics selection so the WaveWorks and HPWater Labs can share identical simulation inputs and remain directly comparable.
- Split water from the opaque scene GBuffer path and preserve separate opaque-scene depth, water depth/mask, and water material buffers required for thickness-aware refraction and compositing.
- Add a water-specific optical pipeline with packed normal/roughness, absorption, scattering, foam, optional depth-guided refraction ray marching, Beer-Lambert attenuation, Fresnel reflection, thin-layer/backlit response, and an independent HDR composite.
- Add camera-centered near-field caustics and low-resolution underwater volumetric accumulation with temporal/depth rejection and edge-aware reconstruction; expensive effects fade or disable at distance while FFT geometry remains visible to the horizon.
- Add HPWater Lab controls, debug views, stage timings, quality tiers, deterministic captures, and validation that existing `ocean` and `waveworks-ocean` Labs remain unchanged.
- Keep all implementation project-owned and backend-neutral through the existing RHI/RenderGraph and Slang/HLSL conventions; do not import Unity HDRP source or HPWater files.

## Capabilities

### New Capabilities

- `renderer/ocean-optics-pipeline`: Decoupled large-area ocean wave sources and HPWater-style water GBuffer, refraction, BSDF, caustics, volumetrics, distance quality, and compositing behavior.
- `scene/hpwater-ocean-lab`: Dedicated CLI-selectable Lab that reuses the spectral ocean simulation and adaptive geometry while isolating HPWater-style presentation and validation defaults.
- `ui/hpwater-ocean-controls`: Controls, diagnostics, debug views, and per-stage performance reporting for the HPWater-style pipeline.

### Modified Capabilities

None. The existing Labs remain behaviorally compatible; the new capability consumes their shared renderer services without changing their contracts.

## Impact

- Scene/catalog: demo-scene identifiers, command-line parsing, scene factory presets, editor selection, and regression tests.
- Renderer: ocean settings taxonomy, shared RenderGraph resources/pass ordering, dedicated water depth/GBuffer resources, optical pipelines, history resources, and stage timings.
- Shaders: water GBuffer encoding, refraction/thickness, HPWater-style BSDF/composite, caustics, underwater volumetrics, temporal reconstruction, and debug entries.
- UI: a new HPWater Ocean Lab panel and quality/debug/statistics controls.
- Tests and documentation: settings separation, graph dependencies, shader reflection, deterministic image captures, D3D12/Vulkan validation, performance budgets, and clean-room reference notes.
- No new runtime dependency is introduced; the HPWater repository remains a behavioral and architectural reference only.
