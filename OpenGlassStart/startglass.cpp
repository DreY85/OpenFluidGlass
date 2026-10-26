// Probe only: one glass window kept under the Start menu or Search, where starttap.dll says they are.
// Both sit in the same place and Search shows along with Start (it holds Start's search box), so one glass serves both:
// it follows Start while DWM shows Start, else Search. It hides when neither is on screen, or as soon as another
// window takes the foreground from them: XAML only moves a closing Start menu at the end of its slide.
// Exits after fifteen minutes.
#include <windows.h>
#include <winternl.h>
#include <dwmapi.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <atomic>
#include <cstdarg>
#include <share.h>
#include <thread>
#include <cstdio>
#include "startshare.h"

namespace
{
	struct ACCENT_POLICY { DWORD AccentState, AccentFlags, GradientColor, AnimationId; };
	struct WINDOWCOMPOSITIONATTRIBDATA { DWORD Attribute; void* Data; SIZE_T Size; };
	using SetWindowCompositionAttribute_t = BOOL(WINAPI*)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);

	FILE* g_log{};
	bool g_trustedXaml{};	// logged once per time Start shows on XAML's word alone
	void Log(const wchar_t* format, ...)
	{
		SYSTEMTIME now{};
		GetLocalTime(&now);
		fwprintf(g_log, L"%02u:%02u:%02u.%03u ", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
		va_list arguments;
		va_start(arguments, format);
		vfwprintf(g_log, format, arguments);
		va_end(arguments);
		fwprintf(g_log, L"\n");
		fflush(g_log);
	}

	struct Surface
	{
		const wchar_t* name;
		const wchar_t* share;
		const wchar_t* movedName;
		const wchar_t* container;	// the exe whose app container holds the share, if it is in one
		const StartShare* view{};
		HANDLE moved{};
		std::atomic<HWND> watched{};	// for WatchCloaks
		std::atomic<DWORD> cloakState{ 1 };
		ULONGLONG retry{};
		DWORD cloaked = 99;
		HWND window{};
		RECT rect{};
		bool onScreen{}, visible{};
	};

	HWND MakeGlass(HINSTANCE instance)
	{
		const HWND glass = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, L"LiquidGlassStartGlass", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
		const auto setAttribute = reinterpret_cast<SetWindowCompositionAttribute_t>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
		ACCENT_POLICY accent{ 3, 0, 0, 0 };
		WINDOWCOMPOSITIONATTRIBDATA data{ 19, &accent, sizeof(accent) };
		if (setAttribute) setAttribute(glass, &data);
		const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
		DwmSetWindowAttribute(glass, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
		const BOOL excluded = TRUE;
		DwmSetWindowAttribute(glass, DWMWA_EXCLUDED_FROM_PEEK, &excluded, sizeof(excluded));
		return glass;
	}

	// SearchHost runs in an app container, so what its TAP makes as Local\ lands in that container's own
	// namespace: \Sessions\<session>\AppContainerNamedObjects\<container SID>\. Opened there through ntdll.
	HANDLE OpenContainerObject(const wchar_t* exe, const wchar_t* object, bool event)
	{
		const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		PROCESSENTRY32W entry{ sizeof(entry) };
		DWORD pid{};
		for (BOOL more = Process32FirstW(snapshot, &entry); more && !pid; more = Process32NextW(snapshot, &entry))
		{
			if (_wcsicmp(entry.szExeFile, exe) == 0) pid = entry.th32ProcessID;
		}
		CloseHandle(snapshot);
		const HANDLE process = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
		HANDLE token{};
		if (!process || !OpenProcessToken(process, TOKEN_QUERY, &token)) { if (process) CloseHandle(process); return nullptr; }
		CloseHandle(process);
		BYTE buffer[256];
		DWORD size{};
		wchar_t* sid{};
		const bool container = GetTokenInformation(token, TokenAppContainerSid, buffer, sizeof(buffer), &size)
			&& reinterpret_cast<TOKEN_APPCONTAINER_INFORMATION*>(buffer)->TokenAppContainer
			&& ConvertSidToStringSidW(reinterpret_cast<TOKEN_APPCONTAINER_INFORMATION*>(buffer)->TokenAppContainer, &sid);
		CloseHandle(token);
		if (!container) return nullptr;
		DWORD session{};
		ProcessIdToSessionId(pid, &session);
		wchar_t path[256];
		swprintf_s(path, L"\\Sessions\\%lu\\AppContainerNamedObjects\\%s\\%s", session, sid, wcschr(object, L'\\') + 1);
		LocalFree(sid);
		using NtOpen_t = LONG(NTAPI*)(HANDLE*, ACCESS_MASK, OBJECT_ATTRIBUTES*);
		const auto open = reinterpret_cast<NtOpen_t>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), event ? "NtOpenEvent" : "NtOpenSection"));
		UNICODE_STRING name{ static_cast<USHORT>(wcslen(path) * sizeof(wchar_t)), static_cast<USHORT>(sizeof(path)), path };
		OBJECT_ATTRIBUTES attributes{ sizeof(attributes), nullptr, &name, OBJ_CASE_INSENSITIVE };
		HANDLE handle{};
		const LONG status = open ? open(&handle, event ? SYNCHRONIZE : SECTION_MAP_READ, &attributes) : -1;
		if (status < 0) { Log(L"%s: nothing at %s (0x%08lx)", exe, path, status); return nullptr; }
		return handle;
	}

	// Reads where the surface is and whether it is on screen: shown by DWM, and above the bottom of the work area,
	// as Search keeps its panel parked just below it (y 1044 on a 1032 work area) while it is not in use.
	void Read(Surface& surface)
	{
		surface.visible = false;
		if (!surface.view)
		{
			// Looking up a container is not free, so only about once a second.
			if (GetTickCount64() < surface.retry) return;
			surface.retry = GetTickCount64() + 1000;
			HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, surface.share);
			if (!mapping && surface.container) mapping = OpenContainerObject(surface.container, surface.share, false);
			surface.view = mapping ? static_cast<const StartShare*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(StartShare))) : nullptr;
			if (!surface.view) return;
			surface.moved = OpenEventW(SYNCHRONIZE, FALSE, surface.movedName);
			if (!surface.moved && surface.container) surface.moved = OpenContainerObject(surface.container, surface.movedName, true);
			Log(L"%s: share open%s", surface.name, surface.moved ? L", with its moved event" : L"");
		}
		StartShare copy;
		LONG sequence;
		do { sequence = surface.view->sequence; copy = *surface.view; } while ((sequence & 1) || sequence != surface.view->sequence);
		surface.window = reinterpret_cast<HWND>(copy.window);
		surface.watched = surface.window;
		surface.rect = copy.menu;
		const DWORD cloaked = surface.window ? surface.cloakState.load() : 1;
		if (cloaked != surface.cloaked) { Log(L"%s: cloaked %lu", surface.name, cloaked); surface.cloaked = cloaked; }
		MONITORINFO monitor{ sizeof(monitor) };
		GetMonitorInfoW(MonitorFromRect(&copy.menu, MONITOR_DEFAULTTONEAREST), &monitor);
		surface.onScreen = !IsRectEmpty(&copy.menu) && copy.menu.top < monitor.rcWork.bottom;
		surface.visible = !cloaked && surface.onScreen;
	}

	// Asking DWM whether Start is cloaked sometimes blocks for two seconds while Start opens, and the glass waited with it.
	// So a thread of its own asks, and the glass goes by the last answer, and by XAML, meanwhile.
	void WatchCloaks(Surface* surfaces, size_t count)
	{
		for (;;)
		{
			for (size_t index = 0; index < count; ++index)
			{
				const HWND window = surfaces[index].watched;
				DWORD cloaked = 1;
				if (window) DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
				surfaces[index].cloakState = cloaked;
			}
			Sleep(8);
		}
	}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	wchar_t path[MAX_PATH];
	GetModuleFileNameW(nullptr, path, MAX_PATH);
	wcscpy_s(wcsrchr(path, L'\\') + 1, 32, L"startglass.txt");
	g_log = _wfsopen(path, L"w, ccs=UTF-8", _SH_DENYNO);	// readable while we run
	if (!g_log) return 1;

	WNDCLASSW type{};
	type.lpfnWndProc = DefWindowProcW;
	type.hInstance = instance;
	type.lpszClassName = L"LiquidGlassStartGlass";
	RegisterClassW(&type);
	Surface surfaces[] = { { L"start", StartShareName, StartMovedName, nullptr }, { L"search", SearchShareName, SearchMovedName, L"SearchHost.exe" } };
	Surface& start = surfaces[0];
	Surface& search = surfaces[1];
	std::thread(WatchCloaks, surfaces, std::size(surfaces)).detach();
	const HWND glass = MakeGlass(instance);
	Log(L"glass window ready");

	HWND foreground{};
	bool shown{}, focused{}, closing{};
	RECT last{};
	ULONGLONG lastTick{};
	const ULONGLONG end = GetTickCount64() + 900000;
	while (GetTickCount64() < end)
	{
		// The two silent seconds above could also have been this loop stalling; this tells the two apart.
		const ULONGLONG tick = GetTickCount64();
		if (lastTick && tick - lastTick > 100) Log(L"loop stalled %llu ms", tick - lastTick);
		lastTick = tick;
		MSG message;
		while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
		const HWND now = GetForegroundWindow();
		if (now != foreground)
		{
			wchar_t name[64]{}, title[64]{};
			GetClassNameW(now, name, 64);
			GetWindowTextW(now, title, 64);
			Log(L"foreground %s '%s'", name, title);
			foreground = now;
		}
		Read(start);
		Read(search);
		// While Start is shown by DWM, the glass belongs to it alone, even before its slide brings it on screen:
		// Search already sits at the top then, and following it put the glass there before the menu arrived.
		// Once, DWM still reported Start cloaked for two seconds while it slid up and stayed open, and the glass never came.
		// So a Start that XAML has brought on screen counts as shown while Start or Search holds the foreground,
		// or nothing does, as happens for a moment while Start opens.
		const bool flyoutFocused = !now || now == start.window || now == search.window;
		const bool startShown = start.window && (start.cloaked == 0 || (start.onScreen && flyoutFocused));
		if (startShown && start.cloaked && !g_trustedXaml) { Log(L"start on screen while DWM says cloaked %lu", start.cloaked); g_trustedXaml = true; }
		if (!start.cloaked) g_trustedXaml = false;
		const bool open = startShown ? start.onScreen : search.visible;
		// Focus moves between Start and Search, and through no window at all on the way; only another window means leaving.
		if (!open) focused = closing = false;
		else if (flyoutFocused) focused = true;
		else if (now && focused && !closing) { closing = true; Log(L"left for another window"); }
		const RECT& rect = startShown ? start.rect : search.rect;
		const bool show = open && !closing;
		if (show && (!shown || !EqualRect(&rect, &last)))
		{
			SetWindowPos(glass, HWND_TOPMOST, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
			Log(L"%s under %s %ld,%ld %ldx%ld", shown ? L"move" : L"show", startShown ? L"start" : L"search", rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
			last = rect;
			shown = true;
		}
		else if (!show && shown)
		{
			ShowWindow(glass, SW_HIDE);
			Log(L"hide");
			shown = false;
		}
		// Start and Search tell us the moment they move; cloaking and the foreground are checked at least every 8 ms.
		HANDLE moved[2];
		DWORD count{};
		for (const auto& surface : surfaces) if (surface.moved) moved[count++] = surface.moved;
		MsgWaitForMultipleObjects(count, moved, FALSE, 8, QS_ALLINPUT);
	}
	Log(L"done");
	return 0;
}
