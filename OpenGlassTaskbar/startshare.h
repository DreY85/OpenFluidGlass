// Shared between starttap.dll (inside StartMenuExperienceHost or SearchHost) and pillhost.exe (our app).
#pragma once
#include <windows.h>

// SearchHost runs in an app container, so these names, made there as Local\, land in its own namespace;
// pillhost opens them there (OpenContainerObject).
constexpr wchar_t StartShareName[] = L"Local\\LiquidGlassStartMenu";
constexpr wchar_t SearchShareName[] = L"Local\\LiquidGlassSearch";	// the same layout, for the Search flyout
constexpr wchar_t StartMovedName[] = L"Local\\LiquidGlassStartMenuMoved";	// set by the TAP each time it publishes a new outline
constexpr wchar_t SearchMovedName[] = L"Local\\LiquidGlassSearchMoved";
// pillhost's glass window. The TAPs clear the flyouts' acrylic only while it exists.
constexpr wchar_t StartGlassClass[] = L"LiquidGlassStartGlass";
constexpr ULONGLONG StartListenLimit = 60000;	// how long the TAP waits, in ms, for the process's XAML to start listening

struct StartShare
{
	volatile LONG sequence;	// odd while the TAP is writing
	UINT64 window;	// the flyout's CoreWindow, so the host can tell whether DWM shows it
	RECT menu;	// the flyout's outline, physical pixels on screen; empty until known
};
