# Frame generation and the driver

Frame generation on this architecture — a D3D11 game, shared textures, a D3D12
proxy swapchain, and ENB — has hung the GPU during development. Not crashed:
hung, with the machine-wide symptoms that come with it. One test produced a
`DPC_WATCHDOG_VIOLATION` bugcheck in `nvlddmkm.sys`.

That class of failure is fixed, and frame generation has since run for tens of
thousands of frames without incident. The protections below are why, and they
should not be removed casually.

## What protects you

**Refusing to run beside the Steam overlay.** The overlay's Present hook faults
when DLSS-G presents from its own thread. Frame generation is disabled while
`gameoverlayrenderer64.dll` is loaded rather than left to crash.


**A present watchdog.** `DX12SwapChain` times each Present call. Two consecutive
calls over 80 ms, or an outright failed present, latch frame generation off for
the rest of the session and log why. Restart the game to try again.

The watchdog deliberately ignores long gaps *between* presents. A menu, a
loading screen, an alt-tab or a CPU hitch all produce one while Present itself
returns promptly, and treating that as a hang once disabled frame generation for
a whole session on a 6.8-second loading pause. Only a Present that *blocks* is
the pre-TDR signature.

**Ordered, double-buffered DLSS-G inputs.** The inputs Streamline reads are
per-backbuffer-index copies made on the present queue, and the present waits on
the upscaler's fence before reading anything it produced. Tagging the live
shared textures instead lets DLSS-G's asynchronous reads race both the upscaler
and the next frame's overwrite, which is what hung the GPU repeatedly.

**Lifetime discipline on D3D12.** A command allocator is never reset before the
GPU has finished with it, queue waits and signals are checked, GPU waits are
bounded, and nothing throws across the DXGI Present ABI.

## If it hangs anyway

Frame generation defaults to off and has to be opted into. If the game freezes
with it enabled:

1. Note whether the log ends with a watchdog line. If it does, the guard worked
   and the configuration is at fault. If it does not, the guard missed a case
   worth reporting.
2. Leave frame generation off and check `SkyrimUpscaler.log` for the last
   `DLSS-G` line before the freeze.
3. Raise the multiplier one step at a time. With the RTX 40 unlock active the
   ceiling can reach 6x, and present pacing gets harder the higher it goes.
