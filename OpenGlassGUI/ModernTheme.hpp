#pragma once
#include "pch.h"

// Fluent-style skin over the native wxWidgets controls: rounded cards, an icon tab bar and accent colors.
// The native controls keep their behavior, but the theme paints them: rounded buttons and fields,
// switches for check boxes, chevrons for collapsible sections, and its own message box and color dialog.
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
	// Themes every control in a window tree that is not a skinned page, such as a dialog's.
	void StyleControls(wxWindow* root);
	void StyleButton(wxButton* button);
	// The window's primary action: an accent gradient while it is enabled.
	void StyleAccentButton(wxButton* button);
	// Check boxes become switches.
	void StyleCheckBox(wxCheckBox* box);
	void StyleGauge(wxGauge* gauge);
	void StyleDialog(wxDialog* dialog);
	// Background of text, number and drop-down fields.
	wxColour FieldColour(bool hover);

	// Sets the renderer for generic controls and themes every dialog as it opens. Called once at startup.
	void Install();
	// Replaces wxMessageBox, with the same arguments and return values.
	int ShowMessage(const wxString& message, const wxString& caption = wxMessageBoxCaptionStr, long style = wxOK | wxCENTRE,
		wxWindow* parent = nullptr, int x = wxDefaultCoord, int y = wxDefaultCoord);
	// Replaces wxColourDialog: a saturation and brightness plane, a hue strip and a hex field.
	bool PickColour(wxWindow* parent, wxColour& colour, const wxString& title = wxEmptyString);

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
