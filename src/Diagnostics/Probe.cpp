#include "PCH.h"

#include "Diagnostics/Probe.h"

#include "ENB/ENBSeriesAPI.h"

#include <d3d11.h>
#include <dxgi.h>

namespace Diagnostics
{
	namespace
	{
		// Force a flush after every step so that, if a later access faults, the
		// log's last line pinpoints exactly where it died.
		void Step(std::string_view a_msg)
		{
			logger::info("[step] {}", a_msg);
			spdlog::default_logger_raw()->flush();
		}

		const char* FormatName(DXGI_FORMAT a_format)
		{
			switch (a_format) {
			case DXGI_FORMAT_R8G8B8A8_UNORM:            return "R8G8B8A8_UNORM";
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:       return "R8G8B8A8_UNORM_SRGB";
			case DXGI_FORMAT_R8G8B8A8_TYPELESS:         return "R8G8B8A8_TYPELESS";
			case DXGI_FORMAT_B8G8R8A8_UNORM:            return "B8G8R8A8_UNORM";
			case DXGI_FORMAT_R10G10B10A2_UNORM:         return "R10G10B10A2_UNORM";
			case DXGI_FORMAT_R11G11B10_FLOAT:           return "R11G11B10_FLOAT";
			case DXGI_FORMAT_R16G16B16A16_FLOAT:        return "R16G16B16A16_FLOAT";
			case DXGI_FORMAT_R16G16_FLOAT:              return "R16G16_FLOAT";
			case DXGI_FORMAT_R16G16_UNORM:              return "R16G16_UNORM";
			case DXGI_FORMAT_R16_FLOAT:                 return "R16_FLOAT";
			case DXGI_FORMAT_R16_UNORM:                 return "R16_UNORM";
			case DXGI_FORMAT_R32G32B32A32_FLOAT:        return "R32G32B32A32_FLOAT";
			case DXGI_FORMAT_R32_FLOAT:                 return "R32_FLOAT";
			case DXGI_FORMAT_R8_UNORM:                  return "R8_UNORM";
			case DXGI_FORMAT_R24G8_TYPELESS:            return "R24G8_TYPELESS";
			case DXGI_FORMAT_D24_UNORM_S8_UINT:         return "D24_UNORM_S8_UINT";
			case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:     return "R24_UNORM_X8_TYPELESS";
			case DXGI_FORMAT_R32_TYPELESS:              return "R32_TYPELESS";
			case DXGI_FORMAT_D32_FLOAT:                 return "D32_FLOAT";
			default:                                    return "OTHER";
			}
		}

		std::string RenderTargetName(std::size_t a_index)
		{
			const auto rt = static_cast<RE::RENDER_TARGET>(a_index);
			const auto name = magic_enum::enum_name(rt);
			if (!name.empty()) {
				return std::string{ name };
			}
			return std::format("RT_{}", a_index);
		}

		void DumpRenderTargets(RE::BSGraphics::RendererData* a_data)
		{
			const auto count = static_cast<std::size_t>(RE::RENDER_TARGET::kTOTAL);
			logger::info("--- Render targets (kTOTAL = {}) ---", count);

			for (std::size_t i = 0; i < count; ++i) {
				const auto& rt = a_data->renderTargets[i];
				auto* texture = reinterpret_cast<ID3D11Texture2D*>(rt.texture);
				if (!texture) {
					continue;  // unallocated slot
				}

				D3D11_TEXTURE2D_DESC desc{};
				texture->GetDesc(&desc);

				logger::info(
					"RT[{:3}] {:<40} {}x{} fmt={} ({}) mips={} samples={} arr={} bind=0x{:X} | RTV={} SRV={} UAV={} copy={}",
					i,
					RenderTargetName(i),
					desc.Width, desc.Height,
					static_cast<int>(desc.Format), FormatName(desc.Format),
					desc.MipLevels, desc.SampleDesc.Count, desc.ArraySize, desc.BindFlags,
					rt.RTV ? "y" : "-",
					rt.SRV ? "y" : "-",
					rt.UAV ? "y" : "-",
					rt.textureCopy ? "y" : "-");
			}
		}

		void DumpDepthStencils(RE::BSGraphics::RendererData* a_data)
		{
			// IMPORTANT: do NOT call GetDesc() on these textures. Unlike the
			// render targets, the depthStencils[] texture pointers can be stale /
			// not-yet-allocated (especially at the main menu), and dereferencing
			// them (GetDesc) crashes. Reading the pointer *values* out of the
			// RendererData struct is safe, so we only log those.
			const auto count = static_cast<std::size_t>(RE::RENDER_TARGET_DEPTHSTENCIL::kTOTAL);
			logger::info("--- Depth-stencil targets (kTOTAL = {}) [pointers only] ---", count);

			for (std::size_t i = 0; i < count; ++i) {
				const auto& ds = a_data->depthStencils[i];
				if (!ds.texture && !ds.depthSRV && !ds.stencilSRV) {
					continue;
				}
				logger::info("DS[{:2}] texture={} depthSRV={} stencilSRV={}",
					i,
					static_cast<void*>(ds.texture),
					static_cast<void*>(ds.depthSRV),
					static_cast<void*>(ds.stencilSRV));
			}
		}

		void DumpCamera()
		{
			auto* cam = RE::Main::WorldRootCamera();
			if (!cam) {
				logger::info("Camera: WorldRootCamera() is null (probably not in world yet)");
				return;
			}
			logger::info("--- Camera (RE::Main::WorldRootCamera @ {}) ---", static_cast<void*>(cam));

			const auto& w = cam->world;  // NiTransform: rotate/translate/scale
			logger::info("world.pos   = ({:.3f}, {:.3f}, {:.3f})  scale={:.4f}",
				w.translate.x, w.translate.y, w.translate.z, w.scale);
			for (int r = 0; r < 3; ++r) {
				logger::info("world.rot[{}] = ({:.5f}, {:.5f}, {:.5f})",
					r, w.rotate.entry[r][0], w.rotate.entry[r][1], w.rotate.entry[r][2]);
			}

			// worldToCam = the game's view matrix (row-major 4x4).
			const auto& rd = cam->GetRuntimeData();
			for (int r = 0; r < 4; ++r) {
				logger::info("worldToCam[{}] = ({:.5f}, {:.5f}, {:.5f}, {:.5f})",
					r, rd.worldToCam[r][0], rd.worldToCam[r][1], rd.worldToCam[r][2], rd.worldToCam[r][3]);
			}

			// viewFrustum = projection parameters.
			const auto& rd2 = cam->GetRuntimeData2();
			const auto& f = rd2.viewFrustum;
			logger::info("frustum L={:.5f} R={:.5f} T={:.5f} B={:.5f} N={:.4f} F={:.1f} ortho={}",
				f.fLeft, f.fRight, f.fTop, f.fBottom, f.fNear, f.fFar, f.bOrtho ? 1 : 0);
			logger::info("minNearPlaneDist={:.4f} maxFarNearRatio={:.4f} lodAdjust={:.4f}",
				rd2.minNearPlaneDist, rd2.maxFarNearRatio, rd2.lodAdjust);
		}

		void DumpSwapChain(RE::BSGraphics::RendererData* a_data)
		{
			auto* swapChain = reinterpret_cast<IDXGISwapChain*>(a_data->renderWindows[0].swapChain);
			if (!swapChain) {
				logger::info("Swapchain: (none on renderWindows[0])");
				return;
			}
			DXGI_SWAP_CHAIN_DESC desc{};
			if (SUCCEEDED(swapChain->GetDesc(&desc))) {
				logger::info(
					"Swapchain: {}x{} fmt={} ({}) buffers={} windowed={} flags=0x{:X}",
					desc.BufferDesc.Width, desc.BufferDesc.Height,
					static_cast<int>(desc.BufferDesc.Format), FormatName(desc.BufferDesc.Format),
					desc.BufferCount, desc.Windowed ? 1 : 0, desc.Flags);
			}
		}
	}

	namespace
	{
		// Dump up to 16 bytes of (executable) memory at a_addr as hex.
		std::string HexBytes(std::uintptr_t a_addr, int a_count = 16)
		{
			if (!a_addr) {
				return "(null)";
			}
			std::string out;
			const auto* p = reinterpret_cast<const std::uint8_t*>(a_addr);
			for (int i = 0; i < a_count; ++i) {
				out += std::format("{:02X} ", p[i]);
			}
			return out;
		}

		// Verify a `write_thunk_call` candidate: the site should hold a
		// `call rel32` (0xE8) if PureDark's offset is still valid on this runtime.
		void VerifyCall(const char* a_name, REL::RelocationID a_id, std::size_t a_offset)
		{
			const auto base = a_id.address();
			const auto site = base + a_offset;
			const auto* p = reinterpret_cast<const std::uint8_t*>(site);
			const bool isCall = p[0] == 0xE8;
			logger::info("[hook] {:<26} base=0x{:X} +0x{:<4X} site=0x{:X}  first={:02X} {}  [{}]",
				a_name, base, a_offset, site, p[0], HexBytes(site, 8),
				isCall ? "OK call(0xE8)" : "!! NOT a call -- offset likely shifted");
		}

		// Log a function's entry bytes (prologue sanity check).
		void VerifyFunc(const char* a_name, REL::RelocationID a_id)
		{
			const auto base = a_id.address();
			logger::info("[func] {:<26} addr=0x{:X}  bytes={}", a_name, base, HexBytes(base, 12));
		}

		void VerifyHooks()
		{
			logger::info("--- PureDark hook candidates on THIS runtime (AE offsets) ---");
			// write_thunk_call sites: expect 0xE8 (call rel32).
			VerifyCall("Init_InitD3D",   REL::RelocationID(75595, 77226), REL::Relocate<std::size_t>(0x50, 0x2BC));
			VerifyCall("UpdateJitter",   REL::RelocationID(75460, 77245), REL::Relocate<std::size_t>(0xE5, 0xE2, 0x104));
			VerifyCall("MainDraw_PreUI", REL::RelocationID(79947, 82084), REL::Relocate<std::size_t>(0x16F, 0x17A, 0x132));
			// NOP-patch sites (game jitter disable): just log what's there.
			VerifyFunc("UpdateJitterFn",       REL::RelocationID(75709, 77518));
			VerifyFunc("BuildCameraStateData", REL::RelocationID(75711, 77520));
			// Base functions for the call hooks (prologue check).
			VerifyFunc("Renderer_Init",  REL::RelocationID(75595, 77226));
			VerifyFunc("Renderer_Begin", REL::RelocationID(75460, 77245));
			VerifyFunc("Main_DrawWorld", REL::RelocationID(79947, 82084));
		}
	}

	void DumpAll(const char* a_phase, bool a_inWorld)
	{
		try {
			logger::info("======== diagnostics dump [{}] ========", a_phase ? a_phase : "?");

			Step("runtime version");
			logger::info("Runtime version: {}", REL::Module::get().version());

			Step("module base");
			logger::info("Module base: 0x{:X}", reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr)));

			Step("ENB probe");
			logger::info("ENB present: {}", ENB_API::RequestENBAPI() ? "yes" : "no");

			Step("Renderer::GetRendererData");
			auto* data = RE::BSGraphics::Renderer::GetRendererData();
			if (!data) {
				logger::warn("RendererData is null -- renderer not ready yet, aborting dump");
				return;
			}
			logger::info("RendererData @ {} device(forwarder)={} context={}",
				static_cast<void*>(data),
				static_cast<void*>(data->forwarder),
				static_cast<void*>(data->context));

			// Most valuable + reliable payload first.
			Step("render targets");
			DumpRenderTargets(data);

			Step("depth-stencils");
			DumpDepthStencils(data);

			Step("swapchain");
			DumpSwapChain(data);

			Step("verify hooks");
			VerifyHooks();

			if (a_inWorld) {
				Step("camera");
				DumpCamera();
			} else {
				logger::info("Camera: skipped (not in world -- main menu / pre-load)");
			}

			// State singleton uses its own Address Library ID; do it last so a bad
			// read here never costs us the render-target dump above.
			Step("BSGraphics::State");
			if (auto* state = RE::BSGraphics::State::GetSingleton()) {
				logger::info("State screen: {}x{}", state->screenWidth, state->screenHeight);
			}

			logger::info("======== end dump [{}] ========", a_phase ? a_phase : "?");
			spdlog::default_logger_raw()->flush();
		} catch (const std::exception& e) {
			logger::error("[Diagnostics] dump exception: {}", e.what());
		} catch (...) {
			logger::error("[Diagnostics] dump unknown exception");
		}
	}
}
