#pragma once

#include <cstdint>

// ===========================================================================
// SceneTargetProbe
//
// ENB grades a scene that is not the one we hand it, and writing into kMAIN
// was proven not to reach it. Rather than guess which of the engine's 114
// render targets ENB actually consumes, fill one per interval with flat
// magenta and watch for magenta in ENB's own output (the "Pre-UI capture"
// composite view). The target that turns the screen magenta is the one ENB
// reads, and is therefore where our upscaled scene has to be written.
//
// Off unless asked for. The mark key writes the currently probed target to
// the log, so the answer is an exact name rather than a guessed timestamp.
// ===========================================================================
namespace SceneTargetProbe
{
	// Clears the currently probed render target. Called from the pre-UI hook,
	// before the rest of the frame runs.
	void Tick();

	// Logs the target being probed right now as a hit.
	void Mark();

	[[nodiscard]] const char* CurrentName();
}
