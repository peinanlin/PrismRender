## Purpose

Defines a scalable water-optics pipeline that consumes the existing kilometer-scale ocean simulation while independently providing HPWater-style material, refraction, caustics, and underwater volumetric behavior.

## ADDED Requirements

### Requirement: Independent wave and optics selection
The renderer SHALL select the ocean wave source independently from the water optics model, and the HPWater-style model SHALL consume the same coherent spectral displacement, gradient, slope-moment, foam-history, and bounded local-wave outputs available to the existing spectral ocean path.

#### Scenario: Select HPWater optics
- **WHEN** the spectral wave source and HPWater-style optics model are selected
- **THEN** the renderer retains the spectral FFT and local-wave simulation while replacing only the water material and post-lighting pipeline

#### Scenario: Switch optics models
- **WHEN** two frames use identical simulation settings, time, camera, and lighting but different water optics models
- **THEN** their surface displacement and local-wave state remain identical while their optical output may differ

### Requirement: Large-area surface continuity and distance quality
The HPWater-style pipeline SHALL render the existing adaptive spectral surface to the configured ocean horizon, SHALL blend local-wave displacement only inside its bounded domain, and SHALL reduce expensive optical work through configurable near, middle, and far quality tiers without removing the far FFT surface.

#### Scenario: View the horizon
- **WHEN** the ocean extends beyond the near optical range
- **THEN** spectral displacement, atmosphere integration, reflection, and simplified absorption remain visible while ray-marched refraction, caustics, and local-wave sampling fade out according to the configured tiers

#### Scenario: Leave the local-wave domain
- **WHEN** a surface point approaches the edge of the bounded local-wave domain
- **THEN** local displacement, gradient, and foam blend continuously into the spectral-only ocean without a height or normal seam

### Requirement: Dedicated water visibility and material buffers
The renderer SHALL preserve opaque-scene depth and color without water, SHALL produce a distinct water depth and water mask, and SHALL encode water normal, roughness, absorption, scattering, foam, and motion data without executing ordinary mesh material sampling.

#### Scenario: Opaque object occludes water
- **WHEN** an opaque object is closer to the camera than the water surface
- **THEN** the water mask remains clear at that pixel and the HPWater composite leaves the opaque result unchanged

#### Scenario: Inspect water material data
- **WHEN** a visible spectral water patch is rasterized
- **THEN** its dedicated buffers contain finite normalized optical data and a motion vector derived from the current and previous displaced surface positions

### Requirement: Thickness-aware reflection and refraction
The water composite SHALL combine Fresnel reflection with scene-color refraction derived from opaque depth, water depth, and water normal; it SHALL provide a bounded approximation by default and an optional depth-guided ray-marching mode with finite sample limits, thickness rejection, and jitter.

#### Scenario: Use default refraction
- **WHEN** high-precision ray marching is disabled
- **THEN** visible water uses a bounded normal- and thickness-aware screen-space offset and falls back safely when the refracted sample leaves the screen or crosses in front of the water surface

#### Scenario: Use high-precision refraction
- **WHEN** high-precision ray marching is enabled
- **THEN** the renderer searches the opaque depth field with the selected bounded sample count and rejects self-intersections or invalid hits without producing non-finite color

### Requirement: Water absorption, scattering, and foam response
The HPWater-style optical model SHALL apply Beer-Lambert transmittance from non-negative absorption and scattering coefficients, Fresnel energy partition, deep-water volume response, thin-layer and backlit response, environment and primary-light reflection, and foam-dependent diffuse and roughness behavior.

#### Scenario: Increase optical thickness
- **WHEN** the refracted path length increases while material coefficients remain fixed
- **THEN** transmitted scene radiance decreases monotonically and in-scattered water color approaches a finite asymptote

#### Scenario: Render foam
- **WHEN** spectral or local simulated foam energy is present
- **THEN** the water result becomes rougher and more diffusely reflective only where that physical foam mask is non-zero

### Requirement: Near-field caustics
The renderer SHALL generate camera-centered, temporally stable caustic energy from the combined spectral and local surface normals, SHALL apply it only to valid underwater receivers, and SHALL provide bounded single-channel and higher-quality chromatic modes whose coverage and resolution are independent of total ocean size.

#### Scenario: Render a near underwater receiver
- **WHEN** a lit opaque receiver lies below visible water inside the active caustic coverage
- **THEN** it receives depth-attenuated caustic energy that responds to the current water normal field

#### Scenario: Render beyond caustic coverage
- **WHEN** a receiver lies outside all active caustic cascades
- **THEN** caustic contribution fades to zero without affecting the far ocean surface or opaque lighting stability

### Requirement: Underwater volumetric reconstruction
The renderer SHALL detect the camera's water-medium state from the coherent ocean surface, accumulate underwater scattering at reduced resolution, reconstruct it with temporal motion/depth rejection and edge-aware upsampling, and reset invalid history after camera cuts, resize, quality change, scene change, or water-history reset.

#### Scenario: Enter the water
- **WHEN** the camera crosses below the queried water surface with the configured hysteresis
- **THEN** underwater absorption, scattering, and the waterline transition activate without changing the underlying FFT simulation

#### Scenario: Reject stale history
- **WHEN** a disocclusion, camera cut, resize, or water-history reset invalidates the previous volumetric sample
- **THEN** the current frame excludes the invalid history and does not leave a persistent foreground halo or ghost

### Requirement: Backend and compatibility guarantees
The pipeline SHALL compile and execute through the project RHI on D3D12 and Vulkan, SHALL expose finite fallbacks when optional capabilities are unavailable, and SHALL leave the existing legacy FFT and WaveWorks-style optical paths unchanged when HPWater-style optics are not selected.

#### Scenario: Run an existing ocean Lab
- **WHEN** `ocean` or `waveworks-ocean` is selected
- **THEN** no HPWater-specific water buffers or passes are scheduled and the existing output contract remains unchanged

#### Scenario: Validate supported backends
- **WHEN** the HPWater Ocean Lab runs under D3D12 or Vulkan validation
- **THEN** its logical bindings, barriers, histories, and output remain valid without backend-specific shader source forks
