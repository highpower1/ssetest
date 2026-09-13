# FAQ

For people who install mods, not people who write them. Nothing here needs a
debugger or a log viewer. If you want the technical version, that is
[TROUBLESHOOTING.md](TROUBLESHOOTING.md).

---

## Read this first

**Do not run this with Community Shaders.** Not "it might conflict" — they both
take over the same part of Skyrim's renderer, and Community Shaders already
includes its own upscaling. Pick one. If both are installed, uninstall one
before doing anything else on this page.

**ENB is fine.** ENB is supported and tested. Your ENB preset's colours,
tonemapping and effects all still apply.

---

## What does this mod actually do?

Three separate things, all optional:

- **Upscaling (DLSS / FSR).** Skyrim renders at a lower resolution and the
  upscaler reconstructs a full-resolution image. You get frames back. At the
  "Native AA" setting it does not lower the resolution at all and just gives you
  much better anti-aliasing than Skyrim's own.
- **Frame generation.** The graphics card invents extra frames in between the
  real ones. Higher frame counter, smoother motion. It does not make the game
  respond faster.
- **Neural Rendering.** An NVIDIA model that reworks the finished image —
  materials, lighting and skin look less flat. This is the newest and most
  demanding feature, and the hardest to get running (see below).

## What do I need?

| Feature | Needs |
| --- | --- |
| FSR | Any reasonably modern GPU |
| DLSS, DLAA | NVIDIA RTX card |
| Frame generation | NVIDIA RTX 40 or 50 |
| Ray Reconstruction | NVIDIA RTX card |
| Neural Rendering | NVIDIA RTX card **and** a DLL you have to find yourself |

Skyrim SE 1.5.97 or AE 1.6.x, plus SKSE. VR and the GOG version are not
supported.

## Where are the settings?

Press **F1** in game to open the Mod Control Panel, then pick **SkyrimUpscaler**
in the list on the left. (F1 is the Mod Control Panel's own default; if you
changed it, use whatever you changed it to.)

Settings are saved when you close the panel.

## I installed it and nothing looks different

Work down this list:

1. **Is anything turned on?** Frame generation and Neural Rendering both default
   to **off**. Upscaling defaults to Native AA, which improves anti-aliasing but
   will not change your frame rate much.
2. **Did you install the runtime DLLs?** This mod does not include NVIDIA's or
   AMD's files. Without them there is nothing to run. See
   [the README's Installing section](../README.md#installing).
3. **Is Community Shaders installed?** See the top of this page.

## Do I have to download extra files? Why?

Yes, and it is not optional. NVIDIA's DLSS runtime and AMD's FSR runtime are
distributed by NVIDIA and AMD under their own licences, so they cannot be
bundled here. You put them in
`Data/SKSE/Plugins/SkyrimUpscaler/Streamline/` yourself.

## Neural Rendering will not turn on

This is expected on most cards, and it is not a bug in this mod.

The file NVIDIA ships (`nvngx_dlssnr.dll`) only contains code for RTX 50-series
GPUs. On a 20, 30 or 40 series card it loads and then refuses to run. A patched
version exists, made by the community, and the
**[RenoDX Discord](https://discord.com/invite/renodx)** is where that work
happens and where the file is shared. This mod's Neural Rendering would not work
at all without them.

## Should I turn frame generation on?

Try it, but know what it is:

- It **doubles the number on your frame counter** and makes motion look
  smoother.
- It does **not** make the game feel more responsive. Input latency stays about
  the same or gets slightly worse.
- It needs a real frame rate that is already decent to work well. Turning it on
  at 25 fps will not feel good.

**Turn the Steam overlay off for Skyrim before using it** (Steam → Skyrim →
Properties → General → In-Game Overlay). The overlay crashes when frame
generation is running. The mod blocks frame generation by itself when it sees
the overlay; there is a setting to override that, and if you do, a crash is on
you.

## The game crashed or froze

1. **Turn frame generation off and try again.** It is the single most likely
   cause. If the freeze goes away, leave it off.
2. **Check for Community Shaders.** Again.
3. If it still happens, the log is at
   `Documents/My Games/Skyrim Special Edition/SKSE/SkyrimUpscaler.log`.
   Attach that when reporting it — without it there is nothing to go on.

A freeze where the picture stops but the sound continues is usually the graphics
driver, not a normal crash. Frame generation is the usual cause.

## My screen went weird after I pressed a key

**Press F10 a few more times.** F10 cycles through diagnostic views used for
development. Some of them hide the whole interface, which is alarming if you did
not mean to press it. Keep pressing until the picture looks normal again.

To stop it happening, set `UICompositeDebugKey = 0` in
`Data/SKSE/Plugins/SkyrimUpscaler/SkyrimUpscaler.ini`.

## The picture looks softer / sharper / wrong

- **Too soft:** raise **Sharpness**, or use a higher quality mode (Quality
  rather than Performance). Performance modes render at a much lower resolution
  and there is only so much any upscaler can rebuild.
- **Too sharp or crunchy:** lower **Sharpness**. If Neural Rendering is on, lower
  **NR Local Structure**.
- **Faces look harsh:** lower **NR Skin Structure**.
- **Ghosting or trails behind moving things:** make sure **Transparency Hint** is
  on.

## My frame rate got worse

Upscaling costs something to run. At **Native AA** it renders at full resolution
*and* runs the upscaler, so it is slower than not using the mod — you are paying
for the anti-aliasing, not for speed. If you want frames, use Quality, Balanced
or Performance.

Ray Reconstruction and Neural Rendering both cost extra on top. Turn them off if
you are short on frames.

## Does this work with my other mods?

Almost certainly, with two exceptions:

- **Community Shaders** — no. See the top of the page.
- **Other upscaler mods** — no. Only one of them can hook the renderer. Pick one.

ENB, ReShade, weather mods, ENB presets, texture packs, lighting mods and script
mods are all unaffected. This mod does not touch anything the game saves, so it
can be installed or removed mid-playthrough without breaking a save.

## How do I uninstall it?

Remove the mod. Nothing is written to your save game, and no game files are
changed. You may want to delete
`Data/SKSE/Plugins/SkyrimUpscaler/SkyrimUpscaler.ini` as well, which is just the
settings file.

## Something is still wrong

The log at
`Documents/My Games/Skyrim Special Edition/SKSE/SkyrimUpscaler.log`
is rewritten every time you start the game and records what loaded, what did
not, and why. It is worth attaching to any report even if it means nothing to
you — it usually means something to whoever reads it.
