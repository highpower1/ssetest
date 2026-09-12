#pragma once

namespace DX11Hooks
{
	// Installs the D3D11CreateDeviceAndSwapChain IAT hook. In this foundation
	// build it captures the game's D3D11 device/context into Game::Renderer and
	// forces feature level 11_1 (required by the D3D12 interop path). As the
	// ported render backend (src/Render/) is wired in, this is where the
	// IDXGIFactory::CreateSwapChain hook that builds the D3D12 proxy swapchain
	// is installed -- see src/Render/DX11Hooks.cpp for the full Fallout 4
	// implementation kept as the porting base.
	void Install();
}
