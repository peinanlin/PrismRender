## Purpose

Make every visible WaveWorks Lab control authoritative, discoverable, and observably applied to the active spectral simulation.

## MODIFIED Requirements

### Requirement: Dedicated grouped WaveWorks Lab panel
The editor SHALL expose a discoverable WaveWorks panel with General settings, Wind waves, Local waves, Quadtree / geometry parameters, Statistics, and Debug groups. When the spectral scene is active, legacy flat FFT controls SHALL be hidden or read-only and SHALL NOT be presented as active spectral controls.

#### Scenario: Edit wind in the editor
- **WHEN** the WaveWorks Lab is active and the user edits the visible wind-speed control
- **THEN** the control updates `OceanSettings.baseWind.speed`, reports the effective unit conversion, and schedules the appropriate spectrum update

#### Scenario: Open legacy FFT Lab
- **WHEN** the legacy FFT Ocean Lab is active
- **THEN** its original flat controls remain editable and continue to update only the legacy implementation

### Requirement: Scoped setting application
The panel SHALL report active and pending settings versions and classify changes so constants, initial spectra, resources, geometry, local state, and history are updated only when owned parameters require them.

#### Scenario: Adjust foam generation
- **WHEN** the user changes foam generation amount
- **THEN** subsequent foam evolution uses the new value without reallocating FFT resources or regenerating H0

#### Scenario: Change quality
- **WHEN** the user selects a different quality
- **THEN** the panel reports a pending resource replacement until the new coherent resource version becomes active

### Requirement: WaveWorks Reference defaults
The reference preset SHALL use actual canonical parameter values from the sample and SHALL enable local waves with rain disabled and moving wake enabled. Direction displays SHALL be derived from stored normalized vectors so displayed degrees and active values cannot disagree.

#### Scenario: Reload reference preset
- **WHEN** the user selects Load WaveWorks Reference
- **THEN** wind, swell, foam, local emitter, geometry, sun, and UV controls return to their documented canonical defaults and immediately show matching effective values

