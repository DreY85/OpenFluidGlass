#include "pch.h"
#include "ModernTheme.hpp"
#include <wx/fontenum.h>
#include <wx/dcbuffer.h>

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
				ColorLists(child, color);
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
			{ L"Liquid Glass", 0xEB42 }, { L"Preset library", 0xE8F1 }, { L"Diagnostics", 0xE9D9 },
			// Cards
			{ L"Global", 0xE774 }, { L"Transparency status", 0xE7B3 }, { L"Symbols", 0xE943 },
			{ L"Symbol download status", 0xE896 }, { L"WER crash dumps", 0xE7BA }, { L"Reflection", 0xE706 },
			{ L"Material", 0xE8B9 }, { L"General", 0xE713 }, { L"Window", 0xE922 }, { L"Caption", 0xE8D2 },
			{ L"Accent", 0xE790 }, { L"Glass", 0xE706 }, { L"Installed presets", 0xE8F1 }, { L"Color presets", 0xE790 }, { L"Glass tint", 0xEB42 },
			{ L"Refraction", 0xEB42 }, { L"Blur", 0xE706 }, { L"Color", 0xE790 }, { L"Rim light", 0xE706 }
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
		});		page->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED, [page](wxCollapsiblePaneEvent& event) { page->Refresh(); event.Skip(); });
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
	}

	wxSize TabBar::DoGetBestClientSize() const
	{
		return FromDIP(wxSize(480, 48));
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
