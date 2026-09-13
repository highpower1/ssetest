# Claude handoff: DLSS 5 Neural Rendering device-loss fix

Status: **resolved on the user's machine**. The user confirmed that the
post-fix build works. The successful run's `SkyrimUpscaler.log` also shows the
uplift surviving the formerly failing frame and running on 600/600 frames in
seven consecutive reporting intervals through 2026-09-13 11:51:54 JST.

## Failure and diagnosis

Before the fix, the frame after direct NGX created DLSS-NR feature 18 failed at
`ID3D12GraphicsCommandList::Close` with `E_INVALIDARG (0x80070057)` and
`GetDeviceRemovedReason() == DXGI_ERROR_INVALID_CALL (0x887A0001)`. The proxy
swapchain then rejected repeated Presents. This was not evidence that the
mid-frame command-list reset itself was wrong.

The first successful debug-layer run (2026-09-13 11:45) made the problem
specific:

- ID 524: a resource with only `D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET` was
  transitioned to `D3D12_RESOURCE_STATE_UNORDERED_ACCESS`; it lacked
  `D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS`.
- ID 340: a UAV was created for a resource that did not allow UAVs.
- ID 232: the runtime removed the device because of an illegal or undefined
  operation.

`D3D12NeuralGBuffer::EnsureResources` created the RR guide textures as
render-target-only resources, then passed them to Streamline's RR path. The
debug output did not identify the exact resource by name, so the specific
guide used as a UAV remains unproven. The resource-flag mismatch itself is
confirmed. The separate clear-value mismatch and simultaneous-access initial
state messages were warnings, not the fatal validation errors.

The debug layer **did** turn on under MO2. Its log showed both `Debug layer
REQUESTED` and `Debug layer ENABLED`; MO2 mapped the virtual game Data path to
the ini in `overwrite`. An earlier suspicion that the debug-layer ini lookup
could not find that file was disproved by the run.

## Change

In `src/Render/D3D12NeuralGBuffer.cpp`, `createTarget` now optionally combines
`ALLOW_RENDER_TARGET` with `ALLOW_UNORDERED_ACCESS`. The option is enabled for
the three RR guides (`normalRoughness`, `albedo`, `specularAlbedo`) and for
`upliftResult`, the direct NGX output. Other render targets retain their
previous flags.

`xmake build SkyrimUpscaler` succeeded. The rebuilt DLL was deployed to the
MO2 `SkyrimUpscaler` mod. The previous deployed DLL was preserved at
`C:\Claude\SkyrimUpscaler\build\SkyrimUpscaler.pre-uav-fix.dll`.

## Verification

The successful run's log is at
`C:\Users\kurif\OneDrive\ドキュメント\My Games\Skyrim Special Edition\SKSE\SkyrimUpscaler.log`.
It shows:

1. Debug layer requested and enabled at 11:50:26.
2. DLSS-NR feature creation submitted at 11:50:52.
3. RR evaluate succeeded, then the present override switched to the uplift
   target on the next frame.
4. Uplift reported 598/600 in the first interval (the intentional startup
   skips), then 600/600 in seven consecutive intervals through 11:51:54.
5. No `Evaluate failed`, device-removal, or D3D12 validation-error entry in
   that run. Three startup errors from Streamline's unavailable DLSS-NR plugin
   and ImGui feature checks remain; the direct NGX DLSS-NR path was used and
   continued running.

## Safety and follow-up

Frame generation is a separate risk: it previously caused GPU hangs and one
Windows `DPC_WATCHDOG_VIOLATION`. Read `docs/SAFETY.md` before touching its
present, watchdog, or cross-queue synchronization paths. The successful run
logged that the Steam overlay was present and frame generation was disabled.
Do not treat the NR/RR fix as proof that DLSS-G is safe.

The ini still requested the D3D12 debug layer during the successful run;
turn `D3D12DebugLayer` off for normal use after any further validation. This
handoff does not change the user's ini. Existing known gaps (synthetic RR
guides, missing DLSS-NR UI correction buffers and explicit jitter parameters,
resolution-change handling) were not addressed by this fix.
