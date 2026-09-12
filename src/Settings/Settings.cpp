#include "PCH.h"

#include "Settings/Settings.h"

#include <SimpleIni.h>

#include <system_error>

namespace
{
	constexpr auto kSection = "General";

	template <class T>
	void GetUInt(const CSimpleIniA& a_ini, const char* a_key, T& a_out)
	{
		const long v = a_ini.GetLongValue(kSection, a_key, static_cast<long>(a_out));
		a_out = static_cast<T>(v);
	}
}

std::filesystem::path SettingsStore::GetIniPath() const
{
	return std::filesystem::path(L"Data") / L"SKSE" / L"Plugins" / L"SkyrimUpscaler" / L"SkyrimUpscaler.ini";
}

void SettingsStore::Load()
{
	std::scoped_lock lock(mutex);

	const auto path = GetIniPath();

	CSimpleIniA ini;
	ini.SetUnicode();
	const SI_Error rc = ini.LoadFile(path.string().c_str());
	if (rc < 0) {
		logger::info("[Settings] No settings file at {} -- using defaults", path.string());
		return;
	}

	GetUInt(ini, "UpscaleMethod", settings.upscaleMethodPreference);
	GetUInt(ini, "QualityMode", settings.qualityMode);
	GetUInt(ini, "FrameGenerationMode", settings.frameGenerationMode);
	GetUInt(ini, "GeneratedFrames", settings.dlssgGeneratedFrames);
	GetUInt(ini, "DynamicMFGEnabled", settings.dynamicMFGEnabled);
	GetUInt(ini, "DynamicMFGTargetFPS", settings.dynamicMFGTargetFPS);
	GetUInt(ini, "ReflexMode", settings.reflexMode);
	GetUInt(ini, "DLSSModelPreset", settings.dlssModelPreset);
	GetUInt(ini, "OSDMode", settings.osdMode);
	GetUInt(ini, "TaggedTextureDebug", settings.taggedTextureDebug);
	GetUInt(ini, "ImageSpaceEffectLog", settings.imageSpaceEffectLog);
	settings.sharpness = static_cast<float>(ini.GetDoubleValue(kSection, "Sharpness", settings.sharpness));
	GetUInt(ini, "NeuralRayReconstruction", settings.neuralRayReconstruction);
	GetUInt(ini, "NeuralExternalModules", settings.neuralExternalModules);

	std::error_code ec;
	lastWriteTime = std::filesystem::last_write_time(path, ec);

	logger::info("[Settings] Loaded from {}", path.string());
}

bool SettingsStore::Save(const Settings& a_settings)
{
	std::scoped_lock lock(mutex);

	const auto path = GetIniPath();

	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);

	CSimpleIniA ini;
	ini.SetUnicode();
	ini.LoadFile(path.string().c_str());  // preserve any unknown keys

	ini.SetLongValue(kSection, "UpscaleMethod", static_cast<long>(a_settings.upscaleMethodPreference));
	ini.SetLongValue(kSection, "QualityMode", static_cast<long>(a_settings.qualityMode));
	ini.SetLongValue(kSection, "FrameGenerationMode", static_cast<long>(a_settings.frameGenerationMode));
	ini.SetLongValue(kSection, "GeneratedFrames", static_cast<long>(a_settings.dlssgGeneratedFrames));
	ini.SetLongValue(kSection, "DynamicMFGEnabled", static_cast<long>(a_settings.dynamicMFGEnabled));
	ini.SetLongValue(kSection, "DynamicMFGTargetFPS", static_cast<long>(a_settings.dynamicMFGTargetFPS));
	ini.SetLongValue(kSection, "ReflexMode", static_cast<long>(a_settings.reflexMode));
	ini.SetLongValue(kSection, "DLSSModelPreset", static_cast<long>(a_settings.dlssModelPreset));
	ini.SetLongValue(kSection, "OSDMode", static_cast<long>(a_settings.osdMode));
	ini.SetLongValue(kSection, "TaggedTextureDebug", static_cast<long>(a_settings.taggedTextureDebug));
	ini.SetLongValue(kSection, "ImageSpaceEffectLog", static_cast<long>(a_settings.imageSpaceEffectLog));
	ini.SetDoubleValue(kSection, "Sharpness", a_settings.sharpness);
	ini.SetLongValue(kSection, "NeuralRayReconstruction", static_cast<long>(a_settings.neuralRayReconstruction));
	ini.SetLongValue(kSection, "NeuralExternalModules", static_cast<long>(a_settings.neuralExternalModules));

	const SI_Error rc = ini.SaveFile(path.string().c_str());
	if (rc < 0) {
		logger::error("[Settings] Failed to save to {}", path.string());
		return false;
	}

	settings = a_settings;
	lastWriteTime = std::filesystem::last_write_time(path, ec);
	logger::info("[Settings] Saved to {}", path.string());
	return true;
}

void SettingsStore::ReloadIfChanged()
{
	const auto path = GetIniPath();
	std::error_code ec;
	const auto current = std::filesystem::last_write_time(path, ec);
	if (ec) {
		return;
	}
	if (current != lastWriteTime) {
		Load();
	}
}
