#pragma once

// ===========================================================================
// Diagnostics probe.
//
// Dumps everything the SkyrimUpscaler port needs from a real running game to
// the SkyrimUpscaler log:
//   * runtime version + module base
//   * BSGraphics::RendererData (device / context / swapchain / screen size)
//   * every RE::RENDER_TARGET slot: name, resolution, DXGI format, and which
//     views (RTV/SRV/UAV) exist -- this is the ground truth for the
//     render-target scaling port
//   * every depth-stencil slot
//   * ENB presence
//
// Safe to call any time after the renderer is initialized (kDataLoaded).
// Everything is null-checked and wrapped so a probe failure never crashes.
// ===========================================================================

namespace Diagnostics
{
	// One full dump to the log. a_phase is a short label (e.g. "kDataLoaded").
	// Call only from the main thread (SKSE messaging callbacks) -- reading the
	// live renderer from a background thread races the game's render thread.
	//
	// a_inWorld: true only when a save/new game is actually loaded. The camera
	// dump (RE::Main::WorldRootCamera) is skipped otherwise, because at the main
	// menu that pointer is not valid and dereferencing it crashes.
	void DumpAll(const char* a_phase, bool a_inWorld = false);
}
