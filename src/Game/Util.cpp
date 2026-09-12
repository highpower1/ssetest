#include "PCH.h"

#include "Game/Util.h"
#include "Game/Renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <d3dcompiler.h>
#include <winrt/base.h>

namespace Util
{
	namespace
	{
		bool Normalize(DirectX::XMFLOAT3& a_vector)
		{
			const auto lengthSq =
				a_vector.x * a_vector.x +
				a_vector.y * a_vector.y +
				a_vector.z * a_vector.z;
			if (!std::isfinite(lengthSq) || lengthSq <= 0.0f) {
				return false;
			}

			const auto invLength = 1.0f / std::sqrt(lengthSq);
			a_vector.x *= invLength;
			a_vector.y *= invLength;
			a_vector.z *= invLength;
			return true;
		}

		// Left-handed basis reconstruction (Streamline convention), matching the
		// Fallout 4 code. Skyrim shares the Creation Engine's row-major matrices.
		bool Orthonormalize(CameraBasis& a_basis)
		{
			if (!Normalize(a_basis.right) || !Normalize(a_basis.forward)) {
				return false;
			}

			a_basis.up = {
				a_basis.forward.y * a_basis.right.z - a_basis.forward.z * a_basis.right.y,
				a_basis.forward.z * a_basis.right.x - a_basis.forward.x * a_basis.right.z,
				a_basis.forward.x * a_basis.right.y - a_basis.forward.y * a_basis.right.x
			};
			if (!Normalize(a_basis.up)) {
				return false;
			}

			a_basis.right = {
				a_basis.up.y * a_basis.forward.z - a_basis.up.z * a_basis.forward.y,
				a_basis.up.z * a_basis.forward.x - a_basis.up.x * a_basis.forward.z,
				a_basis.up.x * a_basis.forward.y - a_basis.up.y * a_basis.forward.x
			};
			return Normalize(a_basis.right);
		}

		bool TrySetCameraProjection(CameraProjection& a_result, const DirectX::XMMATRIX& a_projection)
		{
			float verticalFOV = 0.0f;
			if (!TryGetVerticalFOVFromProjection(a_projection, verticalFOV)) {
				return false;
			}

			DirectX::XMFLOAT4X4 projection{};
			DirectX::XMStoreFloat4x4(&projection, a_projection);
			const auto aspectRatio = projection._22 / projection._11;
			if (!std::isfinite(aspectRatio) || aspectRatio <= 0.0f) {
				return false;
			}

			a_result.cameraViewToClip = a_projection;
			a_result.cameraFOV = verticalFOV;
			a_result.cameraAspectRatio = aspectRatio;
			a_result.usedMatrixFOV = true;
			return true;
		}
	}

	DirectX::XMMATRIX ToXMMatrix(const __m128* a_matrix)
	{
		return DirectX::XMMATRIX(a_matrix[0], a_matrix[1], a_matrix[2], a_matrix[3]);
	}

	bool TryGetVerticalFOVFromProjection(const DirectX::XMMATRIX& a_projection, float& a_verticalFOV)
	{
		DirectX::XMFLOAT4X4 projection{};
		DirectX::XMStoreFloat4x4(&projection, a_projection);

		for (std::size_t row = 0; row < 4; ++row) {
			for (std::size_t column = 0; column < 4; ++column) {
				if (!std::isfinite(projection.m[row][column])) {
					return false;
				}
			}
		}

		// The Creation Engine (Fallout 4 and Skyrim alike) builds its perspective
		// projection with positive X/Y scales, M34 == 1, and M44 == 0. Validate
		// that shape rather than rejecting narrow but legitimate FOVs.
		constexpr auto perspectiveTolerance = 1.0e-5f;
		if (projection._11 <= 0.0f ||
			projection._22 <= 0.0f ||
			projection._33 <= 0.0f ||
			projection._43 >= 0.0f ||
			std::abs(projection._34 - 1.0f) > perspectiveTolerance ||
			std::abs(projection._44) > perspectiveTolerance) {
			return false;
		}

		constexpr auto pi = 3.14159265358979323846f;
		const auto verticalFOV = 2.0f * std::atan2(1.0f, projection._22);
		if (!std::isfinite(verticalFOV) || verticalFOV <= 0.0f || verticalFOV >= pi) {
			return false;
		}

		a_verticalFOV = verticalFOV;
		return true;
	}

	bool CaptureWorldCamera()
	{
		auto* cam = RE::Main::WorldRootCamera();
		if (!cam) {
			return false;
		}

		auto* frame = CameraFrame::GetSingleton();

		// Roll current -> previous for temporal reprojection.
		frame->prevViewProjUnjittered = frame->valid ? frame->viewProjUnjittered : DirectX::XMMatrixIdentity();

		// worldToCam is the game's world->clip (view*projection), row-major.
		const auto& rd = cam->GetRuntimeData();
		DirectX::XMFLOAT4X4 wtc{};
		std::memcpy(&wtc, &rd.worldToCam[0][0], sizeof(float) * 16);
		const auto worldToClip = DirectX::XMLoadFloat4x4(&wtc);
		frame->viewProjUnjittered = worldToClip;

		// Build the projection from the view frustum (Creation Engine convention:
		// positive X/Y scales, M34=1, M44=0, matching TryGetVerticalFOVFromProjection).
		const auto& f = cam->GetRuntimeData2().viewFrustum;
		const float r = f.fRight, t = f.fTop, n = f.fNear, farp = f.fFar;
		DirectX::XMMATRIX proj = DirectX::XMMatrixIdentity();
		if (std::abs(r) > 1e-6f && std::abs(t) > 1e-6f && farp > n && n > 0.0f) {
			DirectX::XMFLOAT4X4 p{};
			p._11 = 1.0f / r;
			p._22 = 1.0f / t;
			p._33 = farp / (farp - n);
			p._34 = 1.0f;
			p._43 = -n * farp / (farp - n);
			p._44 = 0.0f;
			proj = DirectX::XMLoadFloat4x4(&p);
		}
		frame->projMat = proj;

		// view = viewProj * inverse(proj), so that GetCameraProjection recovers a
		// self-consistent cameraViewToClip == proj. (Convention to validate in-game.)
		DirectX::XMVECTOR det{};
		const auto invProj = DirectX::XMMatrixInverse(&det, proj);
		frame->viewMat = DirectX::XMMatrixMultiply(worldToClip, invProj);

		const auto& w = cam->world;  // NiTransform
		frame->position = { w.translate.x, w.translate.y, w.translate.z };
		frame->cameraNear = n;
		frame->cameraFar = farp;
		frame->referenceCamera = cam;
		frame->useJitter = false;  // jitter is supplied separately to Streamline
		frame->valid = true;

		FrameState::GetSingleton()->frameCount++;
		return true;
	}

	namespace
	{
		// Base-b radical inverse (van der Corput) -- the building block of the
		// Halton sequence. Matches the FidelityFX FSR2 reference implementation.
		float Halton(std::int32_t a_index, std::int32_t a_base)
		{
			float f = 1.0f;
			float result = 0.0f;
			for (std::int32_t i = a_index; i > 0;) {
				f /= static_cast<float>(a_base);
				result += f * static_cast<float>(i % a_base);
				i = static_cast<std::int32_t>(std::floor(static_cast<float>(i) / static_cast<float>(a_base)));
			}
			return result;
		}
	}

	std::int32_t GetJitterPhaseCount(std::int32_t a_renderWidth, std::int32_t a_displayWidth)
	{
		if (a_renderWidth <= 0) {
			return 8;
		}
		constexpr float basePhaseCount = 8.0f;
		const float ratio = static_cast<float>(a_displayWidth) / static_cast<float>(a_renderWidth);
		const auto phaseCount = static_cast<std::int32_t>(basePhaseCount * ratio * ratio);
		return phaseCount < 1 ? 1 : phaseCount;
	}

	void GetJitterOffset(float& a_outX, float& a_outY, std::int32_t a_index, std::int32_t a_phaseCount)
	{
		if (a_phaseCount < 1) {
			a_phaseCount = 1;
		}
		const std::int32_t i = (a_index % a_phaseCount) + 1;
		a_outX = Halton(i, 2) - 0.5f;
		a_outY = Halton(i, 3) - 0.5f;
	}

	const CameraFrame* GetWorldCameraStateData()
	{
		auto* frame = CameraFrame::GetSingleton();
		return (frame && frame->valid) ? frame : nullptr;
	}

	CameraProjection GetCameraProjection()
	{
		CameraProjection result{};
		result.cameraState = GetWorldCameraStateData();
		if (!result.cameraState) {
			return result;
		}

		const auto& frame = *result.cameraState;
		const auto view = frame.viewMat;
		const auto viewProjUnjittered = frame.viewProjUnjittered;

		DirectX::XMVECTOR determinant{};
		const auto cameraToWorld = DirectX::XMMatrixInverse(&determinant, view);
		const auto det = DirectX::XMVectorGetX(determinant);
		if (std::isfinite(det) && det != 0.0f) {
			// Test both multiplication orders so this stays correct whether the
			// hook feeds row-major or transposed data.
			const auto rowMajorProjection = DirectX::XMMatrixMultiply(cameraToWorld, viewProjUnjittered);
			const auto transposedProjection = DirectX::XMMatrixMultiply(viewProjUnjittered, cameraToWorld);
			if (TrySetCameraProjection(result, rowMajorProjection) ||
				TrySetCameraProjection(result, transposedProjection)) {
				return result;
			}
		}

		// Fall back to the frustum-derived projection. projMat is built purely from
		// the view frustum (fNear/fFar/fRight/fTop) and is inherently UNJITTERED
		// even on frames where we inject render jitter -- the jitter reaches
		// Streamline separately via jitterOffset -- so it is the correct, safe
		// source of cameraViewToClip on jittered frames too. (This fallback is what
		// supplied a valid projection before jitter was enabled; the matrix-derived
		// paths above are best-effort and routinely fail the strict perspective
		// validation, so do NOT gate this on useJitter.)
		TrySetCameraProjection(result, frame.projMat);
		return result;
	}

	bool TryGetCameraBasis(const CameraFrame& a_frame, CameraBasis& a_basis)
	{
		DirectX::XMVECTOR determinant{};
		const auto cameraToWorld = DirectX::XMMatrixInverse(&determinant, a_frame.viewMat);
		const auto det = DirectX::XMVectorGetX(determinant);
		if (!std::isfinite(det) || det == 0.0f) {
			return false;
		}

		DirectX::XMFLOAT4X4 matrix{};
		DirectX::XMStoreFloat4x4(&matrix, cameraToWorld);

		CameraBasis rows{
			{ matrix._11, matrix._12, matrix._13 },
			{ matrix._21, matrix._22, matrix._23 },
			{ matrix._31, matrix._32, matrix._33 }
		};
		CameraBasis columns{
			{ matrix._11, matrix._21, matrix._31 },
			{ matrix._12, matrix._22, matrix._32 },
			{ matrix._13, matrix._23, matrix._33 }
		};

		const auto rowsValid = Orthonormalize(rows);
		const auto columnsValid = Orthonormalize(columns);
		if (rowsValid) {
			a_basis = rows;
			return true;
		}
		if (columnsValid) {
			a_basis = columns;
			return true;
		}
		return false;
	}

	ID3D11DeviceChild* CompileShader(const wchar_t* FilePath, const std::vector<std::pair<const char*, const char*>>& Defines, const char* ProgramType, const char* Program)
	{
		auto* device = Game::GetD3D11Device();
		if (!device) {
			logger::error("[Util] CompileShader called before the D3D11 device was captured");
			return nullptr;
		}

		std::vector<D3D_SHADER_MACRO> macros;
		for (auto& i : Defines) {
			macros.push_back({ i.first, i.second });
		}
		macros.push_back({ nullptr, nullptr });

		const uint32_t flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;

		winrt::com_ptr<ID3DBlob> shaderBlob;
		winrt::com_ptr<ID3DBlob> shaderErrors;

		std::string str;
		std::wstring path{ FilePath };
		std::transform(path.begin(), path.end(), std::back_inserter(str), [](wchar_t c) {
			return static_cast<char>(c);
		});
		if (!std::filesystem::exists(FilePath)) {
			logger::error("Failed to compile shader; {} does not exist", str);
			return nullptr;
		}
		if (FAILED(D3DCompileFromFile(FilePath, macros.data(), D3D_COMPILE_STANDARD_FILE_INCLUDE, Program, ProgramType, flags, 0, shaderBlob.put(), shaderErrors.put()))) {
			logger::warn("Shader compilation failed:\n\n{}", shaderErrors ? static_cast<char*>(shaderErrors->GetBufferPointer()) : "Unknown error");
			return nullptr;
		}
		if (shaderErrors) {
			logger::debug("Shader logs:\n{}", static_cast<char*>(shaderErrors->GetBufferPointer()));
		}

		if (!_stricmp(ProgramType, "ps_5_0")) {
			ID3D11PixelShader* regShader = nullptr;
			device->CreatePixelShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr, &regShader);
			return regShader;
		} else if (!_stricmp(ProgramType, "vs_5_0")) {
			ID3D11VertexShader* regShader = nullptr;
			device->CreateVertexShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr, &regShader);
			return regShader;
		} else if (!_stricmp(ProgramType, "hs_5_0")) {
			ID3D11HullShader* regShader = nullptr;
			device->CreateHullShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr, &regShader);
			return regShader;
		} else if (!_stricmp(ProgramType, "ds_5_0")) {
			ID3D11DomainShader* regShader = nullptr;
			device->CreateDomainShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr, &regShader);
			return regShader;
		} else if (!_stricmp(ProgramType, "cs_5_0") || !_stricmp(ProgramType, "cs_4_0")) {
			ID3D11ComputeShader* regShader = nullptr;
			DX::ThrowIfFailed(device->CreateComputeShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr, &regShader));
			return regShader;
		}

		return nullptr;
	}
}
