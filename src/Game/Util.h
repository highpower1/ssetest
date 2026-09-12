#pragma once

#include <d3d11.h>

#include <DirectXMath.h>

#include <cstdint>
#include <utility>
#include <vector>

// ===========================================================================
// Game/Util.h  --  Skyrim port of the Fallout 4 Util layer.
//
// Two kinds of thing live here:
//   * Pure math + shader compilation -- ported verbatim (engine-agnostic).
//   * Camera acquisition -- the Fallout 4 code read a per-frame entry out of
//     RE::BSGraphics::State::cameraDataCache (a CommonLibF4-specific layout).
//     Skyrim's render camera data does not share that layout, so instead of
//     reverse-engineering an unverified struct we use the SAME strategy proven
//     for the D3D11 device (Game::Renderer): a small holder, Util::CameraFrame,
//     that the render hook fills once per frame and this layer consumes. The
//     public function surface matches the Fallout 4 Util so the eventual
//     Upscaling.cpp port is a drop-in.
// ===========================================================================

namespace Util
{
	// -----------------------------------------------------------------------
	// Skyrim render targets. Use the real engine enum directly.
	//
	// Confirmed against a live Skyrim AE 1.6.1170 dump (SkyrimUpscalerProbe):
	// every render target is allocated at NATIVE resolution (e.g. 1920x1080),
	// so upscaling works the Fallout-4 way -- swap the main-scene targets for
	// lower-resolution proxies during the scene pass, then upscale back.
	//
	//   RT index / name / format (from the dump):
	//     kMAIN(1)                    R16G16B16A16_FLOAT  <- upscaler color input (HDR)
	//     kMAIN_COPY(2)               R16G16B16A16_FLOAT
	//     kMAIN_ONLY_ALPHA(3)         R16G16B16A16_FLOAT
	//     kNORMAL_TAAMASK_SSRMASK(4)  R8G8B8A8_UNORM
	//     kMOTION_VECTOR(7)           R16G16_FLOAT        <- DLSS/FSR motion input
	//     kTEMPORAL_AA_ACCUMULATION_1/2(81/82) R8G8B8A8   <- TAA history (DLSS replaces)
	//     kTEMPORAL_AA_MASK(85)       (fmt 49)
	//     kSSR/RAW/BLURRED0(109/110/111) R16G16B16A16 @ half-res (960x540)
	// -----------------------------------------------------------------------
	using RenderTarget = RE::RENDER_TARGET;

	// Candidate set of main-scene render targets to recreate at render
	// resolution while upscaling. Grounded in the AE 1.6.1170 dump (these are
	// the full-res scene targets); the DEFINITIVE set must still be confirmed
	// in-game with RenderDoc once the render-pipeline hooks exist (PORTING.md).
	inline constexpr RE::RENDER_TARGET kScaledRenderTargets[] = {
		RE::RENDER_TARGET::kMAIN,
		RE::RENDER_TARGET::kMAIN_COPY,
		RE::RENDER_TARGET::kMAIN_ONLY_ALPHA,
		RE::RENDER_TARGET::kNORMAL_TAAMASK_SSRMASK,
		RE::RENDER_TARGET::kNORMAL_TAAMASK_SSRMASK_SWAP,
		RE::RENDER_TARGET::kMOTION_VECTOR,
		RE::RENDER_TARGET::kTEMPORAL_AA_ACCUMULATION_1,
		RE::RENDER_TARGET::kTEMPORAL_AA_ACCUMULATION_2,
		RE::RENDER_TARGET::kTEMPORAL_AA_MASK,
		RE::RENDER_TARGET::kSSR,
		RE::RENDER_TARGET::kSSR_RAW,
		RE::RENDER_TARGET::kSSR_BLURRED0,
	};

	// The upscaler color input and the motion-vector target, by name.
	inline constexpr RE::RENDER_TARGET kColorRenderTarget = RE::RENDER_TARGET::kMAIN;
	inline constexpr RE::RENDER_TARGET kMotionVectorRenderTarget = RE::RENDER_TARGET::kMOTION_VECTOR;

	// -----------------------------------------------------------------------
	// Per-frame camera data, populated by the render hook (Skyrim equivalent of
	// a Fallout 4 cameraDataCache entry). All matrices are row-major, matching
	// the Creation Engine convention the Fallout 4 code assumed.
	// -----------------------------------------------------------------------
	struct CameraFrame
	{
		DirectX::XMMATRIX viewMat = DirectX::XMMatrixIdentity();
		DirectX::XMMATRIX viewProjUnjittered = DirectX::XMMatrixIdentity();      // world->clip (current)
		DirectX::XMMATRIX prevViewProjUnjittered = DirectX::XMMatrixIdentity();  // world->clip (previous frame)
		DirectX::XMMATRIX projMat = DirectX::XMMatrixIdentity();
		DirectX::XMFLOAT3 position{ 0.0f, 0.0f, 0.0f };  // camera world position
		DirectX::XMFLOAT2 jitter{ 0.0f, 0.0f };          // pixel-space jitter for this frame
		float             cameraNear = 0.0f;
		float             cameraFar = 0.0f;
		const void*       referenceCamera = nullptr;     // identity token for history-reset detection
		bool              useJitter = false;
		bool              valid = false;                 // false until the hook fills it

		static CameraFrame* GetSingleton()
		{
			static CameraFrame singleton;
			return &singleton;
		}
	};

	// Minimal per-frame state. The Fallout 4 code read a frame counter off
	// RE::BSGraphics::State; Skyrim's mapped State doesn't expose one, so we keep
	// our own monotonic counter that the render/present hook advances once per
	// frame. Streamline uses it to key frame tokens and detect history breaks.
	struct FrameState
	{
		uint32_t frameCount = 0;

		static FrameState* GetSingleton()
		{
			static FrameState singleton;
			return &singleton;
		}
	};

	[[nodiscard]] inline FrameState* State_GetSingleton() { return FrameState::GetSingleton(); }

	struct CameraProjection
	{
		const CameraFrame* cameraState{ nullptr };
		DirectX::XMMATRIX  cameraViewToClip{};
		float              cameraFOV{ 0.0f };
		float              cameraAspectRatio{ 0.0f };
		bool               usedMatrixFOV{ false };
	};

	struct CameraBasis
	{
		DirectX::XMFLOAT3 right{};
		DirectX::XMFLOAT3 up{};
		DirectX::XMFLOAT3 forward{};
	};

	// Pure math helpers (engine-agnostic).
	DirectX::XMMATRIX ToXMMatrix(const __m128* a_matrix);
	bool              TryGetVerticalFOVFromProjection(const DirectX::XMMATRIX& a_projection, float& a_verticalFOV);

	// Halton(2,3) sub-pixel jitter for temporal upscalers (DLSS/FSR). The offset
	// is returned in pixel units (~[-0.5, 0.5]); the phase count follows the FSR2
	// rule 8*(displayWidth/renderWidth)^2 (== 8 at native/DLAA). The engine must
	// render the scene with this offset AND the upscaler must be told the same
	// offset, or temporal accumulation ghosts.
	std::int32_t GetJitterPhaseCount(std::int32_t a_renderWidth, std::int32_t a_displayWidth);
	void         GetJitterOffset(float& a_outX, float& a_outY, std::int32_t a_index, std::int32_t a_phaseCount);

	// Populate CameraFrame::GetSingleton() from the live render camera
	// (RE::Main::WorldRootCamera): view / world->clip / projection (built from
	// the view frustum) / position / near / far, and rolls the current
	// viewProj into prevViewProjUnjittered. Advances FrameState::frameCount.
	// Call once per rendered frame (from the render/present hook). Returns false
	// if there is no valid world camera (e.g. in menus). MUST be called in-world.
	bool CaptureWorldCamera();

	// Camera acquisition (reads the CameraFrame holder).
	const CameraFrame* GetWorldCameraStateData();
	CameraProjection   GetCameraProjection();
	bool               TryGetCameraBasis(const CameraFrame& a_frame, CameraBasis& a_basis);

	// Runtime shader compilation (uses the captured D3D11 device).
	ID3D11DeviceChild* CompileShader(const wchar_t* FilePath, const std::vector<std::pair<const char*, const char*>>& Defines, const char* ProgramType, const char* Program = "main");
}
