## Purpose

Make every incremental WaveWorks Lab build self-identifying so users can see
which migration stage is active, which behavior is genuinely available, and
which engineering and visual milestones remain.

## ADDED Requirements

### Requirement: Stable ordered migration stages
The system SHALL expose exactly eleven ordered ocean migration stages covering
all tasks in `add-waveworks-like-ocean`, with a stable identifier, title,
mapped task set, stage category, expected visible result, limitations,
validation gates, and WaveWorks comparison guidance for every stage.

#### Scenario: Inspect the complete roadmap
- **WHEN** the WaveWorks Lab migration roadmap is opened
- **THEN** stages 1 through 11 are displayed in order and every original migration task belongs to exactly one stage

### Requirement: Truthful current checkpoint
The WaveWorks Lab SHALL report the last completed stage, active stage, completed
task count, total task count, current implementation path, currently visible
capabilities, and explicitly unavailable future capabilities without inferring
completion from requested settings alone.

#### Scenario: Run after portable FFT resource work
- **WHEN** stages 1 and 2 are complete and stage 3 is active
- **THEN** the Lab reports four-slice resources as available while stating that four simulated cascades, persistent foam, local waves, dedicated shading, and adaptive quadtree geometry are not yet visible

#### Scenario: Select a future control
- **WHEN** a setting exists for behavior whose owning stage is not complete
- **THEN** the status identifies the setting as planned or inactive rather than claiming that the behavior is rendered

### Requirement: Persistent Lab status summary
The WaveWorks Lab panel SHALL show a compact stage summary before its detailed
parameter groups and SHALL provide an expandable roadmap containing the
expected visible outcome, validation result, next engineering stage, and next
major visual milestone.

#### Scenario: Open the WaveWorks Lab panel
- **WHEN** the WaveWorks Lab is active and its panel is visible
- **THEN** the user can identify the current checkpoint and next visible milestone without expanding General, Wind Waves, Local Waves, Geometry, Statistics, or Debug

### Requirement: Stage categories and visual-impact distinction
Every stage SHALL be categorized as infrastructure, simulation, appearance,
interaction, geometry, or acceptance and SHALL declare whether its completion
is expected to have no major visual change, a partial visual change, a major
visual change, or final acceptance impact.

#### Scenario: Complete an infrastructure-only stage
- **WHEN** a stage changes scheduling or backend correctness without changing the final water surface materially
- **THEN** the UI labels the result as an infrastructure checkpoint and points to the later stage that unlocks the next major visual change

### Requirement: Checked synchronization with implementation progress
The repository SHALL validate that the stage manifest covers all current ocean
migration tasks once, that its completed-task state agrees with the OpenSpec
checkboxes, and that completed stages satisfy their declared validation gates.

#### Scenario: Complete a task without updating status
- **WHEN** an ocean migration task checkbox changes but the user-facing manifest is not updated
- **THEN** the automated stage-consistency test fails with the mismatched task and stage identifier

#### Scenario: Duplicate a task mapping
- **WHEN** the same original migration task is assigned to two stages
- **THEN** manifest validation fails instead of reporting ambiguous progress

### Requirement: Capture and diagnostic attribution
Automated WaveWorks Lab captures and diagnostic reports SHALL include the
manifest version, active stage identifier, last completed stage, completed task
count, total task count, backend, and active simulation and geometry paths.

#### Scenario: Capture an intermediate build
- **WHEN** an automated WaveWorks Lab capture completes
- **THEN** its report identifies the exact migration checkpoint needed to interpret the image and performance result

### Requirement: Reference comparison checkpoints
Stages that introduce visible four-cascade waves, persistent foam, dedicated
optics, local interaction, adaptive geometry, or final acceptance SHALL define
a corresponding comparison checklist against the configured WaveWorks sample,
while infrastructure-only stages SHALL not require subjective visual parity.

#### Scenario: Complete dedicated ocean shading
- **WHEN** the dedicated surface-shading stage reaches its validation gate
- **THEN** the checklist requests comparison of low-sun highlights, sky reflection, deep-water color, crest scattering, and foam appearance against the WaveWorks reference scene

### Requirement: Legacy FFT Lab isolation
The FFT Ocean Lab SHALL retain its original controls, defaults, rendering path,
and presentation and SHALL not display WaveWorks migration roadmap controls.

#### Scenario: Switch to the FFT Ocean Lab
- **WHEN** the user selects the legacy FFT Ocean Lab
- **THEN** no WaveWorks stage status changes its settings or implies that the legacy path implements future spectral capabilities

