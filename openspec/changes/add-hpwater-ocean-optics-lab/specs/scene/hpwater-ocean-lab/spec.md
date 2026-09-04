## Purpose

Defines a dedicated reproducible renderer Lab for comparing HPWater-style optics against the existing spectral ocean while holding simulation, geometry, camera, and lighting inputs constant.

## ADDED Requirements

### Requirement: CLI-selectable HPWater Ocean Lab
The application SHALL expose a dedicated `hpwater-ocean` demo scene whose presentation name and description identify spectral FFT waves with HPWater-style optics.

#### Scenario: Launch from the command line
- **WHEN** the application is started with `--scene=hpwater-ocean`
- **THEN** the HPWater Ocean Lab loads with spectral simulation and HPWater-style optics enabled

### Requirement: Shared large-area spectral scene
The HPWater Ocean Lab SHALL reuse the WaveWorks-style Lab's kilometer-scale spectral source, adaptive geometry, local-wave domain, atmosphere, sun, reference camera, and deterministic initial conditions unless an HPWater-specific optical preset explicitly overrides only optical settings.

#### Scenario: Compare geometry between Labs
- **WHEN** the WaveWorks and HPWater Labs run at the same simulation time and camera
- **THEN** their sampled displacement, adaptive patch selection, foam simulation state, and local disturbance batches are equivalent

#### Scenario: Navigate the large ocean
- **WHEN** the camera moves across the HPWater Lab or looks toward the horizon
- **THEN** adaptive spectral water coverage follows the camera and remains continuous throughout the configured view distance

### Requirement: Isolated scene state
The Lab SHALL initialize and reset its optical settings and temporal resources without leaking them into the legacy FFT Lab, the WaveWorks Lab, or later scene instances.

#### Scenario: Switch away from HPWater Lab
- **WHEN** the user changes from `hpwater-ocean` to an existing ocean Lab
- **THEN** HPWater optical passes and histories are released or disabled and the destination Lab loads its own unchanged defaults

### Requirement: Reproducible validation presentation
The Lab SHALL provide deterministic named camera positions and capture modes for above-water near, horizon, refraction, foam, caustics, underwater, and waterline validation at reported output resolutions and quality settings.

#### Scenario: Capture a reference view
- **WHEN** a named HPWater validation camera and fixed simulation time are selected
- **THEN** the capture metadata identifies the camera, resolution, wave settings, optics quality, enabled passes, and stage timings needed to reproduce the image

