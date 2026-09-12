#include "PCH.h"

#include "Hooks/DX11Hooks.h"

#include "Game/Renderer.h"

#include <d3d11.h>

// ===========================================================================
// Foundation D3D11 hook.
//
// Skyrim (like Fallout 4) creates its device through
// D3D11CreateDeviceAndSwapChain imported from d3d11.dll. We hook that import
// to:
//   1. force D3D_FEATURE_LEVEL_11_1 (needed by the DX12 interop / Streamline /
//      FidelityFX path), and
//   2. capture the resulting ID3D11Device / ID3D11DeviceContext so the whole
//      render backend can reach them in a runtime-layout-independent way.
//
// The full Fallout 4 version additionally hooks IDXGIFactory::CreateSwapChain
// to install a D3D12 proxy swapchain; that logic lives in
// src/Render/DX11Hooks.cpp and is folded in during the backend port.
// ===========================================================================

namespace
{
	using CreateDeviceAndSwapChain_t = HRESULT(WINAPI*)(
		IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
		const D3D_FEATURE_LEVEL*, UINT, UINT,
		const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
		ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

	struct hkD3D11CreateDeviceAndSwapChain
	{
		static HRESULT WINAPI thunk(
			IDXGIAdapter*               pAdapter,
			D3D_DRIVER_TYPE             DriverType,
			HMODULE                     Software,
			UINT                        Flags,
			const D3D_FEATURE_LEVEL*    pFeatureLevels,
			UINT                        FeatureLevels,
			UINT                        SDKVersion,
			const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
			IDXGISwapChain**            ppSwapChain,
			ID3D11Device**              ppDevice,
			D3D_FEATURE_LEVEL*          pFeatureLevel,
			ID3D11DeviceContext**       ppImmediateContext)
		{
			const D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_1;
			pFeatureLevels = &featureLevel;
			FeatureLevels = 1;

			const HRESULT hr = func(
				pAdapter, DriverType, Software, Flags,
				pFeatureLevels, FeatureLevels, SDKVersion,
				pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);

			if (SUCCEEDED(hr)) {
				ID3D11Device*        device = ppDevice ? *ppDevice : nullptr;
				ID3D11DeviceContext* context = ppImmediateContext ? *ppImmediateContext : nullptr;
				if (device && !context) {
					device->GetImmediateContext(&context);  // best-effort
				}
				Game::SetD3D11Objects(device, context);
				logger::info("[DX11Hooks] Game D3D11 device created (feature level forced to 11_1)");
			} else {
				logger::error("[DX11Hooks] D3D11CreateDeviceAndSwapChain failed: {:08X}", static_cast<uint32_t>(hr));
			}

			return hr;
		}
		static inline CreateDeviceAndSwapChain_t func = nullptr;
	};
}

namespace DX11Hooks
{
	void Install()
	{
		const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
		(uintptr_t&)hkD3D11CreateDeviceAndSwapChain::func = Detours::IATHook(
			moduleBase,
			"d3d11.dll",
			"D3D11CreateDeviceAndSwapChain",
			(uintptr_t)hkD3D11CreateDeviceAndSwapChain::thunk);

		if (hkD3D11CreateDeviceAndSwapChain::func) {
			logger::info("[DX11Hooks] Installed D3D11CreateDeviceAndSwapChain IAT hook");
		} else {
			logger::error("[DX11Hooks] Failed to install D3D11CreateDeviceAndSwapChain IAT hook");
		}
	}
}
