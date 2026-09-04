# PrismRender glTF Setup

This document explains how to enable local glTF loading in PrismRender without relying on network fetch during CMake configuration.

## 1. Provide tinygltf locally

Place the header below into the vendored third-party folder:

- `third_party/tinygltf/tiny_gltf.h`

PrismRender checks this location first during CMake configure.

## 2. Provide a startup scene

Place a scene file in:

- `assets/scenes/StartupScene.gltf`

If the scene uses an external `.bin` file or textures, keep them next to the `.gltf` file unless the asset uses another valid relative path layout.

Recommended minimal layout:

- `assets/scenes/StartupScene.gltf`
- `assets/scenes/StartupScene.bin`
- `assets/scenes/<textures...>`

## 3. Reconfigure and build

From the project root:

```powershell
cmake -S . -B build-windows-ci
cmake --build build-windows-ci --config Debug
```

If `tiny_gltf.h` is found, CMake should report that glTF loading is enabled.

## 4. Run

Launch:

- `build-windows-ci/Debug/PrismRender.exe`

## 5. What to check in the UI

In the ImGui debug panel, verify these fields:

- `glTF Import: Enabled`
- `Fallback Scene: No`
- `Scene Source: assets/scenes/StartupScene.gltf`
- `Scene Status: glTF startup scene loaded successfully.`

Also verify:

- `Render Objects` is greater than zero
- `Scene Objects` lists imported object names
- mesh / material handles are populated

## 6. If it still falls back

Check these points in order:

1. `third_party/tinygltf/tiny_gltf.h` exists
2. `assets/scenes/StartupScene.gltf` exists
3. any referenced `.bin` and texture files exist relative to the scene file
4. re-run CMake configure, not only build
5. read the `Scene Status` text in the debug panel for the fallback reason

## 7. Current behavior

If local tinygltf headers are missing, PrismRender still builds and runs. In that mode:

- glTF import stays disabled
- the built-in fallback scene is used
- the debug panel tells you that fallback is active
