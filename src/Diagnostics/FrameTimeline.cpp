#include "PCH.h"

#include "Diagnostics/FrameTimeline.h"

#include "Game/Renderer.h"

#include <array>
#include <atomic>
#include <d3d11.h>
#include <winrt/base.h>
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

		// The first attempt hooked the context captured at device creation and saw
		// a handful of binds a frame, all of them kFRAMEBUFFER -- the whole world
		// render went somewhere else. Under ENB the object the game was handed and
		// the object the renderer actually draws on need not be the same, so
		// collect every context we can reach and hook each distinct vtable. A
		// vtable is shared by all instances of a class, so hooking one catches
		// every context of that type, including ENB's own calls.
		std::array<std::pair<const char*, ID3D11DeviceContext*>, 3> candidates{};
		candidates[0] = { "captured at device creation", Game::GetD3D11Context() };

		winrt::com_ptr<ID3D11DeviceContext> immediate;
		if (auto* device = Game::GetD3D11Device()) {
			device->GetImmediateContext(immediate.put());
			candidates[1] = { "device->GetImmediateContext", immediate.get() };
		}

		if (auto* data = RE::BSGraphics::Renderer::GetRendererData()) {
			if (auto* rendererContext = reinterpret_cast<ID3D11DeviceContext*>(data->context)) {
				candidates[2] = { "BSGraphics renderer data", rendererContext };
			}
		}

		std::array<uintptr_t, 3> hookedVTables{};
		uint32_t                 hookedCount = 0;
		for (const auto& [label, context] : candidates) {
			if (!context) {
				continue;
			}
			const auto vtable = *reinterpret_cast<uintptr_t*>(context);
			logger::info("[FrameTimeline] Context candidate '{}' = {} vtable=0x{:X}",
				label, static_cast<void*>(context), vtable);

			const bool already = std::find(hookedVTables.begin(), hookedVTables.begin() + hookedCount, vtable) !=
			                     hookedVTables.begin() + hookedCount;
			if (already) {
				logger::info("[FrameTimeline]   same vtable as one already hooked; skipping");
				continue;
			}

			auto* original = reinterpret_cast<decltype(&hkOMSetRenderTargets::thunk)>(
				Detours::X64::DetourClassVTable(vtable, &hkOMSetRenderTargets::thunk, kOMSetRenderTargetsVTableIndex));
			if (!original) {
				logger::warn("[FrameTimeline]   hook FAILED");
				continue;
			}
			// Every hooked vtable forwards to the same original, which is correct
			// only while they really are the same function. They are not
			// necessarily, so keep the first and say so if a later one differs.
			if (hkOMSetRenderTargets::func && hkOMSetRenderTargets::func != original) {
				logger::warn("[FrameTimeline]   original differs from the first hook; forwarding may be wrong, "
							 "not hooking this one");
				Detours::X64::DetourClassVTable(vtable, original, kOMSetRenderTargetsVTableIndex);
				continue;
			}
			hkOMSetRenderTargets::func = original;
			hookedVTables[hookedCount++] = vtable;
			logger::info("[FrameTimeline]   hooked");
		}

		g_installed = hookedCount > 0;
		logger::info("[FrameTimeline] OMSetRenderTargets hooked on {} distinct vtable(s)", hookedCount);
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
