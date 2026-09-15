#pragma once

namespace UpscalerHooks
{
	// The scale the engine actually rendered this frame at, read back from the
	// same fields the dynamic-resolution override writes. Returns 1.0 when those
	// fields are not trustworthy on this build or the override is not running.
	//
	// The upscaler must size its input rectangle from this rather than from the
	// scale it asked for. When a write does not take, the scene is drawn at full
	// size while the upscaler crops the top-left fraction of it and stretches
	// that to the display -- which looks exactly like the screen zooming in, and
	// was reported as such.
	[[nodiscard]] float EffectiveRenderScale();

	// Latches the scale for the frame. Called once, at the top of the upscaler's
	// evaluation, before anything asks for a size.
	void SampleRenderScale();

	// Installs the Skyrim render-pipeline hooks used to drive upscaling.
	//
	// Hook points are taken from PureDark/Skyrim-Upscaler (2022) and VERIFIED
	// on the target runtime (SkyrimUpscalerProbe v0.3 confirmed each site holds
	// a `call` on 1.6.1170):
	//   * BSGraphics::Renderer InitD3D   RelocationID(75595, 77226) +0x50/+0x2BC
	//   * BSGraphics::Renderer UpdateJitter RelocationID(75460, 77245) +0xE5/+0xE2
	//   * Main::DrawWorld (pre-UI)        RelocationID(79947, 82084) +0x16F/+0x17A
	//
	// This first pass is SAFE/observational: the thunks call the original and
	// (in-world) capture the camera + advance the frame counter, but do NOT yet
	// inject jitter, patch the game's jitter, or run the upscaler. That keeps
	// rendering unchanged so we can confirm the hooks fire correctly in-game
	// before enabling the image-altering paths.
	void Install();
}
