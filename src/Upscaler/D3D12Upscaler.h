#pragma once

#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <winrt/base.h>

#include <atomic>
#include <memory>
#include <limits>

class D3D11D3D12SharedTexture;  // from Render/DX12SwapChain.h

// ===========================================================================
// D3D12Upscaler  --  ENB-safe DLSS via D3D11<->D3D12 interop.
//
// Approach 1 (fo4test-style): the game renders the scene at low resolution;
// at the verified Main::DrawWorld pre-UI hook we copy the low-res color +
// motion vectors + depth from the game's D3D11 render targets into
// D3D11/D3D12 shared textures, run DLSS on a private D3D12 device, and copy the
// full-resolution result back into the game's color RT. By the time the game's
// UI pass and ENB present run, the color target is already full-res, so ENB
// sees a normal image (unlike the DRS-only path which ENB broke).
//
// This header declares the interop foundation (Init) + the per-frame evaluate
// entry (Evaluate). Init is implemented first (testable); Evaluate follows.
// ===========================================================================

class D3D12Upscaler
{
public:
	// GetSingleton + dtor defined in the .cpp so the static instance's
	// destruction (which deletes unique_ptr<D3D11D3D12SharedTexture>) is emitted
	// where the full type is visible, not in every including TU.
	static D3D12Upscaler* GetSingleton();

	D3D12Upscaler() = default;
	~D3D12Upscaler();

	// Stand up the private D3D12 device + command infrastructure, initialise
	// Streamline in D3D12 mode, and allocate the shared interop textures.
	// Returns true if DLSS is usable. Safe to call once (from the InitD3D hook).
	bool Init();

	[[nodiscard]] bool IsReady() const { return ready; }
	[[nodiscard]] bool IsDLSSAvailable() const { return dlssAvailable; }
	// The private interop device, or null before Init() succeeds. Handed to
	// external neural modules so a D3D12 module can share our device.
	[[nodiscard]] ID3D12Device* GetD3D12Device() const { return d3d12Device.get(); }
	// True only while the DLSS-D denoiser is what actually resolves frames -- not
	// merely enabled in the menu, so the OSD never claims Ray Reconstruction while
	// the upscaler has fallen back to super resolution.
	[[nodiscard]] bool IsRayReconstructionActive() const { return rayReconstruction && !rayReconstructionFailed; }
	// Active = a usable upscaler is selected (DLSS needs DLSS available; FSR does
	// not) AND we're in gameplay (not blocked by a menu/logo/loading screen).
	// Drives whether the jitter hook injects a (non-zero) Halton offset + DRS.
	[[nodiscard]] bool IsActive() const { return ready && !blocked && method != 0 && (method != 2 || dlssAvailable); }

	// Pull the live menu settings (method / quality -> render scale / sharpness)
	// into this upscaler. Called once per frame from the UpdateJitter hook BEFORE
	// the DRS write, so the DRS scale and the Evaluate render size stay in sync.
	void UpdateFromSettings();

	// Display (full output) dimensions, and the render (scaled) dimensions the
	// scene is actually drawn at. renderScale < 1.0 => real upscaling (DLSS reads
	// the low-res sub-rect and outputs at display size); renderScale == 1.0 =>
	// native DLAA. Used by the jitter hook (Halton phase + NDC scale) and by the
	// DRS write in UpdateJitter, so both agree on one source of truth.
	[[nodiscard]] uint32_t GetDisplayWidth() const { return displayWidth; }
	[[nodiscard]] uint32_t GetDisplayHeight() const { return displayHeight; }
	[[nodiscard]] float    GetRenderScale() const { return renderScale; }
	void                   SetRenderScale(float a_scale) { renderScale = a_scale; }
	[[nodiscard]] uint32_t GetRenderWidth() const { return static_cast<uint32_t>(displayWidth * renderScale + 0.5f); }
	[[nodiscard]] uint32_t GetRenderHeight() const { return static_cast<uint32_t>(displayHeight * renderScale + 0.5f); }

	// Per-frame processing at the Main::DrawWorld pre-UI hook.
	// Increment 2a: a D3D11->D3D12->D3D11 identity round-trip of the main color
	// target to validate cross-device copy + shared-fence sync (image must stay
	// unchanged). DLSS replaces the middle copy next.
	void Evaluate();

	// DLSS-G inputs (on the proxy D3D12 device). colorOutput = the upscaled,
	// pre-UI scene color (HUD-less); motionVectors/depth are the render-res
	// sub-rect. Valid after a successful Evaluate this frame. Used by the proxy
	// present's Upscaling::TagDLSSGInputs.
	[[nodiscard]] ID3D12Resource* GetHudlessColor12() const;
	[[nodiscard]] ID3D12Resource* GetMotionVectors12() const;
	[[nodiscard]] ID3D12Resource* GetDepth12() const;

	// The shared fence the upscaler signals after its D3D12 DLSS work completes,
	// and the value of that signal for the most recent Evaluate. The proxy present
	// waits on this before copying the upscaler's motion-vector/depth outputs for
	// DLSS-G (cross-queue sync -- the upscaler runs on its own queue).
	[[nodiscard]] ID3D12Fence* GetWorkFence() const;
	[[nodiscard]] uint64_t     GetWorkFenceValue() const { return workFenceValue.load(std::memory_order_acquire); }
	// Enqueue a signal after the present queue has copied the shared DLSS-G
	// inputs. Evaluate waits for this value on D3D11 before overwriting them.
	HRESULT SignalPresentInputsConsumed(ID3D12CommandQueue* a_presentQueue);
	// Streamline token used by the most recent successful DLSS evaluation.  The
	// proxy present must use this exact token for DLSS-G tags and Reflex markers;
	// the game frame counter may already have advanced by the time Present runs.
	[[nodiscard]] uint32_t GetWorkFrameTokenIndex() const { return workFrameTokenIndex.load(std::memory_order_acquire); }

	bool evalEnabled = true;

private:
	bool CreateD3D12Device(IDXGIAdapter* a_adapter);
	bool CreateCommandInfrastructure();
	bool CreateSharedFence();
	bool CreateSharedTextures(uint32_t a_width, uint32_t a_height);
	// The interop textures are sized to the display. Everything downstream sizes
	// itself from them -- the neural guides and the DLSS-G per-index copies both
	// compare against their descriptions -- so recreating these is what makes a
	// resolution change take effect without restarting the game.
	bool RecreateForDisplaySize(uint32_t a_width, uint32_t a_height);
	// Configure/enable DLSS-G for this frame (Reflex + slDLSSGSetOptions) based on
	// the live settings; sets Upscaling::frameGenerationActive. Called from
	// Evaluate after a successful upscale (frame-gen experiment only).
	void ConfigureFrameGeneration(float a_renderW, float a_renderH, float a_displayW, float a_displayH);

	winrt::com_ptr<ID3D11Device5>            d3d11Device;
	winrt::com_ptr<ID3D12Device>             d3d12Device;
	winrt::com_ptr<ID3D12CommandQueue>       commandQueue;
	winrt::com_ptr<ID3D12CommandAllocator>   commandAllocator;
	winrt::com_ptr<ID3D12GraphicsCommandList> commandList;
	winrt::com_ptr<ID3D12Fence>              fence;
	winrt::handle                            fenceEvent;
	uint64_t                                 fenceValue = 0;

	// Cross-device sync: one fence shared between the game's D3D11 device and
	// our D3D12 device (D3D12 creates it shared; D3D11 opens the same object).
	winrt::com_ptr<ID3D12Fence>              sharedFence;
	winrt::com_ptr<ID3D11Fence>              d3d11Fence;
	winrt::com_ptr<ID3D12Fence>              presentConsumptionFence;
	winrt::com_ptr<ID3D11Fence>              d3d11PresentConsumptionFence;
	winrt::com_ptr<ID3D11DeviceContext4>     d3d11Context4;
	std::atomic_uint64_t                     syncValue{ 0 };
	std::atomic_uint64_t                     presentConsumptionSignalValue{ 0 };
	std::atomic_uint64_t                     presentInputsConsumedValue{ 0 };

	// Interop textures (display-sized; DLSS reads a render-sized sub-region).
	std::unique_ptr<D3D11D3D12SharedTexture> colorInput;
	std::unique_ptr<D3D11D3D12SharedTexture> colorOutput;
	std::unique_ptr<D3D11D3D12SharedTexture> motionVectors;
	std::unique_ptr<D3D11D3D12SharedTexture> depth;
	// The engine's TAA mask, handed to the upscaler as a transparency hint. It
	// marks the pixels the engine itself refuses to accumulate temporally --
	// particles, water, anything alpha-blended -- which is what both DLSS and FSR
	// want to know to stop those from ghosting.
	std::unique_ptr<D3D11D3D12SharedTexture> transparencyMask;
	// DLSS-NR writes here rather than in place, and the upscaler then reads it
	// instead of colorInput. D3D12-only: nothing on the D3D11 side needs it.
	winrt::com_ptr<ID3D12Resource>           neuralColor;
	winrt::com_ptr<ID3D12DescriptorHeap>     neuralColorRTVHeap;  // one RTV, for the debug clear
	// NIS sharpen output. Separate from colorOutput because NIS cannot sharpen a
	// texture in place, and separate from neuralColor because the uplift reads
	// the sharpened image rather than replacing it.
	winrt::com_ptr<ID3D12Resource>           sharpenedColor;
	bool EnsureSharpenedColor();
	bool EnsureNeuralColor();

	uint32_t displayWidth = 0;
	uint32_t displayHeight = 0;
	// 0.6667 = DLSS Quality (render 2/3 of display, upscale to full). 1.0 = DLAA.
	float renderScale = 0.6667f;
	// Live settings mirror (see UpdateFromSettings). method: 0=off,1=FSR,2=DLSS.
	uint32_t method = 2;
	uint32_t qualityMode = 1;
	uint32_t dlssPreset = 0;
	float    sharpness = 0.0f;
	// Ray Reconstruction (DLSS-D) in place of DLSS super resolution. Mirrored from
	// the menu each frame, and only ever true when the feature actually came up.
	bool     rayReconstruction = false;
	bool     rayReconstructionFailed = false;  // latched so the fallback logs once, not every frame

	// DLSS 5 Neural Rendering ("uplift"), run on the scene colour before the
	// upscaler resolves it. Mirrored from the menu each frame.
	bool     neuralRendering = false;
	bool     neuralRenderingActive = false;    // it actually ran this frame
	bool     neuralRenderingFailed = false;    // latched so the fallback logs once
	// NGX creates the feature lazily; that creation must be submitted on its own,
	// after the queue drains, and the uplift skipped for that one frame.
	bool     neuralRenderingSkipFrame = false;
	bool     neuralAfterUpscale = true;        // uplift the resolved image, not the raw scene
	bool     neuralTemporal = true;            // let the model keep a history (see Settings)
	float    neuralMotionScaleX = 1.0f;        // sign/magnitude applied to the uplift's motion
	float    neuralMotionScaleY = 1.0f;
	bool     neuralDebugBypass = false;
	bool     neuralDebugDifference = false;
	float    neuralDebugDifferenceGain = 10.0f;
	uint32_t neuralEncoding = 1;
	float    neuralDiffuseWhiteNits = 0.0f;
	bool     loggedNeuralDebugBypass = false;
	// Set for the frames where the uplift ran after the upscaler: present and
	// DLSS-G read this instead of colorOutput. Null on every other frame.
	ID3D12Resource* neuralColorReady = nullptr;
	// The upscaler's output for this frame, sharpened if NIS ran. Null outside a
	// successful evaluate.
	ID3D12Resource* resolvedSceneColor = nullptr;
	ID3D12Resource* loggedPresentOverride = nullptr;
	uint32_t loggedNeuralDecision = 0xFFFFFFFF;
	std::size_t loggedNeuralConfig = 0;
	bool     transparencyHint = true;
	bool     presentOverride = true;
	ID3D12Resource* loggedCopyBackSource = nullptr;
	bool     loggedActiveState = false;
	bool     loggedTransparencyMismatch = false;
	uint32_t pendingDisplayWidth = 0;
	uint32_t pendingDisplayHeight = 0;
	uint32_t neuralFramesTotal = 0;
	uint32_t neuralFramesActive = 0;

	void FillNeuralColorForDebug();
	void ReportNeuralFailure();
	void ReportNeuralRecovered();
	// True while a menu/logo/loading screen is up (set from UpdateFromSettings).
	bool     blocked = true;
	// sharedFence value after the most recent Evaluate's D3D12 DLSS signal.
	std::atomic_uint64_t workFenceValue{ 0 };
	std::atomic_uint32_t workFrameTokenIndex{ std::numeric_limits<uint32_t>::max() };
	// Engine-TAA management so toggling the upscaler restores the player's TAA.
	bool taaOriginalKnown = false;
	bool taaOriginal = false;
	bool taaOffApplied = false;
	bool ready = false;
	bool dlssAvailable = false;
};
