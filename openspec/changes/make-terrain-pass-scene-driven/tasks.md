## 1. Scene and Frame Feature Usage

- [x] 1.1 Add immutable `RenderSceneFeatureUsage`, derive opaque, transparent, shadow-caster, terrain, and GPU-driven-candidate facts during scene-data rebuild, and verify empty, hidden/editor-only, ordinary, transparent, terrain, and invalid-binding cases in publication tests.
- [x] 1.2 Expose static usage through packet-backed render views without rescanning objects, and verify Game and Scene views sharing a scene revision observe identical usage.
- [x] 1.3 Add settings-independent `RenderFrameFeatureUsage` for effective directional, point, spot, shadowed-point, and shadowed-spot lights, and verify zero-count, zero-intensity, non-shadowing, and shadowing light cases.

## 2. Pure Pipeline Activation

- [x] 2.1 Extend pipeline-plan inputs and execution results to separate capability, policy, static applicability, dynamic applicability, pending work, and producer ownership; verify synthetic plan tests report the expected rejection reason.
- [x] 2.2 Gate directional and variance shadows on an effective directional light plus shadow-casting geometry, and verify an empty frame schedules neither while Shadow Lab remains active.
- [x] 2.3 Gate clustered lighting on effective local lights and split spot/point shadow execution by eligible light type plus caster presence; verify zero-light, non-shadowing, point-only, spot-only, and mixed cases.
- [x] 2.4 Gate GPU-driven visibility on at least one candidate and compute Hi-Z from active consumers only; verify zero-candidate frames declare neither visibility nor geometry-only Hi-Z work.
- [x] 2.5 Gate GTAO and SSR on conservative opaque-geometry usage while retaining independent planar-reflection/HPWater dependencies; verify empty deferred, ordinary geometry, planar-only, and water cases.
- [x] 2.6 Preserve TAA, bloom, tonemap, sky/atmosphere, ocean, and fluid eligibility for valid non-object inputs, and verify sky-only plan and graph tests retain required fullscreen/procedural work.

## 3. Terrain Policy, Work, and Resources

- [x] 3.1 Change general interactive-terrain and erosion defaults to disabled, retain explicit Terrain Lab/editor opt-in, and verify all 20 demo settings make Terrain Lab the only default terrain owner.
- [x] 3.2 Resolve terrain sampling from frozen terrain usage plus policy, add a const pending-work query, and gate mutation on initialization/edit work plus the shared producer decision; verify first, stable, brush, reset, full-update, and secondary-view frames.
- [x] 3.3 Import terrain textures only when sampling is active and declare `InteractiveTerrain.BrushAndErosion` only when mutation is active, preserving side-effect semantics; verify empty/non-terrain graphs have no terrain dependency and edit graphs contain exactly one pass.
- [x] 3.4 Separate stable shared-terrain construction from GPU initialization and initialize at first applicable activation; verify renderer startup outside Terrain Lab leaves terrain GPU resources uninitialized.
- [x] 3.5 Bind valid generic fallback textures before Terrain activation and refresh terrain-dependent bindings afterward; verify descriptor validation and rendering across Preview → Terrain Lab → Preview.

## 4. Diagnostics and Catalog Regression Matrix

- [x] 4.1 Expose per-feature policy, static usage, dynamic usage, pending/producer state, final activation, and rejection reason in architecture diagnostics; verify empty reports explain every inactive content-dependent family.
- [x] 4.2 Add a declarative feature-family expectation matrix for the out-of-catalog empty workload and all 20 `DemoSceneId` values; verify every catalog entry has exactly one matrix row.
- [x] 4.3 Add RenderGraph tests enforcing required/forbidden Terrain, shadow, clustered/local-shadow, GTAO, SSR/Hi-Z, GPU-driven, and fullscreen/procedural families from the matrix.
- [x] 4.4 Capture D3D12 graph reports across all built-in scenes and verify no non-terrain scene activates Terrain, zero-light scenes omit clustered/local-shadow work, and each feature lab either retains capability-supported advertised passes or records an explicit capability rejection with the correct fallback.

## 5. Build and Performance Validation

- [x] 5.1 Build affected unit and renderer targets in RelWithDebInfo and run scene-publication, frame-packet, pipeline-plan, RenderGraph, feature-lifecycle, and shader tests with no failures.
- [x] 5.2 Run D3D12 validation for Preview, Shadow Lab, Atmosphere Lab, Terrain Lab, ocean/fluid scenes, and Game+Scene dual view, confirming no descriptor, resource-state, or validation errors and no visual regressions.
- [x] 5.3 Re-run the matched 1280x800 Game-only empty benchmark and verify the prohibited content-dependent passes are absent, active-pass count and render-lane time decrease, and frame pacing does not regress relative to baseline `45aaa84ad0846d17`.
