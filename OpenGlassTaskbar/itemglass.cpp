// File Explorer item glass, loaded into explorer by pillhost.exe. It replaces the grey hover and selection of the
// folder view and the navigation pane with dark glass: a translucent rounded fill tinted with the accent color, a light
// rim and a sheen, rounded like the window corners. The keyboard focus outline becomes a glass rim, and the classic
// tooltips get a dark glass face in a window DWM rounds. Only theme parts it knows are replaced. Anything else, or a
// target it cannot draw into, keeps the theme's own drawing.
// FluidGlassItems (HKCU DWM key, default on) turns it off and on while it runs. It loads once per explorer and stays.
// It is also a shell folder extension, so the Open and Save dialogs of other apps load it with their folder view.
// There it only stays when FluidGlassDialogs is set (off when the value is missing, but the installer writes it on),
// and it installs its hooks only once that process shows a folder view (see WaitForView).
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
#include <string_view>
#include <vector>
#include <intrin.h>
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
	decltype(&DrawThemeTextEx) g_drawThemeTextEx = DrawThemeTextEx;
	HMODULE g_module{};
	std::atomic<int> g_grayText{ 1 };	// FluidGlassGrayText: 0 off, 1 halo, 2 frosted patch behind the grey item text in light mode
	std::atomic<int> g_grayStrength{ 60 };	// FluidGlassGrayTextStrength: halo peak opacity in percent, 0 to 100

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

	// Grey-text probe, log only: which API and colour Explorer uses for each kind of item text (name, type, size),
	// so a later build can darken or glow only the secondary text in light mode. It changes nothing on screen.
	// Every unique combination of API, calling module, theme class, part, state and colour is logged once.
	decltype(&GetThemeColor) g_getThemeColor = GetThemeColor;
	decltype(&GetThemeSysColor) g_getThemeSysColor = GetThemeSysColor;
	decltype(&DrawThemeText) g_drawThemeText = DrawThemeText;
	decltype(&DrawTextW) g_drawTextW = DrawTextW;
	decltype(&DrawTextExW) g_drawTextExW = DrawTextExW;
	decltype(&ExtTextOutW) g_extTextOutW = ExtTextOutW;
	decltype(&GetSysColor) g_getSysColor = GetSysColor;
	decltype(&SetTextColor) g_setTextColor = SetTextColor;

	std::mutex g_probeLock;	// own lock: the probe can run while the code above holds g_lock
	std::map<HTHEME, std::wstring> g_probeNames;
	std::map<unsigned long long, bool> g_probeSeen;
	std::atomic<bool> g_probeFull{};
	constexpr size_t ProbeLimit = 400;
	thread_local bool t_probing{};	// the APIs call each other, so only the outermost call is logged

	struct ProbeScope
	{
		bool outer;
		ProbeScope() : outer(!t_probing) { t_probing = true; }
		~ProbeScope() { if (outer) t_probing = false; }
	};

	void ProbeName(HTHEME theme, LPCWSTR classList)
	{
		std::lock_guard guard{ g_probeLock };
		g_probeNames[theme] = classList;
	}

	unsigned long long ProbeMix(unsigned long long hash, unsigned long long value)
	{
		hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
		return hash;
	}

	// True the first time this key is seen. Fills the theme class name for the log line.
	bool ProbeFirst(unsigned long long key, HTHEME theme, std::wstring& name)
	{
		if (g_probeFull) return false;
		std::lock_guard guard{ g_probeLock };
		if (theme)
		{
			if (const auto found = g_probeNames.find(theme); found != g_probeNames.end()) name = found->second;
			else name = L"?";
		}
		if (g_probeSeen.count(key)) return false;
		g_probeSeen[key] = true;
		if (g_probeSeen.size() >= ProbeLimit)
		{
			g_probeFull = true;
			Log(L"probe: limit reached, logging stops");
		}
		return true;
	}

	unsigned long long ProbeClassHash(HTHEME theme)
	{
		if (!theme) return 0;
		std::lock_guard guard{ g_probeLock };
		const auto found = g_probeNames.find(theme);
		return found == g_probeNames.end() ? 1 : std::hash<std::wstring>{}(found->second);
	}

	// Calling module as "name+offset", so a later hook can tell Explorer's own text from the ribbon's or a dialog's.
	void ProbeCaller(void* address, wchar_t* out, size_t size)
	{
		PVOID base{};
		RtlPcToFileHeader(address, &base);
		wchar_t path[MAX_PATH]{};
		if (!base || !GetModuleFileNameW(static_cast<HMODULE>(base), path, MAX_PATH))
		{
			swprintf_s(out, size, L"?");
			return;
		}
		const wchar_t* file = wcsrchr(path, L'\\');
		swprintf_s(out, size, L"%s+%llx", file ? file + 1 : path, static_cast<unsigned long long>(static_cast<BYTE*>(address) - static_cast<BYTE*>(base)));
	}

	unsigned long long ProbeBase(void* address)
	{
		PVOID base{};
		RtlPcToFileHeader(address, &base);
		return reinterpret_cast<unsigned long long>(base);
	}

	void ProbeSample(LPCWSTR text, int count, wchar_t* out, size_t size)
	{
		size_t n = 0;
		if (text)
		{
			for (int i = 0; (count < 0 ? text[i] != 0 : i < count) && n + 1 < size && n < 24; ++i)
				out[n++] = text[i] < 32 ? L' ' : text[i];
		}
		out[n] = 0;
	}

	// Font face and height of the DC, plus the text colour, for the text calls.
	void ProbeFont(HDC dc, wchar_t* out, size_t size)
	{
		TEXTMETRICW metrics{};
		wchar_t face[40]{};
		if (dc) { GetTextMetricsW(dc, &metrics); GetTextFaceW(dc, 40, face); }
		swprintf_s(out, size, L"font '%s' h %ld w %ld", face, metrics.tmHeight, metrics.tmWeight);
	}

	void ProbeText(const wchar_t* api, void* caller, HTHEME theme, HDC dc, int part, int state, COLORREF color, DWORD flags, const wchar_t* extra, LPCWSTR text, int count, const RECT* rect)
	{
		unsigned long long key = ProbeMix(std::hash<std::wstring_view>{}(api), ProbeBase(caller));
		key = ProbeMix(key, ProbeClassHash(theme));
		key = ProbeMix(key, (static_cast<unsigned long long>(part) << 32) | static_cast<unsigned>(state));
		key = ProbeMix(key, color);
		key = ProbeMix(key, g_light ? 1 : 2);
		std::wstring name;
		if (!ProbeFirst(key, theme, name)) return;
		wchar_t from[96], font[96], sample[32], line[512];
		ProbeCaller(caller, from, std::size(from));
		ProbeFont(dc, font, std::size(font));
		ProbeSample(text, count, sample, std::size(sample));
		const long height = rect ? rect->bottom - rect->top : -1;
		swprintf_s(line, L"probe %s from %s class '%.50s' part %d state %d color %06lx flags %lx %s rect h %ld %s light %d text '%s'",
			api, from, theme ? name.c_str() : L"-", part, state, static_cast<unsigned long>(color), static_cast<unsigned long>(flags), extra ? extra : L"", height, font, g_light ? 1 : 0, sample);
		Log(line);
	}

	HRESULT WINAPI GetThemeColorHook(HTHEME theme, int part, int state, int property, COLORREF* color)
	{
		ProbeScope scope;
		const HRESULT hr = g_getThemeColor(theme, part, state, property, color);
		if (!scope.outer || g_probeFull) return hr;
		const COLORREF value = SUCCEEDED(hr) && color ? *color : 0xFFFFFFFF;
		void* caller = _ReturnAddress();
		unsigned long long key = ProbeMix(0x47544331, ProbeBase(caller));
		key = ProbeMix(key, ProbeClassHash(theme));
		key = ProbeMix(key, (static_cast<unsigned long long>(part) << 40) | (static_cast<unsigned long long>(state) << 20) | static_cast<unsigned>(property));
		key = ProbeMix(key, value);
		key = ProbeMix(key, g_light ? 1 : 2);
		std::wstring name;
		if (!ProbeFirst(key, theme, name)) return hr;
		wchar_t from[96], line[256];
		ProbeCaller(caller, from, std::size(from));
		swprintf_s(line, L"probe GetThemeColor from %s class '%.50s' part %d state %d prop %d color %06lx hr %lx light %d",
			from, name.c_str(), part, state, property, static_cast<unsigned long>(value), static_cast<unsigned long>(hr), g_light ? 1 : 0);
		Log(line);
		return hr;
	}

	COLORREF WINAPI GetThemeSysColorHook(HTHEME theme, int index)
	{
		ProbeScope scope;
		const COLORREF value = g_getThemeSysColor(theme, index);
		if (!scope.outer) return value;
		void* caller = _ReturnAddress();
		unsigned long long key = ProbeMix(0x47545343, ProbeBase(caller));
		key = ProbeMix(key, ProbeClassHash(theme));
		key = ProbeMix(key, (static_cast<unsigned long long>(index) << 32) | value);
		key = ProbeMix(key, g_light ? 1 : 2);
		std::wstring name;
		if (!ProbeFirst(key, theme, name)) return value;
		wchar_t from[96], line[256];
		ProbeCaller(caller, from, std::size(from));
		swprintf_s(line, L"probe GetThemeSysColor from %s class '%.50s' index %d color %06lx light %d", from, name.c_str(), index, static_cast<unsigned long>(value), g_light ? 1 : 0);
		Log(line);
		return value;
	}

	HRESULT WINAPI DrawThemeTextHook(HTHEME theme, HDC dc, int part, int state, LPCWSTR text, int count, DWORD flags, DWORD flags2, LPCRECT rect)
	{
		ProbeScope scope;
		if (scope.outer)
		{
			COLORREF themed = 0xFFFFFFFF;
			g_getThemeColor(theme, part, state, TMT_TEXTCOLOR, &themed);
			wchar_t extra[64];
			swprintf_s(extra, L"themeText %06lx", static_cast<unsigned long>(themed));
			ProbeText(L"DrawThemeText", _ReturnAddress(), theme, dc, part, state, themed, flags, extra, text, count, rect);
		}
		return g_drawThemeText(theme, dc, part, state, text, count, flags, flags2, rect);
	}

	// Logs one DrawThemeTextEx call. The hook itself sits with the grey-text backdrop, outside the logs block.
	void ProbeDrawThemeTextEx(void* caller, HTHEME theme, HDC dc, int part, int state, LPCWSTR text, int count, DWORD flags, LPRECT rect, const DTTOPTS* options)
	{
		{
			COLORREF themed = 0xFFFFFFFF;
			g_getThemeColor(theme, part, state, TMT_TEXTCOLOR, &themed);
			const DWORD dtt = options ? options->dwFlags : 0;
			const COLORREF color = dtt & DTT_TEXTCOLOR ? options->crText : themed;
			wchar_t extra[128];
			swprintf_s(extra, L"dtt %lx themeText %06lx glow %d composited %d", static_cast<unsigned long>(dtt), static_cast<unsigned long>(themed),
				options && dtt & DTT_GLOWSIZE ? options->iGlowSize : -1, dtt & DTT_COMPOSITED ? 1 : 0);
			ProbeText(L"DrawThemeTextEx", caller, theme, dc, part, state, color, flags, extra, text, count, rect);
		}
	}

	int WINAPI DrawTextWHook(HDC dc, LPCWSTR text, int count, LPRECT rect, UINT format)
	{
		ProbeScope scope;
		if (scope.outer) ProbeText(L"DrawTextW", _ReturnAddress(), nullptr, dc, 0, 0, GetTextColor(dc), format, nullptr, text, count, rect);
		return g_drawTextW(dc, text, count, rect, format);
	}

	int WINAPI DrawTextExWHook(HDC dc, LPWSTR text, int count, LPRECT rect, UINT format, LPDRAWTEXTPARAMS params)
	{
		ProbeScope scope;
		if (scope.outer) ProbeText(L"DrawTextExW", _ReturnAddress(), nullptr, dc, 0, 0, GetTextColor(dc), format, nullptr, text, count, rect);
		return g_drawTextExW(dc, text, count, rect, format, params);
	}

	BOOL WINAPI ExtTextOutWHook(HDC dc, int x, int y, UINT options, const RECT* rect, LPCWSTR text, UINT count, const INT* dx)
	{
		ProbeScope scope;
		if (scope.outer && count)
		{
			const bool glyphs = (options & ETO_GLYPH_INDEX) != 0;
			ProbeText(glyphs ? L"ExtTextOutW(glyphs)" : L"ExtTextOutW", _ReturnAddress(), nullptr, dc, 0, 0, GetTextColor(dc), options, nullptr,
				glyphs ? nullptr : text, static_cast<int>(count), rect);
		}
		return g_extTextOutW(dc, x, y, options, rect, text, count, dx);
	}

	DWORD WINAPI GetSysColorHook(int index)
	{
		ProbeScope scope;
		const DWORD value = g_getSysColor(index);
		if (!scope.outer || g_probeFull) return value;
		void* caller = _ReturnAddress();
		unsigned long long key = ProbeMix(0x47534331, ProbeBase(caller));
		key = ProbeMix(key, (static_cast<unsigned long long>(index) << 32) | value);
		key = ProbeMix(key, g_light ? 1 : 2);
		std::wstring name;
		if (!ProbeFirst(key, nullptr, name)) return value;
		wchar_t from[96], line[192];
		ProbeCaller(caller, from, std::size(from));
		swprintf_s(line, L"probe GetSysColor from %s index %d color %06lx light %d", from, index, static_cast<unsigned long>(value), g_light ? 1 : 0);
		Log(line);
		return value;
	}

	COLORREF WINAPI SetTextColorHook(HDC dc, COLORREF color)
	{
		ProbeScope scope;
		if (scope.outer && !g_probeFull)
		{
			void* caller = _ReturnAddress();
			unsigned long long key = ProbeMix(0x53544331, ProbeBase(caller));
			key = ProbeMix(key, color);
			key = ProbeMix(key, g_light ? 1 : 2);
			std::wstring name;
			if (ProbeFirst(key, nullptr, name))
			{
				wchar_t from[96], line[192];
				ProbeCaller(caller, from, std::size(from));
				swprintf_s(line, L"probe SetTextColor from %s color %06lx light %d", from, static_cast<unsigned long>(color), g_light ? 1 : 0);
				Log(line);
			}
		}
		return g_setTextColor(dc, color);
	}
#endif

	// Grey item text (type, size, status) on glass in light mode. Its colour is fixed by the theme while the glass shows
	// whatever lies behind the window, so on a dark wallpaper it nearly vanishes. Instead of a darker grey, which fails
	// the same way, the text gets local contrast: a soft white halo around its letters, or a frosted patch behind it.
	// Only DUI70's own DrawThemeTextEx calls (the folder view) with a neutral grey colour are touched.
	HMODULE g_dui{};

	bool IsGrey(COLORREF color)
	{
		const int r = GetRValue(color), g = GetGValue(color), b = GetBValue(color);
		return r == g && g == b && r >= 0x50 && r <= 0xA0;
	}

	// Premultiplied white with the given coverage, AlphaBlended onto the DC at the rect's place.
	void BlendWhite(HDC dc, const RECT& place, const std::vector<BYTE>& alpha, int width, int height)
	{
		BITMAPINFO info{};
		info.bmiHeader = { sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB };
		void* bits{};
		HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
		if (!bitmap) return;
		auto* pixels = static_cast<DWORD*>(bits);
		for (size_t i = 0; i < alpha.size(); ++i)
		{
			const DWORD a = alpha[i];
			pixels[i] = (a << 24) | (a << 16) | (a << 8) | a;
		}
		if (HDC memory = CreateCompatibleDC(dc))
		{
			const HGDIOBJ old = SelectObject(memory, bitmap);
			const BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
			GdiAlphaBlend(dc, place.left, place.top, width, height, memory, 0, 0, width, height, blend);
			SelectObject(memory, old);
			DeleteDC(memory);
		}
		DeleteObject(bitmap);
	}

	// Box blur of one channel in place, radius r, horizontal then vertical.
	void Blur(std::vector<BYTE>& map, int width, int height, int r)
	{
		std::vector<BYTE> temp(map.size());
		for (int y = 0; y < height; ++y)
			for (int x = 0; x < width; ++x)
			{
				int sum = 0;
				for (int k = -r; k <= r; ++k) sum += map[y * width + std::clamp(x + k, 0, width - 1)];
				temp[y * width + x] = static_cast<BYTE>(sum / (2 * r + 1));
			}
		for (int y = 0; y < height; ++y)
			for (int x = 0; x < width; ++x)
			{
				int sum = 0;
				for (int k = -r; k <= r; ++k) sum += temp[std::clamp(y + k, 0, height - 1) * width + x];
				map[y * width + x] = static_cast<BYTE>(sum / (2 * r + 1));
			}
	}

	void DrawGreyBackdrop(int mode, HTHEME theme, HDC dc, int part, int state, LPCWSTR text, int count, DWORD flags, const RECT& rect)
	{
		constexpr int Pad = 4;
		const int width = rect.right - rect.left + 2 * Pad, height = rect.bottom - rect.top + 2 * Pad;
		if (width <= 2 * Pad || height <= 2 * Pad || width > 4096 || height > 512) return;

		// The text once, white, into a clear bitmap: its alpha is the exact shape and place of the letters.
		BITMAPINFO info{};
		info.bmiHeader = { sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB };
		void* bits{};
		HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
		if (!bitmap) return;
		HDC memory = CreateCompatibleDC(dc);
		if (!memory) { DeleteObject(bitmap); return; }
		const HGDIOBJ oldBitmap = SelectObject(memory, bitmap);
		const HGDIOBJ oldFont = SelectObject(memory, GetCurrentObject(dc, OBJ_FONT));
		RECT local{ Pad, Pad, width - Pad, height - Pad };
		DTTOPTS options{ sizeof(options) };
		options.dwFlags = DTT_TEXTCOLOR | DTT_COMPOSITED;
		options.crText = RGB(255, 255, 255);
		g_drawThemeTextEx(theme, memory, part, state, text, count, flags & ~(DT_CALCRECT | DT_MODIFYSTRING), &local, &options);
		GdiFlush();
		std::vector<BYTE> alpha(static_cast<size_t>(width) * height);
		const auto* pixels = static_cast<const DWORD*>(bits);
		int left = width, top = height, right = -1, bottom = -1;
		for (int y = 0; y < height; ++y)
			for (int x = 0; x < width; ++x)
			{
				const DWORD p = pixels[y * width + x];
				const BYTE a = static_cast<BYTE>(std::max({ p >> 24, (p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF }));
				alpha[y * width + x] = a;
				if (a > 16) { left = std::min(left, x); right = std::max(right, x); top = std::min(top, y); bottom = std::max(bottom, y); }
			}
		SelectObject(memory, oldFont);
		SelectObject(memory, oldBitmap);
		DeleteDC(memory);
		DeleteObject(bitmap);
		if (right < 0) return;

		if (mode == 1)
		{
			// Halo: the letters grown by one pixel, softened over two more, about 85 % white at its core.
			std::vector<BYTE> grown(alpha.size());
			for (int y = 0; y < height; ++y)
				for (int x = 0; x < width; ++x)
				{
					BYTE m = 0;
					for (int dy = -1; dy <= 1; ++dy)
						for (int dx = -1; dx <= 1; ++dx)
							m = std::max(m, alpha[std::clamp(y + dy, 0, height - 1) * width + std::clamp(x + dx, 0, width - 1)]);
					grown[y * width + x] = m;
				}
			Blur(grown, width, height, 1);
			Blur(grown, width, height, 1);
			const int strength = g_grayStrength;
			for (BYTE& a : grown) a = static_cast<BYTE>(std::min(255, a * 2) * strength / 100);
			alpha.swap(grown);
		}
		else
		{
			// Frosted patch: a rounded rectangle around the text, 60 % white, with a one pixel soft edge.
			const float x0 = left - 3.0f, x1 = right + 4.0f, y0 = top - 1.0f, y1 = bottom + 2.0f, radius = 4.0f;
			for (int y = 0; y < height; ++y)
				for (int x = 0; x < width; ++x)
				{
					const float px = x + 0.5f, py = y + 0.5f;
					const float qx = std::max(std::max(x0 + radius - px, px - (x1 - radius)), 0.0f);
					const float qy = std::max(std::max(y0 + radius - py, py - (y1 - radius)), 0.0f);
					const float outside = std::sqrt(qx * qx + qy * qy) - radius;
					const float cover = std::clamp(0.5f - outside, 0.0f, 1.0f);
					alpha[y * width + x] = static_cast<BYTE>(std::lround(cover * 153));
				}
		}
		const RECT place{ rect.left - Pad, rect.top - Pad, rect.right + Pad, rect.bottom + Pad };
		BlendWhite(dc, place, alpha, width, height);
		static std::atomic<int> s_logged{};
		if (s_logged.exchange(mode) != mode) Log(mode == 1 ? L"grey text: first halo drawn" : L"grey text: first frosted patch drawn");
	}

	HRESULT WINAPI DrawThemeTextExHook(HTHEME theme, HDC dc, int part, int state, LPCWSTR text, int count, DWORD flags, LPRECT rect, const DTTOPTS* options)
	{
		void* caller = _ReturnAddress();
#ifdef FLUIDGLASS_LOGS
		ProbeScope scope;	// held across the drawing below, so the calls it makes inside uxtheme are not logged again
		if (scope.outer) ProbeDrawThemeTextEx(caller, theme, dc, part, state, text, count, flags, rect, options);
#endif
		const int mode = g_grayText;
		if (mode && g_enabled && g_light && rect && dc && text && !(flags & DT_CALCRECT) && options && (options->dwFlags & DTT_TEXTCOLOR) && IsGrey(options->crText))
		{
			if (!g_dui) g_dui = GetModuleHandleW(L"DUI70.dll");
			PVOID base{};
			RtlPcToFileHeader(caller, &base);
			if (g_dui && base == g_dui) DrawGreyBackdrop(mode, theme, dc, part, state, text, count, flags, *rect);
		}
		return g_drawThemeTextEx(theme, dc, part, state, text, count, flags, rect, options);
	}

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
		const auto black = reinterpret_cast<LONG_PTR>(GetStockObject(BLACK_BRUSH));
		// The tab host under the address bar erases with the window colour and the view host with a white brush,
		// white in Light for about half a second before the folder view first paints. Black erases as clear glass.
		// The desktop has its own view classes, so only these two are changed.
		if (window != root && GetClassNameW(window, name, ARRAYSIZE(name)) && (!wcscmp(name, L"ShellTabWindowClass") || !wcscmp(name, L"DirectUIHWND")) && GetClassLongPtrW(window, GCLP_HBRBACKGROUND) != black)
		{
			SetClassLongPtrW(window, GCLP_HBRBACKGROUND, black);
#ifdef FLUIDGLASS_LOGS
			wchar_t line[64];
			swprintf_s(line, L"folder part %s erases black", name);
			Log(line);
#endif
		}
		DWM_SYSTEMBACKDROP_TYPE backdrop{};
		if (SUCCEEDED(DwmGetWindowAttribute(root, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop))) && backdrop >= DWMSBT_MAINWINDOW) return;
		backdrop = DWMSBT_MAINWINDOW;
		DwmSetWindowAttribute(root, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
		const MARGINS sheet{ -1, -1, -1, -1 };
		DwmExtendFrameIntoClientArea(root, &sheet);
		// Explorer can register the class again later, with its grey brush, so this is checked for every window.
		if (GetClassLongPtrW(root, GCLP_HBRBACKGROUND) != black) SetClassLongPtrW(root, GCLP_HBRBACKGROUND, black);
	}

	HTHEME Remember(HTHEME theme, HWND window, LPCWSTR classList)
	{
		if (!theme || !classList) return theme;
#ifdef FLUIDGLASS_LOGS
		TraceTheme(theme, window, classList);
		ProbeName(theme, classList);
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
		if (g_inExplorer) SlimDetoursAttach(reinterpret_cast<PVOID*>(&g_drawThemeTextEx), DrawThemeTextExHook);
#ifdef FLUIDGLASS_LOGS
		if (g_inExplorer)
		{
			SlimDetoursAttach(reinterpret_cast<PVOID*>(&g_dwmSetWindowAttribute), DwmSetWindowAttributeHook);
			SlimDetoursAttach(reinterpret_cast<PVOID*>(&g_dwmExtendFrame), DwmExtendFrameHook);
			SlimDetoursAttach(reinterpret_cast<PVOID*>(&g_fillRect), FillRectHook);
			SlimDetoursAttach(reinterpret_cast<PVOID*>(&g_bufferedPaintSetAlpha), BufferedPaintSetAlphaHook);
			SlimDetoursAttach(reinterpret_cast<PVOID*>(&g_endBufferedPaint), EndBufferedPaintHook);
			SlimDetoursAttach(reinterpret_cast<PVOID*>(&g_getThemeColor), GetThemeColorHook);
			SlimDetoursAttach(reinterpret_cast<PVOID*>(&g_getThemeSysColor), GetThemeSysColorHook);
			SlimDetoursAttach(reinterpret_cast<PVOID*>(&g_drawThemeText), DrawThemeTextHook);
			SlimDetoursAttach(reinterpret_cast<PVOID*>(&g_drawTextW), DrawTextWHook);
			SlimDetoursAttach(reinterpret_cast<PVOID*>(&g_drawTextExW), DrawTextExWHook);
			SlimDetoursAttach(reinterpret_cast<PVOID*>(&g_extTextOutW), ExtTextOutWHook);
			SlimDetoursAttach(reinterpret_cast<PVOID*>(&g_getSysColor), GetSysColorHook);
			SlimDetoursAttach(reinterpret_cast<PVOID*>(&g_setTextColor), SetTextColorHook);
			Log(L"probe: grey-text hooks attached");
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
		DWORD grayText = 1, grayTextSize = sizeof(grayText);
		if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"FluidGlassGrayText", RRF_RT_REG_DWORD, nullptr, &grayText, &grayTextSize) != ERROR_SUCCESS)
		{
			grayTextSize = sizeof(grayText);
			RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\DWM", L"FluidGlassGrayText", RRF_RT_REG_DWORD, nullptr, &grayText, &grayTextSize);
		}
		grayText = std::min<DWORD>(grayText, 2);
		DWORD grayStrength = 60, grayStrengthSize = sizeof(grayStrength);
		if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"FluidGlassGrayTextStrength", RRF_RT_REG_DWORD, nullptr, &grayStrength, &grayStrengthSize) != ERROR_SUCCESS)
		{
			grayStrengthSize = sizeof(grayStrength);
			RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\DWM", L"FluidGlassGrayTextStrength", RRF_RT_REG_DWORD, nullptr, &grayStrength, &grayStrengthSize);
		}
		grayStrength = std::min<DWORD>(grayStrength, 100);
		const bool grayTextChanged =
			(g_grayText.exchange(static_cast<int>(grayText)) != static_cast<int>(grayText)) |
			(g_grayStrength.exchange(static_cast<int>(grayStrength)) != static_cast<int>(grayStrength));
		if (grayTextChanged)
		{
			wchar_t line[64];
			swprintf_s(line, L"grey text mode %lu strength %lu", grayText, grayStrength);
			Log(line);
		}
		if (g_enabled.exchange(enabled) != enabled)
		{
			Log(enabled ? L"turned on" : L"turned off");
			EnumWindows(RefreshFolder, FALSE);
		}
		else if (lightChanged || grayTextChanged)
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
