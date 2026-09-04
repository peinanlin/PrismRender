
# AGENTS.md

## Project
PrismRender is a self-developed real-time renderer focused on demonstrating modern graphics API usage and renderer architecture.
This is a renderer project, not a full game engine.

## Primary goals
- Emphasize Direct3D 12 knowledge and renderer architecture
- Keep the codebase modular, maintainable, and extensible
- Build features incrementally instead of rewriting existing structure
- Prefer real, compilable code over pseudocode
- Keep `main.cpp` minimal

## Tech stack
- Language: C++
- Graphics API: Direct3D 12
- Windowing: GLFW
- UI: ImGui
- Build system: CMake

## Architecture rules
Organize code under these modules when applicable:
- Core/
- Platform/
- RHI/
- Renderer/
- Scene/
- Asset/
- UI/

Do not place unrelated logic into a single file.
Prefer adding new classes/modules instead of expanding `main.cpp`.

## Development rules
- Make incremental changes on top of the current architecture
- Do not rewrite completed modules unless necessary
- When adding a new feature, list:
  1. files to add
  2. files to modify
  3. data flow
  4. validation method
- Explain design trade-offs briefly when relevant
- Use clear class ownership and responsibility boundaries

## Coding style
- Use `#pragma once`
- Prefer RAII and `Microsoft::WRL::ComPtr` for D3D12 objects
- Keep headers clean and avoid unnecessary includes
- Keep method names and member names consistent
- Add concise comments for non-obvious logic
- Avoid magic numbers; prefer named constants or config structs

## Rendering roadmap
The renderer should evolve in these stages:
1. Window + D3D12 initialization + swapchain + triangle
2. Mesh + camera + transform + cube/multi-object rendering
3. Shader/texture/material + Blinn-Phong lighting
4. Shadow map + HDR + bloom + tonemapping
5. Frustum culling + GPU instancing + PSO cache + profiling UI

## Output expectations
When implementing a task:
- show the directory/file path for each generated file
- provide complete code for key files
- preserve future extensibility for shadow, post-process, culling, instancing, and PSO cache
- prefer engineering-quality structure over tutorial-style shortcuts