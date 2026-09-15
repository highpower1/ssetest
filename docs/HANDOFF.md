# Handoff: the quality-mode zoom, and the Mod Control Panel

Written by Claude Code after failing to close this out. Two open faults, one
with a measured cause and a workaround in place, one barely investigated.
Everything below that is a measurement says so; everything that is a guess says
that too, because several of my guesses here were wrong and cost time.

## Build and deploy

```
xmake build SkyrimUpscaler        # must run inside vcvars64 (VS 18, MSVC 14.51)
```

Deploy target is the MO2 mod folder, currently named **`skyrimupscaling`**
(it has been renamed before — check before assuming):

```
C:\Users\kurif\AppData\Local\ModOrganizer\Skyrim Special Edition\mods\skyrimupscaling\SKSE\Plugins\SkyrimUpscaler.dll
```

Live settings ini (the plugin rewrites it when the panel closes):

```
...\ModOrganizer\Skyrim Special Edition\overwrite\SKSE\Plugins\SkyrimUpscaler\SkyrimUpscaler.ini
```

Log:

```
C:\Users\kurif\OneDrive\ドキュメント\My Games\Skyrim Special Edition\SKSE\SkyrimUpscaler.log
```

Test machine: RTX 4070 Ti, AE 1.6.1170, ENB (NAT.ENB III), 1920x1080.

---

## Problem A — every quality mode except DLAA magnifies the picture

### The chain, measured end to end

1. `Hook_UpdateJitter` (`src/Hooks/UpscalerHooks.cpp`) writes the requested
   render scale into `BSGraphics::State` RUNTIME_DATA `+0xA4` / `+0xA8`.
2. **Something writes 1.0 back before the scene is drawn.** From the log:

   ```
   [DRS] rtBase=0x60 scale pair read (1, 1) -> accepted
   [DRS] Scale written last frame was 0.3333; it reads back as 1.0000 now
         -- SOMETHING OVERWROTE IT
   ```

3. The scene therefore renders at full size.
4. `D3D12Upscaler::Evaluate` told DLSS the input was the requested fraction:

   ```
   DLSS-RR evaluate: ok=true render=640x360 display=1920x1080 scale=0.33333
   ```

5. Cropping the top-left third of a full-size frame and resolving it to the
   display is a 3x magnification. 0.6667 gives 1.5x. That is the reported zoom,
   and the ratios match exactly.

### What is NOT the cause (each of these was checked)

- **The offsets.** `+0xA4/+0xA8` validate: they read back as a bit-identical
  pair at 1.0 before the first write, and the write lands.
- **`bEnableAutoDynamicResolution`.** The plugin used to force this ON. I
  changed it to OFF on the theory that the engine's auto controller was the
  writer. **It is still overwritten with the controller off**, so that was
  wrong. The setting is currently `false` in
  `Upscaling::OnDataLoaded`; if that turns out to matter for anything else,
  changing it back is safe.
- **`renderScale` changing between hook and evaluate.** It does not; it is only
  assigned in `UpdateFromSettings` from the quality mode.
- **Resolution.** Reproduces at 1920x1080. A separate reporter saw it at
  2560x1440. The jitter offset is `2*0.5/width`, so 4K produces a *smaller*
  nudge, not a larger one.

### Current mitigation (in place, unverified by the user)

`D3D12Upscaler::EffectiveScale()` reads the scale back out of the same field and
sizes `GetRenderWidth/Height` from that, so the extent handed to DLSS matches
the picture the engine actually drew. `UpscalerHooks::SampleRenderScale()`
latches it once at the top of `Evaluate`, because those getters are called many
times while a frame is set up and reading live game memory per call can return
different answers within one frame.

**Consequence, stated plainly:** while the override does not take, quality modes
save no performance. The scene is full size and is reconstructed from full size,
so every mode behaves like DLAA. The log says so in those words.

An earlier version of this same change was shipped and withdrawn after being
blamed for a fault that turned out to predate it. The per-call read was its one
real defect and is fixed. It is worth knowing that history before removing it.

### What is actually wanted

Find the writer of the 1.0. Then quality modes cost what they should. Places I
did not look:

- Whether `RE::BSGraphics::State` is the right object at all — the plugin reads
  it via `RE::BSGraphics::State::GetSingleton()` in `ResetDynamicResolution` but
  through the hook's `a_state` argument in the jitter hook. If those are
  different instances, the write and the read would disagree and everything
  above still holds.
- `SSE Display Tweaks` is installed on the test machine and is known to touch
  resolution handling.
- The INI probe already in the code reports
  `fDynamicResolutionCurrentWidthScale:Display` as **not found**, so the engine
  will not name these fields for us.

---

## Problem B — the Mod Control Panel opens and closes immediately

Barely investigated. What is known:

- The panel **does** initialise. From `SKSEMenuFramework.log`:

  ```
  [01:05:39] Initializing ImGui...
  [01:05:39] ImGui initialized.
  [01:05:39] FontLoader: ... completed.
  ```

  So F1 is reaching the framework and the failure is after that, not a key
  binding problem.
- `SkyrimUpscaler.log` for that session **ends abruptly** fifteen seconds later
  at `01:05:54`, one second after the pipeline went active, with no `critical`
  line. No CrashLogger output was produced for that run.
- This predates the recent dynamic-resolution work. The user confirmed it is
  not caused by those changes.
- Opening the panel with frame generation enabled hung the GPU once earlier in
  development (see `docs/SAFETY.md`); frame generation was off for this run.

Things worth trying that I did not:

- Whether `D3D12UIComposite` is involved. Under the present override the panel
  is drawn into the cleared colour target and recovered by the UI-difference
  mask; if the mask misclassifies it the panel would be invisible rather than
  closing, but the two are easy to confuse from the outside.
- Whether our input sink (`Upscaling::ProcessEvent(RE::InputEvent* const*)`,
  added for the F10/F12 diagnostic keys) interacts with the framework's own
  input handling. It only acts on two scancodes and always returns
  `kContinue`, but it is the newest thing near the input path.

---

## Diagnostics already built, and how to use them

- **F10** cycles composite debug views: composite → UI layer → mask → scene only
  → pre-UI capture → split (ours | ENB). Two of them hide the entire interface;
  the key is the only way back out. `UICompositeDebugKey = 0` disables it.
- **F12** captures one full frame of `OMSetRenderTargets` with every view
  resolved to its `RE::RENDER_TARGET` name. This is what located the post-
  processing chain and should be the first tool for any "where in the frame does
  X happen" question.
- **`[DRS]` log lines** report the offset validation, the INI-name probe, and
  whether the written scale survived to the next frame.
- The upscaler **degrades rather than flickers** on sustained
  `eWarnOutOfVRAM`: Neural Rendering off, then Ray Reconstruction, each logged.
  A separate reporter's log showed 2194 such failures at 2560x1440, which is
  almost certainly what they were describing as the effect switching on and off
  as they turned.

---

## Mistakes worth not repeating

I made the same class of error three times: changing rendering code on a theory
before measuring, then attributing the next symptom to the most recent change.

- Sized the upscaler's input from a live read of the engine's scale, shipped it,
  and withdrew it when the user reported breakage — which they later confirmed
  predated it. The change was right and the withdrawal cost a cycle.
- Reverted the auto-controller theory's opposite before it had been run once.
- Guessed the camera fault was a hardcoded code offset. A user narrowed it in
  one sentence — "it happens on Quality and not on DLAA" — which eliminated the
  patch outright, because that patch is applied at startup regardless of mode.

The instruments in this repo are good and were built for exactly this. Use them
before editing. Every time I did, the answer came in one run.
