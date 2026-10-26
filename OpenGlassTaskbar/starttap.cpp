// Liquid Glass Start menu and Search, inside StartMenuExperienceHost or SearchHost. Loaded by pillhost.exe, attaches as
// a XAML diagnostics TAP, swaps the flyout's own acrylic for a clear fill and publishes where the flyout is on screen,
// so pillhost can keep a glass window under it. While pillhost's glass window is gone (the host stopped, or the
// setting is off), the original acrylic comes back.
// The approach follows TranslucentTB's ExplorerTAP (GPL-3.0, https://github.com/TranslucentTB/TranslucentTB).
#include <windows.h>
#include <ocidl.h>
#include <xamlOM.h>
#include <CoreWindow.h>
#undef GetCurrentTime	// windows.h macro clashes with C++/WinRT
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.h>
#include "pilllog.h"
#include "startshare.h"

namespace wux = winrt::Windows::UI::Xaml;

// {9B1E4C52-3A7D-4E0F-8C61-2D5A7B3E9F14}
static const CLSID CLSID_StartTap = { 0x9b1e4c52, 0x3a7d, 0x4e0f, { 0x8c, 0x61, 0x2d, 0x5a, 0x7b, 0x3e, 0x9f, 0x14 } };
using InitializeXamlDiagnosticsEx_t = HRESULT(WINAPI*)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, CLSID, LPCWSTR);

namespace
{
	HMODULE g_module{};
	bool g_search{};	// loaded into SearchHost rather than StartMenuExperienceHost
	std::mutex g_logLock;

	// Rare events only. Inside SearchHost's app container GetTempPathW is the package's own AC\Temp folder.
	void Log(const std::wstring& line)
	{
		std::lock_guard guard{ g_logLock };
		FILE* file = OpenLog(g_search ? L"searchtap.txt" : L"starttap.txt");
		if (!file) return;
		SYSTEMTIME now{};
		GetLocalTime(&now);
		fwprintf(file, L"%02u:%02u:%02u.%03u %s\n", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, line.c_str());
		fclose(file);
	}

	std::wstring Hex(UINT64 value) { wchar_t text[24]; swprintf_s(text, L"%llx", value); return text; }

	// One of the flyout's own fills, with what Windows put there and what goes there under our glass.
	struct Restyled
	{
		wux::Controls::Border border{ nullptr };
		wux::Media::Brush original{ nullptr };
		wux::Media::Brush ours{ nullptr };
	};

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
			site->QueryInterface(__uuidof(IXamlDiagnostics), reinterpret_cast<void**>(&m_diagnostics));
			// Lives as long as the process: the handlers below keep using it.
			AddRef();
			std::thread([this]
			{
				IVisualTreeService3* tree{};
				if (SUCCEEDED(m_site->QueryInterface(__uuidof(IVisualTreeService3), reinterpret_cast<void**>(&tree))))
				{
					const HRESULT hr = tree->AdviseVisualTreeChange(this);
					if (FAILED(hr)) Log(L"advise failed, 0x" + Hex(static_cast<unsigned long>(hr)));
				}
			}).detach();
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** out) override { return m_site ? m_site->QueryInterface(riid, out) : E_FAIL; }

		// Runs on the UI thread. Keeps the named parts as they arrive; Search adds them again each time it opens.
		// Start: AcrylicBorder paints the acrylic inside MainMenu. Search: AppBorder is both the acrylic and the outline,
		// and AccentAppBorder takes over in some accent color modes.
		// Start's AcrylicOverlay lightens the middle by 3.5% white, which on the acrylic set the search box band above it
		// and the account band below it apart. On our brighter glass that vanished, so the glass under the whole menu
		// is darkened a little and the middle lifted by more, which brings the two bands back.
		HRESULT STDMETHODCALLTYPE OnVisualTreeChange(ParentChildRelation, VisualElement element, VisualMutationType mutation) override
		{
			if (mutation == Add && element.Name && m_diagnostics)
			{
				const std::wstring_view name(element.Name);
				std::optional<winrt::Windows::UI::Color> fill;
				if (g_search && (name == L"AppBorder" || name == L"AccentAppBorder")) fill = winrt::Windows::UI::Colors::Transparent();
				else if (!g_search && name == L"AcrylicBorder") fill = winrt::Windows::UI::ColorHelper::FromArgb(0x33, 0, 0, 0);
				else if (!g_search && name == L"AcrylicOverlay") fill = winrt::Windows::UI::ColorHelper::FromArgb(0x14, 0xFF, 0xFF, 0xFF);
				const bool outline = name == (g_search ? L"AppBorder" : L"MainMenu");
				if (fill || outline) Take(element.Handle, name, fill, outline);
			}
			// The tree changes as a flyout opens, which may come before the host notices.
			if (m_publishing) Wake(L"tree");
			SysFreeString(element.Type);
			SysFreeString(element.Name);
			SysFreeString(element.SrcInfo.FileName);
			SysFreeString(element.SrcInfo.Hash);
			return S_OK;
		}

	private:
		void Take(InstanceHandle handle, std::wstring_view name, std::optional<winrt::Windows::UI::Color> fill, bool outline)
		{
			try
			{
				winrt::Windows::Foundation::IInspectable object;
				if (FAILED(m_diagnostics->GetIInspectableFromHandle(handle, reinterpret_cast<::IInspectable**>(winrt::put_abi(object)))) || !object) return;
				if (fill)
				{
					const auto border = object.as<wux::Controls::Border>();
					auto known = std::find_if(m_restyled.begin(), m_restyled.end(), [&](const Restyled& item) { return item.border == border; });
					if (known == m_restyled.end())
					{
						// The first time only: when Search adds the part again, it still wears what we gave it.
						m_restyled.push_back({ border, border.Background(), wux::Media::SolidColorBrush(*fill) });
						known = std::prev(m_restyled.end());
						Log(std::wstring(name) + L" found");
					}
					m_glass = GlassShown();
					border.Background(m_glass ? known->ours : known->original);
				}
				if (outline)
				{
					m_outline = object.as<wux::FrameworkElement>();
					Start();
				}
			}
			catch (...) { Log(L"take threw for " + std::wstring(name)); }
		}

		// pillhost keeps this window while the setting is on. FindWindow works from inside the app container too.
		static bool GlassShown() { return FindWindowW(StartGlassClass, nullptr) != nullptr; }

		// Our fills only make sense over our glass; without it the flyout gets its own acrylic back.
		void Restyle(bool glass)
		{
			if (glass == m_glass) return;
			m_glass = glass;
			Log(glass ? L"glass is back, clear fills" : L"glass is gone, original acrylic");
			for (const auto& item : m_restyled)
			{
				try { item.border.Background(glass ? item.ours : item.original); }
				catch (...) {}
			}
		}

		// Publishes where the flyout is on every frame, as XAML does not tell us when a parent moves it.
		// Asking for every frame keeps XAML drawing them, about 7% of a core in each process, so it is asked only
		// while the flyout is open or moving: the host wakes us when it opens (and so do the window and the tree),
		// and we rest once it has stood still for a second with the glass hidden.
		void Start()
		{
			if (m_publishing) return;
			m_publishing = true;
			m_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(StartShare), g_search ? SearchShareName : StartShareName);
			m_share = m_mapping ? static_cast<StartShare*>(MapViewOfFile(m_mapping, FILE_MAP_WRITE, 0, 0, sizeof(StartShare))) : nullptr;
			// The host waits on this rather than polling once a frame, which left the glass a frame behind the slide.
			m_moved = CreateEventW(nullptr, FALSE, FALSE, g_search ? SearchMovedName : StartMovedName);
			const HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, g_search ? SearchWakeName : StartWakeName);
			if (const auto core = winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread())
			{
				HWND window{};
				if (const auto interop = core.try_as<ICoreWindowInterop>()) interop->get_WindowHandle(&window);
				m_window = window;
				core.VisibilityChanged([this](auto&&, auto&&) { Wake(L"visibility"); });
				core.Activated([this](auto&&, auto&&) { Wake(L"activated"); });
				const auto dispatcher = core.Dispatcher();
				if (wake) std::thread([this, wake, dispatcher]
				{
					while (WaitForSingleObject(wake, INFINITE) == WAIT_OBJECT_0)
					{
						dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::High, [this] { Wake(L"host"); });
					}
				}).detach();
			}
			Log(L"publishing, window 0x" + Hex(reinterpret_cast<UINT64>(m_window)) + (m_share ? L"" : L", but no share") + (wake ? L"" : L", no wake event"));
			Wake(L"start");
		}

		void Wake(const wchar_t* by)
		{
			m_movedAt = GetTickCount64();
			if (m_rendering) return;
			try { m_rendering = wux::Media::CompositionTarget::Rendering([this](auto&&, auto&&) { Publish(); }); }
			catch (...) { return; }
			m_wokeBy = by;
		}

		void Rest()
		{
			try { wux::Media::CompositionTarget::Rendering(m_rendering); }
			catch (...) {}
			m_rendering = {};
		}

		void Publish()
		{
			const ULONGLONG now = GetTickCount64();
			if (now - m_glassChecked >= 250)
			{
				m_glassChecked = now;
				const HWND glass = FindWindowW(StartGlassClass, nullptr);
				Restyle(glass != nullptr);
				m_glassVisible = glass && IsWindowVisible(glass);
			}
			RECT menu{};
			try
			{
				if (m_outline && m_outline.ActualWidth() > 0 && m_outline.XamlRoot())
				{
					const double scale = m_outline.XamlRoot().RasterizationScale();
					const auto origin = m_outline.TransformToVisual(nullptr).TransformPoint({ 0, 0 });
					RECT frame{};
					GetWindowRect(m_window, &frame);
					menu.left = frame.left + static_cast<LONG>(std::lround(origin.X * scale));
					menu.top = frame.top + static_cast<LONG>(std::lround(origin.Y * scale));
					menu.right = menu.left + static_cast<LONG>(std::lround(m_outline.ActualWidth() * scale));
					menu.bottom = menu.top + static_cast<LONG>(std::lround(m_outline.ActualHeight() * scale));
				}
			}
			catch (...) {}
			if (m_wokeBy)
			{
				// Once per opening: a menu top still below the screen means we woke before the slide.
				Log(std::wstring(L"awake by ") + m_wokeBy + L", menu top " + std::to_wstring(menu.top));
				m_wokeBy = nullptr;
			}
			if (EqualRect(&menu, &m_last) || !m_share)
			{
				if (now - m_movedAt >= 1000 && !m_glassVisible) Rest();
				return;
			}
			m_movedAt = now;
			m_last = menu;
			InterlockedIncrement(&m_share->sequence);
			m_share->window = reinterpret_cast<UINT64>(m_window);
			m_share->menu = menu;
			InterlockedIncrement(&m_share->sequence);
			if (m_moved) SetEvent(m_moved);
		}

		std::atomic<ULONG> m_refs{ 1 };
		IUnknown* m_site{};
		IXamlDiagnostics* m_diagnostics{};
		std::vector<Restyled> m_restyled;
		bool m_glass{};
		ULONGLONG m_glassChecked{};
		wux::FrameworkElement m_outline{ nullptr };
		bool m_publishing{};
		HWND m_window{};
		HANDLE m_mapping{}, m_moved{};
		StartShare* m_share{};
		RECT m_last{};
		winrt::event_token m_rendering{};	// set while we ask for every frame
		ULONGLONG m_movedAt{};	// last move or wake
		bool m_glassVisible{};
		const wchar_t* m_wokeBy{};
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

	// Both processes run one XAML thread, listening on VisualDiagConnection1. At sign-in the host may load us
	// before that thread starts, and 0x80070490 means nothing listens yet, so this keeps trying for a minute.
	DWORD WINAPI Attach(void*)
	{
		wchar_t location[MAX_PATH];
		GetModuleFileNameW(g_module, location, MAX_PATH);
		const HMODULE xaml = LoadLibraryExW(L"Windows.UI.Xaml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
		const auto initialize = xaml ? reinterpret_cast<InitializeXamlDiagnosticsEx_t>(GetProcAddress(xaml, "InitializeXamlDiagnosticsEx")) : nullptr;
		if (!initialize) { Log(L"no InitializeXamlDiagnosticsEx"); return 1; }
		const ULONGLONG start = GetTickCount64();
		HRESULT hr{};
		do
		{
			// Diagnostics initialize once per thread, hence a new thread per call.
			std::thread([&] { hr = initialize(L"VisualDiagConnection1", GetCurrentProcessId(), nullptr, location, CLSID_StartTap, nullptr); }).join();
			if (hr != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) break;
			Sleep(250);
		} while (GetTickCount64() - start < StartListenLimit);
		Log(SUCCEEDED(hr) ? L"attached after " + std::to_wstring(GetTickCount64() - start) + L" ms" : L"could not attach, error 0x" + Hex(static_cast<unsigned long>(hr)));
		return SUCCEEDED(hr) ? 0 : 1;
	}
}

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID riid, void** out)
{
	if (clsid != CLSID_StartTap) return CLASS_E_CLASSNOTAVAILABLE;
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
		g_search = name && _wcsicmp(name + 1, L"SearchHost.exe") == 0;
		if (name && (g_search || _wcsicmp(name + 1, L"StartMenuExperienceHost.exe") == 0))
		{
			// Pinned: XAML holds the TAP for the life of the process, and pillhost checks for this module before loading another.
			HMODULE pinned{};
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&DllMain), &pinned);
			if (const HANDLE thread = CreateThread(nullptr, 0, Attach, nullptr, 0, nullptr)) CloseHandle(thread);
		}
	}
	return TRUE;
}
