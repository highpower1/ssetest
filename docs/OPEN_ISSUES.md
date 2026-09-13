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

## Resolved: the composite could not tell ENB's post-processing from UI

Fixed 2026-09-13. A user noticed this before it was understood here -- "in the
places where ENB's light falls, the neural rendering seems to revert" -- and
that description turned out to be a literal reading of the shader.

Under the present override the composite took any non-black pixel of the
presented buffer as UI. That buffer holds ENB's finished frame too, so the
composite was computing `lerp(our scene, ENB's scene, brightness)`: a
brightness-weighted crossfade between the two. Bright areas showed ENB's
version, dark areas showed ours. Torch flames looked doubled for the same
reason -- the upscaled flame mixed with ENB's non-upscaled one.

The fix stops inferring. `DX12SwapChain::CaptureUIBaseline` snapshots the
presented buffer at the pre-UI hook, after the world and ENB have written it and
before the UI lands, and a pixel counts as UI only where the final buffer
differs from that capture. ENB's output is in both and cancels. Brightness
cannot separate a health bar from a torch flame; "was this pixel changed by the
UI pass" can.

Verified by the user across all five debug views: the pre-UI capture is the
scene with no HUD, the mask is the HUD's shape alone, and the fire artefact is
gone from the composite.

Two method notes worth keeping. First, the decisive step was not reasoning about
the shader but comparing `Scene only` against the composite on the same frame --
the fire was correct in one and wrong in the other, which named the culprit in a
single keypress. Second, the debug views initially hid the UI with no way to
turn them off from inside the game, and the user was left unable to do anything
but close Skyrim. An instrument that can trap its user is a defect in the
instrument; the F10 cycle exists because of it.

Still true: ENB's tonemapping and grading run against the cleared colour target
and never reach the scene. The mask fixes what was being drawn over the scene,
not what is missing from it.

## Open: frame generation is unavailable with the Steam overlay

Not a bug to fix here -- the overlay faults on DLSS-G's own present thread. It
is blocked by default and can be overridden with `FrameGenWithSteamOverlay = 1`
for configurations that survive it. See [SAFETY.md](SAFETY.md).
