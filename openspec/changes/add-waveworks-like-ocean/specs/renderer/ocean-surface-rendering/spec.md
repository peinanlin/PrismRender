## Purpose

Render the multi-scale spectral and local-wave results as a continuous, physically plausible ocean surface with stable LOD, atmospheric lighting, and diagnostic visualization.

## ADDED Requirements

### Requirement: Coherent cascade composition
The ocean surface SHALL combine four spectral displacement cascades and the optional local-wave displacement in a common world-space convention, with distance-based cascade fading and configurable UV warping.

#### Scenario: Travel from near field to horizon
- **WHEN** the camera moves across the Ocean Lab
- **THEN** the surface transitions between cascade contributions without a visible displacement pop or hard frequency boundary

#### Scenario: Disable UV warping
- **WHEN** UV-warp amplitude is set to zero
- **THEN** each cascade uses its unwarped world-space mapping while all other blending behavior remains unchanged

### Requirement: Ocean-specific optical shading
The renderer SHALL use spectral gradients and slope moments to evaluate water Fresnel response, microfacet sun specular, environment reflection, roughness, masking, deep-water color, and crest scattering.

#### Scenario: Low sun over rough water
- **WHEN** the sun is near the horizon and the ocean contains high slope variance
- **THEN** the highlight and reflection broaden consistently with the simulated slope moments rather than using only a constant material roughness

### Requirement: Atmosphere-consistent reflection and attenuation
The water surface SHALL consume the active physical-atmosphere or environment-lighting data so the reflected sky, sun color, aerial attenuation, exposure, bloom, and tonemapping remain consistent with the rest of the frame.

#### Scenario: Change atmosphere sun direction
- **WHEN** the active atmosphere sun direction changes
- **THEN** the ocean reflection and direct highlight update from the same authoritative sun data during the next rendered frame

### Requirement: Detailed persistent foam appearance
The surface renderer SHALL combine simulation foam energy with multi-scale foam and bubble detail, and SHALL adjust color, roughness, and subsurface appearance in foam-covered regions.

#### Scenario: Persistent whitecap
- **WHEN** foam energy remains above the visible threshold after a crest passes
- **THEN** a temporally continuous whitecap remains visible and gradually fades according to the foam simulation

### Requirement: Camera-dependent crack-free ocean geometry
The renderer SHALL provide camera-dependent geometry density, frustum culling with conservative displacement bounds, adjacent-LOD compatibility, and geomorphing without visible cracks in the rendered surface.

#### Scenario: Cross an LOD boundary
- **WHEN** camera motion causes an ocean region to change geometry LOD
- **THEN** the region morphs to the new representation without exposing a hole, T-junction crack, or one-frame topology pop

### Requirement: Tessellation capability fallback
The renderer SHALL use hull/domain tessellation when the active backend reports compatible support and SHALL retain a crack-free camera-following clipmap path when tessellation is unavailable or disabled.

#### Scenario: Backend lacks tessellation
- **WHEN** the Ocean Lab starts on a backend without the required tessellation capability
- **THEN** the ocean renders through the fallback geometry path and the UI reports the active path without disabling spectral simulation or shading

### Requirement: Ocean rendering diagnostics
The surface renderer SHALL expose wireframe, cascade-boundary, per-cascade contribution, foam-energy, normals, slope moments, and geometry-LOD debug views.

#### Scenario: Show cascade contributions
- **WHEN** the cascade debug view is enabled
- **THEN** each simulated cascade is visually distinguishable without changing its simulation result

### Requirement: Large-area continuity
The surface SHALL follow the camera in the horizontal plane using stable snapping or equivalent precision management and SHALL hide periodic repetition through multi-cascade composition and warping.

#### Scenario: Camera crosses a follow-grid boundary
- **WHEN** the camera crosses a geometry-follow snapping boundary
- **THEN** existing wave features remain world-space stable and the geometry reposition is not visible as swimming or a discontinuity

