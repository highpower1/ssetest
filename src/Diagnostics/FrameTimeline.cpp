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

	// The first pass of the frame that reads the finished scene out of kMAIN.
	// Several are listed because a configuration that disables one still has to
	// trigger: whichever comes first wins, and the frame is only triggered once.
	constexpr std::array kPostChainStart{
		RE::RENDER_TARGET::kIBLENSFLARES_LIGHTS_FILTER,
		RE::RENDER_TARGET::kIBLENSFLARES_DOWNSAMPLE_16X_4Y_PING,
		RE::RENDER_TARGET::kHDR_DOWNSAMPLE0,
		RE::RENDER_TARGET::kIMAGESPACE_TEMP_COPY,
	};

	void (*g_sceneComplete)() = nullptr;
	std::array<const void*, kPostChainStart.size()> g_postChainRTVs{};
	bool g_sceneCompleteFiredThisFrame = false;
	bool g_inSceneCompleteCallback = false;

	std::atomic<bool>     g_armed{ false };
	std::atomic<bool>     g_pending{ false };
	std::atomic<uint32_t> g_entries{ 0 };
	// Counted always, so "how many binds does this context actually see" is
	// answerable without arming anything.
	std::atomic<uint32_t> g_bindsThisFrame{ 0 };
	std::atomic<uint32_t> g_lastFrameBinds{ 0 };
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

	void BuildPostChainViews()
	{
		for (size_t i = 0; i < kPostChainStart.size(); ++i) {
			g_postChainRTVs[i] = Game::GetRenderTargetRTV(kPostChainStart[i]);
		}
	}

	bool IsPostChainStart(const void* a_rtv)
	{
		if (!a_rtv) {
			return false;
		}
		for (const auto* view : g_postChainRTVs) {
			if (view == a_rtv) {
				return true;
			}
		}
		return false;
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
			const void* firstView = (NumViews > 0 && ppRenderTargetViews) ? ppRenderTargetViews[0] : nullptr;

			// Run the upscaler here rather than at the pre-UI hook, where the whole
			// post chain and ENB have already finished with the scene. The guard is
			// not optional: the callback issues D3D11 work on this very context, so
			// without it the first bind it makes would re-enter this function.
			if (g_sceneComplete && !g_sceneCompleteFiredThisFrame && !g_inSceneCompleteCallback &&
				IsPostChainStart(firstView)) {
				g_sceneCompleteFiredThisFrame = true;
				g_inSceneCompleteCallback = true;
				g_sceneComplete();
				g_inSceneCompleteCallback = false;
			}

			g_bindsThisFrame.fetch_add(1, std::memory_order_relaxed);
			if (g_armed.load(std::memory_order_relaxed)) {
				const auto n = g_entries.fetch_add(1, std::memory_order_relaxed);
				if (n < kMaxEntries) {
					logger::info("[FrameTimeline] {:3}  OMSetRenderTargets n={} -> {}", n, NumViews, NameFor(firstView));
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
		// Arming immediately captured only whatever was left of the frame in
		// progress, which was two binds and the closing marker -- the world had
		// already been drawn. Start at the next frame boundary instead, so the
		// window is one whole frame.
		g_pending.store(true, std::memory_order_relaxed);
		logger::info("[FrameTimeline] ===== capture will start at the next frame boundary "
					 "(last frame saw {} binds) =====",
			g_lastFrameBinds.load(std::memory_order_relaxed));
	}

	void Mark(const char* a_label)
	{
		if (!g_armed.load(std::memory_order_relaxed)) {
			return;
		}
		const auto n = g_entries.fetch_add(1, std::memory_order_relaxed);
		logger::info("[FrameTimeline] {:3}  >>> {} <<<", n, a_label);
	}

	void SetSceneCompleteCallback(void (*a_callback)())
	{
		BuildNames();
		BuildPostChainViews();
		g_sceneComplete = a_callback;
		uint32_t resolved = 0;
		for (const auto* view : g_postChainRTVs) {
			resolved += view != nullptr ? 1u : 0u;
		}
		logger::info("[FrameTimeline] Scene-complete callback {} ({} of {} post-chain views resolved)",
			a_callback ? "registered" : "cleared", resolved, kPostChainStart.size());
	}

	void OnFrameBoundary()
	{
		g_sceneCompleteFiredThisFrame = false;
		g_lastFrameBinds.store(g_bindsThisFrame.exchange(0, std::memory_order_relaxed),
			std::memory_order_relaxed);

		if (g_armed.exchange(false, std::memory_order_relaxed)) {
			logger::info("[FrameTimeline] ===== capture ended ({} entries) =====",
				g_entries.load(std::memory_order_relaxed));
			return;
		}
		if (g_pending.exchange(false, std::memory_order_relaxed)) {
			g_entries.store(0, std::memory_order_relaxed);
			g_armed.store(true, std::memory_order_relaxed);
			logger::info("[FrameTimeline] ===== capture started, one full frame =====");
		}
	}
}
