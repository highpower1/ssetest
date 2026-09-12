#include "PCH.h"

#include "Neural/NeuralRendering.h"

#include "Game/Renderer.h"
#include "Render/Streamline.h"
#include "Upscaler/D3D12Upscaler.h"
#include "Upscaler/Upscaling.h"

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
	} else {
		logger::info("[Neural] External neural modules disabled in settings");
	}

	// (A) DLSS Ray Reconstruction is requested from Streamline itself
	// (sl::kFeatureDLSS_RR is now in the D3D12 featuresToLoad list); availability
	// is reported by Streamline::CheckFeatures into featureDLSSD/dlssdStatus and
	// surfaced in the menu. Nothing to do here beyond logging intent.
	if (settings.enableRayReconstruction) {
		logger::info("[Neural] DLSS Ray Reconstruction enabled in settings");
	}
}

std::string NeuralRendering::GetModuleName(std::size_t a_index) const
{
	return a_index < loadedModules.size() ? loadedModules[a_index].name : std::string{};
}

std::string NeuralRendering::GetModuleVersion(std::size_t a_index) const
{
	return a_index < loadedModules.size() ? loadedModules[a_index].version : std::string{};
}

void NeuralRendering::LoadExternalModules()
{
	const auto dir = GetNeuralDirectory();

	std::error_code ec;
	if (!std::filesystem::exists(dir, ec)) {
		// Create it so players have somewhere obvious to drop a module.
		std::error_code createEc;
		std::filesystem::create_directories(dir, createEc);
		logger::info("[Neural] No neural module directory at {} ({})", dir.string(),
			createEc ? "could not create it" : "created it; drop neural DLLs here");
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
			++rejectedModules;
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

		const SkyrimUpscalerNeuralModuleV1* mod = nullptr;
		try {
			mod = getModule();
		} catch (...) {
			mod = nullptr;
		}
		if (!mod || mod->abiVersion != SKYRIM_UPSCALER_NEURAL_ABI_V1 ||
			mod->structSize != sizeof(SkyrimUpscalerNeuralModuleV1)) {
			logger::warn("[Neural] '{}' returned an incompatible module descriptor; ignoring", path.filename().string());
			++rejectedModules;
			FreeLibrary(handle);
			continue;
		}

		LoadedModule loaded;
		loaded.handle = handle;
		loaded.module = mod;
		loaded.path = path.wstring();
		loaded.name = mod->name ? mod->name : path.filename().string();
		loaded.version = mod->version ? mod->version : "?";

		logger::info("[Neural] Discovered neural module '{}' v{} ({})",
			loaded.name, loaded.version, path.filename().string());
		loadedModules.push_back(std::move(loaded));
	}

	logger::info("[Neural] {} external neural module(s) discovered, {} rejected", loadedModules.size(), rejectedModules);
}

bool NeuralRendering::InitializeModules()
{
	if (modulesInitialized || loadedModules.empty()) {
		modulesInitialized = true;
		return true;
	}

	auto* device11 = Game::GetD3D11Device();
	auto* context11 = Game::GetD3D11Context();
	if (!device11 || !context11) {
		return false;  // try again next frame
	}

	// May legitimately be null when the D3D12 proxy is off; modules must cope.
	auto* device12 = D3D12Upscaler::GetSingleton()->GetD3D12Device();

	const auto pluginDir = GetPluginDirectory().wstring();

	for (auto& loaded : loadedModules) {
		if (loaded.initialized || loaded.failed || !loaded.module) {
			continue;
		}
		if (!loaded.module->Init) {
			loaded.initialized = true;
			continue;
		}

		SkyrimUpscalerNeuralHostInfo host{};
		host.structSize = sizeof(host);
		host.abiVersion = SKYRIM_UPSCALER_NEURAL_ABI_V1;
		host.d3d11Device = device11;
		host.d3d11Context = context11;
		host.d3d12Device = device12;
		host.pluginDirectory = pluginDir.c_str();

		int rc = -1;
		try {
			rc = loaded.module->Init(&host);
		} catch (...) {
			logger::error("[Neural] Module '{}' threw from Init(); disabling it", loaded.name);
			loaded.failed = true;
			continue;
		}

		if (rc != 0) {
			logger::warn("[Neural] Module '{}' Init() failed with code {}; disabling it", loaded.name, rc);
			loaded.failed = true;
			continue;
		}

		loaded.initialized = true;
		logger::info("[Neural] Module '{}' v{} initialized (d3d12Device={})",
			loaded.name, loaded.version, static_cast<const void*>(device12));
	}

	modulesInitialized = true;
	return true;
}

bool NeuralRendering::OnFrame()
{
	if (!initialized || !settings.enableExternalModules || loadedModules.empty()) {
		return false;
	}

	if (!modulesInitialized && !InitializeModules()) {
		return false;
	}

	auto* color = Game::GetRenderTargetTexture(RE::RENDER_TARGET::kMAIN);
	if (!color) {
		return false;
	}

	const auto* up = Upscaling::GetSingleton();

	SkyrimUpscalerNeuralFrameInfo frame{};
	frame.structSize = sizeof(frame);
	frame.frameIndex = frameIndex++;
	frame.renderWidth = up->osdRenderSize.x;
	frame.renderHeight = up->osdRenderSize.y;
	frame.displayWidth = up->osdNativeSize.x;
	frame.displayHeight = up->osdNativeSize.y;
	frame.color = color;
	frame.depth = Game::GetRenderTargetTexture(RE::RENDER_TARGET::kSAO_CAMERAZ);
	frame.motionVectors = Game::GetRenderTargetTexture(RE::RENDER_TARGET::kMOTION_VECTOR);

	return EvaluateExternalModules(frame);
}

bool NeuralRendering::EvaluateExternalModules(const SkyrimUpscalerNeuralFrameInfo& a_frame)
{
	bool ran = false;
	for (auto& loaded : loadedModules) {
		if (loaded.failed || !loaded.initialized || !loaded.module || !loaded.module->Evaluate) {
			continue;
		}
		// A third-party DLL runs inside the game's render hook. An exception
		// escaping here would unwind through engine code, so contain it and
		// permanently drop the offending module instead.
		try {
			loaded.module->Evaluate(&a_frame);
			ran = true;
		} catch (...) {
			logger::error("[Neural] Module '{}' threw from Evaluate(); disabling it for this session", loaded.name);
			loaded.failed = true;
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
	modulesInitialized = false;
	rejectedModules = 0;
}
