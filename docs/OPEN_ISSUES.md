# Open issues

## D3D12 device lost at `commandList->Close()` (unresolved)

The game freezes a second or two after the first in-world frame. One line
matters; everything after it is Present refusing a dead device.

```
[D3D12Upscaler] Evaluate failed at 'close list (frame work)';
    Failure with HRESULT of 80070057 removed=0x887A0001
[DRED] D3D12Upscaler::Evaluate: device removed, reason 0x887A0001
[DRED] page fault at GPU virtual address 0x0000000000000000
```

`0x80070057` is `E_INVALIDARG` from `ID3D12GraphicsCommandList::Close`, which is
where the runtime validates what was recorded. So the frame's command list
contains an invalid command.

### Established

- **Not a GPU fault.** DRED is enabled and reports no breadcrumb nodes and a
  page-fault address of zero. Nothing failed on the GPU; the device was lost to
  a call the runtime rejected on the CPU. This is a different failure from the
  DLSS-G `DEVICE_HUNG` history in `SAFETY.md`, which did produce breadcrumbs.
- **Not the Steam overlay.** That was a separate, real crash
  (`gameoverlayrenderer64.dll` faulting under `sl.dlss_g.dll`), fixed by
  refusing frame generation while the overlay is loaded. This freeze predates
  and outlives that fix.
- **Not the discarded uplift-guide pass.** The guide resample used to be
  recorded before the decision about whether the uplift runs, leaving an unused
  pass in skipped frames. That was moved behind the gate; the failure is
  unchanged.

### Timing

Reproduces every run, always on the frame after NGX creates the DLSS-NR feature:

```
:675  [DLSS-NR] Feature creation submitted prepared=true order=after
:736  [D3D12Upscaler] DLSS-RR evaluate: ok=true            <- frame A, fine
:740  [DX12SwapChain] Present override received            <- presented
:797  [D3D12Upscaler] Evaluate failed at 'close list'      <- frame B
```

Frame A does the feature-creation submission (close, execute, wait, reset) and
then records the normal frame. Frame B is the settling frame: the uplift is
skipped, so it records only the Ray Reconstruction path — `Generate` (guide
normals) followed by `UpscaleD3D12RR` — and its `Close` fails.

Whatever is invalid is therefore in the ordinary Ray Reconstruction path, but it
only becomes invalid after the frame that reset the command list mid-frame for
feature creation.

### The next step

Nobody has yet run with the debug layer on, which is the tool that names a
validation failure outright. Add to `SkyrimUpscaler.ini`:

```ini
[General]
D3D12DebugLayer = 1
```

The failure log then carries the validation message. It needs the Graphics Tools
optional Windows feature; the log says so if it is missing. Set it back to 0
afterwards — it costs real frame time.

### Where to look

- `src/Upscaler/D3D12Upscaler.cpp` — `Evaluate`, in particular the NGX
  feature-creation block that closes, submits, waits and resets the command list
  in the middle of a frame.
- `src/Render/D3D12NeuralGBuffer.cpp` — `Generate` and `GenerateUpliftGuides`;
  descriptor heap slots and the `COMMON` transitions around shared textures.
- `src/Render/Streamline.cpp` — `UpscaleD3D12RR`, which tags resources as
  `COMMON` and lets Streamline manage their states.

### Workaround

Setting `DLSSNREnabled = 0` has not been tested against this and is the obvious
first bisection: it removes the feature-creation submission entirely.
