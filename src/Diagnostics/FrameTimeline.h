#pragma once

#include <cstdint>

// ===========================================================================
// FrameTimeline
//
// Plan A needs a point in the frame after the scene is rendered into kMAIN and
// before ENB reads it. The magenta sweep established that ENB is upstream of
// our pre-UI hook, but not where. Rather than hook game call sites blind --
// whose signatures we do not know, and which can corrupt the frame if the
// arguments are not forwarded exactly -- intercept a D3D11 method whose
// signature is exact, and just record.
//
// One frame of ID3D11DeviceContext::OMSetRenderTargets, each render target
// resolved to its RE::RENDER_TARGET name, with our own hook marked in the same
// sequence. That says where ENB writes kFRAMEBUFFER relative to everything
// else, and whether ENB's calls reach the context we hooked at all.
// ===========================================================================
namespace FrameTimeline
{
	// Hooks the context's OMSetRenderTargets. Safe to call more than once.
	void Install();

	// Records one frame's worth of binds on the next frame.
	void Arm();

	// Writes a labelled marker into the running capture.
	void Mark(const char* a_label);

	// Called once per frame from the pre-UI hook so the capture has a frame edge.
	void OnFrameBoundary();

	// Invoked at the first render-target bind of the frame that belongs to the
	// post-processing chain -- the moment the scene is finished in kMAIN and
	// nothing has consumed it yet. This is the point the upscaler has to run at
	// for ENB to grade its output rather than the game's. Registered by the
	// render hooks; the callback runs on the render thread, inside
	// OMSetRenderTargets and before the original call.
	void SetSceneCompleteCallback(void (*a_callback)());
}
