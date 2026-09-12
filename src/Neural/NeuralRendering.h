#pragma once

#include <d3d11.h>
#include <d3d12.h>

#include <cstdint>
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

// The module ABI itself lives in a standalone public header so DLL authors can
// build against it without any of this project's sources.
#include "SkyrimUpscalerNeural.h"

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

	// Discover + LoadLibrary + validate all neural DLLs in the Neural folder.
	// Module Init() is deferred to the first frame (see OnFrame) so the D3D11 and
	// D3D12 devices handed to a module are both live.
	void Initialize();

	// Called once per frame from the pre-UI render hook (Hook_MainDrawWorld), i.e.
	// after the scene is rendered and before the HUD is drawn -- the point a
	// RenoDX-style tone-mapping / neural post-process module wants. Runs every
	// loaded external module (B). Returns true if any module ran.
	bool OnFrame();

	// Run every loaded external module for this frame (B). Returns true if any
	// module ran. The DLSS-RR evaluation (A) is driven by the Streamline backend.
	bool EvaluateExternalModules(const SkyrimUpscalerNeuralFrameInfo& a_frame);

	// (A) Whether DLSS Ray Reconstruction should be requested this frame.
	bool WantsRayReconstruction() const { return settings.enableRayReconstruction; }

	void Shutdown();

	// ---- status, for the SMF menu ---------------------------------------
	[[nodiscard]] std::size_t GetModuleCount() const { return loadedModules.size(); }
	[[nodiscard]] std::string GetModuleName(std::size_t a_index) const;
	[[nodiscard]] std::string GetModuleVersion(std::size_t a_index) const;
	[[nodiscard]] std::uint32_t GetRejectedModuleCount() const { return rejectedModules; }

	[[nodiscard]] std::filesystem::path GetPluginDirectory() const;
	[[nodiscard]] std::filesystem::path GetNeuralDirectory() const;

private:
	struct LoadedModule
	{
		HMODULE                             handle = nullptr;
		const SkyrimUpscalerNeuralModuleV1* module = nullptr;
		std::wstring                        path;
		std::string                         name;
		std::string                         version;
		bool                                initialized = false;
		bool                                failed = false;  // Init() failed / Evaluate() faulted; skip it
	};

	void LoadExternalModules();
	// Deferred module Init(); returns true once every module has been given its
	// chance (successfully or not).
	bool InitializeModules();

	std::vector<LoadedModule> loadedModules;
	bool                      initialized = false;
	bool                      modulesInitialized = false;
	std::uint32_t             rejectedModules = 0;
	std::uint32_t             frameIndex = 0;
};
