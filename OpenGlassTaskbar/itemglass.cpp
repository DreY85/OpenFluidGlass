// File Explorer item glass, loaded into explorer by pillhost.exe. It replaces the grey hover and selection of the
// folder view and the navigation pane with dark glass: a translucent rounded fill tinted with the accent color, a light
// rim and a sheen, rounded like the window corners. The keyboard focus outline becomes a glass rim, and the classic
// tooltips get a dark glass face in a window DWM rounds. Only theme parts it knows are replaced. Anything else, or a
// target it cannot draw into, keeps the theme's own drawing.
// FluidGlassItems (HKCU DWM key, default on) turns it off and on while it runs. It loads once per explorer and stays.
// It is also a shell folder extension, so the Open and Save dialogs of other apps load it with their folder view.
// There it only stays when FluidGlassDialogs (default off) is set: our hooks then live in someone else's process.
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <ocidl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <iterator>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <KNSoft/SlimDetours/SlimDetours.h>
#include "pilllog.h"

namespace
{
	enum class Kind { Other, Items, Focus, Tree, Tooltip };

	struct Theme
	{
		Kind kind{};
		HWND window{};
	};

	std::mutex g_lock;
	std::map<HTHEME, Theme> g_themes;
	std::atomic<bool> g_enabled{ true };
	std::atomic<bool> g_light{};	// apps in light mode draw dark text, so the glass turns light under it
	bool g_inExplorer{};	// false inside another app's Open or Save dialog
	float g_accent[3]{ 0, 120, 215 };	// red, green, blue

	decltype(&OpenThemeData) g_openThemeData = OpenThemeData;
	decltype(&OpenThemeDataForDpi) g_openThemeDataForDpi = OpenThemeDataForDpi;
	decltype(&OpenThemeDataEx) g_openThemeDataEx = OpenThemeDataEx;
	decltype(&CloseThemeData) g_closeThemeData = CloseThemeData;
	decltype(&DrawThemeBackground) g_drawThemeBackground = DrawThemeBackground;
	decltype(&DrawThemeBackgroundEx) g_drawThemeBackgroundEx = DrawThemeBackgroundEx;

	void Log(const wchar_t* line)
	{
		if (FILE* file = OpenLog(L"itemglass.txt"))
		{
			SYSTEMTIME now{};
			GetLocalTime(&now);
			fwprintf(file, L"%02u:%02u:%02u.%03u %s\n", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, line);
			fclose(file);
		}
	}

	HTHEME Remember(HTHEME theme, HWND window, LPCWSTR classList)
	{
		if (!theme || !classList) return theme;
		Kind kind = Kind::Other;
		if (!_wcsicmp(classList, L"ItemsView::ListView")) kind = Kind::Items;
		else if (!_wcsicmp(classList, L"ItemsView")) kind = Kind::Focus;
		else if (!_wcsicmp(classList, L"TreeView") || !_wcsicmp(classList, L"Explorer::TreeView")) kind = Kind::Tree;
		else if (!_wcsicmp(classList, L"Tooltip")) kind = Kind::Tooltip;
		std::lock_guard guard{ g_lock };
		if (kind == Kind::Other) g_themes.erase(theme);
		else g_themes[theme] = { kind, window };
		return theme;
	}

	HTHEME WINAPI OpenThemeDataHook(HWND window, LPCWSTR classList) { return Remember(g_openThemeData(window, classList), window, classList); }
	HTHEME WINAPI OpenThemeDataForDpiHook(HWND window, LPCWSTR classList, UINT dpi) { return Remember(g_openThemeDataForDpi(window, classList, dpi), window, classList); }
	HTHEME WINAPI OpenThemeDataExHook(HWND window, LPCWSTR classList, DWORD flags) { return Remember(g_openThemeDataEx(window, classList, flags), window, classList); }

	HRESULT WINAPI CloseThemeDataHook(HTHEME theme)
	{
		{
			std::lock_guard guard{ g_lock };
			g_themes.erase(theme);
		}
		return g_closeThemeData(theme);
	}

	// The look, in 0..1 alpha: a black fill, a tint of the accent color over it, a white rim brighter at the top,
	// and a white sheen fading down to the middle.
	struct Look
	{
		float fill, rimTop, rimBottom, sheen, tint;
	};

	// By theme state: 2 hot, 3 selected, 5 selected in an unfocused window, 6 selected and hot. Slot 1 is the keyboard
	// focus outline of the folder view (ItemsView part 3), a rim without fill. Slot 4 is the plain tooltip, drawn over
	// an opaque base of TipBase grey. Tuned by eye on the dark theme over Fluid Glass. In light mode the fill turns
	// white and the rim dark at LightRim of its strength, over a TipBaseLight grey for tooltips.
	constexpr Look Looks[7]{ {}, { 0.0f, 0.55f, 0.20f, 0.0f, 0.0f }, { 0.45f, 0.85f, 0.30f, 0.18f, 0.12f }, { 0.55f, 1.0f, 0.35f, 0.25f, 0.30f },
		{ 0.0f, 0.45f, 0.12f, 0.10f, 0.08f }, { 0.45f, 0.70f, 0.25f, 0.15f, 0.15f }, { 0.60f, 1.0f, 0.40f, 0.30f, 0.35f } };
	constexpr float Radius = 8, TipBase = 0.13f, TipBaseLight = 0.96f, LightRim = 0.3f;

	bool LookFor(Kind kind, int part, int state, Look& look)
	{
		if (kind == Kind::Tooltip)
		{
			if (part != TTP_STANDARD) return false;
			state = 4;
		}
		else if (kind == Kind::Focus)
		{
			if (part != 3) return false;
			state = 1;
		}
		else
		{
			const bool item = (kind == Kind::Items && part == LVP_LISTITEM) || (kind == Kind::Tree && part == TVP_TREEITEM);
			if (!item || (state != 2 && state != 3 && state != 5 && state != 6)) return false;
		}
		look = Looks[state];
		return true;
	}

	// Signed distance from a point to a rounded rectangle, negative inside.
	float RoundedDistance(float x, float y, float left, float top, float right, float bottom, float radius)
	{
		const float cx = (left + right) / 2, cy = (top + bottom) / 2;
		const float hx = (right - left) / 2 - radius, hy = (bottom - top) / 2 - radius;
		const float qx = std::fabs(x - cx) - hx, qy = std::fabs(y - cy) - hy;
		const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
		return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - radius;
	}

	// Paints the glass into a premultiplied 32-bit DIB the size of the item, then blends it onto the DC with
	// AlphaBlend, which works on any target and keeps its alpha. The DC's own clipping still applies.
	// (Explorer's buffers are not always DIBs, so drawing into their bits directly fell back too often.)
	bool DrawGlass(HDC dc, const RECT& rect, const RECT* clip, HWND window, const Look& look)
	{
		const LONG width = rect.right - rect.left, height = rect.bottom - rect.top;
		if (width <= 0 || height <= 0) return true;

		BITMAPINFO info{};
		info.bmiHeader = { sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB };
		void* bits{};
		const HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
		if (!bitmap) return false;

		// Rounded like the window corners: 8 px at 100 percent, scaled with the window's DPI.
		const UINT dpi = window ? GetDpiForWindow(window) : 96;
		const float scale = (dpi ? dpi : 96) / 96.0f;
		const float radius = std::min(Radius * scale, std::min(static_cast<float>(width), static_cast<float>(height)) / 2);
		const float rimWidth = std::max(1.0f, scale);
		float accent[3];
		{
			std::lock_guard guard{ g_lock };
			std::copy(std::begin(g_accent), std::end(g_accent), accent);
		}

		const bool light = g_light;
		const float fillShade = light ? 255.0f : 0.0f, rimShade = light ? 0.0f : 255.0f, rimStrength = light ? LightRim : 1.0f;

		auto* pixels = static_cast<BYTE*>(bits);
		for (LONG y = 0; y < height; ++y)
		{
			const float py = y + 0.5f;
			const float rimAlpha = (look.rimTop + (look.rimBottom - look.rimTop) * (py / height)) * rimStrength;
			for (LONG x = 0; x < width; ++x)
			{
				const float d = RoundedDistance(x + 0.5f, py, 0, 0, static_cast<float>(width), static_cast<float>(height), radius);
				const float inside = std::clamp(0.5f - d, 0.0f, 1.0f);
				const float rim = inside * std::clamp(d + rimWidth + 0.5f, 0.0f, 1.0f) * rimAlpha;
				const float sheen = inside * look.sheen * std::max(0.0f, 1 - py / (height / 2.0f));
				const float tint = inside * look.tint;
				// Fill (black, or white in light mode), the accent tint over it, then the white sheen and the rim over
				// that, premultiplied. BGRA.
				float alpha = inside * look.fill;
				float color[3]{};
				for (float& c : color) c = fillShade * alpha;
				for (int c = 0; c < 3; ++c) color[c] = tint * accent[c] + color[c] * (1 - tint);
				alpha = tint + alpha * (1 - tint);
				for (const auto [layer, shade] : { std::pair{ sheen, 255.0f }, std::pair{ rim, rimShade } })
				{
					for (float& c : color) c = shade * layer + c * (1 - layer);
					alpha = layer + alpha * (1 - layer);
				}
				BYTE* p = pixels + (static_cast<size_t>(y) * width + x) * 4;
				for (int c = 0; c < 3; ++c) p[2 - c] = static_cast<BYTE>(std::lround(color[c]));
				p[3] = static_cast<BYTE>(std::lround(255 * alpha));
			}
		}

		const HDC source = CreateCompatibleDC(dc);
		const HGDIOBJ old = SelectObject(source, bitmap);
		const int saved = SaveDC(dc);
		if (clip) IntersectClipRect(dc, clip->left, clip->top, clip->right, clip->bottom);
		const BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
		const BOOL blended = AlphaBlend(dc, rect.left, rect.top, width, height, source, 0, 0, width, height, blend);
		RestoreDC(dc, saved);
		SelectObject(source, old);
		DeleteDC(source);
		DeleteObject(bitmap);
		return blended != FALSE;
	}

	// Tooltips are their own popup windows and opaque: ask DWM to round the window like Windows 11 does, and lay the
	// dark base the glass is drawn over. The window is the DC's own or, when the tooltip paints through a buffer, the
	// one its tree view or list view owns. Translucent tooltips would lose their text, which GDI draws without alpha.
	void PrepareTooltip(HDC dc, const RECT& rect, const RECT* clip, HWND owner)
	{
		HWND tip = WindowFromDC(dc);
		wchar_t name[32]{};
		if (!tip || !GetClassNameW(tip, name, ARRAYSIZE(name)) || _wcsicmp(name, L"tooltips_class32"))
		{
			tip = nullptr;
			if (owner && GetClassNameW(owner, name, ARRAYSIZE(name)))
			{
				if (!_wcsicmp(name, L"SysTreeView32")) tip = reinterpret_cast<HWND>(SendMessageW(owner, TVM_GETTOOLTIPS, 0, 0));
				else if (!_wcsicmp(name, L"SysListView32")) tip = reinterpret_cast<HWND>(SendMessageW(owner, LVM_GETTOOLTIPS, 0, 0));
			}
		}
		if (tip)
		{
			const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
			DwmSetWindowAttribute(tip, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
		}
		const BYTE level = static_cast<BYTE>(std::lround(255 * (g_light ? TipBaseLight : TipBase)));
		const int saved = SaveDC(dc);
		if (clip) IntersectClipRect(dc, clip->left, clip->top, clip->right, clip->bottom);
		const HBRUSH base = CreateSolidBrush(RGB(level, level, level));
		FillRect(dc, &rect, base);
		DeleteObject(base);
		RestoreDC(dc, saved);
	}

	bool Replace(HTHEME theme, HDC dc, int part, int state, const RECT* rect, const RECT* clip)
	{
		if (!rect || !g_enabled) return false;
		Theme known{};
		{
			std::lock_guard guard{ g_lock };
			const auto found = g_themes.find(theme);
			if (found == g_themes.end()) return false;
			known = found->second;
		}
		Look look{};
		if (!LookFor(known.kind, part, state, look)) return false;
		if (known.kind == Kind::Tooltip) PrepareTooltip(dc, *rect, clip, known.window);
		return DrawGlass(dc, *rect, clip, known.window, look);
	}

	HRESULT WINAPI DrawThemeBackgroundHook(HTHEME theme, HDC dc, int part, int state, LPCRECT rect, LPCRECT clip)
	{
		if (Replace(theme, dc, part, state, rect, clip)) return S_OK;
		return g_drawThemeBackground(theme, dc, part, state, rect, clip);
	}

	HRESULT WINAPI DrawThemeBackgroundExHook(HTHEME theme, HDC dc, int part, int state, LPCRECT rect, const DTBGOPTS* options)
	{
		const RECT* clip = options && (options->dwFlags & DTBG_CLIPRECT) ? &options->rcClip : nullptr;
		if (Replace(theme, dc, part, state, rect, clip)) return S_OK;
		return g_drawThemeBackgroundEx(theme, dc, part, state, rect, options);
	}

	HRESULT Hook()
	{
		HRESULT hr = SlimDetoursTransactionBegin();
		if (FAILED(hr)) return hr;
		SlimDetoursAttach(reinterpret_cast<PVOID*>(&g_openThemeData), OpenThemeDataHook);
		SlimDetoursAttach(reinterpret_cast<PVOID*>(&g_openThemeDataForDpi), OpenThemeDataForDpiHook);
		SlimDetoursAttach(reinterpret_cast<PVOID*>(&g_openThemeDataEx), OpenThemeDataExHook);
		SlimDetoursAttach(reinterpret_cast<PVOID*>(&g_closeThemeData), CloseThemeDataHook);
		SlimDetoursAttach(reinterpret_cast<PVOID*>(&g_drawThemeBackground), DrawThemeBackgroundHook);
		SlimDetoursAttach(reinterpret_cast<PVOID*>(&g_drawThemeBackgroundEx), DrawThemeBackgroundExHook);
		return SlimDetoursTransactionCommit();
	}

	// Only the controls whose themes we replace: the folder view and the navigation pane, which also owns its tooltip.
	// A theme change sent to the address bar and toolbars made a dialog's top bar flash light before it went dark again.
	BOOL CALLBACK ThemeChanged(HWND window, LPARAM)
	{
		wchar_t name[32]{};
		if (!GetClassNameW(window, name, ARRAYSIZE(name))) return TRUE;
		if (!wcscmp(name, L"SHELLDLL_DefView") || !wcscmp(name, L"DUIViewWndClassName") || !wcscmp(name, L"DirectUIHWND") || !wcscmp(name, L"SysTreeView32"))
			SendMessageTimeoutW(window, WM_THEMECHANGED, 0, 0, SMTO_ABORTIFHUNG, 1000, nullptr);
		return TRUE;
	}

	// Folder windows and dialogs open before the hooks have their themes already. A theme change makes their folder
	// view and navigation pane open them again, through the hooks. Repaint alone is enough when the effect is turned
	// on or off.
	BOOL CALLBACK RefreshFolder(HWND window, LPARAM rethemeAll)
	{
		wchar_t name[32]{};
		DWORD pid{};
		GetWindowThreadProcessId(window, &pid);
		if (pid != GetCurrentProcessId() || !GetClassNameW(window, name, ARRAYSIZE(name)) || (wcscmp(name, L"CabinetWClass") && wcscmp(name, L"#32770"))) return TRUE;
		if (rethemeAll)
		{
			EnumChildWindows(window, ThemeChanged, 0);
		}
		RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
		return TRUE;
	}

	bool ReadSwitch(const wchar_t* name, DWORD fallback)
	{
		DWORD value = fallback, size = sizeof(value);
		RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", name, RRF_RT_REG_DWORD, nullptr, &value, &size);
		return value != 0;
	}

	void ReadSettings()
	{
		const bool enabled = g_inExplorer ? ReadSwitch(L"FluidGlassItems", 1) : ReadSwitch(L"FluidGlassDialogs", 0);
		DWORD lightTheme{}, size = sizeof(lightTheme);
		RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &lightTheme, &size);
		const bool light = lightTheme != 0;
		const bool lightChanged = g_light.exchange(light) != light;
		if (g_enabled.exchange(enabled) != enabled)
		{
			Log(enabled ? L"turned on" : L"turned off");
			EnumWindows(RefreshFolder, FALSE);
		}
		else if (lightChanged)
		{
			EnumWindows(RefreshFolder, FALSE);
		}
		DWORD color{};
		BOOL opaque{};
		if (SUCCEEDED(DwmGetColorizationColor(&color, &opaque)))
		{
			std::lock_guard guard{ g_lock };
			g_accent[0] = static_cast<float>((color >> 16) & 0xFF);
			g_accent[1] = static_cast<float>((color >> 8) & 0xFF);
			g_accent[2] = static_cast<float>(color & 0xFF);
		}
	}

	// Any app that walks shell folders loads folder extensions, not only one that shows a dialog. Outside explorer the
	// hooks wait for a folder view, so a background process that never shows one keeps its theme drawing untouched.
	BOOL CALLBACK IsView(HWND window, LPARAM found)
	{
		wchar_t name[32]{};
		if (!GetClassNameW(window, name, ARRAYSIZE(name)) || wcscmp(name, L"SHELLDLL_DefView")) return TRUE;
		*reinterpret_cast<bool*>(found) = true;
		return FALSE;
	}

	BOOL CALLBACK FindView(HWND window, LPARAM found)
	{
		DWORD pid{};
		GetWindowThreadProcessId(window, &pid);
		if (pid == GetCurrentProcessId()) EnumChildWindows(window, IsView, found);
		return !*reinterpret_cast<bool*>(found);
	}

	void CALLBACK ViewCreated(HWINEVENTHOOK, DWORD, HWND window, LONG object, LONG, DWORD, DWORD)
	{
		bool found{};
		if (object == OBJID_WINDOW && window) IsView(window, reinterpret_cast<LPARAM>(&found));
		if (found) PostQuitMessage(0);
	}

	void WaitForView()
	{
		// Listen first, then look, so a view created in between is not missed. The events cost nothing while we wait.
		const HWINEVENTHOOK listener = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_CREATE, nullptr, ViewCreated, GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
		bool found{};
		EnumWindows(FindView, reinterpret_cast<LPARAM>(&found));
		if (!found && listener)
		{
			MSG message;
			while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
		}
		if (listener) UnhookWinEvent(listener);
	}

	DWORD WINAPI Run(void*)
	{
		wchar_t line[MAX_PATH + 32], exe[MAX_PATH];
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		const wchar_t* file = wcsrchr(exe, L'\\') + 1;
		if (!g_inExplorer) WaitForView();
		ReadSettings();
		const HRESULT hr = Hook();
		if (FAILED(hr))
		{
			swprintf_s(line, L"hooking failed in %s, error 0x%08lX", file, static_cast<unsigned long>(hr));
			Log(line);
			return 0;
		}
		swprintf_s(line, L"hooks installed in %s", file);
		Log(line);
		EnumWindows(RefreshFolder, TRUE);
		// A dialog loads its folder extensions while it builds the view, so its window may not be complete yet.
		if (!g_inExplorer)
		{
			Sleep(300);
			EnumWindows(RefreshFolder, TRUE);
		}
		for (;;)
		{
			Sleep(1000);
			ReadSettings();
		}
	}
}

// The folder extension object. The shell only needs it to exist: the work happens when the DLL loads.
namespace
{
	// {31DCA04E-200A-45C4-8969-9FEA9673B3D9}, registered under Drive\shellex\FolderExtensions by the installer.
	constexpr CLSID CLSID_ItemGlass{ 0x31dca04e, 0x200a, 0x45c4, { 0x89, 0x69, 0x9f, 0xea, 0x96, 0x73, 0xb3, 0xd9 } };

	class FolderExtension final : public IObjectWithSite
	{
		std::atomic<ULONG> m_references{ 1 };
		IUnknown* m_site{};

	public:
		~FolderExtension() { if (m_site) m_site->Release(); }
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
		{
			if (!out) return E_POINTER;
			*out = riid == IID_IUnknown || riid == IID_IObjectWithSite ? static_cast<IObjectWithSite*>(this) : nullptr;
			if (!*out) return E_NOINTERFACE;
			AddRef();
			return S_OK;
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return ++m_references; }
		ULONG STDMETHODCALLTYPE Release() override
		{
			const ULONG left = --m_references;
			if (!left) delete this;
			return left;
		}
		HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) override
		{
			if (site) site->AddRef();
			if (m_site) m_site->Release();
			m_site = site;
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** out) override
		{
			if (!out) return E_POINTER;
			*out = nullptr;
			return m_site ? m_site->QueryInterface(riid, out) : E_FAIL;
		}
	};

	class Factory final : public IClassFactory
	{
	public:
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
		{
			if (!out) return E_POINTER;
			*out = riid == IID_IUnknown || riid == IID_IClassFactory ? static_cast<IClassFactory*>(this) : nullptr;
			return *out ? S_OK : E_NOINTERFACE;
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
		ULONG STDMETHODCALLTYPE Release() override { return 1; }
		HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** out) override
		{
			if (outer) return CLASS_E_NOAGGREGATION;
			auto* extension = new (std::nothrow) FolderExtension;
			if (!extension) return E_OUTOFMEMORY;
			const HRESULT hr = extension->QueryInterface(riid, out);
			extension->Release();
			return hr;
		}
		HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }
	} g_factory;
}

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID riid, void** out)
{
	if (clsid != CLSID_ItemGlass) return CLASS_E_CLASSNOTAVAILABLE;
	return g_factory.QueryInterface(riid, out);
}

extern "C" HRESULT __stdcall DllCanUnloadNow() { return S_FALSE; }

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, void*)
{
	if (reason != DLL_PROCESS_ATTACH) return TRUE;
	wchar_t exe[MAX_PATH], name[64];
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	const wchar_t* file = wcsrchr(exe, L'\\');
	g_inExplorer = file && !_wcsicmp(file + 1, L"explorer.exe");
	// Another app's dialog keeps the DLL only when the user asked for it. Refusing the load leaves nothing behind.
	if (!g_inExplorer && !ReadSwitch(L"FluidGlassDialogs", 0)) return FALSE;
	// One copy per process: a second load (the host started again, or the shell after the host) is refused,
	// so only the first copy's hooks run.
	swprintf_s(name, L"Local\\FluidGlassItemGlass%lu", GetCurrentProcessId());
	const HANDLE once = CreateMutexW(nullptr, FALSE, name);
	if (!once || GetLastError() == ERROR_ALREADY_EXISTS)
	{
		if (once) CloseHandle(once);
		return FALSE;
	}
	// The hooks point into this copy for the life of the process, so it must never unload.
	HMODULE pinned{};
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&DllMain), &pinned);
	DisableThreadLibraryCalls(module);
	if (const HANDLE thread = CreateThread(nullptr, 0, Run, nullptr, 0, nullptr)) CloseHandle(thread);
	return TRUE;
}
