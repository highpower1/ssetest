#pragma once

#include <DirectXMath.h>
#include <d3d12.h>
#include <winrt/base.h>

// ===========================================================================
// D3D12NeuralGBuffer  --  guide-buffer adapter for DLSS Ray Reconstruction
//
// DLSS-RR is a ray-tracing denoiser: on top of colour / depth / motion vectors
// it wants a G-buffer (world-space normals, roughness, diffuse albedo, specular
// albedo) so it can demodulate lighting before denoising. Skyrim SE is forward
// rendered and has none of those:
//
//   * no albedo buffer at all,
//   * no roughness buffer at all,
//   * kNORMAL_TAAMASK_SSRMASK holds normals, but in an undocumented packed
//     encoding mixed with the TAA and SSR masks.
//
// So we synthesise the G-buffer instead:
//
//   normals    -- reconstructed from kSAO_CAMERAZ (linear view-space Z, the
//                 same depth the upscaler already feeds DLSS, so it is known
//                 good) by differencing neighbouring view-space positions and
//                 rotating the result into world space. Geometric rather than
//                 shading normals -- no normal-map detail -- but exact, with no
//                 guesswork about an engine encoding.
//   roughness  -- a constant (fully rough). Tells RR to treat the scene as
//                 diffuse, which is the honest description of a raster frame
//                 that has no separable specular signal.
//   albedo     -- a constant white, so RR's demodulate/re-modulate round trip
//                 is the identity and cannot tint the image.
//   spec albedo-- constant black: no specular lobe for RR to reproject.
//
// With those constants RR degenerates into "denoise the colour directly", which
// is exactly what a rasterised frame needs from it -- the win is the transformer
// model, not the ray-tracing machinery.
// ===========================================================================

class D3D12NeuralGBuffer
{
public:
	static D3D12NeuralGBuffer* GetSingleton()
	{
		static D3D12NeuralGBuffer singleton;
		return &singleton;
	}

	// Fill the guide buffers for this frame. a_depth is the shared linear
	// view-space depth (R32_FLOAT) at display size, with the render-resolution
	// image in its top-left sub-rect; the outputs follow the same convention, so
	// they are tagged with the same low-res extent.
	//
	// a_view is the game's world->view matrix and a_proj its view->clip matrix.
	// Returns false (and leaves the outputs untouched) if anything is missing.
	bool Generate(
		ID3D12Device*                a_device,
		ID3D12GraphicsCommandList*   a_commandList,
		ID3D12Resource*              a_depth,
		std::uint32_t                a_renderWidth,
		std::uint32_t                a_renderHeight,
		std::uint32_t                a_displayWidth,
		std::uint32_t                a_displayHeight,
		const DirectX::XMMATRIX&     a_view,
		const DirectX::XMMATRIX&     a_proj);

	[[nodiscard]] ID3D12Resource* GetNormalRoughness() const { return normalRoughness.get(); }
	[[nodiscard]] ID3D12Resource* GetAlbedo() const { return albedo.get(); }
	[[nodiscard]] ID3D12Resource* GetSpecularAlbedo() const { return specularAlbedo.get(); }

	void Reset();

	// Material constants handed to RR. Exposed so they can be tuned from one
	// place; see the header comment for why these values.
	static constexpr float kRoughness = 1.0f;       // fully rough => diffuse
	static constexpr float kAlbedo = 1.0f;          // white => demodulation is identity
	static constexpr float kSpecularAlbedo = 0.0f;  // black => no specular lobe

private:
	bool EnsureResources(ID3D12Device* a_device, std::uint32_t a_width, std::uint32_t a_height);

	winrt::com_ptr<ID3D12Device>         device;
	winrt::com_ptr<ID3D12RootSignature>  rootSignature;
	winrt::com_ptr<ID3D12PipelineState>  pipelineState;
	winrt::com_ptr<ID3D12DescriptorHeap> srvHeap;
	winrt::com_ptr<ID3D12DescriptorHeap> rtvHeap;

	winrt::com_ptr<ID3D12Resource> normalRoughness;
	winrt::com_ptr<ID3D12Resource> albedo;
	winrt::com_ptr<ID3D12Resource> specularAlbedo;

	std::uint32_t currentWidth = 0;
	std::uint32_t currentHeight = 0;
	bool          constantsCleared = false;
	bool          creationFailed = false;
};
