#include "PCH.h"

#include "Upscaler/Upscaling.h"

#include "Diagnostics/FrameTimeline.h"
#include "Diagnostics/SceneTargetProbe.h"
#include "Settings/Settings.h"
#include "Hooks/UpscalerHooks.h"
#include "Upscaler/D3D12Upscaler.h"
#include "Render/Streamline.h"

void Upscaling::OnDataLoaded()
{
	if (auto* ui = RE::UI::GetSingleton()) {
		ui->AddEventSink<RE::MenuOpenCloseEvent>(this);
	}
	if (auto* input = RE::BSInputDeviceManager::GetSingleton()) {
		input->AddEventSink(this);
	}
	SettingsStore::GetSingleton()->Load();

	// STEP 2: enable the engine's dynamic-resolution render path so our
	// per-frame scale override (in the UpdateJitter hook) actually downsamples.
	if (auto* drsSetting = RE::GetINISetting("bEnableAutoDynamicResolution:Display")) {
		drsSetting->data.b = true;
		logger::info("[Upscaling] Forced bEnableAutoDynamicResolution:Display = true");
	} else {
		logger::warn("[Upscaling] Could not find bEnableAutoDynamicResolution:Display INI setting");
	}

	// TODO(port): ApplyTextureMemoryUpgradeReserve() / UpdateGameSettings() land
	// with the GPU body; they poke Fallout-4-specific INI settings and renderer
	// memory reserves that need Skyrim equivalents.
	{
		const auto key = SettingsStore::GetSingleton()->settings.uiCompositeDebugKey;
		logger::info("[Upscaling] Data loaded; menu and input sinks registered "
					 "(composite debug cycle key = DIK 0x{:02X}{})",
			key, key == 0 ? " -- disabled" : "");
	}
}

RE::BSEventNotifyControl Upscaling::ProcessEvent(RE::InputEvent* const* a_event,
	RE::BSTEventSource<RE::InputEvent*>*)
{
	auto& settings = SettingsStore::GetSingleton()->settings;
	const auto wanted = settings.uiCompositeDebugKey;
	if (!a_event || wanted == 0) {
		return RE::BSEventNotifyControl::kContinue;
	}

	for (auto* event = *a_event; event; event = event->next) {
		const auto* button = event->AsButtonEvent();
		if (!button || !button->IsDown() || event->GetDevice() != RE::INPUT_DEVICE::kKeyboard) {
			continue;
		}
		if (button->GetIDCode() == settings.sceneTargetProbeMarkKey) {
			if (settings.sceneTargetProbe != 0) {
				SceneTargetProbe::Mark();
			} else {
				FrameTimeline::Arm();
			}
			continue;
		}
		if (button->GetIDCode() != wanted) {
			continue;
		}
		// Written straight into the live settings: the present thread reads this
		// every frame, so the view changes on the next present with no menu, no
		// save and no reload.
		static constexpr const char* kViewNames[]{ "composite", "UI layer", "mask", "scene only", "pre-UI capture", "split: ours | ENB" };
		settings.uiCompositeDebug = (settings.uiCompositeDebug + 1) % static_cast<uint32_t>(std::size(kViewNames));
		logger::info("[Upscaling] Composite debug view -> {} ({})",
			settings.uiCompositeDebug, kViewNames[settings.uiCompositeDebug]);
	}

	return RE::BSEventNotifyControl::kContinue;
}

RE::BSEventNotifyControl Upscaling::ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
	RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
{
	if (!a_event) {
		return RE::BSEventNotifyControl::kContinue;
	}

	const auto& name = a_event->menuName;

	// Block the upscaler (DRS + DLSS/FSR) while a full-screen non-gameplay menu is
	// up: the studio-logo bink videos, the main menu, loading screens, and
	// character creation are not the jittered 3D scene, so upscaling them warps
	// the image into a corner. We start blocked (temporalFeaturesBlocked=true) so
	// the launch logos are covered, and unblock once a loading screen finishes.
	if (name == RE::MainMenu::MENU_NAME) {
		if (a_event->opening) {
			temporalFeaturesBlocked = true;
		}
	} else if (name == RE::LoadingMenu::MENU_NAME) {
		temporalFeaturesBlocked = a_event->opening;  // block while open, unblock on close
	} else if (name == RE::RaceSexMenu::MENU_NAME) {
		// RaceMenu used to be blocked outright alongside loading screens, but it is
		// not the same thing: it is a real jittered 3D scene, and it is the one
		// place a player studies a face closely, so the upscaler and the neural
		// uplift are worth more here than almost anywhere else. What actually
		// warped the image was dynamic resolution -- the sub-rect resolve does not
		// line up when the scene is rendered smaller than its target -- and that
		// only applies while we are downscaling. See ShouldBlockUpscaling.
		raceMenuOpen = a_event->opening;
		if (!a_event->opening) {
			// Kept from the old branch: character creation at the start of a game
			// is one of the paths that clears the main-menu block.
			temporalFeaturesBlocked = false;
		}
	} else if (name == RE::FaderMenu::MENU_NAME) {
		if (!a_event->opening) {
			temporalFeaturesBlocked = false;
		}
	} else if (name == "PauseMenu") {
		// Reload settings when the pause menu (System/MCM path) closes, matching
		// the Fallout 4 behaviour so external edits are picked up.
		if (!a_event->opening) {
			SettingsStore::GetSingleton()->ReloadIfChanged();
		}
	}

	return RE::BSEventNotifyControl::kContinue;
}

void Upscaling::InstallHooks()
{
	// -----------------------------------------------------------------------
	// ENGINE RENDER-PIPELINE HOOKS -- NOT YET IMPLEMENTED FOR SKYRIM.
	//
	// The Fallout 4 InstallHooks() installs ~20 hooks by Address Library ID
	// (e.g. REL::ID{ 984743, 2318321 } + per-runtime offsets) onto specific
	// Creation Engine render functions. Every one of those IDs is Fallout-4-
	// specific; the Skyrim equivalents must be located per runtime (SE 1.5.97 /
	// AE 1.6.x) with RE + in-game testing. Fabricating IDs here would crash the
	// game, so this is left as a documented roadmap instead.
	//
	// Skyrim hook points to locate (mapped from the FO4 spec in
	// src/Game/Upscaling.cpp.fo4ref):
	//   * Disable/replace TAA          -> RE::ImageSpaceEffectTemporalAA
	//   * Drive jitter + dyn.resolution-> BSGraphics::State dynamic-resolution update
	//   * Sampler-state mip bias        -> main scene forward/deferred pre-passes
	//   * Motion-vector / depth capture -> world draw + first-person alpha pass
	//   * SSR / lens-flare / DOF dyn-res fixes
	//   * ENB composite handoff (when ENB is loaded)
	//   * Final upscale evaluation      -> end of DrawWorld / image-space range
	//
	// When implementing, populate Util::CameraFrame each frame via
	// SetCameraFrame(...) from the camera-setup hook.
	// -----------------------------------------------------------------------
	// Verified render-pipeline hooks (observational first pass): fire per-frame,
	// capture the camera, advance the frame counter. Image is left unchanged
	// until the upscaler eval + jitter paths are enabled.
	UpscalerHooks::Install();
}

void Upscaling::SetCameraFrame(const DirectX::XMMATRIX& a_view,
	const DirectX::XMMATRIX& a_viewProjUnjittered,
	const DirectX::XMMATRIX& a_proj,
	DirectX::XMFLOAT2 a_jitter,
	bool a_useJitter)
{
	auto* frame = Util::CameraFrame::GetSingleton();
	frame->viewMat = a_view;
	frame->viewProjUnjittered = a_viewProjUnjittered;
	frame->projMat = a_proj;
	frame->jitter = a_jitter;
	frame->useJitter = a_useJitter;
	frame->valid = true;
}

// ---------------------------------------------------------------------------
// Method selection -- ported from the Fallout 4 logic. Backend calls
// (Streamline / DX12SwapChain) are replaced by the availability flags this
// class exposes; the render backend sets them once compiled in.
// ---------------------------------------------------------------------------

bool Upscaling::ShouldBlockTemporalFeatures() const
{
	// TODO(port): the full FO4 rule also folds in load-screen / transition
	// state. scope + explicit block cover the common cases for now.
	return temporalFeaturesBlocked || scopeMenuOpen;
}

bool Upscaling::ShouldBlockUpscaling() const
{
	if (ShouldBlockTemporalFeatures()) {
		return true;
	}
	// RaceMenu only needs blocking when we are actually rendering below the
	// target; at native scale there is no sub-rect to misalign, so the upscaler,
	// Ray Reconstruction and the uplift all run and the face is shown the way it
	// will look in game.
	return raceMenuOpen && SettingsStore::GetSingleton()->settings.qualityMode != 0;
}

bool Upscaling::IsSteamOverlayLoaded()
{
	// Resolved once: the overlay is injected at process start and never unloads.
	static const bool loaded = [] {
		const bool present = GetModuleHandleW(L"gameoverlayrenderer64.dll") != nullptr;
		if (present) {
			logger::warn(
				"[Upscaling] The Steam overlay is loaded, so frame generation is disabled. DLSS-G presents "
				"from its own thread and the overlay faults on that path. To use frame generation, turn the "
				"Steam overlay off for Skyrim (Properties -> General -> In-Game Overlay).");
		}
		return present;
	}();
	return loaded;
}

bool Upscaling::IsFrameGenerationBlockedByOverlay()
{
	if (!IsSteamOverlayLoaded()) {
		return false;
	}
	if (SettingsStore::GetSingleton()->settings.frameGenWithSteamOverlay == 0) {
		return true;
	}
	static bool loggedOverride = false;
	if (!loggedOverride) {
		loggedOverride = true;
		logger::warn("[Upscaling] The Steam overlay block on frame generation is lifted by explicit setting; whether frame generation actually runs still depends on FrameGenerationMode. "
					 "The overlay has faulted on DLSS-G's present thread before; if the game crashes inside "
					 "gameoverlayrenderer64.dll, this is why.");
	}
	return false;
}

bool Upscaling::ShouldBlockFrameGeneration() const
{
	return ShouldBlockTemporalFeatures() || !dlssgMenuResumeReady || IsFrameGenerationBlockedByOverlay();
}

bool Upscaling::IsFeatureRequestBlocked(FeatureRequest) const
{
	// TODO(port): the FO4 build has a per-feature retry/backoff table that
	// blocks a temporal feature for a few frames after an SDK request fails.
	// That ports with the render backend; until then nothing is blocked.
	return false;
}

Upscaling::UpscaleMethod Upscaling::GetUpscaleMethod(bool a_checkMenu)
{
	if (a_checkMenu && ShouldBlockUpscaling()) {
		return UpscaleMethod::kDisabled;
	}

	const auto& liveSettings = SettingsStore::GetSingleton()->settings;
	auto method = static_cast<UpscaleMethod>(liveSettings.upscaleMethodPreference);

	if ((method == UpscaleMethod::kDLSS || method == UpscaleMethod::kFSR) && !dx12Ready) {
		return UpscaleMethod::kDisabled;
	}

	// Fall back to FSR when DLSS is unavailable on this GPU.
	if (!featureDLSS && method == UpscaleMethod::kDLSS) {
		method = UpscaleMethod::kFSR;
	}

	if ((method == UpscaleMethod::kDLSS && IsFeatureRequestBlocked(FeatureRequest::kDLSS)) ||
		(method == UpscaleMethod::kFSR && IsFeatureRequestBlocked(FeatureRequest::kFSR))) {
		method = UpscaleMethod::kSpatialFallback;
	}

	return method;
}

bool Upscaling::ShouldUseFrameGeneration(bool a_checkMenu)
{
	if constexpr (!kEnableDLSSG) {
		std::ignore = a_checkMenu;
		return false;
	} else {
		if (a_checkMenu) {
			return frameGenerationActive;
		}

		const auto& liveSettings = SettingsStore::GetSingleton()->settings;

		if (ShouldUseFSRFrameGeneration(a_checkMenu)) {
			return false;
		}
		if ((liveSettings.frameGenerationMode == 0 && liveSettings.dynamicMFGEnabled == 0) || !featureDLSSG) {
			return false;
		}
		if (a_checkMenu && ShouldBlockFrameGeneration()) {
			return false;
		}
		return true;
	}
}

// ---------------------------------------------------------------------------
// GPU-body stubs. These no-op until the FO4 Upscaling D3D12 evaluation code is
// ported to Skyrim's render targets (the next major milestone). They let the
// DX12 proxy swapchain link and run; with these returning false, the swapchain
// simply presents the game image unmodified.
// ---------------------------------------------------------------------------
bool Upscaling::EvaluateD3D12DLSS(ID3D12GraphicsCommandList*, uint32_t) { return false; }
bool Upscaling::EvaluateD3D12FSR(ID3D12GraphicsCommandList*, uint32_t) { return false; }
bool Upscaling::EvaluateFSRFrameGeneration(ID3D12GraphicsCommandList*, uint32_t) { return false; }
void Upscaling::TagDLSSGInputs(ID3D12GraphicsCommandList* a_commandList, uint32_t a_frameIndex, ID3D12Resource* a_hudlessColor)
{
	if (!frameGenerationActive || !a_commandList) {
		return;
	}

	auto* up = D3D12Upscaler::GetSingleton();
	// hudless = the FINAL ENB-composited backbuffer (display res) passed in by the
	// present, so generated frames match the real ones (interpolate the final
	// image). Using the pre-ENB/pre-UI upscaler color here made generated frames
	// look different -> severe flicker. Motion vectors + depth are the render-res
	// scene sub-rect the upscaler captured.
	auto* hudless = a_hudlessColor;
	auto* motionVectors = up->GetMotionVectors12();
	auto* depth = up->GetDepth12();
	if (!hudless || !motionVectors || !depth) {
		return;
	}

	// No UI separation yet (uiColorAlpha = nullptr): UI is warped with the frame.
	// IMPORTANT: tag against the GAME frame count (not the backbuffer index) so the
	// frame token matches the present markers + DLSS-SR constants -- otherwise
	// DLSS-G can't find the tagged depth/motion-vector buffers.
	std::ignore = a_frameIndex;
	const uint32_t frameCount = Util::State_GetSingleton()->frameCount;
	const float2 renderSize{ static_cast<float>(up->GetRenderWidth()), static_cast<float>(up->GetRenderHeight()) };
	const float2 displaySize{ static_cast<float>(up->GetDisplayWidth()), static_cast<float>(up->GetDisplayHeight()) };
	Streamline::GetSingleton()->TagDLSSGResources(
		hudless, motionVectors, depth, nullptr, a_commandList, frameCount, renderSize, displaySize);
}
void Upscaling::GetTaggedTextureDebugResources(uint32_t, ID3D12Resource*& a_color, ID3D12Resource*& a_depth, ID3D12Resource*& a_motionVectors) const
{
	a_color = nullptr;
	a_depth = nullptr;
	a_motionVectors = nullptr;
}

bool Upscaling::ShouldUseFSRFrameGeneration(bool a_checkMenu)
{
	if constexpr (!kEnableDLSSG && !kForceFSRFrameGenerationForTesting) {
		std::ignore = a_checkMenu;
		return false;
	} else {
		if (a_checkMenu) {
			return fsrFrameGenerationActive;
		}

		const auto& liveSettings = SettingsStore::GetSingleton()->settings;
		if (static_cast<UpscaleMethod>(liveSettings.upscaleMethodPreference) == UpscaleMethod::kDisabled ||
			(liveSettings.frameGenerationMode == 0 && liveSettings.dynamicMFGEnabled == 0) ||
			!dx12Ready) {
			return false;
		}

		// When DLSS-G is available it owns frame generation; FSR-FG is the fallback.
		if (featureDLSSG) {
			return false;
		}
		if (a_checkMenu && ShouldBlockFrameGeneration()) {
			return false;
		}
		return true;
	}
}
