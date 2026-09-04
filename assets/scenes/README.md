Place `StartupScene.gltf` in this folder to let PrismRender load an external startup scene.

Recommended layout:

- `assets/scenes/StartupScene.gltf`
- `assets/scenes/StartupScene.bin`
- `assets/scenes/<textures...>`

If the file is missing, invalid, or glTF support is not compiled in, PrismRender falls back to the built-in sample scene.

After launching PrismRender, check the debug panel:

- `glTF Import`
- `Fallback Scene`
- `Scene Source`
- `Scene Status`

Those fields tell you whether the external scene was really loaded.

## Built-in renderer demos

The Debug Panel exposes a `Demo Scene` combo for switching between the
built-in feature scenes at runtime:

- `Editor Preview` - editor and startup glTF workflow.
- `Renderer Showcase` - materials, instancing, post-processing, IBL and SSR.
- `Reflection Lab` - IBL fallback, validated SSR and mirror-camera planar
  reflections.
- `Shadow Lab` - Hard/PCF/PCSS/VSM/EVSM directional CSM plus local-light
  shadows and cascade debug tint.
- `Forward+ Lab` - 32 point lights over 48 PBR subjects, sharing the same
  cluster lists as Deferred.
- `GPU Driven Stress Lab` - 120 repeated subjects for culling, instancing and
  indirect draw comparisons.
- `Post Process Lab` - controlled HDR emitters for Bloom, Tonemap, TAA and
  GTAO comparisons.
- `RenderGraph Lab` - full graphics/compute pass chain and GPU timeline.
- `Material Lab` - a 5x5 metallic/roughness PBR matrix.
- `Asset Streaming Lab` - async cooked-asset upload, residency budget and
  eviction statistics.

The same scenes can be selected at startup with
`PRISM_RENDER_DEMO_SCENE=preview|showcase|reflections|shadows|lights|gpu-driven|post-process|render-graph|materials|streaming`.
For example, in
PowerShell:

```powershell
$env:PRISM_RENDER_DEMO_SCENE='shadows'
.\build-windows-ci\Debug\PrismRender.exe --api=d3d12
```

In `Forward+ Lab`, toggle `Deferred Rendering` to compare Deferred and
Forward+ with the same 32 lights. Disable `Forward+ (when Forward)` to inspect
the four-light compatibility path. GPU and CPU timings are visible in the
same Debug Panel.

`Asset Streaming Lab` automatically enables the asynchronous streaming
manager when selected at startup. Use
`PRISM_RENDER_ASSET_STREAMING_BUDGET_MB` to force a small residency budget and
observe eviction counters.
