#include "PCH.h"

#include "UI/Menu.h"

#include "Settings/Settings.h"
#include "Neural/NeuralRendering.h"
#include "Render/Streamline.h"
#include "Upscaler/Upscaling.h"

#include "SKSEMenuFramework.h"

#include <array>
#include <mutex>
#include <optional>

// ===========================================================================
// SKSE Menu Framework 3 settings page.
//
// Ported from the Fallout 4 project's UpscalingMenu.cpp. The F4SE Menu
// Framework and SKSE Menu Framework 3 share the same "MCP" ImGui binding
// namespace (ImGuiMCP), so the widget code carries over unchanged; only the
// framework registration and lifecycle events are swapped
// (F4SEMenuFramework -> SKSEMenuFramework) and settings are read from the
// decoupled SettingsStore instead of the not-yet-ported Upscaling core.
// ===========================================================================

namespace
{
	using Settings = SettingsStore::Settings;

	std::mutex   g_stateMutex;
	Settings     g_editSettings;
	bool         g_editInitialized = false;
	bool         g_dirty = false;
	bool         g_registered = false;

	SKSEMenuFramework::Model::Event* g_menuEvent = nullptr;

	void ShowHelp(const char* a_help)
	{
		if (ImGuiMCP::IsItemHovered()) {
			ImGuiMCP::SetTooltip("%s", a_help);
		}
	}

	template <std::size_t N>
	bool ComboSetting(const char* a_label, uint32_t& a_value, const std::array<const char*, N>& a_items, const char* a_help)
	{
		int value = static_cast<int>(a_value);
		const bool changed = ImGuiMCP::Combo(a_label, &value, a_items.data(), static_cast<int>(a_items.size()));
		ShowHelp(a_help);
		if (changed) {
			a_value = static_cast<uint32_t>(value);
		}
		return changed;
	}

	bool CheckboxSetting(const char* a_label, uint32_t& a_value, const char* a_help)
	{
		bool value = a_value != 0;
		const bool changed = ImGuiMCP::Checkbox(a_label, &value);
		ShowHelp(a_help);
		if (changed) {
			a_value = value ? 1u : 0u;
		}
		return changed;
	}

	bool SliderIntSetting(const char* a_label, uint32_t& a_value, int a_min, int a_max, const char* a_format, const char* a_help)
	{
		int value = static_cast<int>(a_value);
		const bool changed = ImGuiMCP::SliderInt(a_label, &value, a_min, a_max, a_format);
		ShowHelp(a_help);
		if (changed) {
			a_value = static_cast<uint32_t>(value);
		}
		return changed;
	}

	bool SliderFloatSetting(const char* a_label, float& a_value, float a_min, float a_max, const char* a_format, const char* a_help)
	{
		const bool changed = ImGuiMCP::SliderFloat(a_label, &a_value, a_min, a_max, a_format);
		ShowHelp(a_help);
		return changed;
	}

	void InitializeEditState()
	{
		std::scoped_lock lock(g_stateMutex);
		g_editSettings = SettingsStore::GetSingleton()->settings;
		g_editInitialized = true;
		g_dirty = false;
	}

	void QueueSettingsReload(bool a_force)
	{
		if (const auto tasks = SKSE::GetTaskInterface()) {
			tasks->AddTask([a_force] {
				if (a_force) {
					SettingsStore::GetSingleton()->Load();
				} else {
					SettingsStore::GetSingleton()->ReloadIfChanged();
				}
			});
		} else if (a_force) {
			SettingsStore::GetSingleton()->Load();
		} else {
			SettingsStore::GetSingleton()->ReloadIfChanged();
		}
	}

	void __stdcall OnMenuEvent(SKSEMenuFramework::Model::EventType a_type)
	{
		if (a_type == SKSEMenuFramework::Model::kOpenMenu) {
			InitializeEditState();
			return;
		}

		if (a_type != SKSEMenuFramework::Model::kCloseMenu) {
			return;
		}

		std::optional<Settings> settingsToSave;
		{
			std::scoped_lock lock(g_stateMutex);
			if (g_editInitialized && g_dirty) {
				settingsToSave = g_editSettings;
			}
			g_editInitialized = false;
			g_dirty = false;
		}

		if (settingsToSave) {
			if (!SettingsStore::GetSingleton()->Save(*settingsToSave)) {
				logger::error("[Menu] Could not save SkyrimUpscaler settings");
				return;
			}
			// Mirror the neural toggles into the live NeuralRendering module.
			auto* neural = NeuralRendering::GetSingleton();
			neural->settings.enableRayReconstruction = settingsToSave->neuralRayReconstruction != 0;
			neural->settings.enableExternalModules = settingsToSave->neuralExternalModules != 0;
			QueueSettingsReload(true);
		} else {
			QueueSettingsReload(false);
		}
	}

	void __stdcall RenderSettings()
	{
		std::scoped_lock lock(g_stateMutex);
		if (!g_editInitialized) {
			g_editSettings = SettingsStore::GetSingleton()->settings;
			g_editInitialized = true;
			g_dirty = false;
		}

		auto& settings = g_editSettings;
		bool changed = false;

		ImGuiMCP::TextWrapped("Changes are saved and applied when the Mod Control Panel closes.");

		// TODO(port): once the Streamline backend (src/Render/Streamline.cpp) is
		// compiled in, show live availability here:
		//   Streamline* sl = Streamline::GetSingleton();
		//   ImGuiMCP::TextDisabled("Runtime: DLSS %s | Frame Generation %s | Reflex %s", ...);

		ImGuiMCP::SeparatorText("Upscaling");
		static constexpr std::array upscaleMethods{ "Disabled", "AMD FSR", "NVIDIA DLSS" };
		changed |= ComboSetting(
			"Upscale Method",
			settings.upscaleMethodPreference,
			upscaleMethods,
			"Selects the preferred temporal upscaler. DLSS falls back to FSR when unavailable.");

		static constexpr std::array qualityModes{ "Native AA", "Quality", "Balanced", "Performance", "Ultra Performance" };
		changed |= ComboSetting(
			"Quality Mode",
			settings.qualityMode,
			qualityModes,
			"Controls the render resolution used by the temporal upscaler.");
		changed |= SliderFloatSetting(
			"Sharpness",
			settings.sharpness,
			0.0f,
			1.0f,
			"%.2f",
			"Controls NVIDIA Image Scaling sharpen for DLSS and RCAS for FSR.");

		ImGuiMCP::SeparatorText("Frame Generation and Latency");
		const bool upscalingDisabled = settings.upscaleMethodPreference == static_cast<uint32_t>(SettingsStore::UpscaleMethod::kDisabled);
		if constexpr (!Upscaling::kEnableDLSSG) {
			ImGuiMCP::TextWrapped("Frame Generation is temporarily disabled by the driver-safety gate.");
		}
		ImGuiMCP::BeginDisabled(upscalingDisabled || !Upscaling::kEnableDLSSG);
		static constexpr std::array frameGenerationModes{ "Disabled", "On", "Auto" };
		changed |= ComboSetting(
			"Frame Generation",
			settings.frameGenerationMode,
			frameGenerationModes,
			"Uses the selected vendor's frame generation path when supported.");
		ImGuiMCP::EndDisabled();

		const bool frameGenerationDisabled = upscalingDisabled || settings.frameGenerationMode == 0;
		const bool dlssSelected = settings.upscaleMethodPreference == static_cast<uint32_t>(SettingsStore::UpscaleMethod::kDLSS);
		ImGuiMCP::BeginDisabled(frameGenerationDisabled || !dlssSelected);
		static constexpr std::array generatedFrameCounts{ "1 (2x)", "2 (3x)", "3 (4x)", "4 (5x)", "5 (6x)" };
		changed |= ComboSetting(
			"Generated Frames",
			settings.dlssgGeneratedFrames,
			generatedFrameCounts,
			"Controls the requested DLSS generated-frame multiplier. The runtime clamps unsupported values.");
		changed |= CheckboxSetting(
			"Dynamic Multi Frame Generation",
			settings.dynamicMFGEnabled,
			"Lets Streamline dynamically select the generated-frame multiplier when supported.");
		ImGuiMCP::BeginDisabled(settings.dynamicMFGEnabled == 0);
		changed |= SliderIntSetting(
			"Dynamic Target FPS",
			settings.dynamicMFGTargetFPS,
			0,
			500,
			"%d FPS",
			"Target output frame rate. Zero lets Streamline use the display refresh rate.");
		ImGuiMCP::EndDisabled();
		ImGuiMCP::EndDisabled();

		// Without this the RTX 40 unlock is invisible: the multiplier stays at its
		// default and nothing says how much higher it could be set.
		{
			const auto* sl = Streamline::GetSingleton();
			const auto  maxGenerated = sl->GetMaxFramesToGenerate();
			if (sl->featureDLSSG) {
				ImGuiMCP::TextDisabled("Frame generation: up to %ux (%u generated frame%s)%s",
					maxGenerated + 1u, maxGenerated, maxGenerated == 1 ? "" : "s",
					sl->IsDynamicMFGSupported() ? ", dynamic supported" : ", dynamic unsupported");
			}
		}

		static constexpr std::array reflexModes{ "Off", "On", "On + Boost" };
		changed |= ComboSetting(
			"NVIDIA Reflex",
			settings.reflexMode,
			reflexModes,
			"Controls NVIDIA Reflex low-latency mode. Frame generation forces at least On while active.");

		ImGuiMCP::SeparatorText("DLSS");
		ImGuiMCP::BeginDisabled(!dlssSelected);
		static constexpr std::array dlssPresets{ "Recommended", "Default", "K", "M", "L" };
		changed |= ComboSetting(
			"Model Preset",
			settings.dlssModelPreset,
			dlssPresets,
			"Recommended uses K for DLAA/Quality/Balanced, M for Performance, and L for Ultra Performance.");
		ImGuiMCP::EndDisabled();

		ImGuiMCP::SeparatorText("Neural Rendering");
		{
			auto*      sl = Streamline::GetSingleton();
			auto*      neural = NeuralRendering::GetSingleton();
			const bool rrAvailable = sl->featureDLSSD;

			ImGuiMCP::BeginDisabled(!rrAvailable);
			changed |= CheckboxSetting(
				"DLSS Ray Reconstruction",
				settings.neuralRayReconstruction,
				"Runs the DLSS-D neural denoiser instead of plain super resolution. "
				"Requires an RTX GPU, the D3D12 path, and nvngx_dlssd.dll in the Streamline folder.");
			ImGuiMCP::EndDisabled();

			if (rrAvailable) {
				ImGuiMCP::TextDisabled("Ray Reconstruction: available");
			} else {
				ImGuiMCP::TextDisabled("Ray Reconstruction unavailable (%s)", sl->dlssdStatus.c_str());
			}

			// DLSS 5 Neural Rendering (nvngx_dlssnr.dll) -- the feature the RenoDX
			// community drives through ReShade. Reported only; Streamline 2.13 ships
			// no public options struct for it, so there is nothing to switch on yet.
			// DLSS 5 Neural Rendering ("uplift"): colour in, enhanced colour out.
			const bool nrUsable = sl->IsDLSSNRUsable();
			ImGuiMCP::BeginDisabled(!nrUsable);
			changed |= CheckboxSetting(
				"DLSS 5 Neural Rendering",
				settings.dlssNREnabled,
				"Runs NVIDIA's neural uplift on the scene before upscaling: more plausible "
				"detail in faces, skin, hair and cloth. Requires the DLSS method.");
			ImGuiMCP::EndDisabled();

			if (sl->featureDLSSNR) {
				ImGuiMCP::TextDisabled("Neural Rendering: Streamline plugin");
			} else if (sl->directDLSSNRReady) {
				ImGuiMCP::TextDisabled("Neural Rendering: direct NGX path");
			} else {
				ImGuiMCP::TextDisabled("Neural Rendering unavailable (%s)", sl->dlssnrStatus.c_str());
			}

			ImGuiMCP::BeginDisabled(!nrUsable || settings.dlssNREnabled == 0);
			static constexpr std::array nrOrders{ "Before upscaling", "After upscaling" };
			changed |= ComboSetting(
				"NR Order", settings.dlssNRAfterUpscale, nrOrders,
				"Before: the upscaler's temporal resolve averages most of the uplift back out, so it "
				"costs frame time for very little. After: the detail stays on screen. Leave this on After.");
			changed |= SliderFloatSetting(
				"NR Intensity", settings.dlssNRIntensity, 0.0f, 1.0f, "%.2f",
				"Overall strength of the uplift. 1.0 is NVIDIA's neutral value.");
			changed |= SliderFloatSetting(
				"NR Local Tone", settings.dlssNRLocalToneStrength, 0.0f, 1.0f, "%.2f",
				"How far the model may push local contrast and shading.");
			changed |= SliderFloatSetting(
				"NR Local Structure", settings.dlssNRLocalStructureStrength, 0.0f, 1.0f, "%.2f",
				"How much fine surface detail the model may add.");
			changed |= SliderFloatSetting(
				"NR Skin Structure", settings.dlssNRSkinStructureStrength, 0.0f, 1.0f, "%.2f",
				"Detail strength applied specifically to skin. Lower it if faces look harsh.");
			changed |= CheckboxSetting(
				"NR Auto Mask",
				settings.dlssNRUseAutoMask,
				"Lets the model pick which regions to uplift. Keep this on: no explicit uplift mask is "
				"supplied, so with it off there may be nothing marked to uplift at all.");
			static constexpr std::array nrEncodings{ "Linear BT.709", "sRGB", "BT.2100 PQ" };
			changed |= ComboSetting(
				"NR Colour Encoding", settings.dlssNREncoding, nrEncodings,
				"The colour space the uplift is handed. Skyrim's scene is unbounded linear HDR, which the "
				"model has no reference for, so it is normalised first. sRGB is the safe default; try the "
				"others if the effect looks weak or the image shifts.");
			changed |= SliderFloatSetting(
				"NR Diffuse White", settings.dlssNRDiffuseWhiteNits, 0.0f, 400.0f, "%.0f nits",
				"Scene brightness treated as white. 0 uses the encoding's default: 100 nits for linear "
				"BT.709 and sRGB, 250 for BT.2100 PQ.");
			changed |= SliderIntSetting(
				"NR Style", settings.dlssNRStyle, 0, 4, "%d",
				"Selects the model's look. Undocumented; sweep it if the uplift seems to do nothing.");
			{
				// 1..3 passes: each one re-runs the uplift over its own output.
				static constexpr std::array nrPasses{ "1 pass", "2 passes", "3 passes" };
				uint32_t passIndex = std::clamp(settings.dlssNRPassCount, 1u, 3u) - 1u;
				if (ComboSetting("NR Passes", passIndex, nrPasses,
						"More passes push the effect further at a proportional cost.")) {
					settings.dlssNRPassCount = passIndex + 1u;
					changed = true;
				}
			}
			changed |= CheckboxSetting(
				"NR Debug: bypass uplift",
				settings.dlssNRDebugBypass,
				"Diagnostic. Skips the uplift and fills its target with flat magenta instead. "
				"A magenta screen means the target reaches the screen and the uplift is doing nothing; "
				"no magenta means the target never reaches the screen. Turn this back off afterwards.");
			ImGuiMCP::EndDisabled();

			changed |= CheckboxSetting(
				"External Neural Modules",
				settings.neuralExternalModules,
				"Loads RenoDX-style DLLs from Data/SKSE/Plugins/SkyrimUpscaler/Neural/. Restart to apply.");

			const auto moduleCount = neural->GetModuleCount();
			if (moduleCount == 0) {
				ImGuiMCP::TextDisabled("No external modules loaded%s",
					neural->GetRejectedModuleCount() != 0 ? " (some DLLs were rejected -- see the log)" : "");
			} else {
				for (std::size_t i = 0; i < moduleCount; ++i) {
					ImGuiMCP::TextDisabled("  %s v%s", neural->GetModuleName(i).c_str(), neural->GetModuleVersion(i).c_str());
				}
			}
		}

		// ---- FPS overlay: a plain on/off, with the detail level beside it. Both
		// live in the single persisted `osdMode` (0 = off, 1 = compact, 2 = detailed).
		ImGuiMCP::SeparatorText("On-Screen Display");
		{
			// Remembers the chosen detail across an off/on toggle this session.
			static bool lastDetailed = false;
			if (settings.osdMode == 2) {
				lastDetailed = true;
			} else if (settings.osdMode == 1) {
				lastDetailed = false;
			}

			bool osdEnabled = settings.osdMode != 0;
			if (ImGuiMCP::Checkbox("Show FPS Overlay", &osdEnabled)) {
				settings.osdMode = osdEnabled ? (lastDetailed ? 2u : 1u) : 0u;
				changed = true;
			}
			ShowHelp(
				"Live FPS + frame time drawn on screen. With frame generation enabled it also "
				"shows \"Generated FPS\" -- the real presented rate after DLSS-G, so you can "
				"see at a glance whether frame generation is actually doubling your frames.");

			ImGuiMCP::BeginDisabled(!osdEnabled);
			static constexpr std::array osdDetails{ "Compact", "Detailed" };
			int detail = settings.osdMode == 2 ? 1 : 0;
			if (ImGuiMCP::Combo("Overlay Detail", &detail, osdDetails.data(), static_cast<int>(osdDetails.size()))) {
				lastDetailed = detail != 0;
				if (osdEnabled) {
					settings.osdMode = lastDetailed ? 2u : 1u;
				}
				changed = true;
			}
			ShowHelp(
				"Compact: FPS, frame time and Generated FPS. "
				"Detailed: also the render->display resolution, VRAM usage and Reflex latency.");
			ImGuiMCP::EndDisabled();
		}

		ImGuiMCP::SeparatorText("Diagnostics");
		changed |= CheckboxSetting(
			"Tagged Texture Debug View",
			settings.taggedTextureDebug,
			"Shows the color, depth, motion-vector, and final-image resources used by the D3D12 upscaler.");
		changed |= CheckboxSetting(
			"Image-Space Effect Log",
			settings.imageSpaceEffectLog,
			"Logs unique image-space effects dispatched inside the ENB native-resolution scope.");

		g_dirty |= changed;
	}
}

void UI::Menu::Register()
{
	if (g_registered) {
		return;
	}

	if (!SKSEMenuFramework::IsInstalled()) {
		logger::warn("[Menu] SKSE Menu Framework is not installed; in-game settings page is unavailable");
		return;
	}

	SKSEMenuFramework::SetSection("SkyrimUpscaler");
	SKSEMenuFramework::AddSectionItem("Settings", RenderSettings);

	// priority 0.0 -- default ordering among listeners.
	g_menuEvent = SKSEMenuFramework::AddEvent(OnMenuEvent, 0.0f);
	if (!g_menuEvent) {
		logger::warn("[Menu] SKSE Menu Framework did not expose lifecycle events; settings still editable but auto-save on close is unavailable");
	}

	g_registered = true;
	logger::info("[Menu] Registered SkyrimUpscaler settings with SKSE Menu Framework 3");
}
