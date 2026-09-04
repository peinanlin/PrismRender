Place an environment cubemap under `assets/environment/default/` using these six face names:

- `px`
- `nx`
- `py`
- `ny`
- `pz`
- `nz`

Supported file extensions:

- `.png`
- `.jpg`
- `.jpeg`
- `.bmp`
- `.tga`

Example:

- `assets/environment/default/px.png`
- `assets/environment/default/nx.png`
- `assets/environment/default/py.png`
- `assets/environment/default/ny.png`
- `assets/environment/default/pz.png`
- `assets/environment/default/nz.png`

If PrismRender cannot find or load a full cubemap set, it falls back to the built-in solid-color environment cubemap.
