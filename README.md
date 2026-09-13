# Disclaimer: This is an AI slop generated with codex and claude code


## SkyrimUpscaler

> ## **Do not run this with Community Shaders.**
>
> **It is not supported, and it is redundant.** Community Shaders replaces the
> same part of the render pipeline this plugin interposes on and already
> provides its own upscaling. Running both means two mods fighting over the
> scene targets and the present path at once.
>
> **Use one or the other, never both.**

An SKSE plugin that brings NVIDIA DLSS, DLSS Frame Generation, DLSS Ray
Reconstruction, DLSS 5 Neural Rendering and AMD FSR to Skyrim Special Edition
and Anniversary Edition — alongside ENB, configured in-game.

Ported from [jarari/fo4test](https://github.com/jarari/fo4test), the Fallout 4
upscaler this project is derived from, which is itself a fork of
[doodlum/fo4test](https://github.com/doodlum/fo4test). See
[CREDITS.md](CREDITS.md).

> **Status: experimental.** Every feature listed below has been observed working
> in-game, on one configuration: an RTX 4070 Ti, AE 1.6.1170, ENB, 1920x1080.
> No other hardware, runtime or resolution has been tested. During development,
> frame generation hung the GPU repeatedly and once produced a
> `DPC_WATCHDOG_VIOLATION` bugcheck; it is off by default and guarded, and
> [docs/SAFETY.md](docs/SAFETY.md) describes what by.

## Features

| Feature | Notes |
| --- | --- |
| DLSS Super Resolution | Quality modes DLAA, Quality, Balanced, Performance, Ultra Performance |
| AMD FSR | Same quality modes; works without an RTX GPU |
| DLSS Frame Generation | 2x. Off by default. A present watchdog disables it for the session after two consecutive Present calls over 80 ms |
| RTX 40 multi-frame generation | Up to 6x on Ada, which the driver otherwise caps at 2x, via [RTX40MFG-Unlock](https://github.com/dashdogy/RTX40MFG-Unlock) |
| DLSS Ray Reconstruction | Skyrim has no G-buffer, so normals are reconstructed from depth and the material terms are constants ([details](docs/ARCHITECTURE.md)) |
| DLSS 5 Neural Rendering | Off by default. Needs `nvngx_dlssnr.dll`; driven through NGX directly, since Streamline publishes no interface for it |
| External neural modules | Runs DLLs from `Data/SKSE/Plugins/SkyrimUpscaler/Neural/` once per frame on the scene colour ([ABI](docs/NEURAL_MODULES.md)) |
| ENB compatibility | The upscaler runs at the point the scene is finished and before the game's post-processing reads it, so ENB tonemaps and grades the upscaled image as it would the game's own ([details](docs/ARCHITECTURE.md)) |
| In-game settings | SKSE Menu Framework 3; saved to `SkyrimUpscaler.ini` |

## Requirements

- Skyrim Special Edition **1.5.97** or Anniversary Edition **1.6.x**
  (VR and GOG are out of scope)
- [SKSE64](https://skse.silverlock.org/)
- [SKSE Menu Framework](https://www.nexusmods.com/skyrimspecialedition/mods/120352)
- An NVIDIA RTX GPU for the DLSS features; FSR runs anywhere
- **Community Shaders must not be installed** — see the warning above
- The NVIDIA Streamline and AMD FidelityFX runtimes (see Installing)

## Installing

1. Install the plugin so that `SkyrimUpscaler.dll` lands in
   `Data/SKSE/Plugins/`.
2. Place the Streamline runtime DLLs in
   `Data/SKSE/Plugins/SkyrimUpscaler/Streamline/`:
   `sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll`, `sl.dlss_g.dll`,
   `sl.dlss_d.dll`, `sl.nis.dll`, `sl.reflex.dll`, `sl.pcl.dll`, plus the
   matching `nvngx_*.dll` model files.
3. For DLSS 5 Neural Rendering, add `sl.dlss_nr.dll` and `nvngx_dlssnr.dll`.
   On RTX 20/30/40 the stock `nvngx_dlssnr.dll` will not run — its CUDA payload
   targets Blackwell — and a patched build is required. The
   **[RenoDX Discord](https://discord.com/invite/renodx)** is where that work
   happens and where those DLLs are shared; this project would not have a
   working Neural Rendering path without them. A quick way to tell the two
   apart: search the DLL for the string `sm_120`. If it is the only
   architecture present, the file is Blackwell-only.
4. Place the AMD FidelityFX DLLs next to `SkyrimUpscaler.dll` for the FSR path.

Runtime DLLs are not distributed here. The plugin logs each one it looks for as
`found` or `missing` at startup, so a missing file is easy to spot.

## Configuring

Everything is under **SkyrimUpscaler** in the Mod Control Panel; settings are
written to `Data/SKSE/Plugins/SkyrimUpscaler/SkyrimUpscaler.ini` when the panel
closes. Frame generation and Neural Rendering both default to **off**.

## Building

See [docs/BUILDING.md](docs/BUILDING.md). Short version:

```
git clone --recurse-submodules <this repo>
xmake build SkyrimUpscaler
```

## Documentation

- [docs/FAQ.md](docs/FAQ.md) — **start here if you are installing this, not building it**
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — how the renderer interposition works, and why
- [docs/BUILDING.md](docs/BUILDING.md) — toolchain, dependencies, targets
- [docs/NEURAL_MODULES.md](docs/NEURAL_MODULES.md) — writing an external neural rendering DLL
- [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) — reading the log, known limitations
- [docs/SAFETY.md](docs/SAFETY.md) — frame generation and the driver
- [CREDITS.md](CREDITS.md) — upstream projects and licences

## Licence

GPL-3.0. Third-party components keep their own licences; see
[CREDITS.md](CREDITS.md).
