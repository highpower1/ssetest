#include "PCH.h"

#include "Render/DeviceRemovedReport.h"

#include <winrt/base.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
	std::atomic<ID3D12Device*> g_reportedDevice{ nullptr };

	const char* BreadcrumbName(D3D12_AUTO_BREADCRUMB_OP a_op)
	{
		switch (a_op) {
		case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: return "SetMarker";
		case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return "BeginEvent";
		case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return "EndEvent";
		case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return "DrawInstanced";
		case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return "DrawIndexedInstanced";
		case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return "ExecuteIndirect";
		case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return "Dispatch";
		case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "CopyBufferRegion";
		case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "CopyTextureRegion";
		case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return "CopyResource";
		case D3D12_AUTO_BREADCRUMB_OP_COPYTILES: return "CopyTiles";
		case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE: return "ResolveSubresource";
		case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return "ClearRenderTargetView";
		case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW: return "ClearUnorderedAccessView";
		case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: return "ClearDepthStencilView";
		case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "ResourceBarrier";
		case D3D12_AUTO_BREADCRUMB_OP_EXECUTEBUNDLE: return "ExecuteBundle";
		case D3D12_AUTO_BREADCRUMB_OP_PRESENT: return "Present";
		case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA: return "ResolveQueryData";
		case D3D12_AUTO_BREADCRUMB_OP_BEGINSUBMISSION: return "BeginSubmission";
		case D3D12_AUTO_BREADCRUMB_OP_ENDSUBMISSION: return "EndSubmission";
		case D3D12_AUTO_BREADCRUMB_OP_DECODEFRAME: return "DecodeFrame";
		case D3D12_AUTO_BREADCRUMB_OP_PROCESSFRAMES: return "ProcessFrames";
		case D3D12_AUTO_BREADCRUMB_OP_DISPATCHRAYS: return "DispatchRays";
		case D3D12_AUTO_BREADCRUMB_OP_BUILDRAYTRACINGACCELERATIONSTRUCTURE: return "BuildRaytracingAccelerationStructure";
		default: return "op";
		}
	}

	void LogAllocation(const D3D12_DRED_ALLOCATION_NODE* a_node, const char* a_label)
	{
		for (const auto* node = a_node; node; node = node->pNext) {
			logger::critical("[DRED]   {} allocation type={} name={}",
				a_label,
				static_cast<int>(node->AllocationType),
				node->ObjectNameA ? node->ObjectNameA : "<unnamed>");
		}
	}
}

namespace
{
	bool g_debugLayerEnabled = false;

	// Read straight from the ini: the device exists long before SettingsStore is
	// loaded, so the usual settings path is not available yet.
	//
	// Parsed by hand rather than with GetPrivateProfileInt, which returned the
	// default here: the file is UTF-8 with a byte-order mark, and the profile API
	// then fails to match the leading [General] section. SimpleIni strips the
	// mark, which is why every other setting loaded fine.
	bool DebugLayerRequested()
	{
		wchar_t moduleName[MAX_PATH]{};
		HMODULE self = nullptr;
		GetModuleHandleExW(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&DebugLayerRequested), &self);
		if (!self || GetModuleFileNameW(self, moduleName, MAX_PATH) == 0) {
			return false;
		}
		const auto path = std::filesystem::path(moduleName).parent_path() /
		                  L"SkyrimUpscaler" / L"SkyrimUpscaler.ini";

		std::string text;
		{
			std::ifstream file(path, std::ios::binary);
			if (!file) {
				logger::info("[D3D12] Debug layer not requested: no ini at {}", path.string());
				return false;
			}
			text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
		}

		bool       requested = false;
		const auto key = text.find("D3D12DebugLayer");
		if (key != std::string::npos) {
			const auto equals = text.find('=', key);
			const auto lineEnd = text.find('\n', key);
			if (equals != std::string::npos && (lineEnd == std::string::npos || equals < lineEnd)) {
				const auto value = text.find_first_not_of(" \t", equals + 1);
				requested = value != std::string::npos && text[value] != '0';
			}
		}
		logger::info("[D3D12] Debug layer {} in {}", requested ? "REQUESTED" : "not requested", path.string());
		return requested;
	}
}

void DeviceRemovedReport::EnableDebugLayerIfRequested()
{
	if (!DebugLayerRequested()) {
		return;
	}
	winrt::com_ptr<ID3D12Debug> debug;
	if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.put())))) {
		logger::warn("[D3D12] Debug layer requested but unavailable; install the Graphics Tools optional feature");
		return;
	}
	debug->EnableDebugLayer();
	g_debugLayerEnabled = true;
	logger::warn("[D3D12] Debug layer ENABLED by SkyrimUpscaler.ini. This costs frame time; set D3D12DebugLayer=0 when finished.");
}

void DeviceRemovedReport::DrainMessages(ID3D12Device* a_device, const char* a_context)
{
	if (!g_debugLayerEnabled || !a_device) {
		return;
	}
	winrt::com_ptr<ID3D12InfoQueue> queue;
	if (FAILED(a_device->QueryInterface(IID_PPV_ARGS(queue.put())))) {
		return;
	}
	const auto count = queue->GetNumStoredMessages();
	for (UINT64 i = 0; i < count; ++i) {
		SIZE_T length = 0;
		if (FAILED(queue->GetMessage(i, nullptr, &length)) || length == 0) {
			continue;
		}
		std::vector<std::byte> storage(length);
		auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
		if (SUCCEEDED(queue->GetMessage(i, message, &length)) && message->pDescription) {
			logger::critical("[D3D12] {}: severity={} id={} {}",
				a_context, static_cast<int>(message->Severity),
				static_cast<int>(message->ID), message->pDescription);
		}
	}
	queue->ClearStoredMessages();
}

void DeviceRemovedReport::Enable()
{
	winrt::com_ptr<ID3D12DeviceRemovedExtendedDataSettings> settings;
	if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(settings.put())))) {
		logger::info("[DRED] Device Removed Extended Data is unavailable on this runtime");
		return;
	}
	settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
	settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
	logger::info("[DRED] Auto-breadcrumbs and page-fault reporting enabled");
}

void DeviceRemovedReport::Report(ID3D12Device* a_device, const char* a_context)
{
	if (!a_device || SUCCEEDED(a_device->GetDeviceRemovedReason())) {
		return;  // still alive; nothing to report
	}
	// Present rejects every frame once the device is gone, so report once.
	if (g_reportedDevice.exchange(a_device) == a_device) {
		return;
	}

	winrt::com_ptr<ID3D12DeviceRemovedExtendedData> dred;
	if (FAILED(a_device->QueryInterface(IID_PPV_ARGS(dred.put())))) {
		logger::critical("[DRED] {}: device removed, but DRED data is unavailable", a_context);
		return;
	}

	logger::critical("[DRED] {}: device removed, reason 0x{:08X}",
		a_context, static_cast<uint32_t>(a_device->GetDeviceRemovedReason()));

	D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs{};
	if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs))) {
		for (const auto* node = breadcrumbs.pHeadAutoBreadcrumbNode; node; node = node->pNext) {
			const auto executed = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0u;
			// Everything up to pLastBreadcrumbValue completed; the operation AT
			// that index is the one that did not, which is the one that matters.
			logger::critical("[DRED] queue='{}' list='{}' completed {} of {} operations",
				node->pCommandQueueDebugNameA ? node->pCommandQueueDebugNameA : "<unnamed>",
				node->pCommandListDebugNameA ? node->pCommandListDebugNameA : "<unnamed>",
				executed, node->BreadcrumbCount);
			if (executed >= node->BreadcrumbCount) {
				continue;  // this list finished; it is not the culprit
			}
			const auto first = executed > 3 ? executed - 3 : 0u;
			for (auto i = first; i < node->BreadcrumbCount && i < executed + 4; ++i) {
				logger::critical("[DRED]   [{}]{} {}", i, i == executed ? " <-- FAULTED HERE" : "",
					BreadcrumbName(node->pCommandHistory[i]));
			}
		}
	}

	D3D12_DRED_PAGE_FAULT_OUTPUT pageFault{};
	if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pageFault))) {
		logger::critical("[DRED] page fault at GPU virtual address 0x{:016X}", pageFault.PageFaultVA);
		LogAllocation(pageFault.pHeadExistingAllocationNode, "live");
		LogAllocation(pageFault.pHeadRecentFreedAllocationNode, "recently freed");
	}
}
