// Liquid Glass taskbar, our side. Loads pilltap.dll into explorer, then keeps one glass window
// (accent blur marked as a pill, which our OpenGlass rounds) under each group the TAP reports.
// While this runs the TAP hides the taskbar's own background; when it exits the background returns.
// Sizes come from pills.ini next to the exe and are picked up while running.
// pillhost.exe --stop closes a running copy.
#include <windows.h>
#include <algorithm>
#include <cmath>
#include "pillshare.h"

struct ACCENT_POLICY { DWORD AccentState, AccentFlags, GradientColor, AnimationId; };
struct WINDOWCOMPOSITIONATTRIBDATA { DWORD Attrib; PVOID pvData; SIZE_T cbData; };
using SetWindowCompositionAttribute_t = BOOL(WINAPI*)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);

namespace
{
	constexpr DWORD AccentFlagPill = 0x80000000;	// OpenGlass: cut the glass to a fully rounded outline
	constexpr UINT_PTR FollowTimer = 1, UnhookTimer = 2;

	// In taskbar units (pixels at 100% scale).
	struct Settings
	{
		int height = 40;	// pill height; the taskbar is 48
		int padding = 6;	// room left and right of the icons
		int gap = 6;	// space between the tray and the clock
		bool join = false;	// tray and clock in one pill
	};

	HWND g_host{};
	HWND g_pills[PillCount]{};
	HANDLE g_mapping{};
	const PillShare* g_share{};
	DWORD g_explorer{};
	HHOOK g_hook{};
	Settings g_settings{};
	wchar_t g_ini[MAX_PATH]{};
	FILETIME g_iniTime{};
	int g_ticks{};

	// Reads pills.ini again whenever it changes, so sizes can be tuned live.
	void LoadSettings()
	{
		WIN32_FILE_ATTRIBUTE_DATA info{};
		const FILETIME time = GetFileAttributesExW(g_ini, GetFileExInfoStandard, &info) ? info.ftLastWriteTime : FILETIME{};
		if (CompareFileTime(&time, &g_iniTime) == 0) return;
		g_iniTime = time;
		const Settings defaults{};
		g_settings.height = std::clamp<int>(GetPrivateProfileIntW(L"Pills", L"Height", defaults.height, g_ini), 8, 200);
		g_settings.padding = std::clamp<int>(GetPrivateProfileIntW(L"Pills", L"Padding", defaults.padding, g_ini), 0, 100);
		g_settings.gap = std::clamp<int>(GetPrivateProfileIntW(L"Pills", L"Gap", defaults.gap, g_ini), 0, 100);
		g_settings.join = GetPrivateProfileIntW(L"Pills", L"Join", defaults.join, g_ini) != 0;
	}

	// Hands pilltap.dll to explorer through a hook on the taskbar's thread. The DLL pins itself,
	// so the hook only has to live until explorer has loaded it. A second load is a no-op.
	void Inject(HWND tray)
	{
		DWORD pid{};
		const DWORD thread = GetWindowThreadProcessId(tray, &pid);
		wchar_t dll[MAX_PATH];
		GetModuleFileNameW(nullptr, dll, MAX_PATH);
		wcscpy_s(wcsrchr(dll, L'\\') + 1, MAX_PATH - (wcsrchr(dll, L'\\') + 1 - dll), L"pilltap.dll");
		const HMODULE module = LoadLibraryW(dll);
		const auto hook = module ? reinterpret_cast<HOOKPROC>(GetProcAddress(module, "TapHook")) : nullptr;
		if (!hook || !thread) return;
		g_hook = SetWindowsHookExW(WH_CALLWNDPROC, hook, module, thread);
		SendMessageTimeoutW(tray, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 2000, nullptr);
		SetTimer(g_host, UnhookTimer, 3000, nullptr);
	}

	void Disconnect()
	{
		if (g_share) UnmapViewOfFile(g_share);
		if (g_mapping) CloseHandle(g_mapping);
		g_share = nullptr;
		g_mapping = nullptr;
	}

	bool ReadGroups(RECT (&groups)[PillCount])
	{
		if (!g_share)
		{
			g_mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, PillShareName);
			g_share = g_mapping ? static_cast<const PillShare*>(MapViewOfFile(g_mapping, FILE_MAP_READ, 0, 0, sizeof(PillShare))) : nullptr;
			if (!g_share) { Disconnect(); return false; }
		}
		for (int attempt = 0; attempt < 8; ++attempt)
		{
			const LONG before = g_share->sequence;
			if (before & 1) { YieldProcessor(); continue; }
			MemoryBarrier();
			memcpy(groups, const_cast<const RECT*>(g_share->groups), sizeof(groups));
			MemoryBarrier();
			if (g_share->sequence == before) return true;
		}
		return false;
	}

	// Turns the groups the TAP reports into pill rectangles, relative to the taskbar.
	void Layout(const RECT (&groups)[PillCount], int barWidth, double scale, RECT (&pills)[PillCount])
	{
		const auto px = [scale](int value) { return static_cast<LONG>(std::lround(value * scale)); };
		const LONG padding = px(g_settings.padding), halfGap = px(g_settings.gap) / 2;
		const auto make = [&](const RECT& group, LONG left, LONG right) -> RECT
		{
			if (group.right <= group.left) return {};
			const LONG height = std::min<LONG>(px(g_settings.height), group.bottom - group.top);
			const LONG top = (group.top + group.bottom - height) / 2;
			return { std::max<LONG>(left, 0), top, std::min<LONG>(right, barWidth), top + height };
		};
		const RECT& apps = groups[0];
		const RECT& tray = groups[1];
		const RECT& clock = groups[2];
		pills[0] = make(apps, apps.left - padding, apps.right + padding);
		if (g_settings.join || tray.right <= tray.left || clock.right <= clock.left)
		{
			RECT both{};
			UnionRect(&both, &tray, &clock);
			pills[1] = make(both, both.left - padding, both.right + padding / 2);
			pills[2] = {};
		}
		else
		{
			// The tray and the clock touch, so each gives up half the gap.
			pills[1] = make(tray, tray.left - padding, tray.right - halfGap);
			pills[2] = make(clock, clock.left + halfGap, clock.right + padding / 2);
		}
	}

	// Places the pills right under the taskbar, in its band (topmost or not), following it as it moves.
	void Follow()
	{
		if (++g_ticks % 15 == 1) LoadSettings();
		const HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
		DWORD pid{};
		if (tray) GetWindowThreadProcessId(tray, &pid);
		if (pid != g_explorer)
		{
			// Explorer (re)started: the old TAP is gone with it.
			Disconnect();
			g_explorer = pid;
			if (tray) Inject(tray);
		}

		RECT groups[PillCount]{}, pills[PillCount]{};
		RECT bar{};
		const bool shown = tray && IsWindowVisible(tray) && GetWindowRect(tray, &bar) && ReadGroups(groups);
		if (shown) Layout(groups, bar.right - bar.left, GetDpiForWindow(tray) / 96.0, pills);
		const bool topmost = tray && (GetWindowLongW(tray, GWL_EXSTYLE) & WS_EX_TOPMOST);
		HWND above = tray;
		for (UINT index = 0; index < PillCount; ++index)
		{
			const HWND pill = g_pills[index];
			const RECT& r = pills[index];
			if (r.right <= r.left)
			{
				if (IsWindowVisible(pill)) ShowWindow(pill, SW_HIDE);
				continue;
			}
			if (((GetWindowLongW(pill, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) != topmost)
				SetWindowPos(pill, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
			const RECT want{ bar.left + r.left, bar.top + r.top, bar.left + r.right, bar.top + r.bottom };
			RECT now{};
			GetWindowRect(pill, &now);
			if (!IsWindowVisible(pill) || !EqualRect(&now, &want) || GetWindow(pill, GW_HWNDPREV) != above)
				SetWindowPos(pill, above, want.left, want.top, want.right - want.left, want.bottom - want.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
			above = pill;
		}
	}

	LRESULT CALLBACK HostProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
	{
		switch (message)
		{
		case PillChanged:
			Follow();
			return 0;
		case WM_TIMER:
			if (wParam == UnhookTimer)
			{
				KillTimer(window, UnhookTimer);
				if (g_hook) UnhookWindowsHookEx(g_hook);
				g_hook = nullptr;
			}
			else Follow();
			return 0;
		case WM_CLOSE:
			DestroyWindow(window);
			return 0;
		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
		}
		return DefWindowProcW(window, message, wParam, lParam);
	}

	HWND MakePill(HINSTANCE instance)
	{
		const HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"LiquidGlassTaskbarPill", L"", WS_POPUP,
			0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
		const auto setAttribute = reinterpret_cast<SetWindowCompositionAttribute_t>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
		ACCENT_POLICY accent{ 3, AccentFlagPill, 0, 0 };
		WINDOWCOMPOSITIONATTRIBDATA data{ 19, &accent, sizeof(accent) };
		if (setAttribute) setAttribute(window, &data);
		return window;
	}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int)
{
	if (wcsstr(commandLine, L"--stop"))
	{
		if (const HWND running = FindWindowW(PillHostClass, nullptr)) PostMessageW(running, WM_CLOSE, 0, 0);
		return 0;
	}
	if (FindWindowW(PillHostClass, nullptr)) return 0;
	// Pixels everywhere: the TAP reports physical pixels and the taskbar rectangle is physical too.
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	GetModuleFileNameW(nullptr, g_ini, MAX_PATH);
	wcscpy_s(wcsrchr(g_ini, L'\\') + 1, MAX_PATH - (wcsrchr(g_ini, L'\\') + 1 - g_ini), L"pills.ini");

	WNDCLASSW type{};
	type.lpfnWndProc = HostProc;
	type.hInstance = instance;
	type.lpszClassName = PillHostClass;
	RegisterClassW(&type);
	// A hidden top-level window, so the TAP can find it by class.
	g_host = CreateWindowExW(WS_EX_TOOLWINDOW, PillHostClass, L"Liquid Glass taskbar", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);

	WNDCLASSW pillType{};
	pillType.lpfnWndProc = DefWindowProcW;
	pillType.hInstance = instance;
	pillType.lpszClassName = L"LiquidGlassTaskbarPill";
	// Black is fully transparent under the accent, so only the glass shows.
	pillType.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
	RegisterClassW(&pillType);
	for (auto& pill : g_pills) pill = MakePill(instance);

	// The taskbar can move (auto-hide), so it is followed often; the TAP's message covers layout changes.
	SetTimer(g_host, FollowTimer, 33, nullptr);
	Follow();

	MSG message;
	while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
	if (g_hook) UnhookWindowsHookEx(g_hook);
	Disconnect();
	return 0;
}
