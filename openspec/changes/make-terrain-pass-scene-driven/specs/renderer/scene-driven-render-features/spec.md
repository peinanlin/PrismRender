## Purpose

Defines how immutable scene content, dynamic frame content, renderer policy, pending work, and view ownership determine which built-in rendering resources and passes are active for a frame.

## ADDED Requirements

### Requirement: Feature execution requires policy and applicable frame content
The renderer SHALL distinguish support for a feature from user policy and from applicability to the current frame. A feature-specific pass SHALL be declared only when the renderer supports it, policy enables it, and all content required to produce a non-neutral result is present.

#### Scenario: Empty frame with globally enabled policy
- **WHEN** a frame contains no renderable surfaces, no active lights, no procedural surface, and no pending simulation work
- **THEN** the graph contains no terrain, shadow, clustered-light, local-light-shadow, GTAO, SSR, geometry-driven Hi-Z, or GPU-driven visibility pass

#### Scenario: Unrelated scene
- **WHEN** policy enables a feature but frozen or dynamic scene usage reports none of that feature's required content
- **THEN** the feature remains inactive without requiring that scene to explicitly disable it

#### Scenario: Applicable content appears
- **WHEN** required scene content appears while the corresponding policy is enabled
- **THEN** the feature becomes active for that frame without renderer restart

### Requirement: Static scene usage is immutable and shared by views
The renderer SHALL publish immutable feature usage with frozen scene data, including whether the scene has renderable opaque surfaces, transparent surfaces, shadow-casting surfaces, terrain surfaces, and GPU-driven candidates. All views of the same frozen scene revision SHALL observe the same static usage.

#### Scenario: Empty frozen scene
- **WHEN** frozen scene data contains no selectable visible render object with valid rendering inputs
- **THEN** static usage reports no renderable geometry, shadow caster, terrain surface, or GPU-driven candidate

#### Scenario: Multiple views of one frame
- **WHEN** Game and Scene views consume the same frozen scene revision
- **THEN** they use the same static feature-usage value without independently rescanning the object array

#### Scenario: Scene topology changes
- **WHEN** an applicable object is added, removed, hidden, or changes surface or render-queue classification
- **THEN** the next frozen scene revision publishes updated static usage

### Requirement: Dynamic light usage controls lighting work
The renderer SHALL derive effective per-frame light usage from active light counts, light intensity, light type, shadow policy, and shadow-caster availability.

#### Scenario: No local lights
- **WHEN** a frame has zero effective point and spot lights
- **THEN** no clustered-light build, spot-shadow, or point-shadow pass is declared

#### Scenario: Non-shadowing local lights
- **WHEN** local lights contribute lighting but none is eligible to cast shadows
- **THEN** clustered-light work MAY run
- **AND** no local-light-shadow pass is declared

#### Scenario: No effective directional light or caster
- **WHEN** the directional light contributes no energy or the scene has no shadow-casting surface
- **THEN** no directional shadow or variance-shadow filtering pass is declared

#### Scenario: Shadow-capable frame
- **WHEN** an effective shadow-casting light and at least one shadow-casting surface are present and shadow policy is enabled
- **THEN** only the required directional, spot, or point shadow work is declared

### Requirement: Geometry-dependent screen-space work is content driven
GTAO, SSR, and geometry-only Hi-Z generation SHALL require applicable visible screen-space geometry in addition to their renderer policy and pipeline prerequisites.

#### Scenario: Empty deferred frame
- **WHEN** a deferred frame has no applicable opaque geometry
- **THEN** GTAO and SSR are inactive
- **AND** SSR does not keep a Hi-Z chain alive

#### Scenario: GPU-driven frame with no candidates
- **WHEN** GPU-driven policy is enabled but static usage reports no GPU-driven candidate
- **THEN** no GPU-driven visibility pass or geometry-only Hi-Z dependency is declared

#### Scenario: Geometry becomes applicable
- **WHEN** applicable geometry exists and GTAO, SSR, or GPU-driven policy is enabled
- **THEN** the corresponding work is declared according to normal pipeline dependencies

### Requirement: Fullscreen and procedural effects retain valid non-object inputs
The renderer SHALL NOT disable a fullscreen or procedural effect solely because the ordinary render-object list is empty when sky, atmosphere, ocean, fluid, emissive fullscreen content, temporal history, or another valid input can produce a non-neutral result.

#### Scenario: Sky-only frame with bloom
- **WHEN** a sky or atmosphere can produce values above the bloom threshold and bloom policy is enabled
- **THEN** bloom remains eligible to execute

#### Scenario: Temporal resolve without ordinary geometry
- **WHEN** TAA policy is enabled and valid temporal inputs are produced by the active pipeline
- **THEN** TAA remains eligible even if ordinary geometry usage is empty

#### Scenario: Required presentation passes
- **WHEN** a frame is presented
- **THEN** required color production, tonemapping/output conversion, and output-state passes remain active

### Requirement: Terrain activation follows scene content and pending work
General renderer defaults SHALL disable interactive terrain and erosion. Terrain sampling SHALL require a frozen terrain surface and enabled policy, while terrain mutation SHALL additionally require pending initialization or edit work and the designated shared-simulation producer view.

#### Scenario: Non-terrain scene with terrain policy enabled
- **WHEN** a frozen scene contains no terrain surface even though terrain policy is enabled
- **THEN** terrain resources and passes remain inactive for that frame

#### Scenario: Stable terrain frame
- **WHEN** terrain sampling is active but initialization is complete and no brush, reset, or full-update request is pending
- **THEN** the graph contains no `InteractiveTerrain.BrushAndErosion` pass
- **AND** terrain geometry samples the previously published heightfield

#### Scenario: Terrain initialization or edit
- **WHEN** a terrain scene requires initialization or has pending brush, reset, or full-update work
- **THEN** the designated producer declares exactly one `InteractiveTerrain.BrushAndErosion` pass

#### Scenario: Secondary terrain view
- **WHEN** Game and Scene views render the same logical terrain frame
- **THEN** at most one view declares terrain mutation work
- **AND** both views sample the same shared terrain resources

### Requirement: Terrain resources initialize on first applicable activation
Terrain-specific GPU simulation resources SHALL initialize at the ordered activation boundary of the first terrain-capable frame rather than during startup of an unrelated scene. Once initialized, shared resources MAY remain resident for later terrain activations.

#### Scenario: Renderer starts outside Terrain Lab
- **WHEN** renderer initialization completes without terrain content
- **THEN** terrain simulation textures, pipelines, and per-frame descriptor sets have not been initialized

#### Scenario: First transition to Terrain Lab
- **WHEN** the renderer transitions from a non-terrain scene to Terrain Lab
- **THEN** shared terrain resources initialize before the first terrain graph build
- **AND** the first terrain frame produces a valid initialized heightfield

#### Scenario: Return to a non-terrain scene
- **WHEN** the renderer transitions away from Terrain Lab
- **THEN** subsequent graphs contain no terrain pass or terrain sampling dependency

### Requirement: Scene-driven activation is diagnosable across the catalog
Diagnostics and automated validation SHALL report resolved static usage, dynamic usage, policy, and final activation independently, and SHALL validate the empty workload plus every built-in demo scene.

#### Scenario: Empty-workload graph report
- **WHEN** a steady-state graph report is captured for the empty workload
- **THEN** it contains none of the content-dependent passes prohibited by the empty-frame requirement

#### Scenario: Built-in scene matrix
- **WHEN** graph reports are captured for all built-in demo scenes
- **THEN** each feature-specific pass appears only in scenes whose content and policy require it
- **AND** Terrain Lab remains the only built-in scene that can activate interactive terrain

#### Scenario: Activation reason inspection
- **WHEN** a feature is inactive
- **THEN** diagnostics identify whether capability, policy, static usage, dynamic usage, pending work, or producer ownership prevented activation
