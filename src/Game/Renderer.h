#pragma once

#include <d3d11.h>

// ===========================================================================
// Game/Renderer.h  --  Skyrim renderer access abstraction
//
// This is the single porting boundary between the game-agnostic render
// backend (Streamline / FidelityFX / DX12 proxy swapchain, all copied from the
// Fallout 4 `fo4test` project under src/Render/) and the concrete Creation
// Engine renderer.
//
// The original Fallout 4 code reached the D3D11 device/context through
//     RE::BSGraphics::GetRendererData()->device / ->context
// whose struct layout is CommonLibF4-specific. To stay correct across EVERY
// Skyrim runtime (SE 1.5.x / AE 1.6.x / GOG / VR) WITHOUT depending on a
// particular CommonLibSSE-NG struct layout, we instead capture the device and
// context the moment the game creates them in the D3D11CreateDeviceAndSwapChain
// hook (see src/Hooks/DX11Hooks.cpp) and hand them out from here.
//
// `GetRendererData()` returns an adapter object shaped like the fields the
// ported backend expects (`.device`, `.context` as void*), so those files
// compile after only a namespace substitution (RE::BSGraphics -> Game).
// ===========================================================================

namespace Game
{
	// Mirror of the two fields the ported render backend reads from the old
	// CommonLibF4 RendererData. Stored as void* to match the original casts:
	//   reinterpret_cast<ID3D11Device*>(GetRendererData()->device)
	struct RendererData
	{
		void* device = nullptr;   // ID3D11Device*
		void* context = nullptr;  // ID3D11DeviceContext*
	};

	// Called by the D3D11CreateDeviceAndSwapChain hook once the real device and
	// immediate context exist. Version-independent: no Address Library / struct
	// layout dependency.
	void SetD3D11Objects(ID3D11Device* a_device, ID3D11DeviceContext* a_context);

	[[nodiscard]] RendererData*        GetRendererData();
	[[nodiscard]] ID3D11Device*        GetD3D11Device();
	[[nodiscard]] ID3D11DeviceContext* GetD3D11Context();
	[[nodiscard]] bool                 IsReady();

	// Access a game render target's texture by RE::RENDER_TARGET, reading the
	// live RE::BSGraphics::Renderer render-target array (layout confirmed on
	// SE/AE). Returns nullptr if the renderer/slot is unavailable.
	[[nodiscard]] ID3D11Texture2D*     GetRenderTargetTexture(RE::RENDER_TARGET a_target);
	// The render-target view for a target (used to clear kMAIN to black for the
	// present-override UI-only pass). Returns nullptr if unavailable.
	[[nodiscard]] ID3D11RenderTargetView* GetRenderTargetRTV(RE::RENDER_TARGET a_target);

	// The Skyrim render-target enum lives in Game/Util.h (Util::RenderTarget),
	// mirroring RE::RENDER_TARGET, so it sits beside the camera helpers the
	// upscaler consumes.
}
