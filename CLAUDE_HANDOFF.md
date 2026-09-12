# Claude Handoff — SkyrimUpscaler DLSS Frame Generation

Updated: 2026-09-12 (JST)

## Read first

Read `CODEX_HANDOFF.md` for the full architecture, hook locations, port history,
and feature matrix. This file is the short operational handoff for safely
continuing DLSS Frame Generation work.

Repository: `C:\Claude\SkyrimUpscaler`

## Current state

- FSR and DLSS Super Resolution build and were previously working.
- DLSS Frame Generation plumbing exists but is intentionally disabled.
- `Upscaling::kFrameGenExperiment` is `false`.
- `Upscaling::kEnableDLSSG` is `false`.
- Do not flip either gate merely to see whether the crash is gone.
- The current DLL is suitable only for non-FG regression testing.

The safety-gated artifact built from the integrated source is:

`C:\Claude\SkyrimUpscaler\dist\SkyrimUpscaler-dev-safety-gated.dll`

SHA-256 at handoff time:

`AF5C37B3275DFE007E8D1FA553FBE45BE0BBDA47F2A13C75C53499232BD8A8FE`

## Safety incident

An earlier DLSS-G test caused a machine-wide GPU failure rather than a normal
game crash. Windows recorded:

- bugcheck `0x133 DPC_WATCHDOG_VIOLATION`;
- `LiveKernelEvent 0x141` involving `nvlddmkm.sys`;
- dump: `C:\Windows\Minidump\091226-9718-01.dmp`.

The Skyrim log is normally located at:

`C:\Users\kurif\OneDrive\ドキュメント\My Games\Skyrim Special Edition\SKSE\SkyrimUpscaler.log`

Do not distribute or reinstall the removed `SkyrimUpscaler-dlssg-sync-test.dll`.

## Safety fixes now in source

The audit found several independent GPU lifetime and synchronization hazards:

1. `D3D12Upscaler` reused a command allocator before its previous GPU submission
   was guaranteed complete. A dedicated completion fence and five-second timeout
   now protect allocator reset.
2. The next D3D11 frame could overwrite shared color, motion-vector, and depth
   resources while the Present queue was still copying them. A dedicated
   Present-consumption shared fence now provides the reverse dependency.
3. Per-backbuffer DLSS-G input slots could be overwritten before Streamline
   finished consuming them. `slDLSSGGetState` is queried on the Present thread;
   its `inputsProcessingCompletionFence` and value are retained per slot and
   waited before reuse.
4. Partial DLSS-G input-buffer creation could expose a set containing null
   resources. Buffer creation is now transactional: all slots succeed before
   any are published.
5. Queue waits and signals now check their results. Device removal fails fast.
   GPU waits are bounded rather than infinite.
6. Exceptions are contained at the DXGI `Present`/`Present1` ABI boundary.
   `Present1` parameters are forwarded instead of discarded.
7. `ResizeBuffers` now performs a drained recreation: DLSS-G is disabled,
   Streamline and D3D11/D3D12 work is drained, old backbuffers are released, the
   real swapchain is resized, and shared presentation resources are rebuilt.
   The proxy intentionally accepts only its fixed three-buffer configuration.
8. Shared Win32 handles use RAII so failure paths do not leak them.
9. The Streamline frame token used for DLSS-SR evaluation is carried through to
   DLSS-G resource tags and Reflex Present markers.

Primary files:

- `src/Upscaler/D3D12Upscaler.cpp` / `.h`
- `src/Render/DX12SwapChain.cpp` / `.h`
- `src/Render/Streamline.cpp` / `.h`
- `src/Render/DX11Hooks.cpp`
- `src/Upscaler/Upscaling.cpp` / `.h`
- `src/UI/Menu.cpp`

## Verification already completed

- Full releasedbg build succeeds with Visual Studio 2026 and xmake.
- `git diff --check` reports no whitespace errors.
- No unbounded `WaitForSingleObject*` calls remain in active project source.
- Both FG compile-time safety gates remain `false` after the build.
- Only the pre-existing third-party SKSE Menu Framework warnings were observed.

Build command from a Visual Studio x64 developer environment:

```powershell
cd C:\Claude\SkyrimUpscaler
xmake build SkyrimUpscaler
```

Output:

`build\windows\x64\releasedbg\SkyrimUpscaler.dll`

## Required next steps

Keep FG disabled and validate the safety build first:

1. Start Skyrim with upscaling disabled, then FSR, then DLSS-SR.
2. Test window focus changes and repeated Alt-Tab transitions.
3. Test every supported window-size or resolution-change path.
4. Run an extended DLSS-SR soak test and inspect the SKSE log and Windows event
   log for TDR, device removal, or fence timeout messages.
5. Exercise the proxy path under the D3D12 debug layer, and preferably GPU-based
   validation, PIX, or Nsight, without enabling DLSS-G.
6. Only after those checks pass, create a separately named internal FG build and
   test it on a recoverable or disposable setup. Start with one generated frame,
   no dynamic MFG, no runtime toggling, and no ENB/UI recomposition changes.

Do not replace the safety-gated DLL with an FG-enabled build on the primary test
machine until the debug-layer run is clean. A successful compile is not evidence
that the asynchronous Present path is safe.
