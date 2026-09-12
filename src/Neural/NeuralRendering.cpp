#include "PCH.h"

#include "Neural/NeuralRendering.h"

#include "Game/Renderer.h"

#include <system_error>

namespace
{
	constexpr auto kGetModuleExport = "SkyrimUpscalerNeural_GetModuleV1";
	// ReShade addons (which RenoDX is built on) export this. We detect it so we
	// can give a precise diagnostic instead of silently ignoring the DLL.
	constexpr auto kReShadeExport = "ReShadeRegisterAddon";
}

std::filesystem::path NeuralRendering::GetPluginDirectory() const
{
	wchar_t buffer[MAX_PATH]{};
	// Resolve relative to THIS module so it is correct regardless of the game's
	// working directory.
	HMODULE self = nullptr;
	GetModuleHandleExW(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&kGetModuleExport),
		&self);
	if (self && GetModuleFileNameW(self, buffer, MAX_PATH) != 0) {
		return std::filesystem::path(buffer).parent_path() / L"SkyrimUpscaler";
	}
	// Fallback to the conventional install path.
	return std::filesystem::path(L"Data") / L"SKSE" / L"Plugins" / L"SkyrimUpscaler";
}

std::filesystem::path NeuralRendering::GetNeuralDirectory() const
{
	return GetPluginDirectory() / L"Neural";
}

void NeuralRendering::Initialize()
{
	if (initialized) {
		return;
	}
	initialized = true;

	if (settings.enableExternalModules) {
		LoadExternalModules();
	}

	if (settings.enableRayReconstruction) {
		// (A) DLSS Ray Reconstruction.
		//
		// PORTING NOTE: RR is requested through the Streamline wrapper by
		// selecting the DLSS-D (ray reconstruction) preset instead of plain
		// DLSS super resolution, and by tagging the additional guide buffers
		// (albedo, normals/roughness, specular hit distance). The Streamline
		// runtime DLLs are already bundled (sl.dlss_d.dll / nvngx_dlssd.dll /
		// nvngx_dlssnr.dll). Wire this to Streamline::GetSingleton() once the
		// render backend is compiled in. See PORTING.md section "DLSS-RR".
		logger::info("[Neural] DLSS Ray Reconstruction requested (evaluation pending Streamline backend)");
	}
}

void NeuralRendering::LoadExternalModules()
{
	const auto dir = GetNeuralDirectory();

	std::error_code ec;
	if (!std::filesystem::exists(dir, ec)) {
		logger::info("[Neural] No neural module directory at {} (skipping external modules)", dir.string());
		return;
	}

	logger::info("[Neural] Scanning for neural rendering modules in {}", dir.string());

	for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
		if (ec) {
			logger::warn("[Neural] Directory iteration error: {}", ec.message());
			break;
		}
		if (!entry.is_regular_file()) {
			continue;
		}
		const auto& path = entry.path();
		if (_wcsicmp(path.extension().c_str(), L".dll") != 0) {
			continue;
		}

		HMODULE handle = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (!handle) {
			logger::warn("[Neural] Failed to load '{}' (error {})", path.filename().string(), GetLastError());
			continue;
		}

		auto getModule = reinterpret_cast<SkyrimUpscalerNeural_GetModuleV1Fn>(
			GetProcAddress(handle, kGetModuleExport));

		if (!getModule) {
			if (GetProcAddress(handle, kReShadeExport)) {
				logger::warn(
					"[Neural] '{}' looks like a ReShade/RenoDX addon (exports {}). Hosting a raw "
					"ReShade addon requires the ReShade runtime; rebuild it to export {} instead.",
					path.filename().string(), kReShadeExport, kGetModuleExport);
			} else {
				logger::warn("[Neural] '{}' does not export {}; ignoring", path.filename().string(), kGetModuleExport);
			}
			FreeLibrary(handle);
			continue;
		}

		const SkyrimUpscalerNeuralModuleV1* mod = getModule();
		if (!mod || mod->abiVersion != SKYRIM_UPSCALER_NEURAL_ABI_V1 ||
			mod->structSize != sizeof(SkyrimUpscalerNeuralModuleV1)) {
			logger::warn("[Neural] '{}' returned an incompatible module descriptor; ignoring", path.filename().string());
			FreeLibrary(handle);
			continue;
		}

		LoadedModule loaded;
		loaded.handle = handle;
		loaded.module = mod;
		loaded.path = path.wstring();

		if (mod->Init) {
			SkyrimUpscalerNeuralHostInfo host{};
			host.structSize = sizeof(host);
			host.abiVersion = SKYRIM_UPSCALER_NEURAL_ABI_V1;
			host.d3d11Device = Game::GetD3D11Device();
			host.d3d11Context = Game::GetD3D11Context();
			host.d3d12Device = nullptr;  // populated once the DX12 proxy exists
			const auto pluginDir = GetPluginDirectory().wstring();
			host.pluginDirectory = pluginDir.c_str();

			const int rc = mod->Init(&host);
			if (rc != 0) {
				logger::warn("[Neural] Module '{}' Init() failed with code {}; unloading",
					mod->name ? mod->name : "?", rc);
				FreeLibrary(handle);
				continue;
			}
			loaded.initialized = true;
		}

		logger::info("[Neural] Loaded neural module '{}' v{}",
			mod->name ? mod->name : path.filename().string().c_str(),
			mod->version ? mod->version : "?");
		loadedModules.push_back(std::move(loaded));
	}

	logger::info("[Neural] {} external neural module(s) active", loadedModules.size());
}

bool NeuralRendering::EvaluateExternalModules(const SkyrimUpscalerNeuralFrameInfo& a_frame)
{
	bool ran = false;
	for (auto& loaded : loadedModules) {
		if (loaded.module && loaded.module->Evaluate) {
			loaded.module->Evaluate(&a_frame);
			ran = true;
		}
	}
	return ran;
}

void NeuralRendering::Shutdown()
{
	for (auto& loaded : loadedModules) {
		if (loaded.initialized && loaded.module && loaded.module->Shutdown) {
			loaded.module->Shutdown();
		}
		if (loaded.handle) {
			FreeLibrary(loaded.handle);
		}
	}
	loadedModules.clear();
	initialized = false;
}
