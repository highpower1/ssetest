# Credits

## Derived from

- **[jarari/fo4test](https://github.com/jarari/fo4test)** — the Fallout 4
  upscaler this project is a port of. The D3D12 proxy swapchain, present
  override, Streamline and FidelityFX wrappers, and the DLSS 5 Neural Rendering
  backend all originate there. Its `test/dlss-nr` branch reconstructed the
  DLSS-NR ABI that NVIDIA has not published.

## Thanks

- **[RenoDX](https://discord.com/invite/renodx)** — for the DLSS runtime DLLs
  that made DLSS 5 Neural Rendering possible here at all, and for the community
  work that got the feature running on hardware NVIDIA had not enabled it for.
  Their ReShade addon was also the reference that revealed what this
  implementation was still missing: the explicit jitter parameters, the UI
  correction buffers, and the colour encoding and diffuse-white reference the
  uplift model expects.

## Bundled third-party source

- **[RTX40MFG-Unlock](https://github.com/dashdogy/RTX40MFG-Unlock)** by Michael
  Robles (MIT) — `src/third_party/RTX40MFGUnlock/`. Patches the DLSS-G provider
  policy that caps Ada at 2x frame generation.

## Dependencies

- **[CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG)** —
  unified SE/AE reverse-engineered game interface
- **[SKSE64](https://skse.silverlock.org/)**
- **[SKSE Menu Framework](https://www.nexusmods.com/skyrimspecialedition/mods/120352)** — in-game settings UI
- **[NVIDIA Streamline](https://github.com/NVIDIA-RTX/Streamline)** — DLSS, DLSS-G, Ray Reconstruction, Reflex, PCL, NIS
- **[AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)** — FSR
- **Microsoft Detours**, **DirectXTK**, **magic_enum**, **SimpleIni**

## Reference

- **[PureDark/Skyrim-Upscaler](https://github.com/PureDark/Skyrim-Upscaler)** —
  the public 2022 repository was used to identify engine hook points, the
  `BSGraphics::State` layout and the dynamic-resolution offsets. Its D3D11
  inline approach was not used: it predates ENB support and modern DLSS is
  D3D12-only. Every offset taken from it was verified against the live runtime
  before use.
- **ENBSeries** by Boris Vorontsov — the present path is built to coexist with
  it.

## Licence

This project is GPL-3.0. Bundled third-party sources keep their own licences;
`src/third_party/RTX40MFGUnlock/LICENSE` is included verbatim.
