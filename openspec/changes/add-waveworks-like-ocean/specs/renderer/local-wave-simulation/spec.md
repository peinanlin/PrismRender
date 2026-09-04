## Purpose

Provide a bounded interactive wave layer for rain, scripted disturbances, and moving-object wakes that composes predictably with the spectral deep-ocean surface.

## ADDED Requirements

### Requirement: Configurable bounded local domain
The local-wave simulation SHALL expose a world-space center, domain size, and selectable power-of-two grid resolution, including 128, 256, 512, 1024, and 2048 where device limits permit.

#### Scenario: Default local-wave domain
- **WHEN** the WaveWorks Reference preset is loaded
- **THEN** the local simulation uses a 200 m domain centered at the configured origin with a 512 by 512 grid

#### Scenario: Unsupported grid size
- **WHEN** a requested grid size exceeds the active device's resource or dispatch limits
- **THEN** the largest supported preset is selected and the limitation is shown in the Ocean Lab UI

### Requirement: Stable time integration
The local-wave solver SHALL use bounded time steps and configurable damping so that frame-rate variation or a long frame cannot make its state non-finite or numerically unstable.

#### Scenario: Long frame occurs
- **WHEN** the application receives a frame delta larger than the configured stable integration step
- **THEN** the solver advances through bounded substeps or clamps accumulated time and preserves finite output

### Requirement: Disturbance injection
The simulation SHALL accept batched disturbances with world-space position, radius, strength, and optional velocity direction, and SHALL apply only the portion intersecting the active domain.

#### Scenario: Inject a disturbance inside the domain
- **WHEN** a positive disturbance is added at a valid point inside the local domain
- **THEN** an outward-propagating local wave becomes visible and contributes to local foam when its folding threshold is exceeded

#### Scenario: Inject outside the domain
- **WHEN** a disturbance does not intersect the local domain
- **THEN** the existing local-wave state remains unchanged

### Requirement: Reset and scripted emitters
The Ocean Lab SHALL be able to clear the local-wave state and drive repeatable rain and moving-wake disturbance emitters for demonstration and validation.

#### Scenario: Reset local waves
- **WHEN** the reset command is issued
- **THEN** local displacement, velocity, gradients, and foam history return to their neutral state

#### Scenario: Enable deterministic rain
- **WHEN** rain is enabled with a fixed seed and rate
- **THEN** the same sequence of raindrop disturbances is produced for the same elapsed simulation interval

### Requirement: Local derived maps and composition
The local-wave simulation SHALL publish displacement, gradient/folding, and persistent-foam data that the ocean surface blends to zero at the local-domain boundary.

#### Scenario: Observe the domain boundary
- **WHEN** a wave reaches the edge of the local domain
- **THEN** its rendered contribution fades or satisfies the configured boundary behavior without producing a hard rectangular seam in the spectral ocean

### Requirement: Independent local-wave controls
Local-wave amplitude, choppiness, whitecap threshold, foam generation, dissipation, and falloff SHALL be independently configurable from the spectral-ocean values.

#### Scenario: Disable local foam only
- **WHEN** local foam generation amount is set to zero
- **THEN** local displacement continues to render while no new local foam is generated

