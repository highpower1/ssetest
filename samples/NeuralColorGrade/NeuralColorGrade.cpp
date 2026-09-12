// ===========================================================================
// NeuralColorGrade -- reference SkyrimUpscaler neural rendering module
//
// This is a TEMPLATE, not a neural network. It exists to prove the module
// pipeline end to end and to show authors the shape of a correct module: how to
// take the hud-less scene colour before upscaling, run a full-screen pass over
// it, and hand the device context back to the engine exactly as it was found.
// Swap PSMain for your own inference / post-process and the surrounding code is
// unchanged.
//
// What it actually does is a RenoDX-style grade -- exposure, contrast,
// saturation and temperature -- read from NeuralColorGrade.ini next to the DLL,
// so you can see at a glance whether the module loaded and ran.
//
// Build: xmake build NeuralColorGrade
// Install: drop the DLL (and optionally the .ini) into
//          Data/SKSE/Plugins/SkyrimUpscaler/Neural/
// ===========================================================================

#include "SkyrimUpscalerNeural.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#pragma comment(lib, "d3dcompiler.lib")

namespace
{
	// ---- settings ---------------------------------------------------------
	struct GradeSettings
	{
		float exposure = 1.0f;     // linear multiplier
		float contrast = 1.0f;     // 1.0 = unchanged, pivoted around mid grey
		float saturation = 1.0f;   // 1.0 = unchanged, 0 = greyscale
		float temperature = 0.0f;  // -1 cool .. +1 warm
	};

	// Must match the cbuffer in the shader (16-byte aligned).
	struct GradeConstants
	{
		float exposure;
		float contrast;
		float saturation;
		float temperature;
	};

	// ---- module state -----------------------------------------------------
	ID3D11Device*        g_device = nullptr;
	ID3D11DeviceContext* g_context = nullptr;
	GradeSettings        g_settings;

	ID3D11VertexShader* g_vertexShader = nullptr;
	ID3D11PixelShader*  g_pixelShader = nullptr;
	ID3D11SamplerState* g_sampler = nullptr;
	ID3D11Buffer*       g_constantBuffer = nullptr;

	// Scratch copy of the scene colour: the engine has `color` bound as a render
	// target, so it cannot also be read as a shader resource in the same pass.
	ID3D11Texture2D*          g_scratch = nullptr;
	ID3D11ShaderResourceView* g_scratchSRV = nullptr;
	ID3D11RenderTargetView*   g_colorRTV = nullptr;
	ID3D11Texture2D*          g_colorForRTV = nullptr;  // which texture g_colorRTV belongs to
	UINT                      g_scratchWidth = 0;
	UINT                      g_scratchHeight = 0;
	DXGI_FORMAT               g_scratchFormat = DXGI_FORMAT_UNKNOWN;

	bool g_disabled = false;  // a hard failure switches the module off rather than repeating it

	const char* const kShaderSource = R"(
Texture2D    SceneColor : register(t0);
SamplerState PointClamp : register(s0);

cbuffer Grade : register(b0)
{
	float Exposure;
	float Contrast;
	float Saturation;
	float Temperature;
};

struct PSInput
{
	float4 position : SV_POSITION;
	float2 uv       : TEXCOORD0;
};

PSInput VSMain(uint vertexId : SV_VertexID)
{
	// Full-screen triangle: no vertex or index buffer needed, which is also why
	// this module never touches the engine's IA bindings beyond topology.
	float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
	PSInput output;
	output.uv = uv;
	output.position = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
	return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
	float4 source = SceneColor.SampleLevel(PointClamp, input.uv, 0);
	float3 color = source.rgb;

	color *= Exposure;

	// Contrast around mid grey, in the scene's own (linear HDR) space.
	const float pivot = 0.18f;
	color = (color - pivot) * Contrast + pivot;

	const float luma = dot(color, float3(0.2126f, 0.7152f, 0.0722f));
	color = lerp(luma.xxx, color, Saturation);

	// Warm pushes red and pulls blue; cool does the opposite.
	color.r *= 1.0f + Temperature * 0.10f;
	color.b *= 1.0f - Temperature * 0.10f;

	// The scene target is float HDR, so only clamp away negatives.
	return float4(max(color, 0.0f), source.a);
}
)";

	template <typename T>
	void SafeRelease(T*& a_object)
	{
		if (a_object) {
			a_object->Release();
			a_object = nullptr;
		}
	}

	void LoadSettings(const wchar_t* a_pluginDirectory)
	{
		if (!a_pluginDirectory) {
			return;
		}
		std::wstring path = a_pluginDirectory;
		if (!path.empty() && path.back() != L'\\' && path.back() != L'/') {
			path += L'\\';
		}
		path += L"Neural\\NeuralColorGrade.ini";

		const auto readFloat = [&](const wchar_t* a_key, float a_fallback) {
			wchar_t buffer[64]{};
			// A missing file or key just leaves the default in place.
			if (GetPrivateProfileStringW(L"Grade", a_key, L"", buffer, static_cast<DWORD>(std::size(buffer)), path.c_str()) == 0) {
				return a_fallback;
			}
			try {
				return std::stof(buffer);
			} catch (...) {
				return a_fallback;
			}
		};

		g_settings.exposure = readFloat(L"Exposure", g_settings.exposure);
		g_settings.contrast = readFloat(L"Contrast", g_settings.contrast);
		g_settings.saturation = readFloat(L"Saturation", g_settings.saturation);
		g_settings.temperature = std::clamp(readFloat(L"Temperature", g_settings.temperature), -1.0f, 1.0f);
	}

	bool EnsureScratch(ID3D11Texture2D* a_color)
	{
		D3D11_TEXTURE2D_DESC desc{};
		a_color->GetDesc(&desc);

		if (g_scratch && g_scratchWidth == desc.Width && g_scratchHeight == desc.Height && g_scratchFormat == desc.Format) {
			// Resolution is stable; only the render target view can have moved.
		} else {
			SafeRelease(g_scratchSRV);
			SafeRelease(g_scratch);

			D3D11_TEXTURE2D_DESC scratchDesc = desc;
			scratchDesc.MipLevels = 1;
			scratchDesc.ArraySize = 1;
			scratchDesc.Usage = D3D11_USAGE_DEFAULT;
			scratchDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			scratchDesc.CPUAccessFlags = 0;
			scratchDesc.MiscFlags = 0;
			if (FAILED(g_device->CreateTexture2D(&scratchDesc, nullptr, &g_scratch))) {
				return false;
			}

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = desc.Format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;
			if (FAILED(g_device->CreateShaderResourceView(g_scratch, &srvDesc, &g_scratchSRV))) {
				SafeRelease(g_scratch);
				return false;
			}

			g_scratchWidth = desc.Width;
			g_scratchHeight = desc.Height;
			g_scratchFormat = desc.Format;
		}

		// The engine can hand us a different colour texture (render target swap),
		// so key the cached RTV on the exact resource it was made from.
		if (!g_colorRTV || g_colorForRTV != a_color) {
			SafeRelease(g_colorRTV);
			D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
			rtvDesc.Format = desc.Format;
			rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			if (FAILED(g_device->CreateRenderTargetView(a_color, &rtvDesc, &g_colorRTV))) {
				return false;
			}
			g_colorForRTV = a_color;
		}

		return true;
	}

	// The engine keeps drawing with this context the instant Evaluate returns, so
	// every binding this module touches is captured here and put back afterwards.
	struct SavedState
	{
		ID3D11RenderTargetView*   renderTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView*   depthStencil = nullptr;
		D3D11_VIEWPORT            viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
		UINT                      viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		D3D11_RECT                scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
		UINT                      scissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		ID3D11VertexShader*       vertexShader = nullptr;
		ID3D11PixelShader*        pixelShader = nullptr;
		ID3D11GeometryShader*     geometryShader = nullptr;
		ID3D11HullShader*         hullShader = nullptr;
		ID3D11DomainShader*       domainShader = nullptr;
		ID3D11InputLayout*        inputLayout = nullptr;
		D3D11_PRIMITIVE_TOPOLOGY  topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		ID3D11BlendState*         blendState = nullptr;
		FLOAT                     blendFactor[4]{};
		UINT                      sampleMask = 0;
		ID3D11DepthStencilState*  depthStencilState = nullptr;
		UINT                      stencilRef = 0;
		ID3D11RasterizerState*    rasterizerState = nullptr;
		ID3D11ShaderResourceView* pixelSRV = nullptr;
		ID3D11SamplerState*       pixelSampler = nullptr;
		ID3D11Buffer*             pixelConstantBuffer = nullptr;

		void Capture(ID3D11DeviceContext* a_context)
		{
			a_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, renderTargets, &depthStencil);
			a_context->RSGetViewports(&viewportCount, viewports);
			a_context->RSGetScissorRects(&scissorCount, scissors);
			a_context->VSGetShader(&vertexShader, nullptr, nullptr);
			a_context->PSGetShader(&pixelShader, nullptr, nullptr);
			a_context->GSGetShader(&geometryShader, nullptr, nullptr);
			a_context->HSGetShader(&hullShader, nullptr, nullptr);
			a_context->DSGetShader(&domainShader, nullptr, nullptr);
			a_context->IAGetInputLayout(&inputLayout);
			a_context->IAGetPrimitiveTopology(&topology);
			a_context->OMGetBlendState(&blendState, blendFactor, &sampleMask);
			a_context->OMGetDepthStencilState(&depthStencilState, &stencilRef);
			a_context->RSGetState(&rasterizerState);
			a_context->PSGetShaderResources(0, 1, &pixelSRV);
			a_context->PSGetSamplers(0, 1, &pixelSampler);
			a_context->PSGetConstantBuffers(0, 1, &pixelConstantBuffer);
		}

		void Restore(ID3D11DeviceContext* a_context)
		{
			a_context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, renderTargets, depthStencil);
			a_context->RSSetViewports(viewportCount, viewports);
			a_context->RSSetScissorRects(scissorCount, scissors);
			a_context->VSSetShader(vertexShader, nullptr, 0);
			a_context->PSSetShader(pixelShader, nullptr, 0);
			a_context->GSSetShader(geometryShader, nullptr, 0);
			a_context->HSSetShader(hullShader, nullptr, 0);
			a_context->DSSetShader(domainShader, nullptr, 0);
			a_context->IASetInputLayout(inputLayout);
			a_context->IASetPrimitiveTopology(topology);
			a_context->OMSetBlendState(blendState, blendFactor, sampleMask);
			a_context->OMSetDepthStencilState(depthStencilState, stencilRef);
			a_context->RSSetState(rasterizerState);
			a_context->PSSetShaderResources(0, 1, &pixelSRV);
			a_context->PSSetSamplers(0, 1, &pixelSampler);
			a_context->PSSetConstantBuffers(0, 1, &pixelConstantBuffer);

			for (auto*& rtv : renderTargets) {
				SafeRelease(rtv);
			}
			SafeRelease(depthStencil);
			SafeRelease(vertexShader);
			SafeRelease(pixelShader);
			SafeRelease(geometryShader);
			SafeRelease(hullShader);
			SafeRelease(domainShader);
			SafeRelease(inputLayout);
			SafeRelease(blendState);
			SafeRelease(depthStencilState);
			SafeRelease(rasterizerState);
			SafeRelease(pixelSRV);
			SafeRelease(pixelSampler);
			SafeRelease(pixelConstantBuffer);
		}
	};

	// ---- ABI ---------------------------------------------------------------
	int ModuleInit(const SkyrimUpscalerNeuralHostInfo* a_host)
	{
		if (!a_host || a_host->abiVersion != SKYRIM_UPSCALER_NEURAL_ABI_V1 || !a_host->d3d11Device || !a_host->d3d11Context) {
			return 1;
		}

		g_device = a_host->d3d11Device;
		g_context = a_host->d3d11Context;
		LoadSettings(a_host->pluginDirectory);

		ID3DBlob* vertexBlob = nullptr;
		ID3DBlob* pixelBlob = nullptr;
		ID3DBlob* errors = nullptr;

		const auto cleanupBlobs = [&] {
			SafeRelease(vertexBlob);
			SafeRelease(pixelBlob);
			SafeRelease(errors);
		};

		if (FAILED(D3DCompile(kShaderSource, std::strlen(kShaderSource), nullptr, nullptr, nullptr,
				"VSMain", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vertexBlob, &errors))) {
			cleanupBlobs();
			return 2;
		}
		if (FAILED(D3DCompile(kShaderSource, std::strlen(kShaderSource), nullptr, nullptr, nullptr,
				"PSMain", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pixelBlob, &errors))) {
			cleanupBlobs();
			return 3;
		}

		HRESULT hr = g_device->CreateVertexShader(vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(), nullptr, &g_vertexShader);
		if (SUCCEEDED(hr)) {
			hr = g_device->CreatePixelShader(pixelBlob->GetBufferPointer(), pixelBlob->GetBufferSize(), nullptr, &g_pixelShader);
		}
		cleanupBlobs();
		if (FAILED(hr)) {
			return 4;
		}

		D3D11_SAMPLER_DESC samplerDesc{};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		if (FAILED(g_device->CreateSamplerState(&samplerDesc, &g_sampler))) {
			return 5;
		}

		D3D11_BUFFER_DESC bufferDesc{};
		bufferDesc.ByteWidth = sizeof(GradeConstants);
		bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
		bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(g_device->CreateBuffer(&bufferDesc, nullptr, &g_constantBuffer))) {
			return 6;
		}

		return 0;
	}

	int ModuleEvaluate(const SkyrimUpscalerNeuralFrameInfo* a_frame)
	{
		if (g_disabled || !a_frame || !a_frame->color || !g_context || !g_pixelShader) {
			return 1;
		}
		// Nothing to do when the grade is the identity -- skip the whole pass.
		if (g_settings.exposure == 1.0f && g_settings.contrast == 1.0f &&
			g_settings.saturation == 1.0f && g_settings.temperature == 0.0f) {
			return 0;
		}
		if (!EnsureScratch(a_frame->color)) {
			g_disabled = true;
			return 2;
		}

		g_context->CopyResource(g_scratch, a_frame->color);

		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(g_context->Map(g_constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			GradeConstants constants{
				g_settings.exposure,
				g_settings.contrast,
				g_settings.saturation,
				g_settings.temperature
			};
			std::memcpy(mapped.pData, &constants, sizeof(constants));
			g_context->Unmap(g_constantBuffer, 0);
		}

		SavedState saved;
		saved.Capture(g_context);

		// Only the scene render target; no depth, no blending, no vertex buffers.
		g_context->OMSetRenderTargets(1, &g_colorRTV, nullptr);
		g_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
		g_context->OMSetDepthStencilState(nullptr, 0);
		g_context->RSSetState(nullptr);

		// The pass covers only the render sub-rect: outside it the colour target
		// holds last frame's pixels, which the upscaler never reads.
		const D3D11_VIEWPORT viewport{
			0.0f, 0.0f,
			a_frame->renderWidth > 0.0f ? a_frame->renderWidth : static_cast<float>(g_scratchWidth),
			a_frame->renderHeight > 0.0f ? a_frame->renderHeight : static_cast<float>(g_scratchHeight),
			0.0f, 1.0f
		};
		g_context->RSSetViewports(1, &viewport);
		const D3D11_RECT scissor{ 0, 0, static_cast<LONG>(viewport.Width), static_cast<LONG>(viewport.Height) };
		g_context->RSSetScissorRects(1, &scissor);

		g_context->IASetInputLayout(nullptr);
		g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		g_context->VSSetShader(g_vertexShader, nullptr, 0);
		g_context->PSSetShader(g_pixelShader, nullptr, 0);
		g_context->GSSetShader(nullptr, nullptr, 0);
		g_context->HSSetShader(nullptr, nullptr, 0);
		g_context->DSSetShader(nullptr, nullptr, 0);
		g_context->PSSetShaderResources(0, 1, &g_scratchSRV);
		g_context->PSSetSamplers(0, 1, &g_sampler);
		g_context->PSSetConstantBuffers(0, 1, &g_constantBuffer);

		g_context->Draw(3, 0);

		// Unbind our SRV before restoring, or the colour texture stays bound for
		// read while the engine binds it for write.
		ID3D11ShaderResourceView* nullSRV = nullptr;
		g_context->PSSetShaderResources(0, 1, &nullSRV);

		saved.Restore(g_context);
		return 0;
	}

	void ModuleShutdown()
	{
		SafeRelease(g_constantBuffer);
		SafeRelease(g_sampler);
		SafeRelease(g_pixelShader);
		SafeRelease(g_vertexShader);
		SafeRelease(g_colorRTV);
		SafeRelease(g_scratchSRV);
		SafeRelease(g_scratch);
		g_colorForRTV = nullptr;
		g_scratchWidth = 0;
		g_scratchHeight = 0;
		g_scratchFormat = DXGI_FORMAT_UNKNOWN;
		g_device = nullptr;
		g_context = nullptr;
		g_disabled = false;
	}

	const SkyrimUpscalerNeuralModuleV1 kModule{
		sizeof(SkyrimUpscalerNeuralModuleV1),
		SKYRIM_UPSCALER_NEURAL_ABI_V1,
		"Neural Color Grade (reference module)",
		"1.0.0",
		&ModuleInit,
		&ModuleEvaluate,
		&ModuleShutdown
	};
}

extern "C" __declspec(dllexport) const SkyrimUpscalerNeuralModuleV1* SkyrimUpscalerNeural_GetModuleV1(void)
{
	return &kModule;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID)
{
	return TRUE;
}
