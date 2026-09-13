# Credits

This project is a port. Almost none of the hard problems it solves were solved
here first, and the names below are the people who solved them. Where a name
could be verified from the source, the licence header, or the project's own
credits, it is given; where a project is the work of a company or a team we
could not enumerate, it is credited as such rather than guessed at.

## Derived from

- **[jarari/fo4test](https://github.com/jarari/fo4test)** by **jarari** — the
  Fallout 4 upscaler this project is a port of. The D3D12 proxy swapchain, the
  present override, the Streamline and FidelityFX wrappers, and the DLSS 5
  Neural Rendering backend all originate there. Its `test/dlss-nr` branch
  reconstructed the DLSS-NR ABI that NVIDIA has not published, which is the
  single piece of work without which the Neural Rendering path in this repo
  would not exist.

## Thanks

- **[RenoDX](https://discord.com/invite/renodx)** and its community — for the
  DLSS runtime DLLs that made DLSS 5 Neural Rendering possible here at all, and
  for the work that got the feature running on hardware NVIDIA had not enabled
  it for. Their ReShade addon was also the reference that revealed what this
  implementation was still missing: the explicit jitter parameters, the UI
  correction buffers, and the colour encoding and diffuse-white reference the
  uplift model expects.
- **Michael Robles ([dashdogy](https://github.com/dashdogy))** — for
  [RTX40MFG-Unlock](https://github.com/dashdogy/RTX40MFG-Unlock), and for
  publishing it under a licence that let it be vendored rather than
  reimplemented.
- **[PureDark](https://github.com/PureDark)** — for
  [Skyrim-Upscaler](https://github.com/PureDark/Skyrim-Upscaler), which is why
  anyone knew where the engine's hook points were.
- **Boris Vorontsov** — for **ENBSeries**. The present path in this repo is
  shaped almost entirely by the requirement to coexist with it.

## Bundled third-party source

- **[RTX40MFG-Unlock](https://github.com/dashdogy/RTX40MFG-Unlock)** by
  **Michael Robles** (MIT) — `src/third_party/RTX40MFGUnlock/`. Patches the
  DLSS-G provider policy that caps Ada at 2x frame generation. The licence is
  included verbatim at `src/third_party/RTX40MFGUnlock/LICENSE`.

## Dependencies

- **[CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG)** — the
  reverse-engineered game interface, and the reason one DLL can serve SE and AE.
  Original CommonLibSSE by **Ryan McKenzie
  ([Ryan-rsm-McKenzie](https://github.com/Ryan-rsm-McKenzie))**; the NG fork by
  **[CharmedBaryon](https://github.com/CharmedBaryon)**, maintained with
  **[alandtse](https://github.com/alandtse)** and its contributors. (MIT)
- **[SKSE64](https://skse.silverlock.org/)** by **ianpatt**, **behippo** and
  **purple lunchbox** — the extender everything here is built on top of.
- **[SKSE Menu Framework 3](https://github.com/QTR-Modding/SKSE-Menu-Framework-3)**
  by **[Thiago099](https://github.com/Thiago099)** — the in-game settings UI.
  Per the project's own credits it was **Quantumyilmaz**'s idea and is based on
  their template, its ImGui code comes by way of **Mod Function Menu** and
  **po3**'s PhotoMode and Dialogue History, **blnkxin** made non-Latin character
  ranges work, and **alandtse** added VR support.
- **[Dear ImGui](https://github.com/ocornut/imgui)** by **Omar Cornut** and its
  contributors — every control in this mod's menu is one of theirs, reached
  through the Menu Framework's wrapper. (MIT)
- **[NVIDIA Streamline](https://github.com/NVIDIA-RTX/Streamline)** and the
  **NGX / DLSS SDK** — **NVIDIA Corporation**. DLSS, DLSS-G, Ray Reconstruction,
  Neural Rendering, Reflex, PCL and NIS. Used under the NVIDIA RTX SDKs licence
  (`extern/Streamline/external/ngx-sdk/license.txt`).
- **[AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)**
  — **Advanced Micro Devices, Inc.** and the GPUOpen team, for FSR and for
  shipping it open.
- **[Detours (Nukem9 fork)](https://github.com/Nukem9/detours)** by **Nukem**
  (MIT) — `include/detours/`, the hooking library.
- **[DirectXTK](https://github.com/microsoft/DirectXTK)** and
  **[DirectX-Headers](https://github.com/microsoft/DirectX-Headers)** —
  **Microsoft Corporation**, maintained in the open by **Chuck Walbourn**. (MIT)
- **[magic_enum](https://github.com/Neargye/magic_enum)** by **Daniil
  Goncharov ([Neargye](https://github.com/Neargye))**, with **Bela Schaum**.
  (MIT)
- **[SimpleIni](https://github.com/brofield/simpleini)** by **Brodie Thiesfield
  ([brofield](https://github.com/brofield))** — every setting in this mod is
  read and written through it. (MIT)
- **[spdlog](https://github.com/gabime/spdlog)** by **Gabi Melman
  ([gabime](https://github.com/gabime))** and contributors — the logging that
  made almost every bug in this port findable. (MIT)
- **[xmake](https://github.com/xmake-io/xmake)** by **Ruki Wang
  ([waruqi](https://github.com/waruqi))** and contributors — the build system.
  (Apache-2.0)

## Reference

- **[PureDark/Skyrim-Upscaler](https://github.com/PureDark/Skyrim-Upscaler)** —
  the public 2022 repository was used to identify engine hook points, the
  `BSGraphics::State` layout and the dynamic-resolution offsets. Its D3D11
  inline approach was not used: it predates ENB support and modern DLSS is
  D3D12-only. Every offset taken from it was verified against the live runtime
  before use.
- **ENBSeries** by **Boris Vorontsov** — the present path is built to coexist
  with it.

## Corrections

If you are named here and the attribution is wrong, incomplete, or you would
rather not be listed, open an issue and it will be fixed.

## Licence

This project is GPL-3.0. Bundled third-party sources keep their own licences.
