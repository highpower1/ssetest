#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>

// ===========================================================================
// Settings store.
//
// Decouples the UI (SKSE Menu Framework 3) from the not-yet-ported upscaler
// core. The menu reads/writes this store; once Upscaling.cpp is ported it
// reads the very same struct, so nothing about the menu has to change.
//
// The field set mirrors the Fallout 4 project's Upscaling::Settings so the
// future port is a drop-in.
// ===========================================================================

class SettingsStore
{
public:
	static SettingsStore* GetSingleton()
	{
		static SettingsStore singleton;
		return &singleton;
	}

	enum class UpscaleMethod : uint32_t
	{
		kDisabled = 0,
		kFSR = 1,
		kDLSS = 2,
	};

	struct Settings
	{
		uint32_t upscaleMethodPreference = static_cast<uint32_t>(UpscaleMethod::kDLSS);
		uint32_t qualityMode = 1;             // 0=Native AA,1=Quality,2=Balanced,3=Perf,4=Ultra Perf
		uint32_t frameGenerationMode = 0;     // 0=Disabled,1=On,2=Auto
		uint32_t dlssgGeneratedFrames = 0;    // 0=2x ... 4=6x
		uint32_t dynamicMFGEnabled = 0;
		uint32_t dynamicMFGTargetFPS = 300;
		uint32_t reflexMode = 1;              // 0=Off,1=On,2=On+Boost
		uint32_t dlssModelPreset = 0;         // 0=Recommended,1=Default,2=K,3=M,4=L
		uint32_t osdMode = 0;                 // 0=Off,1=Compact,2=Detailed
		uint32_t taggedTextureDebug = 0;
		uint32_t imageSpaceEffectLog = 0;
		float    sharpness = 0.2f;

		// Neural rendering (this project's additions)
		uint32_t neuralRayReconstruction = 0; // DLSS-D / RR
		uint32_t neuralExternalModules = 1;    // load RenoDX-style DLLs from Neural/

		// DLSS 5 Neural Rendering ("uplift"). Runs on the scene colour before the
		// upscaler resolves it. Defaults mirror NVIDIA's neutral values, so a
		// freshly enabled uplift is the model's own judgement and nothing else.
		uint32_t dlssNREnabled = 0;
		uint32_t dlssNRPreset = 0;
		uint32_t dlssNRStyle = 0;
		// On by default: we supply no explicit uplift control mask, so without the
		// model choosing regions itself there is nothing marked to uplift and the
		// output comes back identical to the input.
		uint32_t dlssNRUseAutoMask = 1;
		// Diagnostic. 1 = skip the uplift but still route its target to the
		// screen, which shows the pre-upscale input and so proves whether the
		// target reaches present at all.
		uint32_t dlssNRDebugBypass = 0;
		uint32_t dlssNRPassCount = 1;          // 1..3; more passes = stronger, slower
		// 0 = before the upscaler, 1 = after it. Before means DLSS's temporal
		// resolve runs over the uplift and largely averages the added detail back
		// out, so after is the default. After needs display-resolution guides, so
		// it only applies when the render and display sizes match.
		uint32_t dlssNRAfterUpscale = 1;
		float    dlssNRIntensity = 1.0f;
		float    dlssNRLocalToneStrength = 1.0f;
		float    dlssNRLocalStructureStrength = 1.0f;
		float    dlssNRSkinStructureStrength = 1.0f;
	};

	Settings settings;

	void Load();
	bool Save(const Settings& a_settings);
	void ReloadIfChanged();

	std::mutex mutex;

private:
	std::filesystem::file_time_type lastWriteTime{};
	[[nodiscard]] std::filesystem::path GetIniPath() const;
};
