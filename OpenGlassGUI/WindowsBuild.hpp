#pragma once
#include "pch.h"

namespace OpenGlass
{
	// The real build number. RtlGetVersion, because GetVersionEx reports what the manifest declares, not the system.
	inline DWORD WindowsBuild() noexcept
	{
		using RtlGetVersion_t = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
		static const DWORD build = []() noexcept -> DWORD
		{
			RTL_OSVERSIONINFOW info{ sizeof(info) };
			const auto rtlGetVersion = reinterpret_cast<RtlGetVersion_t>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
			return rtlGetVersion && rtlGetVersion(&info) == 0 ? info.dwBuildNumber : 0;
		}();
		return build;
	}

	// Windows 11 starts at build 22000. It brings the XAML taskbar the floating taskbar works on, and the Mica backdrop.
	inline bool IsWindows11() noexcept
	{
		return WindowsBuild() >= 22000;
	}
}
