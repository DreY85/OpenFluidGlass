// Liquid Glass taskbar, explorer side. Hides the taskbar's own background while pillhost.exe runs
// and publishes where the groups are (apps, tray, clock) so the host can draw a glass pill under each.
// Loaded into explorer by a short WH_CALLWNDPROC hook, then attaches itself as a XAML diagnostics TAP.
// It never touches an element while it is being added, only its parent, which is already in the tree.
// The approach follows TranslucentTB's ExplorerTAP (GPL-3.0, https://github.com/TranslucentTB/TranslucentTB).
#include <windows.h>
#include <ocidl.h>
#include <xamlOM.h>
#undef GetCurrentTime	// windows.h macro clashes with C++/WinRT
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include "pillshare.h"

namespace wux = winrt::Windows::UI::Xaml;

// {5E0C2A37-7F3B-4B7C-9A3D-0B6F2F6E1B02}
static const CLSID CLSID_PillTap = { 0x5e0c2a37, 0x7f3b, 0x4b7c, { 0x9a, 0x3d, 0x0b, 0x6f, 0x2f, 0x6e, 0x1b, 0x02 } };
using InitializeXamlDiagnosticsEx_t = HRESULT(WINAPI*)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, CLSID, LPCWSTR);

namespace
{
	HMODULE g_module{};
	std::mutex g_logLock;

	// Rare events only (attach, found, errors), so writing straight to the file is fine.
	void Log(const std::wstring& line)
	{
		std::lock_guard guard{ g_logLock };
		wchar_t path[MAX_PATH];
		GetTempPathW(MAX_PATH, path);
		wcscat_s(path, L"pilltap.txt");
		FILE* file{};
		if (_wfopen_s(&file, path, L"a, ccs=UTF-8") || !file) return;
		fputws((line + L"\n").c_str(), file);
		fclose(file);
	}

	class Tap : public IObjectWithSite, public IVisualTreeServiceCallback
	{
	public:
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
		{
			if (riid == IID_IUnknown || riid == IID_IObjectWithSite) *out = static_cast<IObjectWithSite*>(this);
			else if (riid == __uuidof(IVisualTreeServiceCallback)) *out = static_cast<IVisualTreeServiceCallback*>(this);
			else { *out = nullptr; return E_NOINTERFACE; }
			AddRef();
			return S_OK;
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
		ULONG STDMETHODCALLTYPE Release() override { const ULONG refs = --m_refs; if (!refs) delete this; return refs; }

		HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) override
		{
			if (m_site) { m_site->Release(); m_site = nullptr; }
			if (!site) return S_OK;
			site->AddRef();
			m_site = site;
			// Lives as long as explorer: the handlers below keep using it.
			AddRef();
			std::thread([this]
			{
				IVisualTreeService3* tree{};
				if (SUCCEEDED(m_site->QueryInterface(__uuidof(IVisualTreeService3), reinterpret_cast<void**>(&tree))))
				{
					const HRESULT hr = tree->AdviseVisualTreeChange(this);
					Log(L"advise 0x" + std::to_wstring(static_cast<unsigned long>(hr)));
				}
			}).detach();
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** out) override { return m_site ? m_site->QueryInterface(riid, out) : E_FAIL; }

		// Runs on the taskbar's UI thread.
		HRESULT STDMETHODCALLTYPE OnVisualTreeChange(ParentChildRelation relation, VisualElement element, VisualMutationType mutation) override
		{
			if (mutation == Add && !m_started && relation.Parent && m_checked.size() < 4000 && m_checked.insert(relation.Parent).second)
			{
				Find(relation.Parent);
			}
			// The strings are ours to free.
			SysFreeString(element.Type);
			SysFreeString(element.Name);
			SysFreeString(element.SrcInfo.FileName);
			SysFreeString(element.SrcInfo.Hash);
			return S_OK;
		}

	private:
		// Climbs from a parent that just gained a child; if it sits in the taskbar, reads the taskbar's parts.
		void Find(InstanceHandle handle)
		{
			try
			{
				IXamlDiagnostics* diagnostics{};
				if (FAILED(m_site->QueryInterface(__uuidof(IXamlDiagnostics), reinterpret_cast<void**>(&diagnostics)))) return;
				winrt::Windows::Foundation::IInspectable object;
				const HRESULT hr = diagnostics->GetIInspectableFromHandle(handle, reinterpret_cast<::IInspectable**>(winrt::put_abi(object)));
				diagnostics->Release();
				if (FAILED(hr) || !object) return;
				auto current = object.try_as<wux::DependencyObject>();
				wux::DependencyObject top{ nullptr };
				bool taskbar = false;
				for (int depth = 0; current && depth < 60; ++depth)
				{
					const std::wstring name(winrt::get_class_name(current));
					if (name == L"Taskbar.TaskbarFrame" || name.rfind(L"SystemTray.", 0) == 0) taskbar = true;
					top = current;
					current = wux::Media::VisualTreeHelper::GetParent(current);
				}
				if (!top) return;
				// The roots XAML lists when the TAP attaches are already in place, not being added, so each
				// new root is searched downwards once in case it holds the taskbar. On build 26200 the
				// taskbar's island is not among them, and the host's nudge brings an addition inside it,
				// which names the taskbar on the way up.
				if (!taskbar && !m_roots.insert(winrt::get_abi(top)).second) return;
				m_frame = m_fill = m_stroke = m_repeater = m_trayGrid = nullptr;
				m_nodes = 0;
				Collect(top, 0);
				if (m_frame && m_fill && m_repeater && m_trayGrid) Start();
				else if (taskbar) Log(L"taskbar found but parts missing");
			}
			catch (...) { Log(L"find threw"); }
		}

		void Collect(const wux::DependencyObject& node, int depth)
		{
			if (depth > 30 || ++m_nodes > 4000) return;
			if (const auto element = node.try_as<wux::FrameworkElement>())
			{
				const std::wstring name(element.Name());
				if (!m_frame && winrt::get_class_name(node) == L"Taskbar.TaskbarFrame") m_frame = element;
				else if (!m_fill && name == L"BackgroundFill") m_fill = element;
				else if (!m_stroke && name == L"BackgroundStroke") m_stroke = element;
				else if (!m_repeater && name == L"TaskbarFrameRepeater") { m_repeater = element; return; }
				else if (!m_trayGrid && name == L"SystemTrayFrameGrid") { m_trayGrid = element; return; }
			}
			const int count = wux::Media::VisualTreeHelper::GetChildrenCount(node);
			for (int index = 0; index < count; ++index)
				Collect(wux::Media::VisualTreeHelper::GetChild(node, index), depth + 1);
		}

		void Start()
		{
			m_started = true;
			m_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(PillShare), PillShareName);
			m_share = m_mapping ? static_cast<PillShare*>(MapViewOfFile(m_mapping, FILE_MAP_WRITE, 0, 0, sizeof(PillShare))) : nullptr;
			Log(m_share ? L"taskbar found, publishing" : L"taskbar found, shared memory failed");
			// After every layout pass (an app opens, an icon appears) and twice a second to notice the host.
			m_frame.LayoutUpdated([this](auto&&, auto&&) { Update(); });
			m_timer = wux::DispatcherTimer();
			m_timer.Interval(std::chrono::milliseconds(500));
			m_timer.Tick([this](auto&&, auto&&) { Update(); });
			m_timer.Start();
			Update();
		}

		static void Hide(const wux::FrameworkElement& element, bool hide)
		{
			if (!element) return;
			const double opacity = hide ? 0.0 : 1.0;
			if (element.Opacity() != opacity) element.Opacity(opacity);
		}

		// Bounds of an element on the taskbar, in DIPs; empty when it takes no room.
		static bool Bounds(const wux::FrameworkElement& element, winrt::Windows::Foundation::Rect& out)
		{
			if (element.Visibility() != wux::Visibility::Visible || element.ActualWidth() < 1 || element.ActualHeight() < 1) return false;
			out = element.TransformToVisual(nullptr).TransformBounds({ 0, 0, static_cast<float>(element.ActualWidth()), static_cast<float>(element.ActualHeight()) });
			return true;
		}

		struct Span
		{
			double left = 1e9, right = -1e9;
			void Add(const winrt::Windows::Foundation::Rect& r) { left = (std::min)(left, double(r.X)); right = (std::max)(right, double(r.X + r.Width)); }
			bool Empty() const { return right <= left; }
		};

		void Update()
		{
			try
			{
				const HWND host = FindWindowW(PillHostClass, nullptr);
				Hide(m_fill, host != nullptr);
				Hide(m_stroke, host != nullptr);
				if (!host || !m_share)
				{
					m_published = false;
					return;
				}

				winrt::Windows::Foundation::Rect frame{};
				if (!Bounds(m_frame, frame)) return;
				const double scale = m_frame.XamlRoot() ? m_frame.XamlRoot().RasterizationScale() : 1.0;

				// Apps: Start and every task button laid out on the taskbar. The repeater keeps
				// recycled buttons around off to the side, so only the ones inside the frame count.
				Span apps;
				const int count = wux::Media::VisualTreeHelper::GetChildrenCount(m_repeater);
				for (int index = 0; index < count; ++index)
				{
					const auto child = wux::Media::VisualTreeHelper::GetChild(m_repeater, index).try_as<wux::FrameworkElement>();
					winrt::Windows::Foundation::Rect r{};
					if (child && Bounds(child, r) && r.X >= frame.X - 1 && r.X + r.Width <= frame.X + frame.Width + 1 && r.Y >= frame.Y - 1 && r.Y < frame.Y + frame.Height)
						apps.Add(r);
				}

				// Tray: icons, language, wifi and volume. Clock: the date, time and bell button.
				Span tray, clock;
				const int trayCount = wux::Media::VisualTreeHelper::GetChildrenCount(m_trayGrid);
				for (int index = 0; index < trayCount; ++index)
				{
					const auto child = wux::Media::VisualTreeHelper::GetChild(m_trayGrid, index).try_as<wux::FrameworkElement>();
					winrt::Windows::Foundation::Rect r{};
					if (!child || !Bounds(child, r)) continue;
					const std::wstring name(child.Name());
					if (name == L"ShowDesktopStack") continue;
					if (name == L"NotificationCenterButton" || name == L"SecondaryClockStack") clock.Add(r);
					else tray.Add(r);
				}

				auto toPixels = [&](const Span& span) -> RECT
				{
					if (span.Empty()) return {};
					return { lround(span.left * scale), lround(frame.Y * scale), lround(span.right * scale), lround((frame.Y + frame.Height) * scale) };
				};
				const RECT next[PillCount]{ toPixels(apps), toPixels(tray), toPixels(clock) };

				if (m_published && memcmp(next, m_last, sizeof(next)) == 0) return;
				memcpy(m_last, next, sizeof(next));
				m_published = true;
				InterlockedIncrement(&m_share->sequence);
				memcpy(const_cast<RECT*>(m_share->groups), next, sizeof(next));
				InterlockedIncrement(&m_share->sequence);
				PostMessageW(host, PillChanged, 0, 0);
			}
			catch (...)
			{
				if (!m_updateFailed) Log(L"update threw");
				m_updateFailed = true;
			}
		}

		std::atomic<ULONG> m_refs{ 1 };
		IUnknown* m_site{};
		std::set<InstanceHandle> m_checked;
		std::set<void*> m_roots;	// roots already searched downwards
		int m_nodes{};
		bool m_started{};
		bool m_published{};
		bool m_updateFailed{};
		wux::FrameworkElement m_frame{ nullptr }, m_fill{ nullptr }, m_stroke{ nullptr }, m_repeater{ nullptr }, m_trayGrid{ nullptr };
		wux::DispatcherTimer m_timer{ nullptr };
		HANDLE m_mapping{};
		PillShare* m_share{};
		RECT m_last[PillCount]{};
	};

	class Factory : public IClassFactory
	{
	public:
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
		{
			if (riid == IID_IUnknown || riid == IID_IClassFactory) { *out = this; return S_OK; }
			*out = nullptr;
			return E_NOINTERFACE;
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
		ULONG STDMETHODCALLTYPE Release() override { return 1; }
		HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** out) override
		{
			if (outer) return CLASS_E_NOAGGREGATION;
			auto* tap = new Tap();
			const HRESULT hr = tap->QueryInterface(riid, out);
			tap->Release();
			return hr;
		}
		HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }
	};
	Factory g_factory;

	// Tells the host how the attempt ended. On success Windows has pinned this DLL, so the host can
	// drop its hook. On failure dropping the hook unloads the DLL, and the host tries again later.
	void Report(bool attached)
	{
		wchar_t name[96];
		const auto set = [&name]
		{
			if (const HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name)) { SetEvent(event); CloseHandle(event); }
		};
		if (attached) { PillEventName(name, GetCurrentProcessId(), L"Attached"); set(); }
		PillEventName(name, GetCurrentProcessId(), L"Done");
		set();
	}

	DWORD Attach()
	{
		wchar_t location[MAX_PATH];
		GetModuleFileNameW(g_module, location, MAX_PATH);
		const HMODULE xaml = LoadLibraryExW(L"Windows.UI.Xaml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
		const auto initialize = xaml ? reinterpret_cast<InitializeXamlDiagnosticsEx_t>(GetProcAddress(xaml, "InitializeXamlDiagnosticsEx")) : nullptr;
		if (!initialize) { Log(L"no InitializeXamlDiagnosticsEx"); Report(false); return 1; }
		HRESULT hr{};
		for (int attempt = 1; attempt <= 60; ++attempt)
		{
			// Diagnostics initialize once per thread, so every attempt gets a new one.
			// Windows only accepts endpoint names like Visual Studio's (a name of our own fails with 0x80070490).
			const std::wstring connection = L"VisualDiagConnection" + std::to_wstring(attempt);
			std::thread([&] { hr = initialize(connection.c_str(), GetCurrentProcessId(), nullptr, location, CLSID_PillTap, nullptr); }).join();
			if (SUCCEEDED(hr)) { Log(L"attached on attempt " + std::to_wstring(attempt)); Report(true); return 0; }
			Sleep(500);
		}
		Log(L"could not attach, last error 0x" + std::to_wstring(static_cast<unsigned long>(hr)));
		Report(false);
		return 1;
	}

	// The thread holds its own reference to the DLL (taken in DllMain), so the DLL cannot unload
	// under it when the hook goes away early, for example because the host exited.
	DWORD WINAPI Install(void*)
	{
		FreeLibraryAndExitThread(g_module, Attach());
	}
}

extern "C" LRESULT CALLBACK TapHook(int code, WPARAM wParam, LPARAM lParam)
{
	return CallNextHookEx(nullptr, code, wParam, lParam);
}

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID riid, void** out)
{
	if (clsid != CLSID_PillTap) return CLASS_E_CLASSNOTAVAILABLE;
	return g_factory.QueryInterface(riid, out);
}

extern "C" HRESULT __stdcall DllCanUnloadNow() { return S_FALSE; }

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, void*)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		g_module = module;
		wchar_t exe[MAX_PATH];
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		const wchar_t* name = wcsrchr(exe, L'\\');
		// Only explorer installs; the host loads the DLL just to hand its hook to Windows.
		// No pinning here: a successful attach pins the DLL, a failed one must leave it free to
		// unload with the hook, or explorer keeps this dead copy and never loads a new one.
		HMODULE self{};
		if (name && _wcsicmp(name + 1, L"explorer.exe") == 0 &&
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&Install), &self))
		{
			// If the thread cannot start, the extra reference only keeps an idle copy loaded.
			if (const HANDLE thread = CreateThread(nullptr, 0, Install, nullptr, 0, nullptr)) CloseHandle(thread);
		}
	}
	return TRUE;
}
