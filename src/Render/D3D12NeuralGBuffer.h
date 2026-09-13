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

	// Guides for DLSS 5 Neural Rendering running AFTER the upscaler.
	//
	// The uplift then works on the resolved, display-resolution, unjittered image,
	// but the engine's motion vectors and depth are render-resolution and were
	// rasterised WITH jitter. Feeding them as-is misaligns every guide by the
	// sub-pixel jitter and confines the uplift to Native AA. This pass resamples
	// both to display resolution and reads each output pixel at the raster
	// position where its feature actually landed, then scales motion into display
	// pixels (which is what NGX wants).
	//
	// a_jitterPixels is the engine's current jitter in pixels; features are
	// displaced by its negation, so that is where the sampling looks.
	bool GenerateUpliftGuides(
		ID3D12Device*              a_device,
		ID3D12GraphicsCommandList* a_commandList,
		ID3D12Resource*            a_motionVectors,
		ID3D12Resource*            a_depth,
		std::uint32_t              a_renderWidth,
		std::uint32_t              a_renderHeight,
		std::uint32_t              a_displayWidth,
		std::uint32_t              a_displayHeight,
		DirectX::XMFLOAT2          a_jitterPixels);

	[[nodiscard]] ID3D12Resource* GetNormalRoughness() const { return normalRoughness.get(); }
	[[nodiscard]] ID3D12Resource* GetAlbedo() const { return albedo.get(); }
	[[nodiscard]] ID3D12Resource* GetSpecularAlbedo() const { return specularAlbedo.get(); }
	// The uplift expects colour in a defined encoding with a known diffuse-white
	// reference. Skyrim's scene colour is unbounded linear HDR with neither, so
	// it is encoded on the way in and decoded on the way out; feeding the raw
	// values leaves the model almost nothing it recognises to work with.
	enum class UpliftEncoding : std::uint32_t
	{
		kLinearBT709 = 0,  // pass through, only the diffuse-white scale applies
		kSRGB = 1,         // srgb_nonlinear
		kPQ = 2,           // hdr10_st2084 / BT.2100 PQ
	};

	// colour -> encoded. Result is GetUpliftEncoded().
	bool EncodeForUplift(
		ID3D12Device*              a_device,
		ID3D12GraphicsCommandList* a_commandList,
		ID3D12Resource*            a_sceneColor,
		std::uint32_t              a_width,
		std::uint32_t              a_height,
		UpliftEncoding             a_encoding,
		float                      a_diffuseWhiteNits);

	// encoded -> colour, written into a_destination.
	bool DecodeFromUplift(
		ID3D12Device*              a_device,
		ID3D12GraphicsCommandList* a_commandList,
		ID3D12Resource*            a_destination,
		std::uint32_t              a_width,
		std::uint32_t              a_height,
		UpliftEncoding             a_encoding,
		float                      a_diffuseWhiteNits);

	// Diffuse-white defaults matching the reference implementation.
	[[nodiscard]] static float DefaultDiffuseWhiteNits(UpliftEncoding a_encoding)
	{
		switch (a_encoding) {
		case UpliftEncoding::kPQ:
			return 250.0f;
		case UpliftEncoding::kSRGB:
		case UpliftEncoding::kLinearBT709:
		default:
			return 100.0f;
		}
	}

	[[nodiscard]] ID3D12Resource* GetUpliftEncoded() const { return upliftEncoded.get(); }
	[[nodiscard]] ID3D12Resource* GetUpliftResult() const { return upliftResult.get(); }
	[[nodiscard]] ID3D12Resource* GetUpliftMotionVectors() const { return upliftMotion.get(); }
	[[nodiscard]] ID3D12Resource* GetUpliftDepth() const { return upliftDepth.get(); }

	void Reset();

	// Material constants handed to RR. Exposed so they can be tuned from one
	// place; see the header comment for why these values.
	static constexpr float kRoughness = 1.0f;       // fully rough => diffuse
	static constexpr float kAlbedo = 1.0f;          // white => demodulation is identity
	static constexpr float kSpecularAlbedo = 0.0f;  // black => no specular lobe

private:
	// Shared body of EncodeForUplift / DecodeFromUplift.
	bool RunUpliftCodec(
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
		float                      a_diffuseWhiteNits);

	bool EnsureResources(ID3D12Device* a_device, std::uint32_t a_width, std::uint32_t a_height);

	winrt::com_ptr<ID3D12Device>         device;
	winrt::com_ptr<ID3D12RootSignature>  rootSignature;
	winrt::com_ptr<ID3D12PipelineState>  pipelineState;        // normals + roughness
	winrt::com_ptr<ID3D12PipelineState>  upliftGuidePipeline;  // resampled motion + depth
	winrt::com_ptr<ID3D12PipelineState>  upliftCodecPipeline;  // colour encode / decode
	winrt::com_ptr<ID3D12DescriptorHeap> srvHeap;
	winrt::com_ptr<ID3D12DescriptorHeap> rtvHeap;

	winrt::com_ptr<ID3D12Resource> normalRoughness;
	winrt::com_ptr<ID3D12Resource> albedo;
	winrt::com_ptr<ID3D12Resource> specularAlbedo;
	winrt::com_ptr<ID3D12Resource> upliftEncoded;
	winrt::com_ptr<ID3D12Resource> upliftResult;
	winrt::com_ptr<ID3D12Resource> upliftMotion;
	winrt::com_ptr<ID3D12Resource> upliftDepth;

	std::uint32_t currentWidth = 0;
	std::uint32_t currentHeight = 0;
	bool          constantsCleared = false;
	bool          creationFailed = false;
};
