## 1. Migration Manifest Model

- [ ] 1.1 Add `OceanMigrationStatus` under `src/Renderer/Features/Ocean/` with manifest version, upstream change name, stage/category/visual-impact enums, mapped task IDs, validation summaries, visible and unavailable capability text, and reference checklists for all eleven designed stages; update CMake and verify a unit test observes stable ordered IDs 1 through 11 while `main.cpp` remains unchanged
- [ ] 1.2 Initialize the manifest at the current 12/80 checkpoint with stages 1 and 2 complete and stage 3 active; verify the status reports the existing single visible JONSWAP compatibility surface and four-slice resources without claiming four active cascades, persistent foam, local waves, dedicated optics, or quadtree geometry
- [ ] 1.3 Keep the manifest read-only with respect to renderer configuration and feature activation; verify changing manifest test data cannot select simulation, geometry, quality, or rendering paths

## 2. OpenSpec Progress Consistency

- [ ] 2.1 Add an `OceanMigrationStatusTests` target that parses `openspec/changes/add-waveworks-like-ocean/tasks.md` only in the developer test environment and verifies all 80 numbered tasks are mapped exactly once across stages 1 through 11
- [ ] 2.2 Compare OpenSpec checkboxes with manifest completed-task and per-stage state; verify the current 12 completed tasks pass and fixtures containing an unreported completion, duplicate task, missing task, invalid active stage, or prematurely completed stage fail with the responsible task and stage identifier
- [ ] 2.3 Register the manifest consistency test in CMake/CTest and document that every ocean apply batch changing a checkbox must update status metadata in the same batch; verify the normal project test command detects a deliberately stale fixture

## 3. WaveWorks Lab Status UI

- [ ] 3.1 Add a compact status banner before all WaveWorks Lab parameter groups showing manifest version, 12/80-style progress, last completed stage, active stage, simulation path, geometry path, `Visible now`, `Not yet`, next engineering checkpoint, and next major visual milestone; verify these fields are visible without expanding another group
- [ ] 3.2 Add an expandable `Migration Roadmap` that renders all eleven stages with textual Completed/Active/Planned/Blocked state, category, visual-impact label, mapped task progress, expected visible result, validation gates, and WaveWorks comparison guidance; verify an ImGui checklist or snapshot contains every stage in stable order
- [ ] 3.3 Label requested controls whose implementation stage is incomplete as planned or inactive using manifest metadata; verify current Local Waves, quadtree/tessellation, slope-moment, and future debug controls do not imply that their runtime behavior is already active
- [ ] 3.4 Remove the manually embedded one-line checkpoint in favor of manifest output and verify switching between WaveWorks Lab and FFT Ocean Lab does not leak status UI, settings, defaults, or implementation selection

## 4. Capture and Diagnostic Attribution

- [ ] 4.1 Extend WaveWorks Lab capture and diagnostic JSON with manifest version, active and last-completed stage IDs/titles, completed and total tasks, backend, simulation path, geometry path, and visible capability list; verify schema tests cover every field and non-ocean captures remain backward compatible
- [ ] 4.2 Produce deterministic D3D12 and Vulkan stage-2 WaveWorks Lab captures and reports whose metadata identifies stage 3 as active and records the current compatibility rendering path; verify both reports match the runtime status banner
- [ ] 4.3 Add a comparison-record format for the reference executable at `D:\unity_project\WaveWorks_example-2.0.0-win64` containing camera, GPU, backend, resolution, timing, quadtree statistics, compared stage, and qualitative checklist results; verify infrastructure-only stages can explicitly record `not required` without a false parity failure

## 5. Roadmap Documentation and Acceptance

- [ ] 5.1 Add an Ocean migration stage document containing the eleven-stage matrix, task mapping, current checkpoint, visible versus unavailable behavior, next visual milestone, and update procedure; verify it agrees with the compiled manifest and links the original OpenSpec change rather than duplicating its detailed task descriptions
- [ ] 5.2 Document the mandatory WaveWorks comparison points at stages 4, 5, 6, 7, 10, and 11, including near/mid/horizon waves, foam persistence, low-sun optics, local disturbance, quadtree geometry, performance, and cross-backend acceptance; verify stages 2, 3, 8, and 9 are correctly identified as primarily numerical or infrastructure checkpoints
- [ ] 5.3 Run manifest unit tests, UI/scene isolation tests, automation schema tests, D3D12/Vulkan stage captures, and strict OpenSpec validation; verify all checks pass and the final handoff states the precise active implementation and the next visual unlock
