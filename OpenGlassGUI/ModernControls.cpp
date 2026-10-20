#include "pch.h"
#include "ModernTheme.hpp"
#include <wx/dcbuffer.h>
#include <wx/renderer.h>
#include <wx/eventfilter.h>
#include <functional>
#include <unordered_set>

// The controls the theme draws itself: buttons, switches, progress bars, the collapse chevrons, and the
// message box and color picker that replace the system dialogs. Native controls keep their behavior;
// only their painting changes.
namespace OpenGlass::ModernTheme
{
	namespace
	{
		constexpr wchar_t c_accentButtonName[] = L"ModernAccentButton";

		// Each window is styled once. A destroyed window's address can come back for a new one.
		bool FirstTime(wxWindow* window)
		{
			static std::unordered_set<wxWindow*> styled;
			if (!styled.insert(window).second)
			{
				return false;
			}
			window->Bind(wxEVT_DESTROY, [window](wxWindowDestroyEvent& event)
			{
				if (event.GetEventObject() == window) styled.erase(window);
				event.Skip();
			});
			return true;
		}

		// Focus is drawn only while the keyboard moves it, as Windows does.
		bool ShowsFocus(wxWindow* window)
		{
			return window->HasFocus() && !(SendMessageW(static_cast<HWND>(window->GetHWND()), WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
		}

		struct Pointer
		{
			bool hover{};
			bool pressed{};
		};

		std::shared_ptr<Pointer> TrackPointer(wxWindow* window)
		{
			auto pointer = std::make_shared<Pointer>();
			const auto set = [window](bool& flag, bool value)
			{
				if (flag != value) { flag = value; window->Refresh(false); }
			};
			window->Bind(wxEVT_ENTER_WINDOW, [pointer, set](wxMouseEvent& event) { set(pointer->hover, true); event.Skip(); });
			window->Bind(wxEVT_LEAVE_WINDOW, [pointer, set](wxMouseEvent& event) { set(pointer->pressed, false); set(pointer->hover, false); event.Skip(); });
			window->Bind(wxEVT_LEFT_DOWN, [pointer, set](wxMouseEvent& event) { set(pointer->pressed, true); event.Skip(); });
			window->Bind(wxEVT_LEFT_DCLICK, [pointer, set](wxMouseEvent& event) { set(pointer->pressed, true); event.Skip(); });
			window->Bind(wxEVT_LEFT_UP, [pointer, set](wxMouseEvent& event) { set(pointer->pressed, false); event.Skip(); });
			window->Bind(wxEVT_SET_FOCUS, [window](wxFocusEvent& event) { window->Refresh(false); event.Skip(); });
			window->Bind(wxEVT_KILL_FOCUS, [window](wxFocusEvent& event) { window->Refresh(false); event.Skip(); });
			window->Bind(wxEVT_SIZE, [window](wxSizeEvent& event) { window->Refresh(false); event.Skip(); });
			window->Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent&) {});
			return pointer;
		}

		wxColour SurfaceOf(wxWindow* window)
		{
			return window->UseBgCol() ? window->GetBackgroundColour() : window->GetParent()->GetBackgroundColour();
		}

		wxColour Mix(const wxColour& from, const wxColour& to, double amount)
		{
			const auto channel = [amount](unsigned char a, unsigned char b) { return static_cast<unsigned char>(a + (b - a) * amount + 0.5); };
			return wxColour(channel(from.Red(), to.Red()), channel(from.Green(), to.Green()), channel(from.Blue(), to.Blue()));
		}

		void DrawFocusRing(wxGraphicsContext& gc, wxWindow* window, double x, double y, double width, double height, double radius)
		{
			gc.SetPen(wxPen(Get().accent, window->FromDIP(2)));
			gc.SetBrush(*wxTRANSPARENT_BRUSH);
			gc.DrawRoundedRectangle(x + 1, y + 1, width - 2, height - 2, radius);
		}

		void PaintButton(wxButton* button, const Pointer& pointer)
		{
			wxAutoBufferedPaintDC dc(button);
			const auto& palette = Get();
			const wxSize size = button->GetClientSize();
			dc.SetBackground(wxBrush(SurfaceOf(button)));
			dc.Clear();
			std::unique_ptr<wxGraphicsContext> gc{ wxGraphicsContext::Create(dc) };
			if (!gc)
			{
				return;
			}
			const bool enabled = button->IsEnabled();
			const bool hover = enabled && pointer.hover;
			const bool pressed = enabled && (pointer.pressed || (SendMessageW(static_cast<HWND>(button->GetHWND()), BM_GETSTATE, 0, 0) & BST_PUSHED));
			const double radius = button->FromDIP(6);
			const double width = size.x - 1.0, height = size.y - 1.0;

			// A color picker shows its color as a swatch inside a field.
			if (auto* picker = wxDynamicCast(button->GetParent(), wxColourPickerCtrl))
			{
				gc->SetPen(wxPen(hover ? palette.subtext : palette.border, button->FromDIP(1)));
				gc->SetBrush(wxBrush(FieldColour(hover)));
				gc->DrawRoundedRectangle(0.5, 0.5, width, height, radius);
				const double inset = button->FromDIP(5);
				const wxColour colour = picker->GetColour();
				gc->SetPen(wxPen(Mix(colour, palette.text, 0.25), 1));
				gc->SetBrush(wxBrush(enabled ? colour : Mix(colour, FieldColour(false), 0.6)));
				gc->DrawRoundedRectangle(inset, inset, size.x - inset * 2, size.y - inset * 2, radius - button->FromDIP(2));
				if (ShowsFocus(button)) DrawFocusRing(*gc, button, 0, 0, size.x, size.y, radius);
				return;
			}

			const bool accent = button->GetName() == c_accentButtonName && enabled;
			if (accent)
			{
				const double lightness = pressed ? 85 : hover ? 112 : 100;
				gc->SetPen(*wxTRANSPARENT_PEN);
				gc->SetBrush(gc->CreateLinearGradientBrush(0, 0, size.x, 0, palette.accent.ChangeLightness(lightness), palette.accentEnd.ChangeLightness(lightness)));
			}
			else
			{
				const wxColour normal = palette.dark ? wxColour(27, 45, 72) : wxColour(248, 250, 253);
				const wxColour fill = !enabled ? SurfaceOf(button)
					: pressed ? (palette.dark ? wxColour(21, 36, 58) : wxColour(226, 234, 244))
					: hover ? (palette.dark ? wxColour(37, 61, 96) : wxColour(236, 242, 250))
					: normal;
				gc->SetPen(wxPen(hover && !pressed ? palette.subtext : palette.border, button->FromDIP(1)));
				gc->SetBrush(wxBrush(fill));
			}
			gc->DrawRoundedRectangle(0.5, 0.5, width, height, radius);

			const wxColour foreground = accent ? palette.accentText : enabled ? palette.text : palette.subtext;
			gc->SetFont(button->GetFont(), foreground);
			const wxString label = button->GetLabelText();
			double textWidth{}, textHeight{};
			gc->GetTextExtent(label, &textWidth, &textHeight);
			gc->DrawText(label, (size.x - textWidth) / 2, (size.y - textHeight) / 2 + (pressed ? 1 : 0));
			if (ShowsFocus(button)) DrawFocusRing(*gc, button, 0, 0, size.x, size.y, radius);
		}

		void PaintSwitch(wxCheckBox* box, const Pointer& pointer)
		{
			wxAutoBufferedPaintDC dc(box);
			const auto& palette = Get();
			const wxSize size = box->GetClientSize();
			dc.SetBackground(wxBrush(SurfaceOf(box)));
			dc.Clear();
			std::unique_ptr<wxGraphicsContext> gc{ wxGraphicsContext::Create(dc) };
			if (!gc)
			{
				return;
			}
			const bool enabled = box->IsEnabled();
			const bool on = box->Is3State() ? box->Get3StateValue() != wxCHK_UNCHECKED : box->GetValue();
			const bool hover = enabled && pointer.hover;
			const double trackWidth = box->FromDIP(40), trackHeight = box->FromDIP(20);
			const double x = 1, y = (size.y - trackHeight) / 2;
			const double radius = trackHeight / 2;

			if (on)
			{
				gc->SetPen(*wxTRANSPARENT_PEN);
				if (enabled)
				{
					const double lightness = pointer.pressed ? 88 : hover ? 112 : 100;
					gc->SetBrush(gc->CreateLinearGradientBrush(x, 0, x + trackWidth, 0, palette.accentEnd.ChangeLightness(lightness), palette.accent.ChangeLightness(lightness)));
				}
				else
				{
					gc->SetBrush(wxBrush(palette.track));
				}
			}
			else
			{
				gc->SetPen(wxPen(enabled ? (hover ? palette.text : palette.subtext) : palette.border, box->FromDIP(1)));
				gc->SetBrush(wxBrush(hover ? palette.hover : SurfaceOf(box)));
			}
			gc->DrawRoundedRectangle(x + 0.5, y + 0.5, trackWidth - 1, trackHeight - 1, radius - 0.5);

			// The knob grows a little under the pointer, like the Windows 11 switch.
			const double knob = (on ? radius - box->FromDIP(4) : radius - box->FromDIP(5)) + (hover ? box->FromDIP(1) : 0);
			const double knobX = on ? x + trackWidth - radius : x + radius;
			gc->SetPen(*wxTRANSPARENT_PEN);
			gc->SetBrush(wxBrush(on ? (enabled ? palette.accentText : palette.subtext) : enabled ? (hover ? palette.text : palette.subtext) : palette.border));
			gc->DrawEllipse(knobX - knob, y + radius - knob, knob * 2, knob * 2);
			if (ShowsFocus(box)) DrawFocusRing(*gc, box, x - 3, y - 3, trackWidth + 6, trackHeight + 6, radius + 3);

			gc->SetFont(box->GetFont(), enabled ? palette.text : palette.subtext);
			const wxString label = box->GetLabelText();
			double textWidth{}, textHeight{};
			gc->GetTextExtent(label, &textWidth, &textHeight);
			gc->DrawText(label, x + trackWidth + box->FromDIP(10), (size.y - textHeight) / 2);
		}

		void PaintGauge(wxGauge* gauge)
		{
			wxAutoBufferedPaintDC dc(gauge);
			const auto& palette = Get();
			const wxSize size = gauge->GetClientSize();
			dc.SetBackground(wxBrush(SurfaceOf(gauge)));
			dc.Clear();
			std::unique_ptr<wxGraphicsContext> gc{ wxGraphicsContext::Create(dc) };
			if (!gc)
			{
				return;
			}
			const double height = gauge->FromDIP(6);
			const double top = (size.y - height) / 2;
			gc->SetPen(*wxTRANSPARENT_PEN);
			gc->SetBrush(wxBrush(palette.track));
			gc->DrawRoundedRectangle(0, top, size.x, height, height / 2);

			double start = 0, end = 0;
			if (GetWindowLongPtrW(static_cast<HWND>(gauge->GetHWND()), GWL_STYLE) & PBS_MARQUEE)
			{
				// Indeterminate: a short bar sweeps across while the control's own timer repaints it.
				const double segment = size.x * 0.3;
				const double phase = (GetTickCount64() % 1600) / 1600.0;
				start = std::max(0.0, -segment + phase * (size.x + segment));
				end = std::min<double>(size.x, -segment + phase * (size.x + segment) + segment);
			}
			else if (gauge->GetRange() > 0)
			{
				end = size.x * std::clamp(static_cast<double>(gauge->GetValue()) / gauge->GetRange(), 0.0, 1.0);
			}
			if (end - start >= 1)
			{
				gc->SetBrush(gc->CreateLinearGradientBrush(start, 0, end, 0, palette.accentEnd, palette.accent));
				gc->DrawRoundedRectangle(start, top, end - start, height, height / 2);
			}
		}

		// Chevrons for collapsible sections, no dotted focus rectangles and rounded check boxes
		// for the generic controls that ask the renderer to draw them.
		class Renderer final : public wxDelegateRendererNative
		{
		public:
			Renderer() : wxDelegateRendererNative(wxRendererNative::GetDefault()) {}

			wxSize GetCollapseButtonSize(wxWindow* window, wxReadOnlyDC&) override
			{
				return window->FromDIP(wxSize(20, 20));
			}

			void DrawCollapseButton(wxWindow* window, wxDC& dc, const wxRect& rect, int flags) override
			{
				const auto& palette = ModernTheme::Get();
				const wxString chevron(wxUniChar(flags & wxCONTROL_EXPANDED ? 0xE70D : 0xE76C));
				// The header draws its label with the same DC right after this.
				wxDCFontChanger font(dc, IconFont(window, 8));
				wxDCTextColourChanger colour(dc, flags & wxCONTROL_CURRENT ? palette.accent : palette.subtext);
				const wxSize extent = dc.GetTextExtent(chevron);
				dc.DrawText(chevron, rect.x + (rect.width - extent.x) / 2, rect.y + (rect.height - extent.y) / 2);
			}

			void DrawFocusRect(wxWindow*, wxDC&, const wxRect&, int) override {}

			void DrawCheckBox(wxWindow* window, wxDC& dc, const wxRect& rect, int flags) override
			{
				std::unique_ptr<wxGraphicsContext> gc{ wxGraphicsContext::CreateFromUnknownDC(dc) };
				if (!gc)
				{
					wxDelegateRendererNative::DrawCheckBox(window, dc, rect, flags);
					return;
				}
				const auto& palette = ModernTheme::Get();
				const bool checked = flags & wxCONTROL_CHECKED;
				const bool enabled = !(flags & wxCONTROL_DISABLED);
				const double side = std::min(rect.width, rect.height) - 2.0;
				const double x = rect.x + (rect.width - side) / 2, y = rect.y + (rect.height - side) / 2;
				gc->SetPen(checked ? *wxTRANSPARENT_PEN : wxPen(enabled ? palette.subtext : palette.border, window->FromDIP(1)));
				gc->SetBrush(checked ? wxBrush(enabled ? palette.accent : palette.track) : *wxTRANSPARENT_BRUSH);
				gc->DrawRoundedRectangle(x + 0.5, y + 0.5, side - 1, side - 1, window->FromDIP(4));
				if (checked)
				{
					const wxString mark(wxUniChar(0xE73E));
					gc->SetFont(IconFont(window, 7), palette.accentText);
					double width{}, height{};
					gc->GetTextExtent(mark, &width, &height);
					gc->DrawText(mark, x + (side - width) / 2, y + (side - height) / 2);
				}
			}
		};

		// Dialogs are themed as they open and the color pickers open the theme's color dialog.
		class Filter final : public wxEventFilter
		{
		public:
			int FilterEvent(wxEvent& event) override
			{
				const auto type = event.GetEventType();
				if (type == wxEVT_INIT_DIALOG)
				{
					if (auto* dialog = wxDynamicCast(event.GetEventObject(), wxDialog))
					{
						StyleDialog(dialog);
					}
				}
				else if (type == wxEVT_BUTTON)
				{
					auto* widget = wxDynamicCast(event.GetEventObject(), wxWindow);
					if (auto* picker = widget ? wxDynamicCast(widget->GetParent(), wxColourPickerCtrl) : nullptr)
					{
						wxColour colour = picker->GetColour();
						if (PickColour(picker, colour))
						{
							picker->SetColour(colour);
							wxColourPickerEvent changed(picker, picker->GetId(), colour);
							picker->GetEventHandler()->ProcessEvent(changed);
						}
						picker->GetPickerCtrl()->Refresh(false);
						return Event_Processed;
					}
				}
				return Event_Skip;
			}
		};

		// A bitmap whose outside of a rounded rectangle is transparent, with an anti-aliased edge.
		template <typename ColourAt>
		wxBitmap RoundedImage(int width, int height, double radius, ColourAt colourAt)
		{
			wxImage image(std::max(width, 1), std::max(height, 1));
			image.InitAlpha();
			unsigned char* data = image.GetData();
			unsigned char* alpha = image.GetAlpha();
			const double halfWidth = width / 2.0, halfHeight = height / 2.0;
			for (int y = 0; y < height; ++y)
			{
				for (int x = 0; x < width; ++x)
				{
					const wxImage::RGBValue rgb = colourAt(width > 1 ? x / (width - 1.0) : 0.0, height > 1 ? y / (height - 1.0) : 0.0);
					*data++ = rgb.red;
					*data++ = rgb.green;
					*data++ = rgb.blue;
					const double qx = std::abs(x + 0.5 - halfWidth) - (halfWidth - radius);
					const double qy = std::abs(y + 0.5 - halfHeight) - (halfHeight - radius);
					const double distance = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0) - radius;
					*alpha++ = static_cast<unsigned char>(std::clamp(0.5 - distance, 0.0, 1.0) * 255);
				}
			}
			return wxBitmap(image);
		}

		void DrawMarker(wxGraphicsContext& gc, wxWindow* window, double x, double y)
		{
			const double radius = window->FromDIP(7);
			gc.SetBrush(*wxTRANSPARENT_BRUSH);
			gc.SetPen(wxPen(wxColour(0, 0, 0, 110), window->FromDIP(4)));
			gc.DrawEllipse(x - radius, y - radius, radius * 2, radius * 2);
			gc.SetPen(wxPen(*wxWHITE, window->FromDIP(2)));
			gc.DrawEllipse(x - radius, y - radius, radius * 2, radius * 2);
		}

		// Mouse drags inside a window report a position in 0..1 on both axes, measured inside
		// the margin the window keeps for its marker.
		template <typename OnMove>
		void BindDrag(wxWindow* window, wxSize margin, OnMove onMove)
		{
			const auto move = [window, margin, onMove](const wxPoint& point)
			{
				const wxSize size = window->GetClientSize() - margin * 2;
				onMove(std::clamp((point.x - margin.x) / std::max(size.x - 1.0, 1.0), 0.0, 1.0),
					std::clamp((point.y - margin.y) / std::max(size.y - 1.0, 1.0), 0.0, 1.0));
			};
			window->Bind(wxEVT_LEFT_DOWN, [window, move](wxMouseEvent& event) { window->CaptureMouse(); move(event.GetPosition()); });
			window->Bind(wxEVT_MOTION, [window, move](wxMouseEvent& event) { if (window->HasCapture()) move(event.GetPosition()); });
			window->Bind(wxEVT_LEFT_UP, [window](wxMouseEvent&) { if (window->HasCapture()) window->ReleaseMouse(); });
			window->Bind(wxEVT_MOUSE_CAPTURE_LOST, [](wxMouseCaptureLostEvent&) {});
			window->SetCursor(wxCursor(wxCURSOR_HAND));
		}

		wxWindow* PaintedWindow(wxWindow* parent, const wxSize& size)
		{
			auto* window = new wxWindow(parent, wxID_ANY, wxDefaultPosition, size, wxBORDER_NONE | wxFULL_REPAINT_ON_RESIZE);
			window->SetBackgroundStyle(wxBG_STYLE_PAINT);
			window->SetMinSize(size);
			return window;
		}
	}

	wxColour FieldColour(bool hover)
	{
		const auto& palette = Get();
		return palette.dark ? (hover ? wxColour(32, 52, 82) : wxColour(24, 41, 66)) : (hover ? wxColour(246, 249, 253) : *wxWHITE);
	}

	void Install()
	{
		// The first Get() asks the app traits for a renderer and would replace one set before it.
		wxRendererNative::Get();
		wxRendererNative::Set(new Renderer);
		wxEvtHandler::AddFilter(new Filter);
	}

	void StyleButton(wxButton* button)
	{
		if (!FirstTime(button))
		{
			return;
		}
		auto pointer = TrackPointer(button);
		button->Bind(wxEVT_PAINT, [button, pointer](wxPaintEvent&) { PaintButton(button, *pointer); });

		// Roomier than the native button, like the drop-downs next to them.
		const bool swatch = wxDynamicCast(button->GetParent(), wxColourPickerCtrl) != nullptr;
		wxClientDC dc(button);
		dc.SetFont(button->GetFont());
		const int textWidth = swatch ? button->FromDIP(36) : dc.GetTextExtent(button->GetLabelText()).x;
		const wxSize current = button->GetMinSize();
		const wxSize best = button->GetBestSize();
		button->SetMinSize(wxSize(
			std::max({ current.x, best.x, textWidth + button->FromDIP(swatch ? 16 : 32) }),
			std::max({ current.y, best.y, button->FromDIP(32) })));
		button->Refresh(false);
	}

	void StyleAccentButton(wxButton* button)
	{
		button->SetName(c_accentButtonName);
		StyleButton(button);
		button->Refresh(false);
	}

	void StyleCheckBox(wxCheckBox* box)
	{
		if (!FirstTime(box))
		{
			return;
		}
		auto pointer = TrackPointer(box);
		box->Bind(wxEVT_PAINT, [box, pointer](wxPaintEvent&) { PaintSwitch(box, *pointer); });
		box->Bind(wxEVT_CHECKBOX, [box](wxCommandEvent& event) { box->Refresh(false); event.Skip(); });

		wxClientDC dc(box);
		dc.SetFont(box->GetFont());
		const wxSize text = dc.GetTextExtent(box->GetLabelText());
		box->SetMinSize(wxSize(box->FromDIP(52) + text.x + box->FromDIP(2), std::max(box->FromDIP(26), text.y)));
		box->InvalidateBestSize();
		box->Refresh(false);
	}

	void StyleGauge(wxGauge* gauge)
	{
		if (!FirstTime(gauge))
		{
			return;
		}
		gauge->Bind(wxEVT_PAINT, [gauge](wxPaintEvent&) { PaintGauge(gauge); });
		gauge->Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent&) {});
		gauge->SetMinSize(wxSize(gauge->GetMinSize().x, gauge->FromDIP(10)));
	}

	void StyleDialog(wxDialog* dialog)
	{
		if (!FirstTime(dialog))
		{
			return;
		}
		const auto& palette = Get();
		dialog->SetBackgroundColour(palette.page);
		// Windows without their own color take their parent's, top down, so footers keep theirs.
		const std::function<void(wxWindow*)> inherit = [&](wxWindow* window)
		{
			for (auto* child : window->GetChildren())
			{
				if (child->IsTopLevel())
				{
					continue;
				}
				if (!child->UseBgCol() && !wxDynamicCast(child, wxTextCtrl) && !wxDynamicCast(child, wxChoice) && !wxDynamicCast(child, wxSpinCtrl))
				{
					child->SetBackgroundColour(window->GetBackgroundColour());
				}
				inherit(child);
			}
		};
		inherit(dialog);
		StyleControls(dialog);

		// Themed controls are larger than native ones; grow the dialog if they no longer fit.
		if (auto* sizer = dialog->GetSizer())
		{
			const wxSize fitting = sizer->ComputeFittingWindowSize(dialog);
			const wxSize minimum = dialog->GetMinSize();
			dialog->SetMinSize(wxSize(std::max(minimum.x, fitting.x), std::max(minimum.y, fitting.y)));
			const wxSize size = dialog->GetSize();
			if (size.x < fitting.x || size.y < fitting.y)
			{
				dialog->SetSize(wxSize(std::max(size.x, fitting.x), std::max(size.y, fitting.y)));
			}
			dialog->Layout();
		}
		dialog->Refresh();
	}

	int ShowMessage(const wxString& message, const wxString& caption, long style, wxWindow* parent, int, int)
	{
		const auto& palette = Get();
		if (!(style & wxICON_MASK))
		{
			style |= (style & wxYES) ? wxICON_QUESTION : wxICON_INFORMATION;
		}
		wxWindow* owner = parent ? wxGetTopLevelParent(parent) : wxTheApp->GetTopWindow();
		wxDialog dialog(owner, wxID_ANY, caption, wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX | wxSYSTEM_MENU);
		dialog.SetBackgroundColour(palette.page);

		auto* root = new wxBoxSizer(wxVERTICAL);
		auto* body = new wxBoxSizer(wxHORIZONTAL);
		if (!(style & wxICON_NONE))
		{
			const auto [glyph, colour] = (style & wxICON_ERROR) ? std::pair{ 0xEA39, wxColour(255, 99, 110) }
				: (style & wxICON_WARNING) ? std::pair{ 0xE7BA, wxColour(252, 190, 40) }
				: (style & wxICON_QUESTION) ? std::pair{ 0xE9CE, palette.accent }
				: std::pair{ 0xE946, palette.accent };
			auto* icon = new wxStaticText(&dialog, wxID_ANY, wxString(wxUniChar(glyph)));
			icon->SetFont(IconFont(&dialog, 20));
			icon->SetForegroundColour(colour);
			body->Add(icon, 0, wxRIGHT, dialog.FromDIP(16));
		}
		auto* text = new wxStaticText(&dialog, wxID_ANY, message);
		text->SetForegroundColour(palette.text);
		text->Wrap(dialog.FromDIP(440));
		body->Add(text, 1, wxTOP, dialog.FromDIP(4));
		root->Add(body, 1, wxEXPAND | wxALL, dialog.FromDIP(24));

		// A footer strip holds the buttons, like the Windows 11 content dialog.
		auto* footer = new wxPanel(&dialog);
		footer->SetBackgroundColour(palette.card);
		auto* buttons = new wxBoxSizer(wxHORIZONTAL);
		buttons->AddStretchSpacer();
		std::vector<std::pair<int, wxString>> ids;
		if (style & wxYES_NO) { ids.emplace_back(wxID_YES, L"Yes"); ids.emplace_back(wxID_NO, L"No"); }
		if (style & wxOK) ids.emplace_back(wxID_OK, L"OK");
		if (style & wxCANCEL) ids.emplace_back(wxID_CANCEL, L"Cancel");
		if (ids.empty()) ids.emplace_back(wxID_OK, L"OK");
		const int primary = (style & wxNO_DEFAULT) && (style & wxYES_NO) ? wxID_NO
			: (style & wxCANCEL_DEFAULT) && (style & wxCANCEL) ? wxID_CANCEL
			: ids.front().first;
		for (const auto& [id, label] : ids)
		{
			auto* button = new wxButton(footer, id, label);
			button->SetMinSize(dialog.FromDIP(wxSize(96, 32)));
			button->Bind(wxEVT_BUTTON, [&dialog, id](wxCommandEvent&) { dialog.EndModal(id); });
			if (id == primary)
			{
				StyleAccentButton(button);
				button->SetDefault();
			}
			buttons->Add(button, 0, wxLEFT, dialog.FromDIP(8));
		}
		auto* footerSizer = new wxBoxSizer(wxVERTICAL);
		footerSizer->Add(buttons, 0, wxEXPAND | wxALL, dialog.FromDIP(16));
		footer->SetSizer(footerSizer);
		root->Add(footer, 0, wxEXPAND);

		const auto has = [&ids](int id) { return std::any_of(ids.begin(), ids.end(), [id](const auto& entry) { return entry.first == id; }); };
		dialog.SetEscapeId(has(wxID_CANCEL) ? wxID_CANCEL : has(wxID_NO) ? wxID_NO : wxID_OK);
		dialog.SetAffirmativeId(has(wxID_YES) ? wxID_YES : wxID_OK);
		dialog.SetSizerAndFit(root);
		dialog.SetMinSize(wxSize(dialog.FromDIP(380), -1));
		dialog.Fit();
		dialog.CentreOnParent();
		if (auto* focus = dialog.FindWindow(primary)) focus->SetFocus();

		switch (dialog.ShowModal())
		{
		case wxID_YES: return wxYES;
		case wxID_NO: return wxNO;
		case wxID_CANCEL: return wxCANCEL;
		default: return wxOK;
		}
	}

	bool PickColour(wxWindow* parent, wxColour& colour, const wxString& title)
	{
		const auto& palette = Get();
		wxDialog dialog(parent ? wxGetTopLevelParent(parent) : nullptr, wxID_ANY, title.empty() ? wxString(L"Choose a color") : title);
		dialog.SetBackgroundColour(palette.page);

		struct State { double hue{}, saturation{}, value{}; wxBitmap plane; double planeHue{ -1 }; };
		auto state = std::make_shared<State>();
		const auto hsv = wxImage::RGBtoHSV(wxImage::RGBValue(colour.Red(), colour.Green(), colour.Blue()));
		state->hue = hsv.hue;
		state->saturation = hsv.saturation;
		state->value = hsv.value;
		const wxColour original = colour;
		const auto current = [state]
		{
			const auto rgb = wxImage::HSVtoRGB(wxImage::HSVValue(state->hue, state->saturation, state->value));
			return wxColour(rgb.red, rgb.green, rgb.blue);
		};

		// Both pickers keep a margin so their round marker is never cut at an edge.
		const wxSize margin = dialog.FromDIP(wxSize(9, 9));
		auto* plane = PaintedWindow(&dialog, dialog.FromDIP(wxSize(300, 190)) + margin * 2);
		auto* hue = PaintedWindow(&dialog, wxSize(dialog.FromDIP(300) + margin.x * 2, dialog.FromDIP(24)));
		auto* preview = PaintedWindow(&dialog, dialog.FromDIP(wxSize(72, 72)));
		auto* hex = new wxTextCtrl(&dialog, wxID_ANY, current().GetAsString(wxC2S_HTML_SYNTAX));
		hex->SetMaxLength(7);

		const auto changed = [=](bool typed)
		{
			plane->Refresh(false);
			hue->Refresh(false);
			preview->Refresh(false);
			if (!typed) hex->ChangeValue(current().GetAsString(wxC2S_HTML_SYNTAX));
		};

		plane->Bind(wxEVT_PAINT, [plane, state, margin](wxPaintEvent&)
		{
			wxAutoBufferedPaintDC dc(plane);
			dc.SetBackground(wxBrush(Get().page));
			dc.Clear();
			const wxSize size = plane->GetClientSize() - margin * 2;
			if (state->planeHue != state->hue || state->plane.GetSize() != size)
			{
				const double hueNow = state->hue;
				state->plane = RoundedImage(size.x, size.y, plane->FromDIP(8), [hueNow](double x, double y)
				{
					return wxImage::HSVtoRGB(wxImage::HSVValue(hueNow, x, 1.0 - y));
				});
				state->planeHue = hueNow;
			}
			std::unique_ptr<wxGraphicsContext> gc{ wxGraphicsContext::Create(dc) };
			if (!gc) return;
			gc->DrawBitmap(state->plane, margin.x, margin.y, size.x, size.y);
			DrawMarker(*gc, plane, margin.x + state->saturation * (size.x - 1), margin.y + (1.0 - state->value) * (size.y - 1));
		});
		BindDrag(plane, margin, [state, changed](double x, double y) { state->saturation = x; state->value = 1.0 - y; changed(false); });

		auto hueStrip = std::make_shared<wxBitmap>();
		hue->Bind(wxEVT_PAINT, [hue, state, hueStrip, margin](wxPaintEvent&)
		{
			wxAutoBufferedPaintDC dc(hue);
			dc.SetBackground(wxBrush(Get().page));
			dc.Clear();
			const wxSize size = hue->GetClientSize();
			const wxSize strip(size.x - margin.x * 2, hue->FromDIP(12));
			if (hueStrip->GetSize() != strip)
			{
				*hueStrip = RoundedImage(strip.x, strip.y, strip.y / 2.0, [](double x, double) { return wxImage::HSVtoRGB(wxImage::HSVValue(x, 1.0, 1.0)); });
			}
			std::unique_ptr<wxGraphicsContext> gc{ wxGraphicsContext::Create(dc) };
			if (!gc) return;
			gc->DrawBitmap(*hueStrip, margin.x, (size.y - strip.y) / 2.0, strip.x, strip.y);
			DrawMarker(*gc, hue, margin.x + state->hue * (strip.x - 1), size.y / 2.0);
		});
		BindDrag(hue, wxSize(margin.x, 0), [state, changed](double x, double) { state->hue = std::min(x, 0.999); changed(false); });

		preview->Bind(wxEVT_PAINT, [preview, current, original, &palette](wxPaintEvent&)
		{
			wxAutoBufferedPaintDC dc(preview);
			dc.SetBackground(wxBrush(palette.page));
			dc.Clear();
			std::unique_ptr<wxGraphicsContext> gc{ wxGraphicsContext::Create(dc) };
			if (!gc) return;
			const wxSize size = preview->GetClientSize();
			const double radius = preview->FromDIP(10);
			gc->SetPen(*wxTRANSPARENT_PEN);
			// New color on top, the color the dialog opened with below it.
			gc->SetBrush(wxBrush(original));
			gc->DrawRoundedRectangle(0, 0, size.x, size.y, radius);
			gc->Clip(0, 0, size.x, size.y / 2.0);
			gc->SetBrush(wxBrush(current()));
			gc->DrawRoundedRectangle(0, 0, size.x, size.y, radius);
			gc->ResetClip();
			gc->SetPen(wxPen(palette.border, preview->FromDIP(1)));
			gc->SetBrush(*wxTRANSPARENT_BRUSH);
			gc->DrawRoundedRectangle(0.5, 0.5, size.x - 1.0, size.y - 1.0, radius);
		});

		hex->Bind(wxEVT_TEXT, [hex, state, changed](wxCommandEvent&)
		{
			wxString text = hex->GetValue().Trim().Trim(false);
			if (!text.StartsWith(L"#")) text.Prepend(L"#");
			wxColour typed;
			if (text.length() == 7 && typed.Set(text))
			{
				const auto value = wxImage::RGBtoHSV(wxImage::RGBValue(typed.Red(), typed.Green(), typed.Blue()));
				state->hue = value.hue;
				state->saturation = value.saturation;
				state->value = value.value;
				changed(true);
			}
		});

		auto* root = new wxBoxSizer(wxVERTICAL);
		auto* top = new wxBoxSizer(wxHORIZONTAL);
		top->Add(plane, 0);
		auto* side = new wxBoxSizer(wxVERTICAL);
		side->Add(preview, 0, wxBOTTOM, dialog.FromDIP(14));
		auto* hexLabel = new wxStaticText(&dialog, wxID_ANY, L"Hex");
		hexLabel->SetForegroundColour(palette.subtext);
		side->Add(hexLabel, 0, wxBOTTOM, dialog.FromDIP(6));
		side->Add(hex, 0, wxEXPAND | wxLEFT | wxRIGHT, dialog.FromDIP(4));
		top->Add(side, 0, wxLEFT | wxTOP | wxRIGHT, dialog.FromDIP(9));
		root->Add(top, 0, wxLEFT | wxRIGHT | wxTOP, dialog.FromDIP(12));
		root->Add(hue, 0, wxLEFT | wxRIGHT | wxTOP, dialog.FromDIP(12));
		root->AddSpacer(dialog.FromDIP(20));

		auto* footer = new wxPanel(&dialog);
		footer->SetBackgroundColour(palette.card);
		auto* buttons = new wxBoxSizer(wxHORIZONTAL);
		buttons->AddStretchSpacer();
		auto* ok = new wxButton(footer, wxID_OK, L"OK");
		auto* cancel = new wxButton(footer, wxID_CANCEL, L"Cancel");
		for (auto* button : { ok, cancel })
		{
			button->SetMinSize(dialog.FromDIP(wxSize(96, 32)));
			buttons->Add(button, 0, wxLEFT, dialog.FromDIP(8));
		}
		StyleAccentButton(ok);
		ok->SetDefault();
		auto* footerSizer = new wxBoxSizer(wxVERTICAL);
		footerSizer->Add(buttons, 0, wxEXPAND | wxALL, dialog.FromDIP(16));
		footer->SetSizer(footerSizer);
		root->Add(footer, 0, wxEXPAND);
		dialog.SetSizerAndFit(root);
		dialog.CentreOnParent();

		if (dialog.ShowModal() != wxID_OK)
		{
			return false;
		}
		colour = current();
		return true;
	}
}
