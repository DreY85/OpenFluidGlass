// Liquid Glass Start menu, inside StartMenuExperienceHost. Attaches as a XAML diagnostics TAP, clears the
// menu's own acrylic and publishes where the menu is on screen, so startglass.exe can draw our glass under it.
// The approach follows TranslucentTB's ExplorerTAP (GPL-3.0, https://github.com/TranslucentTB/TranslucentTB).
#include <windows.h>
#include <ocidl.h>
#include <xamlOM.h>
#include <CoreWindow.h>
#undef GetCurrentTime	// windows.h macro clashes with C++/WinRT
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.h>
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
	std::vector<std::wstring> g_pending;
	size_t g_lines{};
	constexpr size_t LineLimit = 20000;

	// Called on the UI thread for every element, so lines only queue here and a thread of ours writes them.
	// Inside the app container GetTempPathW is the package's own AC\Temp folder.
	void Log(std::wstring line)
	{
		SYSTEMTIME now{};
		GetLocalTime(&now);
		wchar_t stamp[32];
		swprintf_s(stamp, L"%02u:%02u:%02u.%03u ", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
		std::lock_guard guard{ g_logLock };
		if (g_lines++ < LineLimit) g_pending.push_back(stamp + line);
	}

	void Writer()
	{
		wchar_t path[MAX_PATH];
		GetTempPathW(MAX_PATH, path);
		wcscat_s(path, g_search ? L"searchtap.txt" : L"starttap.txt");
		for (;;)
		{
			Sleep(500);
			std::vector<std::wstring> lines;
			{ std::lock_guard guard{ g_logLock }; lines.swap(g_pending); }
			if (lines.empty()) continue;
			FILE* file{};
			if (_wfopen_s(&file, path, L"a, ccs=UTF-8") || !file) continue;
			for (const auto& line : lines) fwprintf(file, L"%s\n", line.c_str());
			fclose(file);
		}
	}

	std::wstring Hex(UINT64 value) { wchar_t text[24]; swprintf_s(text, L"%llx", value); return text; }

	std::wstring BrushName(const wux::Media::Brush& brush)
	{
		if (!brush) return L"-";
		std::wstring name(winrt::get_class_name(brush));
		if (const auto solid = brush.try_as<wux::Media::SolidColorBrush>())
		{
			const auto color = solid.Color();
			wchar_t text[48];
			swprintf_s(text, L" #%02X%02X%02X%02X", color.A, color.R, color.G, color.B);
			name += text;
		}
		if (const auto acrylic = brush.try_as<wux::Media::AcrylicBrush>())
		{
			name += acrylic.BackgroundSource() == wux::Media::AcrylicBackgroundSource::HostBackdrop ? L" host" : L" app";
			wchar_t text[64];
			swprintf_s(text, L" tint %.2f", acrylic.TintOpacity());
			name += text;
		}
		return name;
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
			Log(L"site set");
			site->AddRef();
			m_site = site;
			site->QueryInterface(__uuidof(IXamlDiagnostics), reinterpret_cast<void**>(&m_diagnostics));
			AddRef();
			std::thread([this]
			{
				IVisualTreeService3* tree{};
				if (SUCCEEDED(m_site->QueryInterface(__uuidof(IVisualTreeService3), reinterpret_cast<void**>(&tree))))
				{
					Log(L"advise 0x" + Hex(static_cast<unsigned long>(tree->AdviseVisualTreeChange(this))));
				}
			}).detach();
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** out) override { return m_site ? m_site->QueryInterface(riid, out) : E_FAIL; }

		// Runs on the UI thread. Keeps the named parts as they arrive; Search adds them again each time it opens.
		// Start: AcrylicBorder paints the acrylic inside MainMenu. Search: AppBorder is both the acrylic and the outline,
		// and AccentAppBorder takes over in some accent color modes.
		HRESULT STDMETHODCALLTYPE OnVisualTreeChange(ParentChildRelation, VisualElement element, VisualMutationType mutation) override
		{
			if (mutation == Add && element.Name && m_diagnostics)
			{
				const std::wstring_view name(element.Name);
				const bool clear = g_search ? name == L"AppBorder" || name == L"AccentAppBorder" : name == L"AcrylicBorder";
				const bool outline = name == (g_search ? L"AppBorder" : L"MainMenu");
				if (clear || outline) Take(element.Handle, name, clear, outline);
			}
			SysFreeString(element.Type);
			SysFreeString(element.Name);
			SysFreeString(element.SrcInfo.FileName);
			SysFreeString(element.SrcInfo.Hash);
			return S_OK;
		}

	private:
		std::wstring Describe(InstanceHandle handle)
		{
			try
			{
				winrt::Windows::Foundation::IInspectable object;
				if (FAILED(m_diagnostics->GetIInspectableFromHandle(handle, reinterpret_cast<::IInspectable**>(winrt::put_abi(object)))) || !object) return {};
				std::wstring text;
				if (const auto element = object.try_as<wux::FrameworkElement>())
				{
					wchar_t size[64];
					swprintf_s(size, L" %.0fx%.0f", element.ActualWidth(), element.ActualHeight());
					text += size;
				}
				wux::Media::Brush brush{ nullptr };
				if (const auto panel = object.try_as<wux::Controls::Panel>()) brush = panel.Background();
				else if (const auto border = object.try_as<wux::Controls::Border>()) brush = border.Background();
				else if (const auto control = object.try_as<wux::Controls::Control>()) brush = control.Background();
				else if (const auto shape = object.try_as<wux::Shapes::Shape>()) brush = shape.Fill();
				if (brush) text += L" bg " + BrushName(brush);
				return text;
			}
			catch (...) { return L" (threw)"; }
		}

		void Take(InstanceHandle handle, std::wstring_view name, bool clear, bool outline)
		{
			try
			{
				winrt::Windows::Foundation::IInspectable object;
				if (FAILED(m_diagnostics->GetIInspectableFromHandle(handle, reinterpret_cast<::IInspectable**>(winrt::put_abi(object)))) || !object) return;
				if (clear)
				{
					const auto border = object.as<wux::Controls::Border>();
					Log(std::wstring(name) + L" found, was " + BrushName(border.Background()) + L", now clear");
					border.Background(wux::Media::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
				}
				if (outline)
				{
					m_menu = object.as<wux::FrameworkElement>();
					Log(std::wstring(name) + L" is the outline");
					Start();
				}
			}
			catch (...) { Log(L"take threw for " + std::wstring(name)); }
		}

		// Publishes where the menu is on every frame, as XAML does not tell us when a parent moves it.
		void Start()
		{
			if (m_publishing) return;
			m_publishing = true;
			m_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(StartShare), g_search ? SearchShareName : StartShareName);
			m_share = m_mapping ? static_cast<StartShare*>(MapViewOfFile(m_mapping, FILE_MAP_WRITE, 0, 0, sizeof(StartShare))) : nullptr;
			if (const auto core = winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread())
			{
				HWND window{};
				if (const auto interop = core.try_as<ICoreWindowInterop>()) interop->get_WindowHandle(&window);
				m_window = window;
				Log(L"core window 0x" + Hex(reinterpret_cast<UINT64>(window)));
				// The menu loses activation the moment it starts to close, while XAML only moves it at the end
				// of its slide; the host hides the glass on this rather than on the late position.
				core.Activated([this](auto&&, const winrt::Windows::UI::Core::WindowActivatedEventArgs& args)
				{
					const bool active = args.WindowActivationState() != winrt::Windows::UI::Core::CoreWindowActivationState::Deactivated;
					Log(active ? L"activated" : L"deactivated");
					if (!m_share) return;
					InterlockedIncrement(&m_share->sequence);
					m_share->active = active;
					InterlockedIncrement(&m_share->sequence);
				});
			}
			wux::Media::CompositionTarget::Rendering([this](auto&&, auto&&) { Publish(); });
		}

		void Publish()
		{
			RECT menu{};
			try
			{
				if (m_menu && m_menu.ActualWidth() > 0 && m_menu.XamlRoot())
				{
					const double scale = m_menu.XamlRoot().RasterizationScale();
					const auto origin = m_menu.TransformToVisual(nullptr).TransformPoint({ 0, 0 });
					RECT frame{};
					GetWindowRect(m_window, &frame);
					menu.left = frame.left + static_cast<LONG>(std::lround(origin.X * scale));
					menu.top = frame.top + static_cast<LONG>(std::lround(origin.Y * scale));
					menu.right = menu.left + static_cast<LONG>(std::lround(m_menu.ActualWidth() * scale));
					menu.bottom = menu.top + static_cast<LONG>(std::lround(m_menu.ActualHeight() * scale));
				}
			}
			catch (...) {}
			if (EqualRect(&menu, &m_last)) return;
			m_last = menu;
			Log(L"menu " + std::to_wstring(menu.left) + L"," + std::to_wstring(menu.top) + L" " + std::to_wstring(menu.right - menu.left) + L"x" + std::to_wstring(menu.bottom - menu.top));
			if (!m_share) return;
			InterlockedIncrement(&m_share->sequence);
			m_share->window = reinterpret_cast<UINT64>(m_window);
			m_share->menu = menu;
			InterlockedIncrement(&m_share->sequence);
		}

		std::atomic<ULONG> m_refs{ 1 };
		IUnknown* m_site{};
		IXamlDiagnostics* m_diagnostics{};
		wux::FrameworkElement m_menu{ nullptr };
		bool m_publishing{};
		HWND m_window{};
		HANDLE m_mapping{};
		StartShare* m_share{};
		RECT m_last{};
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

	// Tries the first few XAML endpoints, as we do not know yet which thread hosts the menu.
	DWORD WINAPI Attach(void*)
	{
		std::thread(Writer).detach();
		wchar_t location[MAX_PATH];
		GetModuleFileNameW(g_module, location, MAX_PATH);
		Log(L"attaching from " + std::wstring(location));
		const HMODULE xaml = LoadLibraryExW(L"Windows.UI.Xaml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
		const auto initialize = xaml ? reinterpret_cast<InitializeXamlDiagnosticsEx_t>(GetProcAddress(xaml, "InitializeXamlDiagnosticsEx")) : nullptr;
		if (!initialize) { Log(L"no InitializeXamlDiagnosticsEx"); return 1; }
		for (int endpoint = 1; endpoint <= 3; ++endpoint)
		{
			const std::wstring name = L"VisualDiagConnection" + std::to_wstring(endpoint);
			HRESULT hr{};
			std::thread([&] { hr = initialize(name.c_str(), GetCurrentProcessId(), nullptr, location, CLSID_StartTap, nullptr); }).join();
			Log(name + L": 0x" + Hex(static_cast<unsigned long>(hr)));
		}
		// Stays loaded: XAML holds the TAP, and the writer thread runs from this module.
		return 0;
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
			HMODULE pinned{};
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&DllMain), &pinned);
			if (const HANDLE thread = CreateThread(nullptr, 0, Attach, nullptr, 0, nullptr)) CloseHandle(thread);
		}
	}
	return TRUE;
}
