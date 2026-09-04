## Purpose

Correct cascade mapping and make the ocean surface visually varied, foam-capable, and independent of generic mesh shading.

## MODIFIED Requirements

### Requirement: Coherent cascade composition
The ocean surface SHALL consume one runtime cascade metadata contract for displacement, gradients, moments, foam, debug views, distance fades, and UV mapping. Sinusoidal warp SHALL be applied independently in each cascade's UV space after world-space mapping.

#### Scenario: Warp smallest cascade
- **WHEN** UV warp amplitude is 0.03
- **THEN** each cascade receives a 0.03-unit UV distortion rather than a distortion multiplied by its scale relative to the largest patch

#### Scenario: View the horizon
- **WHEN** fine cascades fall below useful screen resolution
- **THEN** their displacement and shading contributions fade smoothly while the coarsest valid ocean contribution remains visible

### Requirement: Detailed persistent foam appearance
The surface renderer SHALL combine weighted spectral history, current folding whitecaps, and local-wave foam with project-owned multi-scale density and bubble detail expressed in world-space units. Detail SHALL be gated by simulated foam or active breaking candidates.

#### Scenario: Calm water
- **WHEN** spectral and local foam energy are zero and no current folding exceeds the whitecap threshold
- **THEN** procedural or authored detail does not create visible white patches

#### Scenario: Moving wake
- **WHEN** the reference moving wake generates persistent local foam
- **THEN** the surface displays a spatially varied V-shaped white wake whose fine bubble structure remains stable in world space

### Requirement: Ocean-specific optical shading
The renderer SHALL execute an independent ocean pixel path that does not invoke the generic mesh pixel entry and that consumes authoritative atmosphere, sun, environment, slope-moment, auxiliary directional-light, and foam data.

#### Scenario: Inspect ocean shader work
- **WHEN** the WaveWorks Lab surface is rendered
- **THEN** generic mesh material textures and generic mesh direct-light evaluation are not executed before ocean optical shading

#### Scenario: Render ordinary mesh
- **WHEN** a non-ocean mesh is rendered after the ocean shader refactor
- **THEN** its existing shader bindings, reflection, and visible result remain unchanged

