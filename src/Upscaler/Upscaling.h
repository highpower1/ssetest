#pragma once

#include <DirectXMath.h>
#include <d3d12.h>

#include "Game/Util.h"
#include "Settings/Settings.h"

// ===========================================================================
// Upscaling  --  orchestration layer (Skyrim port, in progress)
//
// The Fallout 4 Upscaling class is ~8800 lines. It splits cleanly into:
//   * an ORCHESTRATION layer (method selection, settings, menu events, the
//     per-frame camera contract) -- ported here; compiles against
//     CommonLibSSE-NG with no game-offset dependency, and
//   * a GPU BODY (render-target scaling, depth/motion-vector prep, DLSS/FSR
//     evaluation, ~8000 lines) that is driven by the render backend
//     (src/Render/) and by Fallout-4-specific engine hooks. That body, and the
//     Address-Library-ID hook table in InstallHooks(), are the remaining
//     in-game reverse-engineering work (see PORTING.md).
//
// To keep the buildable foundation green, the orchestration here talks to the
// backend through plain availability flags (featureDLSS / featureDLSSG /
// dx12Ready) that the render backend sets once it is compiled in, instead of
// calling Streamline / DX12SwapChain directly the way the Fallout 4 code did.
// ===========================================================================

class Upscaling : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
{
public:
	static Upscaling* GetSingleton()
	{
		static Upscaling singleton;
		return &singleton;
	}

	enum class UpscaleMethod
	{
		kDisabled,
		kFSR,
		kDLSS,
		kSpatialFallback,
	};

	// When true, forces the FidelityFX frame-generation swapchain path even if
	// DLSS-G is available (used by the DX12 swapchain hook). Kept false.
	static constexpr bool kForceFSRFrameGenerationForTesting = false;

	// EXPERIMENTAL (frame generation foundation): when true, install the D3D12
	// proxy swapchain (Render/DX11Hooks.cpp factory hook) so Present is routed
	// through D3D12/Streamline -- the prerequisite for DLSS-G. While on, the
	// side-device DLSS upscaler (D3D12Upscaler) is disabled to avoid a second
	// Streamline D3D12 initialisation. Off = the shipped, validated upscaler path
	// (side device, ENB presents D3D11). FG-1 tests proxy present with ENB.
	static constexpr bool kFrameGenExperiment = false;

	// Driver-safety gate. When false, DLSS-G is not requested from Streamline
	// and no frame-generation swapchain is installed. Keep disabled until the
	// D3D12 lifetime/synchronization fixes have passed an extended in-game test.
	static constexpr bool kEnableDLSSG = false;

	// ---- lifecycle -------------------------------------------------------
	void        OnDataLoaded();               // register sink + load settings
	static void InstallHooks();               // engine render-pipeline hooks (scaffold)

	// ---- menu event sink -------------------------------------------------
	RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
		RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;

	// ---- method selection (ported logic) --------------------------------
	UpscaleMethod GetUpscaleMethod(bool a_checkMenu);
	bool          ShouldBlockUpscaling() const;
	bool          ShouldBlockFrameGeneration() const;
	bool          ShouldUseFrameGeneration(bool a_checkMenu);
	bool          ShouldUseFSRFrameGeneration(bool a_checkMenu);

	bool IsFrameGenerationActive() const { return frameGenerationActive; }
	bool IsFSRFrameGenerationActive() const { return fsrFrameGenerationActive; }

	// ---- GPU body (driven by the DX12 proxy swapchain) -------------------
	// STUBS for now: the D3D12 evaluation / tagging bodies are the remaining
	// large port of the FO4 Upscaling GPU code. They currently no-op so the
	// render backend links; wiring them up is the next milestone (PORTING.md).
	bool EvaluateD3D12DLSS(ID3D12GraphicsCommandList* a_commandList, uint32_t a_frameIndex);
	bool EvaluateD3D12FSR(ID3D12GraphicsCommandList* a_commandList, uint32_t a_frameIndex);
	bool EvaluateFSRFrameGeneration(ID3D12GraphicsCommandList* a_commandList, uint32_t a_frameIndex);
	void TagDLSSGInputs(ID3D12GraphicsCommandList* a_commandList, uint32_t a_frameIndex, ID3D12Resource* a_hudlessColor);
	void GetTaggedTextureDebugResources(uint32_t a_frameIndex, ID3D12Resource*& a_color, ID3D12Resource*& a_depth, ID3D12Resource*& a_motionVectors) const;

	// ---- per-frame camera contract --------------------------------------
	// Called by the render hook once per frame with the game's current view /
	// projection / jitter; forwards into Util::CameraFrame for the upscaler and
	// Streamline constants. This is the Skyrim replacement for reading Fallout
	// 4's BSGraphics::State::cameraDataCache.
	void SetCameraFrame(const DirectX::XMMATRIX& a_view,
		const DirectX::XMMATRIX& a_viewProjUnjittered,
		const DirectX::XMMATRIX& a_proj,
		DirectX::XMFLOAT2 a_jitter,
		bool a_useJitter);

	// ---- backend-published availability (set by src/Render once wired) ---
	bool featureDLSS = false;
	bool featureDLSSG = false;
	bool dx12Ready = false;

	// ---- state read by the render backend (OSD / DX12 swapchain) ---------
	// FO4 kept settings on Upscaling; we keep the source of truth in
	// SettingsStore and expose it here by reference so the backend's
	// `->settings.field` accesses keep working unchanged.
	SettingsStore::Settings& settings = SettingsStore::GetSingleton()->settings;
	float2 jitter{ 0.0f, 0.0f };          // current frame camera jitter (pixels)
	float2 osdRenderSize{ 0.0f, 0.0f };   // render resolution, for the OSD
	float2 osdNativeSize{ 0.0f, 0.0f };   // display resolution, for the OSD

	// ---- runtime state ---------------------------------------------------
	UpscaleMethod upscaleMethod = UpscaleMethod::kDisabled;
	bool          scopeMenuOpen = false;
	// Start blocked so the launch studio-logo videos + main menu are not upscaled
	// (unblocked once the first loading screen finishes -> in-world). See
	// Upscaling::ProcessEvent.
	bool          temporalFeaturesBlocked = true;
	bool          frameGenerationActive = false;
	bool          fsrFrameGenerationActive = false;
	bool          dlssgMenuResumeReady = true;

	enum class FeatureRequest
	{
		kDLSS,
		kFSR,
		kDLSSG,
		kFSRFrameGeneration,
	};

private:
	bool ShouldBlockTemporalFeatures() const;
	bool IsFeatureRequestBlocked(FeatureRequest a_feature) const;
};
