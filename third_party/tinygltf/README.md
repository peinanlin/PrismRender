Place `tiny_gltf.h` in this directory to enable local vendored glTF support.

Recommended layout:

- `third_party/tinygltf/tiny_gltf.h`

PrismRender's CMake configuration checks this folder first. If the header is present, glTF scene loading will be compiled in without requiring network fetch.

If the header is missing, PrismRender will still build and run, but it will fall back to the built-in sample scene.

After adding the header, re-run:

```powershell
cmake -S . -B build-verify
cmake --build build-verify --config Debug
```

Then launch PrismRender and confirm in the debug panel that:

- `glTF Import: Enabled`
- `Fallback Scene: No` if `assets/scenes/StartupScene.gltf` also exists
