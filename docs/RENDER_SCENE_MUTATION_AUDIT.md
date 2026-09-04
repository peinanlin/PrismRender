# RenderScene mutation audit

This audit is the P4/D5 inventory for immutable scene publication. It covers
every production entry that can mutate the `RenderObject` array directly.

## Public mutation boundary

- `GetRenderObjects()` is read-only. A mutable `RenderScene` no longer selects
  a mutable overload accidentally.
- `AddRenderObject`, `ClearRenderObjects`, and `ReplaceRenderObjects` advance
  the O(1) topology revision.
- `EditRenderObjectsForFullRebuild()` is the only raw value-edit escape hatch.
  Acquiring it advances the O(1) raw-mutation revision. If neither a committed
  data revision nor a scene generation accompanies the write, the versioned
  extractor reports `raw-mutable-write-full-rebuild` and rebuilds.
- A topology change without committed change metadata similarly reports
  `untracked-topology-full-rebuild`.
- No path hashes or compares every `RenderObject` to decide whether to reuse a
  snapshot. `sourceObjectVisitCount == 0` is asserted for reuse.

## Production raw-write call sites

| Call site | Why it writes | Commit/invalidation boundary |
| --- | --- | --- |
| `WorldRenderSceneBridge::ImportRenderScene` | Assigns freshly created World entity IDs back to imported render objects. | Called only by `SceneSession::InitializeWorldFromRenderScene`, which immediately advances `SceneGeneration`, resets data revision, and resets the extractor mapping. |
| `FeatureLabSceneFactory::PopulateLargeWorldPrecision` | Rebases newly constructed demo objects to the large-world anchor. | Runs during demo construction before `SceneSession::InitializeWorldFromRenderScene`; activation publishes a new scene generation. |

All other production `GetRenderObjects()` call sites are const reads. Runtime
World edits use command transactions and
`WorldRenderSceneBridge::SynchronizeToRenderScene`, which publishes through
`ReplaceRenderObjects` plus the committed `SceneChangeSet`. Unknown external
writes must call `SceneSession::MarkWorldChanged`, producing the visible
`unknown-write-full-rebuild` reason.

## D5 coverage map

| Mutation source | Coverage/fallback |
| --- | --- |
| Entity topology, hierarchy, transform, visibility/editor-only, mesh/material override | `RenderScenePublicationTests` mutation matrix: one rebuild per committed data revision, then zero-visit reuse. |
| Transaction commit, undo/redo, journal replay, world load | `SceneSessionTests`; failed command and rollback assertions verify no partial World or publication state. |
| Demo/streaming/programmatic scene activation | Scene-generation and untracked-topology cases in `RenderScenePublicationTests`; activation/session tests cover generation reset. |
| Runtime binding replacement/reimport/eviction | Binding revision and old-frame packet resource-lease cases in `RenderScenePublicationTests`. |
| Camera, lighting, settings, validation navigation | Dynamic input/history cases reuse static data with zero source visits. |
| GPU visibility and editor helpers | Versioned feedback identity tests reject stale generation/data/view mappings without mutating published objects. |
| Ocean/Fluid/terrain simulation and time | Logical time and Feature lifecycle tests keep updates outside static scene dirty state; Fluid remains module-fixture-only. |

