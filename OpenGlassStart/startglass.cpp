// Probe only: one glass window kept under the Start menu or Search, where starttap.dll says they are.
// Both sit in the same place and Search shows along with Start (it holds Start's search box), so one glass serves both:
// it follows Start while Start is on screen, else Search. It hides when neither is on screen, or as soon as another
// window takes the foreground from them: XAML only moves a closing Start menu at the end of its slide.
// Exits after five minutes.
#include <windows.h>
#include <winternl.h>
#include <dwmapi.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <cstdarg>
#include <share.h>
#include <cstdio>
#include "startshare.h"

namespace
{
	struct ACCENT_POLICY { DWORD AccentState, AccentFlags, GradientColor, AnimationId; };
	struct WINDOWCOMPOSITIONATTRIBDATA { DWORD Attribute; void* Data; SIZE_T Size; };
	using SetWindowCompositionAttribute_t = BOOL(WINAPI*)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);

	FILE* g_log{};
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
		const wchar_t* container;	// the exe whose app container holds the share, if it is in one
		const StartShare* view{};
		ULONGLONG retry{};
		DWORD cloaked = 99;
		HWND window{};
		RECT rect{};
		bool visible{};
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

	// SearchHost runs in an app container, so the share its TAP makes as Local\ lands in that container's own
	// namespace: \Sessions\<session>\AppContainerNamedObjects\<container SID>\. Opened there through ntdll.
	HANDLE OpenContainerShare(const wchar_t* exe, const wchar_t* share)
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
		swprintf_s(path, L"\\Sessions\\%lu\\AppContainerNamedObjects\\%s\\%s", session, sid, wcschr(share, L'\\') + 1);
		LocalFree(sid);
		using NtOpenSection_t = LONG(NTAPI*)(HANDLE*, ACCESS_MASK, OBJECT_ATTRIBUTES*);
		const auto open = reinterpret_cast<NtOpenSection_t>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtOpenSection"));
		UNICODE_STRING name{ static_cast<USHORT>(wcslen(path) * sizeof(wchar_t)), static_cast<USHORT>(sizeof(path)), path };
		OBJECT_ATTRIBUTES attributes{ sizeof(attributes), nullptr, &name, OBJ_CASE_INSENSITIVE };
		HANDLE section{};
		const LONG status = open ? open(&section, SECTION_MAP_READ, &attributes) : -1;
		if (status < 0) { Log(L"%s: no share at %s (0x%08lx)", exe, path, status); return nullptr; }
		return section;
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
			if (!mapping && surface.container) mapping = OpenContainerShare(surface.container, surface.share);
			surface.view = mapping ? static_cast<const StartShare*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(StartShare))) : nullptr;
			if (!surface.view) return;
			Log(L"%s: share open", surface.name);
		}
		StartShare copy;
		LONG sequence;
		do { sequence = surface.view->sequence; copy = *surface.view; } while ((sequence & 1) || sequence != surface.view->sequence);
		surface.window = reinterpret_cast<HWND>(copy.window);
		surface.rect = copy.menu;
		DWORD cloaked = 1;
		if (surface.window) DwmGetWindowAttribute(surface.window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
		if (cloaked != surface.cloaked) { Log(L"%s: cloaked %lu", surface.name, cloaked); surface.cloaked = cloaked; }
		MONITORINFO monitor{ sizeof(monitor) };
		GetMonitorInfoW(MonitorFromRect(&copy.menu, MONITOR_DEFAULTTONEAREST), &monitor);
		surface.visible = !cloaked && !IsRectEmpty(&copy.menu) && copy.menu.top < monitor.rcWork.bottom;
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
	Surface start{ L"start", StartShareName, nullptr }, search{ L"search", SearchShareName, L"SearchHost.exe" };
	const HWND glass = MakeGlass(instance);
	Log(L"glass window ready");

	HWND foreground{};
	bool shown{}, focused{}, closing{};
	RECT last{};
	const ULONGLONG end = GetTickCount64() + 300000;
	while (GetTickCount64() < end)
	{
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
		const bool open = start.visible || search.visible;
		// Focus moves between Start and Search, and through no window at all on the way; only another window means leaving.
		if (!open) focused = closing = false;
		else if (now && (now == start.window || now == search.window)) focused = true;
		else if (now && focused && !closing) { closing = true; Log(L"left for another window"); }
		const RECT& rect = start.visible ? start.rect : search.rect;
		const bool show = open && !closing;
		if (show && (!shown || !EqualRect(&rect, &last)))
		{
			SetWindowPos(glass, HWND_TOPMOST, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
			Log(L"%s under %s %ld,%ld %ldx%ld", shown ? L"move" : L"show", start.visible ? L"start" : L"search", rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
			last = rect;
			shown = true;
		}
		else if (!show && shown)
		{
			ShowWindow(glass, SW_HIDE);
			Log(L"hide");
			shown = false;
		}
		DwmFlush();
	}
	Log(L"done");
	return 0;
}
