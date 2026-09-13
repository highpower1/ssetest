# Open issues

## Resolved: D3D12 device lost at `commandList->Close()`

Fixed 2026-09-13, diagnosed by Codex with the D3D12 debug layer.

The frame after direct NGX created DLSS-NR feature 18 failed at
`ID3D12GraphicsCommandList::Close` with `E_INVALIDARG (0x80070057)` and
`GetDeviceRemovedReason() == DXGI_ERROR_INVALID_CALL (0x887A0001)`, after which
the proxy swapchain rejected every Present.

The debug layer named it:

- **524** -- a resource with only `D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET` was
  transitioned to `D3D12_RESOURCE_STATE_UNORDERED_ACCESS`.
- **340** -- a UAV was created for a resource that does not allow one.
- **232** -- the device was removed for an illegal or undefined operation.

`D3D12NeuralGBuffer::EnsureResources` created the Ray Reconstruction guide
textures as render-target-only and then handed them to Streamline's RR path,
which uses them as unordered-access views. `createTarget` now takes the extra
flag, for the three guides (`normalRoughness`, `albedo`, `specularAlbedo`) and
for `upliftResult`, the direct NGX output. The clear-value and
simultaneous-access messages in the same run were warnings, not the fault.

Verified: DLSS-NR feature creation, RR evaluate, the present override switching
to the uplift target on the following frame, and the uplift running 598/600 in
the first interval (the intentional startup skips) then 600/600 for seven
consecutive intervals, with no device removal or validation error.

Worth keeping for the method rather than the fix. Three rounds were spent
reasoning about which recent change looked riskiest, and all three were wrong.
DRED settled that the GPU had not faulted, a per-call stage marker settled which
call failed, and the debug layer named the resource. Each was cheap, and each
answered something no amount of re-reading the diff would have.

## Open: the uplift's visible effect is slight

Neural Rendering now runs on 600 of every 600 frames, with display-resolution
guides sampled where the jittered raster actually put each feature, and colour
normalised into a defined encoding with a diffuse-white reference. The change on
screen is still hard to see.

**NR Debug: show difference** settled the first question, twice. The model is
clearly changing the image: at 19x amplification, particle sprites and object
silhouettes saturate to white, so the uplift is altering edges and particles by
several percent. This is not a dead integration -- it is a question of how the
change reads once composited, and of the inputs listed below.

Note that the second of those two runs was only meaningful once the output path
was fixed. Difference-view runs made while `PresentOverride = 0` under ENB
showed nothing, because nothing this mod rendered was reaching the screen at
all; see the resolved issue below before trusting any visual result.

Since then the explicit `DLSSNR.JitterOffsetX/Y` parameters are set (zero after
the upscaler, where the image is already resolved; the frame's negated Halton
offset before it) and the pass ceiling is the reference's ten rather than three.

`DLSSNR.UI` and `UIAlpha` are deliberately not supplied. UI correction exists for
implementations that uplift the finished frame including the HUD; ours runs at
the pre-UI hook on the hud-less scene, and the UI is composited afterwards at
present time. There is no UI in the input to correct for.

What is left to try is settings rather than plumbing: `Style` selects between
model variants (the addon labels them Model A, B and C) and has only ever been
run at 0, and pass counts above one are now available.

## Resolved: the copy-back output path is dead under ENB

Settled 2026-09-13 with the debug bypass.

Two output paths exist. The present override keeps the upscaled image on D3D12
and composites the UI at present. `PresentOverride = 0` instead copies the
result into the game's `kMAIN` colour target and lets the game present normally.
The second was offered as the ENB-friendly path, on the reasoning that ENB would
then tonemap and grade the upscaled scene like any other frame.

It does not. With ENB installed, ENB's chain overwrites `kMAIN` after we write
it, so **nothing this mod renders reaches the screen on that path** -- not the
neural uplift, not sharpening, not the upscaler's own output. The user's first
report of it was exactly right and exactly literal: "ENB's raw output".

`DLSSNRDebugBypass = 1` clears the uplift target to flat magenta. On the
present-override path the screen turns magenta. On the copy-back path under ENB
it does not change. That is the whole proof, and it took one toggle.

The method note from the device-loss issue applies again, in the other
direction. The copy-back path was proposed and shipped as a fix without ever
being put through the magenta test that the present-override path had already
passed. The instrument existed; it simply was not pointed at the new path.

A startup warning now fires for `PresentOverride = 0` with ENB loaded, since the
combination otherwise reads as "the upscaler does nothing".

## Open: the composite cannot tell ENB's post-processing from UI

This is the real cost of the one working output path, and it is what a user
noticed before it was understood here: "in the places where ENB's light falls,
the neural rendering seems to revert".

Under the present override, `kMAIN` is cleared to black, the game and ENB draw
onto it, and `D3D12UIComposite` treats any non-black pixel as UI to be laid over
the scene. ENB's bloom, lens effects and light sprites are non-black. They are
therefore classified as UI and composited over the uplifted scene, so wherever
ENB light falls, ENB's version of the pixel replaces ours.

Separately, ENB's tonemapping and grading run against the cleared target and
never touch the scene at all.

The fix is to stop inferring UI from luminance and give the composite a real UI
mask. Until then the two effects are inherent to the working path.

## Open: frame generation is unavailable with the Steam overlay

Not a bug to fix here -- the overlay faults on DLSS-G's own present thread. It
is blocked by default and can be overridden with `FrameGenWithSteamOverlay = 1`
for configurations that survive it. See [SAFETY.md](SAFETY.md).
