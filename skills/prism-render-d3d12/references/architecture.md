# PrismRender Architecture Notes

## Core idea
PrismRender is a renderer-focused project that demonstrates modern D3D12 rendering architecture.

## Design priorities
1. Clear separation of platform, RHI, scene, renderer, and UI
2. Incremental extensibility
3. Performance visibility through profiling UI
4. Practical architecture suitable for interviews and portfolio explanation

## Preferred data flow
Scene -> RenderObject/Camera/Light -> Renderer -> RHI -> D3D12 Command Submission

## Future-friendly areas
- shadow pass separation
- post-process chain
- culling pipeline
- instance rendering
- PSO cache