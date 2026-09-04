## Purpose

Offer a dedicated, discoverable WaveWorks Lab control surface that mirrors WaveWorks' parameter organization while clearly presenting PrismRender-specific backend and profiling behavior, without changing the existing FFT Ocean Lab.

## ADDED Requirements

### Requirement: Dedicated grouped WaveWorks Lab panel
The UI SHALL provide separate General, Wind Waves, Local Waves, Geometry, Statistics, and Debug groups for the WaveWorks Lab instead of placing its detailed controls in the general renderer debug section. The existing FFT Ocean Lab SHALL remain separately selectable with its original defaults and controls.

#### Scenario: Open WaveWorks Lab
- **WHEN** the WaveWorks Lab scene is active
- **THEN** the dedicated panel exposes the six groups and the general debug panel contains only the high-level ocean enable control

#### Scenario: Open legacy FFT Ocean Lab
- **WHEN** the existing FFT Ocean Lab scene is active
- **THEN** its legacy FFT defaults and visible result remain unchanged and WaveWorks-specific defaults are not applied

### Requirement: WaveWorks Reference defaults
The UI SHALL provide a WaveWorks Reference preset with the documented reference values, units, and feature toggles, while clearly identifying project-defined quality resolutions and implementation-specific values.

#### Scenario: Load reference preset
- **WHEN** the WaveWorks Reference preset is applied
- **THEN** the UI sets simulation period 1000 m, Compute mode, Extreme quality, time scale 1, choppiness 1, UV warp 0.03 and 2, base wind direction 0 degrees and Beaufort 4.7, base fetch 0.1 km and peaking 3.3, swell direction 90 degrees, speed 1.5 m/s, fetch 520 km and peaking 10, and the documented spectral and local foam defaults

#### Scenario: Load reference geometry defaults
- **WHEN** the WaveWorks Reference preset is applied
- **THEN** geometry controls use 64 cells per patch, 5 m minimum patch size, 10 pixel maximum edge length, 15 maximum LODs, geomorphing 1, and diamond pattern disabled

### Requirement: Unit-aware validated controls
Every physical control SHALL show its unit, valid range, and validation state, and direction controls SHALL offer degree input while storing a normalized vector or equivalent canonical representation.

#### Scenario: Enter a direction of 360 degrees
- **WHEN** a user commits a wind direction of 360 degrees
- **THEN** the control canonicalizes it to the same direction as 0 degrees without creating a zero-length vector

### Requirement: Scoped setting application
The UI SHALL distinguish immediate constants, spectrum rebuilds, resource rebuilds, geometry rebuilds, and simulation resets, and SHALL communicate pending expensive changes before or while they are applied at a safe frame boundary.

#### Scenario: Adjust sun intensity
- **WHEN** only sun intensity changes
- **THEN** the next frame updates shading constants without rebuilding ocean simulation resources

#### Scenario: Change FFT quality
- **WHEN** quality changes
- **THEN** the UI indicates a resource rebuild until the replacement simulation is active

### Requirement: Simulation and rendering controls
The panel SHALL independently control simulation pause, rendering visibility, wireframe, atmosphere use, microfacet Fresnel/specular/reflection, cascade display, local-wave emitters, and supported readback modes.

#### Scenario: Pause simulation while rendering
- **WHEN** simulation is paused and rendering remains enabled
- **THEN** the most recent completed ocean state continues to render without advancing simulation time

### Requirement: Reset and preset recovery
The panel SHALL allow resetting all ocean settings, each major settings group, and simulation history to known defaults without requiring an application restart.

#### Scenario: Reset foam settings
- **WHEN** only the spectral foam group is reset
- **THEN** its reference values and history are restored without modifying wind, swell, local-wave, geometry, or atmosphere settings

### Requirement: Live diagnostics
The panel SHALL display the active backend path, tessellation or fallback geometry path, quality, timing availability, warnings, ocean GPU/CPU statistics, memory, dispatches, geometry counts, and query latency.

#### Scenario: Tessellation fallback active
- **WHEN** the backend lacks tessellation capability
- **THEN** the panel labels the fallback geometry path and does not expose unsupported tessellation-only controls as active
