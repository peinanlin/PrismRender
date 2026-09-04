## Purpose

Make WaveWorks comparison and performance acceptance resolution-aware and attributable to simulation versus surface work.

## MODIFIED Requirements

### Requirement: Observable workload cost
Ocean diagnostics SHALL report resolution, active rendered views, quality, cascade resolution, local grid, per-stage simulation GPU time, ocean surface GPU time, total renderer GPU time, CPU time, dispatches, and memory for every benchmark capture.

#### Scenario: Compare WaveWorks reference timing
- **WHEN** a PrismRender capture is compared with the 1280x800 WaveWorks sample
- **THEN** the report includes a PrismRender 1280x800 measurement at the stated quality and separately labels any 2560x1417 editor measurement

#### Scenario: Hidden scene view
- **WHEN** the editor Scene viewport is not visible
- **THEN** the benchmark reports one rendered view and does not include hidden-view ocean surface work

### Requirement: Quality performance budgets
Normal, High, and Extreme SHALL have explicit simulation, surface, total GPU, and memory budgets at the selected reference resolutions, and the UI SHALL warn when a measured available timer exceeds a budget.

#### Scenario: Ocean pixel refactor regression
- **WHEN** the independent ocean pixel path is benchmarked against the pre-change path at equal settings and resolution
- **THEN** its ocean surface GPU time does not regress and redundant generic mesh material samples are absent

