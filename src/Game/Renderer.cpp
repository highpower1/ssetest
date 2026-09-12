#include "PCH.h"

#include "Game/Renderer.h"

namespace Game
{
	namespace
	{
		RendererData         g_rendererData{};
		ID3D11Device*        g_device = nullptr;
		ID3D11DeviceContext* g_context = nullptr;
	}

	void SetD3D11Objects(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
	{
		g_device = a_device;
		g_context = a_context;
		g_rendererData.device = a_device;
		g_rendererData.context = a_context;
		logger::info("[Renderer] Captured D3D11 device={} context={}",
			static_cast<void*>(a_device), static_cast<void*>(a_context));
	}

	RendererData* GetRendererData()
	{
		// Primary source: the game's own renderer data. Field layout confirmed
		// against the live AE 1.6.1170 dump (forwarder=ID3D11Device, context=
		// ID3D11DeviceContext). This works regardless of which D3D11 hook is
		// installed. The hook-captured values (SetD3D11Objects) take precedence
		// if present.
		if (!g_rendererData.device || !g_rendererData.context) {
			if (auto* data = RE::BSGraphics::Renderer::GetRendererData()) {
				if (!g_rendererData.device) {
					g_rendererData.device = data->forwarder;
				}
				if (!g_rendererData.context) {
					g_rendererData.context = data->context;
				}
			}
		}
		return &g_rendererData;
	}

	ID3D11Device*        GetD3D11Device()  { return reinterpret_cast<ID3D11Device*>(GetRendererData()->device); }
	ID3D11DeviceContext* GetD3D11Context() { return reinterpret_cast<ID3D11DeviceContext*>(GetRendererData()->context); }
	bool                 IsReady()         { return GetD3D11Device() != nullptr && GetD3D11Context() != nullptr; }

	ID3D11Texture2D* GetRenderTargetTexture(RE::RENDER_TARGET a_target)
	{
		auto* data = RE::BSGraphics::Renderer::GetRendererData();
		if (!data) {
			return nullptr;
		}
		const auto idx = static_cast<std::size_t>(a_target);
		if (idx >= static_cast<std::size_t>(RE::RENDER_TARGET::kTOTAL)) {
			return nullptr;
		}
		return reinterpret_cast<ID3D11Texture2D*>(data->renderTargets[idx].texture);
	}

	ID3D11RenderTargetView* GetRenderTargetRTV(RE::RENDER_TARGET a_target)
	{
		auto* data = RE::BSGraphics::Renderer::GetRendererData();
		if (!data) {
			return nullptr;
		}
		const auto idx = static_cast<std::size_t>(a_target);
		if (idx >= static_cast<std::size_t>(RE::RENDER_TARGET::kTOTAL)) {
			return nullptr;
		}
		return data->renderTargets[idx].RTV;
	}
}
