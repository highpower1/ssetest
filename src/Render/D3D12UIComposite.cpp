#include "D3D12UIComposite.h"

#include <cstring>
#include <d3dcompiler.h>
#include <iterator>

namespace
{
	void ThrowIfFailed(HRESULT a_result)
	{
		if (FAILED(a_result)) {
			throw DX::com_exception(a_result);
		}
	}

	winrt::com_ptr<ID3DBlob> CompileShader(const char* a_source, const char* a_entry, const char* a_target)
	{
		winrt::com_ptr<ID3DBlob> shader;
		winrt::com_ptr<ID3DBlob> errors;
		const auto flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
		const auto result = D3DCompile(a_source, std::strlen(a_source), nullptr, nullptr, nullptr, a_entry, a_target, flags, 0, shader.put(), errors.put());
		if (FAILED(result) && errors) {
			logger::warn("[D3D12UIComposite] Shader compile failed: {}", static_cast<const char*>(errors->GetBufferPointer()));
		}
		ThrowIfFailed(result);
		return shader;
	}

	DXGI_FORMAT SRVFormat(DXGI_FORMAT a_format)
	{
		switch (a_format) {
		case DXGI_FORMAT_R32_TYPELESS:
			return DXGI_FORMAT_R32_FLOAT;
		case DXGI_FORMAT_R24G8_TYPELESS:
			return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
		case DXGI_FORMAT_R16_TYPELESS:
			return DXGI_FORMAT_R16_UNORM;
		default:
			return a_format;
		}
	}
}

bool D3D12UIComposite::EnsureResources(ID3D12Device* a_device, DXGI_FORMAT a_backBufferFormat, uint32_t a_descriptorSlotCount)
{
	if (!a_device || a_backBufferFormat == DXGI_FORMAT_UNKNOWN || a_descriptorSlotCount == 0) {
		return false;
	}

	if (device.get() == a_device &&
		pipelineState &&
		currentBackBufferFormat == a_backBufferFormat &&
		currentDescriptorSlotCount >= a_descriptorSlotCount) {
		return true;
	}

	device.copy_from(a_device);
	currentBackBufferFormat = a_backBufferFormat;
	currentDescriptorSlotCount = a_descriptorSlotCount;
	rootSignature = nullptr;
	pipelineState = nullptr;
	srvHeap = nullptr;
	rtvHeap = nullptr;

	try {
		D3D12_DESCRIPTOR_RANGE range{};
		range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		range.NumDescriptors = kSRVsPerSlot;
		range.BaseShaderRegister = 0;
		range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		D3D12_ROOT_PARAMETER rootParameters[2]{};
		rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		rootParameters[0].DescriptorTable.NumDescriptorRanges = 1;
		rootParameters[0].DescriptorTable.pDescriptorRanges = &range;
		rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		rootParameters[1].Constants.ShaderRegister = 0;
		rootParameters[1].Constants.RegisterSpace = 0;
		rootParameters[1].Constants.Num32BitValues = 4;
		rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

		D3D12_STATIC_SAMPLER_DESC samplers[2]{};
		samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplers[0].ShaderRegister = 0;
		samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		samplers[1] = samplers[0];
		samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
		samplers[1].ShaderRegister = 1;

		D3D12_ROOT_SIGNATURE_DESC rootDesc{};
		rootDesc.NumParameters = static_cast<UINT>(std::size(rootParameters));
		rootDesc.pParameters = rootParameters;
		rootDesc.NumStaticSamplers = static_cast<UINT>(std::size(samplers));
		rootDesc.pStaticSamplers = samplers;
		rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

		winrt::com_ptr<ID3DBlob> rootBlob;
		winrt::com_ptr<ID3DBlob> rootError;
		ThrowIfFailed(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, rootBlob.put(), rootError.put()));
		ThrowIfFailed(a_device->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(), IID_PPV_ARGS(rootSignature.put())));

		const char* shaderSource = R"(
Texture2D baseColor : register(t0);
Texture2D postUI : register(t1);
Texture2D uiBaseline : register(t2);
SamplerState linearSampler : register(s0);
SamplerState pointSampler : register(s1);

cbuffer Params : register(b0)
{
	uint  gDebugView;   // 0 composite, 1 UI layer, 2 mask, 3 scene only
	uint  gMaskMode;    // 0 linear coverage, 1 soft threshold
	float gThreshold;
	float gSoftness;
};

struct PSInput
{
	float4 position : SV_POSITION;
	float2 uv : TEXCOORD0;
};

PSInput VSMain(uint vertexId : SV_VertexID)
{
	static const float2 positions[6] = {
		float2(-1.0f,  1.0f),
		float2( 1.0f,  1.0f),
		float2(-1.0f, -1.0f),
		float2(-1.0f, -1.0f),
		float2( 1.0f,  1.0f),
		float2( 1.0f, -1.0f)
	};
	static const float2 uvs[6] = {
		float2(0.0f, 0.0f),
		float2(1.0f, 0.0f),
		float2(0.0f, 1.0f),
		float2(0.0f, 1.0f),
		float2(1.0f, 0.0f),
		float2(1.0f, 1.0f)
	};

	PSInput output;
	output.position = float4(positions[vertexId], 0.0f, 1.0f);
	output.uv = uvs[vertexId];
	return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
	const float4 base = baseColor.Sample(linearSampler, input.uv);
	const float4 afterUI = postUI.Sample(pointSampler, input.uv);

	// postUI is the game's own frame with the scene cleared to black, so a pixel
	// belongs to the UI exactly when something was drawn there -- i.e. when it is
	// not black. Judge that from colour alone.
	//
	// This used to fold afterUI.a into the coverage, which silently discarded the
	// whole base image: postUI is a copy of an opaque backbuffer, so its alpha is
	// 1 everywhere, coverage saturated to 1 everywhere, and the composite returned
	// postUI for every pixel. The upscaler's output, the Ray Reconstruction result
	// and the Neural Rendering uplift were all being computed and then thrown away
	// at this line. An opaque buffer's alpha channel carries no UI information.
	const float rgbMax = max(afterUI.r, max(afterUI.g, afterUI.b));
	const float luma = dot(afterUI.rgb, float3(0.2126f, 0.7152f, 0.0722f));
	const float coverage = max(rgbMax, luma);

	// Colour alone cannot separate the UI from whatever else was drawn onto the
	// cleared target -- ENB's post-processing and late particle passes land there
	// too, and get composited over the scene as if they were UI. Neither mask
	// below fixes that; they only move the trade-off, and the debug views exist
	// so the choice can be made by looking rather than by guessing.
	float alpha;
	if (gMaskMode == 2) {
		// The honest mask. uiBaseline is the same buffer captured before the UI
		// was drawn into it, so a pixel belongs to the UI exactly where the two
		// differ. ENB's output is present in both and cancels, which is the whole
		// point: brightness cannot tell a torch flame from a health bar, but
		// "was this pixel changed by the UI pass" can.
		const float3 delta = abs(afterUI.rgb - uiBaseline.Sample(pointSampler, input.uv).rgb);
		const float  changed = max(delta.r, max(delta.g, delta.b));
		alpha = smoothstep(gThreshold, gThreshold + max(gSoftness, 1e-4f), changed);
	} else if (gMaskMode == 1) {
		// Faint contributions are dropped outright and anything past the knee is
		// taken as fully opaque UI, instead of the linear ramp's habit of
		// half-dissolving both the UI and the scene into each other.
		alpha = smoothstep(gThreshold, gThreshold + max(gSoftness, 1e-4f), coverage);
	} else {
		alpha = coverage > 0.01f ? saturate(coverage) : 0.0f;
	}

	const float3 uiColor = saturate(afterUI.rgb);

	if (gDebugView == 1) {
		return float4(afterUI.rgb, 1.0f);          // exactly what the composite calls UI
	}
	if (gDebugView == 2) {
		return float4(alpha, alpha, alpha, 1.0f);  // white where the scene is replaced
	}
	if (gDebugView == 3) {
		return float4(base.rgb, 1.0f);             // the upscaled scene, uncomposited
	}
	if (gDebugView == 4) {
		return float4(uiBaseline.Sample(pointSampler, input.uv).rgb, 1.0f);  // the pre-UI capture
	}
	return float4(lerp(base.rgb, uiColor, alpha), 1.0f);
}
)";
		auto vertexShader = CompileShader(shaderSource, "VSMain", "vs_5_0");
		auto pixelShader = CompileShader(shaderSource, "PSMain", "ps_5_0");

		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
		psoDesc.InputLayout = {};
		psoDesc.pRootSignature = rootSignature.get();
		psoDesc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
		psoDesc.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
		psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		psoDesc.DepthStencilState.DepthEnable = FALSE;
		psoDesc.DepthStencilState.StencilEnable = FALSE;
		psoDesc.SampleMask = UINT_MAX;
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		psoDesc.NumRenderTargets = 1;
		psoDesc.RTVFormats[0] = a_backBufferFormat;
		psoDesc.SampleDesc.Count = 1;
		ThrowIfFailed(a_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(pipelineState.put())));

		D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
		srvHeapDesc.NumDescriptors = kSRVsPerSlot * currentDescriptorSlotCount;
		srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		ThrowIfFailed(a_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(srvHeap.put())));

		D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
		rtvHeapDesc.NumDescriptors = currentDescriptorSlotCount;
		rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		ThrowIfFailed(a_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(rtvHeap.put())));
	} catch (const std::exception& e) {
		logger::warn("[D3D12UIComposite] Resource creation failed: {}", e.what());
		rootSignature = nullptr;
		pipelineState = nullptr;
		currentDescriptorSlotCount = 0;
		return false;
	}

	return true;
}

void D3D12UIComposite::CreateSRV(ID3D12Device* a_device, ID3D12Resource* a_resource, uint32_t a_index)
{
	auto handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
	handle.ptr += static_cast<SIZE_T>(a_index) * a_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	const auto desc = a_resource->GetDesc();
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = SRVFormat(desc.Format);
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Texture2D.MipLevels = 1;
	a_device->CreateShaderResourceView(a_resource, &srvDesc, handle);
}

void D3D12UIComposite::Render(
	ID3D12Device* a_device,
	ID3D12GraphicsCommandList* a_commandList,
	ID3D12Resource* a_backBuffer,
	ID3D12Resource* a_baseColor,
	ID3D12Resource* a_postUI,
	ID3D12Resource* a_uiBaseline,
	DXGI_FORMAT a_backBufferFormat,
	uint32_t a_width,
	uint32_t a_height,
	uint32_t a_descriptorSlot,
	uint32_t a_descriptorSlotCount,
	const MaskParams& a_mask)
{
	if (!a_device || !a_commandList || !a_backBuffer || !a_baseColor || !a_postUI || a_width == 0 || a_height == 0) {
		return;
	}
	if (!EnsureResources(a_device, a_backBufferFormat, a_descriptorSlotCount)) {
		return;
	}
	if (a_descriptorSlot >= currentDescriptorSlotCount) {
		return;
	}

	const auto srvBaseIndex = a_descriptorSlot * kSRVsPerSlot;
	CreateSRV(a_device, a_baseColor, srvBaseIndex);
	CreateSRV(a_device, a_postUI, srvBaseIndex + 1);
	// Falls back to postUI when no baseline exists, which makes the difference
	// zero and the mask empty -- visibly wrong rather than quietly wrong.
	CreateSRV(a_device, a_uiBaseline ? a_uiBaseline : a_postUI, srvBaseIndex + 2);

	const auto rtvIncrement = a_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
	rtv.ptr += static_cast<SIZE_T>(a_descriptorSlot) * rtvIncrement;
	a_device->CreateRenderTargetView(a_backBuffer, nullptr, rtv);

	a_commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

	ID3D12DescriptorHeap* heaps[] = { srvHeap.get() };
	a_commandList->SetDescriptorHeaps(static_cast<UINT>(std::size(heaps)), heaps);
	a_commandList->SetGraphicsRootSignature(rootSignature.get());
	a_commandList->SetPipelineState(pipelineState.get());
	a_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	const auto srvIncrement = a_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	auto srvTable = srvHeap->GetGPUDescriptorHandleForHeapStart();
	srvTable.ptr += static_cast<UINT64>(srvBaseIndex) * srvIncrement;
	a_commandList->SetGraphicsRootDescriptorTable(0, srvTable);
	const uint32_t maskConstants[4]{
		a_mask.debugView,
		a_mask.maskMode,
		std::bit_cast<uint32_t>(a_mask.threshold),
		std::bit_cast<uint32_t>(a_mask.softness)
	};
	a_commandList->SetGraphicsRoot32BitConstants(1, 4, maskConstants, 0);
	D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(a_width), static_cast<float>(a_height), 0.0f, 1.0f };
	D3D12_RECT scissor{ 0, 0, static_cast<LONG>(a_width), static_cast<LONG>(a_height) };
	a_commandList->RSSetViewports(1, &viewport);
	a_commandList->RSSetScissorRects(1, &scissor);
	a_commandList->DrawInstanced(6, 1, 0, 0);
}
