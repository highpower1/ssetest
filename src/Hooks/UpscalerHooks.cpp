#include "PCH.h"

#include "Hooks/UpscalerHooks.h"

#include "Game/Util.h"
#include "Game/Renderer.h"
#include "Render/Streamline.h"
#include "Upscaler/D3D12Upscaler.h"
#include "Upscaler/Upscaling.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <winrt/base.h>

// ===========================================================================
// Verified Skyrim render-pipeline hooks (see UpscalerHooks.h).
//
// Observational first pass: pass through to the original functions, and use the
// per-frame UpdateJitter hook (which runs once per rendered frame in-world) to
// populate Util::CameraFrame and advance FrameState::frameCount. Logging is
// throttled so we can confirm firing without spamming the log.
// ===========================================================================

namespace
{
	std::atomic<uint64_t> g_initFires{ 0 };
	std::atomic<uint64_t> g_jitterFires{ 0 };
	std::atomic<uint64_t> g_drawFires{ 0 };

	// STEP 2: dynamic resolution. Skyrim renders the scene at
	// fDynamicResolutionCurrentWidthScale/HeightScale of the full RT. Those live
	// in BSGraphics::State::RUNTIME_DATA (base SE 0x58 / AE 0x60) at +0xA4/+0xA8
	// (offsets from PureDark's verified State layout). We overwrite them every
	// frame in the UpdateJitter hook (same State*). Default ON at 2/3 for a
	// visible test (softer image + higher fps); a wrong offset is guarded by a
	// sanity check on the value we read back before writing.
	// DRS is driven by D3D12Upscaler::GetRenderScale() (see the UpdateJitter hook).
	// It is only safe to render at <1.0 because our Evaluate resolves the low-res
	// sub-rect back to full res; standalone DRS (no resolve) gives a broken corner
	// image under ENB, which is why the earlier DRS-only attempt failed.

	// STEP 3: sub-pixel jitter. DLSS/DLAA needs the scene rendered with a moving
	// Halton offset each frame, and the SAME offset fed to the upscaler, or the
	// temporal accumulator has nothing to resolve (image looks unchanged) or
	// ghosts. We write the offset into BSGraphics::State::jitter[2] (offset 0x44,
	// before the version-dependent RUNTIME_DATA, so identical on SE/AE) and stash
	// the raw pixel offset for D3D12Upscaler::Evaluate -> Streamline constants.
	// The two NOP patches in Install() force the engine to apply State::jitter to
	// the projection matrix even when in-game TAA is off (common with ENB).
	bool           g_jitterEnabled = true;
	std::int32_t   g_jitterIndex = 0;

	// BSGraphics::Renderer InitD3D -- fires once when the renderer is set up.
	struct Hook_InitD3D
	{
		static void thunk()
		{
			func();
			g_initFires++;
			logger::info("[UpscalerHooks] InitD3D fired -- renderer initialized");
			// Approach 1: stand up the D3D12 interop upscaler (persistent device +
			// Streamline D3D12 + shared textures). Also reports DLSS availability.
			// In the frame-gen experiment the upscaler REUSES the proxy swapchain's
			// D3D12 device + Streamline init (see D3D12Upscaler::Init), so it is
			// safe to init here too -- no second Streamline D3D12 initialisation.
			D3D12Upscaler::GetSingleton()->Init();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// BSGraphics::Renderer::Begin UpdateJitter -- fires once per rendered frame.
	// PureDark's signature is thunk(BSGraphics::State*). We take the pointer and
	// pass it through untouched (no jitter injection yet), then capture camera.
	struct Hook_UpdateJitter
	{
		static void thunk(void* a_state)
		{
			func(a_state);
			const auto n = ++g_jitterFires;
			// Pull live menu settings first so the DRS write below and the eval in
			// MainDrawWorld use the same method / render scale this frame.
			D3D12Upscaler::GetSingleton()->UpdateFromSettings();
			// Capture the live camera into Util::CameraFrame + advance frameCount.
			// (CaptureWorldCamera resets useJitter=false, so inject jitter AFTER.)
			Util::CaptureWorldCamera();

			// STEP 3: inject Halton jitter for DLSS/FSR temporal accumulation.
			// The jitter-always-on NOP patches force the engine to apply
			// State::jitter every frame, so when NO upscaler is active we must
			// write ZERO (otherwise the scene renders jittered with nothing to
			// resolve it => visible shimmer).
			if (g_jitterEnabled && a_state) {
				auto*          up = D3D12Upscaler::GetSingleton();
				auto*          p = reinterpret_cast<std::uint8_t*>(a_state);
				float*         stateJitter = reinterpret_cast<float*>(p + 0x44);
				auto*          frame = Util::CameraFrame::GetSingleton();
				const uint32_t rw = up->GetRenderWidth();
				const uint32_t rh = up->GetRenderHeight();
				if (up->IsActive() && rw > 0 && rh > 0) {
					const std::int32_t phase =
						Util::GetJitterPhaseCount(static_cast<std::int32_t>(rw), static_cast<std::int32_t>(up->GetDisplayWidth()));
					float jx = 0.0f, jy = 0.0f;
					Util::GetJitterOffset(jx, jy, ++g_jitterIndex, phase);

					// Engine wants the offset in NDC units (matches PureDark): the
					// projection is nudged by jitter[0/1] before the scene renders.
					stateJitter[0] = -2.0f * jx / static_cast<float>(rw);
					stateJitter[1] = 2.0f * jy / static_cast<float>(rh);
					frame->jitter = { jx, jy };  // raw px; UpdateConstants negates it
					frame->useJitter = true;

					if (n <= 3) {
						logger::info("[Jitter] frame={} phase={} px=({:.3f},{:.3f}) ndc=({:.5f},{:.5f})",
							n, phase, jx, jy, stateJitter[0], stateJitter[1]);
					}
				} else {
					stateJitter[0] = 0.0f;
					stateJitter[1] = 0.0f;
					frame->jitter = { 0.0f, 0.0f };
					frame->useJitter = false;
				}
			}

			// STEP 2c: dynamic resolution override. Drive the engine to render the
			// scene at the upscaler's render scale (< 1.0) into a sub-rect of the
			// full RTs; D3D12Upscaler::Evaluate (pre-UI) resolves that sub-rect back
			// to full res via DLSS and then resets the scale so downstream is full.
			auto*       drsUp = D3D12Upscaler::GetSingleton();
			const float drsScale = drsUp->GetRenderScale();
			if (a_state && drsUp->IsActive() && drsScale < 0.999f) {
				static const std::size_t rtBase = REL::Relocate<std::size_t>(0x58, 0x60);
				auto* p = reinterpret_cast<std::uint8_t*>(a_state);
				float* widthScale = reinterpret_cast<float*>(p + rtBase + 0xA4);
				float* heightScale = reinterpret_cast<float*>(p + rtBase + 0xA8);
				const float before = *widthScale;
				const bool sane = std::isfinite(before) && before > 0.05f && before < 4.0f;
				if (sane) {
					*widthScale = drsScale;
					*heightScale = drsScale;
				}
				if (n <= 2) {
					logger::info("[DRS] rtBase=0x{:X} fDynResScale before={} sane={} -> set={}",
						rtBase, before, sane, sane ? drsScale : before);
				}
			}

			if (n <= 3 || (n % 600) == 0) {
				const auto* frame = Util::CameraFrame::GetSingleton();
				logger::info("[UpscalerHooks] UpdateJitter fired frame={} cameraValid={} pos=({:.1f},{:.1f},{:.1f}) near={:.1f} far={:.0f}",
					n, frame->valid,
					frame->position.x, frame->position.y, frame->position.z,
					frame->cameraNear, frame->cameraFar);
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// Main::DrawWorld pre-UI -- where the upscaler will run (EvaluateUpscaler).
	// Observational: pass through, just count/log.
	struct Hook_MainDrawWorld
	{
		static void thunk(std::int64_t a_renderer, int a_unk)
		{
			const auto n = ++g_drawFires;
			// Approach 1 eval: process the main color through the D3D12 interop
			// (increment 2a = identity round-trip to validate sync).
			D3D12Upscaler::GetSingleton()->Evaluate();
			func(a_renderer, a_unk);
			if (n <= 3 || (n % 600) == 0) {
				logger::info("[UpscalerHooks] MainDrawWorld(pre-UI) fired frame={}", n);
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
}

namespace UpscalerHooks
{
	void Install()
	{
		stl::write_thunk_call<Hook_InitD3D>(
			REL::RelocationID(75595, 77226).address() + REL::Relocate<std::size_t>(0x50, 0x2BC));

		stl::write_thunk_call<Hook_UpdateJitter>(
			REL::RelocationID(75460, 77245).address() + REL::Relocate<std::size_t>(0xE5, 0xE2, 0x104));

		stl::write_thunk_call<Hook_MainDrawWorld>(
			REL::RelocationID(79947, 82084).address() + REL::Relocate<std::size_t>(0x16F, 0x17A, 0x132));

		// STEP 3: force the engine to always apply BSGraphics::State::jitter to the
		// projection matrix, even when in-game TAA is disabled (ENB users commonly
		// turn it off). Without this our injected Halton jitter never reaches the
		// rendered scene, and DLSS would receive a non-zero jitter offset for an
		// un-jittered image -> ghosting. NOPs the "skip jitter when TAA off"
		// branches in UpdateJitter and BuildCameraStateData (offsets from PureDark;
		// the same verified RelocationIDs used for the hooks above). Flatrim only.
		if (g_jitterEnabled) {
			const REL::Relocation<std::uintptr_t> updateJitterFn{ REL::RelocationID(75709, 77518) };
			const REL::Relocation<std::uintptr_t> buildCameraStateData{ REL::RelocationID(75711, 77520) };
			constexpr std::uint8_t nop6[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
			constexpr std::uint8_t nop10[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
			REL::safe_write(updateJitterFn.address() + REL::Relocate<std::size_t>(0xE, 0x11), nop6, sizeof(nop6));
			REL::safe_write(buildCameraStateData.address() + 0x1D5, nop10, sizeof(nop10));
			logger::info("[UpscalerHooks] Applied jitter-always-on patches (updateJitter=0x{:X}, buildCameraStateData=0x{:X})",
				updateJitterFn.address() + REL::Relocate<std::size_t>(0xE, 0x11), buildCameraStateData.address() + 0x1D5);
		}

		logger::info("[UpscalerHooks] Installed verified render-pipeline hooks (observational pass)");
	}
}
