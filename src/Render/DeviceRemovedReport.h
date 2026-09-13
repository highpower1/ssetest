#pragma once

#include <d3d12.h>

// ===========================================================================
// Device-removed diagnostics (DRED).
//
// A removed D3D12 device tells you almost nothing on its own: the reason code
// is usually DXGI_ERROR_INVALID_CALL or _HUNG, and the call that actually
// killed it has long since returned. Device Removed Extended Data records a
// breadcrumb per GPU operation and the faulting address, so the runtime can
// name the operation instead of leaving it to be guessed at.
//
// DRED lives in the D3D12 runtime, not the debug layer, so it needs neither the
// SDK layers nor the Graphics Tools feature installed.
// ===========================================================================

namespace DeviceRemovedReport
{
	// Must be called BEFORE any D3D12 device is created; DRED cannot be turned
	// on for a device that already exists.
	void Enable();

	// Also before device creation, and only when SkyrimUpscaler.ini sets
	// D3D12DebugLayer=1. DRED reports GPU faults; it cannot see an invalid call
	// the runtime rejects on the CPU, which is what makes Close fail. The debug
	// layer names those, at a real cost in frame time, so it stays opt-in.
	void EnableDebugLayerIfRequested();

	// Log and clear whatever the debug layer has queued. Does nothing when the
	// layer is off.
	void DrainMessages(ID3D12Device* a_device, const char* a_context);

	// Log the breadcrumbs and page-fault data for a device that has been
	// removed. Safe to call on a live device (it simply reports nothing) and
	// safe to call repeatedly -- only the first report per device is written.
	void Report(ID3D12Device* a_device, const char* a_context);
}
