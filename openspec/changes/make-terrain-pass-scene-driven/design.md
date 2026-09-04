## Context

See `proposal.md` for motivation and the scene-driven feature spec for observable behavior. Today `BuildScenePipelinePlan` mostly copies `RenderSettings` booleans into graph options. The empty workload clears objects and active lights and zeros the directional light, but default settings still schedule Terrain, Shadow, ClusteredLightBuild, SpotShadows, PointShadows, GTAO, SSR-driven Hi-Z, SSR, TAA, and Bloom. The same Terrain default reaches 17 of the 19 non-terrain built-in scenes; only the WaveWorks and HPWater scenes explicitly disable it.

The immutable scene packet already separates stable object topology from dynamic lights and is shared across Game and Scene views. This is the natural boundary for publishing applicability without repeated per-view scans.

## Goals / Non-Goals

**Goals:**

- Establish one testable feature-resolution model for built-in passes.
- Remove content-dependent work from empty and unrelated scenes.
- Keep static usage immutable and dynamic usage cheap to derive each frame.
- Preserve procedural and fullscreen effects when they have valid non-object inputs.
- Preserve shared simulation ownership across multiple views.
- Make every rejection reason visible in diagnostics.

**Non-Goals:**

- Change shading equations, shadow filtering, AO/SSR quality, terrain erosion math, ocean, or fluid simulation.
- Introduce per-material SSR eligibility in the first implementation; visible opaque geometry is the conservative applicability boundary.
- Eliminate the base color/depth pipeline or specialize a separate sky-only renderer.
- Evict feature GPU resources after first initialization.
- Treat object count alone as permission to disable TAA, bloom, tonemapping, sky, atmosphere, ocean, or fluid.

## Decisions

### Resolve activation from capability, policy, applicability, and work

Every built-in feature resolves through the same conceptual expression:

`active = capabilitySupported && policyEnabled && frameApplicable`

Persistent mutation features additionally require `pendingWork`, and shared simulations require `producerView`. This replaces scattered scene-specific disable assignments as the correctness mechanism; explicit demo settings remain presentation policy.

Relying on RenderGraph culling was rejected because side-effect passes are intentionally non-cullable and downstream reads can keep otherwise neutral producers live. Returning early inside callbacks was rejected because graph construction, transitions, scheduling, and command recording still occur.

### Split static and dynamic feature usage

Add `RenderSceneFeatureUsage` to immutable `RenderSceneData`. It is computed while the frozen object array and bindings are rebuilt and initially records:

- renderable opaque geometry;
- renderable transparent geometry;
- shadow-casting geometry;
- terrain surfaces;
- GPU-driven candidates.

Add a compact `RenderFrameFeatureUsage` resolved once from frozen usage plus `RenderFrameDynamicData`, settings-independent light facts, and feature-owner pending state. It records effective directional-light presence, point/spot-light presence, and shadow-eligible point/spot-light presence. The plan consumes this value rather than scanning scene objects or lights.

Keeping all facts in `RenderSettings` was rejected because settings represent user policy, not content. Putting dynamic light facts in immutable scene data was rejected because lights may change each logical frame without topology rebuild.

### Keep geometry classification conservative and reusable

A render object contributes usage only when it is selected for the relevant view-independent scene set, visible, not editor-only for Game content, and has enough mesh/material binding information to enter rendering. Render queue determines opaque versus transparent usage. Terrain and ocean surface identities are recorded separately.

The first implementation treats ordinary opaque geometry as potentially relevant to GTAO and SSR. Per-material roughness/reflectivity masks are deferred because a conservative false positive is preferable to changing reflection output; the empty-scene false positive is still removed.

### Gate lighting and shadow families independently

Directional shadow execution requires enabled shadow policy, an effective directional light, and at least one shadow caster. Variance-shadow filtering follows the resolved directional shadow result.

Clustered-light building requires clustered policy and at least one effective supported local light. Local shadows resolve spot and point work independently from shadow-eligible light counts and caster availability. The graph API therefore carries separate `spotShadows` and `pointShadows` decisions instead of one broad local-shadow boolean.

This avoids the current empty frame declaring all three shadow families and avoids Atmosphere Lab building empty clusters while preserving local lighting in scenes that actually own lights.

### Gate geometry-dependent screen-space and visibility work

GTAO requires deferred rendering plus applicable opaque geometry. SSR requires deferred rendering, SSR policy, and applicable opaque geometry. Planar reflection remains independently scene/policy driven and can still make the composite path active. Hi-Z resolves from the union of active consumers: GPU-driven visibility with candidates, SSR, and HPWater visibility.

GPU-driven visibility requires both backend support/policy and at least one static candidate. A zero-candidate frame uses the ordinary empty geometry path and does not create indirect or visibility work.

### Preserve fullscreen and procedural inputs

TAA and bloom remain policy/pipeline driven in this change. Empty ordinary geometry does not imply an empty HDR image because sky, atmosphere, ocean, fluid, and future fullscreen features can contribute. Tonemap and output readiness remain mandatory presentation work. This conservative boundary prevents the optimization from changing valid sky-only output.

### Preserve Terrain-specific lifecycle decisions

Terrain defaults become false and Terrain Lab explicitly enables them. Sampling requires `hasTerrainSurface`; mutation also requires `InteractiveTerrain::HasPendingUpdate()` and the coordinator-selected producer view. Shared Terrain construction is separated from GPU initialization, which occurs before graph construction at first applicable activation. Generic descriptors use a valid fallback height texture until activation.

The pass remains side-effecting and non-cullable when genuinely declared because its persistent heightfield writes are semantic output.

### Validate every scene with a declarative expectation matrix

Create one matrix keyed by `DemoSceneId` plus the out-of-catalog empty workload. Each row states required and forbidden feature families rather than duplicating an exact full pass list that would make unrelated pipeline evolution brittle. Unit tests validate plan decisions from synthetic usage; graph tests validate pass presence; D3D12 captures validate runtime integration.

Diagnostics expose policy, static applicability, dynamic applicability, pending-work/producer state, and final activation so a missing pass and a correctly culled pass are distinguishable.

Feature-lab validation is capability-aware. A lab retains its advertised pass family when the active backend supports that implementation; an unsupported backend instead records a capability rejection and validates the documented fallback. In particular, the current D3D12 path does not support indexed-object GPU-driven drawing, so its GPU Driven Lab validation checks explicit capability rejection rather than requiring indirect-visibility passes that cannot execute correctly.

## Risks / Trade-offs

- [Risk] Static classification can become stale after visibility or material changes. → Recompute it with the same scene-data revision invalidation used for topology and render-binding changes, and cover hide/queue/surface changes in publication tests.
- [Risk] A feature may be gated too aggressively and alter output. → Start with conservative applicability, retain fullscreen/procedural effects, and compare scene-matrix captures before tightening material-level rules.
- [Risk] Splitting point and spot shadows changes graph resource fallback assumptions. → Keep valid fallback textures/descriptors and test each family independently and together.
- [Risk] Lazy Terrain initialization can hitch on scene transition. → Initialize at the ordered scene-change boundary, retain resources afterward, and measure transition separately from steady state.
- [Risk] Game and Scene views can disagree about dynamic usage or mutation ownership. → Derive usage once per logical packet and retain the coordinator's single-producer decision.
- [Risk] Diagnostic matrices become maintenance overhead. → Assert feature families and reasons rather than every base pass and require a row when adding a catalog scene.
- [Risk] A feature lab can advertise technology unavailable on one backend. → Make catalog captures capability-aware and require an explicit rejection reason plus fallback validation instead of pretending the pass executed.

## Migration Plan

1. Publish static and dynamic usage with diagnostics while preserving existing execution decisions.
2. Gate zero-risk empty work first: Terrain, empty clustered lighting, local shadows, directional shadows, and zero-candidate GPU-driven visibility.
3. Gate GTAO and SSR/Hi-Z using conservative opaque-geometry usage.
4. Introduce lazy Terrain resources and descriptor fallbacks.
5. Run unit/graph tests and D3D12 captures for the empty workload and all built-in scenes, then compare matched performance reports.
6. Roll back individual applicability gates independently if a scene output regression is found; retain usage publication and diagnostics for investigation.
