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
		// Hand the engine's TAA mask to the upscaler as a transparency hint, so
		// particles, water and other alpha-blended pixels stop ghosting.
		uint32_t transparencyHint = 1;
		// Run frame generation even though the Steam overlay is loaded. Off by
		// default: the overlay faults on DLSS-G's present thread, which produced a
		// reproducible access violation. Opt in only knowing that.
		uint32_t frameGenWithSteamOverlay = 0;
		// How the upscaled scene reaches the screen.
		//   1 = present override: the scene stays on D3D12 and is composited with
		//       the UI at present. Required for frame generation, but ENB's
		//       post-processing runs on the cleared colour target and so never
		//       touches the scene.
		//   0 = copy the result back into the game's colour target, so ENB grades
		//       and tonemaps it as usual. Frame generation cannot pace this.
		uint32_t presentOverride = 1;
		// The present-override composite decides what is UI by looking at the
		// cleared colour target. ENB's post-processing lands there too, so these
		// exist to see what is being classified and to move the line.
		uint32_t uiCompositeDebug = 0;   // 0 composite, 1 UI layer, 2 mask, 3 scene only
		// DirectInput scancode that cycles uiCompositeDebug in game. Two of those
		// views hide the UI, so without a key they cannot be turned off from
		// inside the game. 0x44 = F10; 0 disables the key.
		uint32_t uiCompositeDebugKey = 0x44;
		// Finds which render target ENB actually reads, by filling one at a time
		// with magenta and watching ENB's own output for it. 0 off, 1 sweep,
		// 2+ pin to candidate (value - 2). See Diagnostics/SceneTargetProbe.
		// ENB runs before our hook and grades the game's own scene, so ours
		// reaches the screen ungraded. Carry the grade across from the captured
		// ENB frame instead. Radius is in display pixels.
		uint32_t enbGradeTransfer = 0;
		float    enbGradeStrength = 1.0f;
		float    enbGradeRadius = 8.0f;
		uint32_t sceneTargetProbe = 0;
		uint32_t sceneTargetProbeFrames = 180;
		uint32_t sceneTargetProbeMarkKey = 0x57;  // F11
		// 2 = the UI-difference mask: the presented buffer is captured again
		// before the UI is drawn, and only pixels that changed count as UI, so
		// ENB's output cancels instead of being composited over the scene.
		// 0 and 1 are the older brightness heuristics, kept as a fallback.
		uint32_t uiMaskMode = 2;
		float    uiMaskThreshold = 0.02f;
		float    uiMaskSoftness = 0.03f;

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
		// 1 = present the amplified before/after difference instead of the frame,
		// so "is the uplift doing anything" stops being a judgement call.
		uint32_t dlssNRDebugDifference = 0;
		float    dlssNRDebugDifferenceGain = 10.0f;
		uint32_t dlssNRPassCount = 1;          // 1..3; more passes = stronger, slower
		// 0 = before the upscaler, 1 = after it. Before means DLSS's temporal
		// resolve runs over the uplift and largely averages the added detail back
		// out, so after is the default. After needs display-resolution guides, so
		// it only applies when the render and display sizes match.
		uint32_t dlssNRAfterUpscale = 1;
		// Colour space the uplift is handed. 0 = linear BT.709 (raw scene),
		// 1 = sRGB, 2 = BT.2100 PQ. The model was trained on a defined encoding
		// with a known diffuse-white level; Skyrim's scene colour is unbounded
		// linear HDR with neither, so it is normalised on the way in.
		uint32_t dlssNREncoding = 1;
		// Scene value that means diffuse white. 0 uses the per-encoding default.
		float    dlssNRDiffuseWhiteNits = 0.0f;
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
