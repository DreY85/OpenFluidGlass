// Fluid Glass taskbar, explorer side. Hides the taskbar's own background while pillhost.exe runs
// and publishes where the groups are (apps, tray, clock) so the host can draw a glass pill under each.
// Loaded into explorer by a short WH_CALLWNDPROC hook, then attaches itself as a XAML diagnostics TAP.
// It never touches an element while it is being added, only its parent, which is already in the tree.
// The approach follows TranslucentTB's ExplorerTAP (GPL-3.0, https://github.com/TranslucentTB/TranslucentTB).
#include <windows.h>
#include <ocidl.h>
#include <xamlOM.h>
#undef GetCurrentTime	// windows.h macro clashes with C++/WinRT
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include "pilllog.h"
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
		FILE* file = OpenLog(L"pilltap.txt");
		if (!file) return;
		SYSTEMTIME now{};
		GetLocalTime(&now);
		fwprintf(file, L"%02u:%02u:%02u.%03u explorer %lu: %s\n", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, GetCurrentProcessId(), line.c_str());
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
			Log(L"site set in module 0x" + std::to_wstring(reinterpret_cast<ULONG_PTR>(g_module)));
			wchar_t name[96];
			PillEventName(name, GetCurrentProcessId(), L"Site");
			if (const HANDLE event = CreateEventW(nullptr, TRUE, FALSE, name)) { SetEvent(event); CloseHandle(event); }
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
			m_iconSize = static_cast<int>(ReadPillSetting(PillIconSize));
			m_style = ReadStyle();
			m_timer.Tick([this](auto&&, auto&&)
			{
				m_iconSize = static_cast<int>(ReadPillSetting(PillIconSize));
				// A new pill size or radius changes no button, so it would not republish on its own: force it.
				if (const auto style = ReadStyle(); style != m_style)
				{
					m_style = style;
					m_published = false;
				}
				Update();
			});
			m_timer.Start();
			Update();
		}

		static constexpr int DefaultIconSize = 24;

		static wux::FrameworkElement FindNamed(const wux::DependencyObject& node, std::wstring_view name, int depth)
		{
			const int count = wux::Media::VisualTreeHelper::GetChildrenCount(node);
			for (int index = 0; index < count; ++index)
			{
				const auto child = wux::Media::VisualTreeHelper::GetChild(node, index);
				const auto element = child.try_as<wux::FrameworkElement>();
				if (element && element.Name() == name) return element;
				if (depth > 0)
					if (auto found = FindNamed(child, name, depth - 1)) return found;
			}
			return nullptr;
		}

		// App icons at the size the Taskbar tab asks for; the taskbar's own is 24. Each button is a fixed-width
		// panel (44 for a task, 45 for Start) holding the icon, so the panel grows by as much as the icon does.
		// Recycled buttons keep the size; buttons that appear later get it on the next layout pass.
		void SizeIcons(int size)
		{
			if (size == DefaultIconSize && !m_iconsSized) return;
			m_iconsSized = size != DefaultIconSize;
			const int count = wux::Media::VisualTreeHelper::GetChildrenCount(m_repeater);
			for (int index = 0; index < count; ++index)
			{
				const auto button = wux::Media::VisualTreeHelper::GetChild(m_repeater, index);
				if (wux::Media::VisualTreeHelper::GetChildrenCount(button) == 0) continue;
				const auto panel = wux::Media::VisualTreeHelper::GetChild(button, 0).try_as<wux::FrameworkElement>();
				const auto icon = panel ? FindNamed(panel, L"Icon", 2) : nullptr;
				if (!icon) continue;
				// The panel has grown exactly as much as its icon, so its own width follows from the two.
				const double iconWidth = std::isnan(icon.Width()) ? DefaultIconSize : icon.Width();
				const double width = panel.Width() - iconWidth + size;
				if (iconWidth != size) { icon.Width(size); icon.Height(size); }
				if (!std::isnan(width) && panel.Width() != width) panel.Width(width);
			}
		}

		static void Hide(const wux::FrameworkElement& element, bool hide)
		{
			if (!element) return;
			const double opacity = hide ? 0.0 : 1.0;
			if (element.Opacity() != opacity) element.Opacity(opacity);
		}

		// Windows draws a square-cornered hover background behind each tray button. On the light glass it showed as a
		// grey block over the pill. While our pills are up, the tall layers of the tray and app buttons (their
		// backgrounds, never the icons) get the pill's rounding, and the empty ones, which only paint a background,
		// are also shrunk to fit inside their pill: the buttons are as tall as the taskbar and reach into the gap
		// between pills. They follow the pill size and radius settings live.
		// Everything gets its own corners and size back when the pills go away.
		struct Rounded
		{
			winrt::weak_ref<wux::DependencyObject> node;
			wux::DependencyProperty property{ nullptr };
			winrt::Windows::Foundation::IInspectable original, originalTransform;
			void* key{};
		};

		static constexpr float HoverInset = 2;	// DIPs between a hover background and the edge of its pill

		static wux::DependencyProperty CornerProperty(const wux::DependencyObject& node)
		{
			namespace controls = wux::Controls;
			if (node.try_as<controls::Border>()) return controls::Border::CornerRadiusProperty();
			if (node.try_as<controls::ContentPresenter>()) return controls::ContentPresenter::CornerRadiusProperty();
			if (node.try_as<controls::Control>()) return controls::Control::CornerRadiusProperty();
			return nullptr;
		}

		static void Restore(const wux::DependencyObject& node, const wux::DependencyProperty& property, const winrt::Windows::Foundation::IInspectable& original)
		{
			if (original == wux::DependencyProperty::UnsetValue()) node.ClearValue(property);
			else node.SetValue(property, original);
		}

		// The pill look the hovers follow. The Taskbar tab writes the sizes, the Fluid Glass tab the pill corner radius.
		struct PillStyle
		{
			DWORD height{}, padding{}, gap{}, join{}, radius{};
			bool operator==(const PillStyle&) const = default;
		};

		static PillStyle ReadStyle()
		{
			// Hundredths of a pixel, like every Fluid Glass value; the default is more than any pill holds, so round ends.
			DWORD radius = 100000, size = sizeof(radius);
			// The user's own value first, then the standard one the installer wrote for the machine.
			if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"FluidGlassPillRadius", RRF_RT_REG_DWORD, nullptr, &radius, &size) != ERROR_SUCCESS)
			{
				size = sizeof(radius);
				RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\DWM", L"FluidGlassPillRadius", RRF_RT_REG_DWORD, nullptr, &radius, &size);
			}
			return { ReadPillSetting(PillHeight), ReadPillSetting(PillPadding), ReadPillSetting(PillGap), ReadPillSetting(PillJoin), radius };
		}

		void RoundTray(const wux::DependencyObject& node, int depth, const std::vector<winrt::Windows::Foundation::Rect>& pills, double pillRadius, double minHeight)
		{
			const int count = wux::Media::VisualTreeHelper::GetChildrenCount(node);
			for (int index = 0; index < count; ++index)
			{
				const auto child = wux::Media::VisualTreeHelper::GetChild(node, index);
				const auto element = child.try_as<wux::FrameworkElement>();
				const auto property = element && element.ActualHeight() >= minHeight ? CornerProperty(child) : nullptr;
				if (property)
				{
					void* const key = winrt::get_abi(child);
					if (m_roundedNodes.insert(key).second)
						m_rounded.push_back({ winrt::make_weak(child), property, child.ReadLocalValue(property), child.ReadLocalValue(wux::UIElement::RenderTransformProperty()), key });
					const auto& rounded = *std::find_if(m_rounded.begin(), m_rounded.end(), [key](const Rounded& r) { return r.key == key; });
					FitHover(element, property, rounded, wux::Media::VisualTreeHelper::GetChildrenCount(child) == 0, pills, pillRadius);
				}
				if (depth > 0) RoundTray(child, depth - 1, pills, pillRadius, minHeight);
			}
		}

		// pillRadius is the corner radius of the pills in DIPs. A hover inside keeps the same distance to the pill edge
		// all around its corners, so it is rounded by that radius less the inset, and never past a capsule.
		static void FitHover(const wux::FrameworkElement& element, const wux::DependencyProperty& property, const Rounded& rounded, bool empty, const std::vector<winrt::Windows::Foundation::Rect>& pills, double pillRadius)
		{
			using winrt::Windows::Foundation::Rect;
			// Measured without our own transform, which TransformToVisual would include.
			Restore(element, wux::UIElement::RenderTransformProperty(), rounded.originalTransform);
			Rect own{};
			Rect target{};
			bool fitted = false;
			if (empty && Bounds(element, own))
			{
				const float centerX = own.X + own.Width / 2, centerY = own.Y + own.Height / 2;
				for (const Rect& pill : pills)
				{
					if (centerX < pill.X || centerX > pill.X + pill.Width || centerY < pill.Y || centerY > pill.Y + pill.Height) continue;
					const float left = (std::max)(own.X, pill.X + HoverInset), top = (std::max)(own.Y, pill.Y + HoverInset);
					const float right = (std::min)(own.X + own.Width, pill.X + pill.Width - HoverInset), bottom = (std::min)(own.Y + own.Height, pill.Y + pill.Height - HoverInset);
					if (right - left < 1 || bottom - top < 1) break;
					target = { left, top, right - left, bottom - top };
					fitted = true;
					break;
				}
			}
			double scaleX = 1, scaleY = 1;
			if (fitted)
			{
				scaleX = target.Width / own.Width;
				scaleY = target.Height / own.Height;
				wux::Media::MatrixTransform transform;
				transform.Matrix({ scaleX, 0, 0, scaleY, target.X - own.X, target.Y - own.Y });
				element.RenderTransform(transform);
			}
			// The radius applies before the shrink, so it is scaled back up along the short side.
			const double width = element.ActualWidth() * scaleX, height = element.ActualHeight() * scaleY;
			const double shown = (std::min)((std::min)(width, height) / 2, (std::max)(0.0, pillRadius - HoverInset));
			const double radius = width < height ? shown / scaleX : shown / scaleY;
			const wux::CornerRadius corners{ radius, radius, radius, radius };
			if (winrt::unbox_value_or<wux::CornerRadius>(element.GetValue(property), {}) != corners)
				element.SetValue(property, winrt::box_value(corners));
		}

		void UnroundTray()
		{
			for (const auto& rounded : m_rounded)
			{
				const auto node = rounded.node.get();
				if (!node) continue;
				Restore(node, rounded.property, rounded.original);
				Restore(node, wux::UIElement::RenderTransformProperty(), rounded.originalTransform);
			}
			m_rounded.clear();
			m_roundedNodes.clear();
		}

		// Once per TAP: what the tray and the app buttons are made of, to know which layer draws the hover background.
		void DumpTray(const wux::DependencyObject& node, int depth, int indent)
		{
			const int count = wux::Media::VisualTreeHelper::GetChildrenCount(node);
			for (int index = 0; index < count; ++index)
			{
				const auto child = wux::Media::VisualTreeHelper::GetChild(node, index);
				const auto element = child.try_as<wux::FrameworkElement>();
				wchar_t line[256];
				swprintf_s(line, L"tray %*s%s '%s' %.0fx%.0f children %d", indent * 2, L"", winrt::get_class_name(child).c_str(),
					element ? element.Name().c_str() : L"", element ? element.ActualWidth() : 0.0, element ? element.ActualHeight() : 0.0,
					wux::Media::VisualTreeHelper::GetChildrenCount(child));
				Log(line);
				if (depth > 0) DumpTray(child, depth - 1, indent + 1);
			}
		}

		static constexpr double MinRoundedHeight = 28;	// taller than any tray icon (at most 24), so only backgrounds
		static constexpr int HoverDepth = 10;	// how deep the button templates go below the tray grid and the app list

		// Bounds of an element on the taskbar, in DIPs; empty when it takes no room.
		static bool Bounds(const wux::FrameworkElement& element, winrt::Windows::Foundation::Rect& out)
		{
			if (element.Visibility() != wux::Visibility::Visible || element.ActualWidth() < 1 || element.ActualHeight() < 1) return false;
			out = element.TransformToVisual(nullptr).TransformBounds({ 0, 0, static_cast<float>(element.ActualWidth()), static_cast<float>(element.ActualHeight()) });
			return true;
		}

		// Where a group starts and ends along the taskbar: left to right, or top to bottom when it sits on a side.
		struct Span
		{
			bool vertical = false;
			double start = 1e9, end = -1e9;
			void Add(const winrt::Windows::Foundation::Rect& r)
			{
				start = (std::min)(start, double(vertical ? r.Y : r.X));
				end = (std::max)(end, double(vertical ? r.Y + r.Height : r.X + r.Width));
			}
			bool Empty() const { return end <= start; }
		};

		// The pills as the host draws them, in DIPs on the taskbar (Layout in pillhost.cpp does the same in pixels):
		// as tall as the Height setting, centered, Padding beyond the icons and half the Gap between tray and clock.
		static std::vector<winrt::Windows::Foundation::Rect> Pills(const winrt::Windows::Foundation::Rect& frame, const Span& apps, const Span& tray, const Span& clock, const PillStyle& style)
		{
			const double padding = style.padding, halfGap = style.gap / 2.0;
			const double across = tray.vertical ? frame.Width : frame.Height;
			const double thickness = (std::min)(static_cast<double>(style.height), across);
			const double side = (tray.vertical ? frame.X : frame.Y) + (across - thickness) / 2;
			std::vector<winrt::Windows::Foundation::Rect> pills;
			const auto add = [&](double start, double end)
			{
				if (end <= start) return;
				if (tray.vertical) pills.push_back({ float(side), float(start), float(thickness), float(end - start) });
				else pills.push_back({ float(start), float(side), float(end - start), float(thickness) });
			};
			if (!apps.Empty()) add(apps.start - padding, apps.end + padding);
			if (style.join || tray.Empty() || clock.Empty())
			{
				const double start = (std::min)(tray.Empty() ? 1e9 : tray.start, clock.Empty() ? 1e9 : clock.start);
				const double end = (std::max)(tray.Empty() ? -1e9 : tray.end, clock.Empty() ? -1e9 : clock.end);
				add(start - padding, end + padding / 2);
			}
			else
			{
				add(tray.start - padding, tray.end - halfGap);
				add(clock.start + halfGap, clock.end + padding / 2);
			}
			return pills;
		}

		void Update()
		{
			try
			{
				const HWND host = FindWindowW(PillHostClass, nullptr);
				Hide(m_fill, host != nullptr);
				Hide(m_stroke, host != nullptr);
				SizeIcons(host ? m_iconSize : DefaultIconSize);
				if (!host || !m_share)
				{
					if (!m_rounded.empty()) UnroundTray();
					m_published = false;
					return;
				}

				winrt::Windows::Foundation::Rect frame{};
				if (!Bounds(m_frame, frame)) return;
				const double scale = m_frame.XamlRoot() ? m_frame.XamlRoot().RasterizationScale() : 1.0;
				// On the left or right of the screen the taskbar is a tall band and its groups follow each other downwards.
				const bool vertical = frame.Height > frame.Width;

				// Apps: Start and every task button laid out on the taskbar. The repeater keeps
				// recycled buttons around off to the side, so only the ones inside the frame count.
				Span apps{ vertical };
				const int count = wux::Media::VisualTreeHelper::GetChildrenCount(m_repeater);
				for (int index = 0; index < count; ++index)
				{
					const auto child = wux::Media::VisualTreeHelper::GetChild(m_repeater, index).try_as<wux::FrameworkElement>();
					winrt::Windows::Foundation::Rect r{};
					if (child && Bounds(child, r) && r.X >= frame.X - 1 && r.X + r.Width <= frame.X + frame.Width + 1 && r.Y >= frame.Y - 1 && r.Y < frame.Y + frame.Height)
						apps.Add(r);
				}

				// Tray: icons, language, wifi and volume. Clock: the date, time and bell button.
				Span tray{ vertical }, clock{ vertical };
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
					if (vertical) return { lround(frame.X * scale), lround(span.start * scale), lround((frame.X + frame.Width) * scale), lround(span.end * scale) };
					return { lround(span.start * scale), lround(frame.Y * scale), lround(span.end * scale), lround((frame.Y + frame.Height) * scale) };
				};
				const RECT next[PillCount]{ toPixels(apps), toPixels(tray), toPixels(clock) };

				if (m_published && memcmp(next, m_last, sizeof(next)) == 0) return;
				// The buttons changed, the pills just came up or their look changed: the hovers follow the pills again.
				const auto pills = Pills(frame, apps, tray, clock, m_style);
				const double pillRadius = m_style.radius / 100.0 / scale;
				RoundTray(m_trayGrid, HoverDepth, pills, pillRadius, MinRoundedHeight);
				// App icons can be set larger than the tray's, so a layer has to be taller than the icon to count.
				RoundTray(m_repeater, HoverDepth, pills, pillRadius, (std::max)(MinRoundedHeight, m_iconSize + 4.0));
				if (!m_trayDumped)
				{
					m_trayDumped = true;
					DumpTray(m_trayGrid, HoverDepth, 0);
					DumpTray(m_repeater, HoverDepth, 0);
				}
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
		bool m_iconsSized{};
		int m_iconSize{ DefaultIconSize };
		wux::FrameworkElement m_frame{ nullptr }, m_fill{ nullptr }, m_stroke{ nullptr }, m_repeater{ nullptr }, m_trayGrid{ nullptr };
		wux::DispatcherTimer m_timer{ nullptr };
		HANDLE m_mapping{};
		PillShare* m_share{};
		RECT m_last[PillCount]{};
		std::vector<Rounded> m_rounded;
		std::set<void*> m_roundedNodes;
		bool m_trayDumped{};
		PillStyle m_style{};
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
		Log(L"attaching from module 0x" + std::to_wstring(reinterpret_cast<ULONG_PTR>(g_module)) + L" " + location);
		const HMODULE xaml = LoadLibraryExW(L"Windows.UI.Xaml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
		const auto initialize = xaml ? reinterpret_cast<InitializeXamlDiagnosticsEx_t>(GetProcAddress(xaml, "InitializeXamlDiagnosticsEx")) : nullptr;
		if (!initialize) { Log(L"no InitializeXamlDiagnosticsEx"); Report(false); return 1; }
		// Diagnostics initialize once per thread, hence a new thread per call.
		// Each XAML thread in explorer listens on VisualDiagConnection<n>, numbered from 1 in the order the threads
		// start, and 0x80070490 means nothing listens yet; the connection fails before XAML ever sees this file.
		// The first endpoint opens when the taskbar's XAML thread starts, so retrying it quickly from here, rather
		// than through the host's hook, catches the taskbar while it is still being built: the hook only loads
		// a copy when the taskbar's thread is free, and that thread is busy for seconds while it builds the taskbar.
		// Right after the endpoint opens, XAML can accept the connection and still never load the TAP,
		// so success only counts once the TAP has its site; until then, connect again.
		wchar_t name[96];
		PillEventName(name, GetCurrentProcessId(), L"Site");
		const HANDLE site = CreateEventW(nullptr, TRUE, FALSE, name);
		if (site) ResetEvent(site);
		HRESULT hr{};
		bool attached{};
		int waits{};
		const ULONGLONG start = GetTickCount64();
		for (int attempt = 1; attempt <= 4 && !attached;)
		{
			std::thread([&] { hr = initialize(L"VisualDiagConnection1", GetCurrentProcessId(), nullptr, location, CLSID_PillTap, nullptr); }).join();
			if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) && GetTickCount64() - start < TapListenLimit)
			{
				++waits;
				Sleep(100);
				continue;
			}
			if (waits) { Log(L"endpoint opened after " + std::to_wstring(waits) + L" tries"); waits = 0; }
			if (FAILED(hr)) break;
			++attempt;
			attached = site && WaitForSingleObject(site, 1000) == WAIT_OBJECT_0;
			Log(attached ? L"attached" : L"connected, but no site after attempt " + std::to_wstring(attempt - 1));
		}
		if (FAILED(hr)) Log(L"could not attach, error 0x" + std::to_wstring(static_cast<unsigned long>(hr)));
		if (site) CloseHandle(site);
		Report(attached);
		return attached ? 0 : 1;
	}

	// The host's LoadLibraryW holds the reference this thread runs on; ending the attempt drops it,
	// so a failed copy unloads and a successful one stays pinned by XAML.
	DWORD WINAPI Install(void*)
	{
		FreeLibraryAndExitThread(g_module, Attach());
	}
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
		// Only explorer installs. No pinning here: a successful attach pins the DLL, and a failed
		// copy must unload, as explorer would otherwise keep it and never take a new one.
		if (name && _wcsicmp(name + 1, L"explorer.exe") == 0)
		{
			// If the thread cannot start, the host's reference only keeps an idle copy loaded.
			if (const HANDLE thread = CreateThread(nullptr, 0, Install, nullptr, 0, nullptr)) CloseHandle(thread);
		}
	}
	return TRUE;
}
