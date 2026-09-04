## Why

PrismRender currently treats several globally available renderer technologies as if every frame uses them. Terrain, shadows, clustered/local-light shadows, GTAO, SSR/Hi-Z, and GPU-driven visibility can therefore enter an empty or unrelated scene's RenderGraph solely because their settings default to enabled, wasting render-lane and GPU work and obscuring meaningful empty-scene comparisons with Unity and Unreal.

## What Changes

- Publish immutable static scene feature usage and combine it with dynamic per-frame light/work usage.
- Resolve built-in feature execution from renderer capability, user policy, and frame applicability instead of policy alone.
- Make interactive terrain and erosion opt-in, lazily initialize terrain resources, and emit mutation work only for initialization or pending edits on one producer view.
- Skip directional shadows, clustered lighting, spot/point shadows, geometry-dependent GTAO, SSR/Hi-Z, and GPU-driven visibility when their required content is absent.
- Preserve TAA, bloom, tonemapping, sky, atmosphere, ocean, and fluid behavior when valid fullscreen or procedural inputs remain, even if the ordinary object list is empty.
- Add diagnostics and regression coverage for the empty workload and all built-in demo scenes so a feature-specific pass cannot silently leak into unrelated scenes.

## Capabilities

### New Capabilities

- `renderer/scene-driven-render-features`: Defines how frozen scene usage, per-frame dynamic usage, renderer policy, pending mutations, and view ownership jointly control built-in resources and RenderGraph passes.

### Modified Capabilities

None.

## Impact

- Renderer defaults and demo-scene settings.
- Immutable render-scene extraction and dynamic frame packets.
- Scene pipeline planning and built-in RenderGraph feature registration.
- Terrain lifecycle and descriptor fallback handling.
- Shadow, clustered-lighting, screen-space-effect, Hi-Z, and GPU-driven scheduling.
- RenderGraph diagnostics, unit tests, and D3D12 performance regression captures across every built-in scene.
- No public asset-format or graphics-API compatibility break is intended.
