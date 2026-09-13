# Troubleshooting

**Before anything else: this plugin does not support Community Shaders and is
redundant alongside it.** Both interpose on the same part of the render pipeline
and CS already has its own upscaling. If both are installed, that is the first
thing to fix — no other symptom is worth diagnosing until one of them is gone.

In most cases, the logs are located here.
`Documents/My Games/Skyrim Special Edition/SKSE/SkyrimUpscaler.log`.

## Reading a healthy startup

```
[Streamline] Successfully initialized Streamline sdkVersion=0x... (DLSS-NR preview: true)
[Streamline] DLSS feature is available (eOk)
[Streamline] DLSS-G feature is available (eOk)
[Streamline] DLSS-RR feature is available (eOk)
[DLSS-NR Direct] Direct NGX DLSS-NR is ready
[D3D12Upscaler] Initialised: D3D12 interop ready 1920x1080, DLSS(SR)=true
[D3D12Upscaler] DLSS evaluate: ok=true render=1280x720 display=1920x1080
```

Every runtime DLL is logged as `found` or `missing` at startup. A feature that
reports `eErrorFeatureMissing` is almost always a missing `nvngx_*.dll`.

## Expected messages that are not problems

| Message | Why |
| --- | --- |
| `Ignoring plugin 'sl.dlss_nr' since it is not supported on this platform` | Streamline rejects the preview NR plugin; the direct NGX path carries it instead |
| `Ignoring plugin 'sl.deepdvc' ... not requested by the host` | we do not use DeepDVC |
| `SL ImGui feature loaded check failed` | `sl.imgui.dll` is not shipped; the debug overlay is unavailable |
| `Dynamic MFG requested but runtime reports unsupported` | dynamic multiplier selection is a separate capability from multi-frame generation |

## NGX result codes

They are `0xBAD00000 | n`. The two seen most often:

- `0xbad0000c` — `FAIL_OutOfDate`. Something is **out of date**, usually because
  the announced SDK version does not match the runtime. This is *not* a hardware
  limit; "unsupported" would be `0xbad00001`.
- `0xbad00002` — `FAIL_PlatformError`.

## Neural Rendering will not initialise

`nvngx_dlssnr.dll` ships in a Blackwell-only build. Two files can be identical
in size and version and still differ: scan for the string `sm_120`. The stock
build contains it, an Ada-patched build does not, and only the patched one runs
on RTX 40.

## The feature runs but the screen does not change

This happened repeatedly during development and the cause was never the
feature's own settings. Prove the output path first:

1. Turn on **NR Debug: bypass uplift**. It fills the uplift's target with flat
   magenta and presents it.
2. Magenta on screen means the target reaches the screen, so look at the
   feature's parameters.
3. No magenta means the target never gets there, so look at the present
   override and the composite.

A diagnostic that fills the target with a plausible-looking image proves
nothing. An earlier version used the pre-upscale scene, which at Native AA looks
the same as the upscaled one, and it made a broken path look fine.

## The uplift runs but barely changes anything

The model expects colour in a defined encoding with a known diffuse-white
reference. Skyrim's scene colour is unbounded linear HDR with neither, so it is
normalised before the uplift and restored afterwards. If the effect still looks
faint, sweep **NR Colour Encoding** — sRGB, linear BT.709 and BT.2100 PQ each
present the scene to the model differently — and adjust **NR Diffuse White** to
match how bright your ENB preset actually renders.

Two inputs the reference implementation supplies are still missing here: the
explicit `DLSSNR.JitterOffset` parameters (jitter is currently folded into where
the guides are sampled instead) and the UI correction buffers.

## Crash in gameoverlayrenderer64.dll

The Steam overlay hooks Present per swapchain. DLSS-G presents from its own
worker thread, and the overlay dereferences state it never set up for that path,
so the call stack ends in an access violation inside `gameoverlayrenderer64.dll`
below `sl.dlss_g.dll`.

Frame generation is therefore disabled whenever the overlay is loaded, and the
log says so at startup. To use frame generation, turn the overlay off for
Skyrim: **Steam → Skyrim Special Edition → Properties → General → In-Game
Overlay**.

## Diagnosing a lost D3D12 device

Two mechanisms report one, and they cover different halves.

Device Removed Extended Data is always on. When the device is lost it logs the
command queue and list, how many GPU operations completed, and the one that did
not. Breadcrumb nodes and a non-zero page-fault address mean the GPU faulted.
**No breadcrumbs and a page-fault address of zero mean the opposite**: nothing
failed on the GPU, and the device was lost to an invalid call made on the CPU.

For that second case, set `D3D12DebugLayer=1` in `SkyrimUpscaler.ini`. The
runtime then validates every call and the failure log carries the validation
message that names it. This costs real frame time, so set it back to 0 when
finished, and it needs the Graphics Tools optional Windows feature -- the log
says so if it is missing.

`Evaluate failed at '<stage>'` names the last D3D call attempted, which narrows
the search before either of the above is needed.

## Known limitations

- **FSR frame generation is not implemented.** `EvaluateFSRFrameGeneration` is a
  stub; only DLSS-G generates frames.
- **No transparency or reactive mask.** Both are passed as null, so particles,
  water and foliage can ghost more than they should.
- **Ray Reconstruction guides are synthetic** — geometric normals with constant
  material terms. See [ARCHITECTURE.md](ARCHITECTURE.md).
- **VR and GOG are out of scope.**
