---
name: prism-render-d3d12
description: Use this skill when extending PrismRender, a self-developed Direct3D 12 renderer for PC. Follow the project architecture, keep changes incremental, and evolve the renderer toward a modern engine-like runtime with asset import, PBR, render graph, visibility optimization, advanced lighting, and tooling.
---

# PrismRender D3D12 Skill

This skill applies when working on PrismRender, a self-developed real-time renderer focused on modern Direct3D 12 usage, renderer architecture, and incremental evolution toward a lightweight engine-style runtime.

PrismRender is not a full game engine today, but the implementation direction should intentionally move it closer to a real in-house rendering framework rather than a one-off graphics demo.

## Objectives

When using this skill, optimize for these goals:

- Keep the code modular, maintainable, and extensible
- Prefer real, compilable C++/D3D12 code over tutorial-style pseudocode
- Extend the existing structure incrementally instead of rewriting completed modules
- Preserve a clean separation between runtime, rendering, scene, asset, and UI responsibilities
- Build toward a renderer that can support real scene workflows, not only isolated graphics features

## Platform and scope

Current direction:

- Target platform: PC
- Graphics API: Direct3D 12
- Windowing: GLFW
- UI/debug tooling: ImGui
- Build system: CMake

Do not optimize the roadmap around mobile constraints unless the user explicitly asks for that later.

## Current implementation mindset

PrismRender should evolve from a staged renderer demo into a more engine-like rendering runtime. That means future work should not only answer "how to implement a graphics technique", but also:

- what problem exists in the current architecture or render path
- why the previous approach is insufficient for real scenes
- what modular extension point should be added
- how the new solution can be validated or compared against the old one

Prefer problem-driven upgrades over feature checklists.

## Architecture guidance

Organize code under these modules when applicable:

- `Core/`
- `Platform/`
- `RHI/`
- `Renderer/`
- `Scene/`
- `Asset/`
- `UI/`

Additional modules may be introduced when justified, for example:

- `Runtime/` for higher-level runtime orchestration
- `Tools/` for debug views, import helpers, profiling panels, or content inspection

Rules:

- Keep `main.cpp` minimal
- Do not merge unrelated responsibilities into a single class
- Prefer introducing a new class/module over growing a god object
- Preserve clear ownership boundaries between scene data, render submission, and low-level D3D12 resource management
- Avoid non-incremental refactors unless absolutely necessary

## Engine-oriented direction

The renderer should feel progressively closer to a self-developed rendering framework. Borrow high-level architectural ideas from engine-like projects when useful, but do not copy outdated stacks or old API choices directly.

Good ideas to borrow:

- app/runtime separation
- scene graph or entity/component-friendly data flow
- asset import pipeline
- resource registry / handle-based ownership
- pass-based renderer organization
- debug tooling and inspection panels

Avoid blindly copying:

- outdated OpenGL-first architecture
- editor-heavy dependencies too early
- legacy material models when modern PBR is more appropriate
- feature sets that do not fit the current PrismRender roadmap

## Recommended technical roadmap

The old Stage 1-5 renderer milestones are still useful as implementation history, but future work should follow this upgraded route.

### Phase A: Runtime and asset foundation

Priority goals:

- mesh import, preferably glTF first
- scene serialization / scene description loading
- resource registry and clear asset ownership
- camera controller and runtime scene inspection
- stable resize handling and render-target lifecycle hooks

Problems this phase addresses:

- current demo scenes are too hard-coded
- geometry/material content cannot scale without import and asset management
- later PBR, post-process, shadow, and GI work needs stable runtime data flow

### Phase B: Modern material pipeline

Priority goals:

- PBR metallic-roughness workflow
- base color / normal / metallic-roughness / AO / emissive texture support
- image-based lighting (irradiance + prefiltered environment + BRDF integration)
- glTF material mapping

Problems this phase addresses:

- Blinn-Phong is useful for learning but is not the right long-term material model
- material data needs to match real content pipelines
- lighting quality and asset interchangeability depend on a PBR baseline

### Phase C: Renderer architecture upgrade

Priority goals:

- render graph or pass graph
- cleaner pass ownership and dependency tracking
- explicit transient render target management
- GPU profiling hooks and renderer debug views

Problems this phase addresses:

- feature growth eventually makes manual pass orchestration fragile
- HDR, bloom, shadows, post-process, visibility, and GI should not accumulate as ad-hoc sequencing
- real renderer iteration benefits from explicit pass/resource relationships

### Phase D: Visibility and large-scene rendering

Priority goals:

- Hi-Z depth pyramid
- occlusion culling
- cluster-based or meshlet-like geometry preprocessing where appropriate
- indirect draw / GPU-driven submission
- instance batching improvements
- optional visibility-buffer or deferred-material path if justified

Problems this phase addresses:

- current frustum culling and basic instancing are not enough for larger scenes
- CPU-driven submission scales poorly
- overdraw and hidden geometry become major costs in real content

### Phase E: Advanced lighting and shadows

Priority goals:

- better directional shadow workflow, potentially cascaded shadow maps
- improved shadow filtering and stability
- localized light scaling strategy
- hybrid GI exploration on PC
- reflections and better environment lighting integration

Problems this phase addresses:

- basic shadows and local lights are not sufficient for convincing scenes
- full GI is expensive, so hybrid approaches should be introduced deliberately
- lighting architecture must support comparison between simple and improved solutions

### Phase F: Tooling and validation

Priority goals:

- scene hierarchy and inspector panels
- material and texture inspection UI
- renderer debug views
- performance counters and capture-friendly markers
- baseline vs improved comparison modes

Problems this phase addresses:

- complex rendering features are hard to trust without visibility and diagnostics
- profiling and comparison are necessary for engineering decisions
- a self-developed renderer becomes much more useful when it can explain itself at runtime

## GI and advanced rendering direction

If the project later explores techniques inspired by modern production renderers, treat them as staged research topics rather than immediate mandatory rewrites.

Examples of suitable future directions on PC:

- screen-space GI as a near-field baseline
- probe/DDGI-style fallback for off-screen or sparse coverage
- brick/probe/grid-based indirect lighting experiments
- clustered lighting
- visibility-buffer-driven material evaluation
- virtualized or streamed geometry experiments

When introducing these, always document:

- the baseline approach
- the weakness of that baseline
- the improved technique
- trade-offs in memory, bandwidth, stability, and implementation complexity
- how to compare the two approaches in PrismRender

## Implementation rules

When adding or extending a feature:

1. list files to add
2. list files to modify
3. describe data flow
4. describe validation method

Also:

- explain design trade-offs briefly when relevant
- state assumptions instead of hiding them
- keep class ownership explicit
- prefer extension seams that help the next phase

## Coding constraints

Follow these coding rules:

- Use `#pragma once`
- Prefer RAII and `Microsoft::WRL::ComPtr` for D3D12 objects
- Keep headers clean and minimize includes
- Keep naming consistent across modules
- Add concise comments only where logic is not obvious
- Avoid magic numbers; prefer named constants, configs, or descriptors
- Preserve future extensibility for shadowing, post-process, culling, instancing, PSO cache, asset import, PBR, and render graph work

## Practical guidance for Codex

When implementing tasks in PrismRender:

- inspect the existing module boundaries first
- prefer incremental integration over "perfect architecture" rewrites
- do not move large amounts of code unless the user explicitly wants refactoring
- preserve compile-ability at each step
- keep the project usable as a teaching renderer and as an engine-style prototype

If a requested technique is too old, too rigid, or mismatched with current PC renderer practice, adapt the idea to a more modern equivalent and explain why.

## Output expectations

When producing implementation output for PrismRender:

- show the directory/file path for each generated file
- provide complete code for key files
- explain how the feature is integrated into the current architecture
- explain data flow from scene/runtime to renderer to RHI when relevant
- mention validation steps and known limitations
- call out architecture risks before they turn into non-incremental rewrites
