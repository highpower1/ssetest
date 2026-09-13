# External neural rendering modules

The plugin can host your own DLL and run it once per frame on the scene colour,
before the upscaler resolves it. Drop the DLL in:

```
Data/SKSE/Plugins/SkyrimUpscaler/Neural/
```

and enable **External Neural Modules** in the settings panel. Discovery happens
at game start, so a newly added module needs a restart.

`samples/NeuralColorGrade/` is a complete working module. It is a template
rather than a neural network — what it does is an exposure/contrast/saturation
grade read from an ini beside the DLL — but it exercises the whole path and
shows the parts that are easy to get wrong.

## The ABI

Copy [`include/SkyrimUpscalerNeural.h`](../include/SkyrimUpscalerNeural.h) into
your project. It needs nothing else from this repository. Export exactly one
function:

```cpp
extern "C" __declspec(dllexport)
const SkyrimUpscalerNeuralModuleV1* SkyrimUpscalerNeural_GetModuleV1(void);
```

returning a static descriptor with optional `Init`, `Evaluate` and `Shutdown`
callbacks.

## Lifecycle

- **Init** runs on the first rendered frame, so both the game's D3D11 device and
  the plugin's D3D12 interop device are live. `d3d12Device` may still be null;
  handle that.
- **Evaluate** runs once per frame on the render thread, inside the game's draw
  call, after the scene and before the HUD. The colour buffer is hud-less and at
  render resolution, so whatever you write is what DLSS or FSR then upscales.
- **Shutdown** runs at plugin teardown.

Return 0 for success. A module whose `Init` fails, or that throws from either
callback, is dropped for the session and the reason is written to the log.

## Rules that matter

- **Do not block and do not present** in `Evaluate`.
- **Save and restore every device-context binding you touch.** The engine
  resumes drawing on that same context the moment you return. The sample
  captures and restores render targets, viewports, scissors, all five shader
  stages, input layout, topology, blend, depth-stencil and rasterizer state, and
  the pixel-stage SRV, sampler and constant buffer.
- **You cannot read and write the colour buffer in one pass** — the engine has
  it bound as a render target. Copy it to your own scratch texture first.
- **Render only the render-resolution sub-rect**, not the whole texture.
- A raw ReShade or RenoDX addon is not loadable here; those export
  `ReShadeRegisterAddon` and need the ReShade runtime. Rebuild against this ABI.
  The loader detects that case and says so in the log rather than ignoring the
  file silently.
