#pragma once
#include "pch.h"

// Fluent-style skin over the native wxWidgets controls: rounded cards, an icon tab bar and accent colors.
// Native inputs (choices, spin boxes, checkboxes) keep their system look; only the surfaces around them change.
namespace OpenGlass::ModernTheme
{
	struct Palette
	{
		bool dark;
		wxColour page;			// window and gaps between cards
		wxColour card;			// card surface, also the background of controls inside a card
		wxColour border;
		wxColour text;
		wxColour subtext;
		wxColour accent;
		wxColour accentEnd;		// second gradient stop
		wxColour accentText;
		wxColour track;			// unfilled slider track
		wxColour hover;
	};

	// Chosen once from the app appearance, which follows the Windows light or dark mode.
	const Palette& Get();

	wxFont IconFont(const wxWindow* window, double pointSize);
	// Segoe Fluent Icons glyph for a tab or card title, or an empty string.
	wxString IconFor(const wxString& title);

	// A vertical sizer drawn as a rounded card with an icon and title. The page must go through SkinPage.
	wxBoxSizer* MakeCard(wxWindow* parent, const wxString& title);
	// Recolors a finished page: card surfaces for content inside cards, the page color for everything else.
	void SkinPage(wxWindow* page);
	void StyleAccentButton(wxButton* button);

	class TabBar : public wxControl
	{
	public:
		TabBar(wxWindow* parent, wxBookCtrlBase* book);

	protected:
		wxSize DoGetBestClientSize() const override;

	private:
		void OnPaint(wxPaintEvent& event);
		int HitTest(const wxPoint& point) const;

		wxBookCtrlBase* m_book;
		std::vector<wxRect> m_tabs;
		int m_hover{ -1 };
	};
}
