## Purpose

Defines discoverable controls and diagnostics for tuning, validating, and profiling the HPWater-style ocean without exposing inactive settings or hiding quality fallbacks.

## ADDED Requirements

### Requirement: HPWater optical control groups
The HPWater Ocean Lab SHALL expose grouped controls for material coefficients, reflection/refraction, caustics, underwater volumetrics, distance quality, resets, statistics, and debug views while continuing to expose the shared spectral and local-wave controls from the WaveWorks-style Lab.

#### Scenario: Edit an optical coefficient
- **WHEN** the user changes absorption, scattering, roughness, phase, or refraction settings
- **THEN** the effective validated value is applied to the next eligible frame without rebuilding the FFT initial spectrum

#### Scenario: Edit a spectral setting
- **WHEN** the user changes wind, swell, quality, time, or local-wave settings in the HPWater Lab
- **THEN** the shared ocean simulation follows the same dirty-scope and coherent-publication rules as the WaveWorks Lab

### Requirement: Quality and capability feedback
The panel SHALL show the active optics quality tier, effective resolution and sample counts, distance ranges, history validity, camera medium state, and any capability-driven fallback rather than presenting an unavailable mode as active.

#### Scenario: Optional mode is unavailable
- **WHEN** the selected backend or device cannot execute an optional HPWater feature
- **THEN** the panel displays the effective fallback and the renderer continues with a finite supported result

### Requirement: Debug and profiling visibility
The panel SHALL provide non-mutating views for water mask/depth, normal/roughness, absorption/scattering, foam, thickness/refraction hit, caustic energy/cascades, volumetric accumulation/history rejection, and distance tier, together with CPU/GPU timings and memory for each optical stage.

#### Scenario: Enable a debug view
- **WHEN** the user selects one HPWater debug visualization
- **THEN** the final output displays that stage without advancing, resetting, or otherwise mutating the ocean or optical history

#### Scenario: Inspect performance
- **WHEN** a complete HPWater frame is profiled
- **THEN** the statistics identify output resolution, active views, wave quality, optics quality, water-pixel coverage, per-stage GPU time, total GPU/CPU time, memory, and dispatch/draw counts

### Requirement: Safe reset controls
The panel SHALL expose distinct optical-history and full-water resets and SHALL clamp or normalize non-finite and physically invalid inputs before they reach shaders.

#### Scenario: Reset optical history
- **WHEN** the user requests an optical-history reset
- **THEN** refraction, caustic, and volumetric histories clear without rebuilding the FFT spectrum or deleting current local-wave state

