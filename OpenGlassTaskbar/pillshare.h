// Shared between pilltap.dll (inside explorer) and pillhost.exe (our app).
#pragma once
#include <windows.h>

constexpr wchar_t PillShareName[] = L"Local\\LiquidGlassTaskbarGroups";
constexpr wchar_t PillHostClass[] = L"LiquidGlassTaskbarHost";
constexpr UINT PillCount = 3;	// apps, tray, clock
constexpr UINT PillChanged = WM_APP + 1;	// posted by the TAP to the host after it publishes

// What the taskbar shows, without any styling: the host decides how big each pill is.
struct PillShare
{
	volatile LONG sequence;	// odd while the TAP is writing
	RECT groups[PillCount];	// physical pixels, relative to the taskbar window, full taskbar height; empty when absent
};
