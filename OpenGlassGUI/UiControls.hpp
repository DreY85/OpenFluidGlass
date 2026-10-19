#pragma once
#include "pch.h"
#include "ModernTheme.hpp"

namespace OpenGlass
{
	// Native trackbar with a custom-drawn track and thumb: a gradient fill up to a round thumb.
	class NativeSlider : public wxSlider
	{
	public:
		NativeSlider(wxWindow* parent,
			wxWindowID id,
			int value,
			int minValue,
			int maxValue,
			const wxPoint& pos = wxDefaultPosition,
			const wxSize& size = wxDefaultSize,
			long style = wxSL_HORIZONTAL,
			const wxValidator& validator = wxDefaultValidator,
			const wxString& name = wxSliderNameStr)
		{
			Create(parent, id, value, minValue, maxValue, pos, size, style, validator, name);
		}

		bool Create(wxWindow* parent,
			wxWindowID id,
			int value,
			int minValue,
			int maxValue,
			const wxPoint& pos = wxDefaultPosition,
			const wxSize& size = wxDefaultSize,
			long style = wxSL_HORIZONTAL,
			const wxValidator& validator = wxDefaultValidator,
			const wxString& name = wxSliderNameStr)
		{
			if (!wxSlider::Create(parent, id, value, minValue, maxValue, pos, size, style, validator, name))
			{
				return false;
			}
			// The round thumb is wider than the native one, so repaint the whole control as it moves.
			Bind(wxEVT_SLIDER, [this](wxCommandEvent& event) { Refresh(false); event.Skip(); });
			Bind(wxEVT_SCROLL_THUMBTRACK, [this](wxScrollEvent& event) { Refresh(false); event.Skip(); });
			return true;
		}

		void SetValue(int value) override
		{
			wxSlider::SetValue(value);
			Refresh(false);
		}

		bool MSWOnNotify(int idCtrl, WXLPARAM lParam, WXLPARAM* result) override
		{
			auto* draw = reinterpret_cast<NMCUSTOMDRAW*>(lParam);
			if (draw->hdr.code != NM_CUSTOMDRAW || draw->hdr.hwndFrom != static_cast<HWND>(GetHWND()) || HasFlag(wxSL_VERTICAL))
			{
				return wxSlider::MSWOnNotify(idCtrl, lParam, result);
			}
			if (draw->dwDrawStage == CDDS_PREPAINT)
			{
				*result = CDRF_NOTIFYITEMDRAW;
				return true;
			}
			if (draw->dwDrawStage == CDDS_ITEMPREPAINT && (draw->dwItemSpec == TBCD_CHANNEL || draw->dwItemSpec == TBCD_THUMB))
			{
				if (draw->dwItemSpec == TBCD_CHANNEL)
				{
					PaintTrack(draw->hdc, draw->rc);
				}
				else
				{
					PaintThumb(draw->hdc, draw->rc);
				}
				*result = CDRF_SKIPDEFAULT;
				return true;
			}
			*result = CDRF_DODEFAULT;
			return true;
		}

	private:
		void PaintTrack(HDC hdc, const RECT& channel)
		{
			std::unique_ptr<wxGraphicsContext> gc{ wxGraphicsContext::CreateFromNativeHDC(hdc) };
			if (!gc)
			{
				return;
			}
			const auto& palette = ModernTheme::Get();
			RECT thumb{};
			SendMessageW(static_cast<HWND>(GetHWND()), TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));

			const double height = FromDIP(4);
			const double top = (channel.top + channel.bottom) / 2.0 - height / 2;
			const double left = channel.left;
			const double right = channel.right;
			const double filled = std::clamp((thumb.left + thumb.right) / 2.0, left, right);

			gc->SetPen(*wxTRANSPARENT_PEN);
			gc->SetBrush(wxBrush(palette.track));
			gc->DrawRoundedRectangle(left, top, right - left, height, height / 2);
			if (filled > left)
			{
				const wxColour start = IsEnabled() ? palette.accent : palette.subtext;
				const wxColour end = IsEnabled() ? palette.accentEnd : palette.subtext;
				gc->SetBrush(gc->CreateLinearGradientBrush(left, 0, filled, 0, end, start));
				gc->DrawRoundedRectangle(left, top, filled - left, height, height / 2);
			}
		}

		void PaintThumb(HDC hdc, const RECT& thumb)
		{
			std::unique_ptr<wxGraphicsContext> gc{ wxGraphicsContext::CreateFromNativeHDC(hdc) };
			if (!gc)
			{
				return;
			}
			const auto& palette = ModernTheme::Get();
			const double radius = FromDIP(8);
			const double centerX = (thumb.left + thumb.right) / 2.0;
			const double centerY = (thumb.top + thumb.bottom) / 2.0;
			const wxColour ring = IsEnabled() ? palette.accent : palette.subtext;

			gc->SetPen(*wxTRANSPARENT_PEN);
			gc->SetBrush(wxBrush(ring));
			gc->DrawEllipse(centerX - radius, centerY - radius, radius * 2, radius * 2);
			const double inner = radius - FromDIP(3);
			gc->SetBrush(wxBrush(palette.dark ? wxColour(234, 244, 255) : *wxWHITE));
			gc->DrawEllipse(centerX - inner, centerY - inner, inner * 2, inner * 2);
		}
	};
}
