#pragma once

#undef DEBUG

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

// ---------------------------------------------------------------------------
// CommonLibSSE-NG umbrella headers.
//
// CommonLibSSE-NG unifies Skyrim SE (1.5.x), AE (1.6.x), GOG and VR behind a
// single API. Runtime detection + Address Library make the same DLL load on
// every supported runtime, which is how this plugin satisfies the
// "works on every Skyrim version" requirement. (Ported from the Fallout 4
// CommonLibF4 headers used by the original `fo4test` project.)
// ---------------------------------------------------------------------------
#pragma warning(push)
#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"
#include "REL/Relocation.h"
#pragma warning(pop)

#include "Windows.h"

#include <string>
using namespace std::literals;

#include "detours/Detours.h"

#include "SimpleMath.h"

using float2 = DirectX::SimpleMath::Vector2;
using float3 = DirectX::SimpleMath::Vector3;
using float4 = DirectX::SimpleMath::Vector4;
using float4x4 = DirectX::SimpleMath::Matrix;
using uint = uint32_t;

#include <directx/d3dx12.h>

#include <magic_enum/magic_enum.hpp>

#include <spdlog/spdlog.h>

#define DLLEXPORT __declspec(dllexport)

// CommonLibSSE-NG ships a spdlog-backed logger under SKSE::log. The original
// project used a hand-rolled `logger` namespace forwarding to REX::Impl::Log;
// aliasing SKSE::log keeps every `logger::info(...)` call in the ported
// render backend compiling unchanged.
namespace logger = SKSE::log;

namespace stl
{
	using namespace SKSE::stl;

	template <class T>
	void write_thunk_call(std::uintptr_t a_src)
	{
		auto& trampoline = SKSE::GetTrampoline();
		T::func = trampoline.write_call<5>(a_src, T::thunk);
	}

	template <class F, size_t index, class T>
	void write_vfunc()
	{
		REL::Relocation<std::uintptr_t> vtbl{ F::VTABLE[index] };
		T::func = vtbl.write_vfunc(T::size, T::thunk);
	}

	template <class F, class T>
	void write_vfunc()
	{
		write_vfunc<F, 0, T>();
	}

	template <std::size_t idx, class T>
	void write_vfunc(REL::ID id)
	{
		REL::Relocation<std::uintptr_t> vtbl{ id };
		T::func = vtbl.write_vfunc(idx, T::thunk);
	}

	template <class T>
	void detour_thunk(REL::ID a_relId)
	{
		*(uintptr_t*)&T::func = Detours::X64::DetourFunction(a_relId.address(), (uintptr_t)&T::thunk);
	}

	template <class T>
	void detour_thunk_ignore_func(REL::ID a_relId)
	{
		std::ignore = Detours::X64::DetourFunction(a_relId.address(), (uintptr_t)&T::thunk);
	}

	template <std::size_t idx, class T>
	void detour_vfunc(void* target)
	{
		*(uintptr_t*)&T::func = Detours::X64::DetourClassVTable(*(uintptr_t*)target, &T::thunk, idx);
	}
}

namespace DX
{
	// Helper class for COM exceptions
	class com_exception : public std::exception
	{
	public:
		explicit com_exception(HRESULT hr) noexcept :
			result(hr) {}

		const char* what() const override
		{
			static char s_str[64] = {};
			sprintf_s(s_str, "Failure with HRESULT of %08X", static_cast<unsigned int>(result));
			return s_str;
		}

	private:
		HRESULT result;
	};

	// Helper utility converts D3D API failures into exceptions.
	inline void ThrowIfFailed(HRESULT hr)
	{
		if (FAILED(hr)) {
			throw com_exception(hr);
		}
	}
}

#include "Plugin.h"
