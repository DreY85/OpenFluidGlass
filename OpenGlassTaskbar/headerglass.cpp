// File Explorer's header (tabs, address bar, search box, command bar) is WinUI 3, so the GDI halo in itemglass.cpp never
// reaches it. This part of itemglass.dll joins explorer's WinUI 3 XAML as a diagnostics TAP, the way pilltap.cpp joins the
// taskbar's system XAML, and gives each dark TextBlock the light mode halo the tray uses: white drop shadows cut from the
// text's own alpha mask, with the letters drawn again on top.
// Each File Explorer window runs its XAML on its own thread, and each such thread listens on WinUIVisualDiagConnection<n>,
// numbered from 1. A TAP joins one thread, so every window gets its own TAP.
#include <windows.h>
#include <ocidl.h>
#include <xamlOM.h>
#undef GetCurrentTime	// windows.h macro clashes with C++/WinRT
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
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
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include "pilllog.h"

namespace mux = winrt::Microsoft::UI::Xaml;
namespace muc = winrt::Microsoft::UI::Composition;

// {8B1F4C52-3D6E-4A97-B0C8-2E5D7A9F1C63}
extern const CLSID CLSID_HeaderTap = { 0x8b1f4c52, 0x3d6e, 0x4a97, { 0xb0, 0xc8, 0x2e, 0x5d, 0x7a, 0x9f, 0x1c, 0x63 } };

namespace
{
	using InitializeXamlDiagnosticsEx_t = HRESULT(WINAPI*)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, CLSID, LPCWSTR);

	HMODULE g_module{};
	std::mutex g_logLock;

	std::wstring Hex(HRESULT value)
	{
		wchar_t text[16];
		swprintf_s(text, L"0x%08lX", static_cast<unsigned long>(value));
		return text;
	}

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
		bool operator==(const Style&) const = default;
	};

	class Tap;
	std::mutex g_lock;	// guards everything below
	Style g_style;
	struct Served
	{
		Tap* tap;
		winrt::Microsoft::UI::Dispatching::DispatcherQueue queue;
	};
	std::map<DWORD, Served> g_threads;	// the XAML thread each TAP serves; a second TAP on the same thread stays idle
	std::atomic<int> g_sites{};	// TAPs that got their site, so the attach loop knows a connection really loaded one

	// The header sits on glass and its text is dense, so one tight glow and one soft one are enough. More turns the
	// address bar and the command bar into a white haze.
	constexpr int HaloCores = 1;
	constexpr size_t HaloGlows = HaloCores + 1;
	// The glow is a child visual, drawn above the element's own text, so the letters are drawn once more on top of it.
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

	// Menus, flyouts and tooltips have their own solid faces, so their text keeps the plain look.
	bool InPopup(const mux::DependencyObject& node)
	{
		auto current = mux::Media::VisualTreeHelper::GetParent(node);
		for (int depth = 0; current && depth < 60; ++depth)
		{
			const std::wstring name(winrt::get_class_name(current));
			if (name.find(L"Popup") != std::wstring::npos || name.find(L"Flyout") != std::wstring::npos || name.find(L"ToolTip") != std::wstring::npos
				|| name.find(L"Menu") != std::wstring::npos)
				return true;
			current = mux::Media::VisualTreeHelper::GetParent(current);
		}
		return false;
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
	};

	class Tap final : public IObjectWithSite, public IVisualTreeServiceCallback
	{
	public:
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
		{
			if (!out) return E_POINTER;
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
			++g_sites;
			Log(L"site set");
			// Lives as long as explorer: the handlers below keep using it.
			AddRef();
			std::thread([this]
			{
				IVisualTreeService3* tree{};
				if (SUCCEEDED(m_site->QueryInterface(__uuidof(IVisualTreeService3), reinterpret_cast<void**>(&tree))))
				{
					const HRESULT hr = tree->AdviseVisualTreeChange(this);
					Log(L"advise " + Hex(hr));
					tree->Release();
				}
			}).detach();
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** out) override { return m_site ? m_site->QueryInterface(riid, out) : E_FAIL; }

		// Runs on the window's XAML thread. When the TAP joins, the elements already there arrive as additions too.
		HRESULT STDMETHODCALLTYPE OnVisualTreeChange(ParentChildRelation, VisualElement element, VisualMutationType mutation) override
		{
			if (mutation == Add && element.Type && Claim())
			{
				const std::wstring_view type(element.Type, SysStringLen(element.Type));
				// A FontIcon or SymbolIcon draws its glyph with a TextBlock of its own, which the icon holds.
				const bool text = type == L"Microsoft.UI.Xaml.Controls.TextBlock";
				const bool icon = !text && type.starts_with(L"Microsoft.UI.Xaml.Controls.") && type.ends_with(L"Icon");
				if (text || icon) Queue(element.Handle);
			}
			// The strings are ours to free.
			SysFreeString(element.Type);
			SysFreeString(element.Name);
			SysFreeString(element.SrcInfo.FileName);
			SysFreeString(element.SrcInfo.Hash);
			return S_OK;
		}

		// New settings: every text looks at its colour and the halo's strength again.
		void Refresh()
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
			catch (...) { Log(L"refresh threw"); }
		}

	private:
		static Style CurrentStyle()
		{
			std::lock_guard guard{ g_lock };
			return g_style;
		}

		// The first TAP on a thread owns it. The attach loop can connect to a thread twice if a site arrives late,
		// and two TAPs would draw every halo twice.
		bool Claim()
		{
			if (m_owner) return true;
			if (m_idle) return false;
			const auto queue = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
			if (!queue) return false;
			{
				std::lock_guard guard{ g_lock };
				const auto [entry, added] = g_threads.emplace(GetCurrentThreadId(), Served{ this, queue });
				if (!added && entry->second.tap != this) { m_idle = true; Log(L"thread already served, TAP stays idle"); return false; }
				m_style = g_style;
			}
			m_queue = queue;
			m_owner = true;
			const DWORD thread = GetCurrentThreadId();
			m_queue.ShutdownStarting([this, thread](auto&&, auto&&)
			{
				{
					std::lock_guard guard{ g_lock };
					g_threads.erase(thread);
				}
				try { for (auto& [key, halo] : m_halos) Remove(halo); } catch (...) {}
				m_halos.clear();
				m_pending.clear();
				Log(L"window closed");
			});
			Log(L"serving this window");
			return true;
		}

		// Elements are only looked at once XAML is done adding them: their colour and parents are known by then.
		void Queue(InstanceHandle handle)
		{
			try
			{
				IXamlDiagnostics* diagnostics{};
				if (FAILED(m_site->QueryInterface(__uuidof(IXamlDiagnostics), reinterpret_cast<void**>(&diagnostics)))) return;
				winrt::Windows::Foundation::IInspectable object;
				const HRESULT hr = diagnostics->GetIInspectableFromHandle(handle, reinterpret_cast<::IInspectable**>(winrt::put_abi(object)));
				diagnostics->Release();
				const auto node = SUCCEEDED(hr) && object ? object.try_as<mux::DependencyObject>() : nullptr;
				if (!node) return;
				m_pending.push_back(winrt::make_weak(node));
				if (m_flushQueued) return;
				m_flushQueued = m_queue.TryEnqueue(winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority::Low, [this] { Flush(); });
			}
			catch (...) { Log(L"queue threw"); }
		}

		void Flush()
		{
			m_flushQueued = false;
			try
			{
				auto pending = std::move(m_pending);
				m_pending.clear();
				for (const auto& weak : pending)
				{
					const auto node = weak.get();
					if (!node) continue;
					if (const auto text = node.try_as<mux::Controls::TextBlock>()) { Consider(text); continue; }
					const int count = mux::Media::VisualTreeHelper::GetChildrenCount(node);
					for (int index = 0; index < count; ++index)
						if (const auto text = mux::Media::VisualTreeHelper::GetChild(node, index).try_as<mux::Controls::TextBlock>()) Consider(text);
				}
				std::erase_if(m_halos, [](const auto& entry) { return !entry.second.text.get(); });
#ifdef FLUIDGLASS_LOGS
				if (!m_halos.empty()) ScheduleDump();
#endif
				if (m_halos.size() != m_logged)
				{
					m_logged = m_halos.size();
					size_t lit = 0;
					for (const auto& [key, halo] : m_halos) lit += halo.root && halo.root.IsVisible();
					Log(std::to_wstring(m_logged) + L" texts watched, " + std::to_wstring(lit) + L" with a halo");
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
			if (m_halos.size() >= TextLimit || InPopup(text)) return;
			Halo halo;
			halo.text = winrt::make_weak(text);
			// The colour changes with the theme, with hover and with the window's focus; the halo follows it.
			halo.foregroundToken = text.RegisterPropertyChangedCallback(mux::Controls::TextBlock::ForegroundProperty(), [this, key](const mux::DependencyObject& node, auto&&)
			{
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
			halo.textToken = text.RegisterPropertyChangedCallback(mux::Controls::TextBlock::TextProperty(), [this, key](const mux::DependencyObject& node, auto&&)
			{
				if (const auto found = m_halos.find(key); found != m_halos.end()) Paint(found->second, node.as<mux::Controls::TextBlock>(), true);
			});
			halo.sizeToken = text.SizeChanged([this, key](const winrt::Windows::Foundation::IInspectable& sender, auto&&)
			{
				if (const auto found = m_halos.find(key); found != m_halos.end())
				{
					const auto node = sender.as<mux::Controls::TextBlock>();
					Fit(found->second, node);
					Paint(found->second, node, true);
				}
			});
		}

		static void Fit(Halo& halo, const mux::Controls::TextBlock& text)
		{
			if (halo.sized) return;
			const winrt::Windows::Foundation::Numerics::float2 size{ static_cast<float>(text.ActualWidth()), static_cast<float>(text.ActualHeight()) };
			for (auto& sprite : halo.sprites) sprite.Size(size);
		}

		void Paint(Halo& halo, const mux::Controls::TextBlock& text, bool newMask)
		{
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
				for (size_t index = 0; index < HaloGlows; ++index)
				{
					const auto shadow = halo.sprites[index].Shadow().as<muc::DropShadow>();
					const bool core = index < HaloCores;
					shadow.BlurRadius(core ? (std::max)(1.f, size * 0.75f) : size * 1.5f + 1.f);
					shadow.Opacity(core ? strength * 0.85f : strength * 0.45f);
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

		std::atomic<ULONG> m_refs{ 1 };
		IUnknown* m_site{};
		bool m_owner{}, m_idle{}, m_flushQueued{}, m_paintFailed{};
		winrt::Microsoft::UI::Dispatching::DispatcherQueue m_queue{ nullptr };
		Style m_style;
		std::vector<winrt::weak_ref<mux::DependencyObject>> m_pending;
		std::map<void*, Halo> m_halos;
		size_t m_logged{};
#ifdef FLUIDGLASS_LOGS
		winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_dumpTimer{ nullptr };
		int m_dumps{};

		// Logs build only: the header turns opaque white once a TAP joins, so the backgrounds of the top of the window
		// are written down a few seconds after joining, and again later, to see which element paints the white.
		void ScheduleDump()
		{
			if (m_dumpTimer) return;
			m_dumpTimer = m_queue.CreateTimer();
			m_dumpTimer.Interval(std::chrono::seconds(4));
			m_dumpTimer.IsRepeating(true);
			m_dumpTimer.Tick([this](auto&&, auto&&)
			{
				if (++m_dumps >= 3) m_dumpTimer.Stop();
				try
				{
					for (const auto& [key, halo] : m_halos)
						if (const auto text = halo.text.get(); text && text.XamlRoot())
						{
							if (const auto content = text.XamlRoot().Content()) { Dump(content); return; }
						}
					Log(L"dump: no root");
				}
				catch (...) { Log(L"dump threw"); }
			});
			m_dumpTimer.Start();
		}

		static std::wstring Describe(const mux::Media::Brush& brush)
		{
			wchar_t text[256];
			if (const auto solid = brush.try_as<mux::Media::SolidColorBrush>())
			{
				const auto c = solid.Color();
				swprintf_s(text, L"solid #%02X%02X%02X%02X x%.2f", c.A, c.R, c.G, c.B, solid.Opacity());
				return text;
			}
			if (const auto acrylic = brush.try_as<mux::Media::AcrylicBrush>())
			{
				const auto t = acrylic.TintColor(), f = acrylic.FallbackColor();
				const auto luminosity = acrylic.TintLuminosityOpacity();
				swprintf_s(text, L"acrylic tint #%02X%02X%02X%02X op %.2f lum %.2f fallback #%02X%02X%02X%02X always %d x%.2f", t.A, t.R, t.G, t.B,
					acrylic.TintOpacity(), luminosity ? luminosity.Value() : -1.0, f.A, f.R, f.G, f.B, acrylic.AlwaysUseFallback() ? 1 : 0, acrylic.Opacity());
				return text;
			}
			swprintf_s(text, L"%s x%.2f", winrt::get_class_name(brush).c_str(), brush.Opacity());
			return text;
		}

		void Dump(const mux::UIElement& root)
		{
			Log(L"dump " + std::to_wstring(m_dumps) + L" begins: backgrounds in the top 200 px");
			std::vector<std::pair<mux::DependencyObject, int>> stack{ { root, 0 } };
			int visited = 0, logged = 0;
			while (!stack.empty() && visited < 6000 && logged < 400)
			{
				const auto [node, depth] = stack.back();
				stack.pop_back();
				++visited;
				const int count = mux::Media::VisualTreeHelper::GetChildrenCount(node);
				for (int index = count - 1; index >= 0; --index) stack.emplace_back(mux::Media::VisualTreeHelper::GetChild(node, index), depth + 1);
				const auto element = node.try_as<mux::FrameworkElement>();
				if (!element || element.ActualWidth() < 1 || element.ActualHeight() < 1) continue;
				mux::Media::Brush brush{ nullptr };
				const wchar_t* kind = L"bg";
				if (const auto panel = node.try_as<mux::Controls::Panel>()) brush = panel.Background();
				else if (const auto border = node.try_as<mux::Controls::Border>()) brush = border.Background();
				else if (const auto control = node.try_as<mux::Controls::Control>()) brush = control.Background();
				else if (const auto presenter = node.try_as<mux::Controls::ContentPresenter>()) brush = presenter.Background();
				else if (const auto shape = node.try_as<mux::Shapes::Shape>()) { brush = shape.Fill(); kind = L"fill"; }
				if (!brush) continue;
				if (const auto solid = brush.try_as<mux::Media::SolidColorBrush>(); solid && (solid.Color().A == 0 || solid.Opacity() <= 0)) continue;
				const auto at = element.TransformToVisual(nullptr).TransformPoint({ 0, 0 });
				if (at.Y > 200) continue;
				wchar_t line[400];
				swprintf_s(line, L"d%d %s '%s' at %.0f,%.0f %.0fx%.0f opacity %.2f %s ", depth, winrt::get_class_name(node).c_str(), element.Name().c_str(), at.X, at.Y,
					element.ActualWidth(), element.ActualHeight(), element.Opacity(), kind);
				Log(line + Describe(brush));
				++logged;
			}
			Log(L"dump ends: " + std::to_wstring(visited) + L" elements visited, " + std::to_wstring(logged) + L" backgrounds");
		}
#endif
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
			auto* tap = new (std::nothrow) Tap;
			if (!tap) return E_OUTOFMEMORY;
			const HRESULT hr = tap->QueryInterface(riid, out);
			tap->Release();
			return hr;
		}
		HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }
	} g_factory;

	BOOL CALLBACK CollectFolder(HWND window, LPARAM out)
	{
		wchar_t name[32]{};
		DWORD pid{};
		GetWindowThreadProcessId(window, &pid);
		if (pid == GetCurrentProcessId() && GetClassNameW(window, name, ARRAYSIZE(name)) && !wcscmp(name, L"CabinetWClass"))
			reinterpret_cast<std::set<HWND>*>(out)->insert(window);
		return TRUE;
	}

	// Joins every File Explorer window's XAML thread. A new window opens a new endpoint a moment after it is created,
	// so for a while after a window appears the endpoints are tried four times a second, and otherwise left alone.
	// A dead window's number never comes back, so numbers below a joined one that never answered are given up.
	DWORD WINAPI Attach(void*)
	{
		wchar_t location[MAX_PATH];
		GetModuleFileNameW(g_module, location, MAX_PATH);
		InitializeXamlDiagnosticsEx_t initialize{};
		std::set<HWND> folders;
		std::set<int> joined, given;
		ULONGLONG searchUntil{};
		for (;;)
		{
			const ULONGLONG now = GetTickCount64();
			if (!initialize)
			{
				// Explorer loads WinUI 3 with its first File Explorer window.
				if (const HMODULE udk = GetModuleHandleW(L"Microsoft.Internal.FrameworkUdk.dll"))
				{
					initialize = reinterpret_cast<InitializeXamlDiagnosticsEx_t>(GetProcAddress(udk, "InitializeXamlDiagnosticsEx"));
					Log(initialize ? L"WinUI 3 loaded" : L"WinUI 3 has no InitializeXamlDiagnosticsEx");
					if (!initialize) return 0;
					searchUntil = now + 15000;
				}
			}
			std::set<HWND> current;
			EnumWindows(CollectFolder, reinterpret_cast<LPARAM>(&current));
			if (std::any_of(current.begin(), current.end(), [&folders](HWND window) { return !folders.contains(window); })) searchUntil = now + 15000;
			folders = std::move(current);
			const bool searching = initialize && now < searchUntil;
			if (searching)
			{
				const int highest = joined.empty() ? 0 : *joined.rbegin();
				for (int number = 1; number <= highest + 3; ++number)
				{
					if (joined.contains(number) || given.contains(number)) continue;
					wchar_t endpoint[48];
					swprintf_s(endpoint, L"WinUIVisualDiagConnection%d", number);
					const int sites = g_sites;
					HRESULT hr{};
					// Diagnostics initialize once per thread, hence a new thread per call.
					std::thread([&] { hr = initialize(endpoint, GetCurrentProcessId(), nullptr, location, CLSID_HeaderTap, nullptr); }).join();
					if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) continue;
					if (FAILED(hr))
					{
						given.insert(number);
						Log(std::wstring(endpoint) + L" failed, error " + Hex(hr));
						continue;
					}
					// XAML can accept a connection and still not load the TAP; then the number is tried again later.
					for (int wait = 0; wait < 20 && g_sites == sites; ++wait) Sleep(50);
					if (g_sites == sites) { Log(std::wstring(endpoint) + L" connected, but no site yet"); continue; }
					joined.insert(number);
					Log(L"joined " + std::wstring(endpoint));
				}
			}
			else if (!joined.empty())
			{
				for (int number = 1; number < *joined.rbegin(); ++number)
					if (!joined.contains(number)) given.insert(number);
			}
			Sleep(searching ? 250 : 1000);
		}
	}
}

HRESULT HeaderGlassClassObject(REFIID riid, void** out) { return g_factory.QueryInterface(riid, out); }

void HeaderGlassStart(HMODULE module)
{
	g_module = module;
	if (const HANDLE thread = CreateThread(nullptr, 0, Attach, nullptr, 0, nullptr)) CloseHandle(thread);
}

// Called by itemglass once a second; a change reaches every window on its own thread.
// The halo only shows on dark text, so light mode needs no switch of its own.
void HeaderGlassSettings(bool on, int strength, int size)
{
	std::lock_guard guard{ g_lock };
	const Style style{ on, strength, size };
	if (style == g_style) return;
	g_style = style;
	Log(std::wstring(on ? L"on" : L"off") + L", strength " + std::to_wstring(strength) + L", size " + std::to_wstring(size));
	for (const auto& [thread, served] : g_threads)
	{
		Tap* const tap = served.tap;
		try { served.queue.TryEnqueue([tap] { tap->Refresh(); }); }
		catch (...) {}
	}
}
