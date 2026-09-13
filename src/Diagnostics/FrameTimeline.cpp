#include "PCH.h"

#include "Diagnostics/FrameTimeline.h"

#include "Game/Renderer.h"

#include <array>
#include <atomic>
#include <d3d11.h>
#include <detours/Detours.h>
#include <unordered_map>

namespace
{
	constexpr uint32_t kOMSetRenderTargetsVTableIndex = 33;
	constexpr uint32_t kMaxEntries = 600;

	std::atomic<bool>     g_armed{ false };
	std::atomic<uint32_t> g_entries{ 0 };
	bool                  g_installed = false;

	// RTV pointer -> render target name, built once. The engine keeps its views
	// alive for the process, so a plain cache is enough and avoids resolving 114
	// targets on every bind.
	std::unordered_map<const void*, const char*> g_rtvNames;
	bool                                         g_namesBuilt = false;

	void BuildNames()
	{
		if (g_namesBuilt) {
			return;
		}
		const auto total = static_cast<uint32_t>(RE::RENDER_TARGET::kTOTAL);
		for (uint32_t i = 0; i < total; ++i) {
			const auto target = static_cast<RE::RENDER_TARGET>(i);
			if (auto* rtv = Game::GetRenderTargetRTV(target)) {
				const auto name = magic_enum::enum_name(target);
				g_rtvNames.emplace(rtv, name.empty() ? "?" : name.data());
			}
		}
		g_namesBuilt = !g_rtvNames.empty();
		logger::info("[FrameTimeline] Resolved {} render-target views by name", g_rtvNames.size());
	}

	const char* NameFor(const void* a_rtv)
	{
		if (!a_rtv) {
			return "(null)";
		}
		const auto it = g_rtvNames.find(a_rtv);
		return it != g_rtvNames.end() ? it->second : "(not a game RT)";
	}

	struct hkOMSetRenderTargets
	{
		static void STDMETHODCALLTYPE thunk(
			ID3D11DeviceContext*           This,
			UINT                           NumViews,
			ID3D11RenderTargetView* const* ppRenderTargetViews,
			ID3D11DepthStencilView*        pDepthStencilView)
		{
			if (g_armed.load(std::memory_order_relaxed)) {
				const auto n = g_entries.fetch_add(1, std::memory_order_relaxed);
				if (n < kMaxEntries) {
					const void* first = (NumViews > 0 && ppRenderTargetViews) ? ppRenderTargetViews[0] : nullptr;
					logger::info("[FrameTimeline] {:3}  OMSetRenderTargets n={} -> {}", n, NumViews, NameFor(first));
				} else if (n == kMaxEntries) {
					logger::info("[FrameTimeline] ... entry cap reached, stopping this capture");
					g_armed.store(false, std::memory_order_relaxed);
				}
			}
			func(This, NumViews, ppRenderTargetViews, pDepthStencilView);
		}
		static inline decltype(&thunk) func = nullptr;
	};
}

namespace FrameTimeline
{
	void Install()
	{
		if (g_installed) {
			return;
		}
		auto* context = Game::GetD3D11Context();
		if (!context) {
			logger::warn("[FrameTimeline] No D3D11 context yet; timeline capture unavailable");
			return;
		}
		*reinterpret_cast<uintptr_t*>(&hkOMSetRenderTargets::func) =
			Detours::X64::DetourClassVTable(*reinterpret_cast<uintptr_t*>(context),
				&hkOMSetRenderTargets::thunk, kOMSetRenderTargetsVTableIndex);
		g_installed = hkOMSetRenderTargets::func != nullptr;
		logger::info("[FrameTimeline] OMSetRenderTargets hook {}", g_installed ? "installed" : "FAILED");
	}

	void Arm()
	{
		BuildNames();
		g_entries.store(0, std::memory_order_relaxed);
		g_armed.store(true, std::memory_order_relaxed);
		logger::info("[FrameTimeline] ===== capture armed for one frame =====");
	}

	void Mark(const char* a_label)
	{
		if (!g_armed.load(std::memory_order_relaxed)) {
			return;
		}
		const auto n = g_entries.fetch_add(1, std::memory_order_relaxed);
		logger::info("[FrameTimeline] {:3}  >>> {} <<<", n, a_label);
	}

	void OnFrameBoundary()
	{
		if (g_armed.exchange(false, std::memory_order_relaxed)) {
			logger::info("[FrameTimeline] ===== capture ended ({} entries) =====",
				g_entries.load(std::memory_order_relaxed));
		}
	}
}
