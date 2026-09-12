#pragma once

namespace UpscalerHooks
{
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
