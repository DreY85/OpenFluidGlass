// Liquid Glass under the Start menu and Search, our side. Included once by pillhost.cpp, inside its namespace,
// after its helpers (Log, HostFolder, TapFolder); pillhost.cpp also includes what it needs. Runs on a thread of its own,
// so the pills never wait for it.
// It loads starttap.dll into StartMenuExperienceHost and SearchHost, again whenever Windows restarts them, and keeps one
// glass window under whichever flyout is open. Both sit in the same place, and Search shows along with Start (it holds
// Start's search box), so one glass serves both: it follows Start while Start is shown, else Search.

namespace StartGlass
{
	constexpr ULONGLONG ShellWaitLimit = 30000;	// how long, in ms, Start and Search wait for our TAP in explorer before we load into them

	struct Surface
	{
		const wchar_t* name;
		const wchar_t* exe;
		const wchar_t* share;
		const wchar_t* movedName;
		bool container;	// the exe runs in an app container, which holds the share
		DWORD pid{};
		const StartShare* view{};
		HANDLE mapping{}, moved{};
		ULONGLONG retry{};
		std::atomic<HWND> watched{};	// for WatchCloaks
		std::atomic<DWORD> cloakState{ 1 };
		DWORD cloaked = 1;
		HWND window{};
		RECT rect{};
		bool onScreen{}, visible{};
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
		return open && open(&handle, event ? SYNCHRONIZE : SECTION_MAP_READ, &attributes) >= 0 ? handle : nullptr;
	}

	void Forget(Surface& surface)
	{
		if (surface.view) UnmapViewOfFile(surface.view);
		if (surface.mapping) CloseHandle(surface.mapping);
		if (surface.moved) CloseHandle(surface.moved);
		surface.view = nullptr;
		surface.mapping = surface.moved = nullptr;
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
		const DWORD pid = FindProcess(surface.exe);
		if (pid != surface.pid)
		{
			Forget(surface);
			surface.pid = pid;
			if (pid && !HasTap(pid)) LoadTap(surface);
		}
		if (!pid || surface.view) return;
		surface.mapping = surface.container ? OpenContainerObject(pid, surface.share, false) : OpenFileMappingW(FILE_MAP_READ, FALSE, surface.share);
		surface.view = surface.mapping ? static_cast<const StartShare*>(MapViewOfFile(surface.mapping, FILE_MAP_READ, 0, 0, sizeof(StartShare))) : nullptr;
		if (!surface.view) return;
		surface.moved = surface.container ? OpenContainerObject(pid, surface.movedName, true) : OpenEventW(SYNCHRONIZE, FALSE, surface.movedName);
		wchar_t line[96];
		swprintf_s(line, L"start glass: following %s", surface.name);
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
		surface.onScreen = !IsRectEmpty(&copy.menu) && copy.menu.top < monitor.rcWork.bottom;
		surface.visible = !surface.cloaked && surface.onScreen;
	}

	// Asking DWM whether Start is cloaked sometimes blocks for two seconds while Start opens, and the glass waited with it.
	// So a thread of its own asks, and the glass goes by its last answer, and by XAML, meanwhile.
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
			Sleep(16);
		}
	}

	HWND MakeGlass(HINSTANCE instance)
	{
		const HWND glass = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, StartGlassClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
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
			{ L"Start", L"StartMenuExperienceHost.exe", StartShareName, StartMovedName, false },
			{ L"Search", L"SearchHost.exe", SearchShareName, SearchMovedName, true },
		};
		Surface& start = surfaces[0];
		Surface& search = surfaces[1];
		std::thread(WatchCloaks, surfaces, std::size(surfaces)).detach();

		HWND glass{};
		bool shown{}, focused{}, closing{}, met{}, startWasUp{};
		DWORD startPid{}, searchPid{};
		RECT last{};
		ULONGLONG settingRead{};
		const ULONGLONG began = GetTickCount64();
		for (;;)
		{
			MSG message;
			while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
			// The setting is read live, like the pills' own. Without our window the TAPs give back the original acrylic.
			const ULONGLONG tick = GetTickCount64();
			if (tick - settingRead >= 500)
			{
				settingRead = tick;
				const bool enabled = ReadPillSetting(PillStartGlass) != 0;
				if (enabled && !glass) { glass = MakeGlass(instance); shown = false; Log(L"start glass: on"); }
				else if (!enabled && glass) { DestroyWindow(glass); glass = nullptr; Log(L"start glass: off"); }
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
			bool startShown = start.window && (start.cloaked == 0 || (start.onScreen && flyoutFocused));
			// Going over to Search, Start parks its menu a few frames before DWM reports it cloaked, and in that gap
			// the glass went away and came back. A Start that was up and has parked again has handed over to Search.
			if (!startShown) startWasUp = false;
			else if (start.onScreen) startWasUp = true;
			else if (startWasUp && search.visible) startShown = false;
			const bool open = startShown ? start.onScreen : search.visible;
			// XAML only moves a closing Start menu at the end of its slide, so the glass goes as soon as another window
			// takes the foreground. Focus moves between Start and Search, and through no window at all, on the way.
			if (!open) focused = closing = false;
			else if (flyoutFocused) focused = true;
			else if (focused) closing = true;
			const RECT& rect = startShown ? start.rect : search.rect;
			const bool show = open && !closing && met;
			if (show && (!shown || !EqualRect(&rect, &last)))
			{
				SetWindowPos(glass, HWND_TOPMOST, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
				last = rect;
				shown = true;
			}
			else if (!show && shown)
			{
				ShowWindow(glass, SW_HIDE);
				shown = false;
			}
			// Both TAPs say the moment their flyout moves, which is also how an opening shows up; while the glass is out,
			// cloaking and the foreground are checked every 8 ms.
			HANDLE moved[2];
			DWORD count{};
			for (const auto& surface : surfaces) if (surface.moved) moved[count++] = surface.moved;
			MsgWaitForMultipleObjects(count, moved, FALSE, shown ? 8 : 50, QS_ALLINPUT);
		}
	}
}
