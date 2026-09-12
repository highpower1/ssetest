# SkyrimUpscaler — Handoff Spec (for Codex / next engineer)

Reverse‑engineered port of `github.com/highpower1/fo4test` (a Fallout 4 FSR/DLSS/DLSS‑G
upscaler) into an **SKSE plugin for Skyrim Special Edition / Anniversary Edition**.
This document is a self‑contained brief so you can continue **without** re‑deriving the
architecture. Read it top to bottom once.

---

## 1. Goal & scope

Deliver one SKSE DLL that gives Skyrim:

- **FSR** (AMD FidelityFX Super Resolution) — ✅ working
- **DLSS** super resolution (DLAA / Quality / Balanced / Performance / Ultra) — ✅ working
- **DLSS‑G frame generation** — ⚠️ **plumbed but freezes (GPU hang); THE open problem**
- **Neural Rendering** = DLSS Ray Reconstruction + an external RenoDX‑style DLL loader — ⬜ not started (RR model dll unsupported on the test runtime; loader stubbed)
- **ENB support** — ✅ working (this is the hard constraint that shapes everything)
- **UI** via SKSE Menu Framework 3 — ✅ working

Target runtimes: **Steam Skyrim SE 1.5.97 + AE 1.6.x** only. VR and GOG are **out of scope**.
CommonLibSSE‑NG resolves per‑runtime addresses, so one DLL covers SE and AE.

Test machine used throughout: **RTX 4070 Ti, AE 1.6.1170, ENB installed, 1920×1080**.

---

## 2. Current status (feature matrix)

| Feature | State | Notes |
|---|---|---|
| Build (VS2026 + xmake) | ✅ | clean, ~4 s incremental |
| SKSE load + SMF3 menu | ✅ | live settings, saved to INI |
| D3D12 proxy swapchain + ENB coexist | ✅ | the frame‑gen foundation |
| DLSS super‑resolution (DLAA..UltraPerf) | ✅ | menu‑switchable, ENB‑safe |
| FSR super‑resolution | ✅ | menu‑switchable |
| **present‑override** (DLSS output stays D3D12‑native, `D3D12UIComposite` merges UI) | ✅ | **validated in‑game, ENB look preserved** |
| **DLSS‑G frame generation** | ⚠️ | activates, then **GPU DEVICE_HUNG ~15 s later** (freeze). See §7. |
| Neural (RR + RenoDX loader) | ⬜ | not started |

**The one remaining problem is DLSS‑G freezing.** Everything else is a shippable
FSR+DLSS+ENB+UI upscaler.

---

## 3. Build & test workflow

- IDE/toolchain: **Visual Studio 2026 Community (v18, MSVC 14.51)** + **xmake 3.1.1**.
- Build must run inside a `vcvars64` environment. A ready script exists; equivalent to:
  ```
  call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
  cd <repo>
  "C:\Program Files\xmake\xmake.exe" build -y SkyrimUpscaler
  ```
- Output DLL: `build/windows/x64/releasedbg/SkyrimUpscaler.dll`.
- The `commonlibsse-ng.plugin` xmake rule **auto‑generates** `SKSEPlugin_Version` /
  `SKSEPlugin_Query` — do **not** hand‑write them (only `SKSEPlugin_Load` is ours in `src/main.cpp`).
- Test loop: build DLL → user installs it (mod‑manager VFS; the game dir has no direct
  `data/SKSE/Plugins`) → user runs → reads `<Documents>/My Games/Skyrim Special Edition/SKSE/SkyrimUpscaler.log`.
- Runtime deps the user must have in `Data/SKSE/Plugins/SkyrimUpscaler/`: the **Streamline 2.13**
  DLLs (incl. `nvngx_dlss.dll`, `nvngx_dlssg.dll`, `sl.*`) and the **FidelityFX** runtime DLLs.
- **Crash logs**: Crash Logger SSE writes to `<Documents>/My Games/Skyrim Special Edition/SKSE/Crashlogs/`.
  NB: DLSS‑G failures are **GPU hangs (TDR / DEVICE_REMOVED), not CPU CTDs** — they show as a
  freeze + `DXGI_ERROR_DEVICE_HUNG` in `SkyrimUpscaler.log`, and Streamline writes a minidump to
  `C:\ProgramData\NVIDIA\Streamline\`.

Third‑party SDKs are **git‑ignored** (placed manually — see `PORTING.md`):
`extern/Streamline`, `extern/FidelityFX-SDK`; `lib/commonlibsse-ng` is a submodule.

---

## 4. Reference clones (read these for ground truth)

- `C:\Claude\fo4test` — **the working FO4 original.** When in doubt, match it.
- `C:\Claude\pd-upscaler` — PureDark Skyrim‑Upscaler (2022, no ENB). Source of the verified
  Skyrim hook points + jitter/DRS offsets. Verified on this runtime; do NOT copy blindly.
- `C:\Claude\Streamline-sdk`, `C:\Claude\FidelityFX-SDK`, `C:\Claude\CommonLibSSE-NG`.
- `src/Game/Upscaling.cpp.fo4ref`, `src/Game/Upscaling.h.fo4ref` — the FO4 Upscaling GPU body,
  kept verbatim as a reference alongside our (partial) Skyrim port.

---

## 5. Architecture

Two provably‑different pieces cooperate:

### 5a. The upscaler (`src/Upscaler/D3D12Upscaler.{h,cpp}`)
A private **D3D12 device shared with the proxy swapchain** (`kFrameGenExperiment` reuses
`DX12SwapChain::d3d12Device`). Per frame, at the verified `Main::DrawWorld` pre‑UI hook:
1. D3D11 `CopyResource` the game's `kMAIN` (color), `kMOTION_VECTOR`, `kSAO_CAMERAZ` (R32 linear
   depth) into D3D11↔D3D12 **shared textures** (NT‑handle shared, cross‑device fence sync).
2. Run **DLSS (Streamline, D3D12)** or **FSR (FidelityFX, D3D12)** on the shared textures →
   `colorOutput12` (full‑res).
3. **present‑override** (`kPresentOverride=true`): keep `colorOutput12` on D3D12 as the final
   scene, **clear `kMAIN` to black** so the game draws **UI‑only** onto black, and call
   `DX12SwapChain::SetPresentOverride(colorOutput12)`. (Old path: copy the result back to `kMAIN`
   in D3D11 — still selectable via `kPresentOverride=false`.)

Jitter: Halton(2,3) injected into `BSGraphics::State::jitter[0/1]` at offset **0x44** (version
independent) each frame; two NOP patches force the engine to always apply it (see
`src/Hooks/UpscalerHooks.cpp`). DRS: writes `fDynamicResolutionCurrentWidthScale/Height` at
RUNTIME_DATA(0x58/0x60)+0xA4/0xA8; the upscaler resolves the low‑res sub‑rect to full res and
`ResetDynamicResolution()` restores the ratio so downstream passes are full‑res. Menu gating
(`Upscaling::ShouldBlockUpscaling` via `MenuOpenCloseEvent`) disables the upscaler on
logos/menus/loading so those aren't warped.

### 5b. The D3D12 proxy swapchain (`src/Render/DX12SwapChain.{h,cpp}`, `src/Render/DX11Hooks.cpp`)
`src/Render/DX11Hooks.cpp` (compiled in place of the minimal `src/Hooks/DX11Hooks.cpp` when
`kFrameGenExperiment`) IAT‑hooks `D3D11CreateDeviceAndSwapChain` and vtable‑hooks
`IDXGIFactory::CreateSwapChain` (slot 10) to replace the game's swapchain with a
`DXGISwapChainProxy` wrapping a **Streamline‑managed D3D12 swapchain**. ENB renders into a
D3D11 shared texture (`swapChainBufferProxyENB`, returned from `GetBuffer(0)`); on `Present`,
the proxy copies that into the D3D12 swapchain and calls `swapChain->Present`, which is where
Streamline runs DLSS‑G. The proxy present is where present markers / DLSS‑G tagging happen.

**Key: with present‑override, the final present color is the D3D12‑native DLSS output composited
with UI via `D3D12UIComposite` (base = DLSS scene, postUI = UI‑on‑black), giving DLSS‑G a
coherent D3D12 present timeline — this is exactly how fo4test does it.**

### 5c. Compile‑time toggles (all in `src/Upscaler/…`)
- `Upscaling::kFrameGenExperiment` (`Upscaling.h`) — install the proxy swapchain path. **true.**
- `D3D12Upscaler.cpp` `kPresentOverride` — present‑override vs kMAIN round‑trip. **true** (validated).
- `D3D12Upscaler.cpp` `kEnableDLSSG` — enable DLSS‑G frame generation. **true**, but this is what
  freezes; set **false** to ship a stable FSR+DLSS+ENB build.

---

## 6. Verified engine hook points (AE ids shown; SE via `REL::Relocate`)

All confirmed on 1.6.1170 (each site holds a `call` 0xE8) — see `src/Hooks/UpscalerHooks.cpp`:
- `InitD3D`      — `RelocationID(75595, 77226) + Relocate(0x50, 0x2BC)` → stand up the upscaler.
- `UpdateJitter` — `RelocationID(75460, 77245) + Relocate(0xE5, 0xE2, 0x104)` → per‑frame; capture
  camera, inject jitter, write DRS scale.
- `MainDraw_PreUI` — `RelocationID(79947, 82084) + Relocate(0x16F, 0x17A, 0x132)` → run the upscaler eval.
- jitter‑always‑on NOP patches — `RelocationID(75709, 77518)+Relocate(0xE,0x11)` (6 bytes) and
  `RelocationID(75711, 77520)+0x1D5` (10 bytes).
- Engine‑TAA flag (disable engine TAA while upscaling): global `REL::VariantID(527731, 414660, 0x34234C0)`,
  inner ptr at +0x1F0, `bTAA` at +0x18.
- **Do NOT** install `DX12SwapChain::InstallWndProcHook` — it fights SKSE Menu Framework's WndProc
  hook and access‑violation‑CTDs before the first frame (already commented out).

Confirmed render targets (native res): `kMAIN`(1)=R16G16B16A16, `kMOTION_VECTOR`(7)=R16G16,
`kSAO_CAMERAZ`(49)=R32_FLOAT (linear cam‑Z). Depth SRV also at `depthStencils[4].depthSRV`.

---

## 7. THE OPEN PROBLEM — DLSS‑G frame generation freezes (GPU hang)

### What happens
With `Frame Generation = On`, DLSS + method DLSS: logs show
`[D3D12Upscaler] DLSS-G ENABLED mode=1 framesToGen=1`, present switches to `sync 1->0`, runs a few
seconds, then the GPU hangs (screen + cursor freeze, audio stutters) and `SkyrimUpscaler.log` ends
with:
```
[Streamline SDK] dlss_gEntry.cpp slGetData] slDLSSGGetState must be synchronized with the present thread ...
[Streamline SDK] dlfgPresent.cpp presentCommon] Frame rate over 100.00ms, reseting frame timer
[Streamline SDK] d3d12.cpp cloneResource] Unable to clone resource (nv.sl.dlss_g.clone.dlfg-output_0:1920:1080:R8G8B8A8) - device removed
[DX12SwapChain] Present failed result=0x887A0005 (DXGI_ERROR_DEVICE_REMOVED) d3d12Removed=0x887A0006 (DXGI_ERROR_DEVICE_HUNG)
```
i.e. **the hang is inside Streamline's own DLSS‑G present/frame‑pacing** (`dlfgPresent` /
`cloneResource` of its own `dlfg-output`), preceded by a frame‑time spike >100 ms.

### What was tried (all still hang ~15 s in)
1. Side D3D12 queue + tag the live shared textures.
2. Upscaler sharing the proxy command queue.
3. hud‑less = final ENB backbuffer (R8G8B8A8), UI‑recomposition off.
4. **fo4test‑exact:** per‑backbuffer‑index double‑buffered `dlssgHudless/Mvec/Depth[kDX12FrameCount]`,
   copied on the present queue after a **cross‑queue fence wait** on the upscaler's `GetWorkFence()`.
5. **present‑override foundation** (§5b) + hud‑less = D3D12‑native DLSS output + UI‑recomposition on.

Attempt 5's **foundation works** (upscaling + ENB via present‑override validated, no hang) but
**enabling DLSS‑G on top still hangs the same way.** So the failure is now isolated to
**DLSS‑G's inputs (motion/depth) and/or the present cadence**, not the color/present path.

### Leading hypotheses / next steps for you
1. **Motion‑vector correctness for DLSS‑G.** DLSS‑SR tolerates our `kMOTION_VECTOR` (no ghosting),
   but DLSS‑G's optical flow is stricter. Verify the MV convention/scale/units DLSS‑G expects vs
   what we tag (`mvecScale`, `motionVectorsDilated/Jittered` in `Streamline::UpdateConstants`), and
   the extents (`lowResExtent` render‑res vs full). A runaway/NaN MV can TDR the flow pass.
2. **The `slDLSSGGetState must be synchronized with the present thread` warning is persistent.**
   `QueryDLSSGState` is being called off the present thread or at the wrong time. Compare our
   present flow to fo4test's exactly (`OnPresentStart/End`, `SetPresentFrameIndex`, `QueryDLSSGState`
   ordering, and which thread). fo4test may query it only on the present thread with the right fence.
3. **Present cadence / flip queue.** The `Frame rate over 100 ms, reseting frame timer` +
   earlier `throttleFlipQueue: no flip-queue slot freed within 30 ms` suggest DLSS‑G's flip queue
   backs up. Check `numBackBuffers`, present flags, and that our proxy `Present` drains/pace‑matches
   what DLSS‑G expects (fo4test adjusts sync 1→0 too, but its present body differs — diff it).
4. **Reflex frame‑token pipeline.** DLSS‑G needs Reflex markers + `slReflexSleep` with **one
   consistent frame token** across sim→render→present. We route everything through the game
   `frameCount` token (`SetPresentFrameIndex(frameCount)`, `TagDLSSGResources(..., frameCount, ...)`).
   Re‑verify this matches fo4test's `dlssgInputFrameTokenIndices[frameIndex]` scheme (it stores the
   token captured at eval time, per backbuffer index — we currently recompute from frameCount).
5. **Get a GPU capture.** Because the hang is inside Streamline, a PIX/Nsight/Aftermath capture of
   the hanging present is likely the fastest way to see the faulting resource/state.

**Fallback:** set `kEnableDLSSG=false` to ship the stable FSR+DLSS+ENB+UI build immediately.

---

## 8. Key files

```
src/main.cpp                       SKSE entry, ENB probe, hook install order
src/Hooks/UpscalerHooks.cpp        verified engine hooks: InitD3D / UpdateJitter(jitter+DRS) / MainDraw(eval) + NOP patches
src/Hooks/DX11Hooks.cpp            minimal device-capture hook (used when NOT kFrameGenExperiment)
src/Render/DX11Hooks.cpp           full hook: device capture + IDXGIFactory::CreateSwapChain -> D3D12 proxy (kFrameGenExperiment)
src/Render/DX12SwapChain.cpp       proxy swapchain, Present, present-override composite, DLSS-G per-index buffers + tagging
src/Render/Streamline.cpp          Streamline wrapper: DLSS eval, UpdateConstants, UpdateDLSSG, TagDLSSGResources, Reflex/PCL
src/Render/FidelityFX.cpp          FSR (FidelityFX) D3D12 eval + FSR3 frame-gen swapchain
src/Render/D3D12UIComposite.cpp    present-override compositor (base=DLSS scene, postUI=UI-on-black)
src/Upscaler/D3D12Upscaler.cpp     the upscaler: shared-texture interop, DLSS/FSR dispatch, present-override, ConfigureFrameGeneration, kEnableDLSSG/kPresentOverride
src/Upscaler/Upscaling.cpp         orchestration: method selection, menu gating, TagDLSSGInputs (stub -> now done in DX12SwapChain), stubs for the FO4 GPU body
src/Game/Renderer.cpp              device/context + render-target texture/RTV access (version-independent)
src/Game/Util.cpp                  camera capture (NiCamera worldToCam), Halton jitter, shader compile
src/Settings/Settings.cpp          INI-backed settings store (mirrored into Upscaling::settings)
src/UI/Menu.cpp                    SKSE Menu Framework 3 settings page
src/Neural/NeuralRendering.cpp     RenoDX-style external DLL loader (foundation; RR not wired)
src/Diagnostics/Probe.cpp          standalone diagnostic dump (RTs, camera, hook verification)
```

Settings enum: `upscaleMethodPreference` 0=Disabled/1=FSR/2=DLSS; `qualityMode` 0=NativeAA(DLAA)/1=Quality/2=Balanced/3=Performance/4=Ultra; `frameGenerationMode` 0=Off/1=On/2=Auto.
Render‑scale ladder (`RenderScaleForQuality`): 1.0 / 0.667 / 0.58 / 0.5 / 0.333.

---

## 9. Known‑good builds (in `dist/known-good/`, git‑ignored)

- `SkyrimUpscaler-uiFsr-working.dll` — pre‑proxy side‑device build (FSR+DLSS+ENB+UI).
- `SkyrimUpscaler-proxy-fsr-dlss-stable.dll` — proxy‑swapchain build, DLSS‑G off (stable).

---

## 10. TL;DR for the next engineer

The whole stack works **except DLSS‑G frame generation**, which activates then GPU‑hangs inside
Streamline's `dlfgPresent` ~15 s in. The present/color path is now correct (present‑override,
validated with ENB), so focus on **(a) motion‑vector/depth correctness for DLSS‑G, (b) the
present‑thread state‑query + Reflex frame‑token pipeline matching `fo4test` exactly, (c) a GPU
capture of the hang.** Diff `src/Render/DX12SwapChain.cpp::Present` and
`src/Render/Streamline.cpp` against `C:\Claude\fo4test` line by line — the remaining divergence is
in there.
