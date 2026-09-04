## 1. Baseline and Feature Boundaries

- [x] 1.1 Build and run the current Ocean Lab on D3D12, record validation output, GPU timings, dispatch count, memory, camera settings, and representative near/mid/horizon captures in the project test-artifact location, and verify the baseline is reproducible before changing ocean code
- [x] 1.2 Build and run the current Ocean Lab on Vulkan where available, record the same validation and capture set, and verify any pre-existing backend differences are documented rather than attributed to the new implementation
- [x] 1.3 Add the `src/Renderer/Features/Ocean/` module boundary with settings, statistics, simulation, map-builder, foam, surface, local-wave, query, and shared resource headers/sources; update CMake and verify the project builds with empty or pass-through implementations and `main.cpp` remains unchanged
- [x] 1.4 Replace flat ocean fields in `RenderSettings` with nested `OceanSettings` groups for simulation, base wind, swell, foam, local waves, geometry, shading, query, and debug while providing temporary legacy field migration; verify serialization/default-setting tests preserve the old Ocean Lab result when legacy mode is selected
- [x] 1.5 Add `LegacyFft` and `SpectralOcean` implementation selection during migration, ensure only the selected path schedules work, and verify frame diagnostics report exactly one active ocean implementation
- [x] 1.6 Implement and unit-test the WaveWorks Reference preset values, units, project quality mapping, validation ranges, and group-reset defaults, including all wind, swell, spectral foam, local foam, local-domain, geometry, sun, UV-warp, and microfacet values documented by the specs
- [x] 1.7 Add a separately selectable WaveWorks Lab scene that loads the WaveWorks Reference spectral defaults, preserve the existing FFT Ocean Lab configuration and rendering path unchanged, isolate scene-specific UI/defaults, and verify switching between both scenes schedules exactly one implementation without settings leaking across scenes

## 2. Portable FFT Foundation

- [x] 2.1 Extract the existing butterfly logic into an `OceanFft` component supporting runtime power-of-two sizes 128, 256, and 512; verify CPU-side indexing/twiddle tests and a GPU impulse round-trip test pass at every size
- [x] 2.2 Replace the hard-coded seven-bit reversal and fixed stage arrays with resolution-derived resources and dispatch metadata; verify no 128-specific constants remain in the reusable FFT path and the legacy 128 output remains within tolerance
- [x] 2.3 Add four-slice spectrum working resources and per-slice/subresource views with an explicit precision policy; verify D3D12 and Vulkan resource creation, descriptor reflection, UAV writes, and SRV reads pass validation at Normal quality
- [x] 2.4 Split spectrum evolution, horizontal FFT, vertical FFT, output-map, foam, and mip work into separately named RenderGraph passes with declared dependencies; verify graph diagnostics show the expected edges and no manual resource transition contradicts the graph
- [x] 2.5 Add cached deterministic Gaussian initial-spectrum resources keyed by seed and spectrum settings; verify advancing only absolute time schedules no initial-spectrum pass while changing a wind setting schedules exactly one compatible rebuild
- [x] 2.6 Add numerical FFT regression fixtures for impulse, constant, sinusoid, Hermitian symmetry, normalization, and finite output; verify CPU-reference and GPU results satisfy documented FP32 tolerances on D3D12 and Vulkan

## 3. Four-Cascade JONSWAP Simulation

- [x] 3.1 Implement a CPU reference for JONSWAP energy, base-wind and swell directional spreading, fetch/peaking/cutoff controls, and deep-water dispersion; verify unit tests cover zero wave number, zero amplitude, opposing directions, peaking, and finite-value limits
- [x] 3.2 Implement the Slang initial-spectrum kernel for combined base wind and swell using the same canonical units and deterministic random seed; verify sampled GPU spectrum values agree with the CPU reference within tolerance
- [x] 3.3 Implement normalized logarithmic band windows for the four default patch lengths 15.625 m, 62.5 m, 250 m, and 1000 m; verify sampled band weights sum to one across transition regions and do not double or drop total spectral energy
- [x] 3.4 Evolve all four cascades from absolute simulation time and run their inverse FFTs as array-slice workloads; verify each cascade contains the expected wavelength range and replaying a captured time is deterministic on the same backend
- [x] 3.5 Implement Normal=128, High=256, and Extreme=512 resource-set creation and frame-safe quality switching; verify repeated live quality changes do not access retired resources, leak descriptors, or preserve incompatible foam history
- [x] 3.6 Add spectrum and per-cascade displacement debug outputs; verify disabling swell removes only the swell contribution and disabling individual cascades does not alter the remaining cascade data

## 4. Derived Maps, Mips, and Persistent Spectral Foam

- [x] 4.1 Implement spectral displacement output including vertical and choppy horizontal displacement in PrismRender XZ-horizontal/Y-up coordinates; verify analytic single-wave fixtures produce the expected displacement direction and amplitude
- [x] 4.2 Implement gradient, tangent, normal, Jacobian/folding, and first/second slope-moment generation for every cascade; verify flat and analytic sine-wave fixtures produce normalized finite normals and expected derivatives/moments
- [x] 4.3 Create four-slice RGBA16F displacement, gradient, and moment arrays with complete mip chains and coherent publication metadata; verify every mip and slice is initialized, transitioned, and sampled without validation warnings
- [x] 4.4 Implement ping-pong spectral foam history with generation threshold, generation amount, whitecap threshold, dissipation, falloff, and time scaling; verify a generated whitecap persists after folding stops and decays monotonically to zero
- [x] 4.5 Implement foam-history reset/versioning for simulation reset, quality change, incompatible spectrum change, and device/resource recreation; verify the first frame after each event contains no stale or non-finite foam
- [x] 4.6 Implement displacement/gradient/moment/foam mip filters appropriate to each data type rather than reusing an unqualified color average; verify distant sampling remains normalized, bounded, and free of cascade-edge flicker
- [x] 4.7 Add stage-level GPU markers and resource statistics for evolution, FFT, map build, foam, and mips; verify timings and allocated-memory totals correspond to the active resource set

## 5. Dedicated Ocean Surface on the Existing Clipmap

- [x] 5.1 Add dedicated ocean Slang vertex/pixel shaders and `OceanSurfaceRenderer` pipeline/layout creation, remove new-ocean shading dependencies from the generic `Mesh.hlsl` path, and verify ordinary mesh pipeline reflection and rendering remain unchanged
- [x] 5.2 Bind four displacement, gradient, moment, and foam slices plus atmosphere/environment resources through reflected RHI layouts; verify D3D12 and Vulkan descriptor sets expose the same logical resources and render without missing-binding diagnostics
- [x] 5.3 Implement world-space cascade UV mapping, distance fades, derivative-aware mip selection, and sinusoidal UV warping on the current camera-following clipmap; verify camera travel from near field to horizon produces no hard cascade pop or world-space swimming
- [x] 5.4 Implement slope-moment microfacet distribution, Smith masking, effective Fresnel, sun specular, and environment reflection; verify rough-water highlights broaden with moment variance and all microfacet debug toggles operate independently
- [x] 5.5 Integrate physical-atmosphere sun/sky, deep-water color, crest scattering, aerial transmittance, and existing frame tonemapping/bloom ownership; verify changing the authoritative sun direction updates atmosphere and ocean lighting in the same frame
- [x] 5.6 Add licensed or deterministic procedural foam, bubble, and wind-gust detail assets and combine them with persistent foam energy; verify asset provenance is documented and foam changes color/roughness without replacing simulation history
- [x] 5.7 Add wireframe, cascade boundary/contribution, normal, slope-moment, foam-energy, and clipmap-LOD debug modes; verify each mode is selectable without mutating simulation output
- [x] 5.8 Validate clipmap overlap, camera snapping, underlay behavior, near-plane traversal, and horizon coverage under conservative maximum displacement; verify no cracks, exposed clear background, or one-frame geometry pops occur in the reference camera path

## 6. Local Interactive Waves

- [x] 6.1 Add local-wave settings and allocate height, velocity, displacement, gradient, and foam ping-pong resources for supported 128 through 2048 grids; verify capability limits clamp unsupported sizes and the default 200 m/512 configuration reports correct memory
- [x] 6.2 Implement the damped height/velocity wave solver with bounded fixed substeps and absorbing/faded boundaries; verify long-frame, flat-state, impulse-propagation, energy-decay, and finite-output tests pass
- [x] 6.3 Define and upload batched world-space disturbance records with position, radius, strength, and optional velocity direction; verify inside, partially intersecting, and outside-domain disturbances produce the specified behavior
- [x] 6.4 Implement local displacement, gradient/folding, moment-compatible data, and persistent foam generation; verify local choppiness and foam controls remain independent from spectral settings
- [x] 6.5 Composite local displacement and shading into the ocean surface with a seam-free domain-edge fade; verify an impulse reaching the local boundary does not reveal a rectangular discontinuity
- [x] 6.6 Add deterministic rain and moving-wake demonstration emitters plus reset controls; verify fixed-seed replay generates the same disturbance sequence and reset clears displacement, velocity, gradients, and foam
- [x] 6.7 Add local-wave RenderGraph passes, timers, dispatch/memory statistics, and pause behavior; verify pausing spectral simulation does not corrupt local state and the configured shared/independent time policy is reflected in diagnostics

## 7. Ocean Lab UI and Settings Lifecycle

- [x] 7.1 Add `src/UI/OceanLabPanel.h/.cpp`, register it for the Ocean Lab scene, and reduce `DebugPanel` ocean controls to the high-level toggle and panel focus action; verify detailed controls appear only in the dedicated panel
- [x] 7.2 Implement General, Wind Waves, Local Waves, Geometry, Statistics, and Debug groups with units, ranges, tooltips, feature gating, and the WaveWorks Reference preset; verify a UI snapshot/checklist contains every specified control and default
- [x] 7.3 Implement degree-to-direction canonicalization and input validation feedback for non-finite or invalid settings; verify 0 and 360 degrees map identically and invalid values never reach GPU constants
- [x] 7.4 Implement dirty scopes for constant update, initial-spectrum rebuild, resource rebuild, geometry rebuild, local reset, and history reset; verify targeted-setting tests show sun intensity does not rebuild simulation and quality does rebuild resources safely
- [x] 7.5 Implement group reset, full reset, simulation-history reset, simulate/render pause controls, and pending-rebuild status; verify each reset changes only its owned settings and pausing simulation continues rendering the last coherent result
- [x] 7.6 Display backend, geometry path, quality/resolution, validation warnings, timers, memory, dispatches, cascade state, local state, geometry counts, query latency, and simulation-to-render latency; verify unavailable capabilities are labeled and unsupported controls are disabled

## 8. Queries, Bounds, and Profiling

- [x] 8.1 Implement a conservative spectral-plus-local displacement estimator and integrate it with ocean frustum/bounds inflation; verify increasing amplitude updates the bound before culling and captured displaced vertices remain inside it
- [x] 8.2 Implement batched GPU displacement queries tagged with simulation time and sequence ID; verify queries inside and outside the local domain return the correct combined or spectral-only displacement
- [x] 8.3 Implement demand-driven CPU readback with pending/completed/unavailable states and no default blocking; verify an in-flight query can be polled while frame rendering continues without a queue-wide wait
- [x] 8.4 Implement the configurable bounded historical FIFO and expiration reporting; verify archived times return the correct tagged result and expired times never return current unrelated data
- [x] 8.5 Add filtered CPU/GPU timing, render latency, readback latency, memory, dispatch, and resource-version statistics; verify timer-unavailable backends report unavailable values without disabling the ocean
- [x] 8.6 Add automated tests proving no readback copy is scheduled when no CPU query is submitted and no query resource is allocated when both query modes are disabled

## 9. RHI and Slang Tessellation Support

- [x] 9.1 Add Hull and Domain shader stages to RHI shader types, string conversion, reflection stage masks, Slang stage mapping, shader cache identity, and tests; verify valid DXIL and SPIR-V ocean stage fixtures compile and reflect expected bindings
- [x] 9.2 Extend graphics pipeline descriptions with optional hull/domain binaries, patch topology, and control-point count; verify validation accepts complete compatible stage sets and rejects partial stages, non-patch topology, and invalid control counts with actionable messages
- [x] 9.3 Extend descriptor-layout merging and pipeline-cache serialization for hull/domain visibility while preserving existing cache identity behavior; verify all existing vertex/pixel and compute pipeline tests pass unchanged
- [x] 9.4 Implement D3D12 hull/domain bytecode, patch topology, root-signature visibility, capability limits, and native PSO creation; verify a minimal tessellated triangle renders under the D3D12 debug layer without errors
- [x] 9.5 Implement Vulkan tessellation feature detection, hull/domain stage mapping, patch-list topology, `VkPipelineTessellationStateCreateInfo`, descriptor visibility, and limits; verify a minimal tessellated triangle renders with validation layers and unsupported devices report fallback capability
- [x] 9.6 Expose tessellation capability and maximum patch controls through `GraphicsDeviceCapabilities`; verify the Ocean renderer chooses tessellation only when every required feature is reported

## 10. Adaptive Quadtree Ocean Geometry

- [x] 10.1 Implement reusable patch vertex/index geometry for configured cell count and finite edge/topology variants; verify topology unit tests cover every neighbor-LOD edge combination without holes or duplicate out-of-range indices
- [x] 10.2 Implement camera/frustum-driven quadtree node selection using minimum patch size, maximum screen-space edge length, maximum LOD, and conservative displacement margin; verify deterministic camera fixtures produce stable node sets and cull off-screen nodes
- [x] 10.3 Implement adjacent-node LOD balancing and geomorph factors; verify adversarial camera paths never leave neighboring nodes more than the supported LOD difference and geometry transitions are continuous
- [x] 10.4 Group selected nodes by patch topology and upload per-instance transforms/morph data for instanced rendering; verify draw-call and instance counts match selected nodes and remain within buffer limits
- [x] 10.5 Add Slang ocean vertex/hull/domain stages for patch morphing, edge-compatible tessellation factors, cascade/local displacement, and PrismRender coordinate conventions; verify analytic flat and displaced patch tests match the clipmap surface within tolerance
- [x] 10.6 Add solid and wireframe adaptive ocean PSOs and runtime selection between tessellated quadtree and clipmap fallback; verify disabling tessellation or using an unsupported backend preserves simulation and ocean shading
- [x] 10.7 Add quadtree node, rendered node, refinement iteration, patch topology, tessellation factor, CPU update-time, and geometry-LOD debug statistics; verify values update with camera motion and agree with captured draw submissions
- [x] 10.8 Run near-surface, high-altitude, horizon, rapid-camera, and LOD-boundary visual tests; verify no cracks, T-junction exposure, culling holes, geomorph pops, or displacement mismatch occur

## 11. Async Compute and Frame Coherency

- [x] 11.1 Audit every spectral/local resource as persistent history, transient working data, or published output and declare per-subresource RenderGraph states; verify D3D12 and Vulkan barrier diagnostics contain no undefined read or write-after-write hazard
- [x] 11.2 Add coherent output versioning so surface rendering samples only a fully completed set of cascade/local resources; verify an artificially delayed cascade never produces a mixed-time rendered frame
- [x] 11.3 Move eligible evolution, FFT, map, foam, local-wave, and mip passes to the native compute queue behind a setting; verify compute-to-graphics waits are generated and both queue modes produce equivalent captures within tolerance
- [x] 11.4 Integrate ocean workloads with the existing queue scheduling cost model and expose overlap versus synchronization cost; verify native compute is selected only under the configured policy and can be disabled without resource recreation
- [x] 11.5 Stress quality changes, pause/resume, local reset, resize, scene switch, and shutdown with compute work in flight; verify there are no device removals, use-after-retire accesses, deadlocks, or leaked resources

## 12. Cross-Backend Validation and Performance Acceptance

- [x] 12.1 Add automated settings, spectrum, FFT, foam, local solver, geometry, query, and RHI tessellation tests to CMake/CTest; verify the complete test suite passes in the normal project test command
- [x] 12.2 Run the D3D12 debug layer through initialization, all quality modes, all debug views, local disturbances, queries, tessellation/fallback switching, resize, and shutdown; verify no new errors or warnings remain
- [x] 12.3 Run Vulkan validation through the same matrix where available; verify output is finite, descriptor/layout usage is valid, and captures remain within defined numerical/image tolerances of D3D12
- [x] 12.4 Produce versioned golden captures for calm wind, default reference, swell-dominant, strong foam, local impulse, rain/wake, cascade debug, and geometry LOD views; verify the image-regression process detects an intentional shader perturbation
- [x] 12.5 Benchmark Normal, High, and Extreme at the selected reference resolution/GPU, record per-stage GPU time, overlap, visible geometry, dispatches, and memory, then document the accepted budgets and verify no preset exceeds the approved limit without a visible UI warning
- [x] 12.6 Compare legacy and spectral paths for seams, periodic repetition, temporal foam, horizon detail, atmospheric consistency, and camera stability; verify the spectral path satisfies every scenario in the six capability specs
- [x] 12.7 Update the Ocean Lab technical documentation with architecture, data flow, formulas, units, quality mapping, UI controls, backend differences, performance results, limitations, validation steps, and clean-room/NVIDIA reference note; verify every new setting and debug view is documented
- [x] 12.8 Keep `SpectralOcean` as the WaveWorks Lab default only after D3D12/Vulkan gates and approved performance budgets pass, preserve the original FFT Ocean Lab and its `FftOcean` renderer path as a permanent comparison scene, remove only obsolete same-scene migration coupling, and verify each scene schedules exactly its intended implementation
