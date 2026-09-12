#pragma once

#include <d3d11.h>
#include <d3d12.h>

#include <filesystem>
#include <string>
#include <vector>

// ===========================================================================
// Neural Rendering module
//
// Two capabilities, per the project brief:
//
//  (A) DLSS Ray Reconstruction (a.k.a. "DLSS-D", the neural denoiser). The
//      Streamline runtime DLLs for this ship with the project (sl.dlss_d.dll /
//      nvngx_dlssd.dll, and the ray-reconstruction model nvngx_dlssnr.dll).
//      This module owns the RR options and requests it through the Streamline
//      wrapper once the render backend (src/Render/Streamline.cpp) is wired in.
//
//  (B) A generic external-DLL loader that discovers "RenoDX-style" neural
//      rendering DLLs dropped into
//          Data/SKSE/Plugins/SkyrimUpscaler/Neural/
//      and drives them through the C ABI defined below. This lets a DLL built
//      on top of RenoDX (or any custom neural post-process) inject itself into
//      the frame without recompiling this plugin.
//
// The loader (B) is fully implemented here. The RR path (A) is scaffolded and
// documented; its GPU evaluation is completed alongside the Streamline backend
// port (see PORTING.md).
// ===========================================================================

extern "C"
{
	// -------- Neural module C ABI (v1) --------------------------------------
	// A neural rendering DLL exports a single C function:
	//
	//     const SkyrimUpscalerNeuralModuleV1* SkyrimUpscalerNeural_GetModuleV1(void);
	//
	// returning a static, immutable descriptor. All callbacks are optional
	// (may be null). The host guarantees calls happen on the render thread.

	struct SkyrimUpscalerNeuralHostInfo
	{
		uint32_t             structSize;      // sizeof(SkyrimUpscalerNeuralHostInfo)
		uint32_t             abiVersion;      // == SKYRIM_UPSCALER_NEURAL_ABI_V1
		ID3D11Device*        d3d11Device;     // game device (may be null early)
		ID3D11DeviceContext* d3d11Context;    // game immediate context
		ID3D12Device*        d3d12Device;     // interop device (null until proxy up)
		const wchar_t*       pluginDirectory; // Data/SKSE/Plugins/SkyrimUpscaler/
	};

	struct SkyrimUpscalerNeuralFrameInfo
	{
		uint32_t             structSize;
		uint32_t             frameIndex;
		float                renderWidth;
		float                renderHeight;
		float                displayWidth;
		float                displayHeight;
		ID3D11Texture2D*     color;           // in/out color for D3D11 modules
		ID3D11Texture2D*     depth;           // optional
		ID3D11Texture2D*     motionVectors;   // optional
	};

	// Returns 0 on success, non-zero on failure.
	using SkyrimUpscalerNeural_InitFn     = int (*)(const SkyrimUpscalerNeuralHostInfo*);
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

#define SKYRIM_UPSCALER_NEURAL_ABI_V1 1u

class NeuralRendering
{
public:
	static NeuralRendering* GetSingleton()
	{
		static NeuralRendering singleton;
		return &singleton;
	}

	struct Settings
	{
		bool enableRayReconstruction = false;  // (A) DLSS-RR / neural denoiser
		bool enableExternalModules   = true;   // (B) load RenoDX-style DLLs
	};

	Settings settings;

	// Discover + LoadLibrary + validate + Init all neural DLLs in the Neural
	// folder, and prepare the DLSS-RR request. Safe to call once device is up.
	void Initialize();

	// Run every loaded external module for this frame (B). Returns true if any
	// module ran. The DLSS-RR evaluation (A) is driven by the Streamline
	// backend; see EnableRayReconstruction().
	bool EvaluateExternalModules(const SkyrimUpscalerNeuralFrameInfo& a_frame);

	// (A) Whether DLSS Ray Reconstruction should be requested this frame.
	bool WantsRayReconstruction() const { return settings.enableRayReconstruction; }

	void Shutdown();

	[[nodiscard]] std::filesystem::path GetPluginDirectory() const;
	[[nodiscard]] std::filesystem::path GetNeuralDirectory() const;

private:
	struct LoadedModule
	{
		HMODULE                             handle = nullptr;
		const SkyrimUpscalerNeuralModuleV1* module = nullptr;
		std::wstring                        path;
		bool                                initialized = false;
	};

	void LoadExternalModules();

	std::vector<LoadedModule> loadedModules;
	bool                      initialized = false;
};
