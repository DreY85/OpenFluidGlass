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

		// Apple publishes no numbers for Liquid Glass. These are estimates from how its two variants look:
		// Clear is nearly unblurred with a bright rim, Regular is frosted with richer color.
		struct LiquidGlassPreset
		{
			PCWSTR label;
			PCWSTR hint;
			std::vector<std::pair<PCWSTR, float>> values;	// parameters left out take their default
		};
		const LiquidGlassPreset c_liquidGlassPresets[]
		{
			{
				L"Apple Clear", L"Almost no blur, strong bending at the edge, bright rim.",
				{
					{ L"LiquidGlassBezel", 30.f }, { L"LiquidGlassThickness", 40.f }, { L"LiquidGlassSoftness", 0.5f },
					{ L"LiquidGlassClarity", 1.f }, { L"LiquidGlassChroma", 0.02f }, { L"LiquidGlassSaturation", 1.2f },
					{ L"LiquidGlassReadability", 0.35f }, { L"LiquidGlassRim", 1.4f }, { L"LiquidGlassShine", 3.f }
				}
			},
			{
				L"Apple Regular", L"Frosted glass with richer color, softer bending.",
				{
					{ L"LiquidGlassThickness", 30.f }, { L"LiquidGlassSoftness", 4.f }, { L"LiquidGlassClarity", 0.55f },
					{ L"LiquidGlassChroma", 0.01f }, { L"LiquidGlassSaturation", 1.8f }, { L"LiquidGlassReadability", 0.6f },
					{ L"LiquidGlassShine", 2.5f }
				}
			}
		};

		float GetPresetValue(const LiquidGlassPreset& preset, const LiquidGlassParameter& parameter)
		{
			for (const auto& [name, value] : preset.values)
			{
				if (wcscmp(name, parameter.name) == 0)
				{
					return value;
				}
			}
			return parameter.defaultValue;
		}

		// User presets live under HKCU, one subkey per name holding the same DWORDs as the DWM key.
		constexpr PCWSTR c_userPresetsKey = L"Software\\OpenGlass\\LiquidGlassPresets";

		std::wstring UserPresetPath(const wxString& name)
		{
			return std::wstring(c_userPresetsKey) + L"\\" + name.ToStdWstring();
		}

		wxArrayString ListUserPresets()
		{
			wxArrayString names;
			wil::unique_hkey key;
			if (RegOpenKeyExW(HKEY_CURRENT_USER, c_userPresetsKey, 0, KEY_READ, key.put()) != ERROR_SUCCESS)
			{
				return names;
			}
			wchar_t name[256];
			for (DWORD index = 0;; ++index)
			{
				DWORD length = ARRAYSIZE(name);
				if (RegEnumKeyExW(key.get(), index, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
				{
					break;
				}
				names.Add(name);
			}
			names.Sort();
			return names;
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

		auto* generalGroup = ModernTheme::MakeCard(panel, L"Liquid Glass");
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

		std::vector<std::pair<wxButton*, const LiquidGlassPreset*>> presetButtons;
		{
			wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
			row->Add(new wxStaticText(panel, wxID_ANY, L"Presets:", wxDefaultPosition, FromDIP(wxSize(160, -1))), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
			for (const auto& preset : c_liquidGlassPresets)
			{
				auto* button = new wxButton(panel, wxID_ANY, preset.label);
				button->SetToolTip(preset.hint);
				row->Add(button, 0, wxRIGHT, 5);
				presetButtons.emplace_back(button, &preset);
			}
			generalGroup->Add(row, 0, wxEXPAND | wxALL, 4);

			auto* presetNote = new wxStaticText(panel, wxID_ANY,
				L"Apple publishes no values, so these are estimates to start from. Fine-tune them with the sliders below.");
			presetNote->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
			presetNote->Wrap(FromDIP(560));
			generalGroup->Add(presetNote, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);
		}

		wxChoice* userPresets{};
		wxButton* applyUserPreset{};
		wxButton* saveUserPreset{};
		wxButton* deleteUserPreset{};
		{
			wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
			row->Add(new wxStaticText(panel, wxID_ANY, L"My presets:", wxDefaultPosition, FromDIP(wxSize(160, -1))), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
			userPresets = new wxChoice(panel, wxID_ANY);
			userPresets->SetToolTip(L"Presets you saved with Save as. They are stored for your account.");
			row->Add(userPresets, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
			applyUserPreset = new wxButton(panel, wxID_ANY, L"Apply");
			saveUserPreset = new wxButton(panel, wxID_ANY, L"Save as...");
			saveUserPreset->SetToolTip(L"Save the current values and bezel shape under a name you choose.");
			deleteUserPreset = new wxButton(panel, wxID_ANY, L"Delete");
			row->Add(applyUserPreset, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
			row->Add(saveUserPreset, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
			row->Add(deleteUserPreset, 0, wxALIGN_CENTER_VERTICAL);
			generalGroup->Add(row, 0, wxEXPAND | wxALL, 4);
		}
		sizer->Add(generalGroup, 0, wxEXPAND | wxALL, 2);

		const wxColour hintColour = wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);
		wxFont hintFont = panel->GetFont();
		hintFont.SetFractionalPointSize(hintFont.GetFractionalPointSize() * 0.9);

		for (const auto& group : c_liquidGlassGroups)
		{
			auto* groupSizer = ModernTheme::MakeCard(panel, group.title);
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

		for (const auto& [button, preset] : presetButtons)
		{
			button->Bind(wxEVT_BUTTON, [this, rows, load, preset](wxCommandEvent&)
			{
				for (const auto& row : *rows)
				{
					WriteLiquidGlassDword(row.parameter->name, EncodeLiquidGlassValue(*row.parameter, GetPresetValue(*preset, *row.parameter)));
				}
				WriteLiquidGlassDword(L"LiquidGlassProfile", 0);
				NotifyLiquidGlassChange();
				load();
				SetStatusText(wxString(L"Liquid Glass: ") + preset->label + L" applied");
			});
		}

		auto refreshUserPresets = [userPresets, applyUserPreset, deleteUserPreset](const wxString& select)
		{
			userPresets->Set(ListUserPresets());
			const int index = select.empty() ? (userPresets->IsEmpty() ? wxNOT_FOUND : 0) : userPresets->FindString(select, true);
			userPresets->SetSelection(index);
			applyUserPreset->Enable(index != wxNOT_FOUND);
			deleteUserPreset->Enable(index != wxNOT_FOUND);
		};
		refreshUserPresets(wxEmptyString);

		saveUserPreset->Bind(wxEVT_BUTTON, [this, rows, profile, refreshUserPresets](wxCommandEvent&)
		{
			wxTextEntryDialog dialog(this, L"Name for this preset:", L"Save Liquid Glass preset");
			if (dialog.ShowModal() != wxID_OK)
			{
				return;
			}
			const wxString name = dialog.GetValue().Strip(wxString::both);
			if (name.empty() || name.length() > 64 || name.find_first_of(L"\\/") != wxString::npos)
			{
				wxMessageBox(L"Use a name of 1 to 64 characters without \\ or /.", L"Save Liquid Glass preset", wxOK | wxICON_WARNING, this);
				return;
			}
			if (ListUserPresets().Index(name, false) != wxNOT_FOUND
				&& wxMessageBox(L"A preset named \"" + name + L"\" already exists. Replace it?", L"Save Liquid Glass preset", wxYES_NO | wxICON_QUESTION, this) != wxYES)
			{
				return;
			}
			wil::unique_hkey key;
			if (RegCreateKeyExW(HKEY_CURRENT_USER, UserPresetPath(name).c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, key.put(), nullptr) != ERROR_SUCCESS)
			{
				SetStatusText(L"Liquid Glass: the preset could not be saved");
				return;
			}
			for (const auto& row : *rows)
			{
				const DWORD value = EncodeLiquidGlassValue(*row.parameter, static_cast<float>(row.slider->GetValue()) * row.parameter->step);
				RegSetValueExW(key.get(), row.parameter->name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
			}
			const DWORD shape = static_cast<DWORD>(std::max(profile->GetSelection(), 0));
			RegSetValueExW(key.get(), L"LiquidGlassProfile", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&shape), sizeof(shape));
			refreshUserPresets(name);
			SetStatusText(L"Liquid Glass: saved preset " + name);
		});

		applyUserPreset->Bind(wxEVT_BUTTON, [this, rows, load, userPresets](wxCommandEvent&)
		{
			const wxString name = userPresets->GetStringSelection();
			wil::unique_hkey key;
			if (name.empty() || RegOpenKeyExW(HKEY_CURRENT_USER, UserPresetPath(name).c_str(), 0, KEY_READ, key.put()) != ERROR_SUCCESS)
			{
				SetStatusText(L"Liquid Glass: the preset could not be read");
				return;
			}
			const auto read = [&key](PCWSTR value, DWORD fallback)
			{
				DWORD data{}, size = sizeof(data), type{};
				return RegQueryValueExW(key.get(), value, nullptr, &type, reinterpret_cast<BYTE*>(&data), &size) == ERROR_SUCCESS && type == REG_DWORD ? data : fallback;
			};
			for (const auto& row : *rows)
			{
				WriteLiquidGlassDword(row.parameter->name, read(row.parameter->name, EncodeLiquidGlassValue(*row.parameter, row.parameter->defaultValue)));
			}
			WriteLiquidGlassDword(L"LiquidGlassProfile", std::min(read(L"LiquidGlassProfile", 0), 2ul));
			NotifyLiquidGlassChange();
			load();
			SetStatusText(L"Liquid Glass: " + name + L" applied");
		});

		deleteUserPreset->Bind(wxEVT_BUTTON, [this, userPresets, refreshUserPresets](wxCommandEvent&)
		{
			const wxString name = userPresets->GetStringSelection();
			if (name.empty()
				|| wxMessageBox(L"Delete the preset \"" + name + L"\"?", L"Delete Liquid Glass preset", wxYES_NO | wxICON_QUESTION, this) != wxYES)
			{
				return;
			}
			RegDeleteTreeW(HKEY_CURRENT_USER, UserPresetPath(name).c_str());
			refreshUserPresets(wxEmptyString);
			SetStatusText(L"Liquid Glass: deleted preset " + name);
		});

		userPresets->Bind(wxEVT_CHOICE, [userPresets, applyUserPreset, deleteUserPreset](wxCommandEvent&)
		{
			applyUserPreset->Enable(userPresets->GetSelection() != wxNOT_FOUND);
			deleteUserPreset->Enable(userPresets->GetSelection() != wxNOT_FOUND);
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
