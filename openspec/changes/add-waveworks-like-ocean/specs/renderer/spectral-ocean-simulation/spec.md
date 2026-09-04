## Purpose

Provide a deterministic, multi-scale deep-ocean simulation that combines local wind waves and swell while remaining portable across PrismRender's Slang and RHI backends.

## ADDED Requirements

### Requirement: Native self-contained ocean simulation
The renderer SHALL implement spectral initialization, time evolution, inverse FFT, derived-map generation, and foam evolution without requiring NVIDIA WaveWorks binaries or another ocean runtime library.

#### Scenario: Launch without WaveWorks installed
- **WHEN** the Ocean Lab is launched on a supported build with no WaveWorks DLLs or libraries present
- **THEN** the complete spectral ocean simulation initializes and runs using PrismRender-owned resources and shaders

### Requirement: Four-scale wind and swell spectrum
The simulation SHALL combine independently configurable base-wind and swell JONSWAP spectra and distribute their energy across exactly four world-space cascades with smooth, non-overlapping band transitions.

#### Scenario: Default reference spectrum
- **WHEN** the WaveWorks Reference preset is selected
- **THEN** four cascades are generated from the configured 1000 m period, base wind, and swell parameters without a visible energy discontinuity between adjacent bands

#### Scenario: Swell disabled
- **WHEN** the swell amplitude is set to zero
- **THEN** the output contains the base-wind contribution only and remains valid in every cascade

### Requirement: Deterministic absolute-time evolution
The simulation SHALL produce repeatable displacement results for the same validated settings, random seed, backend, and absolute simulation time, and SHALL not derive spectral phase from accumulated variable frame deltas.

#### Scenario: Replay a captured time
- **WHEN** the simulation is reset and evaluated twice at the same absolute time using identical settings and seed on the same backend
- **THEN** the generated base displacement values match within the documented floating-point tolerance

#### Scenario: Variable frame pacing
- **WHEN** two runs reach the same absolute time through different frame-delta sequences
- **THEN** their spectral displacement results match within the documented floating-point tolerance

### Requirement: Cached initial spectrum and scoped updates
The simulation SHALL retain its initial random spectrum until a spectrum-affecting setting or quality setting changes, while time-only updates SHALL reuse the cached spectrum.

#### Scenario: Advance simulation time
- **WHEN** only simulation time changes between frames
- **THEN** no initial-spectrum regeneration workload is scheduled

#### Scenario: Change wind direction
- **WHEN** a base-wind direction change is committed
- **THEN** the affected initial spectrum is regenerated before subsequent evolved output is rendered

### Requirement: Portable inverse FFT
The simulation SHALL execute a two-dimensional inverse FFT for every enabled cascade through RHI compute commands using Slang-compiled DXIL or SPIR-V, with explicit resource dependencies visible to RenderGraph.

#### Scenario: D3D12 execution
- **WHEN** the Ocean Lab runs on the D3D12 backend
- **THEN** all enabled cascades complete spectrum evolution and inverse FFT without RHI validation errors

#### Scenario: Vulkan execution
- **WHEN** the Ocean Lab runs on a Vulkan device satisfying the feature requirements
- **THEN** all enabled cascades complete spectrum evolution and inverse FFT without Vulkan validation errors

### Requirement: Derived ocean maps
The simulation SHALL publish mipmapped displacement, gradient/folding, slope-moment, and foam resources for all four cascades after each completed simulation update.

#### Scenario: Consume a completed simulation frame
- **WHEN** the surface renderer requests the latest completed spectral result
- **THEN** the displacement, gradient, moment, and foam data refer to one coherent simulation time and expose all required mip levels

### Requirement: Persistent foam evolution
Foam SHALL be generated from surface folding and configured thresholds, retained across frames, and updated using generation amount, dissipation, and temporal falloff controls.

#### Scenario: Wave crest stops folding
- **WHEN** a previously folding region falls below the generation threshold
- **THEN** existing foam decays over subsequent simulation updates instead of disappearing in one frame

#### Scenario: Foam reset
- **WHEN** the simulation is reset or foam history becomes incompatible after a resource rebuild
- **THEN** foam history is cleared deterministically before new foam is accumulated

### Requirement: Runtime quality presets
The simulation SHALL provide Normal, High, and Extreme quality presets that define project-owned FFT resolutions and recreate incompatible resources safely at a frame boundary.

#### Scenario: Change quality while running
- **WHEN** the user changes from Normal to Extreme quality
- **THEN** old resources remain valid until in-flight work is complete, replacement resources are initialized, and rendering resumes without accessing retired resources

### Requirement: Validated physical parameters
The simulation SHALL express directions, speeds, fetch distances, periods, amplitudes, cutoffs, and time scale in documented units and SHALL clamp or reject non-finite and physically invalid inputs before GPU submission.

#### Scenario: Invalid period entered
- **WHEN** a non-positive or non-finite simulation period is supplied
- **THEN** the simulation uses the last valid value or a documented safe minimum and reports the validation adjustment to the UI

