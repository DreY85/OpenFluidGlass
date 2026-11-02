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
		bool darkText{};	// a tooltip whose theme writes dark text, like the desktop's, which stays light in dark mode
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
	HMODULE g_module{};

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

#ifdef FLUIDGLASS_LOGS
	// Dev trace of a folder window's first seconds, for the dark flash on the first open after a reboot: when its
	// parts are created and shown, which backdrop and frame it asks DWM for, and what its class erases with.
	std::atomic<LONGLONG> g_traceStart{};
	decltype(&DwmSetWindowAttribute) g_dwmSetWindowAttribute = DwmSetWindowAttribute;
	decltype(&DwmExtendFrameIntoClientArea) g_dwmExtendFrame = DwmExtendFrameIntoClientArea;

	decltype(&FillRect) g_fillRect = FillRect;
	decltype(&BufferedPaintSetAlpha) g_bufferedPaintSetAlpha = BufferedPaintSetAlpha;
	decltype(&EndBufferedPaint) g_endBufferedPaint = EndBufferedPaint;
	std::map<HTHEME, std::wstring> g_themeNames;

	// Milliseconds since the last folder window started, or -1 past its first five seconds or outside it. A null
	// window is a memory DC, kept on the chance that it draws for the folder window.
	long long TraceTime(HWND window)
	{
		wchar_t name[32]{};
		LARGE_INTEGER now{}, frequency{};
		QueryPerformanceCounter(&now);
		QueryPerformanceFrequency(&frequency);
		const long long ms = (now.QuadPart - g_traceStart) * 1000 / frequency.QuadPart;
		if (!g_traceStart || ms > 5000 || (window && (!GetClassNameW(GetAncestor(window, GA_ROOT), name, ARRAYSIZE(name)) || wcscmp(name, L"CabinetWClass")))) return -1;
		return ms;
	}

	void Trace(HWND window, const wchar_t* what)
	{
		const long long ms = TraceTime(window);
		if (ms < 0) return;
		wchar_t name[64]{}, line[320];
		if (window) GetClassNameW(window, name, ARRAYSIZE(name));
		swprintf_s(line, L"trace +%lldms t%lu %s %p %s visible=%d", ms, GetCurrentThreadId(), what, window, name, window && IsWindowVisible(window));
		Log(line);
	}

	bool Big(const RECT* rect) { return rect && rect->right - rect->left >= 100 && rect->bottom - rect->top >= 100; }

	int WINAPI FillRectHook(HDC dc, const RECT* rect, HBRUSH brush)
	{
		if (g_traceStart && Big(rect) && TraceTime(nullptr) >= 0)
		{
			LOGBRUSH look{};
			wchar_t what[128];
			if (reinterpret_cast<ULONG_PTR>(brush) < 64) look.lbColor = GetSysColor(static_cast<int>(reinterpret_cast<ULONG_PTR>(brush)) - 1);
			else GetObjectW(brush, sizeof(look), &look);
			swprintf_s(what, L"FillRect %ldx%ld color %06lX", rect->right - rect->left, rect->bottom - rect->top, static_cast<unsigned long>(look.lbColor));
			Trace(WindowFromDC(dc), what);
		}
		return g_fillRect(dc, rect, brush);
	}

	HRESULT WINAPI BufferedPaintSetAlphaHook(HPAINTBUFFER buffer, const RECT* rect, BYTE alpha)
	{
		if (g_traceStart && TraceTime(nullptr) >= 0)
		{
			wchar_t what[96];
			swprintf_s(what, L"BufferedPaintSetAlpha %u %s", alpha, rect ? L"rect" : L"all");
			Trace(WindowFromDC(GetBufferedPaintTargetDC(buffer)), what);
		}
		return g_bufferedPaintSetAlpha(buffer, rect, alpha);
	}

	HRESULT WINAPI EndBufferedPaintHook(HPAINTBUFFER buffer, BOOL update)
	{
		if (g_traceStart && update && TraceTime(nullptr) >= 0)
		{
			RECT rect{};
			GetBufferedPaintTargetRect(buffer, &rect);
			if (Big(&rect))
			{
				wchar_t what[96];
				swprintf_s(what, L"EndBufferedPaint %ldx%ld", rect.right - rect.left, rect.bottom - rect.top);
				Trace(WindowFromDC(GetBufferedPaintTargetDC(buffer)), what);
			}
		}
		return g_endBufferedPaint(buffer, update);
	}

	void TraceTheme(HTHEME theme, HWND window, LPCWSTR classList)
	{
		if (!theme || !classList) return;
		{
			std::lock_guard guard{ g_lock };
			g_themeNames[theme] = classList;
		}
		wchar_t what[96];
		swprintf_s(what, L"OpenThemeData %.60s", classList);
		Trace(window, what);
	}

	void TraceDraw(HTHEME theme, HDC dc, int part, int state, const RECT* rect)
	{
		if (!g_traceStart || !Big(rect) || TraceTime(nullptr) < 0) return;
		std::wstring name;
		{
			std::lock_guard guard{ g_lock };
			if (const auto found = g_themeNames.find(theme); found != g_themeNames.end()) name = found->second;
		}
		wchar_t what[128];
		swprintf_s(what, L"DrawThemeBackground %.60s part %d state %d %ldx%ld", name.c_str(), part, state, rect->right - rect->left, rect->bottom - rect->top);
		Trace(WindowFromDC(dc), what);
	}

	void CALLBACK TraceEvent(HWINEVENTHOOK, DWORD event, HWND window, LONG object, LONG, DWORD, DWORD)
	{
		if (object != OBJID_WINDOW || !window) return;
		wchar_t name[32]{};
		// Parts made while it handles WM_CREATE come before the window itself, so the first of them starts the trace.
		if (event == EVENT_OBJECT_CREATE && TraceTime(nullptr) < 0 && GetClassNameW(GetAncestor(window, GA_ROOT), name, ARRAYSIZE(name)) && !wcscmp(name, L"CabinetWClass"))
		{
			LARGE_INTEGER now{};
			QueryPerformanceCounter(&now);
			g_traceStart = now.QuadPart;
		}
		if (event == EVENT_OBJECT_CREATE && GetClassNameW(window, name, ARRAYSIZE(name)) && !wcscmp(name, L"CabinetWClass"))
		{
			LOGBRUSH brush{};
			const auto handle = reinterpret_cast<HBRUSH>(GetClassLongPtrW(window, GCLP_HBRBACKGROUND));
			wchar_t line[128];
			if (reinterpret_cast<ULONG_PTR>(handle) < 64) swprintf_s(line, L"trace folder class brush is system color %Iu", reinterpret_cast<ULONG_PTR>(handle) - 1);
			else if (GetObjectW(handle, sizeof(brush), &brush)) swprintf_s(line, L"trace folder class brush style %u color %06lX", brush.lbStyle, static_cast<unsigned long>(brush.lbColor));
			else swprintf_s(line, L"trace folder class brush %p unreadable", handle);
			Log(line);
		}
		Trace(window, event == EVENT_OBJECT_CREATE ? L"create" : event == EVENT_OBJECT_SHOW ? L"show" : event == EVENT_OBJECT_HIDE ? L"hide" : L"destroy");
	}

	HRESULT WINAPI DwmSetWindowAttributeHook(HWND window, DWORD attribute, LPCVOID value, DWORD size)
	{
		const HRESULT hr = g_dwmSetWindowAttribute(window, attribute, value, size);
		wchar_t what[64];
		swprintf_s(what, L"DwmSetWindowAttribute %lu = %ld hr %08lX", attribute, value && size >= sizeof(LONG) ? *static_cast<const LONG*>(value) : 0L, static_cast<unsigned long>(hr));
		Trace(window, what);
		return hr;
	}

	HRESULT WINAPI DwmExtendFrameHook(HWND window, const MARGINS* margins)
	{
		const HRESULT hr = g_dwmExtendFrame(window, margins);
		wchar_t what[80];
		if (!margins) return hr;
		swprintf_s(what, L"DwmExtendFrameIntoClientArea %d %d %d %d", margins->cxLeftWidth, margins->cxRightWidth, margins->cyTopHeight, margins->cyBottomHeight);
		Trace(window, what);
		return hr;
	}
#endif

	// A folder window shows long before it asks DWM for its backdrop: about 0.6 s on the first open after a reboot.
	// Until then it is a plain window, erased grey with a black band on top, and the glass only takes over after.
	// Asking for the backdrop as soon as its first part exists makes it glass from its first frame. Black paints as
	// clear glass and grey as a lighter haze, so the class erases black.
	void CALLBACK FolderEvent(HWINEVENTHOOK hook, DWORD event, HWND window, LONG object, LONG child, DWORD thread, DWORD time)
	{
#ifdef FLUIDGLASS_LOGS
		TraceEvent(hook, event, window, object, child, thread, time);
#endif
		if (event != EVENT_OBJECT_CREATE || object != OBJID_WINDOW || !window) return;
		const HWND root = GetAncestor(window, GA_ROOT);
		wchar_t name[32]{};
		if (!root || !GetClassNameW(root, name, ARRAYSIZE(name)) || wcscmp(name, L"CabinetWClass")) return;
		DWM_SYSTEMBACKDROP_TYPE backdrop{};
		if (SUCCEEDED(DwmGetWindowAttribute(root, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop))) && backdrop >= DWMSBT_MAINWINDOW) return;
		backdrop = DWMSBT_MAINWINDOW;
		DwmSetWindowAttribute(root, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
		const MARGINS sheet{ -1, -1, -1, -1 };
		DwmExtendFrameIntoClientArea(root, &sheet);
		// Explorer can register the class again later, with its grey brush, so this is checked for every window.
		const auto black = reinterpret_cast<LONG_PTR>(GetStockObject(BLACK_BRUSH));
		if (GetClassLongPtrW(root, GCLP_HBRBACKGROUND) != black) SetClassLongPtrW(root, GCLP_HBRBACKGROUND, black);
	}

	HTHEME Remember(HTHEME theme, HWND window, LPCWSTR classList)
	{
		if (!theme || !classList) return theme;
#ifdef FLUIDGLASS_LOGS
		TraceTheme(theme, window, classList);
#endif
		Kind kind = Kind::Other;
		if (!_wcsicmp(classList, L"ItemsView::ListView")) kind = Kind::Items;
		else if (!_wcsicmp(classList, L"ItemsView")) kind = Kind::Focus;
		else if (!_wcsicmp(classList, L"TreeView") || !_wcsicmp(classList, L"Explorer::TreeView")) kind = Kind::Tree;
		else if (!_wcsicmp(classList, L"Tooltip")) kind = Kind::Tooltip;
		// Our dark base under the desktop's dark tooltip text left it grey on black. Such a tooltip keeps its own face.
		bool darkText = false;
		COLORREF text{};
		if (kind == Kind::Tooltip && SUCCEEDED(GetThemeColor(theme, TTP_STANDARD, TTSS_NORMAL, TMT_TEXTCOLOR, &text)))
		{
			darkText = GetRValue(text) * 299 + GetGValue(text) * 587 + GetBValue(text) * 114 < 128 * 1000;
#ifdef FLUIDGLASS_LOGS
			wchar_t line[96];
			swprintf_s(line, L"tooltip theme text %06lX, %s", static_cast<unsigned long>(text), darkText ? L"dark" : L"light");
			Log(line);
#endif
		}
		std::lock_guard guard{ g_lock };
		if (kind == Kind::Other) g_themes.erase(theme);
		else g_themes[theme] = { kind, window, darkText };
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
		if (known.darkText && !g_light) return false;
		Look look{};
		if (!LookFor(known.kind, part, state, look)) return false;
		if (known.kind == Kind::Tooltip) PrepareTooltip(dc, *rect, clip, known.window);
		return DrawGlass(dc, *rect, clip, known.window, look);
	}

	HRESULT WINAPI DrawThemeBackgroundHook(HTHEME theme, HDC dc, int part, int state, LPCRECT rect, LPCRECT clip)
	{
#ifdef FLUIDGLASS_LOGS
		TraceDraw(theme, dc, part, state, rect);
#endif
		if (Replace(theme, dc, part, state, rect, clip)) return S_OK;
		return g_drawThemeBackground(theme, dc, part, state, rect, clip);
	}

	HRESULT WINAPI DrawThemeBackgroundExHook(HTHEME theme, HDC dc, int part, int state, LPCRECT rect, const DTBGOPTS* options)
	{
#ifdef FLUIDGLASS_LOGS
		TraceDraw(theme, dc, part, state, rect);
#endif
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
#ifdef FLUIDGLASS_LOGS
		if (g_inExplorer)
		{
			SlimDetoursAttach(reinterpret_cast<PVOID*>(&g_dwmSetWindowAttribute), DwmSetWindowAttributeHook);
			SlimDetoursAttach(reinterpret_cast<PVOID*>(&g_dwmExtendFrame), DwmExtendFrameHook);
			SlimDetoursAttach(reinterpret_cast<PVOID*>(&g_fillRect), FillRectHook);
			SlimDetoursAttach(reinterpret_cast<PVOID*>(&g_bufferedPaintSetAlpha), BufferedPaintSetAlphaHook);
			SlimDetoursAttach(reinterpret_cast<PVOID*>(&g_endBufferedPaint), EndBufferedPaintHook);
		}
#endif
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
		// The user's own value first, then the standard one the installer wrote for the machine.
		DWORD value = fallback, size = sizeof(value);
		if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
		{
			size = sizeof(value);
			RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\DWM", name, RRF_RT_REG_DWORD, nullptr, &value, &size);
		}
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
		// In context, so a new folder window is seen while it is created, before it shows. The dev trace also wants
		// its parts shown and hidden.
#ifdef FLUIDGLASS_LOGS
		constexpr DWORD lastEvent = EVENT_OBJECT_HIDE;
#else
		constexpr DWORD lastEvent = EVENT_OBJECT_CREATE;
#endif
		if (g_inExplorer && !SetWinEventHook(EVENT_OBJECT_CREATE, lastEvent, g_module, FolderEvent, GetCurrentProcessId(), 0, WINEVENT_INCONTEXT)) Log(L"folder hook failed");
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
	g_module = module;
	if (const HANDLE thread = CreateThread(nullptr, 0, Run, nullptr, 0, nullptr)) CloseHandle(thread);
	return TRUE;
}
