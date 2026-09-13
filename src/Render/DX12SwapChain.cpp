#include "DX12SwapChain.h"

#include <algorithm>
#include <limits>

#include "D3D12UIComposite.h"
#include "FidelityFX.h"
#include "OSD.h"
#include "Streamline.h"
#include "Render/DeviceRemovedReport.h"
#include "third_party/RTX40MFGUnlock/integration.h"
#include "TaggedTextureDebug.h"
#include "Upscaling.h"

#include "Game/Util.h"  // Util::State_GetSingleton()->frameCount
#include "Upscaler/D3D12Upscaler.h"

extern bool enbLoaded;

namespace
{
	LRESULT CALLBACK DX12SwapChainWndProc(HWND a_hwnd, UINT a_msg, WPARAM a_wParam, LPARAM a_lParam)
	{
		auto* streamline = Streamline::GetSingleton();
		if (streamline->GetPCLStatsWindowMessage() != 0 && a_msg == streamline->GetPCLStatsWindowMessage()) {
			streamline->OnPCLStatsPing();
		}

		auto* swapChain = DX12SwapChain::GetSingleton();
		return swapChain->CallOriginalWndProc(a_hwnd, a_msg, a_wParam, a_lParam);
	}

	double QueryDesktopRefreshHz(IDXGISwapChain* a_swapChain)
	{
		if (!a_swapChain) {
			return 0.0;
		}

		winrt::com_ptr<IDXGIOutput> output;
		if (FAILED(a_swapChain->GetContainingOutput(output.put())) || !output) {
			return 0.0;
		}

		DXGI_OUTPUT_DESC outputDesc{};
		if (FAILED(output->GetDesc(&outputDesc))) {
			return 0.0;
		}

		DEVMODEW displayMode{};
		displayMode.dmSize = sizeof(displayMode);
		if (!EnumDisplaySettingsW(outputDesc.DeviceName, ENUM_CURRENT_SETTINGS, &displayMode) || displayMode.dmDisplayFrequency == 0) {
			return 0.0;
		}

		return static_cast<double>(displayMode.dmDisplayFrequency);
	}

	const char* HResultName(HRESULT a_result)
	{
		switch (a_result) {
		case S_OK:
			return "S_OK";
		case DXGI_ERROR_DEVICE_HUNG:
			return "DXGI_ERROR_DEVICE_HUNG";
		case DXGI_ERROR_DEVICE_REMOVED:
			return "DXGI_ERROR_DEVICE_REMOVED";
		case DXGI_ERROR_DEVICE_RESET:
			return "DXGI_ERROR_DEVICE_RESET";
		case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
			return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
		case DXGI_ERROR_INVALID_CALL:
			return "DXGI_ERROR_INVALID_CALL";
		case DXGI_ERROR_ACCESS_DENIED:
			return "DXGI_ERROR_ACCESS_DENIED";
		case DXGI_ERROR_WAS_STILL_DRAWING:
			return "DXGI_ERROR_WAS_STILL_DRAWING";
		default:
			return "UNKNOWN";
		}
	}

	bool LogStreamlineProxy(Streamline* a_streamline, const char* a_name, IUnknown* a_interface)
	{
		if (!a_streamline || !a_streamline->slGetNativeInterface || !a_interface) {
			return false;
		}

		void* nativeInterface = nullptr;
		if (SL_FAILED(result, a_streamline->slGetNativeInterface(a_interface, &nativeInterface))) {
			logger::warn("[DX12SwapChain] slGetNativeInterface({}) failed: {}", a_name, magic_enum::enum_name(result));
			return false;
		}

		const auto isProxy = nativeInterface && nativeInterface != a_interface;
		logger::info(
			"[DX12SwapChain] Streamline proxy check {} proxy={} interface={} native={}",
			a_name,
			isProxy,
			static_cast<void*>(a_interface),
			nativeInterface);

		if (nativeInterface) {
			static_cast<IUnknown*>(nativeInterface)->Release();
		}

		return isProxy;
	}

	DXGI_SWAP_CHAIN_DESC1 MakeSwapChainDescFromWindow(const DXGI_SWAP_CHAIN_DESC& a_swapChainDesc, BOOL a_allowTearing)
	{
		DXGI_SWAP_CHAIN_DESC1 desc{};
		desc.BufferCount = kDX12FrameCount;
		desc.Width = a_swapChainDesc.BufferDesc.Width;
		desc.Height = a_swapChainDesc.BufferDesc.Height;
		desc.Format = a_swapChainDesc.BufferDesc.Format;
		desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		desc.SampleDesc.Count = 1;

		RECT clientRect{};
		if (a_swapChainDesc.OutputWindow && GetClientRect(a_swapChainDesc.OutputWindow, &clientRect)) {
			const auto clientWidth = static_cast<UINT>(std::max<LONG>(0, clientRect.right - clientRect.left));
			const auto clientHeight = static_cast<UINT>(std::max<LONG>(0, clientRect.bottom - clientRect.top));
			if (clientWidth > 0 && clientHeight > 0) {
				desc.Width = clientWidth;
				desc.Height = clientHeight;
			}
		}

		if (a_allowTearing) {
			desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
		}

		return desc;
	}

}

D3D11D3D12SharedTexture::D3D11D3D12SharedTexture(const D3D11_TEXTURE2D_DESC& a_desc, ID3D11Device5* a_d3d11Device, ID3D12Device* a_d3d12Device)
{
	auto desc = a_desc;
	desc.MiscFlags |= D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
	DX::ThrowIfFailed(a_d3d11Device->CreateTexture2D(&desc, nullptr, resource11.put()));

	winrt::com_ptr<IDXGIResource1> dxgiResource;
	DX::ThrowIfFailed(resource11->QueryInterface(IID_PPV_ARGS(dxgiResource.put())));

	HANDLE rawSharedHandle = nullptr;
	DX::ThrowIfFailed(dxgiResource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &rawSharedHandle));
	winrt::handle sharedHandle;
	sharedHandle.attach(rawSharedHandle);
	DX::ThrowIfFailed(a_d3d12Device->OpenSharedHandle(sharedHandle.get(), IID_PPV_ARGS(resource12.put())));
}

DXGISwapChainProxy::DXGISwapChainProxy(IDXGISwapChain4* a_swapChain)
{
	swapChain.copy_from(a_swapChain);
}

void DXGISwapChainProxy::SetSwapChain(IDXGISwapChain4* a_swapChain)
{
	swapChain.copy_from(a_swapChain);
}

ULONG STDMETHODCALLTYPE DXGISwapChainProxy::AddRef()
{
	return ++refCount;
}

ULONG STDMETHODCALLTYPE DXGISwapChainProxy::Release()
{
	const auto refs = --refCount;
	if (refs == 0) {
		delete this;
	}
	return refs;
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::QueryInterface(REFIID riid, void** ppvObj)
{
	if (!ppvObj) {
		return E_POINTER;
	}

	*ppvObj = nullptr;
	if (riid == __uuidof(IUnknown) ||
		riid == __uuidof(IDXGIObject) ||
		riid == __uuidof(IDXGIDeviceSubObject) ||
		riid == __uuidof(IDXGISwapChain) ||
		riid == __uuidof(IDXGISwapChain1) ||
		riid == __uuidof(IDXGISwapChain2) ||
		riid == __uuidof(IDXGISwapChain3) ||
		riid == __uuidof(IDXGISwapChain4)) {
		*ppvObj = static_cast<IDXGISwapChain4*>(this);
		AddRef();
		return S_OK;
	}

	return swapChain->QueryInterface(riid, ppvObj);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) { return swapChain->SetPrivateData(Name, DataSize, pData); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) { return swapChain->SetPrivateDataInterface(Name, pUnknown); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) { return swapChain->GetPrivateData(Name, pDataSize, pData); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetParent(REFIID riid, void** ppParent) { return swapChain->GetParent(riid, ppParent); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetDevice(REFIID riid, void** ppDevice) { return DX12SwapChain::GetSingleton()->GetDevice(riid, ppDevice); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::Present(UINT SyncInterval, UINT Flags)
{
	try {
		return DX12SwapChain::GetSingleton()->Present(SyncInterval, Flags);
	} catch (const std::exception& e) {
		logger::critical("[DX12SwapChain] Present aborted before the DXGI boundary: {}", e.what());
		auto* device = DX12SwapChain::GetSingleton()->GetD3D12Device();
		const auto removedReason = device ? device->GetDeviceRemovedReason() : S_OK;
		return FAILED(removedReason) ? removedReason : DXGI_ERROR_DEVICE_RESET;
	} catch (...) {
		logger::critical("[DX12SwapChain] Present aborted by an unknown exception before the DXGI boundary");
		return DXGI_ERROR_DEVICE_RESET;
	}
}
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetBuffer(UINT Buffer, REFIID riid, void** ppSurface) { return DX12SwapChain::GetSingleton()->GetBuffer(Buffer, riid, ppSurface); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget) { return swapChain->SetFullscreenState(Fullscreen, pTarget); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget) { return swapChain->GetFullscreenState(pFullscreen, ppTarget); }

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc)
{
	if (!pDesc) {
		return E_POINTER;
	}

	DXGI_SWAP_CHAIN_DESC1 desc1{};
	DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreenDesc{};
	HWND hwnd = nullptr;
	DX::ThrowIfFailed(GetDesc1(&desc1));
	std::ignore = GetFullscreenDesc(&fullscreenDesc);
	std::ignore = GetHwnd(&hwnd);

	pDesc->BufferDesc.Width = desc1.Width;
	pDesc->BufferDesc.Height = desc1.Height;
	pDesc->BufferDesc.RefreshRate = fullscreenDesc.RefreshRate;
	pDesc->BufferDesc.Format = desc1.Format;
	pDesc->BufferDesc.ScanlineOrdering = fullscreenDesc.ScanlineOrdering;
	pDesc->BufferDesc.Scaling = fullscreenDesc.Scaling;
	pDesc->SampleDesc = desc1.SampleDesc;
	pDesc->BufferUsage = desc1.BufferUsage;
	pDesc->BufferCount = desc1.BufferCount;
	pDesc->OutputWindow = hwnd;
	pDesc->Windowed = fullscreenDesc.Windowed;
	pDesc->SwapEffect = desc1.SwapEffect;
	pDesc->Flags = desc1.Flags;
	return S_OK;
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
	try {
		return DX12SwapChain::GetSingleton()->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
	} catch (const std::exception& e) {
		logger::critical("[DX12SwapChain] ResizeBuffers aborted: {}", e.what());
		auto* device = DX12SwapChain::GetSingleton()->GetD3D12Device();
		const auto removedReason = device ? device->GetDeviceRemovedReason() : S_OK;
		return FAILED(removedReason) ? removedReason : DXGI_ERROR_DEVICE_RESET;
	} catch (...) {
		logger::critical("[DX12SwapChain] ResizeBuffers aborted by an unknown exception");
		return DXGI_ERROR_DEVICE_RESET;
	}
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters) { return swapChain->ResizeTarget(pNewTargetParameters); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetContainingOutput(IDXGIOutput** ppOutput) { return swapChain->GetContainingOutput(ppOutput); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats) { return swapChain->GetFrameStatistics(pStats); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetLastPresentCount(UINT* pLastPresentCount) { return swapChain->GetLastPresentCount(pLastPresentCount); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc) { return swapChain->GetDesc1(pDesc); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc) { return swapChain->GetFullscreenDesc(pDesc); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetHwnd(HWND* pHwnd) { return swapChain->GetHwnd(pHwnd); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetCoreWindow(REFIID refiid, void** ppUnk) { return swapChain->GetCoreWindow(refiid, ppUnk); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::Present1(UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
	try {
		return DX12SwapChain::GetSingleton()->Present(SyncInterval, PresentFlags, pPresentParameters);
	} catch (const std::exception& e) {
		logger::critical("[DX12SwapChain] Present1 aborted before the DXGI boundary: {}", e.what());
		auto* device = DX12SwapChain::GetSingleton()->GetD3D12Device();
		const auto removedReason = device ? device->GetDeviceRemovedReason() : S_OK;
		return FAILED(removedReason) ? removedReason : DXGI_ERROR_DEVICE_RESET;
	} catch (...) {
		logger::critical("[DX12SwapChain] Present1 aborted by an unknown exception before the DXGI boundary");
		return DXGI_ERROR_DEVICE_RESET;
	}
}
BOOL STDMETHODCALLTYPE DXGISwapChainProxy::IsTemporaryMonoSupported() { return swapChain->IsTemporaryMonoSupported(); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput) { return swapChain->GetRestrictToOutput(ppRestrictToOutput); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetBackgroundColor(const DXGI_RGBA* pColor) { return swapChain->SetBackgroundColor(pColor); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetBackgroundColor(DXGI_RGBA* pColor) { return swapChain->GetBackgroundColor(pColor); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetRotation(DXGI_MODE_ROTATION Rotation) { return swapChain->SetRotation(Rotation); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetRotation(DXGI_MODE_ROTATION* pRotation) { return swapChain->GetRotation(pRotation); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetSourceSize(UINT Width, UINT Height) { return swapChain->SetSourceSize(Width, Height); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetSourceSize(UINT* pWidth, UINT* pHeight) { return swapChain->GetSourceSize(pWidth, pHeight); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetMaximumFrameLatency(UINT MaxLatency) { return swapChain->SetMaximumFrameLatency(MaxLatency); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetMaximumFrameLatency(UINT* pMaxLatency) { return swapChain->GetMaximumFrameLatency(pMaxLatency); }
HANDLE STDMETHODCALLTYPE DXGISwapChainProxy::GetFrameLatencyWaitableObject() { return swapChain->GetFrameLatencyWaitableObject(); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix) { return swapChain->SetMatrixTransform(pMatrix); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix) { return swapChain->GetMatrixTransform(pMatrix); }
UINT STDMETHODCALLTYPE DXGISwapChainProxy::GetCurrentBackBufferIndex() { return swapChain->GetCurrentBackBufferIndex(); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace, UINT* pColorSpaceSupport) { return swapChain->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace) { return swapChain->SetColorSpace1(ColorSpace); }

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format, UINT SwapChainFlags, const UINT*, IUnknown* const*)
{
	// The proxy owns one fixed D3D12 presentation queue, so per-buffer queue
	// replacement from the D3D11 caller is not applicable. Preserve the resize
	// semantics through the same drained recreation path.
	return ResizeBuffers(BufferCount, Width, Height, Format, SwapChainFlags);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData) { return swapChain->SetHDRMetaData(Type, Size, pMetaData); }

void DX12SwapChain::CreateD3D12Device(IDXGIAdapter* a_adapter, Streamline* a_streamline)
{
	// Must precede device creation: DRED cannot be turned on retroactively, and a
	// removed device otherwise reports only a reason code with no operation.
	DeviceRemovedReport::EnableDebugLayerIfRequested();
	DeviceRemovedReport::Enable();
	DX::ThrowIfFailed(D3D12CreateDevice(a_adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(d3d12Device.put())));

	// The Ada multi-frame-generation unlock needs the adapter identity before it
	// can apply its midpoint correction, then another discovery pass.
	RTX40MFGUnlock::ObserveD3D12Device(d3d12Device.get());
	RTX40MFGUnlock::PatchLoadedModules();

	if (a_streamline && a_streamline->slSetD3DDevice) {
		if (SL_FAILED(result, a_streamline->slSetD3DDevice(d3d12Device.get()))) {
			logger::warn("[DX12SwapChain] slSetD3DDevice(D3D12) failed: {}", magic_enum::enum_name(result));
		}
	}

	ID3D12Device* deviceForQueue = d3d12Device.get();
	if (a_streamline && a_streamline->slUpgradeInterface) {
		if (SL_FAILED(result, a_streamline->slUpgradeInterface(reinterpret_cast<void**>(&deviceForQueue)))) {
			logger::warn("[DX12SwapChain] Could not upgrade D3D12 device for Streamline: {}", magic_enum::enum_name(result));
			deviceForQueue = d3d12Device.get();
		}
	}

	if (deviceForQueue == d3d12Device.get()) {
		proxyD3D12Device.copy_from(d3d12Device.get());
	} else {
		proxyD3D12Device.attach(deviceForQueue);
	}

	D3D12_COMMAND_QUEUE_DESC queueDesc{};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;

	DX::ThrowIfFailed(proxyD3D12Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(commandQueue.put())));
	logger::info("[DX12SwapChain] D3D12 command queue created via {} device: queue={}", proxyD3D12Device.get() == d3d12Device.get() ? "native" : "Streamline proxy", static_cast<void*>(commandQueue.get()));
	LogStreamlineProxy(a_streamline, "d3d12Device", d3d12Device.get());
	LogStreamlineProxy(a_streamline, "deviceForQueue", proxyD3D12Device.get());
	LogStreamlineProxy(a_streamline, "commandQueue", commandQueue.get());

	for (auto& context : commandContexts) {
		DX::ThrowIfFailed(d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(context.allocator.put())));
		DX::ThrowIfFailed(d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, context.allocator.get(), nullptr, IID_PPV_ARGS(context.list.put())));
		DX::ThrowIfFailed(context.list->Close());
	}
}

void DX12SwapChain::CreateSwapChain(IDXGIFactory5* a_dxgiFactory, const DXGI_SWAP_CHAIN_DESC& a_swapChainDesc, Streamline* a_streamline, bool a_useFidelityFXFrameGeneration)
{
	hwnd = a_swapChainDesc.OutputWindow;
	BOOL allowTearing = FALSE;
	std::ignore = a_dxgiFactory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing));

	swapChainDesc = MakeSwapChainDescFromWindow(a_swapChainDesc, allowTearing);

	IDXGIFactory5* factoryForSwapChain = a_dxgiFactory;
	winrt::com_ptr<IDXGIFactory5> upgradedFactory;
	if (a_streamline && a_streamline->slUpgradeInterface) {
		if (SL_FAILED(result, a_streamline->slUpgradeInterface(reinterpret_cast<void**>(&factoryForSwapChain)))) {
			logger::warn("[DX12SwapChain] Could not upgrade DXGI factory for Streamline: {}", magic_enum::enum_name(result));
			factoryForSwapChain = a_dxgiFactory;
		}
	}

	if (factoryForSwapChain == a_dxgiFactory) {
		upgradedFactory.copy_from(a_dxgiFactory);
	} else {
		upgradedFactory.attach(factoryForSwapChain);
	}

	fidelityFXFrameGenerationSwapChainAllowed = a_useFidelityFXFrameGeneration;
	if (fidelityFXFrameGenerationSwapChainAllowed) {
		logger::info("[DX12SwapChain] FidelityFX frame generation swapchain will be created during swapchain initialization");
	}

	if (!swapChain) {
		if (fidelityFXFrameGenerationSwapChainAllowed) {
			DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreenDesc{};
			fullscreenDesc.RefreshRate = a_swapChainDesc.BufferDesc.RefreshRate;
			fullscreenDesc.ScanlineOrdering = a_swapChainDesc.BufferDesc.ScanlineOrdering;
			fullscreenDesc.Scaling = a_swapChainDesc.BufferDesc.Scaling;
			fullscreenDesc.Windowed = a_swapChainDesc.Windowed;

			IDXGISwapChain4* fidelityFXSwapChain = nullptr;
			if (FidelityFX::GetSingleton()->CreateFrameGenerationSwapChainForHwnd(
					upgradedFactory.get(),
					a_swapChainDesc.OutputWindow,
					&swapChainDesc,
					&fullscreenDesc,
					commandQueue.get(),
					&fidelityFXSwapChain) &&
				fidelityFXSwapChain) {
				swapChain.attach(fidelityFXSwapChain);
				logger::info("[DX12SwapChain] FidelityFX frame generation swapchain created for hwnd: swapchain={}", static_cast<void*>(swapChain.get()));
			} else {
				if (fidelityFXSwapChain) {
					fidelityFXSwapChain->Release();
				}
				fidelityFXFrameGenerationSwapChainAllowed = false;
				logger::warn("[DX12SwapChain] FidelityFX frame generation swapchain creation failed; falling back to a regular D3D12 swapchain");
			}
		}

		if (!swapChain) {
			winrt::com_ptr<IDXGISwapChain1> swapChain1;
			DX::ThrowIfFailed(upgradedFactory->CreateSwapChainForHwnd(commandQueue.get(), a_swapChainDesc.OutputWindow, &swapChainDesc, nullptr, nullptr, swapChain1.put()));
			DX::ThrowIfFailed(swapChain1->QueryInterface(IID_PPV_ARGS(swapChain.put())));
		}
	}

	logger::info("[DX12SwapChain] Swapchain created via {} factory: swapchain={}", upgradedFactory.get() == a_dxgiFactory ? "native" : "Streamline proxy", static_cast<void*>(swapChain.get()));
	LogStreamlineProxy(a_streamline, "dxgiFactory", a_dxgiFactory);
	LogStreamlineProxy(a_streamline, "factoryForSwapChain", upgradedFactory.get());
	auto swapChainIsStreamlineProxy = LogStreamlineProxy(a_streamline, "swapChain", swapChain.get());
	if (!swapChainIsStreamlineProxy && a_streamline && a_streamline->slUpgradeInterface) {
		IDXGISwapChain* upgradedSwapChain = swapChain.get();
		if (SL_FAILED(result, a_streamline->slUpgradeInterface(reinterpret_cast<void**>(&upgradedSwapChain)))) {
			logger::warn("[DX12SwapChain] Could not upgrade swapchain for Streamline: {}", magic_enum::enum_name(result));
		} else if (upgradedSwapChain && upgradedSwapChain != swapChain.get()) {
			winrt::com_ptr<IDXGISwapChain> upgradedSwapChainOwner;
			upgradedSwapChainOwner.attach(upgradedSwapChain);
			winrt::com_ptr<IDXGISwapChain4> upgradedSwapChain4;
			DX::ThrowIfFailed(upgradedSwapChainOwner->QueryInterface(IID_PPV_ARGS(upgradedSwapChain4.put())));
			swapChain = upgradedSwapChain4;
			logger::info("[DX12SwapChain] Swapchain explicitly upgraded for Streamline: swapchain={}", static_cast<void*>(swapChain.get()));
			swapChainIsStreamlineProxy = LogStreamlineProxy(a_streamline, "swapChain.afterUpgrade", swapChain.get());
		}
	}
	if (!swapChainIsStreamlineProxy) {
		logger::warn("[DX12SwapChain] D3D12 swapchain is not a Streamline proxy; DLSS-G Present interception may not run on this swapchain");
	}
	RefreshBackBuffers();
	frameIndex = swapChain->GetCurrentBackBufferIndex();
	swapChainProxy = new DXGISwapChainProxy(swapChain.get());
	desktopRefreshHz = QueryDesktopRefreshHz(swapChain.get());
	if (desktopRefreshHz <= 0.0 &&
		a_swapChainDesc.BufferDesc.RefreshRate.Numerator != 0 &&
		a_swapChainDesc.BufferDesc.RefreshRate.Denominator != 0) {
		desktopRefreshHz =
			static_cast<double>(a_swapChainDesc.BufferDesc.RefreshRate.Numerator) /
			static_cast<double>(a_swapChainDesc.BufferDesc.RefreshRate.Denominator);
	}

	logger::info(
		"[DX12SwapChain] Created D3D12 Streamline swapchain {}x{} format={} buffers={} flags={} desktopRefreshHz={:.3f}",
		swapChainDesc.Width,
		swapChainDesc.Height,
		static_cast<uint32_t>(swapChainDesc.Format),
		swapChainDesc.BufferCount,
		swapChainDesc.Flags,
		desktopRefreshHz);

	// NOTE: the PCL-stats WndProc hook is intentionally NOT installed. It replaces
	// the window's WndProc via SetWindowLongPtrW, which corrupts the WndProc chain
	// that SKSE Menu Framework 3 also hooks -> access-violation CTD before the
	// first frame (verified via Crash Logger: jump to a bad address inside the
	// USER32 message dispatch with SMF's WndProcHook on the stack). PCL latency
	// stats are non-essential for the upscaler / frame generation, so we skip it.
	// InstallWndProcHook(hwnd);
}

void DX12SwapChain::InstallWndProcHook(HWND a_hwnd)
{
	if (!a_hwnd || originalWndProc) {
		return;
	}

	SetLastError(0);
	const auto previous = SetWindowLongPtrW(a_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(DX12SwapChainWndProc));
	if (previous == 0 && GetLastError() != 0) {
		logger::warn("[DX12SwapChain] Could not install PCL stats window proc hook: {}", GetLastError());
		return;
	}

	originalWndProc = reinterpret_cast<WNDPROC>(previous);
	logger::info("[DX12SwapChain] Installed PCL stats window proc hook");
}

LRESULT DX12SwapChain::CallOriginalWndProc(HWND a_hwnd, UINT a_msg, WPARAM a_wParam, LPARAM a_lParam) const
{
	if (originalWndProc) {
		return CallWindowProcW(originalWndProc, a_hwnd, a_msg, a_wParam, a_lParam);
	}
	return DefWindowProcW(a_hwnd, a_msg, a_wParam, a_lParam);
}

void DX12SwapChain::CreateInterop()
{
	HANDLE rawSharedFenceHandle = nullptr;
	DX::ThrowIfFailed(d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(d3d12Fence.put())));
	DX::ThrowIfFailed(d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(commandFence.put())));
	DX::ThrowIfFailed(d3d12Device->CreateSharedHandle(d3d12Fence.get(), nullptr, GENERIC_ALL, nullptr, &rawSharedFenceHandle));
	winrt::handle sharedFenceHandle;
	sharedFenceHandle.attach(rawSharedFenceHandle);
	DX::ThrowIfFailed(d3d11Device->OpenSharedFence(sharedFenceHandle.get(), IID_PPV_ARGS(d3d11Fence.put())));

	RecreateInteropTextures();
}

void DX12SwapChain::RecreateInteropTextures()
{
	D3D11_TEXTURE2D_DESC textureDesc{};
	textureDesc.Width = swapChainDesc.Width;
	textureDesc.Height = swapChainDesc.Height;
	textureDesc.MipLevels = 1;
	textureDesc.ArraySize = 1;
	textureDesc.Format = swapChainDesc.Format;
	textureDesc.SampleDesc.Count = 1;
	textureDesc.Usage = D3D11_USAGE_DEFAULT;
	textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

	if (enbLoaded) {
		swapChainBufferProxyENB = std::make_unique<D3D11D3D12SharedTexture>(textureDesc, d3d11Device.get(), d3d12Device.get());
		swapChainBufferProxy = nullptr;
	} else {
		winrt::com_ptr<ID3D11Texture2D> proxyTexture;
		DX::ThrowIfFailed(d3d11Device->CreateTexture2D(&textureDesc, nullptr, proxyTexture.put()));
		swapChainBufferProxy = std::make_unique<Texture2D>(proxyTexture.detach());
		swapChainBufferProxyENB = nullptr;
	}
	for (auto& context : commandContexts) {
		context.presentStaging = std::make_unique<D3D11D3D12SharedTexture>(textureDesc, d3d11Device.get(), d3d12Device.get());
	}
}

DX12SwapChain::CommandContext& DX12SwapChain::AcquireCommandContext()
{
	const auto completedValue = commandFence ? commandFence->GetCompletedValue() : 0;
	for (UINT i = 0; i < std::size(commandContexts); ++i) {
		const auto contextIndex = (nextCommandContext + i) % std::size(commandContexts);
		auto& context = commandContexts[contextIndex];
		if (context.fenceValue == 0 || completedValue >= context.fenceValue) {
			nextCommandContext = static_cast<UINT>((contextIndex + 1) % std::size(commandContexts));
			context.index = static_cast<UINT>(contextIndex);
			context.fenceValue = 0;
			DX::ThrowIfFailed(context.allocator->Reset());
			DX::ThrowIfFailed(context.list->Reset(context.allocator.get(), nullptr));
			return context;
		}
	}

	UINT waitContextIndex = nextCommandContext;
	auto waitValue = std::numeric_limits<UINT64>::max();
	for (UINT i = 0; i < std::size(commandContexts); ++i) {
		if (const auto value = commandContexts[i].fenceValue; value != 0 && value < waitValue) {
			waitContextIndex = i;
			waitValue = value;
		}
	}

	WaitForCommandFence(waitValue);
	nextCommandContext = static_cast<UINT>((waitContextIndex + 1) % std::size(commandContexts));
	auto& context = commandContexts[waitContextIndex];
	context.index = waitContextIndex;
	context.fenceValue = 0;
	DX::ThrowIfFailed(context.allocator->Reset());
	DX::ThrowIfFailed(context.list->Reset(context.allocator.get(), nullptr));
	return context;
}

void DX12SwapChain::TripDLSSGWatchdog(const char* a_reason, float a_presentMs, float a_frameMs)
{
	if (dlssgAutoDisabled.exchange(true, std::memory_order_acq_rel)) {
		return;  // already latched
	}

	logger::error(
		"[DX12SwapChain] DLSS-G WATCHDOG TRIPPED ({}) present={:.1f}ms frame={:.1f}ms -- frame generation is "
		"disabled for the rest of this session to avoid a GPU hang. Restart the game to try it again.",
		a_reason, a_presentMs, a_frameMs);

	// Ask Streamline to drop DLSS-G; the pending disable is applied at the top of
	// the next Present, which is the safe point for slDLSSGSetOptions.
	auto* streamline = Streamline::GetSingleton();
	streamline->RequestDLSSGDisable();
	Upscaling::GetSingleton()->frameGenerationActive = false;
}

bool DX12SwapChain::EnsureDLSSGInputBuffers()
{
	auto* up = D3D12Upscaler::GetSingleton();
	auto* hudlessSrc = up->GetHudlessColor12();
	auto* mvecSrc = up->GetMotionVectors12();
	auto* depthSrc = up->GetDepth12();
	if (!hudlessSrc || !mvecSrc || !depthSrc || !d3d12Device) {
		return false;
	}
	const auto hudlessDesc = hudlessSrc->GetDesc();
	const auto mvecDesc = mvecSrc->GetDesc();
	const auto depthDesc = depthSrc->GetDesc();
	const auto matches = [](ID3D12Resource* a_resource, const D3D12_RESOURCE_DESC& a_expected) {
		if (!a_resource) {
			return false;
		}
		const auto actual = a_resource->GetDesc();
		return actual.Dimension == a_expected.Dimension &&
			actual.Width == a_expected.Width &&
			actual.Height == a_expected.Height &&
			actual.DepthOrArraySize == a_expected.DepthOrArraySize &&
			actual.MipLevels == a_expected.MipLevels &&
			actual.Format == a_expected.Format &&
			actual.SampleDesc.Count == a_expected.SampleDesc.Count &&
			actual.SampleDesc.Quality == a_expected.SampleDesc.Quality;
	};
	bool completeAndCompatible = true;
	for (uint32_t i = 0; i < kDX12FrameCount; ++i) {
		completeAndCompatible = completeAndCompatible &&
			matches(dlssgHudless[i].get(), hudlessDesc) &&
			matches(dlssgMvec[i].get(), mvecDesc) &&
			matches(dlssgDepth[i].get(), depthDesc);
	}
	if (completeAndCompatible) {
		return true;
	}
	// Do not replace resources that may still be retained asynchronously by
	// DLSS-G. A size/format change requires a full, drained swapchain rebuild.
	if (dlssgHudless[0] || dlssgMvec[0] || dlssgDepth[0]) {
		logger::error("[DX12SwapChain] DLSS-G input descriptors changed or the buffer set is incomplete; frame generation disabled until swapchain recreation");
		return false;
	}

	const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
	auto makeTex = [&](winrt::com_ptr<ID3D12Resource>& a_out, DXGI_FORMAT a_fmt, uint64_t a_w, uint32_t a_h) {
		auto desc = CD3DX12_RESOURCE_DESC::Tex2D(a_fmt, a_w, a_h, 1, 1);
		return SUCCEEDED(d3d12Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(a_out.put())));
	};
	winrt::com_ptr<ID3D12Resource> newHudless[kDX12FrameCount];
	winrt::com_ptr<ID3D12Resource> newMvec[kDX12FrameCount];
	winrt::com_ptr<ID3D12Resource> newDepth[kDX12FrameCount];
	for (uint32_t i = 0; i < kDX12FrameCount; ++i) {
		if (!makeTex(newHudless[i], hudlessDesc.Format, hudlessDesc.Width, static_cast<uint32_t>(hudlessDesc.Height)) ||
			!makeTex(newMvec[i], mvecDesc.Format, mvecDesc.Width, mvecDesc.Height) ||
			!makeTex(newDepth[i], depthDesc.Format, depthDesc.Width, depthDesc.Height)) {
			logger::error("[DX12SwapChain] Failed to create DLSS-G input buffers");
			return false;
		}
	}
	for (uint32_t i = 0; i < kDX12FrameCount; ++i) {
		dlssgHudless[i] = std::move(newHudless[i]);
		dlssgMvec[i] = std::move(newMvec[i]);
		dlssgDepth[i] = std::move(newDepth[i]);
	}
	logger::info("[DX12SwapChain] DLSS-G per-index input buffers created ({} sets)", kDX12FrameCount);
	return true;
}

bool DX12SwapChain::PrepareAndTagDLSSGInputs(ID3D12GraphicsCommandList* a_commandList, UINT a_frameIndex, ID3D12Resource* a_hudlessSrc)
{
	auto* up = D3D12Upscaler::GetSingleton();
	auto  streamline = Streamline::GetSingleton();
	// hud-less = the D3D12-native DLSS output (present-override scene), NOT the
	// passed backbuffer -- DLSS-G interpolates the scene and recomposes UI from
	// (backbuffer - hud-less).
	std::ignore = a_hudlessSrc;
	auto* hudlessSrc = up->GetHudlessColor12();
	auto* mvecSrc = up->GetMotionVectors12();
	auto* depthSrc = up->GetDepth12();
	if (!hudlessSrc || !mvecSrc || !depthSrc || !EnsureDLSSGInputBuffers()) {
		streamline->RequestDLSSGDisable();
		streamline->ApplyPendingDLSSGDisable();
		streamline->ClearDLSSGResourceTags(a_commandList);
		return false;
	}

	const UINT idx = a_frameIndex % kDX12FrameCount;
	auto* hud = dlssgHudless[idx].get();
	auto* mv = dlssgMvec[idx].get();
	auto* dp = dlssgDepth[idx].get();
	// Copy each input into its per-index buffer on this (present) queue. Sources
	// are all in COMMON here (upscaler outputs + the staged backbuffer).
	const auto copyTex = [&](ID3D12Resource* a_src, ID3D12Resource* a_dst) {
		D3D12_RESOURCE_BARRIER pre[] = {
			CD3DX12_RESOURCE_BARRIER::Transition(a_src, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(a_dst, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST)
		};
		a_commandList->ResourceBarrier(_countof(pre), pre);
		a_commandList->CopyResource(a_dst, a_src);
		D3D12_RESOURCE_BARRIER post[] = {
			CD3DX12_RESOURCE_BARRIER::Transition(a_src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
			CD3DX12_RESOURCE_BARRIER::Transition(a_dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON)
		};
		a_commandList->ResourceBarrier(_countof(post), post);
	};
	copyTex(hudlessSrc, hud);
	copyTex(mvecSrc, mv);
	copyTex(depthSrc, dp);

	const float2 renderSize{ static_cast<float>(up->GetRenderWidth()), static_cast<float>(up->GetRenderHeight()) };
	const float2 displaySize{ static_cast<float>(up->GetDisplayWidth()), static_cast<float>(up->GetDisplayHeight()) };
	const uint32_t frameTokenIndex = up->GetWorkFrameTokenIndex();
	if (frameTokenIndex == std::numeric_limits<uint32_t>::max()) {
		logger::warn("[DX12SwapChain] DLSS-G inputs skipped: no token from the matching DLSS evaluation");
		streamline->RequestDLSSGDisable();
		streamline->ApplyPendingDLSSGDisable();
		streamline->ClearDLSSGResourceTags(a_commandList);
		return false;
	}
	if (!streamline->TagDLSSGResources(hud, mv, dp, nullptr, a_commandList, frameTokenIndex, renderSize, displaySize)) {
		logger::error("[DX12SwapChain] DLSS-G input tagging failed; disabling frame generation");
		streamline->RequestDLSSGDisable();
		streamline->ApplyPendingDLSSGDisable();
		streamline->ClearDLSSGResourceTags(a_commandList);
		return false;
	}
	// Keep the Reflex PresentStart/PresentEnd markers on the exact token used by
	// common constants, DLSS-SR evaluation, and the DLSS-G resource tags.
	streamline->SetPresentFrameIndex(frameTokenIndex);
	return true;
}

void DX12SwapChain::ExecuteCommandContext(CommandContext& a_context)
{
	DX::ThrowIfFailed(a_context.list->Close());
	ID3D12CommandList* lists[] = { a_context.list.get() };
	commandQueue->ExecuteCommandLists(static_cast<UINT>(std::size(lists)), lists);

	const auto signalValue = commandFenceValue++;
	DX::ThrowIfFailed(commandQueue->Signal(commandFence.get(), signalValue));
	a_context.fenceValue = signalValue;
}

void DX12SwapChain::WaitForCommandFence(UINT64 a_value)
{
	if (!commandFence || a_value == 0 || commandFence->GetCompletedValue() >= a_value) {
		return;
	}

	if (!commandFenceEvent) {
		commandFenceEvent.attach(CreateEventW(nullptr, FALSE, FALSE, nullptr));
		if (!commandFenceEvent) {
			DX::ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
		}
	}

	DX::ThrowIfFailed(commandFence->SetEventOnCompletion(a_value, commandFenceEvent.get()));
	constexpr DWORD kGpuWaitTimeoutMs = 5000;
	const auto waitResult = WaitForSingleObjectEx(commandFenceEvent.get(), kGpuWaitTimeoutMs, FALSE);
	if (waitResult != WAIT_OBJECT_0) {
		const auto removedReason = d3d12Device ? d3d12Device->GetDeviceRemovedReason() : S_OK;
		logger::critical(
			"[DX12SwapChain] GPU command fence wait failed/timed out result={} completed={} expected={} removed=0x{:08X}({})",
			waitResult,
			commandFence->GetCompletedValue(),
			a_value,
			static_cast<uint32_t>(removedReason),
			HResultName(removedReason));
		DX::ThrowIfFailed(waitResult == WAIT_FAILED ? HRESULT_FROM_WIN32(GetLastError()) : HRESULT_FROM_WIN32(ERROR_TIMEOUT));
	}
}

void DX12SwapChain::WaitForIdleForResize()
{
	if (!commandQueue || !commandFence || !d3d11Context || !d3d11Fence || !d3d12Fence) {
		DX::ThrowIfFailed(E_UNEXPECTED);
	}

	// Retired DLSS-G slots may still be consumed by Streamline after Present.
	// Queue all known completion waits before the final drain signal.
	for (uint32_t i = 0; i < kDX12FrameCount; ++i) {
		if (dlssgCompletionFences[i] && dlssgCompletionValues[i] != 0) {
			DX::ThrowIfFailed(commandQueue->Wait(dlssgCompletionFences[i].get(), dlssgCompletionValues[i]));
		}
	}

	// Drain D3D11 accesses to the proxy texture into the D3D12 presentation
	// queue, then wait on a CPU-visible fence for all prior queue work.
	const auto d3d11IdleValue = fenceValue++;
	DX::ThrowIfFailed(d3d11Context->Signal(d3d11Fence.get(), d3d11IdleValue));
	DX::ThrowIfFailed(commandQueue->Wait(d3d12Fence.get(), d3d11IdleValue));
	const auto queueIdleValue = commandFenceValue++;
	DX::ThrowIfFailed(commandQueue->Signal(commandFence.get(), queueIdleValue));
	WaitForCommandFence(queueIdleValue);

	for (auto& context : commandContexts) {
		context.fenceValue = 0;
	}
}

HRESULT DX12SwapChain::ResizeBuffers(UINT a_bufferCount, UINT a_width, UINT a_height, DXGI_FORMAT a_newFormat, UINT a_swapChainFlags)
{
	if (!IsReady()) {
		return DXGI_ERROR_INVALID_CALL;
	}
	const auto effectiveBufferCount = a_bufferCount == 0 ? swapChainDesc.BufferCount : a_bufferCount;
	if (effectiveBufferCount != kDX12FrameCount) {
		logger::error("[DX12SwapChain] ResizeBuffers rejected unsupported buffer count {} (required {})", effectiveBufferCount, kDX12FrameCount);
		return DXGI_ERROR_INVALID_CALL;
	}
	if (const auto removedReason = d3d12Device->GetDeviceRemovedReason(); FAILED(removedReason)) {
		DeviceRemovedReport::Report(d3d12Device.get(), "DX12SwapChain::Present");
		return removedReason;
	}

	auto* streamline = Streamline::GetSingleton();
	streamline->RequestDLSSGDisable();
	streamline->ApplyPendingDLSSGDisable();
	WaitForIdleForResize();

	presentOverrideFinalColor = nullptr;
	for (auto& backBuffer : swapChainBuffers) {
		backBuffer = nullptr;
	}

	const auto result = swapChain->ResizeBuffers(a_bufferCount, a_width, a_height, a_newFormat, a_swapChainFlags);
	if (FAILED(result)) {
		logger::error("[DX12SwapChain] Underlying ResizeBuffers failed: 0x{:08X}({})", static_cast<uint32_t>(result), HResultName(result));
		// DXGI leaves the old buffers intact on failure; reacquire them so the
		// proxy remains usable if the caller handles the error and continues.
		RefreshBackBuffers();
		frameIndex = swapChain->GetCurrentBackBufferIndex();
		return result;
	}

	DX::ThrowIfFailed(swapChain->GetDesc1(&swapChainDesc));
	if (swapChainDesc.BufferCount != kDX12FrameCount || swapChainDesc.Width == 0 || swapChainDesc.Height == 0) {
		logger::critical(
			"[DX12SwapChain] Resize produced an unsupported descriptor {}x{} buffers={}",
			swapChainDesc.Width,
			swapChainDesc.Height,
			swapChainDesc.BufferCount);
		return DXGI_ERROR_INVALID_CALL;
	}

	for (uint32_t i = 0; i < kDX12FrameCount; ++i) {
		dlssgHudless[i] = nullptr;
		dlssgMvec[i] = nullptr;
		dlssgDepth[i] = nullptr;
		dlssgCompletionFences[i] = nullptr;
		dlssgCompletionValues[i] = 0;
	}
	RefreshBackBuffers();
	RecreateInteropTextures();
	frameIndex = swapChain->GetCurrentBackBufferIndex();
	nextCommandContext = 0;
	OSD::GetSingleton()->Reset();
	logger::info(
		"[DX12SwapChain] ResizeBuffers completed {}x{} format={} buffers={} flags=0x{:X}",
		swapChainDesc.Width,
		swapChainDesc.Height,
		static_cast<uint32_t>(swapChainDesc.Format),
		swapChainDesc.BufferCount,
		swapChainDesc.Flags);
	return S_OK;
}

void DX12SwapChain::SetD3D11Device(ID3D11Device* a_d3d11Device)
{
	DX::ThrowIfFailed(a_d3d11Device->QueryInterface(IID_PPV_ARGS(d3d11Device.put())));
}

void DX12SwapChain::SetD3D11DeviceContext(ID3D11DeviceContext* a_d3d11Context)
{
	DX::ThrowIfFailed(a_d3d11Context->QueryInterface(IID_PPV_ARGS(d3d11Context.put())));
}

HRESULT DX12SwapChain::GetBuffer(UINT a_buffer, REFIID a_riid, void** a_surface)
{
	static bool logged = false;
	if (!logged) {
		logged = true;
		logger::info("[DX12SwapChain] GetBuffer first call (enbPath={})", swapChainBufferProxyENB != nullptr);
	}
	if (!a_surface) {
		return E_POINTER;
	}
	*a_surface = nullptr;
	if (a_buffer != 0) {
		return DXGI_ERROR_INVALID_CALL;
	}

	if (swapChainBufferProxyENB) {
		return swapChainBufferProxyENB->resource11->QueryInterface(a_riid, a_surface);
	}

	if (!swapChainBufferProxy) {
		return E_POINTER;
	}

	return swapChainBufferProxy->resource->QueryInterface(a_riid, a_surface);
}

HRESULT DX12SwapChain::Present(UINT SyncInterval, UINT Flags, const DXGI_PRESENT_PARAMETERS* a_presentParameters)
{
	static bool logged = false;
	if (!logged) {
		logged = true;
		logger::info("[DX12SwapChain] Present first call sync={} flags=0x{:X}", SyncInterval, Flags);
	}
	if (!IsReady()) {
		return DXGI_ERROR_INVALID_CALL;
	}
	if (const auto removedReason = d3d12Device->GetDeviceRemovedReason(); FAILED(removedReason)) {
		// Every subsequent present hits this too; the last run logged it 374
		// times, which buries the frames that led up to the loss. Say it once,
		// with whatever DRED can still recover, and then stay quiet.
		static bool reportedRemoval = false;
		if (!reportedRemoval) {
			reportedRemoval = true;
			logger::critical("[DX12SwapChain] Present rejected because the D3D12 device is removed: 0x{:08X}({}); further presents will fail silently", static_cast<uint32_t>(removedReason), HResultName(removedReason));
			DeviceRemovedReport::Report(d3d12Device.get(), "DX12SwapChain::Present (device removed)");
		}
		return removedReason;
	}

	auto streamline = Streamline::GetSingleton();
	auto upscaling = Upscaling::GetSingleton();
	const auto osdEnabled = upscaling->settings.osdMode != 0;
	if (streamline->HasPendingDLSSGDisable()) {
		streamline->ApplyPendingDLSSGDisable();
	}
	const auto dlssgPresentSafety = streamline->NeedsDLSSGPresentSafety();

	auto& commandContext = AcquireCommandContext();
	auto* commandList = commandContext.list.get();
	auto* presentStaging = commandContext.presentStaging.get();
	if (!presentStaging) {
		return DXGI_ERROR_INVALID_CALL;
	}

	if (swapChainBufferProxyENB) {
		d3d11Context->CopyResource(presentStaging->resource11.get(), swapChainBufferProxyENB->resource11.get());
	} else {
		d3d11Context->CopyResource(presentStaging->resource11.get(), swapChainBufferProxy->resource.get());
	}
	DX::ThrowIfFailed(d3d11Context->Signal(d3d11Fence.get(), fenceValue));
	DX::ThrowIfFailed(commandQueue->Wait(d3d12Fence.get(), fenceValue));
	++fenceValue;

	auto destination = swapChainBuffers[frameIndex].get();
	auto copySource = presentStaging->resource12.get();
	auto overrideFinalColor = presentOverrideFinalColor.get();
	const bool usePresentOverride = overrideFinalColor != nullptr;
	presentOverrideFinalColor = nullptr;

	// The other end of the trace in D3D12Upscaler: this says what present really
	// received, so a mismatch between the two lines localises where the chain
	// breaks. Logged only when it changes.
	if (overrideFinalColor != loggedPresentOverrideSeen) {
		loggedPresentOverrideSeen = overrideFinalColor;
		logger::info("[DX12SwapChain] Present override received: {} (compositing {})",
			static_cast<const void*>(overrideFinalColor),
			usePresentOverride ? "override + UI staging" : "staging only");
	}

	D3D12_RESOURCE_BARRIER beforeCopy[] = {
		CD3DX12_RESOURCE_BARRIER::Transition(
			copySource,
			D3D12_RESOURCE_STATE_COMMON,
			usePresentOverride ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COPY_SOURCE),
		CD3DX12_RESOURCE_BARRIER::Transition(destination, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST)
	};
	commandList->ResourceBarrier(static_cast<UINT>(std::size(beforeCopy)), beforeCopy);
	D3D12_RESOURCE_STATES destinationState = D3D12_RESOURCE_STATE_COPY_DEST;
	if (usePresentOverride) {
		D3D12_RESOURCE_BARRIER beforeComposite[] = {
			CD3DX12_RESOURCE_BARRIER::Transition(destination, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET),
			CD3DX12_RESOURCE_BARRIER::Transition(overrideFinalColor, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
		};
		commandList->ResourceBarrier(static_cast<UINT>(std::size(beforeComposite)), beforeComposite);
		D3D12UIComposite::GetSingleton()->Render(
			d3d12Device.get(),
			commandList,
			destination,
			overrideFinalColor,
			copySource,
			swapChainDesc.Format,
			swapChainDesc.Width,
			swapChainDesc.Height,
			commandContext.index,
			static_cast<uint32_t>(std::size(commandContexts)));
		auto afterComposite = CD3DX12_RESOURCE_BARRIER::Transition(overrideFinalColor, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
		commandList->ResourceBarrier(1, &afterComposite);
		destinationState = D3D12_RESOURCE_STATE_RENDER_TARGET;
	} else {
		commandList->CopyResource(destination, copySource);
	}
	auto afterSourceCopy = CD3DX12_RESOURCE_BARRIER::Transition(
		copySource,
		usePresentOverride ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COPY_SOURCE,
		D3D12_RESOURCE_STATE_COMMON);
	commandList->ResourceBarrier(1, &afterSourceCopy);

	bool dlssgInputsSubmitted = false;
	if (upscaling->IsFrameGenerationActive()) {
		const auto inputSlot = frameIndex % kDX12FrameCount;
		if (dlssgCompletionFences[inputSlot] && dlssgCompletionValues[inputSlot] != 0) {
			// Streamline explicitly requires tagged inputs to remain unchanged until
			// this completion point. This wait is queued before the slot copy below.
			DX::ThrowIfFailed(commandQueue->Wait(dlssgCompletionFences[inputSlot].get(), dlssgCompletionValues[inputSlot]));
			dlssgCompletionFences[inputSlot] = nullptr;
			dlssgCompletionValues[inputSlot] = 0;
		}
		// hudless = the final composited image we just staged (copySource), so
		// DLSS-G interpolates the ENB+UI frame the player actually sees. Copy it +
		// the upscaler's motion/depth into per-index buffers on THIS queue, then
		// tag those (fo4test-style double-buffering -- no cross-queue race).
		dlssgInputsSubmitted = PrepareAndTagDLSSGInputs(commandList, frameIndex, copySource);
	} else if (dlssgPresentSafety) {
		streamline->ClearDLSSGResourceTags(commandList);
	}
	auto fidelityFX = FidelityFX::GetSingleton();
	if (fidelityFX->IsFrameGenerationEnabled() &&
		!upscaling->IsFSRFrameGenerationActive()) {
		const auto desc = destination->GetDesc();
		const auto displaySize = float2(static_cast<float>(desc.Width), static_cast<float>(desc.Height));
		fidelityFX->DisableFrameGeneration(
			d3d12Device.get(),
			commandList,
			swapChain.get(),
			displaySize,
			desc.Format);
	}

	const auto taggedTextureDebug = upscaling->settings.taggedTextureDebug != 0;
	static bool wasOSDEnabled = false;
	if (!osdEnabled && wasOSDEnabled) {
		OSD::GetSingleton()->Reset();
	}
	wasOSDEnabled = osdEnabled;
	if (osdEnabled || taggedTextureDebug) {
		if (destinationState != D3D12_RESOURCE_STATE_RENDER_TARGET) {
			auto beforeOSD = CD3DX12_RESOURCE_BARRIER::Transition(destination, destinationState, D3D12_RESOURCE_STATE_RENDER_TARGET);
			commandList->ResourceBarrier(1, &beforeOSD);
			destinationState = D3D12_RESOURCE_STATE_RENDER_TARGET;
		}
		if (taggedTextureDebug) {
			ID3D12Resource* color = nullptr;
			ID3D12Resource* depth = nullptr;
			ID3D12Resource* motionVectors = nullptr;
			upscaling->GetTaggedTextureDebugResources(frameIndex, color, depth, motionVectors);
			if (color && depth && motionVectors) {
				D3D12_RESOURCE_BARRIER beforeDebug[] = {
					CD3DX12_RESOURCE_BARRIER::Transition(color, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(motionVectors, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(copySource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
				};
				commandList->ResourceBarrier(static_cast<UINT>(std::size(beforeDebug)), beforeDebug);
				TaggedTextureDebug::GetSingleton()->Render(
					d3d12Device.get(),
					commandList,
					destination,
					color,
					depth,
					motionVectors,
					copySource,
					swapChainDesc.Format,
					swapChainDesc.Width,
					swapChainDesc.Height);
				D3D12_RESOURCE_BARRIER afterDebug[] = {
					CD3DX12_RESOURCE_BARRIER::Transition(color, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
					CD3DX12_RESOURCE_BARRIER::Transition(depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
					CD3DX12_RESOURCE_BARRIER::Transition(motionVectors, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
					CD3DX12_RESOURCE_BARRIER::Transition(copySource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON)
				};
				commandList->ResourceBarrier(static_cast<UINT>(std::size(afterDebug)), afterDebug);
			}
		}
		if (osdEnabled) {
			OSD::GetSingleton()->Render(
				d3d12Device.get(),
				commandList,
				destination,
				frameIndex,
				swapChainDesc.Format,
				swapChainDesc.Width,
				swapChainDesc.Height);
		}
		auto afterOSD = CD3DX12_RESOURCE_BARRIER::Transition(destination, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
		commandList->ResourceBarrier(1, &afterOSD);
	} else {
		auto afterCopy = CD3DX12_RESOURCE_BARRIER::Transition(destination, destinationState, D3D12_RESOURCE_STATE_PRESENT);
		commandList->ResourceBarrier(1, &afterCopy);
	}

	// Cross-queue sync: the present-override composite + the DLSS-G input copies
	// read the upscaler's D3D12 outputs (color/motion/depth), produced on the
	// upscaler's own queue. Make this (present) queue wait for that work first.
	if (upscaling->IsFrameGenerationActive() || usePresentOverride) {
		auto* up = D3D12Upscaler::GetSingleton();
		if (auto* workFence = up->GetWorkFence()) {
			DX::ThrowIfFailed(commandQueue->Wait(workFence, up->GetWorkFenceValue()));
		}
	}

	ExecuteCommandContext(commandContext);
	if (dlssgInputsSubmitted) {
		DX::ThrowIfFailed(D3D12Upscaler::GetSingleton()->SignalPresentInputsConsumed(commandQueue.get()));
	}

	const auto fidelityFXFrameGenerationActive = upscaling->IsFSRFrameGenerationActive();
	const auto presentSyncInterval = (dlssgPresentSafety || fidelityFXFrameGenerationActive) ? 0u : SyncInterval;
	const auto presentFlags = dlssgPresentSafety ? (Flags & ~DXGI_PRESENT_ALLOW_TEARING) : Flags;
	const auto emitPresentMarkers = streamline->NeedsPresentMarkers();
	if (emitPresentMarkers) {
		streamline->OnPresentStart();
	}
	const auto presentStart = std::chrono::steady_clock::now();
	const auto result = a_presentParameters ?
		swapChain->Present1(presentSyncInterval, presentFlags, a_presentParameters) :
		swapChain->Present(presentSyncInterval, presentFlags);
	const auto presentEnd = std::chrono::steady_clock::now();
	if (emitPresentMarkers) {
		streamline->OnPresentEnd(result, false);
	}

	// --- DLSS-G safety watchdog -------------------------------------------
	// Bail out of frame generation at the first sign of the pre-TDR signature
	// (a present or frame interval blowing past the budget) instead of letting
	// the driver wedge. Two consecutive slow frames trip it, so a one-off hitch
	// (shader compile, cell load) does not.
	{
		using ms = std::chrono::duration<float, std::milli>;
		const float presentMs = ms(presentEnd - presentStart).count();
		const float frameMs = lastPresentEnd.time_since_epoch().count() != 0 ?
			ms(presentEnd - lastPresentEnd).count() : 0.0f;
		if (dlssgPresentSafety && !dlssgAutoDisabled.load(std::memory_order_acquire)) {
			constexpr float kStallMs = 80.0f;
			// A long gap between two presents is NOT by itself a GPU problem: a
			// menu, a loading screen, an alt-tab or a CPU hitch all produce one
			// while Present itself returns promptly. Judging on the gap cost us a
			// false trip (present=1.0ms, frame=6861.7ms) that disabled frame
			// generation for a whole session. The pre-TDR signature is Present
			// *blocking*, so only that arms the watchdog; an outright failed
			// present is handled separately below.
			constexpr float kPauseMs = 1000.0f;
			if (frameMs > kPauseMs) {
				// Resuming from a pause -- the next interval is the first one that
				// says anything about the GPU.
				dlssgSlowPresentStreak = 0;
			} else if (presentMs > kStallMs) {
				if (++dlssgSlowPresentStreak >= 2) {
					TripDLSSGWatchdog("present stall", presentMs, frameMs);
				}
			} else {
				dlssgSlowPresentStreak = 0;
			}
		}
		lastPresentEnd = presentEnd;
	}
	if (FAILED(result) && !dlssgAutoDisabled.load(std::memory_order_acquire)) {
		// Device removed/hung: never retry frame generation this session.
		TripDLSSGWatchdog("present failed", 0.0f, 0.0f);
	}
	if (dlssgPresentSafety && SUCCEEDED(result)) {
		streamline->OnDLSSGPresentComplete();
	}
	if (FAILED(result)) {
		const auto d3d12RemovedReason = d3d12Device ? d3d12Device->GetDeviceRemovedReason() : S_OK;
		HRESULT d3d11RemovedReason = S_OK;
		if (d3d11Device) {
			winrt::com_ptr<ID3D11Device> d3d11Base;
			if (SUCCEEDED(d3d11Device->QueryInterface(IID_PPV_ARGS(d3d11Base.put())))) {
				d3d11RemovedReason = d3d11Base->GetDeviceRemovedReason();
			}
		}

		logger::error(
			"[DX12SwapChain] Present failed result=0x{:08X}({}) d3d12Removed=0x{:08X}({}) d3d11Removed=0x{:08X}({}) sync={} flags=0x{:X} frameIndex={} queue={} swapchain={} thread={}",
			static_cast<uint32_t>(result),
			HResultName(result),
			static_cast<uint32_t>(d3d12RemovedReason),
			HResultName(d3d12RemovedReason),
			static_cast<uint32_t>(d3d11RemovedReason),
			HResultName(d3d11RemovedReason),
			presentSyncInterval,
			presentFlags,
			frameIndex,
			static_cast<void*>(commandQueue.get()),
			static_cast<void*>(swapChain.get()),
			GetCurrentThreadId());
	}
	if (FAILED(result)) {
		return result;
	}

	const auto nextFrameIndex = swapChain->GetCurrentBackBufferIndex();

	if (dlssgPresentSafety || upscaling->IsFrameGenerationActive()) {
		streamline->QueryDLSSGState("post-wait");
		if (auto* completionFence = streamline->GetDLSSGInputsProcessingCompletionFence()) {
			const auto completionValue = streamline->GetDLSSGInputsProcessingCompletionFenceValue();
			if (completionValue != 0) {
				const auto inputSlot = frameIndex % kDX12FrameCount;
				dlssgCompletionFences[inputSlot].copy_from(completionFence);
				dlssgCompletionValues[inputSlot] = completionValue;
			}
		}
	}

	static bool loggedPresentAdjustment = false;
	if (dlssgPresentSafety && !loggedPresentAdjustment && (presentSyncInterval != SyncInterval || presentFlags != Flags)) {
		logger::info(
			"[DX12SwapChain] DLSS-G adjusted present sync {}->{} flags 0x{:X}->0x{:X}",
			SyncInterval,
			presentSyncInterval,
			Flags,
			presentFlags);
		loggedPresentAdjustment = true;
	}

	frameIndex = nextFrameIndex;
	return S_OK;
}

DX12SwapChain::D3D12EvaluationResult DX12SwapChain::EvaluateD3D12WorkForCurrentFrame(bool a_evaluateDLSS, bool a_evaluateFSR, bool a_evaluateFSRFrameGeneration, bool a_waitForD3D11Consumption)
{
	D3D12EvaluationResult result{};
	if (!IsReady()) {
		return result;
	}

	if (a_evaluateFSRFrameGeneration && !EnsureFidelityFXFrameGenerationSwapChain()) {
		a_evaluateFSRFrameGeneration = false;
	}

	if (!a_evaluateDLSS && !a_evaluateFSR && !a_evaluateFSRFrameGeneration) {
		return result;
	}

	const auto evaluationFrameIndex = frameIndex;
	DX::ThrowIfFailed(d3d11Context->Signal(d3d11Fence.get(), fenceValue));
	DX::ThrowIfFailed(commandQueue->Wait(d3d12Fence.get(), fenceValue));
	++fenceValue;

	auto& commandContext = AcquireCommandContext();
	auto* commandList = commandContext.list.get();

	result = EvaluateD3D12WorkOnCommandList(commandList, evaluationFrameIndex, a_evaluateDLSS, a_evaluateFSR, a_evaluateFSRFrameGeneration);

	DX::ThrowIfFailed(commandList->Close());

	if (!result.Any()) {
		return result;
	}

	ID3D12CommandList* lists[] = { commandList };
	commandQueue->ExecuteCommandLists(static_cast<UINT>(std::size(lists)), lists);
	const auto signalValue = commandFenceValue++;
	DX::ThrowIfFailed(commandQueue->Signal(commandFence.get(), signalValue));
	commandContext.fenceValue = signalValue;

	if (a_waitForD3D11Consumption) {
		DX::ThrowIfFailed(commandQueue->Signal(d3d12Fence.get(), fenceValue));
		DX::ThrowIfFailed(d3d11Context->Wait(d3d11Fence.get(), fenceValue));
		++fenceValue;
	}
	return result;
}

DX12SwapChain::D3D12EvaluationResult DX12SwapChain::EvaluateD3D12WorkOnCommandList(ID3D12GraphicsCommandList* a_commandList, UINT a_frameIndex, bool a_evaluateDLSS, bool a_evaluateFSR, bool a_evaluateFSRFrameGeneration)
{
	D3D12EvaluationResult result{};
	if (!a_commandList) {
		return result;
	}

	auto* upscaling = Upscaling::GetSingleton();
	if (a_evaluateFSR) {
		result.fsr = upscaling->EvaluateD3D12FSR(a_commandList, a_frameIndex);
	}
	if (a_evaluateDLSS) {
		result.dlss = upscaling->EvaluateD3D12DLSS(a_commandList, a_frameIndex);
	}

	const bool upscalerRequested = a_evaluateFSR || a_evaluateDLSS;
	const bool upscalerSucceeded = (!a_evaluateFSR || result.fsr) && (!a_evaluateDLSS || result.dlss);
	if (a_evaluateFSRFrameGeneration && (!upscalerRequested || upscalerSucceeded)) {
		result.fsrFrameGeneration = upscaling->EvaluateFSRFrameGeneration(a_commandList, a_frameIndex);
	}

	return result;
}

bool DX12SwapChain::EvaluateD3D12DLSSForCurrentFrame()
{
	return EvaluateD3D12WorkForCurrentFrame(true, false, false).dlss;
}

bool DX12SwapChain::EvaluateD3D12FSRForCurrentFrame()
{
	return EvaluateD3D12WorkForCurrentFrame(false, true, false).fsr;
}

bool DX12SwapChain::EvaluateFSRFrameGenerationForCurrentFrame()
{
	return EvaluateD3D12WorkForCurrentFrame(false, false, true).fsrFrameGeneration;
}

void DX12SwapChain::SetPresentOverride(ID3D12Resource* a_finalColor)
{
	presentOverrideFinalColor.copy_from(a_finalColor);
}

bool DX12SwapChain::EnsureFidelityFXFrameGenerationSwapChain()
{
	if (!IsReady() || !fidelityFXFrameGenerationSwapChainAllowed) {
		return false;
	}

	auto* fidelityFX = FidelityFX::GetSingleton();
	if (fidelityFX->IsFrameGenerationSwapChainActive()) {
		return true;
	}

	auto* originalSwapChain = swapChain.get();
	if (!originalSwapChain) {
		return false;
	}

	originalSwapChain->AddRef();
	IDXGISwapChain4* wrappedSwapChain = originalSwapChain;
	const auto created = fidelityFX->CreateFrameGenerationSwapChain(&wrappedSwapChain, commandQueue.get());
	if (created && wrappedSwapChain) {
		if (wrappedSwapChain != originalSwapChain) {
			winrt::com_ptr<IDXGISwapChain4> wrappedOwner;
			wrappedOwner.attach(wrappedSwapChain);
			swapChain = wrappedOwner;
			if (swapChainProxy) {
				swapChainProxy->SetSwapChain(swapChain.get());
			}
			RefreshBackBuffers();
			frameIndex = swapChain->GetCurrentBackBufferIndex();
		}

		originalSwapChain->Release();
		logger::info("[DX12SwapChain] FidelityFX frame generation swapchain enabled at runtime");
		return true;
	}

	if (wrappedSwapChain && wrappedSwapChain != originalSwapChain) {
		wrappedSwapChain->Release();
	}
	originalSwapChain->Release();
	logger::warn("[DX12SwapChain] FidelityFX frame generation swapchain could not be enabled");
	return false;
}

HRESULT DX12SwapChain::GetDevice(REFIID a_riid, void** a_device)
{
	static bool logged = false;
	if (!logged) {
		logged = true;
		logger::info("[DX12SwapChain] GetDevice first call");
	}
	if (!a_device) {
		return E_POINTER;
	}

	if (a_riid == __uuidof(ID3D11Device) ||
		a_riid == __uuidof(ID3D11Device1) ||
		a_riid == __uuidof(ID3D11Device2) ||
		a_riid == __uuidof(ID3D11Device3) ||
		a_riid == __uuidof(ID3D11Device4) ||
		a_riid == __uuidof(ID3D11Device5)) {
		return d3d11Device->QueryInterface(a_riid, a_device);
	}

	return swapChain->GetDevice(a_riid, a_device);
}

void DX12SwapChain::RefreshBackBuffers()
{
	for (auto& backBuffer : swapChainBuffers) {
		backBuffer = nullptr;
	}

	for (auto i = 0; i < std::size(swapChainBuffers); ++i) {
		DX::ThrowIfFailed(swapChain->GetBuffer(i, IID_PPV_ARGS(swapChainBuffers[i].put())));
	}
}
