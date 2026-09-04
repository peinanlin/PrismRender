## Purpose

Correct the four-cascade spectrum so reference wind and swell energy are represented and live parameter changes publish coherent output.

## MODIFIED Requirements

### Requirement: Four-scale wind and swell spectrum
The simulation SHALL combine independently configurable base-wind and swell JONSWAP spectra and distribute their energy across exactly four world-space cascades using resolution-aware, normalized wavelength windows that continuously cover the FFT-resolvable range.

#### Scenario: Reference base-wind peak is represented
- **WHEN** Extreme quality uses the reference 4.7 Beaufort, 0.1 km fetch base wind
- **THEN** the approximately 0.54 meter peak receives non-zero normalized band ownership and contributes finite energy to at least one cascade

#### Scenario: Reference swell peak is represented independently
- **WHEN** the reference 1.5 m/s, 520 km swell is enabled
- **THEN** its approximately 48 meter peak receives non-zero band ownership without removing or replacing base-wind energy

#### Scenario: Sum adjacent band windows
- **WHEN** a wavelength lies in an overlap between adjacent supported cascades
- **THEN** all cascade weights for that wavelength sum to one within numerical tolerance

### Requirement: Cached initial spectrum and scoped updates
The simulation SHALL retain its active coherent result while spectrum-affecting settings rebuild an inactive initial spectrum, and SHALL not regenerate initial spectrum for time-scale, choppiness, foam, geometry, or shading-only changes.

#### Scenario: Drag wind-speed control
- **WHEN** the user changes base-wind speed over consecutive UI frames
- **THEN** each committed settings version produces at most one initial-spectrum rebuild and rendering never samples partially rebuilt cascade slices

#### Scenario: Change time scale
- **WHEN** only time scale changes
- **THEN** phase evolution uses the new value without scheduling initial-spectrum regeneration

#### Scenario: Change choppiness
- **WHEN** only lateral displacement multiplier changes
- **THEN** evolved horizontal displacement changes without rebuilding H0

