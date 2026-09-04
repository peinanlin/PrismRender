## Context

See `proposal.md` for motivation and
`specs/ui/ocean-migration-status/spec.md` for observable behavior. The ongoing
ocean change contains 80 tasks and already exposes a separate WaveWorks Lab,
but `OceanLabPanel` currently embeds a static checkpoint sentence. Requested
settings can therefore appear before their implementation is active, and
screenshots cannot be attributed to a precise migration checkpoint.

The runtime must not depend on OpenSpec files being shipped. Developer tests,
however, can read the repo-local task document to detect drift. The stage model
must also remain independent of renderer ownership: it describes implementation
state but does not enable simulation or rendering features.

## Goals / Non-Goals

**Goals:**

- Make the running WaveWorks Lab accurately describe its current implementation.
- Give every original task one stable stage owner and one visible acceptance story.
- Make stage-only, visual, numerical, and performance checkpoints distinct.
- Fail developer validation when progress and the user-facing status diverge.
- Attribute captures and reports to a manifest version and implementation stage.

**Non-Goals:**

- Automatically enabling a feature because a stage says it is complete.
- Parsing Markdown or invoking OpenSpec in a distributed renderer build.
- Replacing the 80 implementation tasks or changing their technical order.
- Treating WaveWorks as a pixel-identical golden image or runtime dependency.
- Showing migration controls in the preserved FFT Ocean Lab.

## Decisions

### 1. Use eleven user-facing stages over the existing task plan

The manifest groups the existing task identifiers into the following stable
checkpoints. The original task list remains the implementation source of truth.

| Stage | Task range | Category | Expected result after completion | WaveWorks comparison |
|---|---:|---|---|---|
| 1. Baseline and Lab isolation | 1.1-1.7 | Infrastructure | Original FFT Lab and separate WaveWorks Lab run independently with reference defaults; the new Lab is still a compatibility rendering path. | Record baseline only. |
| 2. Portable FFT and four-slice resources | 2.1-2.3 | Infrastructure | Runtime 128/256/512 FFT and four-layer RGBA32F working arrays pass both backends; the visible surface can still be one map. | Numerical/backend validation; no parity claim. |
| 3. Observable deterministic spectrum pipeline | 2.4-2.6, 3.1-3.3 | Simulation | Named RenderGraph stages, cached deterministic initial spectrum, GPU/CPU fixtures, JONSWAP wind+swell, and normalized band windows work; final appearance may change little. | Inspect debug spectra, not final beauty. |
| 4. Visible four-cascade base ocean | 3.4-3.6, 4.1-4.3 | Simulation | Four cascades evolve and publish displacement, gradient/folding, and moment arrays with debug views and safe quality switching. This is the first major multi-scale visual change. | Compare near/mid/horizon frequency coverage and repetition. |
| 5. Persistent spectral foam and filtered publication | 4.4-4.7 | Appearance | Foam persists and decays through history; maps have data-aware mips and stage timing. Distant water and whitecaps become temporally stable. | Compare whitecap lifetime, breakup, and distant stability. |
| 6. Dedicated ocean optics on clipmap | 5.1-5.8 | Appearance | Dedicated Slang surface pipeline adds cascade blending, slope-moment BRDF, atmosphere, low-sun highlight, scattering, detailed foam, and stable clipmap coverage. | Compare the full reference beauty view except local interaction and adaptive geometry. |
| 7. Local interactive waves | 6.1-6.7 | Interaction | Bounded local height/velocity waves, disturbances, rain/wake demos, local gradients, and local persistent foam blend into the spectral ocean. | Compare the near-camera disturbed wave and broad local white foam. |
| 8. Complete controls, queries, and observability | 7.1-8.6 | Infrastructure | Dedicated grouped UI, dirty scopes, resets, live statistics, conservative bounds, and asynchronous displacement queries are operational. | Compare control grouping and reported statistics, not water parity. |
| 9. Tessellation-capable RHI | 9.1-9.6 | Infrastructure | Slang/RHI/D3D12/Vulkan hull/domain and patch pipelines validate with explicit fallback capability; clipmap remains the visible fallback. | Minimal tessellated fixture only. |
| 10. Adaptive quadtree ocean geometry | 10.1-10.8 | Geometry | Crack-free quadtree selection, balancing, geomorphing, instancing, tessellation/fallback selection, and node statistics replace or complement the clipmap. | Compare horizon density, camera travel, nodes rendered, cracks, and geometry CPU time. |
| 11. Coherent async execution and final acceptance | 11.1-12.8 | Acceptance | Versioned coherent outputs, optional async compute, stress validation, golden captures, budgets, documentation, and D3D12/Vulkan acceptance allow the spectral Lab to become the approved default. | Run the complete visual and performance matrix. |

This grouping covers the original tasks contiguously: stage 1 owns task IDs
1-7, stage 2 owns 8-10, stage 3 owns 11-16, stage 4 owns 17-22, stage 5 owns
23-26, stage 6 owns 27-34, stage 7 owns 35-41, stage 8 owns 42-53, stage 9
owns 54-59, stage 10 owns 60-67, and stage 11 owns 68-80.

Alternative: expose all 80 tasks in ImGui. Rejected because it makes the Lab a
developer issue tracker and still does not explain visible impact. Alternative:
reuse the twelve task-section headings. Rejected because spectrum prerequisites
were completed out of numerical order and several headings do not correspond to
a runnable visual checkpoint.

### 2. Keep one compiled, read-only migration manifest

Add an ocean-owned status model containing manifest version, upstream change
name, completed task count, total task count, last completed stage, active
stage, and an ordered array of stage records. Each record contains stable ID,
title, task IDs, category, visual-impact level, available-result summary,
not-yet-available summary, validation gates, and reference checklist.

The renderer and UI read this model but cannot use it to activate features.
Actual settings, capabilities, resources, and pipeline selection remain the
authority for runtime behavior. This prevents an optimistic status update from
silently enabling unfinished code.

Alternative: load a JSON manifest at runtime. Rejected for the first version
because missing or malformed deployment assets would make status unreliable.
Alternative: parse `tasks.md` at runtime. Rejected because OpenSpec artifacts
are developer inputs and are not guaranteed in packaged builds.

### 3. Validate the compiled manifest against OpenSpec during tests

A developer-only test reads
`openspec/changes/add-waveworks-like-ocean/tasks.md`, extracts the numbered task
IDs and checkboxes, and compares them with the compiled manifest. It verifies:

- exactly 80 current tasks are covered once;
- completed task count and per-stage progress agree with checkboxes;
- stage IDs and order are stable and unique;
- a stage is complete only if every mapped task is complete;
- the active stage is the first incomplete stage;
- user-facing capability claims do not exceed a stage's declared gate.

The build defines the source-root path only for this test. Production code has
no file-system or Markdown dependency. Whenever an apply session changes an
ocean task checkbox, the test requires the manifest checkpoint to change in the
same implementation batch.

Alternative: update the manifest only at full-stage boundaries. Rejected
because the user also needs the accurate `12/80` style progress within an active
stage.

### 4. Put a compact truth banner before all WaveWorks controls

The WaveWorks Lab panel begins with:

- manifest version and overall task progress;
- `Last completed` and `Active` stage;
- active simulation and geometry paths;
- a short `Visible now` statement;
- a warning-colored `Not yet` statement;
- `Next engineering checkpoint` and `Next major visual milestone`.

An expandable `Migration Roadmap` shows all eleven stages. Completed, active,
planned, and blocked states use text as well as color so the display does not
depend on color perception. Requested controls whose implementation stage is
not complete remain labeled planned/inactive using the same manifest metadata.

Alternative: place the information only in Statistics. Rejected because the
current usability problem is that users must already know where to look and may
run the scene without expanding that group.

### 5. Attribute automation output to the same checkpoint

WaveWorks Lab capture and diagnostic JSON gains a nested ocean-migration block
with manifest version, active stage ID/title, last completed stage, completed
and total task counts, backend, simulation path, geometry path, and enabled
visible capabilities. Image filenames may include the stable stage ID, while
the JSON fields remain authoritative.

The WaveWorks executable at
`D:\unity_project\WaveWorks_example-2.0.0-win64` is invoked only at the stage
comparison gates listed in the matrix. Its capture, camera, renderer, GPU time,
and quadtree statistics are stored as reference observations, not pass/fail
pixel-equality criteria.

Alternative: compare against WaveWorks after every task. Rejected because most
infrastructure tasks deliberately have no beauty-render change and subjective
comparison would create noise.

### 6. Preserve legacy presentation through scene gating

The status banner and roadmap render only when the active scene selects
`SpectralOcean`. The FFT Ocean Lab continues to call its existing legacy panel
and never reads or mutates the migration manifest. Tests cover scene switching
in both directions to catch status or settings leakage.

## Risks / Trade-offs

- [The compiled manifest can become stale] → Parse the authoritative OpenSpec task file in a required developer test and fail on any mismatch.
- [Task wording or numbering may change] → Use stable numeric task identifiers and fail loudly when the upstream task set is renumbered rather than guessing a remap.
- [A completed stage may still regress later] → Report stage completion and current validation health separately; a failed current gate adds a warning without rewriting history.
- [Status text can overpromise visual parity] → Every stage has both `Visible now` and `Not yet` claims plus a visual-impact classification.
- [The additional panel text may crowd controls] → Keep the summary compact and place the full matrix under one collapsed roadmap node.
- [WaveWorks performance is not directly comparable across builds] → Record GPU, backend, resolution, camera, and enabled features with every comparison and treat results as contextual.

## Migration Plan

1. Introduce the manifest model with stages 1 and 2 complete, stage 3 active,
   and overall progress matching the current 12/80 task state.
2. Add manifest/OpenSpec consistency tests before replacing existing status text.
3. Render the compact banner and expandable roadmap in the WaveWorks Lab only.
4. Add stage fields to capture and diagnostic output and produce a stage-2
   attribution fixture.
5. Add the human-readable stage matrix and update checklist to ocean technical
   documentation.
6. Thereafter, update the manifest and capture metadata in the same batch as
   each completed ocean migration task or stage.

Rollback removes the status presentation and diagnostic fields while leaving
the simulation, renderer settings, and both Lab scenes unchanged.
