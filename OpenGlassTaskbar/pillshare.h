// Shared between pilltap.dll (inside explorer) and pillhost.exe (our app).
#pragma once
#include <windows.h>
#include <cstdio>

constexpr wchar_t PillShareName[] = L"Local\\LiquidGlassTaskbarGroups";
constexpr wchar_t PillHostClass[] = L"LiquidGlassTaskbarHost";
constexpr UINT PillCount = 3;	// apps, tray, clock
constexpr UINT PillChanged = WM_APP + 1;	// posted by the TAP to the host after it publishes
constexpr ULONGLONG TapListenLimit = 30000;	// how long the TAP waits, in ms, for explorer's XAML to start listening

// What the taskbar shows, without any styling: the host decides how big each pill is.
struct PillShare
{
	volatile LONG sequence;	// odd while the TAP is writing
	RECT groups[PillCount];	// physical pixels, relative to the taskbar window, full taskbar height; empty when absent
};

// Events the host creates before it hooks explorer and the TAP sets when its attach attempt ends.
// Named per explorer process, so an old explorer's TAP never answers for a new one.
inline void PillEventName(wchar_t (&name)[96], DWORD explorer, const wchar_t* what)
{
	swprintf_s(name, L"Local\\LiquidGlassTaskbarTap%lu%s", explorer, what);
}
