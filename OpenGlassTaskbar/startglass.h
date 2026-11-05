// Fluid Glass under the Start menu and Search, our side. Included once by pillhost.cpp, inside its namespace,
// after its helpers (Log, HostFolder, TapFolder); pillhost.cpp also includes what it needs. Runs on a thread of its own,
// so the pills never wait for it.
// It loads starttap.dll into StartMenuExperienceHost and SearchHost, again whenever Windows restarts them, and keeps one
// glass window under whichever flyout is open. Both sit in the same place, and Search shows along with Start (it holds
// Start's search box), so one glass serves both: it follows Start while Start is shown, else Search.

namespace StartGlass
{
	constexpr ULONGLONG ShellWaitLimit = 30000;	// how long, in ms, Start and Search wait for our TAP in explorer before we load into them
	// Start's close slide, measured at 75 Hz: from the moment it loses the foreground it eases in as e^(t / 43 ms) and is gone 200 ms in.
	constexpr double CloseEase = 43.3, CloseLength = 200;
	// DWM uncloaks Start two frames before its opening starts to slide, and the TAP sees the first frame a frame after it shows.
	constexpr double OpeningWait = 300;	// how long, in ms, the glass goes by the uncloak alone before it gives up
	// Start's opening, measured at 75 Hz: how far up the menu is, frame by frame, as a share of the way from where it parks
	// to where it rests. It runs on the compositor's clock, whatever Start's UI thread does meanwhile.
	constexpr double OpenCurve[] = { 0, .3916, .5568, .6705, .7558, .8211, .8716, .9116, .9421, .9653, .9821, .9926, .9989, 1 };
	constexpr double OpenStep = 1000.0 / 75;
	constexpr double UncloakEarly = 2 * OpenStep;
	// Filmed, the glass that started 35 ms after Start lost the foreground ran 5 to 20 ms behind its close. Ahead of the
	// menu the glass leaves it bare, and behind it the glass only shows a little more of itself, so it errs late.
	constexpr double CloseDelay = 24;
	constexpr ULONGLONG UnparkedWait = 100;	// how long, in ms, an uncloaked Start may still park the menu it laid out

	// While the glass moves with a flyout, the cloak watcher asks every 4 ms.
	inline std::atomic<bool> g_watchFast{};
	// Set as the glass starts to hurry, so the resting cloak watcher asks at once.
	inline const HANDLE g_watchWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	// The cloak watcher sets this when a flyout cloaks or uncloaks, so the glass hears of an opening at once.
	inline const HANDLE g_cloakChanged = CreateEventW(nullptr, FALSE, FALSE, nullptr);

	// How far up Start's opening is, ms into it.
	inline double OpenShare(double ms)
	{
		const double at = ms / OpenStep;
		if (at <= 0) return 0;
		const size_t index = static_cast<size_t>(at);
		if (index + 1 >= std::size(OpenCurve)) return 1;
		return OpenCurve[index] + (at - index) * (OpenCurve[index + 1] - OpenCurve[index]);
	}

	// How many ms into Start's opening the menu is this far up.
	inline double OpenTime(double share)
	{
		if (share <= 0) return 0;
		for (size_t index = 1; index < std::size(OpenCurve); ++index)
			if (share <= OpenCurve[index])
				return (index - 1 + (share - OpenCurve[index - 1]) / (OpenCurve[index] - OpenCurve[index - 1])) * OpenStep;
		return (std::size(OpenCurve) - 1) * OpenStep;
	}

	struct Surface
	{
		const wchar_t* name;
		const wchar_t* exe;
		const wchar_t* share;
		const wchar_t* movedName;
		const wchar_t* wakeName;
		bool container;	// the exe runs in an app container, which holds the share
		DWORD pid{};
		HANDLE process{};	// tells when pid exits
		const StartShare* view{};
		HANDLE mapping{}, moved{}, wake{};
		ULONGLONG retry{};
		std::atomic<HWND> watched{};	// for WatchCloaks
		std::atomic<DWORD> cloakState{ 1 };
		DWORD cloaked = 1;
		HWND window{};
		RECT rect{};
		bool onScreen{}, visible{}, unparked{};
		RECT laidOut{};	// where a fresh Start laid its menu out, while unparked
		ULONGLONG uncloakedAt{};	// when an unparked Start uncloaked
	};

	DWORD FindProcess(const wchar_t* exe)
	{
		const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snapshot == INVALID_HANDLE_VALUE) return 0;
		PROCESSENTRY32W entry{ sizeof(entry) };
		DWORD pid{};
		for (BOOL more = Process32FirstW(snapshot, &entry); more && !pid; more = Process32NextW(snapshot, &entry))
		{
			if (_wcsicmp(entry.szExeFile, exe) == 0) pid = entry.th32ProcessID;
		}
		CloseHandle(snapshot);
		return pid;
	}

	// A TAP pins itself, so one loaded by an earlier run of the host is still there and still working.
	bool HasTap(DWORD pid)
	{
		const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
		if (snapshot == INVALID_HANDLE_VALUE) return false;
		MODULEENTRY32W entry{ sizeof(entry) };
		bool found{};
		for (BOOL more = Module32FirstW(snapshot, &entry); more && !found; more = Module32NextW(snapshot, &entry))
		{
			found = _wcsnicmp(entry.szModule, L"starttap", 8) == 0;
		}
		CloseHandle(snapshot);
		return found;
	}

	// App container processes read only files granted to ALL APPLICATION PACKAGES and ALL RESTRICTED APPLICATION PACKAGES.
	bool GrantAppContainers(const wchar_t* path)
	{
		PSID packages{}, restricted{};
		if (!ConvertStringSidToSidW(L"S-1-15-2-1", &packages) || !ConvertStringSidToSidW(L"S-1-15-2-2", &restricted))
		{
			LocalFree(packages);
			return false;
		}
		EXPLICIT_ACCESSW entries[2]{};
		const PSID sids[2] = { packages, restricted };
		for (int index = 0; index < 2; ++index)
		{
			entries[index].grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
			entries[index].grfAccessMode = GRANT_ACCESS;
			entries[index].Trustee.TrusteeForm = TRUSTEE_IS_SID;
			entries[index].Trustee.ptstrName = static_cast<LPWSTR>(sids[index]);
		}
		PACL current{}, merged{};
		PSECURITY_DESCRIPTOR descriptor{};
		const bool ok = GetNamedSecurityInfoW(path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &current, nullptr, &descriptor) == ERROR_SUCCESS
			&& SetEntriesInAclW(2, entries, current, &merged) == ERROR_SUCCESS
			&& SetNamedSecurityInfoW(const_cast<LPWSTR>(path), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, merged, nullptr) == ERROR_SUCCESS;
		LocalFree(merged);
		LocalFree(descriptor);
		LocalFree(packages);
		LocalFree(restricted);
		return ok;
	}

	// Loads a fresh copy of starttap.dll, from the user's tap folder, the way Inject loads pilltap into explorer.
	// The TAP waits for the process's XAML itself, so this does not wait for anything.
	void LoadTap(const Surface& surface)
	{
		wchar_t folder[MAX_PATH], source[MAX_PATH], copy[MAX_PATH], line[160];
		HostFolder(folder);
		swprintf_s(source, L"%sstarttap.dll", folder);
		if (!TapFolder(folder)) return;
		swprintf_s(copy, L"%sstarttap-%lu-%llu.dll", folder, surface.pid, GetTickCount64());	// both loads can fall in one tick
		if (!CopyFileW(source, copy, FALSE) || !GrantAppContainers(copy))
		{
			swprintf_s(line, L"start glass: cannot stage starttap.dll for %s", surface.exe);
			Log(line);
			return;
		}
		const HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE, FALSE, surface.pid);
		if (!process) { swprintf_s(line, L"start glass: cannot open %s", surface.exe); Log(line); return; }
		const SIZE_T size = (wcslen(copy) + 1) * sizeof(wchar_t);
		void* remote = VirtualAllocEx(process, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		const auto load = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
		HANDLE thread{};
		if (remote && WriteProcessMemory(process, remote, copy, size, nullptr)) thread = CreateRemoteThread(process, nullptr, 0, load, remote, 0, nullptr);
		swprintf_s(line, L"start glass: %s %s", thread ? L"loading the TAP into" : L"cannot start a thread in", surface.exe);
		Log(line);
		if (!thread)
		{
			if (remote) VirtualFreeEx(process, remote, 0, MEM_RELEASE);
			CloseHandle(process);
			return;
		}
		// The path stays readable until LoadLibraryW is done with it.
		std::thread([process, thread, remote]
		{
			if (WaitForSingleObject(thread, 30000) == WAIT_OBJECT_0) VirtualFreeEx(process, remote, 0, MEM_RELEASE);
			CloseHandle(thread);
			CloseHandle(process);
		}).detach();
	}

	// What SearchHost's TAP makes as Local\ lands in its app container's own namespace:
	// \Sessions\<session>\AppContainerNamedObjects\<container SID>\. Opened there through ntdll.
	HANDLE OpenContainerObject(DWORD pid, const wchar_t* object, bool event)
	{
		const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
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
		return open && open(&handle, event ? SYNCHRONIZE | EVENT_MODIFY_STATE : SECTION_MAP_READ, &attributes) >= 0 ? handle : nullptr;
	}

	void Forget(Surface& surface)
	{
		if (surface.view) UnmapViewOfFile(surface.view);
		if (surface.mapping) CloseHandle(surface.mapping);
		if (surface.moved) CloseHandle(surface.moved);
		if (surface.wake) CloseHandle(surface.wake);
		surface.view = nullptr;
		surface.mapping = surface.moved = surface.wake = nullptr;
		surface.window = nullptr;
		surface.watched = nullptr;
		surface.rect = {};
	}

	// About once a second: notices a new process (Windows restarts both now and then), loads the TAP into it,
	// and opens what the TAP shares once it has found the flyout.
	void Connect(Surface& surface)
	{
		const ULONGLONG now = GetTickCount64();
		if (now < surface.retry) return;
		surface.retry = now + 1000;
		// A process snapshot each second cost a few percent of a core while nothing happened,
		// so the list is searched only once the process we follow has exited.
		const bool alive = surface.process && WaitForSingleObject(surface.process, 0) == WAIT_TIMEOUT;
		const DWORD pid = alive ? surface.pid : FindProcess(surface.exe);
		if (pid != surface.pid)
		{
			Forget(surface);
			if (surface.process) CloseHandle(surface.process);
			surface.process = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
			surface.pid = pid;
			if (pid && !HasTap(pid)) LoadTap(surface);
		}
		if (!pid || surface.view) return;
		surface.mapping = surface.container ? OpenContainerObject(pid, surface.share, false) : OpenFileMappingW(FILE_MAP_READ, FALSE, surface.share);
		surface.view = surface.mapping ? static_cast<const StartShare*>(MapViewOfFile(surface.mapping, FILE_MAP_READ, 0, 0, sizeof(StartShare))) : nullptr;
		if (!surface.view) return;
		surface.moved = surface.container ? OpenContainerObject(pid, surface.movedName, true) : OpenEventW(SYNCHRONIZE, FALSE, surface.movedName);
		surface.wake = surface.container ? OpenContainerObject(pid, surface.wakeName, true) : OpenEventW(EVENT_MODIFY_STATE, FALSE, surface.wakeName);
		wchar_t line[96];
		swprintf_s(line, L"start glass: following %s%s", surface.name, surface.wake ? L"" : L", but cannot wake it");
		Log(line);
	}

	// Where the flyout is and whether it is on screen: above the bottom of the work area, as Start parks its menu
	// below the screen when closed, and Search its panel just below the work area (y 1044 on a 1032 work area).
	void Read(Surface& surface)
	{
		surface.visible = surface.onScreen = false;
		if (!surface.view) return;
		StartShare copy;
		LONG sequence;
		do { sequence = surface.view->sequence; copy = *surface.view; } while ((sequence & 1) || sequence != surface.view->sequence);
		surface.window = reinterpret_cast<HWND>(copy.window);
		surface.watched = surface.window;
		surface.rect = copy.menu;
		surface.cloaked = surface.window ? surface.cloakState.load() : 1;
		MONITORINFO monitor{ sizeof(monitor) };
		GetMonitorInfoW(MonitorFromRect(&copy.menu, MONITOR_DEFAULTTONEAREST), &monitor);
		// A menu Start has never parked counts, while cloaked, as parked where Start parks it (y 1104 on a 1080 screen).
		// Most first openings still park it, 70 to 90 ms after the uncloak, and the glass that went up from the uncloak
		// ran ahead of the menu and flashed where Search was. So it counts as parked for UnparkedWait after the uncloak too.
		surface.unparked = copy.unparked != 0;
		surface.laidOut = surface.unparked ? copy.menu : RECT{};
		const ULONGLONG tick = GetTickCount64();
		if (!surface.unparked || surface.cloaked) surface.uncloakedAt = 0;
		else if (!surface.uncloakedAt) surface.uncloakedAt = tick;
		if (surface.unparked && (surface.cloaked || tick - surface.uncloakedAt < UnparkedWait))
		{
			const UINT dpi = GetDpiForWindow(surface.window);
			OffsetRect(&surface.rect, 0, monitor.rcMonitor.bottom + MulDiv(24, dpi ? dpi : 96, 96) - copy.menu.top);
			copy.menu = surface.rect;
		}
		surface.onScreen = !IsRectEmpty(&copy.menu) && copy.menu.top < monitor.rcWork.bottom;
		surface.visible = !surface.cloaked && surface.onScreen;
	}

	// Asking DWM whether Start is cloaked sometimes blocks for two seconds while Start opens, and the glass waited with it.
	// So a thread of its own asks, and the glass goes by its last answer, and by XAML, meanwhile.
	// At rest it asked every 16 ms all the same, which woke it 32 times a second for nothing. Now it rests until a window
	// cloaks or uncloaks, or the glass starts to hurry, and asks four times a second meanwhile. Start takes the foreground
	// 80 to 100 ms before it uncloaks, which is what makes the glass hurry, so the watcher asks every 4 ms by then.
	// Measured against a 2 ms poll, the cloak event came as fast, once 5 ms late, so while hurrying the asking still leads.
	void WatchCloaks(Surface* surfaces, size_t count)
	{
		// The hook's message alone wakes the wait, so it does nothing itself.
		SetWinEventHook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED, nullptr, [](HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {}, 0, 0, WINEVENT_OUTOFCONTEXT);
		for (;;)
		{
			for (size_t index = 0; index < count; ++index)
			{
				const HWND window = surfaces[index].watched;
				DWORD cloaked = 1;
				if (window) DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
				if (surfaces[index].cloakState.exchange(cloaked) != cloaked) SetEvent(g_cloakChanged);
			}
			MsgWaitForMultipleObjects(1, &g_watchWake, FALSE, g_watchFast ? 4 : 250, QS_ALLINPUT);
			MSG message;
			while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
		}
	}

	HWND MakeGlass(HINSTANCE instance)
	{
		const HWND glass = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, StartGlassClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
		const auto setAttribute = reinterpret_cast<SetWindowCompositionAttribute_t>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
		// OpenGlass: light or dark by the Windows mode, like Start itself.
		ACCENT_POLICY accent{ 3, 0x40000000, 0, 0 };
		WINDOWCOMPOSITIONATTRIBDATA data{ 19, &accent, sizeof(accent) };
		if (setAttribute) setAttribute(glass, &data);
		const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
		DwmSetWindowAttribute(glass, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
		const BOOL excluded = TRUE;
		DwmSetWindowAttribute(glass, DWMWA_EXCLUDED_FROM_PEEK, &excluded, sizeof(excluded));
		// Out for good and cloaked while no flyout needs it: shown from hidden, it came out two frames into the slide.
		// While Start's menu is parked below the screen, it waits there uncloaked too.
		const BOOL cloak = TRUE;
		DwmSetWindowAttribute(glass, DWMWA_CLOAK, &cloak, sizeof(cloak));
		ShowWindow(glass, SW_SHOWNOACTIVATE);
		return glass;
	}

	void Run(HINSTANCE instance)
	{
		WNDCLASSW type{};
		type.lpfnWndProc = DefWindowProcW;
		type.hInstance = instance;
		type.lpszClassName = StartGlassClass;
		// Black is fully transparent under the accent, so only the glass shows.
		type.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
		RegisterClassW(&type);
		static Surface surfaces[] =
		{
			{ L"Start", L"StartMenuExperienceHost.exe", StartShareName, StartMovedName, StartWakeName, false },
			{ L"Search", L"SearchHost.exe", SearchShareName, SearchMovedName, SearchWakeName, true },
		};
		Surface& start = surfaces[0];
		Surface& search = surfaces[1];
		std::thread(WatchCloaks, surfaces, std::size(surfaces)).detach();

		HWND glass{};
		bool shown{}, focused{}, closing{}, met{}, startWasUp{}, hurried{}, searchWasOut{}, taskbarHeld{};
		DWORD startPid{}, searchPid{};
		RECT last{}, seen{}, parked{}, rest{}, slideFrom{};
		LONG step{}, stepBefore{}, slideSeen{};
		bool fromBelow{};	// the last step came from below every screen
		bool seenStart{};	// what was last seen was Start's menu, not Search's
		bool openSeen{};	// the TAP has seen a frame of this opening
		double seenAt{}, gap{}, gapBefore{}, slideAt{}, anchorAt{}, openAt{};
		double frame = 1000.0 / 60;	// ms between two frames of the display
		ULONGLONG settingRead{}, wokeAt{};
		bool out{};	// uncloaked, on screen or parked below it
		int lastStirring{};
		const ULONGLONG began = GetTickCount64();
		// The glass checked the foreground every 8 ms, so it heard of a close up to 8 ms late. Now a change of foreground
		// wakes it at once. The hook's message alone wakes the wait, so it does nothing itself.
		SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, [](HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {}, 0, 0, WINEVENT_OUTOFCONTEXT);
		for (;;)
		{
			MSG message;
			while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
			// The setting is read live, like the pills' own. Without our window the TAPs give back the original acrylic.
			const ULONGLONG tick = GetTickCount64();
			if (tick - settingRead >= 500)
			{
				settingRead = tick;
				DWM_TIMING_INFO timing{ sizeof(timing) };
				if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &timing)) && timing.rateRefresh.uiNumerator) frame = 1000.0 * timing.rateRefresh.uiDenominator / timing.rateRefresh.uiNumerator;
				const bool enabled = ReadPillSetting(PillStartGlass) != 0;
				const bool switched = enabled != (glass != nullptr);
				if (enabled && !glass) { glass = MakeGlass(instance); shown = out = false; last = {}; Log(L"start glass: on"); }
				else if (!enabled && glass) { DestroyWindow(glass); glass = nullptr; Log(L"start glass: off"); }
				// Resting TAPs would only swap the acrylic the next time their flyout opens.
				if (switched) for (const auto& surface : surfaces) if (surface.wake) SetEvent(surface.wake);
			}
			if (!glass)
			{
				MsgWaitForMultipleObjects(0, nullptr, FALSE, 500, QS_ALLINPUT);
				continue;
			}
			// At sign-in, loading into Start and Search while explorer still builds the taskbar slowed explorer down.
			// So that waits for our TAP in explorer, or for half a minute in case it never gets in.
			if (g_shellReady || tick - began > ShellWaitLimit) for (auto& surface : surfaces) Connect(surface);
			Read(start);
			Read(search);
			const HWND now = GetForegroundWindow();
			// While Start is shown, the glass belongs to it alone, even before its slide brings it on screen:
			// Search already sits at the top then, and following it put the glass there before the menu arrived.
			// DWM sometimes still reports Start cloaked for seconds after it opened, so a Start that XAML has brought
			// on screen also counts while Start or Search holds the foreground, or nothing does, as happens while Start opens.
			const bool flyoutFocused = !now || now == start.window || now == search.window;
			// A fresh Start lays its menu out on screen before it parks it below, and at sign-in nothing holds the
			// foreground, so the glass showed there, empty. It waits until Start or Search has really had the foreground.
			if (start.pid != startPid || search.pid != searchPid) { startPid = start.pid; searchPid = search.pid; met = false; }
			if (now && (now == start.window || now == search.window)) met = true;
			// The TAPs only follow their flyout frame by frame while woken. Start opening gives Search the foreground
			// first and uncloaks both, so each of those wakes them, and every half second while it lasts.
			const int stirring = (now && now == start.window) | (now && now == search.window) << 1 | (start.cloaked == 0) << 2 | (search.cloaked == 0) << 3;
			if (stirring && (stirring != lastStirring || tick - wokeAt >= 500))
			{
				for (const auto& surface : surfaces) if (surface.wake) SetEvent(surface.wake);
				wokeAt = tick;
			}
			lastStirring = stirring;
			bool startShown = start.window && (start.cloaked == 0 || (start.onScreen && flyoutFocused));
			// Going over to Search, Start parks its menu a few frames before DWM reports it cloaked, and in that gap
			// the glass went away and came back. A Start that was up and has parked again has handed over to Search.
			if (!startShown) startWasUp = false;
			else if (start.onScreen && !start.unparked) startWasUp = true;
			else if (startWasUp && search.visible) startShown = false;
			const bool open = startShown ? start.onScreen : search.visible;
			// Focus moves between Start and Search, and through no window at all, on the way. Once another window has it,
			// the flyout is closing, and the glass goes, or for Start slides away with it (see below).
			const RECT& rect = startShown ? start.rect : search.rect;
			const bool wasClosing = closing;
			// A right click on the taskbar gives it the foreground while Start stays up under the taskbar's menu, and the
			// glass that went then left the menu bare for as long as that menu showed. So the taskbar holding the foreground
			// is not yet a close: a Start that does close from there hides Search 7 to 12 ms in, which the check below catches.
			// Nor is what follows: an item picked from that menu takes the foreground while Start stays up another 400 to
			// 600 ms, so once the taskbar has held it, only Search's cloak closes the glass, until Start has it back.
			const auto taskbarHolds = [now]
			{
				wchar_t name[32]{};
				return GetClassNameW(now, name, ARRAYSIZE(name)) && (!wcscmp(name, L"Shell_TrayWnd") || !wcscmp(name, L"Shell_SecondaryTrayWnd"));
			};
			if (!open) focused = closing = taskbarHeld = false;
			else if (flyoutFocused) focused = true, taskbarHeld = false;
			else if (focused && !closing)
			{
				if (taskbarHolds()) taskbarHeld = true;
				else if (!taskbarHeld) closing = true;
			}
			// Closed with the Windows key or the Start button, Start leaves the foreground with Search, so only Search's
			// cloak tells it is going, about 200 ms before Start's own, and the glass stood still while the menu slid down.
			if (open && startShown && (startWasUp || start.unparked) && start.onScreen && searchWasOut && search.cloaked) closing = true;
			searchWasOut = search.cloaked == 0;
			bool show = open && met && !closing;
			LARGE_INTEGER counter, frequency;
			QueryPerformanceFrequency(&frequency);
			QueryPerformanceCounter(&counter);
			const double ms = counter.QuadPart * 1000.0 / frequency.QuadPart;
			// The glass lands a frame after the menu, so while the menu slides it goes where the next frame should put it.
			// The slides ease in and out, so each step is about the last one times the ratio of the last two, never more
			// than the last one, both taken per frame, as a step that took two frames overshot. Start's opening always
			// goes 1104, 732, 575 on a 1080 screen, so its first step from below the screen is followed by one 0.42 as long.
			// Once no new frame has come for 20 ms, the glass goes where the menu is.
			// Before the slide the glass may still go by Search, so the first step is taken from where Start was parked.
			if (start.window && !start.onScreen && !IsRectEmpty(&start.rect)) parked = start.rect;
			const bool fresh = !EqualRect(&rect, &seen);
			if (fresh)
			{
				const auto alongside = [&](const RECT& other) { return rect.left == other.left && rect.right == other.right && rect.bottom - rect.top == other.bottom - other.top; };
				const bool along = alongside(seen) && seenStart == startShown;
				fromBelow = startShown && !IsRectEmpty(&parked) && rect.top < parked.top && alongside(parked) && (!along || EqualRect(&seen, &parked));
				stepBefore = along && !fromBelow ? step : 0;
				step = fromBelow ? rect.top - parked.top : along ? rect.top - seen.top : 0;
				gapBefore = gap;
				gap = fromBelow ? frame : ms - seenAt;
				seen = rect;
				seenStart = startShown;
				seenAt = ms;
			}
			if (startShown && start.onScreen && !closing && ms - seenAt > 100) rest = start.rect;
			// A fresh Start rests its menu where it laid it out, so its first opening goes by the uncloak like every other.
			if (start.unparked && IsRectEmpty(&rest)) rest = start.laidOut;
			RECT place = rect;
			if (show && step && ms - seenAt < 20)
			{
				double lead{};
				const auto perFrame = [&](LONG distance, double took) { return distance / (std::max)(1.0, std::round(took / frame)); };
				if (stepBefore && (step > 0) == (stepBefore > 0))
				{
					const double latest = perFrame(step, gap), earlier = perFrame(stepBefore, gapBefore);
					lead = latest * (std::min)(1.0, latest / earlier);
				}
				else if (fromBelow) lead = step * 0.42;
				OffsetRect(&place, 0, static_cast<LONG>(std::lround(lead)));
			}
			const auto sameSize = [](const RECT& one, const RECT& other) { return one.left == other.left && one.right == other.right && one.bottom - one.top == other.bottom - other.top; };
			// Uneven gaps between frames made that guess overshoot past where Start rests.
			if (startShown && !closing && !IsRectEmpty(&rest) && sameSize(place, rest) && place.top < rest.top && rect.top >= rest.top) OffsetRect(&place, 0, rest.top - place.top);
			// The TAP sees the first frame of an opening a frame late, and up to 100 ms late when the cloak watcher was slow,
			// so the glass came in 200 px behind. Now the opening's clock starts from the uncloak, which comes two frames
			// before the slide. Until the TAP sees the menu, the glass goes no further than four frames into the slide.
			const bool opening = startShown && !start.onScreen && !startWasUp && start.cloaked == 0 && met && flyoutFocused
				&& !IsRectEmpty(&parked) && !IsRectEmpty(&rest) && sameSize(rest, parked) && rest.top < parked.top;
			if (opening && !anchorAt) anchorAt = ms;
			if (opening && ms - anchorAt < OpeningWait)
			{
				show = true;
				const double share = (std::min)(OpenShare(ms + frame - anchorAt - UncloakEarly), OpenCurve[4]);
				place = parked;
				OffsetRect(&place, 0, -static_cast<LONG>(std::lround(share * (parked.top - rest.top))));
			}
			// The TAP misses frames of Start's opening while the compositor keeps sliding the menu, and the glass that went
			// back to the last frame the TAP saw dropped 50 to 100 px behind. So the opening has its own clock. The uncloak
			// starts it, and the first frame the TAP sees sets it right. The TAP is never early, so every later frame can only
			// set the clock earlier. The glass goes where the next frame should put the menu, never back down, never above
			// where it rests, never below the TAP.
			const bool rising = startShown && start.onScreen && !closing && !IsRectEmpty(&rest) && !IsRectEmpty(&parked)
				&& sameSize(rest, parked) && sameSize(rect, rest) && rest.top < parked.top;
			if (!rising) openAt = 0, openSeen = false;
			// A menu Start never parked slides up where the TAP cannot see it, so the glass goes by the uncloak alone.
			else if (start.unparked)
			{
				if (!openSeen) openAt = (anchorAt ? anchorAt : ms) + UncloakEarly, openSeen = true;
			}
			else
			{
				if (!openAt && anchorAt && ms - anchorAt < OpeningWait) openAt = anchorAt + UncloakEarly;
				if (fresh && rect.top < parked.top && (openAt || fromBelow))
				{
					const double at = seenAt - OpenTime(static_cast<double>(parked.top - rect.top) / (parked.top - rest.top));
					openAt = openSeen ? (std::min)(openAt, at) : at;
					openSeen = true;
				}
			}
			if (!opening) anchorAt = 0;
			if (openAt)
			{
				LONG top = parked.top - std::lround(OpenShare(ms + frame - openAt) * (parked.top - rest.top));
				if (!start.unparked) top = (std::min)(top, rect.top);
				if (sameSize(last, rest)) top = (std::min)(top, last.top);
				top = (std::max)(top, rest.top);
				place = rest;
				OffsetRect(&place, 0, top - rest.top);
				if ((rect.top <= rest.top && !start.unparked) || ms - openAt > 1000) openAt = 0;
			}
			// Start slides its menu away the moment it loses the foreground, mostly while its UI thread is too busy for the
			// TAP to see it, so the glass that went at once left the menu bare. Now the glass slides away on its own, the way
			// Start's close goes, a frame ahead, and every frame the TAP does see sets its clock right.
			if (closing && !wasClosing && shown && startShown && !IsRectEmpty(&parked) && sameSize(last, parked) && last.top < parked.top)
			{
				slideAt = ms + CloseDelay;
				slideFrom = last;
				slideSeen = start.rect.top;
			}
			if (slideAt)
			{
				const double travel = parked.top - slideFrom.top;
				const double grows = std::exp(CloseLength / CloseEase) - 1;
				if (start.onScreen && start.rect.top != slideSeen && start.rect.top > slideFrom.top && sameSize(start.rect, slideFrom))
				{
					slideSeen = start.rect.top;
					slideAt = ms - CloseEase * std::log(1 + (slideSeen - slideFrom.top) / travel * grows);
				}
				const double progress = (std::exp((ms + frame - slideAt) / CloseEase) - 1) / grows;
				if (now == start.window || start.cloaked || !start.onScreen || progress >= 1) slideAt = 0;
				else
				{
					show = true;
					place = slideFrom;
					OffsetRect(&place, 0, static_cast<LONG>(std::lround(progress * travel)));
					// A late frame of the TAP sets the clock back, but the glass never goes back up.
					if (place.top < last.top) place = last;
				}
			}
			// While cloaked, the glass waits where Start parks its menu, below the screen, so an opening only moves it.
			if (!show) place = start.window && !start.onScreen && !IsRectEmpty(&start.rect) ? start.rect : last;
			if (!IsRectEmpty(&place) && !EqualRect(&place, &last))
			{
				SetWindowPos(glass, HWND_TOPMOST, place.left, place.top, place.right - place.left, place.bottom - place.top, SWP_NOACTIVATE);
				last = place;
			}
			// Below every screen the glass can stay uncloaked, so an opening only has to move it.
			const bool wantOut = show || (!IsRectEmpty(&last) && !MonitorFromRect(&last, MONITOR_DEFAULTTONULL));
			if (wantOut != out)
			{
				const BOOL cloak = !wantOut;
				DwmSetWindowAttribute(glass, DWMWA_CLOAK, &cloak, sizeof(cloak));
				out = wantOut;
			}
			if (show != shown)
			{
				// The TAPs read this to tell whether the glass shows, as the window itself always is out.
				SetWindowLongPtrW(glass, GWLP_USERDATA, show);
				shown = show;
			}
#ifdef FLUIDGLASS_LOGS
			// Log builds only: each change in what the glass goes by, with where it is and who holds the foreground.
			{
				static wchar_t lastState[320];
				wchar_t holder[64] = L"nobody";
				if (now == start.window) wcscpy_s(holder, L"Start");
				else if (now == search.window) wcscpy_s(holder, L"Search");
				else if (now)
				{
					wchar_t name[40]{};
					GetClassNameW(now, name, ARRAYSIZE(name));
					DWORD pid{};
					GetWindowThreadProcessId(now, &pid);
					swprintf_s(holder, L"\"%s\" pid %lu", name, pid);
				}
				wchar_t state[320];
				swprintf_s(state, L"show %d open %d start %d (cloaked %lu, on %d, was up %d) search cloaked %lu on %d, focused %d closing %d met %d, fg %s",
					show, open, startShown, start.cloaked, start.onScreen, startWasUp, search.cloaked, search.onScreen, focused, closing, met, holder);
				if (wcscmp(state, lastState))
				{
					wcscpy_s(lastState, state);
					wchar_t line[480];
					swprintf_s(line, L"start glass: %s; start %ld,%ld %ldx%ld, search %ld,%ld %ldx%ld", state,
						start.rect.left, start.rect.top, start.rect.right - start.rect.left, start.rect.bottom - start.rect.top,
						search.rect.left, search.rect.top, search.rect.right - search.rect.left, search.rect.bottom - search.rect.top);
					Log(line);
				}
			}
#endif
			// Both TAPs say the moment their flyout moves, and the cloak watcher the moment one uncloaks, which is how an
			// opening shows up. While the glass is out, or Start or Search holds the foreground, the foreground is checked
			// every 8 ms, and every 4 ms while the glass moves. Windows wakes a process only every 16 ms unless it asks for
			// finer timing, so it asks then, and only then. Moving the glass up onto the screen asks for a paint each time,
			// which woke the wait at once and spun it, so paints wait for the next turn. At rest nothing needs the clock
			// sooner than every half second (the setting, the TAPs' wake, a new Start), so the wait lasts 250 ms then.
			const bool held = now && (now == start.window || now == search.window);
			const bool hurry = shown || held || slideAt || anchorAt || openAt;
			if (hurry != hurried)
			{
				if (hurry) timeBeginPeriod(1);
				else timeEndPeriod(1);
				hurried = hurry;
				g_watchFast = hurry;
				if (hurry) SetEvent(g_watchWake);
			}
			const bool moving = slideAt || anchorAt || openAt || ms - seenAt < 100;
			HANDLE moved[3];
			DWORD count{};
			for (const auto& surface : surfaces) if (surface.moved) moved[count++] = surface.moved;
			if (g_cloakChanged) moved[count++] = g_cloakChanged;
			MsgWaitForMultipleObjects(count, moved, FALSE, !hurry ? 250 : moving ? 4 : 8, QS_ALLINPUT & ~QS_PAINT);
		}
	}
}
