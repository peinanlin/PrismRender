## Why

The WaveWorks migration is tracked as 80 implementation tasks, but the running
Lab currently exposes only a manually written checkpoint sentence. After each
incremental build, a user cannot reliably tell which architecture stage is
complete, which capabilities are actually visible, or which later milestone
will produce the next meaningful visual change.

## What Changes

- Define a versioned eleven-stage ocean migration manifest that maps the
  existing `add-waveworks-like-ocean` work into stable, user-facing milestones.
- Distinguish infrastructure checkpoints from visual checkpoints so a
  successful run does not imply that four cascades, persistent foam, local
  waves, dedicated shading, or adaptive geometry are already active.
- Show the current stage, completed stages, active implementation path,
  currently visible capabilities, known limitations, next engineering stage,
  and next visual milestone at the top of the WaveWorks Lab panel.
- Add an expandable roadmap view with per-stage entry criteria, expected
  visible result, validation gates, and WaveWorks comparison checkpoints.
- Record the stage identifier and manifest version in automated capture and
  diagnostic output so screenshots and performance reports remain attributable
  to a concrete implementation checkpoint.
- Add a checked update procedure so completing a migration stage also updates
  the manifest, UI status, documentation, and representative capture metadata.
- Preserve the original FFT Ocean Lab without showing WaveWorks migration
  controls or changing its rendering defaults.

## Capabilities

### New Capabilities

- `ui/ocean-migration-status`: Versioned ocean migration stages, truthful
  runtime status, visible-versus-planned capability reporting, roadmap display,
  capture attribution, and stage-update validation.

### Modified Capabilities

None. There are currently no synchronized main OpenSpec capability files; this
change introduces a separate status capability without rewriting the ongoing
ocean implementation delta.

## Impact

- UI: `OceanLabPanel` gains a persistent implementation-status header and an
  expandable migration roadmap for the WaveWorks Lab only.
- Renderer/Ocean: a small read-only migration manifest model exposes stable
  stage metadata without owning simulation or rendering behavior.
- Automation: capture/diagnostic metadata includes the ocean stage identifier
  and manifest version when the WaveWorks Lab is active.
- Tests: manifest coverage, ordering, status consistency, legacy-Lab isolation,
  UI-content, and capture-metadata tests.
- Documentation: a human-readable stage matrix defines what should and should
  not be visible after every milestone and when to run the reference executable
  under `D:\unity_project\WaveWorks_example-2.0.0-win64`.
- Dependencies: no NVIDIA runtime dependency and no runtime parsing of the
  developer-only OpenSpec task Markdown.
