#include "PCH.h"

#include "Upscaler/D3D12Upscaler.h"

#include "Game/Renderer.h"
#include "Game/Util.h"
#include "Render/D3D12NeuralGBuffer.h"
#include "Render/DX12SwapChain.h"  // D3D11D3D12SharedTexture
#include "Render/Streamline.h"
#include "Render/FidelityFX.h"
#include "Settings/Settings.h"
#include "Upscaler/Upscaling.h"  // kFrameGenExperiment

namespace
{
	D3D11_TEXTURE2D_DESC MakeInteropDesc(uint32_t a_width, uint32_t a_height, DXGI_FORMAT a_format)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = a_width;
		desc.Height = a_height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = a_format;
		desc.SampleDesc = { 1, 0 };
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
		return desc;
	}
}

D3D12Upscaler* D3D12Upscaler::GetSingleton()
{
	static D3D12Upscaler singleton;
	return &singleton;
}

D3D12Upscaler::~D3D12Upscaler() = default;

bool D3D12Upscaler::CreateD3D12Device(IDXGIAdapter* a_adapter)
{
	if (FAILED(D3D12CreateDevice(a_adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(d3d12Device.put())))) {
		logger::error("[D3D12Upscaler] D3D12CreateDevice failed");
		return false;
	}
	return true;
}

bool D3D12Upscaler::CreateCommandInfrastructure()
{
	// The upscaler uses its OWN command queue. (Sharing the proxy present queue was
	// tried for DLSS-G frame gen but its blocking Wait on the D3D11 fence risks
	// stalling the present queue, and it did not fix the DLSS-G hang -- so keep the
	// upscaler independent; this is the stable FG-2 configuration.)
	D3D12_COMMAND_QUEUE_DESC queueDesc{};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	if (FAILED(d3d12Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(commandQueue.put())))) {
		logger::error("[D3D12Upscaler] CreateCommandQueue failed");
		return false;
	}
	if (FAILED(d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(commandAllocator.put())))) {
		logger::error("[D3D12Upscaler] CreateCommandAllocator failed");
		return false;
	}
	if (FAILED(d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator.get(), nullptr, IID_PPV_ARGS(commandList.put())))) {
		logger::error("[D3D12Upscaler] CreateCommandList failed");
		return false;
	}
	if (FAILED(commandList->Close())) {  // start closed so it can be Reset before recording
		logger::error("[D3D12Upscaler] Initial command-list Close failed");
		return false;
	}
	if (FAILED(d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put())))) {
		logger::error("[D3D12Upscaler] CreateFence failed");
		return false;
	}
	fenceEvent.attach(CreateEventW(nullptr, FALSE, FALSE, nullptr));
	return fenceEvent.get() != nullptr;
}

bool D3D12Upscaler::CreateSharedFence()
{
	HANDLE rawHandle = nullptr;
	if (FAILED(d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(sharedFence.put())))) {
		logger::error("[D3D12Upscaler] CreateFence(shared) failed");
		return false;
	}
	if (FAILED(d3d12Device->CreateSharedHandle(sharedFence.get(), nullptr, GENERIC_ALL, nullptr, &rawHandle))) {
		logger::error("[D3D12Upscaler] CreateSharedHandle(fence) failed");
		return false;
	}
	winrt::handle handle;
	handle.attach(rawHandle);
	const HRESULT hr = d3d11Device->OpenSharedFence(handle.get(), IID_PPV_ARGS(d3d11Fence.put()));
	if (FAILED(hr)) {
		logger::error("[D3D12Upscaler] OpenSharedFence failed");
		return false;
	}

	if (FAILED(d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(presentConsumptionFence.put())))) {
		logger::error("[D3D12Upscaler] CreateFence(present consumption) failed");
		return false;
	}
	HANDLE rawPresentHandle = nullptr;
	if (FAILED(d3d12Device->CreateSharedHandle(presentConsumptionFence.get(), nullptr, GENERIC_ALL, nullptr, &rawPresentHandle))) {
		logger::error("[D3D12Upscaler] CreateSharedHandle(present consumption) failed");
		return false;
	}
	winrt::handle presentHandle;
	presentHandle.attach(rawPresentHandle);
	if (FAILED(d3d11Device->OpenSharedFence(presentHandle.get(), IID_PPV_ARGS(d3d11PresentConsumptionFence.put())))) {
		logger::error("[D3D12Upscaler] OpenSharedFence(present consumption) failed");
		return false;
	}
	return true;
}

bool D3D12Upscaler::CreateSharedTextures(uint32_t a_width, uint32_t a_height)
{
	try {
		colorInput = std::make_unique<D3D11D3D12SharedTexture>(
			MakeInteropDesc(a_width, a_height, DXGI_FORMAT_R16G16B16A16_FLOAT), d3d11Device.get(), d3d12Device.get());
		colorOutput = std::make_unique<D3D11D3D12SharedTexture>(
			MakeInteropDesc(a_width, a_height, DXGI_FORMAT_R16G16B16A16_FLOAT), d3d11Device.get(), d3d12Device.get());
		motionVectors = std::make_unique<D3D11D3D12SharedTexture>(
			MakeInteropDesc(a_width, a_height, DXGI_FORMAT_R16G16_FLOAT), d3d11Device.get(), d3d12Device.get());
		depth = std::make_unique<D3D11D3D12SharedTexture>(
			MakeInteropDesc(a_width, a_height, DXGI_FORMAT_R32_FLOAT), d3d11Device.get(), d3d12Device.get());
	} catch (const std::exception& e) {
		logger::error("[D3D12Upscaler] Shared texture creation failed: {}", e.what());
		return false;
	}
	return true;
}

bool D3D12Upscaler::Init()
{
	if (ready) {
		return dlssAvailable;
	}

	try {
		// 1. Game D3D11 device (as ID3D11Device5 for shared-NT-handle support).
		auto* gameDevice = Game::GetD3D11Device();
		if (!gameDevice || FAILED(gameDevice->QueryInterface(IID_PPV_ARGS(d3d11Device.put())))) {
			logger::error("[D3D12Upscaler] Game D3D11 device unavailable / not ID3D11Device5");
			return false;
		}

		if constexpr (Upscaling::kFrameGenExperiment) {
			// Frame-gen experiment: the D3D12 proxy swapchain already created a
			// D3D12 device and initialised Streamline in D3D12 mode (+PostDevice).
			// REUSE that device so we don't init Streamline twice; just build our
			// own command infrastructure + cross-device fence + shared textures on
			// it. This lets DLSS upscaling and the proxy present share one device.
			auto* proxy = DX12SwapChain::GetSingleton();
			d3d12Device = proxy->d3d12Device;
			if (!d3d12Device) {
				logger::error("[D3D12Upscaler] Proxy D3D12 device unavailable (frame-gen experiment)");
				return false;
			}
			if (!CreateCommandInfrastructure() || !CreateSharedFence()) {
				return false;
			}
			if (auto* ctx = Game::GetD3D11Context()) {
				ctx->QueryInterface(IID_PPV_ARGS(d3d11Context4.put()));
			}
			if (!d3d11Context4) {
				logger::error("[D3D12Upscaler] ID3D11DeviceContext4 unavailable (needed for cross-device sync)");
				return false;
			}
			dlssAvailable = Streamline::GetSingleton()->featureDLSS;
		} else {
			// 2. Adapter.
			winrt::com_ptr<IDXGIDevice> dxgiDevice;
			winrt::com_ptr<IDXGIAdapter> adapter;
			if (FAILED(d3d11Device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put()))) ||
				FAILED(dxgiDevice->GetAdapter(adapter.put()))) {
				logger::error("[D3D12Upscaler] Could not resolve DXGI adapter");
				return false;
			}

			// 3. Private D3D12 device + command infrastructure + cross-device fence.
			if (!CreateD3D12Device(adapter.get()) || !CreateCommandInfrastructure() || !CreateSharedFence()) {
				return false;
			}
			if (auto* ctx = Game::GetD3D11Context()) {
				ctx->QueryInterface(IID_PPV_ARGS(d3d11Context4.put()));
			}
			if (!d3d11Context4) {
				logger::error("[D3D12Upscaler] ID3D11DeviceContext4 unavailable (needed for cross-device sync)");
				return false;
			}

			// 4. Streamline in D3D12 mode on our private device.
			auto* sl = Streamline::GetSingleton();
			sl->LoadInterposer();
			if (sl->interposer) {
				sl->Initialize(sl::RenderAPI::eD3D12);
				if (sl->slSetD3DDevice) {
					sl->slSetD3DDevice(d3d12Device.get());
				}
				sl->CheckFeatures(adapter.get());
				sl->PostDevice();  // resolves DLSS-specific fn ptrs (slDLSSSetOptions etc.)
				dlssAvailable = sl->featureDLSS;
			} else {
				logger::warn("[D3D12Upscaler] Streamline interposer not loaded; DLSS unavailable");
			}
		}

		// 5. Display-sized interop textures (DLSS reads a render-sized sub-region).
		if (auto* state = RE::BSGraphics::State::GetSingleton()) {
			displayWidth = state->screenWidth;
			displayHeight = state->screenHeight;
		}
		if (displayWidth == 0 || displayHeight == 0) {
			displayWidth = 1920;
			displayHeight = 1080;
		}
		if (!CreateSharedTextures(displayWidth, displayHeight)) {
			return false;
		}

		ready = true;
		logger::info("[D3D12Upscaler] Initialised: D3D12 interop ready {}x{}, DLSS(SR)={}",
			displayWidth, displayHeight, dlssAvailable);
		return dlssAvailable;
	} catch (const std::exception& e) {
		logger::error("[D3D12Upscaler] Init failed: {}", e.what());
		return false;
	}
}

namespace
{
	// Disable the engine's own TAA resolve so it doesn't fight our upscaler. The
	// global UnkOuterStruct (SE 527731 / AE 414660) holds an inner struct pointer
	// at +0x1F0 whose bTAA flag sits at +0x18 (PureDark's layout). With TAA off
	// and DRS on, the engine renders the scene into a low-res sub-rect and does
	// NOT stretch it -- our DLSS eval is what resolves the sub-rect to full res.
	bool* EngineTAAFlag()
	{
		const REL::Relocation<std::uintptr_t> global{ REL::VariantID(527731, 414660, 0x34234C0) };
		const auto outer = *reinterpret_cast<std::uintptr_t*>(global.address());
		if (!outer) {
			return nullptr;
		}
		const auto inner = *reinterpret_cast<std::uintptr_t*>(outer + 0x1F0);
		if (!inner) {
			return nullptr;
		}
		return reinterpret_cast<bool*>(inner + 0x18);
	}

	void SetEngineTAA(bool a_enabled)
	{
		try {
			if (auto* flag = EngineTAAFlag()) {
				*flag = a_enabled;
			}
		} catch (...) {
		}
	}

	bool GetEngineTAA()
	{
		try {
			if (auto* flag = EngineTAAFlag()) {
				return *flag;
			}
		} catch (...) {
		}
		return false;
	}

	// Standard temporal-upscaler render-scale ladder (matches DLSS quality modes).
	// qualityMode: 0=Native/DLAA, 1=Quality, 2=Balanced, 3=Performance, 4=Ultra.
	float RenderScaleForQuality(uint32_t a_qualityMode)
	{
		switch (a_qualityMode) {
		case 1:  return 0.66667f;  // Quality        (1/1.5)
		case 2:  return 0.58000f;  // Balanced       (1/1.72)
		case 3:  return 0.50000f;  // Performance    (1/2)
		case 4:  return 0.33333f;  // Ultra Perf     (1/3)
		case 0:
		default: return 1.0f;      // Native AA / DLAA
		}
	}

	// After we resolve to full res, tell the downstream imagespace/UI passes the
	// frame is full-res again by resetting the dynamic-resolution current scale
	// AND ratio to 1.0 (the scene already rendered low-res before this point).
	void ResetDynamicResolution()
	{
		auto* state = RE::BSGraphics::State::GetSingleton();
		if (!state) {
			return;
		}
		static const std::size_t rtBase = REL::Relocate<std::size_t>(0x58, 0x60);
		auto* p = reinterpret_cast<std::uint8_t*>(state);
		for (std::size_t off : { std::size_t{ 0xA4 }, std::size_t{ 0xA8 }, std::size_t{ 0xB4 }, std::size_t{ 0xB8 } }) {
			*reinterpret_cast<float*>(p + rtBase + off) = 1.0f;
		}
	}

	void Transition(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_res,
		D3D12_RESOURCE_STATES a_from, D3D12_RESOURCE_STATES a_to)
	{
		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		barrier.Transition.pResource = a_res;
		barrier.Transition.StateBefore = a_from;
		barrier.Transition.StateAfter = a_to;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		a_list->ResourceBarrier(1, &barrier);
	}
}

// The final pre-UI scene colour: the uplift's output when Neural Rendering ran
// after the upscaler this frame, otherwise the upscaler's own output. Present
// and DLSS-G both go through here so they always see the same image.
ID3D12Resource* D3D12Upscaler::GetHudlessColor12() const
{
	if (neuralColorReady) {
		return neuralColorReady;
	}
	return colorOutput ? colorOutput->resource12.get() : nullptr;
}
ID3D12Resource* D3D12Upscaler::GetMotionVectors12() const { return motionVectors ? motionVectors->resource12.get() : nullptr; }
ID3D12Resource* D3D12Upscaler::GetDepth12() const { return depth ? depth->resource12.get() : nullptr; }
ID3D12Fence* D3D12Upscaler::GetWorkFence() const { return sharedFence.get(); }

HRESULT D3D12Upscaler::SignalPresentInputsConsumed(ID3D12CommandQueue* a_presentQueue)
{
	if (!a_presentQueue || !presentConsumptionFence) {
		return E_POINTER;
	}

	const auto consumedValue = presentConsumptionSignalValue.fetch_add(1, std::memory_order_acq_rel) + 1;
	const auto result = a_presentQueue->Signal(presentConsumptionFence.get(), consumedValue);
	if (FAILED(result)) {
		logger::critical("[D3D12Upscaler] Could not signal present-input consumption value={} result=0x{:08X}", consumedValue, static_cast<uint32_t>(result));
		return result;
	}
	presentInputsConsumedValue.store(consumedValue, std::memory_order_release);
	return S_OK;
}

// DLSS-G re-enabled on the present-override foundation (Step B). Now the DLSS
// output is fully D3D12-native as the final present color, so DLSS-G gets a
// coherent, paceable present timeline (the earlier kMAIN round-trip gave it an
// erratic one it couldn't pace -> the DEVICE_HUNGs). hud-less = the D3D12 DLSS
// output (colorOutput12); the composited scene+UI is the backbuffer DLSS-G reads.
// Safety gate: DLSS-G currently causes a driver-level TDR/bugcheck on the
// target system. Keep the feature hard-disabled until the proxy-present and
// Streamline synchronization path has been validated without risking another
// machine-wide crash. DLSS-SR and FSR remain available.
// present-override (fo4test flow): keep the DLSS output on D3D12 as the final
// present color instead of copying it back to the game's D3D11 kMAIN. kMAIN is
// then cleared so the game renders UI-only onto black, and the proxy present
// composites (D3D12UIComposite) the DLSS scene + that UI. This gives DLSS-G a
// coherent D3D12-native present it can pace (the round-trip did not). Off = the
// stable kMAIN-round-trip path.
static constexpr bool kPresentOverride = true;

void D3D12Upscaler::ConfigureFrameGeneration(float a_renderW, float a_renderH, float a_displayW, float a_displayH)
{
	auto* sl = Streamline::GetSingleton();
	auto* up = Upscaling::GetSingleton();
	const auto& s = SettingsStore::GetSingleton()->settings;

	// Frame gen wants: DLSS-G available, enabled in the menu, DLSS method active
	// and in-world (IsActive). DLSS-G REQUIRES Reflex, so force Reflex on while
	// generating (otherwise Streamline reports eFailReflexNotDetectedAtRuntime).
	// Never re-arm after the present watchdog latched frame generation off.
	const bool want = Upscaling::kEnableDLSSG && IsActive() && method == 2 && sl->featureDLSSG &&
		s.frameGenerationMode != 0 && !DX12SwapChain::GetSingleton()->IsDLSSGAutoDisabled();
	sl->UpdateReflex(want ? (s.reflexMode == 0 ? 1u : s.reflexMode) : s.reflexMode, want);

	sl->UpdateDLSSG(
		want,
		s.frameGenerationMode,
		s.dlssgGeneratedFrames + 1,
		s.dynamicMFGEnabled != 0,
		s.dynamicMFGTargetFPS,
		float2(a_renderW, a_renderH),
		float2(a_displayW, a_displayH),
		DXGI_FORMAT_R16G16B16A16_FLOAT,  // hudless color = D3D12-native DLSS output
		DXGI_FORMAT_R16G16_FLOAT,        // motion vectors
		DXGI_FORMAT_R32_FLOAT,           // depth
		DXGI_FORMAT_UNKNOWN);            // UI recomposed from backbuffer - hudless

	up->frameGenerationActive = want && sl->dlssgActive;

	static bool loggedOn = false;
	if (up->frameGenerationActive && !loggedOn) {
		loggedOn = true;
		logger::info("[D3D12Upscaler] DLSS-G ENABLED mode={} framesToGen={} render={}x{} display={}x{}",
			s.frameGenerationMode, s.dlssgGeneratedFrames + 1,
			static_cast<int>(a_renderW), static_cast<int>(a_renderH),
			static_cast<int>(a_displayW), static_cast<int>(a_displayH));
	}
}

void D3D12Upscaler::UpdateFromSettings()
{
	const auto& s = SettingsStore::GetSingleton()->settings;
	method = s.upscaleMethodPreference;   // 0=off, 1=FSR, 2=DLSS
	qualityMode = s.qualityMode;          // 0=Native/DLAA .. 4=Ultra
	dlssPreset = s.dlssModelPreset;
	sharpness = s.sharpness;
	// Ray Reconstruction replaces DLSS super resolution with the DLSS-D denoiser.
	// It is a DLSS-path-only option and needs the feature to have come up.
	rayReconstruction = s.neuralRayReconstruction != 0 && Streamline::GetSingleton()->featureDLSSD;
	// DLSS 5 Neural Rendering. Like RR this is a DLSS-path option, and only ever
	// true once a backend (Streamline plugin or direct NGX) actually came up.
	neuralRendering = s.dlssNREnabled != 0 && Streamline::GetSingleton()->IsDLSSNRUsable();
	neuralAfterUpscale = s.dlssNRAfterUpscale != 0;
	neuralDebugBypass = s.dlssNRDebugBypass != 0;
	// Blocked while a menu / logo / loading screen is up (not the jittered 3D
	// scene) -- upscaling those warps the image, so treat the upscaler as inactive.
	blocked = Upscaling::GetSingleton()->ShouldBlockUpscaling();
	// Mirror the active method for the OSD / method-selection logic.
	Upscaling::GetSingleton()->upscaleMethod =
		method == 1 ? Upscaling::UpscaleMethod::kFSR :
		method == 2 ? Upscaling::UpscaleMethod::kDLSS :
					  Upscaling::UpscaleMethod::kDisabled;
	// Method off, or a native-AA quality on the FSR path (FSR has no DLAA), means
	// render at full res (no DRS). DLSS keeps DLAA at quality 0.
	renderScale = (method == 0) ? 1.0f : RenderScaleForQuality(qualityMode);
}

void D3D12Upscaler::ReportNeuralFailure()
{
	if (!neuralRenderingFailed) {
		neuralRenderingFailed = true;
		logger::warn("[DLSS-NR] Uplift evaluation failed; the frame is presented without it");
	}
}

void D3D12Upscaler::ReportNeuralRecovered()
{
	if (neuralRenderingFailed) {
		neuralRenderingFailed = false;
		logger::info("[DLSS-NR] Uplift recovered");
	}
}

bool D3D12Upscaler::EnsureNeuralColor()
{
	if (neuralColor) {
		return true;
	}
	if (!d3d12Device || displayWidth == 0 || displayHeight == 0) {
		return false;
	}

	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = displayWidth;
	desc.Height = displayHeight;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	// Matches the shared scene colour, so the uplift is a drop-in for colorInput.
	desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	const auto hr = d3d12Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
		D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(neuralColor.put()));
	if (FAILED(hr)) {
		logger::warn("[DLSS-NR] Could not create the uplift target {}x{}: 0x{:08X}",
			displayWidth, displayHeight, static_cast<uint32_t>(hr));
		neuralColor = nullptr;
		return false;
	}
	logger::info("[DLSS-NR] Uplift target created {}x{}", displayWidth, displayHeight);
	return true;
}

void D3D12Upscaler::Evaluate()
{
	if (!ready) {
		return;
	}
	workFrameTokenIndex.store(std::numeric_limits<uint32_t>::max(), std::memory_order_release);
	if (const auto removedReason = d3d12Device->GetDeviceRemovedReason(); FAILED(removedReason)) {
		logger::critical("[D3D12Upscaler] Evaluate disabled because the D3D12 device is removed: 0x{:08X}", static_cast<uint32_t>(removedReason));
		ready = false;
		workFenceValue.store(0, std::memory_order_release);
		Streamline::GetSingleton()->RequestDLSSGDisable();
		return;
	}

	// Capture the player's original engine-TAA state once, so we can restore it
	// when the upscaler is turned off from the menu.
	if (!taaOriginalKnown) {
		taaOriginal = GetEngineTAA();
		taaOriginalKnown = true;
	}

	const bool upscaling = renderScale < 0.999f;
	const bool active = IsActive();

	// Engine TAA: off while actively upscaling (so it can't touch the low-res
	// scene before our resolve), restored to the player's setting otherwise.
	const bool wantTAAOff = active && upscaling;
	if (wantTAAOff && !taaOffApplied) {
		SetEngineTAA(false);
		taaOffApplied = true;
		logger::info("[D3D12Upscaler] Engine TAA disabled for upscaling (renderScale={})", renderScale);
	} else if (!wantTAAOff && taaOffApplied) {
		SetEngineTAA(taaOriginal);
		taaOffApplied = false;
		logger::info("[D3D12Upscaler] Engine TAA restored to {}", taaOriginal);
	}

	if (!active) {
		// The uplift did not run, so the present and DLSS-G must go back to the
		// upscaler's own output. Leaving this set would hand them last active
		// frame's image while a menu or loading screen is up.
		neuralColorReady = nullptr;
		neuralRenderingActive = false;
		// Nothing to do; make sure the frame is presented at full resolution and
		// DLSS-G is disabled (so the present doesn't tag stale resources).
		{
			auto* up = Upscaling::GetSingleton();
			up->osdRenderSize = { static_cast<float>(displayWidth), static_cast<float>(displayHeight) };
			up->osdNativeSize = up->osdRenderSize;
		}
		if (upscaling) {
			ResetDynamicResolution();
		}
		if constexpr (Upscaling::kFrameGenExperiment) {
			ConfigureFrameGeneration(
				static_cast<float>(displayWidth), static_cast<float>(displayHeight),
				static_cast<float>(displayWidth), static_cast<float>(displayHeight));
		}
		return;
	}

	// Inputs: kMAIN color, kMOTION_VECTOR, and kSAO_CAMERAZ (R32_FLOAT linear
	// depth, matches our shared depth format).
	auto* kMain = Game::GetRenderTargetTexture(RE::RENDER_TARGET::kMAIN);
	auto* kMv = Game::GetRenderTargetTexture(RE::RENDER_TARGET::kMOTION_VECTOR);
	auto* kDepth = Game::GetRenderTargetTexture(RE::RENDER_TARGET::kSAO_CAMERAZ);
	if (!kMain || !kMv || !kDepth || !d3d11Context4 || !colorInput || !colorOutput || !motionVectors || !depth) {
		return;
	}

	auto* sl = Streamline::GetSingleton();

	try {
		// The previous Present may still be copying motion/depth/color from these
		// shared resources on its own D3D12 queue. Queue a D3D11-side wait before
		// overwriting them for this frame.
		if (const auto consumedValue = presentInputsConsumedValue.exchange(0, std::memory_order_acq_rel); consumedValue != 0) {
			DX::ThrowIfFailed(d3d11Context4->Wait(d3d11PresentConsumptionFence.get(), consumedValue));
		}
		// --- D3D11: copy upscaler inputs into shared textures, signal the fence ---
		d3d11Context4->CopyResource(colorInput->resource11.get(), kMain);
		d3d11Context4->CopyResource(motionVectors->resource11.get(), kMv);
		d3d11Context4->CopyResource(depth->resource11.get(), kDepth);
		const auto inputsReadyValue = syncValue.fetch_add(1, std::memory_order_acq_rel) + 1;
		DX::ThrowIfFailed(d3d11Context4->Signal(d3d11Fence.get(), inputsReadyValue));

		const auto* frame = Util::CameraFrame::GetSingleton();
		const DirectX::XMFLOAT2 jitter = frame->useJitter ? frame->jitter : DirectX::XMFLOAT2{ 0.0f, 0.0f };

		// --- DLSS needs its per-frame camera constants + frame token up front ---
		sl::FrameToken* frameToken = nullptr;
		if (method == 2) {
			if (!sl->UpdateConstants(float2(jitter.x, jitter.y))) {
				return;
			}
			frameToken = sl->GetFrameTokenForFrame(sl->GetCurrentFrameTokenIndex());
			if (!frameToken) {
				return;
			}
		}

		// --- D3D12: wait for D3D11 copies, run the upscaler, signal ---
		DX::ThrowIfFailed(commandQueue->Wait(sharedFence.get(), inputsReadyValue));
		// A D3D12 command allocator cannot be reset until every submitted command
		// list that used it has completed on the GPU. The D3D11-side Wait below is
		// only a queued GPU dependency; it does not block this CPU thread.
		if (fenceValue != 0 && fence->GetCompletedValue() < fenceValue) {
			DX::ThrowIfFailed(fence->SetEventOnCompletion(fenceValue, fenceEvent.get()));
			constexpr DWORD kGpuWaitTimeoutMs = 5000;
			const auto waitResult = WaitForSingleObjectEx(fenceEvent.get(), kGpuWaitTimeoutMs, FALSE);
			if (waitResult != WAIT_OBJECT_0) {
				logger::critical(
					"[D3D12Upscaler] GPU command fence wait failed/timed out result={} completed={} expected={}; disabling upscaler",
					waitResult,
					fence->GetCompletedValue(),
					fenceValue);
				ready = false;
				return;
			}
		}
		DX::ThrowIfFailed(commandAllocator->Reset());
		DX::ThrowIfFailed(commandList->Reset(commandAllocator.get(), nullptr));

		const float2 renderSize{ static_cast<float>(GetRenderWidth()), static_cast<float>(GetRenderHeight()) };
		const float2 displaySize{ static_cast<float>(displayWidth), static_cast<float>(displayHeight) };
		// Feed the on-screen display: "Res: <render> -> <display>".
		{
			auto* up = Upscaling::GetSingleton();
			up->osdRenderSize = renderSize;
			up->osdNativeSize = displaySize;
		}
		bool ok = false;
		bool sharpened = false;

		// ---- DLSS 5 Neural Rendering ("uplift") ------------------------------
		// Running the uplift BEFORE the upscaler means DLSS's temporal resolve
		// then averages most of the added detail back out -- it costs the frame
		// time and shows almost nothing. Running it AFTER leaves the detail on
		// screen, but the guides must then be display resolution, which our
		// render-resolution motion vectors and depth only are when the upscaler
		// is not actually scaling. So: after by default, before when scaling.
		ID3D12Resource* upscalerInput = colorInput->resource12.get();
		neuralColorReady = nullptr;
		neuralRenderingActive = false;

		nvngx::dlss_nr::D3D12EvaluationParameters nrParameters{};
		bool       nrWanted = method == 2 && neuralRendering && EnsureNeuralColor();
		const bool nrGuidesAreDisplayRes = GetRenderWidth() == displayWidth && GetRenderHeight() == displayHeight;
		const bool nrAfterUpscale = neuralAfterUpscale && nrGuidesAreDisplayRes;
		if (nrWanted && neuralAfterUpscale && !nrGuidesAreDisplayRes && !loggedNeuralOrderFallback) {
			loggedNeuralOrderFallback = true;
			logger::info("[DLSS-NR] Uplift asked to run after the upscaler, but the guides are render resolution ({}x{} vs {}x{}); running before it instead",
				GetRenderWidth(), GetRenderHeight(), displayWidth, displayHeight);
		}

		if (nrWanted) {
			const auto nrWidth = nrAfterUpscale ? displayWidth : GetRenderWidth();
			const auto nrHeight = nrAfterUpscale ? displayHeight : GetRenderHeight();

			nrParameters = Streamline::MakeDLSSNRParameters();
			nrParameters.motionVectors = motionVectors->resource12.get();
			nrParameters.depth = depth->resource12.get();
			nrParameters.inputWidth = nrParameters.outputWidth = nrParameters.guideWidth = nrWidth;
			nrParameters.inputHeight = nrParameters.outputHeight = nrParameters.guideHeight = nrHeight;
			// Skyrim's motion vectors are normalised screen space (Streamline runs
			// them at mvecScale 1,1); NGX wants pixels, so scale by the guide size.
			nrParameters.motionVectorScaleX = static_cast<float>(nrWidth);
			nrParameters.motionVectorScaleY = static_cast<float>(nrHeight);
			nrParameters.depthInverted = false;
			nrParameters.outputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
			nrParameters.reset = neuralRenderingSkipFrame;

			// NGX creates the feature lazily and that creation must not share a
			// submission with an evaluation. Handle it here, before any of this
			// frame's work is recorded: we already waited on the work fence above,
			// so the queue is idle. Record creation alone, submit it, wait, and
			// start a fresh list.
			if (sl->NeedsDLSSNRPreparation(nrParameters)) {
				const bool prepared = sl->PrepareDLSSNR(commandList.get(), nrParameters);
				DX::ThrowIfFailed(commandList->Close());
				ID3D12CommandList* prepareLists[] = { commandList.get() };
				commandQueue->ExecuteCommandLists(1, prepareLists);
				DX::ThrowIfFailed(commandQueue->Signal(fence.get(), ++fenceValue));
				if (fence->GetCompletedValue() < fenceValue) {
					DX::ThrowIfFailed(fence->SetEventOnCompletion(fenceValue, fenceEvent.get()));
					if (WaitForSingleObjectEx(fenceEvent.get(), 5000, FALSE) != WAIT_OBJECT_0) {
						logger::critical("[DLSS-NR] Feature creation did not complete; disabling the upscaler");
						ready = false;
						return;
					}
				}
				DX::ThrowIfFailed(commandAllocator->Reset());
				DX::ThrowIfFailed(commandList->Reset(commandAllocator.get(), nullptr));
				// Creation changes the working set: let one plain frame through
				// before evaluating the new feature.
				neuralRenderingSkipFrame = true;
				nrWanted = false;
				logger::info("[DLSS-NR] Feature creation submitted prepared={} size={}x{} passes={} order={}",
					prepared, nrWidth, nrHeight, nrParameters.passCount, nrAfterUpscale ? "after" : "before");
			} else if (neuralRenderingSkipFrame) {
				neuralRenderingSkipFrame = false;
				nrWanted = false;
			}
		}

		// Uplift the scene before the upscaler reads it. On success the upscaler's
		// input swaps to neuralColor; on failure it stays colorInput, unaffected.
		if (nrWanted && !nrAfterUpscale) {
			nrParameters.color = colorInput->resource12.get();
			nrParameters.output = neuralColor.get();
			if (sl->EvaluateDLSSNR(commandList.get(), nrParameters)) {
				upscalerInput = neuralColor.get();
				neuralRenderingActive = true;
				ReportNeuralRecovered();
			} else {
				ReportNeuralFailure();
			}
		}

		if (method == 2 && rayReconstruction) {
			// NVIDIA DLSS Ray Reconstruction. Skyrim has no G-buffer, so synthesise
			// the guide buffers RR needs from the depth we already share (see
			// D3D12NeuralGBuffer). If that fails, fall through to plain DLSS SR
			// rather than dropping the frame.
			const auto* cameraFrame = Util::CameraFrame::GetSingleton();
			const bool  guidesReady = cameraFrame->valid &&
                D3D12NeuralGBuffer::GetSingleton()->Generate(
					d3d12Device.get(),
					commandList.get(),
					depth->resource12.get(),
					GetRenderWidth(), GetRenderHeight(),
					displayWidth, displayHeight,
					cameraFrame->viewMat,
					cameraFrame->projMat);

			if (guidesReady) {
				auto* gbuffer = D3D12NeuralGBuffer::GetSingleton();
				ok = sl->UpscaleD3D12RR(
					upscalerInput,
					colorOutput->resource12.get(),
					motionVectors->resource12.get(),
					depth->resource12.get(),
					gbuffer->GetNormalRoughness(),
					gbuffer->GetAlbedo(),
					gbuffer->GetSpecularAlbedo(),
					commandList.get(),
					frameToken,
					renderSize, displaySize,
					cameraFrame->viewMat,
					qualityMode,
					sharpness);
			}

			if (!ok) {
				// One warning per transition, not one per frame.
				if (!rayReconstructionFailed) {
					rayReconstructionFailed = true;
					logger::warn("[D3D12Upscaler] Ray Reconstruction unavailable this frame (guides={}); using DLSS super resolution", guidesReady);
				}
				ok = sl->UpscaleD3D12(
					upscalerInput,
					colorOutput->resource12.get(),
					nullptr,  // sharpened output
					motionVectors->resource12.get(),
					depth->resource12.get(),
					nullptr,  // transparency mask
					commandList.get(),
					frameToken,
					renderSize, displaySize,
					DXGI_FORMAT_R16G16B16A16_FLOAT,
					DXGI_FORMAT_R16G16_FLOAT,
					DXGI_FORMAT_R32_FLOAT,
					qualityMode,
					sharpness,
					dlssPreset,
					&sharpened);
			} else if (rayReconstructionFailed) {
				rayReconstructionFailed = false;
				logger::info("[D3D12Upscaler] Ray Reconstruction recovered");
			}
		} else if (method == 2) {
			// NVIDIA DLSS via Streamline (Streamline manages resource states as
			// COMMON internally, so no explicit barriers here).
			ok = sl->UpscaleD3D12(
				upscalerInput,
				colorOutput->resource12.get(),
				nullptr,  // sharpened output
				motionVectors->resource12.get(),
				depth->resource12.get(),
				nullptr,  // transparency mask
				commandList.get(),
				frameToken,
				renderSize, displaySize,
				DXGI_FORMAT_R16G16B16A16_FLOAT,
				DXGI_FORMAT_R16G16_FLOAT,
				DXGI_FORMAT_R32_FLOAT,
				qualityMode,
				sharpness,
				dlssPreset,
				&sharpened);
		} else if (method == 1) {
			// AMD FSR via FidelityFX. FFX declares each resource in a specific
			// state (shader-read inputs, UAV output); our shared textures live in
			// COMMON, so bracket the dispatch with explicit transitions and return
			// them to COMMON for the next cross-device copy.
			constexpr D3D12_RESOURCE_STATES kRead =
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			Transition(commandList.get(), colorInput->resource12.get(), D3D12_RESOURCE_STATE_COMMON, kRead);
			Transition(commandList.get(), depth->resource12.get(), D3D12_RESOURCE_STATE_COMMON, kRead);
			Transition(commandList.get(), motionVectors->resource12.get(), D3D12_RESOURCE_STATE_COMMON, kRead);
			Transition(commandList.get(), colorOutput->resource12.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			ok = FidelityFX::GetSingleton()->UpscaleD3D12(
				d3d12Device.get(),
				commandList.get(),
				colorInput->resource12.get(),
				colorOutput->resource12.get(),
				motionVectors->resource12.get(),
				depth->resource12.get(),
				nullptr,  // reactive mask
				nullptr,  // opaque-only color
				float2(jitter.x, jitter.y),
				renderSize, displaySize,
				sharpness);

			Transition(commandList.get(), colorInput->resource12.get(), kRead, D3D12_RESOURCE_STATE_COMMON);
			Transition(commandList.get(), depth->resource12.get(), kRead, D3D12_RESOURCE_STATE_COMMON);
			Transition(commandList.get(), motionVectors->resource12.get(), kRead, D3D12_RESOURCE_STATE_COMMON);
			Transition(commandList.get(), colorOutput->resource12.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
		}

		// Uplift the resolved image. This is the ordering that actually shows on
		// screen, because nothing temporal runs after it to average it away.
		if (ok && nrWanted && nrAfterUpscale) {
			// The upscaler wrote colorOutput moments ago on this same list and
			// Streamline leaves it in COMMON, so nothing orders that write against
			// the uplift's read of it. Without this the uplift can sample a
			// half-written or stale image and the frame flickers between the
			// uplifted and un-uplifted result.
			const auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
			commandList->ResourceBarrier(1, &uavBarrier);

			nrParameters.color = colorOutput->resource12.get();
			nrParameters.output = neuralColor.get();
			if (neuralDebugBypass) {
				// Diagnostic: don't run the uplift, just put something unmistakably
				// different in its target and present that. colorInput shares the
				// target's format, and at render resolution it fills only part of
				// the texture -- so if the screen changes, the target reaches
				// present and the uplift itself is returning its input unchanged.
				// If the screen does NOT change, the target never reaches present.
				commandList->CopyResource(neuralColor.get(), colorInput->resource12.get());
				neuralColorReady = neuralColor.get();
				neuralRenderingActive = true;
				if (!loggedNeuralDebugBypass) {
					loggedNeuralDebugBypass = true;
					logger::warn("[DLSS-NR] DEBUG BYPASS is on: presenting the uplift target filled with the pre-upscale scene, not an uplifted image");
				}
			} else if (sl->EvaluateDLSSNR(commandList.get(), nrParameters)) {
				// Present and DLSS-G read this instead of the upscaler's output.
				neuralColorReady = neuralColor.get();
				neuralRenderingActive = true;
				ReportNeuralRecovered();
			} else {
				ReportNeuralFailure();
			}
		}

		DX::ThrowIfFailed(commandList->Close());
		ID3D12CommandList* lists[] = { commandList.get() };
		commandQueue->ExecuteCommandLists(1, lists);
		DX::ThrowIfFailed(commandQueue->Signal(fence.get(), ++fenceValue));
		const auto workReadyValue = syncValue.fetch_add(1, std::memory_order_acq_rel) + 1;
		DX::ThrowIfFailed(commandQueue->Signal(sharedFence.get(), workReadyValue));
		workFenceValue.store(workReadyValue, std::memory_order_release);  // proxy present waits on this before reading mvec/depth
		if (ok && method == 2 && frameToken) {
			workFrameTokenIndex.store(static_cast<uint32_t>(*frameToken), std::memory_order_release);
		}

		// --- D3D11: wait for D3D12, copy the result back into main color ---
		DX::ThrowIfFailed(d3d11Context4->Wait(d3d11Fence.get(), workReadyValue));
		if (ok) {
			if constexpr (Upscaling::kFrameGenExperiment) {
				if (kPresentOverride) {
					// Keep the upscaled scene on D3D12 as the final present color,
					// and clear kMAIN so the game draws UI-only onto black; the proxy
					// present composites the two (D3D12UIComposite).
					DX12SwapChain::GetSingleton()->SetPresentOverride(GetHudlessColor12());
					if (auto* rtv = Game::GetRenderTargetRTV(RE::RENDER_TARGET::kMAIN)) {
						const float black[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
						d3d11Context4->ClearRenderTargetView(rtv, black);
					}
				} else {
					d3d11Context4->CopyResource(kMain, colorOutput->resource11.get());
				}
			} else {
				d3d11Context4->CopyResource(kMain, colorOutput->resource11.get());
			}
			// Our full-res result now lives in kMAIN (or is set as the present
			// override); make the downstream passes treat the frame as full
			// resolution again (undo the DRS scale).
			if (upscaling) {
				ResetDynamicResolution();
			}
			// Frame generation (DLSS-G): configure per frame; the proxy present
			// tags our shared textures and inserts the generated frames.
			// (ConfigureFrameGeneration's own gate requires method==DLSS, so FSR
			// disables it here.)
			if constexpr (Upscaling::kFrameGenExperiment) {
				ConfigureFrameGeneration(renderSize.x, renderSize.y, displaySize.x, displaySize.y);
			}
		}

		// The uplift is only worth anything if it runs on EVERY frame -- if it runs
		// on some and not others the image alternates and reads as a flicker. Count
		// both and report the ratio periodically, so "is it actually on?" is a
		// question the log answers rather than the eye.
		if (neuralRendering) {
			++neuralFramesTotal;
			if (neuralRenderingActive) {
				++neuralFramesActive;
			}
			if ((neuralFramesTotal % 600) == 0) {
				logger::info("[DLSS-NR] Uplift ran on {} of the last {} frames (order={})",
					neuralFramesActive, neuralFramesTotal, nrAfterUpscale ? "after" : "before");
				neuralFramesTotal = 0;
				neuralFramesActive = 0;
			}
		}

		static uint32_t loggedMethod = 0xFFFFFFFF;
		static bool     loggedRR = false;
		const bool      rrActive = method == 2 && rayReconstruction && !rayReconstructionFailed;
		if (loggedMethod != method || loggedRR != rrActive) {
			loggedMethod = method;
			loggedRR = rrActive;
			logger::info("[D3D12Upscaler] {} evaluate: ok={} sharpened={} render={}x{} display={}x{} scale={} sharpness={}",
				method != 2 ? "FSR" : rrActive ? "DLSS-RR" : "DLSS",
				ok, sharpened, GetRenderWidth(), GetRenderHeight(), displayWidth, displayHeight, renderScale, sharpness);
		}
	} catch (const std::exception& e) {
		const auto removedReason = d3d12Device ? d3d12Device->GetDeviceRemovedReason() : S_OK;
		logger::critical(
			"[D3D12Upscaler] Evaluate failed; disabling D3D12 upscaling and requesting DLSS-G shutdown: {} removed=0x{:08X}",
			e.what(),
			static_cast<uint32_t>(removedReason));
		if (commandList) {
			std::ignore = commandList->Close();
		}
		ready = false;
		workFenceValue.store(0, std::memory_order_release);
		workFrameTokenIndex.store(std::numeric_limits<uint32_t>::max(), std::memory_order_release);
		sl->RequestDLSSGDisable();
	}
}
