#include "pch.h"
#include "ModernTheme.hpp"
#include <wx/fontenum.h>
#include <wx/dcbuffer.h>
#include <wx/checklst.h>

namespace OpenGlass::ModernTheme
{
	namespace
	{
		Palette CreatePalette()
		{
			if (wxSystemSettings::GetAppearance().IsDark())
			{
				return {
					true,
					wxColour(11, 20, 34), wxColour(18, 32, 53), wxColour(36, 58, 89),
					wxColour(232, 238, 246), wxColour(150, 168, 192),
					wxColour(40, 170, 255), wxColour(30, 110, 245), *wxWHITE,
					wxColour(44, 64, 94), wxColour(27, 45, 72)
				};
			}
			return {
				false,
				wxColour(238, 243, 249), wxColour(255, 255, 255), wxColour(212, 222, 235),
				wxColour(26, 31, 41), wxColour(92, 104, 120),
				wxColour(0, 120, 215), wxColour(0, 90, 200), *wxWHITE,
				wxColour(200, 212, 228), wxColour(225, 233, 243)
			};
		}


		// A vertical box sizer that leaves room for a header and padding; the page paints the card behind it.
		class CardSizer : public wxBoxSizer
		{
		public:
			CardSizer(wxWindow* page, const wxString& title) : wxBoxSizer(wxVERTICAL), m_page{ page }, m_title{ title } {}

			const wxString& GetTitle() const noexcept { return m_title; }
			int Header() const { return m_page->FromDIP(40); }
			int Padding() const { return m_page->FromDIP(12); }

			wxSize CalcMin() override
			{
				return wxBoxSizer::CalcMin() + wxSize(Padding() * 2, Header() + Padding());
			}

			void RepositionChildren(const wxSize& minSize) override
			{
				const wxPoint position = m_position;
				const wxSize size = m_size;
				m_position += wxPoint(Padding(), Header());
				m_size -= wxSize(Padding() * 2, Header() + Padding());
				wxBoxSizer::RepositionChildren(minSize - wxSize(Padding() * 2, Header() + Padding()));
				m_position = position;
				m_size = size;
			}

		private:
			wxWindow* m_page;
			wxString m_title;
		};

		void PaintCard(wxGraphicsContext& gc, const wxWindow& page, const CardSizer& card, const wxRect& rect)
		{
			const auto& palette = Get();
			gc.SetPen(wxPen(palette.border, page.FromDIP(1)));
			gc.SetBrush(wxBrush(palette.card));
			gc.DrawRoundedRectangle(rect.x + 0.5, rect.y + 0.5, rect.width - 1.0, rect.height - 1.0, page.FromDIP(8));

			double x = rect.x + page.FromDIP(16);
			const double centerY = rect.y + page.FromDIP(21);
			const wxString icon = IconFor(card.GetTitle());
			double width{}, height{};
			if (!icon.empty())
			{
				gc.SetFont(IconFont(&page, 13), palette.accent);
				gc.GetTextExtent(icon, &width, &height);
				gc.DrawText(icon, x, centerY - height / 2);
				x += page.FromDIP(30);
			}
			wxFont titleFont = page.GetFont();
			titleFont.SetFractionalPointSize(titleFont.GetFractionalPointSize() * 1.2);
			titleFont.SetWeight(wxFONTWEIGHT_SEMIBOLD);
			gc.SetFont(titleFont, palette.text);
			gc.GetTextExtent(card.GetTitle(), &width, &height);
			gc.DrawText(card.GetTitle(), x, centerY - height / 2);
		}

		// Sizer positions do not follow scrolling, so measure the offset from any laid out window.
		wxPoint ScrollOffset(wxWindow* page)
		{
			for (auto* child : page->GetChildren())
			{
				if (!child->IsShown())
				{
					continue;
				}
				if (const auto* item = page->GetSizer()->GetItem(child, true))
				{
					return child->GetPosition() - item->GetPosition();
				}
			}
			return {};
		}
		// Lists keep their own black dark-mode field unless told otherwise; all of them sit inside cards.
		void ColorLists(wxWindow* window, const wxColour& color)
		{
			for (auto* child : window->GetChildren())
			{
				if (wxDynamicCast(child, wxListCtrl) || wxDynamicCast(child, wxListBox))
				{
					child->SetBackgroundColour(color);
				}
				// Check list items are owner drawn with their own background.
				if (auto* checkList = wxDynamicCast(child, wxCheckListBox))
				{
					for (unsigned int index = 0; index < checkList->GetCount(); ++index)
					{
						checkList->GetItem(index)->SetBackgroundColour(color);
					}
				}
				ColorLists(child, color);
			}
		}

		// Drop-downs keep the native list but draw their closed field as a rounded box with a chevron.
		void StyleChoice(wxChoice* choice)
		{
			const auto hwnd = static_cast<HWND>(choice->GetHWND());
			// Taller selection field, like the concept's roomy inputs.
			SendMessageW(hwnd, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), choice->FromDIP(24));
			choice->InvalidateBestSize();
			// The wider padding and chevron need room beyond the native best width.
			choice->SetMinSize(wxSize(choice->GetBestSize().x + choice->FromDIP(24), choice->FromDIP(32)));

			auto hover = std::make_shared<bool>(false);
			choice->Bind(wxEVT_ENTER_WINDOW, [choice, hover](wxMouseEvent& event) { *hover = true; choice->Refresh(false); event.Skip(); });
			choice->Bind(wxEVT_LEAVE_WINDOW, [choice, hover](wxMouseEvent& event) { *hover = false; choice->Refresh(false); event.Skip(); });
			choice->Bind(wxEVT_SET_FOCUS, [choice](wxFocusEvent& event) { choice->Refresh(false); event.Skip(); });
			choice->Bind(wxEVT_KILL_FOCUS, [choice](wxFocusEvent& event) { choice->Refresh(false); event.Skip(); });
			choice->Bind(wxEVT_CHOICE, [choice](wxCommandEvent& event) { choice->Refresh(false); event.Skip(); });
			choice->Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent&) {});
			// Windows repaints only the newly exposed strip of a resized control, which would leave the old box behind.
			choice->Bind(wxEVT_SIZE, [choice](wxSizeEvent& event) { choice->Refresh(false); event.Skip(); });
			choice->Bind(wxEVT_PAINT, [choice, hover](wxPaintEvent&)
			{
				wxAutoBufferedPaintDC dc(choice);
				const auto& palette = Get();
				const wxSize size = choice->GetClientSize();
				dc.SetBackground(wxBrush(choice->GetParent()->GetBackgroundColour()));
				dc.Clear();
				std::unique_ptr<wxGraphicsContext> gc{ wxGraphicsContext::Create(dc) };
				if (!gc)
				{
					return;
				}
				const bool enabled = choice->IsEnabled();
				const bool focused = choice->HasFocus();
				const wxColour field = palette.dark ? (*hover && enabled ? wxColour(32, 52, 82) : wxColour(24, 41, 66)) : (*hover && enabled ? wxColour(246, 249, 253) : *wxWHITE);
				const wxColour outline = focused ? palette.accent : *hover && enabled ? palette.subtext : palette.border;
				gc->SetPen(wxPen(outline, choice->FromDIP(1)));
				gc->SetBrush(wxBrush(field));
				gc->DrawRoundedRectangle(0.5, 0.5, size.x - 1.0, size.y - 1.0, choice->FromDIP(6));

				const wxColour foreground = enabled ? palette.text : palette.subtext;
				const double padding = choice->FromDIP(12);
				const wxString chevron(wxUniChar(0xE70D));
				double chevronWidth{}, chevronHeight{};
				gc->SetFont(IconFont(choice, 8), enabled ? palette.subtext : palette.border);
				gc->GetTextExtent(chevron, &chevronWidth, &chevronHeight);
				const double chevronX = size.x - padding - chevronWidth;
				gc->DrawText(chevron, chevronX, (size.y - chevronHeight) / 2);

				gc->SetFont(choice->GetFont(), foreground);
				gc->Clip(padding, 0, std::max(chevronX - padding - choice->FromDIP(6), 0.0), size.y);
				double textWidth{}, textHeight{};
				const wxString text = choice->GetStringSelection();
				gc->GetTextExtent(text, &textWidth, &textHeight);
				gc->DrawText(text, padding, (size.y - textHeight) / 2);
				gc->ResetClip();
			});
		}

		// Number boxes keep the native edit for typing but lose its 3D frame and the old up-down arrows.
		// Their parent draws a rounded field around each, like a drop-down's, with two chevrons where the
		// arrows were; a click on a chevron steps the value. Styled boxes carry this name.
		constexpr wchar_t c_numberBoxName[] = L"ModernNumberBox";

		HWND UpDownOf(const wxSpinCtrl* spin)
		{
			return static_cast<HWND>(spin->GetHWND());
		}

		HWND EditOf(const wxSpinCtrl* spin)
		{
			return reinterpret_cast<HWND>(SendMessageW(UpDownOf(spin), UDM_GETBUDDY, 0, 0));
		}

		wxRect ParentRectOf(const wxSpinCtrl* spin, HWND hwnd)
		{
			RECT screen{};
			GetWindowRect(hwnd, &screen);
			return wxRect(spin->GetParent()->ScreenToClient(wxPoint(screen.left, screen.top)), wxSize(screen.right - screen.left, screen.bottom - screen.top));
		}

		wxColour NumberFieldColour(bool hover)
		{
			const auto& palette = Get();
			return palette.dark ? (hover ? wxColour(32, 52, 82) : wxColour(24, 41, 66)) : (hover ? wxColour(246, 249, 253) : *wxWHITE);
		}

		// Called by whatever paints the parent, after its own background.
		void DrawNumberBoxes(wxGraphicsContext& gc, wxWindow& parent)
		{
			const auto& palette = Get();
			for (auto* child : parent.GetChildren())
			{
				auto* spin = wxDynamicCast(child, wxSpinCtrl);
				if (!spin || spin->GetName() != c_numberBoxName || !spin->IsShown())
				{
					continue;
				}
				const wxRect arrows = ParentRectOf(spin, UpDownOf(spin));
				const wxRect field = ParentRectOf(spin, EditOf(spin)).Union(arrows).Inflate(spin->FromDIP(4), spin->FromDIP(4));
				const bool enabled = spin->IsEnabled();
				const bool focused = GetFocus() == EditOf(spin);
				gc.SetPen(wxPen(focused ? palette.accent : palette.border, spin->FromDIP(1)));
				gc.SetBrush(wxBrush(NumberFieldColour(false)));
				gc.DrawRoundedRectangle(field.x + 0.5, field.y + 0.5, field.width - 1.0, field.height - 1.0, spin->FromDIP(6));

				const wxColour chevronColour = enabled ? palette.subtext : palette.border;
				const wxFont font = IconFont(spin, 6.5);
				for (const bool up : { true, false })
				{
					const wxString chevron(wxUniChar(up ? 0xE70E : 0xE70D));
					double width{}, height{};
					gc.SetFont(font, chevronColour);
					gc.GetTextExtent(chevron, &width, &height);
					const double centerY = arrows.y + arrows.height * (up ? 0.3 : 0.7);
					gc.DrawText(chevron, arrows.x + (arrows.width - width) / 2, centerY - height / 2);
				}
			}
		}

		void BindNumberBoxClicks(wxWindow* parent)
		{
			static std::set<wxWindow*> bound;
			if (!bound.insert(parent).second)
			{
				return;
			}
			parent->Bind(wxEVT_LEFT_DOWN, [parent](wxMouseEvent& event)
			{
				event.Skip();
				for (auto* child : parent->GetChildren())
				{
					auto* spin = wxDynamicCast(child, wxSpinCtrl);
					if (!spin || spin->GetName() != c_numberBoxName || !spin->IsShown() || !spin->IsEnabled())
					{
						continue;
					}
					const wxRect arrows = ParentRectOf(spin, UpDownOf(spin));
					if (!arrows.Contains(event.GetPosition()))
					{
						continue;
					}
					const int step = event.GetPosition().y < arrows.y + arrows.height / 2 ? 1 : -1;
					const int value = std::clamp(spin->GetValue() + step, spin->GetMin(), spin->GetMax());
					if (value != spin->GetValue())
					{
						spin->SetValue(value);
						wxSpinEvent changed(wxEVT_SPINCTRL, spin->GetId());
						changed.SetEventObject(spin);
						changed.SetPosition(value);
						spin->ProcessWindowEvent(changed);
					}
					spin->SetFocus();
					return;
				}
			});
		}

		void StyleNumberBox(wxSpinCtrl* spin)
		{
			spin->SetName(c_numberBoxName);
			ShowWindow(UpDownOf(spin), SW_HIDE);
			// wxWidgets draws a themed border itself for its default border, so it must forget it too.
			spin->SetWindowStyleFlag((spin->GetWindowStyleFlag() & ~wxBORDER_MASK) | wxBORDER_NONE);
			const HWND edit = EditOf(spin);
			SetWindowLongPtrW(edit, GWL_EXSTYLE, GetWindowLongPtrW(edit, GWL_EXSTYLE) & ~WS_EX_CLIENTEDGE);
			SetWindowLongPtrW(edit, GWL_STYLE, GetWindowLongPtrW(edit, GWL_STYLE) & ~WS_BORDER);
			SetWindowPos(edit, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
			// Even without a border style, the dark mode still outlines the edit inside its client area.
			// Clipping its outer pixel lets our field show there instead, at every size it is given.
			const auto clip = [spin]
			{
				const HWND edit = EditOf(spin);
				RECT client{};
				GetClientRect(edit, &client);
				SetWindowRgn(edit, CreateRectRgn(1, 1, std::max(client.right - 1, 1L), std::max(client.bottom - 1, 1L)), TRUE);
			};
			clip();
			spin->Bind(wxEVT_SIZE, [clip](wxSizeEvent& event) { event.Skip(); clip(); });
			spin->SetBackgroundColour(NumberFieldColour(false));
			spin->SetForegroundColour(Get().text);

			wxWindow* parent = spin->GetParent();
			const auto repaint = [spin, parent]
			{
				parent->RefreshRect(ParentRectOf(spin, EditOf(spin)).Union(ParentRectOf(spin, UpDownOf(spin))).Inflate(spin->FromDIP(6)), false);
			};
			spin->Bind(wxEVT_SET_FOCUS, [repaint](wxFocusEvent& event) { repaint(); event.Skip(); });
			spin->Bind(wxEVT_KILL_FOCUS, [repaint](wxFocusEvent& event) { repaint(); event.Skip(); });
			BindNumberBoxClicks(parent);
		}

		void StyleNumberBoxes(wxWindow* window, std::set<wxWindow*>& parents)
		{
			for (auto* child : window->GetChildren())
			{
				if (auto* spin = wxDynamicCast(child, wxSpinCtrl))
				{
					StyleNumberBox(spin);
					parents.insert(window);
				}
				StyleNumberBoxes(child, parents);
			}
		}

		void StyleChoices(wxWindow* window)
		{
			for (auto* child : window->GetChildren())
			{
				if (auto* choice = wxDynamicCast(child, wxChoice))
				{
					StyleChoice(choice);
				}
				StyleChoices(child);
			}
		}

		// Row labels read cleaner without trailing colons, like the concept.
		void StripColons(wxWindow* window)
		{
			for (auto* child : window->GetChildren())
			{
				if (auto* text = wxDynamicCast(child, wxStaticText))
				{
					const wxString label = text->GetLabel();
					if (label.EndsWith(L":") && !label.Contains(L"\n"))
					{
						text->SetLabel(label.Left(label.length() - 1));
					}
				}
				StripColons(child);
			}
		}

		// Native inputs draw their own dark or light field; a custom background breaks that.
		bool IsInputControl(wxWindow* window)
		{
			return wxDynamicCast(window, wxChoice) || wxDynamicCast(window, wxComboBox)
				|| wxDynamicCast(window, wxSpinCtrl) || wxDynamicCast(window, wxSpinCtrlDouble)
				|| wxDynamicCast(window, wxTextCtrl)
				|| wxDynamicCast(window, wxAnyButton) || wxDynamicCast(window, wxPickerBase);
		}

		void Recolor(wxWindow* window, const wxColour& from, const wxColour& to)
		{
			for (auto* child : window->GetChildren())
			{
				if (child->IsTopLevel())
				{
					continue;
				}
				if (child->UseBgCol() && child->GetBackgroundColour() == from && !IsInputControl(child))
				{
					child->SetBackgroundColour(to);
				}
				Recolor(child, from, to);
			}
		}

		bool TakesSurfaceColor(wxWindow* window)
		{
			if (IsInputControl(window))
			{
				return false;
			}
			return window->UseBgCol()
				|| wxDynamicCast(window, wxStaticText) || wxDynamicCast(window, wxCheckBox)
				|| wxDynamicCast(window, wxRadioBox) || wxDynamicCast(window, wxRadioButton)
				|| wxDynamicCast(window, wxSlider) || wxDynamicCast(window, wxPanel)
				|| wxDynamicCast(window, wxCollapsiblePane) || wxDynamicCast(window, wxHyperlinkCtrl);
		}

		// Windows outside every card sit on the page color.
		void CollectLooseWindows(wxWindow* page, wxSizer* sizer, bool insideCard, std::vector<wxWindow*>& loose, std::vector<CardSizer*>& cards)
		{
			for (auto* item : sizer->GetChildren())
			{
				if (item->IsWindow())
				{
					if (!insideCard)
					{
						loose.push_back(item->GetWindow());
					}
				}
				else if (item->IsSizer())
				{
					auto* card = dynamic_cast<CardSizer*>(item->GetSizer());
					if (card)
					{
						cards.push_back(card);
						if (!insideCard)
						{
							// Room between cards, like the concept's floating surfaces.
							item->SetBorder(page->FromDIP(6));
						}
					}
					CollectLooseWindows(page, item->GetSizer(), insideCard || card, loose, cards);
				}
			}
		}
	}

	const Palette& Get()
	{
		static const Palette palette = CreatePalette();
		return palette;
	}

	wxFont IconFont([[maybe_unused]] const wxWindow* window, double pointSize)
	{
		static const wxString face = wxFontEnumerator::IsValidFacename(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
		wxFont font(wxFontInfo(pointSize).FaceName(face));
		return font;
	}

	wxString IconFor(const wxString& title)
	{
		static const std::unordered_map<std::wstring, wchar_t> icons
		{
			// Tabs
			{ L"System", 0xE713 }, { L"Theme", 0xE771 }, { L"Appearance", 0xE7F4 }, { L"Glass colors", 0xE790 },
			{ L"Liquid Glass", 0xEB42 }, { L"Taskbar", 0xE7C4 }, { L"Preset library", 0xE8F1 }, { L"Diagnostics", 0xE9D9 },
			// Cards
			{ L"Global", 0xE774 }, { L"Transparency status", 0xE7B3 }, { L"Symbols", 0xE943 },
			{ L"Symbol download status", 0xE896 }, { L"WER crash dumps", 0xE7BA }, { L"Reflection", 0xE706 },
			{ L"Material", 0xE8B9 }, { L"General", 0xE713 }, { L"Window", 0xE922 }, { L"Caption", 0xE8D2 },
			{ L"Accent", 0xE790 }, { L"Glass", 0xE706 }, { L"Installed presets", 0xE8F1 }, { L"Color presets", 0xE790 }, { L"Glass tint", 0xEB42 },
			{ L"Refraction", 0xEB42 }, { L"Blur", 0xE706 }, { L"Color", 0xE790 }, { L"Rim light", 0xE706 },
			{ L"Floating taskbar", 0xE7C4 }, { L"Pills", 0xEB42 }
		};
		const auto found = icons.find(title.ToStdWstring());
		return found == icons.end() ? wxString() : wxString(found->second);
	}

	wxBoxSizer* MakeCard(wxWindow* parent, const wxString& title)
	{
		return new CardSizer(parent, title);
	}

	void SkinPage(wxWindow* page)
	{
		const auto& palette = Get();
		const wxColour original = page->GetBackgroundColour();
		Recolor(page, original, palette.card);
		page->SetBackgroundColour(palette.card);

		std::vector<wxWindow*> loose;
		auto cards = std::make_shared<std::vector<CardSizer*>>();
		if (page->GetSizer())
		{
			CollectLooseWindows(page, page->GetSizer(), false, loose, *cards);
		}
		StripColons(page);
		StyleChoices(page);
		std::set<wxWindow*> numberParents;
		StyleNumberBoxes(page, numberParents);
		for (auto* parent : numberParents)
		{
			if (parent == page)
			{
				continue;	// the page's own paint below draws them
			}
			// A panel nested in the page (a collapsible section) draws its own boxes over its plain color.
			parent->SetBackgroundStyle(wxBG_STYLE_PAINT);
			parent->Bind(wxEVT_PAINT, [parent](wxPaintEvent&)
			{
				wxAutoBufferedPaintDC dc(parent);
				dc.SetBackground(wxBrush(parent->GetBackgroundColour()));
				dc.Clear();
				if (std::unique_ptr<wxGraphicsContext> gc{ wxGraphicsContext::Create(dc) })
				{
					DrawNumberBoxes(*gc, *parent);
				}
			});
			parent->Bind(wxEVT_SIZE, [parent](wxSizeEvent& event) { parent->Refresh(false); event.Skip(); });
		}
		ColorLists(page, palette.card);
		for (auto* window : loose)
		{
			if (TakesSurfaceColor(window))
			{
				window->SetBackgroundColour(palette.page);
				Recolor(window, palette.card, palette.page);
			}
		}

		// The page shows through between cards; controls inside a card inherit the card color.
		page->SetBackgroundStyle(wxBG_STYLE_PAINT);
		page->Bind(wxEVT_PAINT, [page, cards](wxPaintEvent&)
		{
			wxAutoBufferedPaintDC dc(page);
			dc.SetBackground(wxBrush(Get().page));
			dc.Clear();
			std::unique_ptr<wxGraphicsContext> gc{ wxGraphicsContext::Create(dc) };
			if (!gc || !page->GetSizer())
			{
				return;
			}
			const wxPoint offset = ScrollOffset(page);
			for (const auto* card : *cards)
			{
				if (card->GetSize().x > 0 && card->GetSize().y > 0)
				{
					PaintCard(*gc, *page, *card, wxRect(card->GetPosition() + offset, card->GetSize()));
				}
			}
			DrawNumberBoxes(*gc, *page);
		});
		// The cards move and stretch with the window; a partial repaint would leave pieces of them behind.
		page->Bind(wxEVT_SIZE, [page](wxSizeEvent& event) { page->Refresh(false); event.Skip(); });
		page->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED, [page](wxCollapsiblePaneEvent& event) { page->Refresh(); event.Skip(); });
		page->Refresh();
	}

	void StyleAccentButton(wxButton* button)
	{
		const auto& palette = Get();
		const bool enabled = button->IsEnabled();
		button->SetBackgroundColour(enabled ? palette.accent : palette.hover);
		button->SetForegroundColour(enabled ? palette.accentText : palette.subtext);
		button->Refresh();
	}

	TabBar::TabBar(wxWindow* parent, wxBookCtrlBase* book) : m_book{ book }
	{
		SetBackgroundStyle(wxBG_STYLE_PAINT);
		Create(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
		SetBackgroundColour(Get().page);
		Bind(wxEVT_PAINT, &TabBar::OnPaint, this);
		Bind(wxEVT_MOTION, [this](wxMouseEvent& event)
		{
			const int hover = HitTest(event.GetPosition());
			if (hover != m_hover)
			{
				m_hover = hover;
				SetCursor(hover >= 0 ? wxCursor(wxCURSOR_HAND) : wxNullCursor);
				Refresh(false);
			}
		});
		Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&)
		{
			m_hover = -1;
			Refresh(false);
		});
		Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event)
		{
			const int tab = HitTest(event.GetPosition());
			if (tab >= 0 && tab != m_book->GetSelection())
			{
				m_book->SetSelection(tab);
			}
		});
		m_book->Bind(wxEVT_BOOKCTRL_PAGE_CHANGED, [this](wxBookCtrlEvent& event) { Refresh(false); event.Skip(); });
		Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { Refresh(false); event.Skip(); });
	}

	// Wide enough for every tab, measured the way OnPaint lays them out (bold, as when selected).
	wxSize TabBar::DoGetBestClientSize() const
	{
		wxBitmap bitmap(1, 1);
		wxMemoryDC dc(bitmap);
		std::unique_ptr<wxGraphicsContext> gc{ wxGraphicsContext::Create(dc) };
		if (!gc)
		{
			return FromDIP(wxSize(480, 48));
		}
		const wxFont iconFont = IconFont(const_cast<TabBar*>(this), 12);
		wxFont textFont = GetFont();
		textFont.SetFractionalPointSize(textFont.GetFractionalPointSize() * 1.1);
		textFont.SetWeight(wxFONTWEIGHT_SEMIBOLD);
		double x = FromDIP(4);
		for (size_t index = 0; index < m_book->GetPageCount(); ++index)
		{
			const wxString label = m_book->GetPageText(index);
			const wxString icon = IconFor(label);
			double iconWidth{}, height{}, textWidth{};
			if (!icon.empty())
			{
				gc->SetFont(iconFont, Get().text);
				gc->GetTextExtent(icon, &iconWidth, &height);
			}
			gc->SetFont(textFont, Get().text);
			gc->GetTextExtent(label, &textWidth, &height);
			x += (icon.empty() ? 0 : iconWidth + FromDIP(8)) + textWidth + FromDIP(14) * 2 + FromDIP(4);
		}
		return wxSize(static_cast<int>(std::ceil(x)), FromDIP(48));
	}

	int TabBar::HitTest(const wxPoint& point) const
	{
		for (size_t index = 0; index < m_tabs.size(); ++index)
		{
			if (m_tabs[index].Contains(point))
			{
				return static_cast<int>(index);
			}
		}
		return -1;
	}

	void TabBar::OnPaint(wxPaintEvent&)
	{
		wxAutoBufferedPaintDC dc(this);
		const auto& palette = Get();
		const wxSize size = GetClientSize();
		dc.SetBackground(wxBrush(palette.page));
		dc.Clear();

		std::unique_ptr<wxGraphicsContext> gc{ wxGraphicsContext::Create(dc) };
		if (!gc)
		{
			return;
		}

		const wxFont iconFont = IconFont(this, 12);
		wxFont textFont = GetFont();
		textFont.SetFractionalPointSize(textFont.GetFractionalPointSize() * 1.1);
		wxFont selectedFont = textFont;
		selectedFont.SetWeight(wxFONTWEIGHT_SEMIBOLD);

		const double padding = FromDIP(14);
		const double gap = FromDIP(8);
		const double tabTop = FromDIP(4);
		const double tabHeight = size.y - FromDIP(10);
		const int selection = m_book->GetSelection();

		m_tabs.clear();
		double x = FromDIP(4);
		for (size_t index = 0; index < m_book->GetPageCount(); ++index)
		{
			const bool selected = static_cast<int>(index) == selection;
			const wxString label = m_book->GetPageText(index);
			const wxString icon = IconFor(label);

			double iconWidth{}, iconHeight{}, textWidth{}, textHeight{};
			if (!icon.empty())
			{
				gc->SetFont(iconFont, palette.text);
				gc->GetTextExtent(icon, &iconWidth, &iconHeight);
			}
			gc->SetFont(selected ? selectedFont : textFont, palette.text);
			gc->GetTextExtent(label, &textWidth, &textHeight);

			const double contentWidth = (icon.empty() ? 0 : iconWidth + gap) + textWidth;
			const double width = contentWidth + padding * 2;
			m_tabs.emplace_back(static_cast<int>(x), static_cast<int>(tabTop), static_cast<int>(width), static_cast<int>(tabHeight));

			if (selected || static_cast<int>(index) == m_hover)
			{
				gc->SetPen(*wxTRANSPARENT_PEN);
				gc->SetBrush(wxBrush(selected ? palette.card : palette.hover));
				gc->DrawRoundedRectangle(x, tabTop, width, tabHeight, FromDIP(6));
			}

			double cursor = x + padding;
			const double centerY = tabTop + tabHeight / 2;
			const wxColour foreground = selected ? palette.text : palette.subtext;
			if (!icon.empty())
			{
				gc->SetFont(iconFont, selected ? palette.accent : palette.subtext);
				gc->DrawText(icon, cursor, centerY - iconHeight / 2);
				cursor += iconWidth + gap;
			}
			gc->SetFont(selected ? selectedFont : textFont, foreground);
			gc->DrawText(label, cursor, centerY - textHeight / 2);

			if (selected)
			{
				const double lineWidth = std::max(contentWidth * 0.6, static_cast<double>(FromDIP(16)));
				const double lineX = x + (width - lineWidth) / 2;
				const double lineY = tabTop + tabHeight - FromDIP(3);
				gc->SetBrush(gc->CreateLinearGradientBrush(lineX, 0, lineX + lineWidth, 0, palette.accent, palette.accentEnd));
				gc->DrawRoundedRectangle(lineX, lineY, lineWidth, FromDIP(3), FromDIP(3) / 2.0);
			}
			x += width + FromDIP(4);
		}

		gc->SetPen(wxPen(palette.border, FromDIP(1)));
		gc->StrokeLine(0, size.y - 0.5, size.x, size.y - 0.5);
	}
}
