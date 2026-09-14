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
2. **Did the install land intact?** Check the log for any runtime DLL reported
   as `missing`. A feature whose DLL is absent stays unavailable in the menu
   rather than failing loudly.
3. **Is Community Shaders installed?** See the top of this page.

## Do I have to download extra files?

**Not if you installed the release package** — every NVIDIA and AMD runtime is
in it and lands where it needs to be.

If you built the plugin from source instead, you supply them yourself: NVIDIA's
Streamline DLLs go in `Data/SKSE/Plugins/SkyrimUpscaler/Streamline/` and AMD's
FidelityFX DLLs go next to `SkyrimUpscaler.dll`. Every file is reported as
`found` or `missing` in the log at startup.

## About Neural Rendering

The file NVIDIA ships (`nvngx_dlssnr.dll`) only contains code for RTX 50-series
GPUs. On a 20, 30 or 40 series card it loads and then refuses to run — which is
why this feature does not simply work everywhere.

The build in the release package is the community-patched one and runs on Ada
and earlier. That work is not this project's. It comes from the
**[RenoDX](https://discord.com/invite/renodx)** community, and this mod's Neural
Rendering would not exist without them. If this is the feature you came for, go
and say thank you over there.

## Which frame generation should I use?

There are two, and **Frame Generation Backend** in the menu picks between them:

- **NVIDIA DLSS-G** — needs an RTX 40 or 50 card. Better tested here.
- **AMD FSR** — runs on far more hardware, including AMD and Intel cards.
- **Auto** (the default) — DLSS-G where it works, FSR everywhere else.

**Changing it requires restarting the game.** The two use different swapchains
and the game only gets one, decided before any setting can change it.

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

## My camera is in the wrong place, or the field of view keeps changing

Set `CameraStateJitterPatch = 0` in
`Data/SKSE/Plugins/SkyrimUpscaler/SkyrimUpscaler.ini` and restart.

This mod overwrites ten bytes of the game's camera code so the engine keeps
applying the sub-pixel offset upscaling needs even with the game's own
anti-aliasing off. The address it writes to is the same for every supported
game version, which may not be correct on all of them — and where it is wrong
it damages whatever is actually there. A third-person camera sitting high and
to the left, or a field of view that shifts as you look around, is what that
looks like.

With it off the upscaler may ghost slightly, because the image is no longer
being rendered with the offset the upscaler is told about. **If turning it off
fixes your camera, please report it with your game version** — the log now
prints the bytes at that address on your runtime, and that is what is needed to
fix it properly rather than by switch.

## My frame rate at 4K is much worse than I expected

Some of this is inherent and some is not.

Running the upscaler at **Native AA** means the game renders at full resolution
*and* the upscaler runs on top, so at 4K you are paying for a full 4K frame plus
the upscaler. That costs frames rather than saving them; it buys anti-aliasing.
If you want frames, pick **Quality**, **Balanced** or **Performance** — those
render smaller and let the upscaler rebuild to your monitor's resolution, which
is the entire point of the feature.

**Leave the game's resolution at your monitor's resolution.** Do not set Skyrim
to 1080p and expect the upscaler to fill a 4K screen — the quality mode is the
control for that, and lowering the game's resolution as well just gives the
upscaler less to work with.

Frame generation does not help here. It raises the number on the counter without
raising the real frame rate, and Skyrim's physics is tied to the real one, so
generating frames on top of a real 25 fps leaves the physics behaving like 25
fps. Get the real frame rate up first with a quality mode, then add frame
generation on top if you still want it.

## The picture looks softer / sharper / wrong

- **Too soft:** raise **Sharpness**, or use a higher quality mode (Quality
  rather than Performance). Performance modes render at a much lower resolution
  and there is only so much any upscaler can rebuild.
- **Too sharp or crunchy:** lower **Sharpness**. If Neural Rendering is on, lower
  **NR Local Structure**.
- **Faces look harsh:** lower **NR Skin Structure**.
- **Ghosting or trails behind moving things:** make sure **Transparency Hint** is
  on.
- **Square blocks around the sun or a campfire:** set **NR Colour Encoding** back
  to **BT.2100 PQ**. The Linear option hands the model brightness values it has
  no reference for, and the game's bloom then smears the result into tiles.

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
