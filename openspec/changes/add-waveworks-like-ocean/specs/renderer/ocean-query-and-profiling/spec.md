## Purpose

Expose asynchronous ocean sampling, conservative bounds, latency, timing, and resource statistics without introducing hidden synchronization into normal rendering.

## ADDED Requirements

### Requirement: Non-blocking displacement queries
The ocean system SHALL accept batched world-space displacement queries and SHALL return the most recent completed result with an associated simulation time or an explicit pending status, without forcing the render thread to wait by default.

#### Scenario: Result is not ready
- **WHEN** a query batch has been submitted but its GPU result is still in flight
- **THEN** polling reports pending and the render thread continues without a queue-wide wait

#### Scenario: Completed combined query
- **WHEN** a query result becomes available inside the local-wave domain
- **THEN** it contains the combined spectral and local displacement and identifies the simulation time used

### Requirement: Optional query and readback allocation
CPU readback and GPU query resources SHALL be independently configurable and SHALL not schedule readback copies when no consumer requests displacement data.

#### Scenario: Readback is unused
- **WHEN** query support is enabled but no batches are submitted during a frame
- **THEN** no ocean readback copy is added for that frame

### Requirement: Bounded historical results
When historical queries are enabled, the system SHALL retain a configured finite FIFO of completed simulation results and SHALL report when a requested time has expired from the archive.

#### Scenario: Query an expired time
- **WHEN** a caller requests a simulation time older than the retained FIFO
- **THEN** the query returns an unavailable-history status rather than unrelated current data

### Requirement: Conservative displacement estimate
The ocean system SHALL expose a conservative maximum displacement estimate suitable for inflating frusta and geometry bounds for the active spectral and local-wave settings.

#### Scenario: Wind amplitude increases
- **WHEN** a higher-amplitude spectrum configuration becomes active
- **THEN** the published conservative displacement estimate is refreshed before it is used for subsequent ocean culling

### Requirement: Stage-level performance statistics
The system SHALL report filtered CPU and GPU timings for spectrum evolution, inverse FFT, derived-map generation, foam, local waves, geometry selection, and total ocean work when timing support is enabled.

#### Scenario: GPU timers unavailable
- **WHEN** the backend cannot provide the requested timestamp capability
- **THEN** GPU timing fields are marked unavailable while the ocean continues to simulate and render

### Requirement: Resource and latency statistics
The system SHALL report active quality, texture resolution, allocated ocean memory, dispatch counts, visible geometry nodes or rings, query latency, and simulation-to-render latency.

#### Scenario: Change simulation quality
- **WHEN** a quality transition completes
- **THEN** the displayed resolution and memory statistics describe the newly active resources rather than retired allocations

