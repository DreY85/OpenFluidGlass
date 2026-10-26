// Liquid Glass taskbar, our side. Loads pilltap.dll into explorer, then keeps one glass window
// (accent blur marked as a pill, which our OpenGlass rounds) under each group the TAP reports.
// While this runs the TAP hides the taskbar's own background; when it exits the background returns.
// Settings come from HKCU\Software\OpenGlass\Taskbar (the Taskbar tab of OpenGlassGUI) and apply while running.
// It also keeps glass under the Start menu and Search (startglass.h).
// pillhost.exe --stop closes a running copy.
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <dwmapi.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <iterator>
#include <thread>
#include "pilllog.h"
#include "pillshare.h"
#include "startshare.h"

struct ACCENT_POLICY { DWORD AccentState, AccentFlags, GradientColor, AnimationId; };
struct WINDOWCOMPOSITIONATTRIBDATA { DWORD Attrib; PVOID pvData; SIZE_T cbData; };
using SetWindowCompositionAttribute_t = BOOL(WINAPI*)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);

namespace
{
	constexpr DWORD AccentFlagPill = 0x80000000;	// OpenGlass: cut the glass to a fully rounded outline
	constexpr UINT_PTR FollowTimer = 1;
	constexpr UINT_PTR GlideTimer = 2;	// a faster tick while a pill glides

	// In taskbar units (pixels at 100% scale); see pillshare.h.
	struct Settings
	{
		int height, padding, gap;
		bool join, animate;
	};

	HWND g_host{};
	HWND g_pills[PillCount]{};
	HANDLE g_mapping{};
	const PillShare* g_share{};
	DWORD g_explorer{};
	Settings g_settings{};
	int g_ticks{};

	// Read twice a second, so the Taskbar tab's changes show while it is being tuned.
	// Turning the taskbar off there closes this host, which brings the normal taskbar back.
	void LoadSettings()
	{
		if (!ReadPillSetting(PillEnabled))
		{
			PostMessageW(g_host, WM_CLOSE, 0, 0);
			return;
		}
		g_settings.height = static_cast<int>(ReadPillSetting(PillHeight));
		g_settings.padding = static_cast<int>(ReadPillSetting(PillPadding));
		g_settings.gap = static_cast<int>(ReadPillSetting(PillGap));
		g_settings.join = ReadPillSetting(PillJoin) != 0;
		g_settings.animate = ReadPillSetting(PillAnimate) != 0;
	}

	// Rare events (attach attempts, nudges) to %TEMP%\pillhost.txt, timestamped like the TAP's log.
	void Log(const wchar_t* line)
	{
		FILE* file = OpenLog(L"pillhost.txt");
		if (!file) return;
		SYSTEMTIME now{};
		GetLocalTime(&now);
		fwprintf(file, L"%02u:%02u:%02u.%03u %s\n", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, line);
		fclose(file);
	}

	// Attaching to explorer. A freshly started explorer refuses the TAP until its taskbar's XAML starts,
	// so the host loads the TAP into it once, as early as it can, and the TAP keeps trying from inside. The host waits
	// for the TAP to say how that ended and, if it failed, tries again. Every attempt loads its own copy
	// of pilltap.dll, from the user's own tap folder, as XAML may keep refusing a file it refused once.
	enum class TapState { Idle, Waiting, Attached };
	TapState g_tap{ TapState::Idle };
	std::atomic<bool> g_shellReady{};	// the TAP got into explorer once; the Start glass waits for that (startglass.h)
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

	// %LOCALAPPDATA%\OpenGlass\tap\, made if missing, with a trailing backslash. The copies live in the user's own
	// profile because the exe may sit in Program Files, where a standard user cannot write, and because a folder
	// shared between users would let one of them swap the DLL another user's explorer is about to load.
	bool TapFolder(wchar_t (&path)[MAX_PATH])
	{
		wchar_t root[MAX_PATH];
		const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", root, MAX_PATH);
		if (!length || length >= MAX_PATH - 32) return false;
		swprintf_s(path, L"%s\\OpenGlass", root);
		CreateDirectoryW(path, nullptr);
		swprintf_s(path, L"%s\\OpenGlass\\tap", root);
		if (!CreateDirectoryW(path, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
		wcscat_s(path, L"\\");
		return true;
	}

	// Copies left by earlier runs; the ones explorer still holds stay locked and are skipped.
	void CleanTapCopies()
	{
		wchar_t folder[MAX_PATH], pattern[MAX_PATH];
		if (!TapFolder(folder)) return;
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

	// The copy the next attempt loads. The first one is made when the host starts, before explorer does:
	// at sign-in the disk and the antivirus are busy, and a copy made then took seconds.
	wchar_t g_staged[MAX_PATH]{};

	bool StageTap()
	{
		wchar_t folder[MAX_PATH], tap[MAX_PATH], source[MAX_PATH];
		HostFolder(folder);
		swprintf_s(source, L"%spilltap.dll", folder);
		if (!TapFolder(tap)) return false;
		swprintf_s(g_staged, L"%spilltap-%llu.dll", tap, GetTickCount64());
		if (CopyFileW(source, g_staged, FALSE)) return true;
		g_staged[0] = L'\0';
		return false;
	}

	// Has explorer load a copy of pilltap.dll on a thread of its own, started at LoadLibraryW, which
	// kernel32 maps at the same address in every process. A hook on the taskbar's thread would wait for
	// that thread to be free, and while explorer starts, it is busy building the taskbar for seconds.
	// The TAP drops this load's reference when its attempt ends; a successful attach pins the DLL.
	void Inject(DWORD pid)
	{
		if (!pid || (!g_staged[0] && !StageTap())) return;
		wchar_t dll[MAX_PATH];
		wcscpy_s(dll, g_staged);
		g_staged[0] = L'\0';
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
	// At sign-in the first nudge comes before the taskbar is fully built and goes unseen. Five seconds before the next
	// kept the pills away for eight seconds after the TAP attached, so the nudges now follow each other closely.
	constexpr ULONGLONG NudgeLimit = 2000, NudgeRetryDelay = 1000;
	constexpr int NudgeTries = 8;

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

	void Attach(DWORD explorer)
	{
		const ULONGLONG now = GetTickCount64();
		if (g_tap == TapState::Idle && explorer && now >= g_tapTime)
		{
			g_tapTime = now + TapRetryDelay;	// in case explorer cannot even be reached
			Inject(explorer);
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
			if (attached) { CleanTapCopies(); g_nudges = 0; g_shellReady = true; }
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

	RECT Transposed(const RECT& r)
	{
		return { r.top, r.left, r.bottom, r.right };
	}

	// Turns the groups the TAP reports into pill rectangles, relative to the taskbar. On a side of the screen
	// the taskbar is a tall band: there the layout runs on the groups turned on their side, then turns the pills back,
	// so "height" is the pill's thickness across the band and padding and gap run along it.
	void Layout(const RECT (&reported)[PillCount], int barWidth, int barHeight, double scale, RECT (&pills)[PillCount])
	{
		const bool vertical = barHeight > barWidth;
		RECT groups[PillCount];
		for (UINT i = 0; i < PillCount; ++i) groups[i] = vertical ? Transposed(reported[i]) : reported[i];
		if (vertical) barWidth = barHeight;
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
		if (vertical)
		{
			for (auto& pill : pills) pill = Transposed(pill);
		}
	}

	// The explorer that will become the shell, before it has built its taskbar: the newest explorer.exe
	// in this session. At sign-in, explorer starts about 20 s before the taskbar window appears, and a
	// TAP loaded right away is ready when the taskbar's XAML starts, while one loaded once the window is
	// up waits seconds for explorer, busy loading, to run it. Looked up four times a second at most.
	DWORD StartingShell()
	{
		static ULONGLONG checked{};
		static DWORD found{};
		const ULONGLONG now = GetTickCount64();
		if (now - checked < 250) return found;
		checked = now;
		found = 0;
		DWORD session{};
		ProcessIdToSessionId(GetCurrentProcessId(), &session);
		const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snapshot == INVALID_HANDLE_VALUE) return 0;
		PROCESSENTRY32W entry{ sizeof(entry) };
		ULONGLONG newest{};
		// A process younger than a second may still be initializing; a thread of ours would only wait for it.
		FILETIME clock{};
		GetSystemTimeAsFileTime(&clock);
		const ULONGLONG settled = ((static_cast<ULONGLONG>(clock.dwHighDateTime) << 32) | clock.dwLowDateTime) - 10'000'000;
		for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
		{
			DWORD processSession{};
			if (_wcsicmp(entry.szExeFile, L"explorer.exe") || !ProcessIdToSessionId(entry.th32ProcessID, &processSession) || processSession != session) continue;
			const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
			if (!process) continue;
			FILETIME created{}, exited{}, kernel{}, user{};
			if (GetProcessTimes(process, &created, &exited, &kernel, &user))
			{
				const ULONGLONG time = (static_cast<ULONGLONG>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
				if (time > newest && time <= settled) { newest = time; found = entry.th32ProcessID; }
			}
			CloseHandle(process);
		}
		CloseHandle(snapshot);
		return found;
	}

	// At sign-in the host starts while Windows' welcome screen (LogonUI) still covers the desktop, and the entrance
	// played under it: the log showed it run in full at two sign-ins nobody saw it at. It now waits for that screen to go,
	// but never longer than SignInWaitLimit, so a LogonUI left running (a locked session) cannot hold the pills back.
	// Once it has gone, the session is still busy for a moment and the taskbar icons play their own entrance:
	// an entrance started right then looked like a lag, with the pills rising along with the icons. One second was not
	// enough once the pills were ready early, on a saved layout: they came up 0.7 s after the welcome screen went, while
	// Windows was still fading from it to the desktop, and the screen froze mid-fade for 3 to 4 s.
	// The host watches for the welcome screen from its start: checked only once the pills were ready, it was sometimes
	// already gone at the first look, the sign-in passed for a restarted host, and the entrance played under the fade.
	constexpr ULONGLONG SignInWaitLimit = 60000;	// ms from the first check, the host's start
	constexpr ULONGLONG SignInSettle = 2000;	// ms after the welcome screen goes
	bool SignInShowing()
	{
		static ULONGLONG first{}, checked{}, gone{};
		static bool showing = true, seen = false;
		const ULONGLONG now = GetTickCount64();
		if (!first) first = now;
		if (gone) return now - gone < SignInSettle && now - first <= SignInWaitLimit;
		if (!showing || now - first > SignInWaitLimit) return showing = false;
		if (now - checked < 250) return showing;
		checked = now;
		DWORD session{};
		ProcessIdToSessionId(GetCurrentProcessId(), &session);
		const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snapshot == INVALID_HANDLE_VALUE) return showing = false;
		bool found = false;
		PROCESSENTRY32W entry{ sizeof(entry) };
		for (BOOL more = Process32FirstW(snapshot, &entry); more && !found; more = Process32NextW(snapshot, &entry))
		{
			DWORD processSession{};
			found = !_wcsicmp(entry.szExeFile, L"LogonUI.exe") && ProcessIdToSessionId(entry.th32ProcessID, &processSession) && processSession == session;
		}
		CloseHandle(snapshot);
		if (found && !seen) Log(L"sign-in screen up");
		if (found) seen = true;
		else
		{
			Log(L"sign-in screen gone");
			// Only a real sign-in waits for the session to settle; a restarted host starts at once.
			if (seen) { gone = now; return true; }
		}
		return showing = found;
	}

	// The welcome screen is often long gone by the time the pills are ready (22 s after sign-in once), yet the session
	// is still starting its apps: the host then went 600 ms without a frame right as the entrance began, and it stuttered.
	// The entrance waits until the host's own frames have come on time for a moment, but never more than SettleLimit
	// after the pills are ready. It used to wait for the processors too: they sat at 100% for the whole wait at a sign-in
	// while the frames came every 31 ms at worst, so the pills only showed late, and the entrance then played smoothly.
	constexpr ULONGLONG SettleLimit = 1500, SettleSpan = 300, SettleGap = 50;	// ms
	bool SessionSettled()
	{
		static ULONGLONG first{}, lastFrame{}, smoothSince{};
		static ULONGLONG longestGap{};	// for the log: what kept the session from counting as calm
		static bool settled = false;
		if (settled) return true;
		const ULONGLONG now = GetTickCount64();
		if (!first) first = now;
		const ULONGLONG gap = lastFrame ? now - lastFrame : SettleGap + 1;
		if (lastFrame) longestGap = (std::max)(longestGap, gap);
		lastFrame = now;
		wchar_t line[128];
		if (now - first > SettleLimit)
		{
			swprintf_s(line, L"frames still late after %llu ms, entering anyway (longest gap %llu ms)", now - first, longestGap);
			Log(line);
			return settled = true;
		}
		if (gap > SettleGap) smoothSince = now;
		if (now - smoothSince < SettleSpan) return false;
		swprintf_s(line, L"frames on time after %llu ms", now - first);
		Log(line);
		return settled = true;
	}

	// Each pill moves to where the layout puts it on springs instead of jumping there. When an app opens or closes,
	// Windows slides the icons over about a quarter of a second, but the layout the TAP reads changes in steps
	// (closing one app gave four steps in 900 ms), and a pill that jumped to each step shrank, overshot and grew back.
	// Every edge is a damped spring: it settles in about a third of a second and runs about a tenth past the target
	// first, the springy feel of Apple's motion. While a pill stretches or shrinks fast it also gets a little thinner,
	// as a liquid keeping its volume would. The first time the pills show after the host starts (at sign-in) each one
	// grows from a drop at its middle, the apps first and the tray a moment later.
	constexpr double SpringFrequency = 18.0;	// rad/s
	// The entrance is slower: at the everyday speed a pill grew in 170 ms, too fast to see.
	// At 8 rad/s it went by too fast to see where it started, at 4.5 (two seconds in all) it dragged.
	constexpr double EntranceFrequency = 7.5;	// rad/s; with the stagger, about 1.25 s in all
	constexpr double SpringDamping = 0.6;	// of critical; below 1 it overshoots
	constexpr double EntranceDamping = 0.8;	// the long entrance stretch would overshoot too far at 0.6
	constexpr double SquashSpeed = 600.0;	// px/s of length change that gives the full squash
	constexpr double SquashAmount = 0.12;	// of the thickness at most
	constexpr double EntranceStagger = 0.2;	// s between the pills' entrances

	struct Edge
	{
		double position, velocity;
	};
	struct Motion
	{
		Edge edges[4];	// left, top, right, bottom
		double frequency, damping;
		double delay;	// s before an entrance starts; the pill stays hidden until then
		bool valid;
	};
	Motion g_motion[PillCount]{};
	RECT g_lastBar{};
	double g_lastStep{};	// s, from Seconds; 0 before the first placement
	bool g_entranceDone{};
	// For the log: when the entrance began and how many frames it took, to tell an entrance that played at sign-in,
	// while the session was still busy, from one that never started.
	double g_entranceStart{};
	// Also for the log: whether DWM stopped composing around the entrance, the way the screen froze for seconds once.
	// While the entrance plays the pills move every frame, so DWM must compose; while it waits nothing of ours moves,
	// and a still screen there may just be a screen with nothing to update.
	struct DwmWatch
	{
		bool active;
		UINT64 frame;	// DWM's count of composed frames
		ULONGLONG frameAt, hostAt;	// ms, when it last changed and when the host last looked
		ULONGLONG waitingLongest, enteringLongest, hostLongest;
	} g_dwmWatch{};
	void WatchDwm(bool entering)
	{
		DWM_TIMING_INFO info{ sizeof(info) };
		if (FAILED(DwmGetCompositionTimingInfo(nullptr, &info))) return;
		const ULONGLONG now = GetTickCount64();
		auto& w = g_dwmWatch;
		if (!w.active)
		{
			w.active = true;
			w.frame = info.cFrame;
			w.frameAt = w.hostAt = now;
			return;
		}
		w.hostLongest = (std::max)(w.hostLongest, now - w.hostAt);
		w.hostAt = now;
		if (info.cFrame == w.frame) return;
		auto& longest = entering ? w.enteringLongest : w.waitingLongest;
		longest = (std::max)(longest, now - w.frameAt);
		w.frame = info.cFrame;
		w.frameAt = now;
	}
	int g_entranceFrames{};
	bool g_entranceLogged{};
	RECT g_entranceTarget{};

	double Seconds()
	{
		LARGE_INTEGER counter{}, frequency{};
		QueryPerformanceCounter(&counter);
		QueryPerformanceFrequency(&frequency);
		return static_cast<double>(counter.QuadPart) / frequency.QuadPart;
	}

	// Advances one edge toward its goal, in 4 ms steps so a long frame cannot make the spring blow up.
	// Returns whether it is still moving.
	bool StepSpring(Edge& edge, double goal, double frequency, double damping, double elapsed)
	{
		const double stiffness = frequency * frequency;
		const double friction = 2.0 * damping * frequency;
		for (double left = elapsed; left > 0.0; left -= 0.004)
		{
			const double h = (std::min)(left, 0.004);
			edge.velocity += (-stiffness * (edge.position - goal) - friction * edge.velocity) * h;
			edge.position += edge.velocity * h;
		}
		if (std::abs(edge.position - goal) < 0.25 && std::abs(edge.velocity) < 5.0)
		{
			edge = { goal, 0.0 };
			return false;
		}
		return true;
	}

	void SetMotion(Motion& motion, const RECT& r, double frequency, double damping, double delay)
	{
		motion = { { { double(r.left), 0 }, { double(r.top), 0 }, { double(r.right), 0 }, { double(r.bottom), 0 } }, frequency, damping, delay, true };
	}

	// Moves the pill's edges toward the target; returns where to put the pill now and whether it is still moving.
	RECT StepMotion(Motion& motion, const RECT& target, bool vertical, double elapsed, bool& moving)
	{
		const double goals[4]{ double(target.left), double(target.top), double(target.right), double(target.bottom) };
		bool any = false;
		for (int i = 0; i < 4; ++i) any = StepSpring(motion.edges[i], goals[i], motion.frequency, motion.damping, elapsed) || any;
		moving = any;
		const Edge* e = motion.edges;
		double place[4]{ e[0].position, e[1].position, e[2].position, e[3].position };
		// Along the band the pill keeps its springs; across it, it thins while its length changes fast.
		const int startAlong = vertical ? 1 : 0, startAcross = vertical ? 0 : 1;
		const double lengthSpeed = std::abs(e[startAlong + 2].velocity - e[startAlong].velocity);
		const double squash = SquashAmount * (std::min)(lengthSpeed / SquashSpeed, 1.0);
		const double middle = (place[startAcross] + place[startAcross + 2]) / 2.0;
		const double half = (place[startAcross + 2] - place[startAcross]) / 2.0 * (1.0 - squash);
		place[startAcross] = middle - half;
		place[startAcross + 2] = middle + half;
		return { std::lround(place[0]), std::lround(place[1]), std::lround(place[2]), std::lround(place[3]) };
	}

	bool g_trayShownLogged{};

	// Where the groups were when the pills last showed, kept for the next sign-in. There the TAP is in a few seconds
	// before the taskbar notices a nudge and lets it report (2.8 s at one sign-in, with the icons already up), so the
	// pills show on the saved layout as soon as the TAP is in, and their springs carry them over once it reports.
	// Only a taskbar of the same size and scale uses it; the saved app group may be off by the apps that started since.
	constexpr wchar_t PillLastLayoutValue[] = L"LastLayout";
	struct PillLayout
	{
		RECT groups[PillCount];
		LONG barWidth, barHeight;
		UINT dpi;
	};
	PillLayout g_savedLayout{};
	ULONGLONG g_savedAt{};
	constexpr ULONGLONG SaveLayoutDelay = 2000;	// ms between writes, while apps open and close

	PillLayout MakeLayout(const RECT (&groups)[PillCount], const RECT& bar, UINT dpi)
	{
		PillLayout layout{};
		memcpy(layout.groups, groups, sizeof(layout.groups));
		layout.barWidth = bar.right - bar.left;
		layout.barHeight = bar.bottom - bar.top;
		layout.dpi = dpi;
		return layout;
	}

	void SaveLayout(const RECT (&groups)[PillCount], const RECT& bar, UINT dpi)
	{
		const PillLayout layout = MakeLayout(groups, bar, dpi);
		const ULONGLONG now = GetTickCount64();
		// A change skipped for being too soon is written on a later frame: the next call sees it differ still.
		if (!memcmp(&layout, &g_savedLayout, sizeof(layout)) || now - g_savedAt < SaveLayoutDelay) return;
		if (RegSetKeyValueW(HKEY_CURRENT_USER, PillSettingsKey, PillLastLayoutValue, REG_BINARY, &layout, sizeof(layout)) == ERROR_SUCCESS)
		{
			g_savedLayout = layout;
			g_savedAt = now;
		}
	}

	bool SavedLayout(RECT (&groups)[PillCount], const RECT& bar, UINT dpi)
	{
		static bool read = false, logged = false;
		static PillLayout saved{};
		if (!read)
		{
			read = true;
			DWORD size = sizeof(saved);
			if (RegGetValueW(HKEY_CURRENT_USER, PillSettingsKey, PillLastLayoutValue, RRF_RT_REG_BINARY, nullptr, &saved, &size) != ERROR_SUCCESS || size != sizeof(saved)) saved = {};
			g_savedLayout = saved;	// what is on disk already needs no writing
		}
		const PillLayout current = MakeLayout(groups, bar, dpi);
		if (!saved.dpi || saved.dpi != current.dpi || saved.barWidth != current.barWidth || saved.barHeight != current.barHeight) return false;
		memcpy(groups, saved.groups, sizeof(groups));
		if (!logged) { Log(L"showing the saved layout until the TAP reports"); logged = true; }
		return true;
	}

	// Places the pills right under the taskbar, in its band (topmost or not), following it as it moves.
	void Follow()
	{
		if (++g_ticks % 15 == 1) LoadSettings();
		if (!g_entranceDone) SignInShowing();
		const HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
		DWORD pid{};
		if (tray) GetWindowThreadProcessId(tray, &pid);
		else pid = StartingShell();
		if (pid != g_explorer)
		{
			// Explorer (re)started: the old TAP is gone with it.
			Log(tray || !pid ? L"new explorer" : L"new explorer, taskbar not built yet");
			Disconnect();
			EndNudge();
			ReleaseTap();
			g_explorer = pid;
			g_tap = TapState::Idle;
			g_tapTime = GetTickCount64();
			g_trayShownLogged = false;
		}
		// For the log: how long the taskbar shows without pills while the TAP is still on its way.
		if (!g_trayShownLogged && tray && IsWindowVisible(tray))
		{
			Log(L"taskbar window shown");
			g_trayShownLogged = true;
		}
		Attach(pid);

		RECT groups[PillCount]{}, pills[PillCount]{};
		RECT bar{};
		const bool visible = tray && IsWindowVisible(tray) && GetWindowRect(tray, &bar);
		const UINT dpi = visible ? GetDpiForWindow(tray) : 0;
		bool shown = visible && ReadGroups(groups);
		if (shown) SaveLayout(groups, bar, dpi);
		// The saved layout shows as soon as the taskbar does: waiting for the TAP to attach kept the pills away 6.4 s after
		// the taskbar showed at one sign-in, and how long that takes depends on the machine.
		else if (visible) shown = SavedLayout(groups, bar, dpi);
		if (shown) Layout(groups, bar.right - bar.left, bar.bottom - bar.top, dpi / 96.0, pills);
		const bool topmost = tray && (GetWindowLongW(tray, GWL_EXSTYLE) & WS_EX_TOPMOST);
		// A pill that shows again, or a taskbar moved to another side or screen, jumps straight to its place.
		const bool barMoved = !EqualRect(&bar, &g_lastBar);
		g_lastBar = bar;
		const bool vertical = bar.bottom - bar.top > bar.right - bar.left;
		const double now = Seconds();
		const double elapsed = g_lastStep > 0.0 ? std::clamp(now - g_lastStep, 0.0, 0.1) : 0.0;
		g_lastStep = now;
		bool animating = false, entering = false, placed = false;
		// Asked once a frame, and only once a pill is ready to enter.
		int holdState = -1;
		const auto holdEntrance = [&holdState] { if (holdState < 0) holdState = SignInShowing() || !SessionSettled(); return holdState == 1; };
		HWND above = tray;
		for (UINT index = 0; index < PillCount; ++index)
		{
			const HWND pill = g_pills[index];
			const RECT& r = pills[index];
			Motion& motion = g_motion[index];
			if (r.right <= r.left)
			{
				if (motion.valid && g_entranceStart > 0.0 && !g_entranceLogged)
				{
					wchar_t line[96];
					swprintf_s(line, L"pill %u lost its place: %s", index, shown ? L"empty in the layout" : L"no taskbar or groups");
					Log(line);
				}
				motion.valid = false;
				if (IsWindowVisible(pill)) ShowWindow(pill, SW_HIDE);
				continue;
			}
			if (((GetWindowLongW(pill, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) != topmost)
				SetWindowPos(pill, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
			const RECT target{ bar.left + r.left, bar.top + r.top, bar.left + r.right, bar.top + r.bottom };
			// For the log: where the apps pill is headed while the entrance plays, to tell a taskbar that slides in
			// at sign-in from a layout that is still settling.
			if (index == 0 && g_entranceStart > 0.0 && !g_entranceLogged && !EqualRect(&target, &g_entranceTarget))
			{
				g_entranceTarget = target;
				wchar_t line[128];
				swprintf_s(line, L"apps pill heads to %ld,%ld,%ld,%ld, taskbar %ld,%ld,%ld,%ld", target.left, target.top, target.right, target.bottom, bar.left, bar.top, bar.right, bar.bottom);
				Log(line);
			}
			if (!g_settings.animate)
			{
				SetMotion(motion, target, SpringFrequency, SpringDamping, 0.0);
			}
			else if (!g_entranceDone && !motion.valid && holdEntrance())
			{
				// The entrance waits for the welcome screen to go and the session to calm down; the pills stay hidden meanwhile.
				if (IsWindowVisible(pill)) ShowWindow(pill, SW_HIDE);
				entering = true;
				animating = true;
				continue;
			}
			else if (!g_entranceDone && !motion.valid)
			{
				// The entrance: a drop as long as the pill is thick, at its middle, waiting its turn.
				RECT drop = target;
				const LONG thickness = vertical ? target.right - target.left : target.bottom - target.top;
				if (vertical) { const LONG middle = (target.top + target.bottom) / 2; drop.top = middle - thickness / 2; drop.bottom = drop.top + thickness; }
				else { const LONG middle = (target.left + target.right) / 2; drop.left = middle - thickness / 2; drop.right = drop.left + thickness; }
				SetMotion(motion, drop, EntranceFrequency, EntranceDamping, EntranceStagger * index);
				if (g_entranceStart == 0.0)
				{
					g_entranceStart = now;
					wchar_t line[128];
					swprintf_s(line, L"entrance started: taskbar %ld,%ld,%ld,%ld", bar.left, bar.top, bar.right, bar.bottom);
					Log(line);
				}
			}
			else if (!motion.valid || barMoved || !IsWindowVisible(pill) && motion.delay <= 0.0)
			{
				// For the log: a pill that jumps while the entrance plays (it once lasted 0.44 s, the right side instant).
				if (g_entranceStart > 0.0 && !g_entranceLogged)
				{
					wchar_t line[128];
					swprintf_s(line, L"pill %u jumps into place: %s, taskbar %ld,%ld,%ld,%ld", index,
						!motion.valid ? L"no motion" : barMoved ? L"taskbar moved" : L"was hidden", bar.left, bar.top, bar.right, bar.bottom);
					Log(line);
				}
				SetMotion(motion, target, SpringFrequency, SpringDamping, 0.0);
			}
			if (motion.delay > 0.0)
			{
				motion.delay -= elapsed;
				// Still waiting: stay hidden. Once the wait is over the pill shows in this same frame, as the drop;
				// hidden one frame more, the next one would take it for a pill showing again and jump it into place.
				if (motion.delay > 0.0)
				{
					entering = true;
					animating = true;
					continue;
				}
			}
			bool moving = false;
			const RECT want = StepMotion(motion, target, vertical, elapsed, moving);
			if (moving) animating = true;
			else
			{
				motion.frequency = SpringFrequency;
				motion.damping = SpringDamping;
			}
			RECT current{};
			GetWindowRect(pill, &current);
			if (!IsWindowVisible(pill) || !EqualRect(&current, &want) || GetWindow(pill, GW_HWNDPREV) != above)
				SetWindowPos(pill, above, want.left, want.top, want.right - want.left, want.bottom - want.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
			above = pill;
			placed = true;
		}
		// The entrance plays once, when the pills first show after the host starts; an Explorer restart does not replay it.
		// It counts as played only once a pill is up: until the TAP has reported, the taskbar shows with no pills at all.
		if (placed && !entering) g_entranceDone = true;
		const bool entranceRunning = g_entranceStart > 0.0 && !g_entranceLogged;
		if (holdState == 1 || entranceRunning) WatchDwm(entranceRunning);
		if (entranceRunning)
		{
			++g_entranceFrames;
			if (g_entranceDone && !animating)
			{
				wchar_t line[192];
				const auto& w = g_dwmWatch;
				swprintf_s(line, L"entrance done: %.0f ms, %d frames; longest pause, host %llu ms, DWM %llu ms while waiting and %llu ms during the entrance",
					(now - g_entranceStart) * 1000.0, g_entranceFrames, w.hostLongest, w.waitingLongest, w.enteringLongest);
				Log(line);
				g_entranceLogged = true;
			}
		}
		if (animating) SetTimer(g_host, GlideTimer, 16, nullptr);
		else KillTimer(g_host, GlideTimer);
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
		// Peek (hovering a taskbar thumbnail, or the corner) fades every window but the one shown and the taskbar;
		// the pills went with the rest and left the bare taskbar behind. They stay, like the taskbar.
		const BOOL excluded = TRUE;
		DwmSetWindowAttribute(window, DWMWA_EXCLUDED_FROM_PEEK, &excluded, sizeof(excluded));
		return window;
	}

#include "startglass.h"
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int)
{
	if (wcsstr(commandLine, L"--stop"))
	{
		if (const HWND running = FindWindowW(PillHostClass, nullptr)) PostMessageW(running, WM_CLOSE, 0, 0);
		return 0;
	}
	if (FindWindowW(PillHostClass, nullptr) || !ReadPillSetting(PillEnabled)) return 0;
	// The logon task starts us below normal, and at sign-in that left the host waiting
	// seconds behind everything else while the old taskbar showed.
	SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
	MEMORY_PRIORITY_INFORMATION memory{ MEMORY_PRIORITY_NORMAL };
	SetProcessInformation(GetCurrentProcess(), ProcessMemoryPriority, &memory, sizeof(memory));
	Log(L"host started");
	// Pixels everywhere: the TAP reports physical pixels and the taskbar rectangle is physical too.
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	wchar_t self[MAX_PATH];
	const DWORD length = GetModuleFileNameW(nullptr, self, MAX_PATH);
	RegSetKeyValueW(HKEY_CURRENT_USER, PillSettingsKey, PillHostPathValue, REG_SZ, self, (length + 1) * sizeof(wchar_t));
	CleanTapCopies();
	StageTap();

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
	// Glass under the Start menu and Search; it ends with the process.
	std::thread(StartGlass::Run, instance).detach();

	MSG message;
	while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
	EndNudge();
	ReleaseTap();
	Disconnect();
	return 0;
}
