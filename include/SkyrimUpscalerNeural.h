#pragma once

// ===========================================================================
// SkyrimUpscaler -- Neural Rendering module ABI (v1)
//
// Public, standalone header for authors of neural rendering DLLs. Copy this
// file into your project; it needs nothing from SkyrimUpscaler but <d3d11.h>
// and <d3d12.h>.
//
// A module is an ordinary DLL dropped into
//     Data/SKSE/Plugins/SkyrimUpscaler/Neural/
// that exports exactly one function:
//
//     extern "C" __declspec(dllexport)
//     const SkyrimUpscalerNeuralModuleV1* SkyrimUpscalerNeural_GetModuleV1(void);
//
// returning a static, immutable descriptor. All three callbacks are optional
// (may be null).
//
// Lifecycle
// ---------
//   Init      -- called once, on the first rendered frame, so both the game's
//                D3D11 device and (when the D3D12 proxy path is on) the
//                plugin's D3D12 device are live. d3d12Device may still be null;
//                handle that. Return 0 for success, non-zero to be unloaded.
//   Evaluate  -- called once per frame on the render thread, from inside the
//                game's draw call, after the scene is rendered and before the
//                HUD. The colour buffer is hud-less and at render resolution,
//                so whatever you write is what DLSS/FSR then upscales.
//   Shutdown  -- called once at plugin teardown.
//
// Rules
// -----
//   * Do not block and do not present in Evaluate.
//   * Save and restore any D3D11 device-context state you change -- the engine
//     keeps drawing with that context the moment you return.
//   * `color` is bound as a render target by the engine, so you cannot read and
//     write it in one pass; copy it to your own scratch texture first.
//   * If Init fails or Evaluate throws, the module is dropped for the session
//     and the reason is written to SkyrimUpscaler.log.
// ===========================================================================

#include <d3d11.h>
#include <d3d12.h>
#include <cstdint>

#define SKYRIM_UPSCALER_NEURAL_ABI_V1 1u

extern "C"
{
	struct SkyrimUpscalerNeuralHostInfo
	{
		uint32_t             structSize;      // sizeof(SkyrimUpscalerNeuralHostInfo)
		uint32_t             abiVersion;      // == SKYRIM_UPSCALER_NEURAL_ABI_V1
		ID3D11Device*        d3d11Device;     // game device
		ID3D11DeviceContext* d3d11Context;    // game immediate context
		ID3D12Device*        d3d12Device;     // interop device (may be null)
		const wchar_t*       pluginDirectory; // Data/SKSE/Plugins/SkyrimUpscaler/
	};

	struct SkyrimUpscalerNeuralFrameInfo
	{
		uint32_t         structSize;
		uint32_t         frameIndex;
		float            renderWidth;    // scene render resolution
		float            renderHeight;
		float            displayWidth;   // post-upscale output resolution
		float            displayHeight;
		ID3D11Texture2D* color;          // in/out scene colour (hud-less)
		ID3D11Texture2D* depth;          // linear view-space Z, may be null
		ID3D11Texture2D* motionVectors;  // may be null
	};

	// Return 0 on success, non-zero on failure.
	using SkyrimUpscalerNeural_InitFn = int (*)(const SkyrimUpscalerNeuralHostInfo*);
	using SkyrimUpscalerNeural_EvaluateFn = int (*)(const SkyrimUpscalerNeuralFrameInfo*);
	using SkyrimUpscalerNeural_ShutdownFn = void (*)(void);

	struct SkyrimUpscalerNeuralModuleV1
	{
		uint32_t                        structSize;   // sizeof(this)
		uint32_t                        abiVersion;   // SKYRIM_UPSCALER_NEURAL_ABI_V1
		const char*                     name;         // human-readable, UTF-8
		const char*                     version;      // module version string
		SkyrimUpscalerNeural_InitFn     Init;
		SkyrimUpscalerNeural_EvaluateFn Evaluate;
		SkyrimUpscalerNeural_ShutdownFn Shutdown;
	};

	using SkyrimUpscalerNeural_GetModuleV1Fn = const SkyrimUpscalerNeuralModuleV1* (*)(void);
}
