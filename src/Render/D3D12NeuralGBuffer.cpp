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

	// Descriptor slots in srvHeap. Each pass binds a contiguous pair, because the
	// shared root signature declares a two-SRV table; the normals pass simply
	// ignores its second entry.
	constexpr std::uint32_t kSRVNormalsDepth = 0;
	constexpr std::uint32_t kSRVNormalsUnused = 1;
	constexpr std::uint32_t kSRVUpliftMotion = 2;
	constexpr std::uint32_t kSRVUpliftDepth = 3;
	constexpr std::uint32_t kSRVUpliftEncodeSrc = 4;
	constexpr std::uint32_t kSRVUpliftDecodeSrc = 6;
	constexpr std::uint32_t kSRVUpliftDiffSrc = 8;  // t0 = before, t1 = after
	constexpr std::uint32_t kSRVCount = 10;

	// Descriptor slots in rtvHeap.
	constexpr std::uint32_t kRTVNormalRoughness = 0;
	constexpr std::uint32_t kRTVAlbedo = 1;
	constexpr std::uint32_t kRTVSpecularAlbedo = 2;
	constexpr std::uint32_t kRTVUpliftMotion = 3;
	constexpr std::uint32_t kRTVUpliftDepth = 4;
	constexpr std::uint32_t kRTVUpliftEncoded = 5;
	constexpr std::uint32_t kRTVUpliftDecodeDst = 6;
	constexpr std::uint32_t kRTVCount = 7;

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

	// Colour encode / decode around the uplift. The model was trained on a
	// defined encoding with a known diffuse-white level; Skyrim's scene colour is
	// unbounded linear HDR with neither, so normalise into that space and undo it
	// afterwards. Decode is the exact inverse, so the round trip is lossless for
	// anything the model leaves untouched.
	const char* const kUpliftCodecSource = R"(
Texture2D<float4> Source : register(t0);

cbuffer Params : register(b0)
{
	float4 Codec;  // x = decode?, y = encoding, z = diffuse white scale, w = unused
};

struct PSInput
{
	float4 position : SV_POSITION;
};

PSInput VSMain(uint vertexId : SV_VertexID)
{
	static const float2 positions[3] = {
		float2(-1.0f,  3.0f),
		float2(-1.0f, -1.0f),
		float2( 3.0f, -1.0f)
	};
	PSInput output;
	output.position = float4(positions[vertexId], 0.0f, 1.0f);
	return output;
}

float3 LinearToSRGB(float3 c)
{
	c = saturate(c);
	return c <= 0.0031308f ? c * 12.92f : 1.055f * pow(c, 1.0f / 2.4f) - 0.055f;
}

float3 SRGBToLinear(float3 c)
{
	c = saturate(c);
	return c <= 0.04045f ? c / 12.92f : pow((c + 0.055f) / 1.055f, 2.4f);
}

// BT.2100 PQ (SMPTE ST 2084), normalised so 1.0 is 10000 nits.
float3 LinearToPQ(float3 c)
{
	const float m1 = 0.1593017578125f;
	const float m2 = 78.84375f;
	const float c1 = 0.8359375f;
	const float c2 = 18.8515625f;
	const float c3 = 18.6875f;
	const float3 y = pow(max(c, 0.0f), m1);
	return pow((c1 + c2 * y) / (1.0f + c3 * y), m2);
}

float3 PQToLinear(float3 c)
{
	const float m1 = 0.1593017578125f;
	const float m2 = 78.84375f;
	const float c1 = 0.8359375f;
	const float c2 = 18.8515625f;
	const float c3 = 18.6875f;
	const float3 e = pow(max(c, 0.0f), 1.0f / m2);
	return pow(max(e - c1, 0.0f) / (c2 - c3 * e), 1.0f / m1);
}

float4 PSMain(PSInput input) : SV_TARGET
{
	const float4 source = Source.Load(int3(int2(input.position.xy), 0));
	const bool  decode = Codec.x > 0.5f;
	const uint  encoding = (uint)Codec.y;
	const float whiteScale = max(Codec.z, 1e-6f);

	float3 c = source.rgb;
	if (!decode) {
		// Scene linear -> the model's space. whiteScale maps the scene value that
		// means "diffuse white" onto 1.0 (or onto its nits fraction, for PQ).
		c = max(c, 0.0f) * whiteScale;
		if (encoding == 1u) {
			c = LinearToSRGB(c);
		} else if (encoding == 2u) {
			c = LinearToPQ(c);
		}
	} else {
		if (encoding == 1u) {
			c = SRGBToLinear(c);
		} else if (encoding == 2u) {
			c = PQToLinear(c);
		}
		c = max(c, 0.0f) / whiteScale;
	}
	return float4(c, source.a);
}
)";


	// Amplified before/after difference, for judging whether the uplift is doing
	// anything at all rather than squinting at the composited frame.
	const char* const kUpliftDiffSource = R"(
Texture2D<float4> Before : register(t0);
Texture2D<float4> After  : register(t1);

cbuffer Params : register(b0)
{
	float4 Diff;  // x = amplification
};

struct PSInput
{
	float4 position : SV_POSITION;
};

PSInput VSMain(uint vertexId : SV_VertexID)
{
	static const float2 positions[3] = {
		float2(-1.0f,  3.0f),
		float2(-1.0f, -1.0f),
		float2( 3.0f, -1.0f)
	};
	PSInput output;
	output.position = float4(positions[vertexId], 0.0f, 1.0f);
	return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
	const int3 px = int3(int2(input.position.xy), 0);
	const float3 before = Before.Load(px).rgb;
	const float3 after = After.Load(px).rgb;
	// Signed difference would hide as much as it shows once amplified, so take
	// the magnitude per channel: brightness is how much the model moved a pixel.
	return float4(abs(after - before) * Diff.x, 1.0f);
}
)";

	// Resample the engine's render-resolution, jitter-rasterised motion vectors
	// and depth up to display resolution for the post-upscale uplift.
	const char* const kUpliftGuideSource = R"(
Texture2D<float2> EngineMotion : register(t0);
Texture2D<float>  EngineDepth  : register(t1);

cbuffer Params : register(b0)
{
	float4 Extent;        // xy = render extent, zw = output (display) extent
	float4 SampleOffset;  // xy = pixel offset applied when reading the raster
};

struct PSInput
{
	float4 position : SV_POSITION;
};

struct PSOutput
{
	float2 motion : SV_TARGET0;
	float  depth  : SV_TARGET1;
};

PSInput VSMain(uint vertexId : SV_VertexID)
{
	static const float2 positions[3] = {
		float2(-1.0f,  3.0f),
		float2(-1.0f, -1.0f),
		float2( 3.0f, -1.0f)
	};
	PSInput output;
	output.position = float4(positions[vertexId], 0.0f, 1.0f);
	return output;
}

PSOutput PSMain(PSInput input)
{
	// This output pixel's centre is an UNJITTERED display-space position. The
	// raster it came from was drawn jittered, so its feature sits at that
	// position plus SampleOffset -- read there, not at the naive scaled centre.
	const float2 source = (input.position.xy) * Extent.xy / Extent.zw + SampleOffset.xy;
	const int2   p = clamp(int2(floor(source)), int2(0, 0), int2(Extent.xy) - 1);

	PSOutput output;
	// The engine's motion is normalised screen space; NGX wants display pixels.
	output.motion = EngineMotion.Load(int3(p, 0)) * Extent.zw;
	output.depth = EngineDepth.Load(int3(p, 0));
	return output;
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
	upliftGuidePipeline = nullptr;
	upliftCodecPipeline = nullptr;
	upliftDiffPipeline = nullptr;
	normalRoughness = nullptr;
	albedo = nullptr;
	specularAlbedo = nullptr;
	upliftEncoded = nullptr;
	upliftResult = nullptr;
	upliftMotion = nullptr;
	upliftDepth = nullptr;
	currentWidth = 0;
	currentHeight = 0;
	decodeDestination = nullptr;
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
		// Two per pass (t0, t1); the normals pass ignores t1.
		range.NumDescriptors = 2;
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

		auto guideVertexShader = CompileShader(kUpliftGuideSource, "VSMain", "vs_5_0");
		auto guidePixelShader = CompileShader(kUpliftGuideSource, "PSMain", "ps_5_0");

		auto guidePsoDesc = psoDesc;
		guidePsoDesc.VS = { guideVertexShader->GetBufferPointer(), guideVertexShader->GetBufferSize() };
		guidePsoDesc.PS = { guidePixelShader->GetBufferPointer(), guidePixelShader->GetBufferSize() };
		guidePsoDesc.NumRenderTargets = 2;
		guidePsoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16_FLOAT;
		guidePsoDesc.RTVFormats[1] = DXGI_FORMAT_R32_FLOAT;
		ThrowIfFailed(a_device->CreateGraphicsPipelineState(&guidePsoDesc, IID_PPV_ARGS(upliftGuidePipeline.put())));

		auto codecVertexShader = CompileShader(kUpliftCodecSource, "VSMain", "vs_5_0");
		auto codecPixelShader = CompileShader(kUpliftCodecSource, "PSMain", "ps_5_0");

		auto codecPsoDesc = psoDesc;
		codecPsoDesc.VS = { codecVertexShader->GetBufferPointer(), codecVertexShader->GetBufferSize() };
		codecPsoDesc.PS = { codecPixelShader->GetBufferPointer(), codecPixelShader->GetBufferSize() };
		codecPsoDesc.NumRenderTargets = 1;
		codecPsoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
		ThrowIfFailed(a_device->CreateGraphicsPipelineState(&codecPsoDesc, IID_PPV_ARGS(upliftCodecPipeline.put())));

		auto diffVertexShader = CompileShader(kUpliftDiffSource, "VSMain", "vs_5_0");
		auto diffPixelShader = CompileShader(kUpliftDiffSource, "PSMain", "ps_5_0");
		auto diffPsoDesc = codecPsoDesc;
		diffPsoDesc.VS = { diffVertexShader->GetBufferPointer(), diffVertexShader->GetBufferSize() };
		diffPsoDesc.PS = { diffPixelShader->GetBufferPointer(), diffPixelShader->GetBufferSize() };
		ThrowIfFailed(a_device->CreateGraphicsPipelineState(&diffPsoDesc, IID_PPV_ARGS(upliftDiffPipeline.put())));

		// ---- guide textures ---------------------------------------------------
		const auto createTarget = [&](winrt::com_ptr<ID3D12Resource>& a_out, DXGI_FORMAT a_format, bool a_allowUav = false) {
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
			desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
			             (a_allowUav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);

			D3D12_CLEAR_VALUE clear{};
			clear.Format = a_format;
			ThrowIfFailed(a_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_COMMON, &clear, IID_PPV_ARGS(a_out.put())));
		};

		// Streamline's RR path may create UAVs for these guides while processing them.
		createTarget(normalRoughness, DXGI_FORMAT_R16G16B16A16_FLOAT, true);
		createTarget(albedo, DXGI_FORMAT_R8G8B8A8_UNORM, true);
		createTarget(specularAlbedo, DXGI_FORMAT_R8G8B8A8_UNORM, true);
		// Float intermediates for both codec ends: the encoded values are nominally
		// 0..1, but keeping full precision means the decode is an exact inverse.
		createTarget(upliftEncoded, DXGI_FORMAT_R16G16B16A16_FLOAT);
		// Direct NGX writes the uplift result before the codec draws from it.
		createTarget(upliftResult, DXGI_FORMAT_R16G16B16A16_FLOAT, true);
		createTarget(upliftMotion, DXGI_FORMAT_R16G16_FLOAT);
		createTarget(upliftDepth, DXGI_FORMAT_R32_FLOAT);

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
		rtv.ptr += rtvIncrement;
		a_device->CreateRenderTargetView(upliftMotion.get(), nullptr, rtv);
		rtv.ptr += rtvIncrement;
		a_device->CreateRenderTargetView(upliftDepth.get(), nullptr, rtv);
		rtv.ptr += rtvIncrement;
		a_device->CreateRenderTargetView(upliftEncoded.get(), nullptr, rtv);
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
	const auto srvIncrement = a_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	auto       srvCpu = srvHeap->GetCPUDescriptorHandleForHeapStart();
	srvCpu.ptr += static_cast<SIZE_T>(kSRVNormalsDepth) * srvIncrement;
	a_device->CreateShaderResourceView(a_depth, &srvDesc, srvCpu);
	// The root signature's table is two SRVs wide; give t1 a valid descriptor
	// even though this pass never reads it.
	srvCpu.ptr += srvIncrement;
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


bool D3D12NeuralGBuffer::RunUpliftCodec(
	ID3D12Device*              a_device,
	ID3D12GraphicsCommandList* a_commandList,
	ID3D12Resource*            a_source,
	ID3D12Resource*            a_destination,
	std::uint32_t              a_destinationRTV,
	std::uint32_t              a_sourceSRV,
	std::uint32_t              a_width,
	std::uint32_t              a_height,
	bool                       a_decode,
	UpliftEncoding             a_encoding,
	float                      a_diffuseWhiteNits)
{
	if (!a_device || !a_commandList || !a_source || !a_destination || a_width == 0 || a_height == 0) {
		return false;
	}
	if (!EnsureResources(a_device, a_width, a_height) || !upliftCodecPipeline) {
		return false;
	}

	struct Params
	{
		float codec[4];
	} params{};

	params.codec[0] = a_decode ? 1.0f : 0.0f;
	params.codec[1] = static_cast<float>(a_encoding);
	// PQ is absolute: 1.0 means 10000 nits, so diffuse white lands at its own
	// fraction of that. The relative encodings put diffuse white at 1.0, and the
	// nits value only says which of them was intended.
	params.codec[2] = a_encoding == UpliftEncoding::kPQ ? (a_diffuseWhiteNits / 10000.0f) : 1.0f;

	Transition(a_commandList, a_source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	Transition(a_commandList, a_destination, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);

	const auto srvIncrement = a_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Texture2D.MipLevels = 1;

	auto srvCpu = srvHeap->GetCPUDescriptorHandleForHeapStart();
	srvCpu.ptr += static_cast<SIZE_T>(a_sourceSRV) * srvIncrement;
	a_device->CreateShaderResourceView(a_source, &srvDesc, srvCpu);
	// The table is two SRVs wide; t1 is unread but must still be valid.
	srvCpu.ptr += srvIncrement;
	a_device->CreateShaderResourceView(a_source, &srvDesc, srvCpu);

	const auto rtvIncrement = a_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	auto       rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
	rtv.ptr += static_cast<SIZE_T>(a_destinationRTV) * rtvIncrement;
	a_commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

	ID3D12DescriptorHeap* heaps[] = { srvHeap.get() };
	a_commandList->SetDescriptorHeaps(static_cast<UINT>(std::size(heaps)), heaps);
	a_commandList->SetGraphicsRootSignature(rootSignature.get());
	a_commandList->SetPipelineState(upliftCodecPipeline.get());
	auto srvTable = srvHeap->GetGPUDescriptorHandleForHeapStart();
	srvTable.ptr += static_cast<UINT64>(a_sourceSRV) * srvIncrement;
	a_commandList->SetGraphicsRootDescriptorTable(0, srvTable);
	a_commandList->SetGraphicsRoot32BitConstants(1, 4, &params, 0);
	a_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	const D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(a_width), static_cast<float>(a_height), 0.0f, 1.0f };
	const D3D12_RECT     scissor{ 0, 0, static_cast<LONG>(a_width), static_cast<LONG>(a_height) };
	a_commandList->RSSetViewports(1, &viewport);
	a_commandList->RSSetScissorRects(1, &scissor);
	a_commandList->DrawInstanced(3, 1, 0, 0);

	Transition(a_commandList, a_destination, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
	Transition(a_commandList, a_source, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
	return true;
}

bool D3D12NeuralGBuffer::EncodeForUplift(
	ID3D12Device*              a_device,
	ID3D12GraphicsCommandList* a_commandList,
	ID3D12Resource*            a_sceneColor,
	std::uint32_t              a_width,
	std::uint32_t              a_height,
	UpliftEncoding             a_encoding,
	float                      a_diffuseWhiteNits)
{
	if (!EnsureResources(a_device, a_width, a_height)) {
		return false;
	}
	return RunUpliftCodec(a_device, a_commandList, a_sceneColor, upliftEncoded.get(),
		kRTVUpliftEncoded, kSRVUpliftEncodeSrc, a_width, a_height, false, a_encoding, a_diffuseWhiteNits);
}

bool D3D12NeuralGBuffer::DecodeFromUplift(
	ID3D12Device*              a_device,
	ID3D12GraphicsCommandList* a_commandList,
	ID3D12Resource*            a_destination,
	std::uint32_t              a_width,
	std::uint32_t              a_height,
	UpliftEncoding             a_encoding,
	float                      a_diffuseWhiteNits)
{
	if (!EnsureResources(a_device, a_width, a_height) || !upliftResult) {
		return false;
	}
	// The destination belongs to the caller, so it needs a render target view of
	// its own. It gets a dedicated slot: a command list records only a handle and
	// the GPU reads the descriptor's contents at execute time, so a slot shared
	// with the encode pass and rewritten after recording would send BOTH draws to
	// whichever resource the descriptor happened to hold last -- the decode would
	// never reach the destination, and the encoded texture would be bound as a
	// render target while NGX still had it bound for reading.
	if (decodeDestination != a_destination) {
		const auto rtvIncrement = a_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		auto       rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
		rtv.ptr += static_cast<SIZE_T>(kRTVUpliftDecodeDst) * rtvIncrement;
		a_device->CreateRenderTargetView(a_destination, nullptr, rtv);
		decodeDestination = a_destination;
	}

	return RunUpliftCodec(a_device, a_commandList, upliftResult.get(), a_destination,
		kRTVUpliftDecodeDst, kSRVUpliftDecodeSrc, a_width, a_height, true, a_encoding, a_diffuseWhiteNits);
}

bool D3D12NeuralGBuffer::RenderUpliftDifference(
	ID3D12Device*              a_device,
	ID3D12GraphicsCommandList* a_commandList,
	ID3D12Resource*            a_destination,
	std::uint32_t              a_width,
	std::uint32_t              a_height,
	float                      a_amplification)
{
	if (!a_device || !a_commandList || !a_destination || a_width == 0 || a_height == 0) {
		return false;
	}
	if (!EnsureResources(a_device, a_width, a_height) || !upliftDiffPipeline || !upliftEncoded || !upliftResult) {
		return false;
	}

	const float params[4]{ a_amplification, 0.0f, 0.0f, 0.0f };

	Transition(a_commandList, upliftEncoded.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	Transition(a_commandList, upliftResult.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	Transition(a_commandList, a_destination, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);

	const auto srvIncrement = a_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Texture2D.MipLevels = 1;

	auto srvCpu = srvHeap->GetCPUDescriptorHandleForHeapStart();
	srvCpu.ptr += static_cast<SIZE_T>(kSRVUpliftDiffSrc) * srvIncrement;
	a_device->CreateShaderResourceView(upliftEncoded.get(), &srvDesc, srvCpu);
	srvCpu.ptr += srvIncrement;
	a_device->CreateShaderResourceView(upliftResult.get(), &srvDesc, srvCpu);

	// The destination is the caller's, so it needs its own view; the decode slot
	// is free here because decoding and the difference view are exclusive.
	const auto rtvIncrement = a_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	auto       rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
	rtv.ptr += static_cast<SIZE_T>(kRTVUpliftDecodeDst) * rtvIncrement;
	a_device->CreateRenderTargetView(a_destination, nullptr, rtv);
	decodeDestination = a_destination;
	a_commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

	ID3D12DescriptorHeap* heaps[] = { srvHeap.get() };
	a_commandList->SetDescriptorHeaps(static_cast<UINT>(std::size(heaps)), heaps);
	a_commandList->SetGraphicsRootSignature(rootSignature.get());
	a_commandList->SetPipelineState(upliftDiffPipeline.get());
	auto srvTable = srvHeap->GetGPUDescriptorHandleForHeapStart();
	srvTable.ptr += static_cast<UINT64>(kSRVUpliftDiffSrc) * srvIncrement;
	a_commandList->SetGraphicsRootDescriptorTable(0, srvTable);
	a_commandList->SetGraphicsRoot32BitConstants(1, 4, params, 0);
	a_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	const D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(a_width), static_cast<float>(a_height), 0.0f, 1.0f };
	const D3D12_RECT     scissor{ 0, 0, static_cast<LONG>(a_width), static_cast<LONG>(a_height) };
	a_commandList->RSSetViewports(1, &viewport);
	a_commandList->RSSetScissorRects(1, &scissor);
	a_commandList->DrawInstanced(3, 1, 0, 0);

	Transition(a_commandList, a_destination, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
	Transition(a_commandList, upliftResult.get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
	Transition(a_commandList, upliftEncoded.get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
	return true;
}

bool D3D12NeuralGBuffer::GenerateUpliftGuides(
	ID3D12Device*              a_device,
	ID3D12GraphicsCommandList* a_commandList,
	ID3D12Resource*            a_motionVectors,
	ID3D12Resource*            a_depth,
	std::uint32_t              a_renderWidth,
	std::uint32_t              a_renderHeight,
	std::uint32_t              a_displayWidth,
	std::uint32_t              a_displayHeight,
	DirectX::XMFLOAT2          a_jitterPixels)
{
	if (!a_device || !a_commandList || !a_motionVectors || !a_depth ||
		a_renderWidth == 0 || a_renderHeight == 0 || a_displayWidth == 0 || a_displayHeight == 0) {
		return false;
	}
	if (!EnsureResources(a_device, a_displayWidth, a_displayHeight) || !upliftGuidePipeline) {
		return false;
	}

	struct Params
	{
		float extent[4];        // render w/h, display w/h
		float sampleOffset[4];  // pixel offset, then padding
	} params{};

	params.extent[0] = static_cast<float>(a_renderWidth);
	params.extent[1] = static_cast<float>(a_renderHeight);
	params.extent[2] = static_cast<float>(a_displayWidth);
	params.extent[3] = static_cast<float>(a_displayHeight);
	// The raster displaces features by the negation of the engine's jitter, so
	// that is where an unjittered output pixel has to look to find its own data.
	params.sampleOffset[0] = -a_jitterPixels.x;
	params.sampleOffset[1] = -a_jitterPixels.y;

	Transition(a_commandList, a_motionVectors, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	Transition(a_commandList, a_depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	Transition(a_commandList, upliftMotion.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
	Transition(a_commandList, upliftDepth.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);

	const auto srvIncrement = a_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	auto       srvCpu = srvHeap->GetCPUDescriptorHandleForHeapStart();
	srvCpu.ptr += static_cast<SIZE_T>(kSRVUpliftMotion) * srvIncrement;

	D3D12_SHADER_RESOURCE_VIEW_DESC motionSrv{};
	motionSrv.Format = DXGI_FORMAT_R16G16_FLOAT;
	motionSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	motionSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	motionSrv.Texture2D.MipLevels = 1;
	a_device->CreateShaderResourceView(a_motionVectors, &motionSrv, srvCpu);

	srvCpu.ptr += srvIncrement;
	D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv{};
	depthSrv.Format = DXGI_FORMAT_R32_FLOAT;
	depthSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	depthSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	depthSrv.Texture2D.MipLevels = 1;
	a_device->CreateShaderResourceView(a_depth, &depthSrv, srvCpu);

	const auto rtvIncrement = a_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	auto       rtvBase = rtvHeap->GetCPUDescriptorHandleForHeapStart();
	D3D12_CPU_DESCRIPTOR_HANDLE guideRtvs[2]{};
	guideRtvs[0] = rtvBase;
	guideRtvs[0].ptr += static_cast<SIZE_T>(kRTVUpliftMotion) * rtvIncrement;
	guideRtvs[1] = rtvBase;
	guideRtvs[1].ptr += static_cast<SIZE_T>(kRTVUpliftDepth) * rtvIncrement;
	a_commandList->OMSetRenderTargets(2, guideRtvs, FALSE, nullptr);

	ID3D12DescriptorHeap* heaps[] = { srvHeap.get() };
	a_commandList->SetDescriptorHeaps(static_cast<UINT>(std::size(heaps)), heaps);
	a_commandList->SetGraphicsRootSignature(rootSignature.get());
	a_commandList->SetPipelineState(upliftGuidePipeline.get());
	auto srvTable = srvHeap->GetGPUDescriptorHandleForHeapStart();
	srvTable.ptr += static_cast<UINT64>(kSRVUpliftMotion) * srvIncrement;
	a_commandList->SetGraphicsRootDescriptorTable(0, srvTable);
	a_commandList->SetGraphicsRoot32BitConstants(1, 8, &params, 0);
	a_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	const D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(a_displayWidth), static_cast<float>(a_displayHeight), 0.0f, 1.0f };
	const D3D12_RECT     scissor{ 0, 0, static_cast<LONG>(a_displayWidth), static_cast<LONG>(a_displayHeight) };
	a_commandList->RSSetViewports(1, &viewport);
	a_commandList->RSSetScissorRects(1, &scissor);
	a_commandList->DrawInstanced(3, 1, 0, 0);

	Transition(a_commandList, upliftMotion.get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
	Transition(a_commandList, upliftDepth.get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
	Transition(a_commandList, a_motionVectors, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
	Transition(a_commandList, a_depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
	return true;
}
