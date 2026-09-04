## Purpose

Make the reference moving wake a correctly delivered, stable GPU local-wave and foam producer.

## MODIFIED Requirements

### Requirement: Reset and scripted emitters
The WaveWorks Reference preset SHALL enable the deterministic moving-wake demonstration by default, disable rain by default, and publish the final generated disturbance batch to both CPU validation and GPU simulation in the same frame.

#### Scenario: Load reference preset
- **WHEN** the WaveWorks Reference preset is loaded
- **THEN** local waves and demo emitters are enabled, rain is disabled, and moving wake is enabled

#### Scenario: Generate a wake disturbance
- **WHEN** the wake emitter advances for one frame
- **THEN** the generated disturbance is present in the GPU upload batch before pending disturbances are cleared

### Requirement: Stable time integration
The GPU local-wave solver SHALL use world-space cell size in its spatial derivatives, bounded stable time steps, finite-value guards, and an absorbing or faded boundary rather than wrapping wave energy to the opposite edge.

#### Scenario: Wake reaches domain edge
- **WHEN** wake energy enters the configured edge absorption region
- **THEN** it decays without reappearing on the opposite side of the domain

### Requirement: Local derived maps and composition
The GPU local-wave simulation SHALL publish vertical and lateral displacement, finite normalized gradients, folding candidates, and persistent foam controlled by local whitecap threshold, generation threshold, generation amount, dissipation, and temporal falloff.

#### Scenario: Disable local foam only
- **WHEN** local foam generation amount is zero
- **THEN** the wake displacement remains visible while no new local foam is accumulated

#### Scenario: Stop wake emitter
- **WHEN** the moving wake is disabled after producing foam
- **THEN** existing foam persists and then decays monotonically according to local dissipation and falloff

