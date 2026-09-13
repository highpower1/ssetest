# Handoff: unresolved D3D12 device loss

Working document for whoever picks up the open bug in
[docs/OPEN_ISSUES.md](docs/OPEN_ISSUES.md). Delete it when that issue closes.

Read [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) first -- the whole design
turns on three constraints (Skyrim is D3D11, modern DLSS is D3D12-only, ENB owns
Present) and none of this makes sense without them.

---

## 1. The bug

The game freezes a second or two after the first in-world frame. Reproduces
every run.

```
[D3D12Upscaler] Evaluate failed at 'close list (frame work)';
    Failure with HRESULT of 80070057 removed=0x887A0001
[DRED] D3D12Upscaler::Evaluate: device removed, reason 0x887A0001
[DRED] page fault at GPU virtual address 0x0000000000000000
```

E_INVALIDARG from `ID3D12GraphicsCommandList::Close`, which is where the runtime
validates what was recorded. The frame's command list contains an invalid
command. Everything after that line is Present refusing a dead device, several
hundred times -- ignore it.

### Timing, which is the strongest clue

Always the frame **after** NGX creates the DLSS-NR feature:

```
:675  [DLSS-NR] Feature creation submitted prepared=true order=after   <- frame A
:736  [D3D12Upscaler] DLSS-RR evaluate: ok=true                        <- frame A, fine
:740  [DX12SwapChain] Present override received                        <- presented
:797  [D3D12Upscaler] Evaluate failed at 'close list (frame work)'     <- frame B
```

Frame A closes, submits, waits on and resets the command list **mid-frame** for
NGX feature creation, then records the normal frame into the reset list and
closes it successfully.

Frame B is the settling frame. The uplift is skipped, so it records only the Ray
Reconstruction path -- `D3D12NeuralGBuffer::Generate` then
`Streamline::UpscaleD3D12RR` -- the same two things frame A recorded after its
reset. Its Close fails.

So the invalid command is in the ordinary Ray Reconstruction path, and it only
becomes invalid after a frame that reset the list mid-flight.

## 2. Already ruled out

Do not re-spend runs on these.

| Hypothesis | Evidence against |
| --- | --- |
| GPU fault / TDR | DRED is on and reports **no breadcrumb nodes** and page-fault VA **zero**. Nothing failed on the GPU. Different from the DLSS-G `DEVICE_HUNG` history in docs/SAFETY.md, which did produce breadcrumbs |
| Steam overlay | Real and separate: `gameoverlayrenderer64.dll` faulting under `sl.dlss_g.dll` on DLSS-G's present thread. Fixed by refusing frame generation while the overlay is loaded. This freeze predates and outlives that fix |
| The discarded uplift-guide pass | The guide resample used to be recorded before the decision about whether the uplift runs, leaving unused work in skipped frames. Moved behind the gate; failure unchanged |
| The colour codec | Never executes on the failing frame -- the uplift is skipped, so encode and decode never run |

## 3. Do this first

The debug layer names a validation failure outright, and **has never actually
run**. It was set in the ini but silently ignored: the file is UTF-8 with a
byte-order mark, which defeats GetPrivateProfileInt. That is now fixed (the flag
is parsed by hand and the decision is logged either way), but no run has
happened since.

```ini
[General]
D3D12DebugLayer = 1
```

Expect `[D3D12] Debug layer REQUESTED in <path>` then either `Debug layer
ENABLED` or `unavailable; install the Graphics Tools optional feature`. On
failure the log then carries:

```
[D3D12] Evaluate: severity=... id=... D3D12 ERROR: <what was invalid>
```

That one line should end the investigation. Set the flag back to 0 afterwards --
it costs real frame time.

### If the debug layer cannot be installed

Bisect instead, in this order:

1. `DLSSNREnabled = 0` -- removes the mid-frame feature-creation submission
   entirely. If stable, the fault is in or around that block.
2. `NeuralRayReconstruction = 0` -- leaves plain DLSS SR. If stable, the fault
   is in the RR path (`D3D12NeuralGBuffer::Generate` or `UpscaleD3D12RR`).
3. `FrameGenerationMode = 0`.

## 4. Where to look

- `src/Upscaler/D3D12Upscaler.cpp` -> `Evaluate`. The `stage` local names the
  last D3D call attempted and is reported on failure; add more markers freely.
  The NGX feature-creation block (close / execute / wait / reset mid-frame) is
  the most suspicious thing in the function and the newest.
- `src/Render/D3D12NeuralGBuffer.cpp` -> `Generate`. Descriptor heap slots, and
  COMMON transitions on shared textures that Streamline also manages.
- `src/Render/Streamline.cpp` -> `UpscaleD3D12RR`. Tags every resource as COMMON
  and lets Streamline own their states -- a mismatch with our explicit barriers
  is a plausible source of an invalid recorded command.
- `src/Render/DeviceRemovedReport.cpp` -- DRED and the debug layer live here.

## 5. Build and deploy

Toolchain and dependencies: [docs/BUILDING.md](docs/BUILDING.md). xmake must run
inside a Visual Studio developer environment; from Git Bash it mis-detects
mingw.

```
xmake build SkyrimUpscaler
```

Deploy the DLL straight into the Mod Organizer 2 mod folder -- the user runs the
game through MO2, so a zip means a manual copy every iteration:

```
build/windows/x64/releasedbg/SkyrimUpscaler.dll
  -> %LOCALAPPDATA%\ModOrganizer\Skyrim Special Edition\mods\SkyrimUpscaler\SKSE\Plugins
```

Do not wipe that folder: it also holds the Streamline and FidelityFX runtimes
and an ini the user edits.

Log: `Documents/My Games/Skyrim Special Edition/SKSE/SkyrimUpscaler.log`. Note
that Documents may be redirected to OneDrive. Settings live in MO2's `overwrite`
tree, not the mod folder.

Test surface is one machine: RTX 4070 Ti, AE 1.6.1170, ENB, 1920x1080.

## 6. Constraints

**Frame generation has hung this GPU repeatedly**, once with a
DPC_WATCHDOG_VIOLATION bugcheck in nvlddmkm.sys. Read
[docs/SAFETY.md](docs/SAFETY.md) before touching it. The present watchdog, the
per-backbuffer-index DLSS-G input copies and the cross-queue fence waits are all
load-bearing; do not remove them to simplify a repro.

The current bug is **not** that failure mode -- no breadcrumbs, no TDR -- but
the machinery is shared.

## 7. Known gaps, deliberately not being worked on

So they are not mistaken for the bug, or rabbit-holed into:

- FSR frame generation is unimplemented (`EvaluateFSRFrameGeneration` returns
  false). The other two `Evaluate*` stubs beside it are dead interface inherited
  from the Fallout 4 port; the real path is in `D3D12Upscaler`.
- The sharpness slider only affects FSR -- the DLSS path passes no sharpened
  output target.
- No transparency or reactive mask is supplied to either upscaler.
- Ray Reconstruction guides are synthetic: geometric normals from depth, and
  constant roughness, albedo and specular albedo.
- Changing resolution mid-session leaves the shared textures stale; restart.
- Against the RenoDX reference addon, DLSS 5 Neural Rendering is still missing
  the explicit `DLSSNR.JitterOffsetX/Y` parameters (jitter is currently folded
  into where the guides are sampled) and the `DLSSNR.UI` / `UIAlpha` correction
  buffers.
