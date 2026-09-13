#include "PCH.h"

#include "Diagnostics/SceneTargetProbe.h"

#include "Game/Renderer.h"
#include "Settings/Settings.h"

namespace
{
	struct Candidate
	{
		RE::RENDER_TARGET target;
		const char*       name;
	};

	// Full-resolution targets that could plausibly carry the scene into ENB's
	// chain. Ordered most-likely first so a short sweep usually finds it.
	constexpr Candidate kCandidates[]{
		{ RE::RENDER_TARGET::kFRAMEBUFFER, "kFRAMEBUFFER" },
		{ RE::RENDER_TARGET::kMAIN, "kMAIN" },
		{ RE::RENDER_TARGET::kMAIN_COPY, "kMAIN_COPY" },
		{ RE::RENDER_TARGET::kMAIN_ONLY_ALPHA, "kMAIN_ONLY_ALPHA" },
		{ RE::RENDER_TARGET::kIMAGESPACE_TEMP_COPY, "kIMAGESPACE_TEMP_COPY" },
		{ RE::RENDER_TARGET::kIMAGESPACE_TEMP_COPY2, "kIMAGESPACE_TEMP_COPY2" },
		{ RE::RENDER_TARGET::kTEMPORAL_AA_ACCUMULATION_1, "kTEMPORAL_AA_ACCUMULATION_1" },
		{ RE::RENDER_TARGET::kTEMPORAL_AA_ACCUMULATION_2, "kTEMPORAL_AA_ACCUMULATION_2" },
		{ RE::RENDER_TARGET::kSCREENSHOT, "kSCREENSHOT" },
		{ RE::RENDER_TARGET::kMENUBG, "kMENUBG" },
		{ RE::RENDER_TARGET::kGETHIT_BUFFER, "kGETHIT_BUFFER" },
		{ RE::RENDER_TARGET::kGETHIT_BLURSWAP, "kGETHIT_BLURSWAP" },
		{ RE::RENDER_TARGET::kSNOW_SWAP, "kSNOW_SWAP" },
		{ RE::RENDER_TARGET::kRAW_WATER, "kRAW_WATER" },
	};
	constexpr uint32_t kCandidateCount = static_cast<uint32_t>(std::size(kCandidates));

	uint32_t g_index = 0;
	uint64_t g_frames = 0;
	uint32_t g_loggedIndex = kCandidateCount;  // force the first log
}

namespace SceneTargetProbe
{
	const char* CurrentName()
	{
		return g_index < kCandidateCount ? kCandidates[g_index].name : "none";
	}

	void Tick()
	{
		const auto& settings = SettingsStore::GetSingleton()->settings;
		const auto  mode = settings.sceneTargetProbe;
		if (mode == 0) {
			g_frames = 0;
			g_loggedIndex = kCandidateCount;
			return;
		}

		if (mode == 1) {
			// Sweep. The interval is in frames rather than seconds so that a slow
			// frame does not skip a candidate unseen.
			const auto interval = settings.sceneTargetProbeFrames == 0 ? 180u : settings.sceneTargetProbeFrames;
			if (++g_frames >= interval) {
				g_frames = 0;
				g_index = (g_index + 1) % kCandidateCount;
			}
		} else {
			g_index = std::min(mode - 2u, kCandidateCount - 1u);
		}

		if (g_index != g_loggedIndex) {
			g_loggedIndex = g_index;
			logger::info("[SceneProbe] Filling {} ({} of {}) with magenta", CurrentName(), g_index + 1, kCandidateCount);
		}

		auto* context = Game::GetD3D11Context();
		auto* rtv = Game::GetRenderTargetRTV(kCandidates[g_index].target);
		if (!context || !rtv) {
			return;
		}
		constexpr float kMagenta[4]{ 1.0f, 0.0f, 1.0f, 1.0f };
		context->ClearRenderTargetView(rtv, kMagenta);
	}

	void Mark()
	{
		if (SettingsStore::GetSingleton()->settings.sceneTargetProbe == 0) {
			logger::info("[SceneProbe] Mark pressed while the probe is off");
			return;
		}
		logger::info("[SceneProbe] *** HIT *** the magenta target when marked was {} (index {})",
			CurrentName(), g_index);
	}
}
