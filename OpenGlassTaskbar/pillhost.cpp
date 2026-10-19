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
	constexpr UINT_PTR FollowTimer = 1;

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

	// Rare events (attach attempts, nudges) to %TEMP%\pillhost.txt, timestamped like the TAP's log.
	void Log(const wchar_t* line)
	{
		wchar_t path[MAX_PATH];
		GetTempPathW(MAX_PATH, path);
		wcscat_s(path, L"pillhost.txt");
		FILE* file{};
		if (_wfopen_s(&file, path, L"a, ccs=UTF-8") || !file) return;
		SYSTEMTIME now{};
		GetLocalTime(&now);
		fwprintf(file, L"%02u:%02u:%02u.%03u %s\n", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, line);
		fclose(file);
	}

	// Attaching to explorer. A freshly started explorer refuses the TAP until its taskbar's XAML starts,
	// so the host loads the TAP into it once, as early as it can, and the TAP keeps trying from inside. The host waits
	// for the TAP to say how that ended and, if it failed, tries again. Every attempt loads its own copy
	// of pilltap.dll, from the tap folder next to the exe, as XAML may keep refusing a file it refused once.
	enum class TapState { Idle, Waiting, Attached };
	TapState g_tap{ TapState::Idle };
	ULONGLONG g_tapTime{};	// while idle: when to try next; while waiting: when the TAP was loaded
	HANDLE g_tapDone{}, g_tapAttached{};
	// The TAP waits for explorer's XAML itself, then makes up to four attempts of about a second each.
	constexpr ULONGLONG TapRetryDelay = 500, TapWaitLimit = TapListenLimit + 10000, NudgeDelay = 300;

	void ReleaseTap()
	{
		if (g_tapDone) CloseHandle(g_tapDone);
		if (g_tapAttached) CloseHandle(g_tapAttached);
		g_tapDone = g_tapAttached = nullptr;
	}

	// The folder next to the exe, with a trailing backslash.
	void HostFolder(wchar_t (&path)[MAX_PATH])
	{
		GetModuleFileNameW(nullptr, path, MAX_PATH);
		*(wcsrchr(path, L'\\') + 1) = L'\0';
	}

	// Copies left by earlier runs; the ones explorer still holds stay locked and are skipped.
	void CleanTapCopies()
	{
		wchar_t folder[MAX_PATH], pattern[MAX_PATH];
		HostFolder(folder);
		wcscat_s(folder, L"tap\\");
		swprintf_s(pattern, L"%s*.dll", folder);
		WIN32_FIND_DATAW found{};
		const HANDLE search = FindFirstFileW(pattern, &found);
		if (search == INVALID_HANDLE_VALUE) return;
		do
		{
			wchar_t file[MAX_PATH];
			swprintf_s(file, L"%s%s", folder, found.cFileName);
			DeleteFileW(file);
		} while (FindNextFileW(search, &found));
		FindClose(search);
	}

	// Has explorer load a copy of pilltap.dll on a thread of its own, started at LoadLibraryW, which
	// kernel32 maps at the same address in every process. A hook on the taskbar's thread would wait for
	// that thread to be free, and while explorer starts, it is busy building the taskbar for seconds.
	// The TAP drops this load's reference when its attempt ends; a successful attach pins the DLL.
	void Inject(HWND tray)
	{
		DWORD pid{};
		if (!GetWindowThreadProcessId(tray, &pid)) return;
		wchar_t folder[MAX_PATH], source[MAX_PATH], dll[MAX_PATH];
		HostFolder(folder);
		swprintf_s(source, L"%spilltap.dll", folder);
		swprintf_s(dll, L"%stap", folder);
		CreateDirectoryW(dll, nullptr);
		swprintf_s(dll, L"%stap\\pilltap-%lu-%llu.dll", folder, pid, GetTickCount64());
		if (!CopyFileW(source, dll, FALSE)) return;
		const HANDLE explorer = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE, FALSE, pid);
		if (!explorer) { Log(L"cannot open explorer"); return; }
		wchar_t name[96];
		PillEventName(name, pid, L"Done");
		g_tapDone = CreateEventW(nullptr, TRUE, FALSE, name);
		PillEventName(name, pid, L"Attached");
		g_tapAttached = CreateEventW(nullptr, TRUE, FALSE, name);
		if (g_tapDone) ResetEvent(g_tapDone);
		if (g_tapAttached) ResetEvent(g_tapAttached);
		const SIZE_T size = (wcslen(dll) + 1) * sizeof(wchar_t);
		void* remote = VirtualAllocEx(explorer, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		const auto load = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
		HANDLE thread{};
		if (remote && WriteProcessMemory(explorer, remote, dll, size, nullptr))
		{
			thread = CreateRemoteThread(explorer, nullptr, 0, load, remote, 0, nullptr);
		}
		if (thread)
		{
			// LoadLibraryW only runs DllMain, which hands the attempt to a thread of the TAP's own.
			if (WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0) VirtualFreeEx(explorer, remote, 0, MEM_RELEASE);
			CloseHandle(thread);
			g_tap = TapState::Waiting;
			g_tapTime = GetTickCount64();
		}
		else
		{
			Log(L"cannot start a thread in explorer");
			if (remote) VirtualFreeEx(explorer, remote, 0, MEM_RELEASE);
			ReleaseTap();
		}
		CloseHandle(explorer);
	}

	// XAML does not list the taskbar's existing elements to the TAP, so the TAP only finds the
	// taskbar when something on it changes. If nothing does soon after attaching, the host shows a
	// window of its own for a moment: fully transparent and off screen, but with a taskbar button,
	// whose arrival the TAP sees. The window goes away as soon as the TAP has published.
	// (ITaskbarList::AddTab on a hidden window changes nothing on the Windows 11 taskbar.)
	HWND g_nudge{};
	ULONGLONG g_nudgeTime{};
	int g_nudges{};	// since the TAP attached; a TAP that never answers would otherwise get nudged forever
	constexpr ULONGLONG NudgeLimit = 2000, NudgeRetryDelay = 5000;
	constexpr int NudgeTries = 3;

	void EndNudge()
	{
		if (g_nudge && IsWindowVisible(g_nudge)) ShowWindow(g_nudge, SW_HIDE);
	}

	void Nudge(ULONGLONG now)
	{
		if (!g_nudge) return;
		if (IsWindowVisible(g_nudge))
		{
			if (!g_share && now - g_nudgeTime < NudgeLimit) return;
			EndNudge();
			if (!g_share) g_tapTime = now + NudgeRetryDelay;
		}
		else if (!g_share && now >= g_tapTime && g_nudges < NudgeTries)
		{
			if (++g_nudges == NudgeTries) Log(L"last nudge; the TAP has not answered");
			g_tapTime = now + NudgeRetryDelay;
			g_nudgeTime = now;
			ShowWindow(g_nudge, SW_SHOWNOACTIVATE);
			Log(L"nudging the taskbar");
		}
	}

	void Attach(HWND tray)
	{
		const ULONGLONG now = GetTickCount64();
		if (g_tap == TapState::Idle && tray && now >= g_tapTime)
		{
			g_tapTime = now + TapRetryDelay;	// in case explorer cannot even be reached
			Inject(tray);
			Log(L"loading the TAP into explorer");
		}
		else if (g_tap == TapState::Waiting)
		{
			const bool done = g_tapDone && WaitForSingleObject(g_tapDone, 0) == WAIT_OBJECT_0;
			if (!done && now - g_tapTime < TapWaitLimit) return;
			const bool attached = g_tapAttached && WaitForSingleObject(g_tapAttached, 0) == WAIT_OBJECT_0;
			ReleaseTap();
			g_tap = attached ? TapState::Attached : TapState::Idle;
			Log(attached ? L"TAP attached" : done ? L"TAP refused, retrying" : L"TAP did not answer, retrying");
			// Once attached, the next wait is for the nudge: just long enough for the TAP to subscribe.
			g_tapTime = now + (attached ? NudgeDelay : TapRetryDelay);
			// Refused copies are unloaded by now; the attached one is locked and stays.
			if (attached) { CleanTapCopies(); g_nudges = 0; }
		}
		else if (g_tap == TapState::Attached)
		{
			Nudge(now);
		}
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
			Log(L"new explorer");
			Disconnect();
			EndNudge();
			ReleaseTap();
			g_explorer = pid;
			g_tap = TapState::Idle;
			g_tapTime = GetTickCount64();
		}
		Attach(tray);

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
			Follow();
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
	CleanTapCopies();

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
	g_nudge = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, L"LiquidGlassTaskbarPill", L"Liquid Glass taskbar", WS_POPUP,
		-32000, -32000, 1, 1, nullptr, nullptr, instance, nullptr);
	SetLayeredWindowAttributes(g_nudge, 0, 0, LWA_ALPHA);

	// The taskbar can move (auto-hide), so it is followed often; the TAP's message covers layout changes.
	SetTimer(g_host, FollowTimer, 33, nullptr);
	Follow();

	MSG message;
	while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
	EndNudge();
	ReleaseTap();
	Disconnect();
	return 0;
}
