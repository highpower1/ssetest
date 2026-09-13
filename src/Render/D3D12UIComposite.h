#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#include <winrt/base.h>

#include <bit>
#include <cstdint>

class D3D12UIComposite
{
public:
	// How the composite decides which pixels of the cleared target are UI, and
	// which of the intermediate images to show instead of the composite.
	struct MaskParams
	{
		uint32_t debugView = 0;  // 0 composite, 1 UI layer, 2 mask, 3 scene only, 4 pre-UI baseline
		uint32_t maskMode = 0;   // 0 linear coverage, 1 soft threshold, 2 difference from the baseline
		float    threshold = 0.10f;
		float    softness = 0.20f;
	};

	static D3D12UIComposite* GetSingleton()
	{
		static D3D12UIComposite singleton;
		return &singleton;
	}

	void Render(
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
		const MaskParams& a_mask);

private:
	bool EnsureResources(ID3D12Device* a_device, DXGI_FORMAT a_backBufferFormat, uint32_t a_descriptorSlotCount);
	void CreateSRV(ID3D12Device* a_device, ID3D12Resource* a_resource, uint32_t a_index);

	static constexpr uint32_t kSRVsPerSlot = 3;

	winrt::com_ptr<ID3D12Device> device;
	winrt::com_ptr<ID3D12RootSignature> rootSignature;
	winrt::com_ptr<ID3D12PipelineState> pipelineState;
	winrt::com_ptr<ID3D12DescriptorHeap> srvHeap;
	winrt::com_ptr<ID3D12DescriptorHeap> rtvHeap;
	DXGI_FORMAT currentBackBufferFormat = DXGI_FORMAT_UNKNOWN;
	uint32_t currentDescriptorSlotCount = 0;
};
