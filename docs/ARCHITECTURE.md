# Architecture

Skyrim is a D3D11 game. Modern DLSS is D3D12-only. ENB owns the final present.
Almost every design decision here follows from those three facts.

## The shape of a frame

```
game renders scene (D3D11, at render resolution)
        |
        v
MainDrawWorld pre-UI hook
        |-- external neural modules run here (D3D11, hud-less scene)
        |-- scene / motion / depth copied into D3D11<->D3D12 shared textures
        |
        v  (private D3D12 device, own command queue)
   DLSS 5 Neural Rendering "uplift"  [optional, before the upscaler]
        |
   DLSS SR  or  DLSS Ray Reconstruction  or  FSR
        |
   DLSS 5 Neural Rendering "uplift"  [optional, after the upscaler — the default]
        |
        v
present override: the result stays D3D12-native
        |
kMAIN is cleared to black, so the game draws UI onto black
        |
        v
D3D12 proxy swapchain Present
        |-- D3D12UIComposite merges the D3D12 scene with the UI-on-black buffer
        |-- DLSS-G tags its inputs and inserts generated frames
        v
                screen
```

## Why a D3D12 proxy swapchain

DLSS Super Resolution is compute work; it can be done on a side D3D12 device and
copied back into the game's colour target. Frame generation cannot: it has to
*insert* frames at Present, and Present belongs to ENB.

`Render/DX11Hooks.cpp` therefore hooks `IDXGIFactory::CreateSwapChain` (vtable
slot 10) and hands back a D3D12/Streamline swapchain. ENB keeps drawing through
what it believes is its own swapchain, while Streamline gets a coherent D3D12
present it can pace.

## Why present-override rather than copying back

Copying the upscaled image back into `kMAIN` means it makes another round trip
through D3D11 and ENB's post chain before reaching the screen, and DLSS-G then
cannot pace a present it does not own. Every attempt to run frame generation
that way ended in a GPU hang.

Instead the upscaled image stays on D3D12 and is registered as the present
override. `kMAIN` is cleared to black so the game's own UI pass draws onto
black, and `D3D12UIComposite` merges the two at present time: a pixel that is
not black in the UI buffer is UI, everything else is the scene.

"Not black in the UI buffer" was the original rule and it was wrong. That
buffer holds ENB's finished frame as well, so brightness classified ENB's own
output -- bloom, light sprites, torch flames -- as UI and composited it over the
upscaled scene in proportion to how bright it was. The visible result was that
the neural uplift appeared to stop working wherever ENB light fell, and that
fire looked doubled: the upscaled flame crossfaded with ENB's non-upscaled one.

The mask is now derived instead of guessed. `DX12SwapChain::CaptureUIBaseline`
snapshots the presented buffer at the pre-UI hook, after the world and ENB have
written it and before the UI is drawn, and the composite takes a pixel as UI
only where the final buffer differs from that capture. ENB's output appears in
both and cancels. The capture is recorded on the immediate context the present
already copies on, so the fence the present signals orders it against the D3D12
read without extra synchronisation.

`UIMaskMode` selects it (`2`, the default); `0` and `1` are the older brightness
heuristics, kept because the difference mask is only correct while the capture
lands at the right point in the frame. The `Pre-UI capture` debug view shows
what was snapshotted so that stays checkable.

## Where ENB sits, and the grade transfer

A magenta sweep over the engine's render targets found the one ENB's output
lands in: `kFRAMEBUFFER`, which under the proxy is the buffer `GetBuffer` hands
out. Magenta written there at the pre-UI hook survived to the screen, so nothing
writes the scene into it after that point. The frame is therefore:

1. the game renders the scene into `kMAIN` (HDR, ungraded)
2. **ENB post-processes and writes `kFRAMEBUFFER`**
3. our pre-UI hook reads `kMAIN` and upscales
4. the UI is drawn into `kFRAMEBUFFER`
5. present

ENB is upstream of us. That is why writing into `kMAIN` reaches nothing -- ENB
has already read it -- and why the scene we upscale is the ungraded one. It also
means `uiBaseline`, the capture the UI mask subtracts, is ENB's finished frame.

Both images are consequently bound in the composite at once, and
`ENBGradeTransfer` uses that: the low frequencies of ENB's frame divided by the
low frequencies of ours, applied to ours as a ratio. Tonemapping and colour
grading live in those low frequencies and come across; the detail DLSS and the
neural uplift produced is high-frequency and survives the multiply. Effects that
are purely high-frequency do not transfer, and bloom transfers only as far as
`ENBGradeRadius` reaches.

This is a workaround for the hook's position, not a fix for it. The fix is to
run the upscaler before step 2, so ENB grades our image instead of the game's;
that needs a hook site inside `Main_DrawWorld` earlier than the one PureDark's
offsets provide, and has not been attempted.

`PresentOverride = 0` is not a way out. It restores the older path -- copy the
result into `kMAIN` and let the game present normally -- but with ENB installed
that path is dead: **ENB's chain overwrites `kMAIN` after we write it, so
nothing this mod renders reaches the screen at all.** This was measured, not
reasoned about. With `DLSSNRDebugBypass = 1` the uplift target is cleared to
flat magenta; on the present-override path the screen turns magenta, and on the
copy-back path under ENB it does not change. Not the uplift, not sharpening,
not the upscaler's own output.

So under ENB there is currently exactly one working output path, and its known
defect is the composite's inability to tell ENB's post-processing from UI.
Fixing that -- giving the composite a real UI mask instead of a luminance
heuristic -- is the open problem, not choosing between the two paths.

> The composite's coverage term must be derived from colour alone. Folding in
> the UI buffer's alpha discards the entire scene: that buffer is a copy of an
> opaque backbuffer, so its alpha is 1 at every pixel, coverage saturates, and
> the shader returns the UI buffer everywhere. Nothing in the log changes when
> this happens.

## Engine interposition points

Three call sites, resolved through Address Library so one DLL covers SE and AE:

| Hook | RelocationID | Purpose |
| --- | --- | --- |
| `Init_InitD3D` | `(75595, 77226)` + `0x2BC` | renderer is up; stand up D3D12 |
| `UpdateJitter` | `(75460, 77245)` + `0xE2` | per-frame camera, jitter, dynamic resolution |
| `MainDraw_PreUI` | `(79947, 82084)` + `0x17A` | run the upscaler before the HUD |

Two additional patches NOP the engine's own jitter handling so the projection
carries our Halton offset even with in-game TAA off:
`(75709, 77518) + 0xE/0x11` and `(75711, 77520) + 0x1D5`.

Render resolution is driven through the engine's own dynamic-resolution scale
(`BSGraphics::State` RUNTIME_DATA `0x58`/`0x60`, `+0xA4`/`+0xA8`), and reset to
1.0 after the upscaler resolves so downstream passes see a full-resolution
frame.

## Ray Reconstruction on a game with no G-buffer

DLSS-RR is a ray-tracing denoiser and wants albedo, roughness, normals and
specular albedo. Skyrim is forward rendered and has none of them —
`kNORMAL_TAAMASK_SSRMASK` holds normals in an undocumented packing mixed with
the TAA and SSR masks.

`Render/D3D12NeuralGBuffer.cpp` synthesises the guides instead:

- **Normals** are reconstructed from `kSAO_CAMERAZ`, the linear view-space depth
  the upscaler already feeds DLSS, by differencing neighbouring view-space
  positions. Each gradient takes the nearer of its two neighbours so object
  silhouettes do not produce normals pointing at nothing. Geometric rather than
  shading normals, but exact — no guessing at an engine encoding.
- **Roughness** is 1.0. A raster frame has no separable specular signal, so
  fully rough is the only defensible value.
- **Albedo** is white, so RR's demodulate/re-modulate round trip is the identity
  and cannot tint the image.
- **Specular albedo** is black: no specular lobe to reproject.

With those constants RR reduces to denoising the colour directly: the
ray-tracing machinery has nothing to act on, and what remains in use is the
transformer model.

Super Resolution and Ray Reconstruction cannot share a Streamline viewport, so
each options path turns the other off when it takes over.

## DLSS 5 Neural Rendering

Not reachable through Streamline. `slInit` must announce
`sl::kSDKVersionDLSSNRPreview` — the stock `kSDKVersion` is what makes the
plugin declare itself out of date — and even then Streamline rejects the preview
plugin on most driver and runtime combinations.

`Neural/nvngx_dlss_nr_private.cpp` loads `nvngx_dlssnr.dll` itself and calls NGX
with feature id **18**, application id **141959980**, SDK version **21**, and a
hook that spoofs the calling module's name.

The uplift runs *after* the upscaler by default. Before it, the temporal resolve
averages the added detail straight back out — it costs frame time and shows
almost nothing. Running after means the guides must match the resolved image, so
a resample pass lifts motion and depth to display resolution and reads each
output pixel at the raster position where its feature actually landed, offset by
the negation of the engine jitter.

## Frame generation safety

`DX12SwapChain` times each Present. Two consecutive Present *calls* over 80 ms —
the pre-TDR signature — or an outright failed present latches frame generation
off for the session.

A long gap *between* presents does not count: a menu, a loading screen, an
alt-tab or a CPU hitch all produce one while Present itself returns in about a
millisecond. Judging on the gap once disabled frame generation for a whole
session on a 6.8-second loading pause.

## Source layout

```
src/
  main.cpp                     SKSE entry, ENB probe, messaging
  Game/                        renderer access, camera, math, Address Library glue
  Hooks/UpscalerHooks.cpp      the three engine hooks
  Upscaler/
    D3D12Upscaler.cpp          per-frame orchestration: NR, SR/RR/FSR, present override
    Upscaling.cpp              method selection, menu gating, settings mirror
  Render/
    DX11Hooks.cpp              factory hook, device capture
    DX12SwapChain.cpp          proxy swapchain, present, DLSS-G, watchdog
    Streamline.cpp             Streamline wrapper: DLSS, DLSS-G, RR, Reflex, PCL
    FidelityFX.cpp             FSR
    D3D12NeuralGBuffer.cpp     RR guides + uplift guide resampling
    D3D12UIComposite.cpp       scene + UI composite at present
    OSD.cpp                    on-screen display
  Neural/
    NeuralRendering.cpp        external module discovery and dispatch
    nvngx_dlss_nr_private.cpp  direct NGX DLSS-NR backend
  Settings/, UI/               ini persistence, SKSE Menu Framework 3 page
  third_party/RTX40MFGUnlock/  Ada multi-frame-generation unlock (MIT)
```
