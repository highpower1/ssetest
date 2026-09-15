#include "PCH.h"

#include "Hooks/UpscalerHooks.h"

#include "Game/Util.h"
#include "Game/Renderer.h"
#include "Diagnostics/FrameTimeline.h"
#include "Diagnostics/SceneTargetProbe.h"
#include "Neural/NeuralRendering.h"
#include "Settings/Settings.h"
#include "Render/DX12SwapChain.h"
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

	// Whether the dynamic-resolution offsets are trustworthy on this game build.
	// Checked once against the shape of the data rather than assumed, because
	// they are hardcoded across every supported version.
	bool           g_drsOffsetVerified = false;
	bool           g_drsOffsetUsable = true;
	float          g_drsLastWritten = 0.0f;

	// ---- camera-state patch self-repair ---------------------------------
	//
	// The camera-state NOP is written at an offset that is the same for every
	// supported build, and on a build where it is wrong it corrupts camera
	// state. That was reported as a third-person camera sitting high and to the
	// left with the field of view changing as the player looked around, and it
	// cannot be reproduced here, so there is no way to find the right offset by
	// inspection.
	//
	// So do not try. Keep the original bytes, watch for the symptom, and put
	// them back if it appears. What distinguishes the fault from a legitimate
	// field-of-view change is that it oscillates: sprinting, drawing a bow and
	// the killcam all ramp the value smoothly in one direction, while a
	// corrupted camera flips between values frame to frame. Count reversals of
	// a large per-frame delta, and revert on a run of them.
	std::uintptr_t g_cameraPatchSite = 0;
	std::uint8_t   g_cameraPatchOriginal[10]{};
	bool           g_cameraPatchApplied = false;
	bool           g_cameraPatchReverted = false;
	float          g_lastFOV = 0.0f;
	float          g_lastFOVDelta = 0.0f;
	std::uint32_t  g_fovReversals = 0;

	// A vertical field of view moves by a fraction of a degree per frame when
	// the game is changing it on purpose. Anything past this is not animation.
	constexpr float kFOVJumpRadians = 0.02f;   // ~1.1 degrees in one frame
	constexpr std::uint32_t kFOVReversalsToRevert = 12;

	void WatchCameraPatchForCorruption()
	{
		// Runs whether or not the camera patch was applied: the likelier culprit
		// is the dynamic-resolution write, which is independent of it.
		if (g_cameraPatchReverted && !g_drsOffsetUsable) {
			return;  // nothing left to switch off
		}

		const auto projection = Util::GetCameraProjection();
		if (!projection.cameraState || !projection.usedMatrixFOV || projection.cameraFOV <= 0.0f) {
			return;
		}

		const float fov = projection.cameraFOV;
		if (g_lastFOV <= 0.0f) {
			g_lastFOV = fov;
			return;
		}

		const float delta = fov - g_lastFOV;
		g_lastFOV = fov;

		if (std::abs(delta) < kFOVJumpRadians) {
			// A quiet frame ends the run. Real corruption does not stop.
			g_fovReversals = 0;
			g_lastFOVDelta = delta;
			return;
		}

		if (g_lastFOVDelta != 0.0f && ((delta > 0.0f) != (g_lastFOVDelta > 0.0f))) {
			++g_fovReversals;
		} else {
			g_fovReversals = 0;
		}
		g_lastFOVDelta = delta;

		if (g_fovReversals < kFOVReversalsToRevert) {
			return;
		}
		g_fovReversals = 0;

		// Two things write into the game on our behalf, and a player narrowed
		// which one this is: the fault appears with DLSS Quality and not with
		// DLAA. The camera-state patch is applied once at startup regardless of
		// quality mode, so it cannot be the difference. The dynamic-resolution
		// write is skipped entirely at native scale, so it is. Stop that one
		// first, and only reach for the patch if the oscillation survives it.
		if (g_drsOffsetUsable) {
			g_drsOffsetUsable = false;
			logger::warn("[UpscalerHooks] The field of view has flipped direction {} times in a row by more "
						 "than {:.3f} rad a frame, which is what corrupted camera state looks like and what "
						 "no legitimate field-of-view animation does. The dynamic-resolution offsets are "
						 "the only thing we write that a quality mode turns on, so they are wrong for this "
						 "game build and have been switched off for the session. Quality modes will now "
						 "render at full resolution -- slower, but correct. Please report this line with "
						 "your exact game version.",
				kFOVReversalsToRevert, kFOVJumpRadians);
			return;
		}

		if (!g_cameraPatchApplied || g_cameraPatchReverted) {
			return;
		}

		// Still oscillating with dynamic resolution off, so the other patch is
		// the one. Put the game's own bytes back; ghosting is far cheaper than a
		// camera that cannot be aimed.
		REL::safe_write(g_cameraPatchSite, g_cameraPatchOriginal, sizeof(g_cameraPatchOriginal));
		g_cameraPatchReverted = true;
		logger::warn("[UpscalerHooks] The field of view is still oscillating with dynamic resolution "
					 "disabled, so the camera-state jitter patch is landing on the wrong bytes for this "
					 "build too. The original code has been restored and the patch will not be applied "
					 "again this session. The image may ghost slightly.");
	}

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
			// Log-only interception of the game's render-target binds; nothing is
			// captured until a key arms it.
			FrameTimeline::Install();
			// Plan A: with the render-target timeline in hand, the upscaler can run
			// where the scene is finished and nothing has read it yet, instead of
			// after ENB has already produced the frame.
			FrameTimeline::SetSceneCompleteCallback([] {
				if (SettingsStore::GetSingleton()->settings.upscalerHookPoint == 1) {
					D3D12Upscaler::GetSingleton()->Evaluate();
				}
			});
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
			WatchCameraPatchForCorruption();

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
			if (a_state && drsUp->IsActive() && drsScale < 0.999f && g_drsOffsetUsable) {
				static const std::size_t rtBase = REL::Relocate<std::size_t>(0x58, 0x60);
				auto* p = reinterpret_cast<std::uint8_t*>(a_state);
				float* widthScale = reinterpret_cast<float*>(p + rtBase + 0xA4);
				float* heightScale = reinterpret_cast<float*>(p + rtBase + 0xA8);

				// These offsets are inherited and not adjusted per game build, so
				// on a runtime where they are wrong this writes a float into some
				// other field of State every frame. The old guard -- finite and
				// between 0.05 and 4.0 -- passes a vertical field of view in
				// radians without noticing, and overwriting that every frame looks
				// exactly like a camera whose FOV changes as you look around.
				//
				// Check the shape of the data instead of just its range. Skyrim's
				// dynamic resolution keeps the two scales in lockstep and at or
				// below 1.0, and a pair of unrelated floats is very unlikely to be
				// bit-identical. Verified once, before the first write, and again
				// against what we last wrote.
				// Before trusting a hardcoded offset at all, see whether the game
				// will name the field for us. INI settings are looked up by string
				// through the engine's own collection, so a name that resolves is
				// correct on every build by construction -- which is exactly what a
				// raw offset is not. Probed once and logged either way, because if
				// this works it replaces the guessing entirely.
				static bool probedNamedScale = false;
				if (!probedNamedScale) {
					probedNamedScale = true;
					for (const char* name : { "fDynamicResolutionCurrentWidthScale:Display",
							 "fDynamicResolutionCurrentHeightScale:Display" }) {
						auto* setting = RE::GetINISetting(name);
						logger::info("[DRS] INI setting '{}' {}", name,
							setting ? std::format("resolved, value={}", setting->data.f) : "not found");
					}
				}

				if (!g_drsOffsetVerified) {
					const float w = *widthScale;
					const float h = *heightScale;
					const bool plausible = std::isfinite(w) && std::isfinite(h) &&
					                       w == h && w > 0.2f && w <= 1.0001f;
					g_drsOffsetVerified = true;
					g_drsOffsetUsable = plausible;
					logger::info("[DRS] rtBase=0x{:X} scale pair read ({}, {}) -> {}",
						rtBase, w, h,
						plausible ? "accepted" : "REJECTED, dynamic resolution is off for this session");
					if (!plausible) {
						logger::warn("[DRS] The dynamic-resolution offsets do not look like a scale pair on "
									 "this game build. Writing there anyway would corrupt whatever is "
									 "actually at that address, so it is left alone. Upscaling still works; "
									 "the quality modes will render at full resolution, which costs "
									 "performance but nothing else.");
					}
				}

				if (g_drsOffsetUsable) {
					// If something other than us owns this memory it will not read
					// back as the value we wrote. Catch that rather than keep
					// writing into it for the rest of the session.
					if (g_drsLastWritten > 0.0f && *widthScale != *heightScale) {
						g_drsOffsetUsable = false;
						logger::warn("[DRS] The two scale fields stopped matching ({} vs {}) after we wrote "
									 "{}. Something else owns that memory on this build; dynamic resolution "
									 "is off for this session.",
							*widthScale, *heightScale, g_drsLastWritten);
					} else {
						*widthScale = drsScale;
						*heightScale = drsScale;
						g_drsLastWritten = drsScale;
					}
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
			// Neural rendering (B): external RenoDX-style modules get the scene
			// colour BEFORE upscaling, while it is still hud-less and at render
			// resolution -- so a module's output is what DLSS/FSR then resolves.
			// Diagnostic only, off unless SceneTargetProbe is set: paint one
			// candidate render target magenta so ENB's output says whether it
			// reads that one.
			FrameTimeline::Mark("pre-UI hook: upscaler runs here");
			SceneTargetProbe::Tick();
			NeuralRendering::GetSingleton()->OnFrame();
			// Approach 1 eval: process the main color through the D3D12 interop
			// (increment 2a = identity round-trip to validate sync). Skipped when
			// the scene-complete hook owns the evaluation instead -- but only while
			// that hook is actually firing. It depends on a render-target bind we
			// do not control, so a setup where none of the post-chain targets is
			// used would otherwise leave nothing running the upscaler at all.
			const auto hookPoint = SettingsStore::GetSingleton()->settings.upscalerHookPoint;
			auto*      upscaling = Upscaling::GetSingleton();
			bool       evaluateHere = hookPoint == 0;
			if (hookPoint == 1) {
				// The scene-complete trigger is a render-target bind, and some of
				// the passes it watches for only run when the scene calls for them
				// -- lens flares need a light source on screen. So it fires while
				// the player faces one way and not while they face another.
				//
				// Deciding the output path per frame off that was the real damage.
				// The two paths composite completely differently, so flipping
				// between them as the camera turns looks exactly like the upscaler
				// switching itself on and off, which is how a player described it.
				//
				// Sample it over a window instead and then commit for the session.
				// A path that is wrong but steady is a performance problem; a path
				// that changes with view direction is a visible fault.
				static uint32_t sampledFrames = 0;
				static uint32_t firedFrames = 0;
				static bool     decided = false;

				if (!decided) {
					++sampledFrames;
					if (FrameTimeline::SceneCompleteFiredThisFrame()) {
						++firedFrames;
					}
					constexpr uint32_t kWindow = 600;
					if (sampledFrames >= kWindow) {
						decided = true;
						// Anything short of nearly every frame means the trigger is
						// tied to something optional, and the steady path is the one
						// that does not depend on it.
						const bool reliable = firedFrames * 100 >= sampledFrames * 95;
						upscaling->sceneCompleteFallback = !reliable;
						if (reliable) {
							logger::info("[UpscalerHooks] Scene-complete hook fired on {} of {} frames; "
										 "upscaling before the game's post-processing for this session.",
								firedFrames, sampledFrames);
						} else {
							logger::warn("[UpscalerHooks] Scene-complete hook fired on only {} of {} frames, "
										 "so it depends on passes this scene does not always run. Using the "
										 "pre-UI hook with the present override for the whole session "
										 "instead: ENB's grading will not reach the upscaled image, but the "
										 "image will not change as you turn the camera.",
								firedFrames, sampledFrames);
						}
					}
					// Until the decision is made, stay on the path that always
					// reaches the screen rather than alternating.
					upscaling->sceneCompleteFallback = true;
				}

				// Committed to the scene-complete path: the callback owns the
				// evaluation. On a frame where the trigger misses we do nothing
				// rather than run here, because at this point in the frame the
				// copy-back no longer reaches the screen under ENB -- running
				// would cost the work and show the same un-upscaled frame either
				// way.
				evaluateHere = upscaling->sceneCompleteFallback;
			} else if (upscaling->sceneCompleteFallback) {
				upscaling->sceneCompleteFallback = false;
			}
			if (evaluateHere) {
				D3D12Upscaler::GetSingleton()->Evaluate();
			}
			func(a_renderer, a_unk);
			// Everything the world and ENB draw has landed in the presented buffer
			// by here; the UI has not. Snapshot it so the composite can subtract
			// this from the final buffer and get the UI on its own.
			DX12SwapChain::GetSingleton()->CaptureUIBaseline();
			FrameTimeline::Mark("pre-UI hook: UI baseline captured");
			FrameTimeline::OnFrameBoundary();
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

			// Log what is actually at each site before overwriting it. These are
			// PureDark's offsets and the camera-state one is not version-adjusted,
			// so on a runtime where it is wrong this patch silently destroys ten
			// bytes of something else. A log that names the bytes turns "the
			// camera is broken on my machine" into a comparable fact.
			const auto siteBytes = [](std::uintptr_t a_address, std::size_t a_count) {
				std::string text;
				for (std::size_t i = 0; i < a_count; ++i) {
					text += std::format("{:02X} ", *reinterpret_cast<const std::uint8_t*>(a_address + i));
				}
				return text;
			};

			const auto jitterSite = updateJitterFn.address() + REL::Relocate<std::size_t>(0xE, 0x11);
			const auto cameraSite = buildCameraStateData.address() + 0x1D5;
			logger::info("[UpscalerHooks] Jitter patch sites: updateJitter=0x{:X} [{}] buildCameraStateData=0x{:X} [{}]",
				jitterSite, siteBytes(jitterSite, sizeof(nop6)), cameraSite, siteBytes(cameraSite, sizeof(nop10)));

			REL::safe_write(jitterSite, nop6, sizeof(nop6));

			// Both patches replace a branch that skips applying the jitter. What
			// that branch is encoded as varies, but every encoding of one begins
			// with a jump or with a compare feeding a jump a couple of bytes
			// later. If the bytes here are neither, this is not the branch on this
			// build and NOPing them destroys whatever it really is -- so decline.
			// The cost of declining is ghosting. The cost of being wrong is the
			// game's camera.
			const auto looksLikeBranch = [](std::uintptr_t a_address, std::size_t a_count) {
				for (std::size_t i = 0; i < std::min<std::size_t>(a_count, 6); ++i) {
					const auto op = *reinterpret_cast<const std::uint8_t*>(a_address + i);
					if (op == 0xEB || (op >= 0x70 && op <= 0x7F)) {
						return true;  // jmp / jcc rel8
					}
					if (op == 0x0F) {
						const auto second = *reinterpret_cast<const std::uint8_t*>(a_address + i + 1);
						if (second >= 0x80 && second <= 0x8F) {
							return true;  // jcc rel32
						}
					}
				}
				return false;
			};

			const bool patchRequested = SettingsStore::GetSingleton()->settings.cameraStateJitterPatch != 0;
			const bool cameraSiteLooksRight = looksLikeBranch(cameraSite, sizeof(nop10));
			const bool patchCamera = patchRequested && cameraSiteLooksRight;
			if (patchRequested && !cameraSiteLooksRight) {
				logger::warn("[UpscalerHooks] The camera-state jitter patch site does not contain a branch on "
							 "this game build, so it has NOT been written. The offset is inherited and is not "
							 "adjusted per version; overwriting the wrong ten bytes there corrupts the "
							 "camera. Upscaling still works and may ghost slightly. Please report the byte "
							 "line above with your exact game version.");
			}
			if (patchCamera) {
				std::memcpy(g_cameraPatchOriginal, reinterpret_cast<const void*>(cameraSite), sizeof(g_cameraPatchOriginal));
				g_cameraPatchSite = cameraSite;
				REL::safe_write(cameraSite, nop10, sizeof(nop10));
				g_cameraPatchApplied = true;
			} else if (!patchRequested) {
				logger::warn("[UpscalerHooks] The camera-state jitter patch is disabled by setting. The scene "
							 "may render without our sub-pixel offset while the upscaler is told there is "
							 "one, which ghosts -- but if a camera or field-of-view fault goes away with "
							 "this off, that patch is the cause.");
			}
			logger::info("[UpscalerHooks] Applied jitter-always-on patches (updateJitter=yes, buildCameraStateData={})",
				patchCamera ? "yes" : "SKIPPED");
		}

		logger::info("[UpscalerHooks] Installed verified render-pipeline hooks (observational pass)");
	}
}
