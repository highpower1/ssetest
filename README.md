# SkyrimUpscaler

An SKSE plugin that brings NVIDIA DLSS, DLSS Frame Generation, DLSS Ray
Reconstruction, DLSS 5 Neural Rendering and AMD FSR to Skyrim Special Edition
and Anniversary Edition — alongside ENB, configured in-game.

Ported from [jarari/fo4test](https://github.com/jarari/fo4test), the Fallout 4
upscaler this project is derived from.

> **Status: working, but experimental.** Every feature below has been validated
> in-game on an RTX 4070 Ti running AE 1.6.1170 with ENB. It has not been tested
> broadly, and frame generation in particular drives the GPU hard enough that a
> bad configuration used to hang the driver. Read
> [docs/SAFETY.md](docs/SAFETY.md) before enabling it.

## Features

| Feature | Notes |
| --- | --- |
| DLSS Super Resolution | DLAA through Ultra Performance |
| AMD FSR | For non-RTX hardware |
| DLSS Frame Generation | Guarded by a present watchdog |
| RTX 40 multi-frame generation | Via [RTX40MFG-Unlock](https://github.com/dashdogy/RTX40MFG-Unlock); Ada is otherwise capped at 2x |
| DLSS Ray Reconstruction | Guides are synthesised — see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) |
| DLSS 5 Neural Rendering | Driven directly through NGX; Streamline has no contract for it |
| External neural modules | Load your own DLL — see [docs/NEURAL_MODULES.md](docs/NEURAL_MODULES.md) |
| ENB compatibility | The final image is composited rather than fought over |
| In-game settings | SKSE Menu Framework 3 |

## Requirements

- Skyrim Special Edition **1.5.97** or Anniversary Edition **1.6.x**
  (VR and GOG are out of scope)
- [SKSE64](https://skse.silverlock.org/)
- [SKSE Menu Framework](https://www.nexusmods.com/skyrimspecialedition/mods/120352)
- An NVIDIA RTX GPU for the DLSS features; FSR runs anywhere
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
   targets Blackwell — and a patched build is required.
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

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — how the renderer interposition works, and why
- [docs/BUILDING.md](docs/BUILDING.md) — toolchain, dependencies, targets
- [docs/NEURAL_MODULES.md](docs/NEURAL_MODULES.md) — writing an external neural rendering DLL
- [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) — reading the log, known limitations
- [docs/SAFETY.md](docs/SAFETY.md) — frame generation and the driver
- [CREDITS.md](CREDITS.md) — upstream projects and licences

## Licence

GPL-3.0. Third-party components keep their own licences; see
[CREDITS.md](CREDITS.md).
