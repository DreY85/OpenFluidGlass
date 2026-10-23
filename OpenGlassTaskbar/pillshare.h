// Shared between pilltap.dll (inside explorer) and pillhost.exe (our app).
#pragma once
#include <windows.h>
#include <cstdio>

constexpr wchar_t PillShareName[] = L"Local\\LiquidGlassTaskbarGroups";
constexpr wchar_t PillHostClass[] = L"LiquidGlassTaskbarHost";
constexpr UINT PillCount = 3;	// apps, tray, clock
constexpr UINT PillChanged = WM_APP + 1;	// posted by the TAP to the host after it publishes
constexpr ULONGLONG TapListenLimit = 60000;	// how long the TAP waits, in ms, for explorer's XAML to start listening

// What the taskbar shows, without any styling: the host decides how big each pill is.
struct PillShare
{
	volatile LONG sequence;	// odd while the TAP is writing
	RECT groups[PillCount];	// physical pixels, relative to the taskbar window, the full taskbar across it; empty when absent
};

// Events the host creates before it hooks explorer and the TAP sets when its attach attempt ends.
// Named per explorer process, so an old explorer's TAP never answers for a new one.
inline void PillEventName(wchar_t (&name)[96], DWORD explorer, const wchar_t* what)
{
	swprintf_s(name, L"Local\\LiquidGlassTaskbarTap%lu%s", explorer, what);
}

// Settings: written by the Taskbar tab of OpenGlassGUI, read live by the host. Sizes are pixels at 100% scale.
constexpr wchar_t PillSettingsKey[] = L"Software\\OpenGlass\\Taskbar";
constexpr wchar_t PillHostPathValue[] = L"HostPath";	// where the host last ran from, so the GUI can start it
struct PillSetting { const wchar_t* name; DWORD minimum, maximum, fallback; };
constexpr PillSetting PillEnabled{ L"Enabled", 0, 1, 1 };
constexpr PillSetting PillHeight{ L"Height", 24, 48, 40 };	// the taskbar is 48
constexpr PillSetting PillPadding{ L"Padding", 0, 24, 6 };	// room left and right of the icons
constexpr PillSetting PillGap{ L"Gap", 0, 32, 6 };	// space between the tray and the clock
constexpr PillSetting PillJoin{ L"Join", 0, 1, 0 };	// tray and clock in one pill
constexpr PillSetting PillIconSize{ L"IconSize", 16, 40, 24 };	// app icons, in DIPs; the taskbar's own size is 24

inline DWORD ReadPillSetting(const PillSetting& setting)
{
	DWORD value{}, size = sizeof(value);
	if (RegGetValueW(HKEY_CURRENT_USER, PillSettingsKey, setting.name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) return setting.fallback;
	return value < setting.minimum ? setting.minimum : value > setting.maximum ? setting.maximum : value;
}
