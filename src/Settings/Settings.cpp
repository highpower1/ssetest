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

	void GetFloat(const CSimpleIniA& a_ini, const char* a_key, float& a_out)
	{
		a_out = static_cast<float>(a_ini.GetDoubleValue(kSection, a_key, a_out));
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
	GetUInt(ini, "TransparencyHint", settings.transparencyHint);
	GetUInt(ini, "FrameGenWithSteamOverlay", settings.frameGenWithSteamOverlay);
	GetUInt(ini, "PresentOverride", settings.presentOverride);
	GetUInt(ini, "UICompositeDebug", settings.uiCompositeDebug);
	GetUInt(ini, "UICompositeDebugKey", settings.uiCompositeDebugKey);
	GetUInt(ini, "UpscalerHookPoint", settings.upscalerHookPoint);
	GetUInt(ini, "ENBGradeTransfer", settings.enbGradeTransfer);
	GetFloat(ini, "ENBGradeStrength", settings.enbGradeStrength);
	GetFloat(ini, "ENBGradeRadius", settings.enbGradeRadius);
	GetUInt(ini, "SceneTargetProbe", settings.sceneTargetProbe);
	GetUInt(ini, "SceneTargetProbeFrames", settings.sceneTargetProbeFrames);
	GetUInt(ini, "SceneTargetProbeMarkKey", settings.sceneTargetProbeMarkKey);
	GetUInt(ini, "UIMaskMode", settings.uiMaskMode);
	GetFloat(ini, "UIMaskThreshold", settings.uiMaskThreshold);
	GetFloat(ini, "UIMaskSoftness", settings.uiMaskSoftness);
	GetUInt(ini, "NeuralRayReconstruction", settings.neuralRayReconstruction);
	GetUInt(ini, "NeuralExternalModules", settings.neuralExternalModules);
	GetUInt(ini, "DLSSNREnabled", settings.dlssNREnabled);
	GetUInt(ini, "DLSSNRPreset", settings.dlssNRPreset);
	GetUInt(ini, "DLSSNRStyle", settings.dlssNRStyle);
	GetUInt(ini, "DLSSNRUseAutoMask", settings.dlssNRUseAutoMask);
	GetUInt(ini, "DLSSNRDebugBypass", settings.dlssNRDebugBypass);
	GetUInt(ini, "DLSSNRDebugDifference", settings.dlssNRDebugDifference);
	settings.dlssNRDebugDifferenceGain = static_cast<float>(ini.GetDoubleValue(kSection, "DLSSNRDebugDifferenceGain", settings.dlssNRDebugDifferenceGain));
	GetUInt(ini, "DLSSNRPassCount", settings.dlssNRPassCount);
	GetUInt(ini, "DLSSNRTemporal", settings.dlssNRTemporal);
	GetFloat(ini, "DLSSNRMotionScaleX", settings.dlssNRMotionScaleX);
	GetFloat(ini, "DLSSNRMotionScaleY", settings.dlssNRMotionScaleY);
	GetUInt(ini, "DLSSNRAfterUpscale", settings.dlssNRAfterUpscale);
	GetUInt(ini, "DLSSNREncoding", settings.dlssNREncoding);
	settings.dlssNRDiffuseWhiteNits = static_cast<float>(ini.GetDoubleValue(kSection, "DLSSNRDiffuseWhiteNits", settings.dlssNRDiffuseWhiteNits));
	settings.dlssNRIntensity = static_cast<float>(ini.GetDoubleValue(kSection, "DLSSNRIntensity", settings.dlssNRIntensity));
	settings.dlssNRLocalToneStrength = static_cast<float>(ini.GetDoubleValue(kSection, "DLSSNRLocalToneStrength", settings.dlssNRLocalToneStrength));
	settings.dlssNRLocalStructureStrength = static_cast<float>(ini.GetDoubleValue(kSection, "DLSSNRLocalStructureStrength", settings.dlssNRLocalStructureStrength));
	settings.dlssNRSkinStructureStrength = static_cast<float>(ini.GetDoubleValue(kSection, "DLSSNRSkinStructureStrength", settings.dlssNRSkinStructureStrength));

	std::error_code ec;
	lastWriteTime = std::filesystem::last_write_time(path, ec);

	logger::info("[Settings] Loaded from {}", path.string());
	logger::info("[Settings] Neural Rendering: enabled={} style={} preset={} passes={} autoMask={} order={} encoding={} intensity={:.2f} localTone={:.2f} localStructure={:.2f} skin={:.2f}",
		settings.dlssNREnabled, settings.dlssNRStyle, settings.dlssNRPreset, settings.dlssNRPassCount,
		settings.dlssNRUseAutoMask, settings.dlssNRAfterUpscale ? "after" : "before", settings.dlssNREncoding,
		settings.dlssNRIntensity, settings.dlssNRLocalToneStrength,
		settings.dlssNRLocalStructureStrength, settings.dlssNRSkinStructureStrength);
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
	ini.SetLongValue(kSection, "TransparencyHint", static_cast<long>(a_settings.transparencyHint));
	ini.SetLongValue(kSection, "FrameGenWithSteamOverlay", static_cast<long>(a_settings.frameGenWithSteamOverlay));
	ini.SetLongValue(kSection, "PresentOverride", static_cast<long>(a_settings.presentOverride));
	ini.SetLongValue(kSection, "UICompositeDebug", static_cast<long>(a_settings.uiCompositeDebug));
	ini.SetLongValue(kSection, "UICompositeDebugKey", static_cast<long>(a_settings.uiCompositeDebugKey));
	ini.SetLongValue(kSection, "UpscalerHookPoint", static_cast<long>(a_settings.upscalerHookPoint));
	ini.SetLongValue(kSection, "ENBGradeTransfer", static_cast<long>(a_settings.enbGradeTransfer));
	ini.SetDoubleValue(kSection, "ENBGradeStrength", a_settings.enbGradeStrength);
	ini.SetDoubleValue(kSection, "ENBGradeRadius", a_settings.enbGradeRadius);
	ini.SetLongValue(kSection, "SceneTargetProbe", static_cast<long>(a_settings.sceneTargetProbe));
	ini.SetLongValue(kSection, "SceneTargetProbeFrames", static_cast<long>(a_settings.sceneTargetProbeFrames));
	ini.SetLongValue(kSection, "SceneTargetProbeMarkKey", static_cast<long>(a_settings.sceneTargetProbeMarkKey));
	ini.SetLongValue(kSection, "UIMaskMode", static_cast<long>(a_settings.uiMaskMode));
	ini.SetDoubleValue(kSection, "UIMaskThreshold", a_settings.uiMaskThreshold);
	ini.SetDoubleValue(kSection, "UIMaskSoftness", a_settings.uiMaskSoftness);
	ini.SetLongValue(kSection, "NeuralRayReconstruction", static_cast<long>(a_settings.neuralRayReconstruction));
	ini.SetLongValue(kSection, "NeuralExternalModules", static_cast<long>(a_settings.neuralExternalModules));
	ini.SetLongValue(kSection, "DLSSNREnabled", static_cast<long>(a_settings.dlssNREnabled));
	ini.SetLongValue(kSection, "DLSSNRPreset", static_cast<long>(a_settings.dlssNRPreset));
	ini.SetLongValue(kSection, "DLSSNRStyle", static_cast<long>(a_settings.dlssNRStyle));
	ini.SetLongValue(kSection, "DLSSNRUseAutoMask", static_cast<long>(a_settings.dlssNRUseAutoMask));
	ini.SetLongValue(kSection, "DLSSNRDebugBypass", static_cast<long>(a_settings.dlssNRDebugBypass));
	ini.SetLongValue(kSection, "DLSSNRDebugDifference", static_cast<long>(a_settings.dlssNRDebugDifference));
	ini.SetDoubleValue(kSection, "DLSSNRDebugDifferenceGain", a_settings.dlssNRDebugDifferenceGain);
	ini.SetLongValue(kSection, "DLSSNRPassCount", static_cast<long>(a_settings.dlssNRPassCount));
	ini.SetLongValue(kSection, "DLSSNRTemporal", static_cast<long>(a_settings.dlssNRTemporal));
	ini.SetDoubleValue(kSection, "DLSSNRMotionScaleX", a_settings.dlssNRMotionScaleX);
	ini.SetDoubleValue(kSection, "DLSSNRMotionScaleY", a_settings.dlssNRMotionScaleY);
	ini.SetLongValue(kSection, "DLSSNRAfterUpscale", static_cast<long>(a_settings.dlssNRAfterUpscale));
	ini.SetLongValue(kSection, "DLSSNREncoding", static_cast<long>(a_settings.dlssNREncoding));
	ini.SetDoubleValue(kSection, "DLSSNRDiffuseWhiteNits", a_settings.dlssNRDiffuseWhiteNits);
	ini.SetDoubleValue(kSection, "DLSSNRIntensity", a_settings.dlssNRIntensity);
	ini.SetDoubleValue(kSection, "DLSSNRLocalToneStrength", a_settings.dlssNRLocalToneStrength);
	ini.SetDoubleValue(kSection, "DLSSNRLocalStructureStrength", a_settings.dlssNRLocalStructureStrength);
	ini.SetDoubleValue(kSection, "DLSSNRSkinStructureStrength", a_settings.dlssNRSkinStructureStrength);

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
