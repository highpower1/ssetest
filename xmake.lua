set_xmakever("2.9.0")

-- ---------------------------------------------------------------------------
-- Target runtimes: Steam Skyrim Special Edition 1.5.97 + Anniversary Edition
-- 1.6.x ONLY. VR and GOG are out of scope for this project, so VR runtime
-- support in CommonLibSSE-NG is disabled here. (Can be overridden on the
-- command line: `xmake f --skyrim_vr=y`.)
-- ---------------------------------------------------------------------------
set_config("skyrim_vr", false)

-- CommonLibSSE-NG as a git submodule under lib/ (see .gitmodules).
-- This unifies SE (1.5.x) and AE (1.6.x) behind one DLL via Address Library.
includes("lib/commonlibsse-ng")

set_project("SkyrimUpscaler")
set_version("0.1.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")
set_arch("x64")

add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- Same auxiliary packages the Fallout 4 backend uses. directxtk provides
-- SimpleMath; directx-headers provides d3dx12.h.
add_requires("directxtk", "directx-headers", "magic_enum", "simpleini")

target("SkyrimUpscaler")
    set_kind("shared")
    set_filename("SkyrimUpscaler.dll")

    add_deps("commonlibsse-ng")
    add_rules("commonlibsse-ng.plugin", {
        name = "SkyrimUpscaler",
        author = "",
        description = "FSR / DLSS / DLSS Frame Generation / Neural Rendering upscaler for Skyrim SE & AE"
    })

    add_packages("directxtk", "directx-headers", "magic_enum", "simpleini")

    -- =======================================================================
    -- Plugin sources. The render backend under src/Render/ is listed explicitly
    -- rather than globbed, so adding a file is a deliberate act.
    -- =======================================================================
    add_files(
        "src/main.cpp",
        "src/Diagnostics/Probe.cpp",
        "src/Game/Renderer.cpp",
        "src/Game/Util.cpp",
        -- Device capture plus the IDXGIFactory::CreateSwapChain hook that
        -- installs the D3D12 proxy swapchain (see docs/ARCHITECTURE.md).
        "src/Render/DX11Hooks.cpp",
        "src/Neural/NeuralRendering.cpp",
        -- DLSS 5 Neural Rendering ("uplift"). Ported from jarari/fo4test branch
        -- test/dlss-nr: NVIDIA ships no public contract for this feature, so the
        -- ABI is reconstructed and the snippet is driven directly through NGX.
        "src/Neural/sl_dlss_nr.cpp",
        "src/Neural/nvngx_dlss_nr_private.cpp",
        "src/Hooks/UpscalerHooks.cpp",
        "src/Settings/Settings.cpp",
        "src/Upscaler/Upscaling.cpp",
        "src/Upscaler/D3D12Upscaler.cpp",
        "src/UI/Menu.cpp",
        -- render backend: Streamline (DLSS / DLSS-G / RR) + FidelityFX (FSR)
        "src/Render/Streamline.cpp",
        "src/Render/FidelityFX.cpp",
        "src/Render/D3D12UIComposite.cpp",
        -- Device Removed Extended Data: names the GPU operation behind a lost device
        "src/Render/DeviceRemovedReport.cpp",
        -- RTX40MFG-Unlock (MIT, github.com/dashdogy/RTX40MFG-Unlock), vendored
        -- via jarari/fo4test: patches the DLSS-G provider policy that caps Ada
        -- (RTX 40) at 2x frame generation.
        "src/third_party/RTX40MFGUnlock/integration.cpp",
        "src/third_party/RTX40MFGUnlock/loader_discovery.cpp",
        "src/third_party/RTX40MFGUnlock/midpoint_fix.cpp",
        "src/third_party/RTX40MFGUnlock/dlssg_provider_policy.cpp",
        -- Ray Reconstruction guide buffers (Skyrim has no G-buffer of its own)
        "src/Render/D3D12NeuralGBuffer.cpp",
        "src/Render/TaggedTextureDebug.cpp",
        "src/Render/OSD.cpp",
        "src/Render/DX12SwapChain.cpp")

    add_headerfiles("src/**.h")
    add_headerfiles("include/**.h", "include/**.hpp")
    add_includedirs("src", "include")
    -- so the copied render backend's `#include "Upscaling.h"` finds our port
    add_includedirs("src/Upscaler")
    add_includedirs("extern/Streamline/include")
    -- NGX SDK headers: DLSS-NR has no Streamline plugin contract, so it is
    -- driven straight through NGX (src/Neural/nvngx_dlss_nr_private.*).
    add_includedirs("extern/Streamline/external/ngx-sdk/include")
    add_includedirs("extern/FidelityFX-SDK/Kits/FidelityFX/api/include")
    add_includedirs("extern/FidelityFX-SDK/Kits/FidelityFX/backend/dx12")
    add_includedirs("extern/FidelityFX-SDK/Kits/FidelityFX/upscalers/include")
    add_includedirs("extern/FidelityFX-SDK/Kits/FidelityFX/framegeneration/include")
    add_includedirs("extern/FidelityFX-SDK/Kits/FidelityFX/framegeneration/include/dx12")
    set_pcxxheader("src/PCH.h")

    -- bcrypt: RTX40MFG-Unlock SHA-256-verifies each module before patching it.
    add_links("d3d11", "d3d12", "d3dcompiler", "dxgi", "gdi32", "bcrypt")

    -- Bundled Detours (IATHook / X64::DetourFunction / DetourClassVTable).
    add_linkdirs("include/detours/Release", { public = false })
    add_links("detours")

    -- AMD FidelityFX loader (delay-loaded so its DLL is only needed at runtime).
    add_linkdirs("extern/FidelityFX-SDK/Kits/FidelityFX/signedbin", { public = false })
    add_links("amd_fidelityfx_loader_dx12", "delayimp")
    add_shflags("/DELAYLOAD:amd_fidelityfx_loader_dx12.dll", { force = true })

    add_defines("_AMD64_", "_WINDOWS", "_UNICODE")
    add_cxxflags("cl::/bigobj", "cl::/MP", "cl::/permissive-", "cl::/Zc:__cplusplus", "cl::/arch:AVX")

-- ===========================================================================
-- NeuralColorGrade -- reference neural rendering module.
-- A plain DLL (NOT an SKSE plugin) implementing the module ABI in
-- include/SkyrimUpscalerNeural.h, so the external-module loader has something
-- real to load and module authors have a working template. Build with:
--     xmake build NeuralColorGrade
-- then drop the DLL into Data/SKSE/Plugins/SkyrimUpscaler/Neural/.
-- ===========================================================================
target("NeuralColorGrade")
    set_kind("shared")
    set_filename("NeuralColorGrade.dll")

    add_files("samples/NeuralColorGrade/NeuralColorGrade.cpp")
    add_includedirs("include")

    add_links("d3d11", "d3dcompiler")

    add_defines("_AMD64_", "_WINDOWS", "NOMINMAX", "WIN32_LEAN_AND_MEAN")
    add_cxxflags("cl::/permissive-", "cl::/Zc:__cplusplus")

-- ===========================================================================
-- SkyrimUpscalerProbe -- standalone diagnostic plugin.
-- Minimal, dependency-light target that only dumps the renderer / render-target
-- layout to its own log. Build just this with:  xmake build SkyrimUpscalerProbe
-- ===========================================================================
target("SkyrimUpscalerProbe")
    set_kind("shared")
    set_filename("SkyrimUpscalerProbe.dll")

    add_deps("commonlibsse-ng")
    add_rules("commonlibsse-ng.plugin", {
        name = "SkyrimUpscalerProbe",
        author = "",
        description = "Diagnostic probe: dumps Skyrim renderer / render-target layout"
    })

    add_packages("directxtk", "directx-headers", "magic_enum")

    add_files("src/Diagnostics/ProbeMain.cpp", "src/Diagnostics/Probe.cpp")
    add_headerfiles("src/Diagnostics/Probe.h")
    add_includedirs("src", "include")
    set_pcxxheader("src/PCH.h")

    add_links("d3d11", "dxgi")

    add_defines("_AMD64_", "_WINDOWS", "_UNICODE")
    add_cxxflags("cl::/bigobj", "cl::/MP", "cl::/permissive-", "cl::/Zc:__cplusplus", "cl::/arch:AVX")
