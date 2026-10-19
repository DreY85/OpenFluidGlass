#include "pch.h"
#include "MainFrame.hpp"
#include "UiControls.hpp"
#include "DwmSettings.hpp"
#include <wx/clipbrd.h>

// Liquid Glass tab: refraction settings for the Direct3D renderer.
// Unlike the other tabs these values are live. Every change is written to HKCU and DWM reloads at once,
// so they stay outside Save and Revert. OpenGlass stores them as DWORDs holding the value times 100;
// the light angle is stored with 360 degrees added.

namespace OpenGlass
{
	namespace
	{
		struct LiquidGlassParameter
		{
			PCWSTR name;
			PCWSTR label;
			PCWSTR hint;
			PCWSTR option;	// set-liquid-glass.ps1 parameter, used by Copy values
			float minimum;
			float maximum;
			float step;
			float defaultValue;
			PCWSTR unit;
		};

		struct LiquidGlassGroup
		{
			PCWSTR title;
			std::initializer_list<LiquidGlassParameter> parameters;
		};

		const LiquidGlassGroup c_liquidGlassGroups[]
		{
			{ L"Refraction", {
				{ L"LiquidGlassBezel", L"Bezel width:", L"Width of the curved edge where the backdrop bends.", L"Bezel", 1.f, 200.f, 1.f, 26.f, L"px" },
				{ L"LiquidGlassThickness", L"Glass thickness:", L"How far the backdrop shifts under the bezel.", L"Thickness", 0.f, 200.f, 1.f, 26.f, L"px" },
				{ L"LiquidGlassScale", L"Refraction strength:", L"Multiplies the shift. 0 turns bending off.", L"Scale", 0.f, 5.f, 0.05f, 1.f, L"×" },
				{ L"LiquidGlassIor", L"Index of refraction:", L"How dense the glass is. Window glass is about 1.5.", L"Ior", 1.f, 3.f, 0.01f, 1.5f, L"" },
				{ L"LiquidGlassGap", L"Backdrop distance:", L"Extra depth that also shifts the flat area.", L"Gap", 0.f, 100.f, 1.f, 0.f, L"px" },
				{ L"LiquidGlassRadius", L"Corner radius:", L"Rounding of the glass corners.", L"Radius", 0.f, 60.f, 1.f, 8.f, L"px" },
			} },
			{ L"Blur", {
				{ L"LiquidGlassSoftness", L"Blur radius:", L"Blur of the backdrop seen through the glass. 0 is perfectly clear.", L"Softness", 0.f, 20.f, 0.5f, 1.5f, L"px" },
				{ L"LiquidGlassClarity", L"Blur mix:", L"1 uses the blur radius above. 0 falls back to the heavy OpenGlass blur.", L"Clarity", 0.f, 1.f, 0.01f, 0.75f, L"" },
			} },
			{ L"Color", {
				{ L"LiquidGlassChroma", L"Dispersion:", L"Red and blue split at the edges, like a prism.", L"Chroma", 0.f, 0.5f, 0.01f, 0.03f, L"" },
				{ L"LiquidGlassSaturation", L"Saturation:", L"0 is grayscale, 1 leaves colors unchanged.", L"Saturation", 0.f, 4.f, 0.05f, 1.4f, L"×" },
				{ L"LiquidGlassReadability", L"Text readability:", L"Darkens backdrops that are too bright for white text.", L"Readability", 0.f, 1.f, 0.01f, 0.5f, L"" },
			} },
			{ L"Rim light", {
				{ L"LiquidGlassRim", L"Rim light:", L"How brightly the edge catches the light.", L"Rim", 0.f, 5.f, 0.05f, 1.f, L"" },
				{ L"LiquidGlassShine", L"Highlight focus:", L"Higher values narrow the highlight toward the lit corner.", L"Shine", 0.1f, 16.f, 0.1f, 2.f, L"" },
				{ L"LiquidGlassLight", L"Light direction:", L"Angle of the light. -135 is from the top left.", L"Light", -180.f, 180.f, 1.f, -135.f, L"°" },
			} },
		};

		bool IsLightAngle(const LiquidGlassParameter& parameter)
		{
			return wcscmp(parameter.name, L"LiquidGlassLight") == 0;
		}

		DWORD EncodeLiquidGlassValue(const LiquidGlassParameter& parameter, float value)
		{
			if (IsLightAngle(parameter))
			{
				value = std::fmod(std::fmod(value, 360.f) + 360.f, 360.f) + 360.f;
			}
			return static_cast<DWORD>(std::lround(value * 100.f));
		}

		float DecodeLiquidGlassValue(const LiquidGlassParameter& parameter, DWORD raw)
		{
			float value = static_cast<float>(raw) / 100.f;
			if (IsLightAngle(parameter))
			{
				value -= 360.f;
				if (value > 180.f)
				{
					value -= 360.f;
				}
			}
			return std::clamp(value, parameter.minimum, parameter.maximum);
		}

		// The effective value: HKCU first, then HKLM, as OpenGlass reads it.
		DWORD ReadLiquidGlassDword(PCWSTR name, DWORD defaultValue)
		{
			wil::unique_hkey userKey{};
			wil::unique_hkey machineKey{};
			RegOpenKeyExW(HKEY_CURRENT_USER, DwmSettings::g_keyPath, 0, KEY_READ, userKey.put());
			RegOpenKeyExW(HKEY_LOCAL_MACHINE, DwmSettings::g_keyPath, 0, KEY_READ, machineKey.put());
			return DwmSettings::GetDword(userKey.get(), machineKey.get(), name, defaultValue);
		}

		void WriteLiquidGlassDword(PCWSTR name, DWORD value)
		{
			RegSetKeyValueW(HKEY_CURRENT_USER, DwmSettings::g_keyPath, name, REG_DWORD, &value, sizeof(value));
		}

		void NotifyLiquidGlassChange()
		{
			DwmSettings::NotifyChange(FindWindowW(L"Dwm", nullptr), DwmSettings::UpdateType::Backdrop);
		}

		wxString FormatLiquidGlassValue(const LiquidGlassParameter& parameter, float value)
		{
			const int decimals = parameter.step >= 1.f ? 0 : parameter.step >= 0.1f ? 1 : 2;
			wxString text = wxString::FromDouble(value, decimals);
			if (*parameter.unit)
			{
				text << L" " << parameter.unit;
			}
			return text;
		}

		struct LiquidGlassRow
		{
			const LiquidGlassParameter* parameter;
			wxSlider* slider;
			wxStaticText* value;
		};
	}

	void MainFrame::CreateLiquidGlassTab()
	{
		wxScrolledWindow* panel = new wxScrolledWindow(m_notebook);
		panel->SetScrollRate(5, 5);
		panel->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
		wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

		auto rows = std::make_shared<std::vector<LiquidGlassRow>>();
		auto pending = std::make_shared<std::map<std::wstring, DWORD>>();
		auto* applyTimer = new wxTimer(panel);

		wxStaticBoxSizer* generalGroup = new wxStaticBoxSizer(wxVERTICAL, panel, L"Liquid Glass");
		auto* note = new wxStaticText(panel, wxID_ANY,
			L"Refraction for the Direct3D renderer. Changes apply instantly and are saved to HKCU, so Save and Revert do not affect them.");
		note->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
		note->Wrap(FromDIP(560));
		generalGroup->Add(note, 0, wxEXPAND | wxALL, 4);

		auto* enabled = new wxCheckBox(panel, wxID_ANY, L"Enable Liquid Glass");
		enabled->SetToolTip(L"Turns the refraction effect on or off. Requires the Direct3D renderer.");
		generalGroup->Add(enabled, 0, wxALL, 4);

		wxChoice* profile{};
		{
			wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
			row->Add(new wxStaticText(panel, wxID_ANY, L"Bezel shape:", wxDefaultPosition, FromDIP(wxSize(160, -1))), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
			wxArrayString profiles;
			profiles.Add(L"Squircle (Apple-like, steep at the edge)");
			profiles.Add(L"Circle (even curve)");
			profiles.Add(L"Lip (raised, then hollow)");
			profile = new wxChoice(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, profiles);
			profile->SetToolTip(L"Squircle bends only close to the edge. Circle spreads the bend across the whole bezel.");
			row->Add(profile, 1, wxALIGN_CENTER_VERTICAL);
			generalGroup->Add(row, 0, wxEXPAND | wxALL, 4);
		}
		sizer->Add(generalGroup, 0, wxEXPAND | wxALL, 2);

		const wxColour hintColour = wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);
		wxFont hintFont = panel->GetFont();
		hintFont.SetFractionalPointSize(hintFont.GetFractionalPointSize() * 0.9);

		for (const auto& group : c_liquidGlassGroups)
		{
			wxStaticBoxSizer* groupSizer = new wxStaticBoxSizer(wxVERTICAL, panel, group.title);
			for (const auto& parameter : group.parameters)
			{
				wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
				auto* label = new wxStaticText(panel, wxID_ANY, parameter.label, wxDefaultPosition, FromDIP(wxSize(160, -1)));
				row->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);

				auto* slider = new NativeSlider(
					panel,
					wxID_ANY,
					static_cast<int>(std::lround(parameter.defaultValue / parameter.step)),
					static_cast<int>(std::lround(parameter.minimum / parameter.step)),
					static_cast<int>(std::lround(parameter.maximum / parameter.step))
				);
				row->Add(slider, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);

				auto* value = new wxStaticText(panel, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(64, -1)), wxALIGN_RIGHT | wxST_NO_AUTORESIZE);
				row->Add(value, 0, wxALIGN_CENTER_VERTICAL);

				label->SetToolTip(parameter.hint);
				slider->SetToolTip(parameter.hint);
				groupSizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 4);

				auto* hint = new wxStaticText(panel, wxID_ANY, parameter.hint);
				hint->SetForegroundColour(hintColour);
				hint->SetFont(hintFont);
				groupSizer->Add(hint, 0, wxLEFT | wxBOTTOM, FromDIP(8));

				rows->push_back({ &parameter, slider, value });
			}
			sizer->Add(groupSizer, 0, wxEXPAND | wxALL, 2);
		}

		wxBoxSizer* buttons = new wxBoxSizer(wxHORIZONTAL);
		auto* resetButton = new wxButton(panel, wxID_ANY, L"Reset to defaults");
		resetButton->SetToolTip(L"Write the default Liquid Glass values to HKCU and apply them.");
		auto* copyButton = new wxButton(panel, wxID_ANY, L"Copy values");
		copyButton->SetToolTip(L"Copy the current values as set-liquid-glass.ps1 arguments.");
		buttons->AddStretchSpacer();
		buttons->Add(resetButton, 0, wxRIGHT, 5);
		buttons->Add(copyButton, 0);
		sizer->Add(buttons, 0, wxEXPAND | wxALL, 6);

		auto showValue = [](const LiquidGlassRow& row)
		{
			row.value->SetLabel(FormatLiquidGlassValue(*row.parameter, static_cast<float>(row.slider->GetValue()) * row.parameter->step));
		};

		auto load = [rows, enabled, profile, showValue]()
		{
			for (auto& row : *rows)
			{
				const auto& parameter = *row.parameter;
				const float value = DecodeLiquidGlassValue(parameter, ReadLiquidGlassDword(parameter.name, EncodeLiquidGlassValue(parameter, parameter.defaultValue)));
				row.slider->SetValue(static_cast<int>(std::lround(value / parameter.step)));
				showValue(row);
			}
			enabled->SetValue(ReadLiquidGlassDword(L"LiquidGlass", 0) != 0);
			profile->SetSelection(static_cast<int>(std::min(ReadLiquidGlassDword(L"LiquidGlassProfile", 0), 2ul)));
		};
		load();

		for (auto& row : *rows)
		{
			row.slider->Bind(wxEVT_SLIDER, [row, pending, applyTimer, showValue](wxCommandEvent&)
			{
				showValue(row);
				(*pending)[row.parameter->name] = EncodeLiquidGlassValue(*row.parameter, static_cast<float>(row.slider->GetValue()) * row.parameter->step);
				applyTimer->StartOnce(100);
			});
		}

		panel->Bind(wxEVT_TIMER, [this, pending](wxTimerEvent&)
		{
			for (const auto& [name, value] : *pending)
			{
				WriteLiquidGlassDword(name.c_str(), value);
			}
			pending->clear();
			NotifyLiquidGlassChange();
			SetStatusText(L"Liquid Glass: applied " + wxDateTime::Now().FormatISOTime());
		}, applyTimer->GetId());

		enabled->Bind(wxEVT_CHECKBOX, [this, enabled](wxCommandEvent&)
		{
			WriteLiquidGlassDword(L"LiquidGlass", enabled->GetValue() ? 1 : 0);
			NotifyLiquidGlassChange();
			SetStatusText(enabled->GetValue() ? L"Liquid Glass: on" : L"Liquid Glass: off");
		});

		profile->Bind(wxEVT_CHOICE, [this, profile](wxCommandEvent&)
		{
			WriteLiquidGlassDword(L"LiquidGlassProfile", static_cast<DWORD>(profile->GetSelection()));
			NotifyLiquidGlassChange();
			SetStatusText(L"Liquid Glass: bezel shape applied");
		});

		resetButton->Bind(wxEVT_BUTTON, [this, rows, load](wxCommandEvent&)
		{
			for (const auto& row : *rows)
			{
				WriteLiquidGlassDword(row.parameter->name, EncodeLiquidGlassValue(*row.parameter, row.parameter->defaultValue));
			}
			WriteLiquidGlassDword(L"LiquidGlassProfile", 0);
			NotifyLiquidGlassChange();
			load();
			SetStatusText(L"Liquid Glass: defaults applied");
		});

		copyButton->Bind(wxEVT_BUTTON, [this, rows, profile](wxCommandEvent&)
		{
			wxString text;
			for (const auto& row : *rows)
			{
				text << L"-" << row.parameter->option << L" "
					<< wxString::FromCDouble(static_cast<double>(row.slider->GetValue()) * row.parameter->step) << L" ";
			}
			static constexpr PCWSTR profiles[]{ L"squircle", L"circle", L"lip" };
			text << L"-Profile " << profiles[std::clamp(profile->GetSelection(), 0, 2)];
			if (wxTheClipboard->Open())
			{
				wxTheClipboard->SetData(new wxTextDataObject(text));
				wxTheClipboard->Close();
				SetStatusText(L"Liquid Glass: values copied");
			}
		});

		panel->SetSizer(sizer);
		m_notebook->AddPage(panel, L"Liquid Glass");
	}
}
