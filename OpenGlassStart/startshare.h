// Shared between starttap.dll (inside StartMenuExperienceHost or SearchHost) and startglass.exe (our app).
#pragma once
#include <windows.h>

constexpr wchar_t StartShareName[] = L"Local\\LiquidGlassStartMenu";
constexpr wchar_t SearchShareName[] = L"Local\\LiquidGlassSearch";	// the same layout, for the Search flyout

struct StartShare
{
	volatile LONG sequence;	// odd while the TAP is writing
	UINT64 window;	// the Start CoreWindow, so the host can tell whether DWM shows it
	RECT menu;	// the menu's outline, physical pixels on screen; empty until known
	LONG active;	// CoreWindow activation; logged only, as Start raised it once in a whole test
};
