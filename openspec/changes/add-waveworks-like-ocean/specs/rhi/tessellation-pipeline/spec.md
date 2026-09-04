## Purpose

Extend the portable graphics abstraction so Slang hull and domain shaders can create validated tessellation pipelines on capable D3D12 and Vulkan devices.

## ADDED Requirements

### Requirement: Hull and domain shader compilation
The shader compiler interface SHALL accept hull and domain stages and SHALL produce valid reflected DXIL or SPIR-V binaries for supported targets.

#### Scenario: Compile ocean tessellation shaders for D3D12
- **WHEN** valid Slang hull and domain entry points are requested for the D3D12 target
- **THEN** the compiler returns valid DXIL binaries with the expected stage identity and resource reflection

#### Scenario: Compile ocean tessellation shaders for Vulkan
- **WHEN** valid Slang hull and domain entry points are requested for the Vulkan target
- **THEN** the compiler returns valid SPIR-V binaries with the expected stage identity and resource reflection

### Requirement: Tessellation graphics pipeline description
The RHI SHALL allow a graphics pipeline to specify a compatible vertex, hull, domain, and pixel shader set together with patch topology and control-point count.

#### Scenario: Create a valid triangle-patch pipeline
- **WHEN** a pipeline contains compatible vertex, hull, domain, and pixel stages and a supported patch control-point count
- **THEN** pipeline validation succeeds and the backend can create the native pipeline

#### Scenario: Domain stage supplied without hull stage
- **WHEN** a graphics pipeline supplies a domain shader without its required hull shader
- **THEN** validation rejects the pipeline with a diagnostic identifying the incompatible stage set

### Requirement: Backend capability reporting
The RHI SHALL report whether tessellation shaders, patch topology, and the requested tessellation limits are supported by the active adapter.

#### Scenario: Unsupported adapter
- **WHEN** the active adapter does not expose required tessellation support
- **THEN** capability reporting returns false before the renderer attempts to create a tessellation pipeline

### Requirement: Native stage and topology mapping
The D3D12 and Vulkan backends SHALL map hull/domain stages, descriptor visibility, patch topology, control points, and raster state to the corresponding native API definitions.

#### Scenario: Bind tessellation resources
- **WHEN** a tessellation pipeline uses reflected constant buffers and sampled textures from multiple shader stages
- **THEN** the generated root signature or descriptor layout makes those bindings visible to every reflected consuming stage

### Requirement: Non-tessellation compatibility
Existing vertex/pixel and compute pipelines SHALL retain their current validation, serialized cache identity, and runtime behavior when tessellation support is added.

#### Scenario: Build an existing mesh pipeline
- **WHEN** an existing vertex/pixel pipeline description is used unchanged
- **THEN** it creates and renders without requiring hull/domain shaders or patch topology

### Requirement: Actionable unsupported-path diagnostics
Requests for unsupported tessellation combinations SHALL fail through normal RHI validation with actionable diagnostics and SHALL not cause a native API device error or process crash.

#### Scenario: Request too many control points
- **WHEN** a pipeline requests more patch control points than the active device supports
- **THEN** validation fails before native pipeline creation and reports the supported limit

