# Parallel command-recording audit

`GraphPassOptions::parallelRecordable` is only a request. A pass enters a
worker lane when it also supplies
`GraphParallelRecordingContract::AuditedIndependent()`. That declaration
means the callback:

- records only through the isolated `ICommandContext` supplied to it;
- reads frame-stable pipelines, descriptor sets, resources and value captures;
- does not allocate/update descriptors, create PSOs, write caches or enqueue
  dynamic uploads;
- does not mutate Feature, scene, frame, history or readback state.

Barrier planning, aliasing, queue-batch assembly, native recording creation,
ordered replay/append and GPU submission remain on the RenderGraph execution
lane. The executor asserts that affinity at those boundaries.

## Audited worker-eligible callbacks

- shared frontend: Hi-Z mip recording/readiness, deferred lighting and the
  three bloom passes;
- clustered-light build, GPU visibility, atmosphere sky-view, GTAO, SSR, TAA
  resolve and variance-shadow conversion/blur;
- water-optics volumetric accumulate/temporal/reconstruct/publish. These
  methods are command-only readers of frame-indexed immutable bindings.

## Execution-lane-only callbacks

- every pass without the complete contract, even if legacy code requests
  `parallelRecordable`;
- descriptor creation/update, PSO/shader cache population, dynamic upload,
  resource resize/rebuild, readback publication and Feature/history mutation;
- particle-fluid pass callbacks remain execution-lane-only in this change.
  Their four demos are excluded from the automated architecture matrix, so
  they are not promoted without a dedicated ownership fixture.

Adding a pass to the eligible list requires code review of its complete callback
and its captures. The graph resource declarations alone are not proof of CPU
thread safety.
