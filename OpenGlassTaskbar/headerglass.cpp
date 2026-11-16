// File Explorer's header (tabs, address bar, search box, command bar) is WinUI 3, so the GDI halo in itemglass.cpp never
// reaches it. This part of itemglass.dll gives each dark TextBlock there the light mode halo the tray uses: white drop
// shadows cut from the text's own alpha mask, with the letters drawn again on top.
//
// It opens no XAML diagnostics connection of its own. WinUI 3 serves one diagnostics client per process: every new
// connection makes a new XamlDiagnostics object and DiagnosticsInterop::Launch swaps it in for the previous one, so
// only the last TAP to join hears about the tree. ExplorerBlurMica keeps the header's backgrounds clear through such
// a TAP. A second TAP takes its tree changes away and the header turns opaque white. So this module listens on
// ExplorerBlurMica's own watcher instead: it takes the watcher's OnVisualTreeChange slot, looks at each new element
// and passes every call on unchanged.
#include <windows.h>
#include <ocidl.h>
#include <xamlOM.h>
#undef GetCurrentTime	// windows.h macro clashes with C++/WinRT
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.UI.h>
#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include "pilllog.h"

namespace mux = winrt::Microsoft::UI::Xaml;
namespace muc = winrt::Microsoft::UI::Composition;

namespace
{
	// The one ExplorerBlurMica build these offsets were read from (the DLL in ThirdParty\ExplorerBlurMica). Any other
	// build leaves the header without a halo and changes nothing.
	constexpr DWORD EbmTimeDateStamp = 0x6ACC38D4;
	constexpr DWORD EbmSizeOfImage = 0xA9000;
	// heap_implements<MBox::VisualTreeWatcher>: IVisualTreeServiceCallback2 is its first base, so slot 3 of that vtable
	// is OnVisualTreeChange.
	constexpr DWORD EbmOnChangeSlot = 0x7E0A8;
	constexpr DWORD EbmOnChange = 0x2B210;
	// VisualTreeWatcher::m_xamlDiagnostics. The members follow two vtable pointers, the reference count and a
	// std::mutex; m_advised and m_stopping sit at 0x78 and 0x79.
	constexpr size_t EbmDiagnostics = 0x68;

	std::mutex g_logLock;

	void Log([[maybe_unused]] const std::wstring& line)
	{
#ifdef FLUIDGLASS_LOGS
		std::lock_guard guard{ g_logLock };
		FILE* file = OpenLog(L"headerglass.txt");
		if (!file) return;
		SYSTEMTIME now{};
		GetLocalTime(&now);
		fwprintf(file, L"%02u:%02u:%02u.%03u explorer %lu thread %lu: %s\n", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, GetCurrentProcessId(),
			GetCurrentThreadId(), line.c_str());
		fclose(file);
#endif
	}

	// The settings itemglass reads once a second (FluidGlassItems, FluidGlassGrayText and its strength and size).
	struct Style
	{
		bool on{};
		int strength{ 60 }, size{ 3 };
		bool light{ true };	// the apps' mode; a change makes every text look at its colour again
		// The user's colours for text and for icon glyphs (FluidGlassTextColorDark and so on), 0x01000000 | COLORREF or
		// 0 for the system's. Index 1 is light mode.
		DWORD text[2]{}, second[2]{}, icon[2]{};
		bool operator==(const Style&) const = default;
	};

	// The plain text colour of a mode, the one a user colour replaces: black or white, at full or secondary strength.
	// Accent colours keep theirs, and so do disabled and placeholder text, which are fainter.
	bool PlainInk(const winrt::Windows::UI::Color& color, bool light)
	{
		if (color.A < 0x80) return false;
		if ((std::max)({ color.R, color.G, color.B }) - (std::min)({ color.R, color.G, color.B }) > 8) return false;
		return light ? color.R <= 0xA0 : color.R >= 0x60;
	}

	winrt::Windows::UI::Color UserInk(DWORD value, uint8_t alpha)
	{
		return { alpha, GetRValue(value), GetGValue(value), GetBValue(value) };
	}

	class Header;
	std::mutex g_lock;	// guards everything below
	Style g_style;
	std::map<DWORD, Header*> g_threads;	// the XAML thread of each File Explorer window

	// A drop shadow of thin letters peaks far below its opacity, so a few tight glows are stacked to reach the strength
	// the item list's halo shows at the same setting, with one soft glow around them.
	constexpr int HaloCores = 6;
	constexpr size_t HaloGlows = HaloCores + 1;
	constexpr size_t TextLimit = 1500;	// per window, so a huge Home page cannot pile up visuals without end

	// The mask brush stretches its surface over the sprite. The same surface drawn 1:1 from the top left lines up
	// whatever the surface's size (pilltap.cpp found this on the clock).
	muc::CompositionBrush PinMask(const muc::CompositionBrush& mask)
	{
		const auto source = mask ? mask.try_as<muc::CompositionSurfaceBrush>() : nullptr;
		if (!source) return mask;
		const auto pinned = source.Compositor().CreateSurfaceBrush(source.Surface());
		pinned.Stretch(muc::CompositionStretch::None);
		pinned.HorizontalAlignmentRatio(0);
		pinned.VerticalAlignmentRatio(0);
		pinned.Offset(source.Offset());
		pinned.Scale(source.Scale());
		pinned.CenterPoint(source.CenterPoint());
		pinned.TransformMatrix(source.TransformMatrix());
		return pinned;
	}

	// The text's colour when it is dark enough to need the halo: light mode only.
	bool DarkInk(const mux::Controls::TextBlock& text, winrt::Windows::UI::Color& out)
	{
		const auto brush = text.Foreground().try_as<mux::Media::SolidColorBrush>();
		if (!brush) return false;
		out = brush.Color();
		out.A = static_cast<uint8_t>(out.A * std::clamp(brush.Opacity(), 0.0, 1.0));
		return out.A > 0 && out.R * 299 + out.G * 587 + out.B * 114 < 128 * 1000;
	}

	// Menus, flyouts and tooltips have their own solid faces, so their text keeps the plain look. They live outside the
	// window's content: their parents never lead to it. (Class names do not tell: the command bar's Sort and View
	// buttons are menu buttons that sit in the header itself.)
	bool InPopup(const mux::Controls::TextBlock& text)
	{
		const auto xamlRoot = text.XamlRoot();
		const auto content = xamlRoot ? xamlRoot.Content() : nullptr;
		if (!content) return true;
		mux::DependencyObject current = text;
		for (int depth = 0; current && depth < 200; ++depth)
		{
			if (current == content) return false;
			current = mux::Media::VisualTreeHelper::GetParent(current);
		}
		return true;
	}

	// One TextBlock. Every text is watched for its colour, but the visuals are only made once it turns dark.
	struct Halo
	{
		winrt::weak_ref<mux::Controls::TextBlock> text;
		muc::ContainerVisual root{ nullptr };
		std::vector<muc::SpriteVisual> sprites;	// the glows, then the letters
		muc::CompositionColorBrush ink{ nullptr };
		muc::CompositionMaskBrush letters{ nullptr };
		bool sized{};	// the sprites follow the element visual's size through an expression
		// A shadow without a mask is cast by the whole sprite, a white box the size of the text's slot. The halo only
		// shows once the text gave its mask.
		bool masked{};
		int64_t foregroundToken{ -1 }, textToken{ -1 };
		winrt::event_token sizeToken{};
		// The user's colour is a Foreground value of the text's own: the theme's brushes are sealed and cannot be
		// recoloured. An icon's glyph (a TextBlock inside a FontIcon or SymbolIcon) takes the icon colour. A local value
		// hides the disabled colour, so the text drops it while its control is disabled.
		bool glyph{};
		mux::Media::SolidColorBrush tint{ nullptr };
		winrt::weak_ref<mux::Controls::Control> owner;
		mux::Controls::Control::IsEnabledChanged_revoker enabledRevoker;
	};

	// The halos of one XAML thread. Explorer runs each window's XAML on its own thread, and a thread can hold more
	// than one window (Explorer keeps a hidden one ready). The object is never freed: queued work may still hold it
	// while the thread shuts down, and it is small.
	class Header
	{
	public:
		// The header of the calling XAML thread, made on first use. Null on a thread without WinUI 3 or one that is
		// shutting down.
		static Header* ForThread()
		{
			const DWORD thread = GetCurrentThreadId();
			Header* known{};
			{
				std::lock_guard guard{ g_lock };
				if (const auto found = g_threads.find(thread); found != g_threads.end()) known = found->second;
			}
			if (known && !known->m_closed) return known;
			const auto queue = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
			if (!queue) return nullptr;
			// A closed header whose queue is still this thread's: the window is going away. A different queue means
			// the thread id came back for a new window.
			if (known && known->m_queue == queue) return nullptr;
			auto* const header = new Header(queue);
			{
				std::lock_guard guard{ g_lock };
				g_threads[thread] = header;
				header->m_style = g_style;
			}
			queue.ShutdownStarting([header](auto&&, auto&&)
			{
				header->m_closed = true;
				try { for (auto& [key, halo] : header->m_halos) Remove(halo); } catch (...) {}
				header->m_halos.clear();
				header->m_pending.clear();
				header->m_stale.clear();
				try { if (header->m_repaintTimer) header->m_repaintTimer.Stop(); } catch (...) {}
				try { if (header->m_staleTimer) header->m_staleTimer.Stop(); } catch (...) {}
				header->m_roots.clear();
				Log(L"window closed");
			});
			Log(L"serving this window");
			return header;
		}

		bool Closed() const { return m_closed; }
		// Until a window's root is known every element is worth a look; after that only texts and icons are.
		bool Rooted() const { return !m_roots.empty(); }
		winrt::Microsoft::UI::Dispatching::DispatcherQueue Queue() const { return m_queue; }

		// Elements are only looked at once XAML is done adding them: their colour and parents are known by then.
		void Adopt(const mux::DependencyObject& node)
		{
			try
			{
				m_pending.push_back(winrt::make_weak(node));
				if (m_flushQueued) return;
				m_flushQueued = m_queue.TryEnqueue(winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority::Low, [this] { Flush(); });
			}
			catch (...) { Log(L"adopt threw"); }
		}

		// New settings: every text looks at its colour and the halo's strength again.
		void Refresh()
		{
			if (m_closed) return;
			Repaint();
			try { LogCount(); } catch (...) {}
			StartRepaints();
		}

	private:
		void Repaint()
		{
			try
			{
				m_style = CurrentStyle();
				for (auto it = m_halos.begin(); it != m_halos.end();)
				{
					const auto text = it->second.text.get();
					if (!text) { it = m_halos.erase(it); continue; }
					Paint(it->second, text, false);
					++it;
				}
			}
			catch (...) { Log(L"repaint threw"); }
		}

		// A switch between light and dark mode recolours the header's text without a Foreground change on the
		// TextBlock itself (the colour is inherited, or the brush is recoloured in place), and it reaches the window
		// a moment after the setting changes. So the texts are looked at again a few times over the next seconds.
		// Without this the halo kept drawing black letters over text that had turned white.
		void StartRepaints()
		{
			if (m_closed) return;
			try
			{
				m_repaints = 6;
				if (!m_repaintTimer)
				{
					m_repaintTimer = m_queue.CreateTimer();
					m_repaintTimer.Interval(std::chrono::milliseconds(500));
					m_repaintTimer.IsRepeating(true);
					m_repaintTimer.Tick([this](auto&&, auto&&)
					{
						if (--m_repaints <= 0 || m_closed) m_repaintTimer.Stop();
						if (!m_closed) Repaint();
					});
				}
				if (!m_repaintTimer.IsRunning()) m_repaintTimer.Start();
			}
			catch (...) { Log(L"repaint timer threw"); }
		}

		void LogCount()
		{
			size_t lit = 0, unmasked = 0, glyphs = 0, tinted = 0;
			for (const auto& [key, halo] : m_halos)
			{
				lit += halo.root && halo.root.IsVisible();
				unmasked += halo.root && !halo.masked;
				glyphs += halo.glyph;
				tinted += halo.tint != nullptr;
			}
			Log(std::to_wstring(m_halos.size()) + L" texts watched (" + std::to_wstring(glyphs) + L" icon glyphs, " + std::to_wstring(tinted) + L" in the user's colour), " +
				std::to_wstring(lit) + L" with a halo, " + std::to_wstring(unmasked) + L" without a mask, " +
				std::to_wstring(m_popupTexts) + L" left out as popup text");
		}

		explicit Header(const winrt::Microsoft::UI::Dispatching::DispatcherQueue& queue) : m_queue(queue) {}

		static Style CurrentStyle()
		{
			std::lock_guard guard{ g_lock };
			return g_style;
		}

		// Remembers the XAML root of the window this element is in. A window that was built before the watch began
		// (or whose texts were missed) is read once from its root.
		void NoteRoot(const mux::DependencyObject& node)
		{
			try
			{
				const auto element = node.try_as<mux::UIElement>();
				const auto xamlRoot = element ? element.XamlRoot() : nullptr;
				const auto content = xamlRoot ? xamlRoot.Content() : nullptr;
				if (!content) return;
				void* const key = winrt::get_abi(content);
				if (const auto found = m_roots.find(key); found != m_roots.end() && found->second.get() == content) return;
				std::erase_if(m_roots, [](const auto& entry) { return !entry.second.get(); });
				m_roots[key] = winrt::make_weak(content);
				Log(L"window root found, " + std::to_wstring(m_roots.size()) + L" known");
				if (const auto frame = content.try_as<mux::FrameworkElement>()) frame.ActualThemeChanged([this](auto&&, auto&&) { StartRepaints(); });
				Scan(content);
			}
			catch (...) {}
		}

		void Scan(const mux::UIElement& root)
		{
			std::vector<mux::DependencyObject> stack{ root };
			for (int visited = 0; !stack.empty() && visited < 6000; ++visited)
			{
				const auto node = stack.back();
				stack.pop_back();
				if (const auto text = node.try_as<mux::Controls::TextBlock>()) { Consider(text); continue; }
				const int count = mux::Media::VisualTreeHelper::GetChildrenCount(node);
				for (int index = 0; index < count; ++index) stack.push_back(mux::Media::VisualTreeHelper::GetChild(node, index));
			}
		}

		void Flush()
		{
			m_flushQueued = false;
			if (m_closed) return;
			try
			{
				auto pending = std::move(m_pending);
				m_pending.clear();
				for (const auto& weak : pending)
				{
					const auto node = weak.get();
					if (!node) continue;
					NoteRoot(node);
					if (const auto text = node.try_as<mux::Controls::TextBlock>()) { Consider(text); continue; }
					// A FontIcon or SymbolIcon draws its glyph with a TextBlock of its own, which the icon holds.
					const int count = mux::Media::VisualTreeHelper::GetChildrenCount(node);
					for (int index = 0; index < count; ++index)
						if (const auto text = mux::Media::VisualTreeHelper::GetChild(node, index).try_as<mux::Controls::TextBlock>()) Consider(text);
				}
				std::erase_if(m_halos, [](const auto& entry) { return !entry.second.text.get(); });
				if (m_halos.size() != m_logged)
				{
					m_logged = m_halos.size();
					LogCount();
				}
			}
			catch (...) { Log(L"flush threw"); }
		}

		void Consider(const mux::Controls::TextBlock& text)
		{
			void* const key = winrt::get_abi(text);
			if (const auto found = m_halos.find(key); found != m_halos.end())
			{
				// A freed TextBlock's address can come back as a new one.
				if (found->second.text.get() == text) return;
				m_halos.erase(found);
			}
			if (m_halos.size() >= TextLimit) return;
			if (InPopup(text)) { ++m_popupTexts; return; }
			Halo halo;
			halo.text = winrt::make_weak(text);
			try
			{
				const auto parent = mux::Media::VisualTreeHelper::GetParent(text);
				// The glyph sits a level or two below its icon: a FontIcon holds a Grid that holds the TextBlock.
				mux::DependencyObject up = parent;
				for (int depth = 0; up && depth < 4 && !halo.glyph; ++depth, up = mux::Media::VisualTreeHelper::GetParent(up))
					halo.glyph = static_cast<bool>(up.try_as<mux::Controls::IconElement>());
				mux::DependencyObject current = parent;
				for (int depth = 0; current && depth < 12; ++depth, current = mux::Media::VisualTreeHelper::GetParent(current))
				{
					const auto control = current.try_as<mux::Controls::Control>();
					if (!control) continue;
					halo.owner = winrt::make_weak(control);
					halo.enabledRevoker = control.IsEnabledChanged(winrt::auto_revoke, [this, key](auto&&, auto&&)
					{
						const auto found = m_halos.find(key);
						if (found == m_halos.end()) return;
						if (const auto text = found->second.text.get()) Paint(found->second, text, false);
					});
					break;
				}
			}
			catch (...) {}
			// The colour changes with the theme, with hover and with the window's focus; the halo follows it.
			halo.foregroundToken = text.RegisterPropertyChangedCallback(mux::Controls::TextBlock::ForegroundProperty(), [this, key](const mux::DependencyObject& node, auto&&)
			{
				if (m_busy) return;
				if (const auto found = m_halos.find(key); found != m_halos.end()) Paint(found->second, node.as<mux::Controls::TextBlock>(), false);
			});
			auto& stored = m_halos.emplace(key, std::move(halo)).first->second;
			Paint(stored, text, false);
		}

		// The visuals, made the first time the text is dark.
		void Build(Halo& halo, const mux::Controls::TextBlock& text)
		{
			void* const key = winrt::get_abi(text);
			const auto host = mux::Hosting::ElementCompositionPreview::GetElementVisual(text);
			const auto compositor = host.Compositor();
			halo.root = compositor.CreateContainerVisual();
			halo.root.IsVisible(false);
			for (size_t index = 0; index < HaloGlows; ++index)
			{
				const auto glow = compositor.CreateSpriteVisual();
				const auto shadow = compositor.CreateDropShadow();
				shadow.Color(winrt::Windows::UI::Color{ 255, 255, 255, 255 });
				shadow.Offset({ 0, 0, 0 });
				glow.Shadow(shadow);
				halo.root.Children().InsertAtTop(glow);
				halo.sprites.push_back(glow);
			}
			halo.ink = compositor.CreateColorBrush();
			halo.letters = compositor.CreateMaskBrush();
			halo.letters.Source(halo.ink);
			{
				// The glow is a child visual, drawn above the element's own text, so the letters are drawn once more on
				// top of it.
				const auto letters = compositor.CreateSpriteVisual();
				letters.Brush(halo.letters);
				halo.root.Children().InsertAtTop(letters);
				halo.sprites.push_back(letters);
			}
			// The alpha mask covers the element's layout slot, not its ActualWidth, and the element visual carries the
			// slot's size (pilltap.cpp found this on the right-aligned clock).
			try
			{
				const auto follow = compositor.CreateExpressionAnimation(L"host.Size");
				follow.SetReferenceParameter(L"host", host);
				for (auto& sprite : halo.sprites) sprite.StartAnimation(L"Size", follow);
				halo.sized = true;
			}
			catch (...) {}
			Fit(halo, text);
			mux::Hosting::ElementCompositionPreview::SetElementChildVisual(text, halo.root);
			// A new path in the address bar or a new tab title: the mask follows the text.
			halo.textToken = text.RegisterPropertyChangedCallback(mux::Controls::TextBlock::TextProperty(), [this, key](auto&&, auto&&) { Stale(key); });
			halo.sizeToken = text.SizeChanged([this, key](const winrt::Windows::Foundation::IInspectable& sender, auto&&)
			{
				if (const auto found = m_halos.find(key); found != m_halos.end()) Fit(found->second, sender.as<mux::Controls::TextBlock>());
				Stale(key);
			});
		}

		// The text or its size changed. Until XAML has drawn the new letters the alpha mask still holds the old ones,
		// and a halo cut from it would write the old text over the new. So the halo hides at once and fades back in
		// some 60 to 100 ms later with a fresh mask, which is read once more in case the first reading was early.
		void Stale(void* key)
		{
			if (m_closed) return;
			const auto found = m_halos.find(key);
			if (found == m_halos.end()) return;
			try
			{
				if (found->second.root) found->second.root.IsVisible(false);
				m_stale[key] = { 2, GetTickCount64() };
				if (!m_staleTimer)
				{
					m_staleTimer = m_queue.CreateTimer();
					m_staleTimer.Interval(std::chrono::milliseconds(40));
					m_staleTimer.IsRepeating(true);
					m_staleTimer.Tick([this](auto&&, auto&&) { Remask(); });
				}
				if (!m_staleTimer.IsRunning()) m_staleTimer.Start();
			}
			catch (...) { Log(L"stale threw"); }
		}

		void Remask()
		{
			for (auto it = m_stale.begin(); it != m_stale.end();)
			{
				const auto found = m_halos.find(it->first);
				const auto text = found != m_halos.end() ? found->second.text.get() : nullptr;
				if (!text || m_closed) { it = m_stale.erase(it); continue; }
				if (GetTickCount64() - it->second.since < 60) { ++it; continue; }
				auto& halo = found->second;
				const bool first = it->second.readings == 2;
				Paint(halo, text, true);
				if (first && halo.root && halo.root.IsVisible())
				{
					// Back in with a short fade, so the return does not read as a blink.
					try
					{
						const auto compositor = halo.root.Compositor();
						const auto fade = compositor.CreateScalarKeyFrameAnimation();
						fade.InsertKeyFrame(0.f, 0.f);
						fade.InsertKeyFrame(1.f, 1.f);
						fade.Duration(std::chrono::milliseconds(160));
						halo.root.StartAnimation(L"Opacity", fade);
					}
					catch (...) { try { halo.root.Opacity(1.f); } catch (...) {} }
				}
				if (--it->second.readings <= 0) it = m_stale.erase(it);
				else ++it;
			}
			if (m_stale.empty() && m_staleTimer) m_staleTimer.Stop();
		}

		static void Fit(Halo& halo, const mux::Controls::TextBlock& text)
		{
			if (halo.sized) return;
			const winrt::Windows::Foundation::Numerics::float2 size{ static_cast<float>(text.ActualWidth()), static_cast<float>(text.ActualHeight()) };
			for (auto& sprite : halo.sprites) sprite.Size(size);
		}

		// The user's colours, before the halo, so the halo judges and redraws the colour the text ends up with.
		void Recolor(Halo& halo, const mux::Controls::TextBlock& text)
		{
			try
			{
				const auto property = mux::Controls::TextBlock::ForegroundProperty();
				// Our own value comes off first, so the text shows the colour the system gives it.
				m_busy = true;
				if (halo.tint && text.ReadLocalValue(property) == halo.tint) text.ClearValue(property);
				m_busy = false;
				halo.tint = nullptr;
				const bool light = text.ActualTheme() == mux::ElementTheme::Light;
				if (!m_style.icon[light] && !m_style.text[light] && !m_style.second[light]) return;
				if (const auto owner = halo.owner.get(); owner && !owner.IsEnabled()) return;
				const auto brush = text.Foreground().try_as<mux::Media::SolidColorBrush>();
				if (!brush) return;
				const double opacity = std::clamp(brush.Opacity(), 0.0, 1.0);
				auto system = brush.Color();
				system.A = static_cast<uint8_t>(system.A * opacity);
				if (!PlainInk(system, light)) return;
				// WinUI draws secondary text (placeholders, hints) in the same ink as primary text, only fainter
				// (about 0x9E of 0xFF in light mode and 0xC5 in dark mode, against 0xE4 and 0xFF for primary text).
				const bool secondary = !halo.glyph && system.A < 0xD0;
				const DWORD user = halo.glyph ? m_style.icon[light] : secondary ? m_style.second[light] : m_style.text[light];
				if (!user) return;
				// Primary text keeps the system's strength. A secondary colour shows as picked, it already is the softer shade.
				halo.tint = mux::Media::SolidColorBrush(UserInk(user, secondary ? static_cast<uint8_t>(0xFF * opacity) : system.A));
				m_busy = true;
				text.Foreground(halo.tint);
				m_busy = false;
			}
			catch (const winrt::hresult_error& error)
			{
				m_busy = false;
				halo.tint = nullptr;
				if (m_tintFailed++ < 5)
				{
					wchar_t line[48];
					swprintf_s(line, L"tint threw %08X", static_cast<unsigned>(error.code().value));
					Log(line);
				}
			}
			catch (...)
			{
				m_busy = false;
				halo.tint = nullptr;
				if (m_tintFailed++ < 5) Log(L"tint threw");
			}
		}

		void Paint(Halo& halo, const mux::Controls::TextBlock& text, bool newMask)
		{
			Recolor(halo, text);
			try
			{
				winrt::Windows::UI::Color ink{};
				const bool dark = m_style.on && DarkInk(text, ink);
				if (!dark)
				{
					if (halo.root) halo.root.IsVisible(false);
					return;
				}
				if (!halo.root) { Build(halo, text); newMask = true; }
				if (newMask)
				{
					const auto mask = PinMask(text.GetAlphaMask());
					if (!mask)
					{
						halo.root.IsVisible(false);
						halo.masked = false;
						return;
					}
					for (size_t index = 0; index < HaloGlows; ++index) halo.sprites[index].Shadow().as<muc::DropShadow>().Mask(mask);
					halo.letters.Mask(mask);
					halo.masked = true;
				}
				if (!halo.masked) { halo.root.IsVisible(false); return; }
				halo.ink.Color(ink);
				const float strength = m_style.strength / 100.f, size = static_cast<float>(m_style.size);
				// One glow of the header's thin strokes peaks at about erf(0.32 / sigma) of its opacity, with sigma
				// near half the blur radius (measured twice on the breadcrumb). The stacked glows together aim at the
				// strength, and stacking also widens the bright bed around the letters, as the item list's halo has.
				const float coreBlur = (std::max)(1.f, size * 0.75f);
				const float peak = std::erf(0.32f / (coreBlur * 0.5f));
				const float coreOpacity = (std::min)(1.f, (1.f - std::pow(1.f - (std::min)(strength, 0.97f), 1.f / HaloCores)) / peak);
				for (size_t index = 0; index < HaloGlows; ++index)
				{
					const auto shadow = halo.sprites[index].Shadow().as<muc::DropShadow>();
					const bool core = index < HaloCores;
					shadow.BlurRadius(core ? coreBlur : size * 1.5f + 1.f);
					shadow.Opacity(core ? coreOpacity : strength * 0.45f);
				}
				halo.root.IsVisible(true);
			}
			catch (...)
			{
				// Whatever failed, no half-made halo stays on screen.
				try { if (halo.root) halo.root.IsVisible(false); } catch (...) {}
				halo.masked = false;
				if (!m_paintFailed) Log(L"paint threw");
				m_paintFailed = true;
			}
		}

		static void Remove(Halo& halo)
		{
			const auto text = halo.text.get();
			if (!text) return;
			if (halo.root) mux::Hosting::ElementCompositionPreview::SetElementChildVisual(text, nullptr);
			if (halo.foregroundToken >= 0) text.UnregisterPropertyChangedCallback(mux::Controls::TextBlock::ForegroundProperty(), halo.foregroundToken);
			if (halo.textToken >= 0) text.UnregisterPropertyChangedCallback(mux::Controls::TextBlock::TextProperty(), halo.textToken);
			if (halo.root) text.SizeChanged(halo.sizeToken);
		}

		winrt::Microsoft::UI::Dispatching::DispatcherQueue m_queue;
		std::atomic<bool> m_closed{};
		bool m_flushQueued{}, m_paintFailed{};
		int m_tintFailed{};
		bool m_busy{};	// our own Foreground change, which the callback leaves alone
		Style m_style;
		std::vector<winrt::weak_ref<mux::DependencyObject>> m_pending;
		std::map<void*, Halo> m_halos;
		size_t m_logged{}, m_popupTexts{};
		int m_repaints{};
		winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_repaintTimer{ nullptr };
		struct Wait { int readings; ULONGLONG since; };
		std::map<void*, Wait> m_stale;	// halos waiting for a fresh mask, with the readings left
		winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_staleTimer{ nullptr };
		std::map<void*, winrt::weak_ref<mux::UIElement>> m_roots;	// the content of each window seen on this thread
	};

	using OnChange_t = HRESULT(STDMETHODCALLTYPE*)(IVisualTreeServiceCallback*, ParentChildRelation, VisualElement, VisualMutationType);
	OnChange_t g_watcherOnChange{};

	// Runs on the XAML thread of the element. The strings belong to the watcher, which frees them.
	void Peek(IVisualTreeServiceCallback* watcher, const VisualElement& element)
	{
		const std::wstring_view type(element.Type, SysStringLen(element.Type));
		// The same watcher class can serve system XAML too; only WinUI 3 elements are ours.
		if (!type.starts_with(L"Microsoft.UI.Xaml.")) return;
		// A FontIcon or SymbolIcon draws its glyph with a TextBlock of its own, which the icon holds.
		const bool text = type == L"Microsoft.UI.Xaml.Controls.TextBlock";
		const bool icon = !text && type.starts_with(L"Microsoft.UI.Xaml.Controls.") && type.ends_with(L"Icon");
		Header* const header = Header::ForThread();
		if (!header || (!text && !icon && header->Rooted())) return;
		IXamlDiagnostics* const diagnostics = *reinterpret_cast<IXamlDiagnostics**>(reinterpret_cast<BYTE*>(watcher) + EbmDiagnostics);
		if (!diagnostics) return;
		winrt::Windows::Foundation::IInspectable object;
		if (FAILED(diagnostics->GetIInspectableFromHandle(element.Handle, reinterpret_cast<::IInspectable**>(winrt::put_abi(object)))) || !object) return;
		if (const auto node = object.try_as<mux::DependencyObject>()) header->Adopt(node);
	}

	HRESULT STDMETHODCALLTYPE WatcherOnChange(IVisualTreeServiceCallback* watcher, ParentChildRelation relation, VisualElement element, VisualMutationType mutation)
	{
		if (mutation == Add && element.Type)
		{
			try { Peek(watcher, element); }
			catch (...) {}
		}
		return g_watcherOnChange(watcher, relation, element, mutation);
	}

	// Waits for ExplorerBlurMica and takes its watcher's OnVisualTreeChange slot. Explorer loads both DLLs as folder
	// extensions, in no fixed order.
	DWORD WINAPI Listen(void*)
	{
		for (int tries = 0;; ++tries)
		{
			if (const HMODULE module = GetModuleHandleW(L"ExplorerBlurMica.dll"))
			{
				BYTE* const base = reinterpret_cast<BYTE*>(module);
				const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
				const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
				auto* const slot = reinterpret_cast<void**>(base + EbmOnChangeSlot);
				if (nt->FileHeader.TimeDateStamp != EbmTimeDateStamp || nt->OptionalHeader.SizeOfImage != EbmSizeOfImage || *slot != base + EbmOnChange)
				{
					Log(L"unknown ExplorerBlurMica build, the header gets no halo");
					return 0;
				}
				g_watcherOnChange = reinterpret_cast<OnChange_t>(*slot);
				DWORD old{};
				if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
				{
					Log(L"watcher slot is locked, the header gets no halo");
					return 0;
				}
				InterlockedExchangePointer(slot, reinterpret_cast<void*>(&WatcherOnChange));
				VirtualProtect(slot, sizeof(void*), old, &old);
				Log(L"listening on ExplorerBlurMica's watcher");
				return 0;
			}
			Sleep(tries < 480 ? 250 : 2000);
		}
	}
}

void HeaderGlassStart(HMODULE)
{
	if (const HANDLE thread = CreateThread(nullptr, 0, Listen, nullptr, 0, nullptr)) CloseHandle(thread);
}

// Called by itemglass once a second; a change reaches every window on its own thread.
// The halo only shows on dark text, so light mode needs no switch of its own.
void HeaderGlassSettings(bool on, int strength, int size, DWORD textDark, DWORD textLight, DWORD secondDark, DWORD secondLight, DWORD iconDark, DWORD iconLight)
{
	std::lock_guard guard{ g_lock };
	DWORD light = 1, lightSize = sizeof(light);
	RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &lightSize);
	const Style style{ on, strength, size, light != 0, { textDark, textLight }, { secondDark, secondLight }, { iconDark, iconLight } };
	if (style == g_style) return;
	g_style = style;
	wchar_t colors[128];
	swprintf_s(colors, L", text %08lX %08lX, secondary %08lX %08lX, icons %08lX %08lX", textDark, textLight, secondDark, secondLight, iconDark, iconLight);
	Log(std::wstring(on ? L"on" : L"off") + L", strength " + std::to_wstring(strength) + L", size " + std::to_wstring(size) + (light ? L", light" : L", dark") + colors);
	for (const auto& [thread, header] : g_threads)
	{
		if (header->Closed()) continue;
		try { header->Queue().TryEnqueue([header] { header->Refresh(); }); }
		catch (...) {}
	}
}
