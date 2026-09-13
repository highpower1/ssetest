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

That may be the honest result on a rasterised, already-resolved Skyrim frame.
Before concluding it, **NR Debug: show difference** presents the amplified
before/after difference: a near-black frame means the model changed nothing,
visible structure means it did and only the amount is in question.

Still not supplied to the model, per the RenoDX reference addon:

- `DLSSNR.JitterOffsetX` / `JitterOffsetY`. Jitter is currently folded into
  where the guides are sampled rather than passed explicitly.
- `DLSSNR.UI` and `DLSSNR.UIAlpha`, with `UICorrection` left at 0.
- `Style` chooses between model variants (the addon labels them Model A, B and
  C) and has only ever been run at 0.
- Pass count is clamped to 3; the reference allows up to 10.

## Open: frame generation is unavailable with the Steam overlay

Not a bug to fix here -- the overlay faults on DLSS-G's own present thread -- but
it does mean frame generation and the Steam overlay cannot both be on. See
[SAFETY.md](SAFETY.md).
