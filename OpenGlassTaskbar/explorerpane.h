#pragma once

#include <UIAutomation.h>
#include <wrl/client.h>
#include <dwmapi.h>
#include <algorithm>
#include <cstdio>
#include <optional>
#include <vector>
#include "../OpenGlass/ExplorerPaneRect.hpp"

// Runs on the existing Explorer settings worker. Only reads UI Automation geometry.
// No UIA calls from a paint callback, UI thread or compositor.
namespace ExplorerPaneObserver
{
	using Microsoft::WRL::ComPtr;
	class Observer
	{
		// What one folder window shows. A new reading has to come back the same once more before it replaces the
		// shown value, so a layout caught halfway (items still arriving, a window between two places) never reaches
		// the glass. A reading that fails keeps the shown value, and only a run of failures clears it.
		struct Pane { HWND window{}; std::uint32_t shown{}; std::uint32_t candidate{}; unsigned misses{}; };
		static constexpr unsigned MaxMisses = 3;
		static constexpr ULONGLONG MoveTimeout = 15000;	// ms, in case the end of a drag is never reported

		ComPtr<IUIAutomation> m_ui;
		ComPtr<IUIAutomationTreeWalker> m_walker;
		ComPtr<IUIAutomationCacheRequest> m_cache;
		ComPtr<IUIAutomationCondition> m_view;
		bool m_initialized{};
		std::vector<Pane> m_panes;
		HWINEVENTHOOK m_events{}, m_moves{};
		bool m_enabled{}, m_wasMoving{};
		inline static thread_local bool s_dirty{};
		inline static thread_local HWND s_moving{};
		inline static thread_local ULONGLONG s_movingSince{};
		ULONGLONG m_nextScan{}, m_fallback{};
		void (*m_log)(const wchar_t*) {};

		template <class... Args> void Note(const wchar_t* format, Args... args)
		{
			if (!m_log) return;
			wchar_t line[160]{};
			swprintf_s(line, format, args...);
			m_log(line);
		}

		static bool IsFolderWindow(HWND window)
		{
			wchar_t name[32]{};
			return window && GetClassNameW(window, name, ARRAYSIZE(name)) && !wcscmp(name, L"CabinetWClass");
		}

		static void CALLBACK LayoutChanged(HWINEVENTHOOK, DWORD event, HWND target, LONG object, LONG, DWORD, DWORD)
		{
			if (!target || object == OBJID_CURSOR || object == OBJID_CARET) return;
			const HWND root = GetAncestor(target, GA_ROOT);
			switch (event)
			{
			// A drag or a resize runs a modal loop between these two events. The shown value is relative to the
			// frame, so it stays right while the window moves. The window is read again once it settles.
			case EVENT_SYSTEM_MOVESIZESTART:
				if (IsFolderWindow(root)) { s_moving = root; s_movingSince = GetTickCount64(); }
				return;
			case EVENT_SYSTEM_MOVESIZEEND:
				if (s_moving) { s_moving = nullptr; s_dirty = true; }
				return;
			case EVENT_OBJECT_SHOW: case EVENT_OBJECT_HIDE: case EVENT_OBJECT_REORDER:
			case EVENT_OBJECT_LOCATIONCHANGE: case EVENT_OBJECT_NAMECHANGE:
			case EVENT_OBJECT_VALUECHANGE: case EVENT_OBJECT_STATECHANGE:
				break;
			default: return;
			}
			if (IsFolderWindow(root)) s_dirty = true;
		}

		bool Connect()
		{
			if (m_view) return true;
			if (!m_initialized)
			{
				if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return false;
				m_initialized = true;
			}
			ComPtr<IUIAutomation> ui;
			if (FAILED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ui)))) return false;
			ComPtr<IUIAutomation2> limits;
			if (SUCCEEDED(ui.As(&limits))) { limits->put_ConnectionTimeout(300); limits->put_TransactionTimeout(300); }
			ComPtr<IUIAutomationCacheRequest> cache;
			ComPtr<IUIAutomationTreeWalker> walker;
			if (FAILED(ui->CreateCacheRequest(&cache)) || FAILED(ui->get_ControlViewWalker(&walker))) return false;
			for (auto id : { UIA_ControlTypePropertyId, UIA_BoundingRectanglePropertyId, UIA_IsOffscreenPropertyId })
				if (FAILED(cache->AddProperty(id))) return false;
			auto condition = [&](PROPERTYID id, const wchar_t* value, IUIAutomationCondition** result) {
				VARIANT v{};
				v.vt = VT_BSTR;
				v.bstrVal = SysAllocString(value);
				if (!v.bstrVal) return E_OUTOFMEMORY;
				const HRESULT hr = ui->CreatePropertyCondition(id, v, result);
				SysFreeString(v.bstrVal);
				return hr;
			};
			ComPtr<IUIAutomationCondition> byId, byClass, classicList, modernView, view;
			if (FAILED(condition(UIA_AutomationIdPropertyId, L"ItemsView", &byId)) ||
				FAILED(condition(UIA_ClassNamePropertyId, L"UIItemsView", &byClass)) ||
				FAILED(condition(UIA_ClassNamePropertyId, L"SysListView32", &classicList)) ||
				FAILED(ui->CreateOrCondition(byId.Get(), byClass.Get(), &modernView)) ||
				FAILED(ui->CreateOrCondition(modernView.Get(), classicList.Get(), &view))) return false;
			m_ui = ui; m_walker = walker; m_cache = cache; m_view = view;
			return true;
		}

		// Done: every row was seen. Full: the rows reach the bottom, so there is no empty space. Unknown: the tree
		// could not be read completely this time (a timeout, the budget, items not laid out yet).
		enum class Walk { Done, Full, Unknown };
		struct Scan { RECT pane; LONG last; LONG groupHeight; unsigned budget{ 180 }; ULONGLONG deadline; };
		Walk ScanRows(IUIAutomationElement* parent, Scan& scan, unsigned depth)
		{
			if (depth > 8) return Walk::Unknown;
			ComPtr<IUIAutomationElement> item;
			HRESULT hr = m_walker->GetFirstChildElementBuildCache(parent, m_cache.Get(), &item);
			while (SUCCEEDED(hr) && item)
			{
				if (!scan.budget || GetTickCount64() > scan.deadline) return Walk::Unknown;
				--scan.budget;
				RECT box{};
				BOOL off{};
				CONTROLTYPEID type{};
				if (FAILED(item->get_CachedControlType(&type)) || FAILED(item->get_CachedIsOffscreen(&off))) return Walk::Unknown;
				if (!off)
				{
					if (FAILED(item->get_CachedBoundingRectangle(&box))) return Walk::Unknown;
					RECT visible{};
					if (IntersectRect(&visible, &box, &scan.pane))
					{
						if (type == UIA_GroupControlTypeId || type == UIA_PaneControlTypeId ||
							type == UIA_ListControlTypeId || type == UIA_DataGridControlTypeId)
						{
							if (type == UIA_GroupControlTypeId)
								scan.last = (std::max)(scan.last, (std::min)(visible.bottom, visible.top + scan.groupHeight));
							const Walk inner = ScanRows(item.Get(), scan, depth + 1);
							if (inner != Walk::Done) return inner;
						}
						else if (type != UIA_ScrollBarControlTypeId)
						{
							// Includes file items, column headers and empty/search status messages.
							scan.last = (std::max)(scan.last, visible.bottom);
						}
						if (scan.last >= scan.pane.bottom) return Walk::Full;
					}
					else if (IsRectEmpty(&box) && (type == UIA_ListItemControlTypeId || type == UIA_DataItemControlTypeId))
						return Walk::Unknown;
				}
				ComPtr<IUIAutomationElement> next;
				hr = m_walker->GetNextSiblingElementBuildCache(item.Get(), m_cache.Get(), &next);
				item = std::move(next);
			}
			return SUCCEEDED(hr) ? Walk::Done : Walk::Unknown;
		}

		struct ViewMatch { HWND window{}; bool ambiguous{}; };
		static BOOL CALLBACK FindVisibleView(HWND child, LPARAM out)
		{
			wchar_t name[48]{};
			if (IsWindowVisible(child) && GetClassNameW(child, name, ARRAYSIZE(name)) && !wcscmp(name, L"SHELLDLL_DefView"))
			{
				auto& match = *reinterpret_cast<ViewMatch*>(out);
				if (match.window) { match.ambiguous = true; return FALSE; }
				match.window = child;
			}
			return TRUE;
		}

		// The empty space of one folder window, 0 when it has none, or nothing when the view could not be read.
		std::optional<std::uint32_t> Measure(HWND window)
		{
			ViewMatch match{};
			EnumChildWindows(window, FindVisibleView, reinterpret_cast<LPARAM>(&match));
			if (!match.window || match.ambiguous) return 0u;
			const HWND defView = match.window;
			ComPtr<IUIAutomationElement> root, list;
			if (FAILED(m_ui->ElementFromHandle(defView, &root)) || !root ||
				FAILED(root->FindFirst(TreeScope_Subtree, m_view.Get(), &list))) return std::nullopt;
			if (!list) return 0u;
			BOOL offscreen{};
			if (FAILED(list->get_CurrentIsOffscreen(&offscreen))) return std::nullopt;
			if (offscreen) return 0u;
			RECT frame{}, pane{}, viewRect{}, clipped{};
			if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))) ||
				IsRectEmpty(&frame) || !GetWindowRect(defView, &viewRect) ||
				FAILED(list->get_CurrentBoundingRectangle(&pane)) || !IntersectRect(&clipped, &pane, &viewRect) ||
				!IntersectRect(&pane, &clipped, &frame)) return std::nullopt;
			ComPtr<IUIAutomationScrollPattern> scroll;
			if (SUCCEEDED(list->GetCurrentPatternAs(UIA_ScrollPatternId, IID_PPV_ARGS(&scroll))) && scroll)
			{
				BOOL vertical{};
				if (FAILED(scroll->get_CurrentVerticallyScrollable(&vertical))) return std::nullopt;
				if (vertical) return 0u;
			}
			const int dpi = static_cast<int>(GetDpiForWindow(window));
			const int pad = MulDiv(8, dpi, 96), header = MulDiv(32, dpi, 96);
			Scan scan{ pane, (std::min)(pane.top + header, pane.bottom), header, 180, GetTickCount64() + 200 };
			const Walk walk = ScanRows(list.Get(), scan, 0);
			if (walk == Walk::Unknown) return std::nullopt;
			if (walk == Walk::Full) return 0u;
			const float w = static_cast<float>(frame.right - frame.left), h = static_cast<float>(frame.bottom - frame.top);
			return OpenGlass::ExplorerPane::Pack({ (pane.left - frame.left) / w, (scan.last + pad - frame.top) / h,
				(pane.right - frame.left) / w, (pane.bottom - frame.top) / h });
		}

		static BOOL CALLBACK Collect(HWND window, LPARAM context)
		{
			DWORD pid{};
			GetWindowThreadProcessId(window, &pid);
			if (pid == GetCurrentProcessId() && IsWindowVisible(window) && !IsIconic(window) && IsFolderWindow(window))
				reinterpret_cast<std::vector<HWND>*>(context)->push_back(window);
			return TRUE;
		}

		static void Publish(HWND window, std::uint32_t value)
		{
			DWORD pid{};
			GetWindowThreadProcessId(window, &pid);
			if (pid != GetCurrentProcessId()) return;
			const auto prop = OpenGlass::ExplorerPane::Property;
			const auto next = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(value));
			if (GetPropW(window, prop) == next) return;
			if (value) { if (!SetPropW(window, prop, next)) return; }
			else RemovePropW(window, prop);
			RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
		}

		// Returns true while a window waits for a confirming reading or for a retry after a failed one.
		bool Update(bool enabled)
		{
			std::vector<HWND> windows;
			if (enabled && Connect()) EnumWindows(Collect, reinterpret_cast<LPARAM>(&windows));
			for (const Pane& old : m_panes)
				if (std::find(windows.begin(), windows.end(), old.window) == windows.end()) Publish(old.window, 0);
			std::vector<Pane> panes;
			bool again = false;
			if (!windows.empty())
			{
				const auto previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
				for (HWND window : windows)
				{
					Pane pane{ window };
					const auto known = std::find_if(m_panes.begin(), m_panes.end(), [window](const Pane& p) { return p.window == window; });
					if (known != m_panes.end()) pane = *known;
					const auto reading = Measure(window);
					if (reading) pane.misses = 0;
					else if (++pane.misses < MaxMisses)
					{
						Note(L"pane %p: reading failed (%u), keeping %08x", window, pane.misses, pane.shown);
						again = true;
						panes.push_back(pane);
						continue;
					}
					const std::uint32_t value = reading.value_or(0u);
					if (value == pane.shown) pane.candidate = value;
					else if (value == pane.candidate)
					{
						Note(L"pane %p: %08x -> %08x", window, pane.shown, value);
						Publish(window, value);
						pane.shown = value;
					}
					else
					{
						pane.candidate = value;
						again = true;
					}
					panes.push_back(pane);
				}
				if (previous) SetThreadDpiAwarenessContext(previous);
			}
			m_panes.swap(panes);
			return again;
		}

		void Unhook()
		{
			if (m_events) UnhookWinEvent(m_events);
			if (m_moves) UnhookWinEvent(m_moves);
			m_events = m_moves = nullptr;
			s_moving = nullptr;
		}
	public:
		void SetLog(void (*log)(const wchar_t*)) { m_log = log; }

		// Keep the existing one-second registry cadence. Coalesce layout events; a slow rescan
		// also covers providers that omit a scroll or tab-change notification.
		void WaitForLayoutChanges(bool enabled)
		{
			if (enabled != m_enabled)
			{
				m_enabled = enabled;
				if (enabled)
				{
					m_events = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_VALUECHANGE, nullptr, LayoutChanged,
						GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
					m_moves = SetWinEventHook(EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND, nullptr, LayoutChanged,
						GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
					s_dirty = true; m_nextScan = 0; m_fallback = 0;
				}
				else
				{
					Unhook();
					Update(false);
				}
			}
			if (!enabled) { Sleep(1000); return; }
			const auto end = GetTickCount64() + 1000;
			for (;;)
			{
				auto now = GetTickCount64();
				// No readings while a folder window is dragged or resized. Its layout is read again 150 ms after it settles.
				const bool moving = s_moving && IsWindow(s_moving) && now - s_movingSince < MoveTimeout;
				if (moving != m_wasMoving)
				{
					m_wasMoving = moving;
					Note(moving ? L"pane: window move started, readings paused" : L"pane: window settled");
					if (!moving) { s_dirty = true; m_nextScan = now + 150; }
				}
				if (!moving && ((s_dirty && now >= m_nextScan) || now >= m_fallback))
				{
					s_dirty = false;
					if (Update(true)) s_dirty = true;
					now = GetTickCount64();
					m_nextScan = now + 150;
					m_fallback = now + (m_events ? 4000 : 1000);
				}
				if (now >= end) return;
				auto wake = moving ? end : (std::min)(end, m_fallback);
				if (!moving && s_dirty) wake = (std::min)(wake, m_nextScan);
				MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(wake - now), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
				MSG msg{};
				for (int i = 0; i < 64 && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE); ++i)
					DispatchMessageW(&msg);
			}
		}
		~Observer()
		{
			Unhook();
			for (const Pane& pane : m_panes) Publish(pane.window, 0);
			m_view.Reset(); m_cache.Reset(); m_walker.Reset(); m_ui.Reset();
			if (m_initialized) CoUninitialize();
		}
	};
}

#pragma comment(lib, "oleaut32.lib")
