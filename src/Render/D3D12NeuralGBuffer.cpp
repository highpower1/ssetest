#include "PCH.h"

#include "Render/D3D12NeuralGBuffer.h"

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
		const auto result = D3DCompile(a_source, std::strlen(a_source), nullptr, nullptr, nullptr,
			a_entry, a_target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, shader.put(), errors.put());
		if (FAILED(result) && errors) {
			logger::warn("[NeuralGBuffer] Shader compile failed: {}", static_cast<const char*>(errors->GetBufferPointer()));
		}
		ThrowIfFailed(result);
		return shader;
	}

	void Transition(ID3D12GraphicsCommandList* a_commandList, ID3D12Resource* a_resource,
		D3D12_RESOURCE_STATES a_before, D3D12_RESOURCE_STATES a_after)
	{
		if (!a_resource || a_before == a_after) {
			return;
		}
		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = a_resource;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = a_before;
		barrier.Transition.StateAfter = a_after;
		a_commandList->ResourceBarrier(1, &barrier);
	}

	// Descriptor slots in srvHeap.
	constexpr std::uint32_t kSRVDepth = 0;
	constexpr std::uint32_t kSRVCount = 1;

	// Descriptor slots in rtvHeap.
	constexpr std::uint32_t kRTVNormalRoughness = 0;
	constexpr std::uint32_t kRTVAlbedo = 1;
	constexpr std::uint32_t kRTVSpecularAlbedo = 2;
	constexpr std::uint32_t kRTVCount = 3;

	const char* const kShaderSource = R"(
Texture2D<float> CameraZ : register(t0);

cbuffer Params : register(b0)
{
	float4x4 ViewToWorld;  // rotation part only is used
	float4   InvProj;      // xy = 1/P00, 1/P11 ; zw = 1/renderWidth, 1/renderHeight
	float4   Material;     // x = roughness, yz = render extent - 1
};

struct PSInput
{
	float4 position : SV_POSITION;
};

PSInput VSMain(uint vertexId : SV_VertexID)
{
	// Full-screen triangle -- covers the viewport with three vertices.
	static const float2 positions[3] = {
		float2(-1.0f,  3.0f),
		float2(-1.0f, -1.0f),
		float2( 3.0f, -1.0f)
	};
	PSInput output;
	output.position = float4(positions[vertexId], 0.0f, 1.0f);
	return output;
}

// Reconstruct the view-space position of a pixel from linear view-space depth.
// Pixel coordinates are relative to the render sub-rect, which sits at the
// top-left of the (display-sized) depth texture, so they index it directly.
float3 ViewPosAt(int2 px)
{
	px = clamp(px, int2(0, 0), int2(Material.y, Material.z));
	const float z = CameraZ.Load(int3(px, 0));
	float2 ndc = (px + 0.5f) * InvProj.zw * 2.0f - 1.0f;
	ndc.y = -ndc.y;
	return float3(ndc * InvProj.xy * z, z);
}

float4 PSMain(PSInput input) : SV_TARGET
{
	const int2 px = int2(input.position.xy);

	const float3 p  = ViewPosAt(px);
	const float3 pr = ViewPosAt(px + int2(1, 0));
	const float3 pl = ViewPosAt(px - int2(1, 0));
	const float3 pd = ViewPosAt(px + int2(0, 1));
	const float3 pu = ViewPosAt(px - int2(0, 1));

	// Pick the nearer neighbour on each axis so the gradient never straddles a
	// silhouette -- otherwise every object edge gets a normal pointing at nothing.
	const float3 dx = (abs(pr.z - p.z) < abs(p.z - pl.z)) ? (pr - p) : (p - pl);
	const float3 dy = (abs(pd.z - p.z) < abs(p.z - pu.z)) ? (pd - p) : (p - pu);

	float3 n = cross(dy, dx);
	const float len = length(n);
	// Degenerate gradient (flat depth, sky, or a bad tap): fall back to facing
	// the camera rather than emitting a NaN into the denoiser.
	n = len > 1e-8f ? n / len : float3(0.0f, 0.0f, -1.0f);
	// View space here is +Z away from the camera, so a visible surface faces -Z.
	n = n.z > 0.0f ? -n : n;

	const float3 worldNormal = normalize(mul((float3x3)ViewToWorld, n));
	return float4(worldNormal, Material.x);
}
)";
}

void D3D12NeuralGBuffer::Reset()
{
	device = nullptr;
	rootSignature = nullptr;
	pipelineState = nullptr;
	srvHeap = nullptr;
	rtvHeap = nullptr;
	normalRoughness = nullptr;
	albedo = nullptr;
	specularAlbedo = nullptr;
	currentWidth = 0;
	currentHeight = 0;
	constantsCleared = false;
	creationFailed = false;
}

bool D3D12NeuralGBuffer::EnsureResources(ID3D12Device* a_device, std::uint32_t a_width, std::uint32_t a_height)
{
	if (device.get() == a_device && pipelineState && currentWidth == a_width && currentHeight == a_height) {
		return true;
	}
	if (creationFailed && device.get() == a_device && currentWidth == a_width && currentHeight == a_height) {
		return false;  // do not retry a known-bad configuration every frame
	}

	Reset();
	device.copy_from(a_device);
	currentWidth = a_width;
	currentHeight = a_height;

	try {
		// ---- root signature: one SRV table (depth) + inline constants --------
		D3D12_DESCRIPTOR_RANGE range{};
		range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		range.NumDescriptors = kSRVCount;
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
		rootParameters[1].Constants.Num32BitValues = 24;  // 4x4 matrix + 2 float4s
		rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

		D3D12_ROOT_SIGNATURE_DESC rootDesc{};
		rootDesc.NumParameters = static_cast<UINT>(std::size(rootParameters));
		rootDesc.pParameters = rootParameters;
		rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

		winrt::com_ptr<ID3DBlob> rootBlob;
		winrt::com_ptr<ID3DBlob> rootError;
		ThrowIfFailed(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, rootBlob.put(), rootError.put()));
		ThrowIfFailed(a_device->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(), IID_PPV_ARGS(rootSignature.put())));

		auto vertexShader = CompileShader(kShaderSource, "VSMain", "vs_5_0");
		auto pixelShader = CompileShader(kShaderSource, "PSMain", "ps_5_0");

		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
		psoDesc.pRootSignature = rootSignature.get();
		psoDesc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
		psoDesc.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
		psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		psoDesc.DepthStencilState.DepthEnable = FALSE;
		psoDesc.DepthStencilState.StencilEnable = FALSE;
		psoDesc.SampleMask = UINT_MAX;
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		psoDesc.NumRenderTargets = 1;
		psoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
		psoDesc.SampleDesc.Count = 1;
		ThrowIfFailed(a_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(pipelineState.put())));

		// ---- guide textures ---------------------------------------------------
		const auto createTarget = [&](winrt::com_ptr<ID3D12Resource>& a_out, DXGI_FORMAT a_format) {
			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = D3D12_HEAP_TYPE_DEFAULT;

			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			desc.Width = a_width;
			desc.Height = a_height;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = a_format;
			desc.SampleDesc.Count = 1;
			desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

			D3D12_CLEAR_VALUE clear{};
			clear.Format = a_format;
			ThrowIfFailed(a_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_COMMON, &clear, IID_PPV_ARGS(a_out.put())));
		};

		createTarget(normalRoughness, DXGI_FORMAT_R16G16B16A16_FLOAT);
		createTarget(albedo, DXGI_FORMAT_R8G8B8A8_UNORM);
		createTarget(specularAlbedo, DXGI_FORMAT_R8G8B8A8_UNORM);

		// ---- descriptor heaps -------------------------------------------------
		D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
		srvHeapDesc.NumDescriptors = kSRVCount;
		srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		ThrowIfFailed(a_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(srvHeap.put())));

		D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
		rtvHeapDesc.NumDescriptors = kRTVCount;
		rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		ThrowIfFailed(a_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(rtvHeap.put())));

		const auto rtvIncrement = a_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		auto       rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
		a_device->CreateRenderTargetView(normalRoughness.get(), nullptr, rtv);
		rtv.ptr += rtvIncrement;
		a_device->CreateRenderTargetView(albedo.get(), nullptr, rtv);
		rtv.ptr += rtvIncrement;
		a_device->CreateRenderTargetView(specularAlbedo.get(), nullptr, rtv);
	} catch (const std::exception& e) {
		logger::warn("[NeuralGBuffer] Resource creation failed ({}x{}): {}", a_width, a_height, e.what());
		const auto failedWidth = currentWidth;
		const auto failedHeight = currentHeight;
		Reset();
		device.copy_from(a_device);
		currentWidth = failedWidth;
		currentHeight = failedHeight;
		creationFailed = true;
		return false;
	}

	logger::info("[NeuralGBuffer] Guide buffers created {}x{} (roughness={} albedo={} specular={})",
		a_width, a_height, kRoughness, kAlbedo, kSpecularAlbedo);
	return true;
}

bool D3D12NeuralGBuffer::Generate(
	ID3D12Device*              a_device,
	ID3D12GraphicsCommandList* a_commandList,
	ID3D12Resource*            a_depth,
	std::uint32_t              a_renderWidth,
	std::uint32_t              a_renderHeight,
	std::uint32_t              a_displayWidth,
	std::uint32_t              a_displayHeight,
	const DirectX::XMMATRIX&   a_view,
	const DirectX::XMMATRIX&   a_proj)
{
	if (!a_device || !a_commandList || !a_depth || a_renderWidth == 0 || a_renderHeight == 0 ||
		a_displayWidth == 0 || a_displayHeight == 0) {
		return false;
	}
	if (!EnsureResources(a_device, a_displayWidth, a_displayHeight)) {
		return false;
	}

	// The projection's x/y scales are what turn an NDC coordinate back into a
	// view-space ray. A zero here means the camera matrix has not been captured
	// yet -- emit nothing rather than a division by zero.
	DirectX::XMFLOAT4X4 proj{};
	DirectX::XMStoreFloat4x4(&proj, a_proj);
	if (std::abs(proj._11) < 1e-6f || std::abs(proj._22) < 1e-6f) {
		return false;
	}

	// Only the rotation matters for a normal, so the view matrix's translation is
	// irrelevant -- transpose of the rotation is its inverse.
	DirectX::XMFLOAT4X4 viewToWorld{};
	DirectX::XMStoreFloat4x4(&viewToWorld, DirectX::XMMatrixTranspose(a_view));

	struct Params
	{
		float viewToWorld[16];
		float invProj[4];
		float material[4];
	} params{};

	std::memcpy(params.viewToWorld, &viewToWorld, sizeof(params.viewToWorld));
	params.invProj[0] = 1.0f / proj._11;
	params.invProj[1] = 1.0f / proj._22;
	params.invProj[2] = 1.0f / static_cast<float>(a_renderWidth);
	params.invProj[3] = 1.0f / static_cast<float>(a_renderHeight);
	params.material[0] = kRoughness;
	params.material[1] = static_cast<float>(a_renderWidth) - 1.0f;
	params.material[2] = static_cast<float>(a_renderHeight) - 1.0f;
	params.material[3] = 0.0f;

	// The depth arrives from the shared-texture copy in COMMON; read it as a
	// pixel-shader resource and hand it back in the state everything else expects.
	Transition(a_commandList, a_depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	Transition(a_commandList, normalRoughness.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Texture2D.MipLevels = 1;
	auto srvCpu = srvHeap->GetCPUDescriptorHandleForHeapStart();
	srvCpu.ptr += static_cast<SIZE_T>(kSRVDepth) * a_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	a_device->CreateShaderResourceView(a_depth, &srvDesc, srvCpu);

	const auto rtvIncrement = a_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	auto       rtvBase = rtvHeap->GetCPUDescriptorHandleForHeapStart();

	// Albedo and specular albedo are constants; a clear is all they ever need, and
	// only once -- nothing else writes them.
	if (!constantsCleared) {
		Transition(a_commandList, albedo.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
		Transition(a_commandList, specularAlbedo.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);

		auto albedoRtv = rtvBase;
		albedoRtv.ptr += static_cast<SIZE_T>(kRTVAlbedo) * rtvIncrement;
		const float albedoClear[4] = { kAlbedo, kAlbedo, kAlbedo, 1.0f };
		a_commandList->ClearRenderTargetView(albedoRtv, albedoClear, 0, nullptr);

		auto specularRtv = rtvBase;
		specularRtv.ptr += static_cast<SIZE_T>(kRTVSpecularAlbedo) * rtvIncrement;
		const float specularClear[4] = { kSpecularAlbedo, kSpecularAlbedo, kSpecularAlbedo, 1.0f };
		a_commandList->ClearRenderTargetView(specularRtv, specularClear, 0, nullptr);

		Transition(a_commandList, albedo.get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
		Transition(a_commandList, specularAlbedo.get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
		constantsCleared = true;
	}

	auto normalRtv = rtvBase;
	normalRtv.ptr += static_cast<SIZE_T>(kRTVNormalRoughness) * rtvIncrement;
	a_commandList->OMSetRenderTargets(1, &normalRtv, FALSE, nullptr);

	ID3D12DescriptorHeap* heaps[] = { srvHeap.get() };
	a_commandList->SetDescriptorHeaps(static_cast<UINT>(std::size(heaps)), heaps);
	a_commandList->SetGraphicsRootSignature(rootSignature.get());
	a_commandList->SetPipelineState(pipelineState.get());
	a_commandList->SetGraphicsRootDescriptorTable(0, srvHeap->GetGPUDescriptorHandleForHeapStart());
	a_commandList->SetGraphicsRoot32BitConstants(1, 24, &params, 0);
	a_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	// Render only the low-res sub-rect: RR is tagged with the same extent.
	const D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(a_renderWidth), static_cast<float>(a_renderHeight), 0.0f, 1.0f };
	const D3D12_RECT     scissor{ 0, 0, static_cast<LONG>(a_renderWidth), static_cast<LONG>(a_renderHeight) };
	a_commandList->RSSetViewports(1, &viewport);
	a_commandList->RSSetScissorRects(1, &scissor);
	a_commandList->DrawInstanced(3, 1, 0, 0);

	Transition(a_commandList, normalRoughness.get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
	Transition(a_commandList, a_depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
	return true;
}
