#include "pch.h"
#include "MainFrame.hpp"
#include "UiControls.hpp"
#include "DwmSettings.hpp"
#include <wx/clipbrd.h>

// Fluid Glass tab: refraction settings for the Direct3D renderer.
// Unlike the other tabs these values are live. Every change is written to HKCU and DWM reloads at once,
// so they stay outside Save and Revert. OpenGlass stores them as DWORDs holding the value times 100;
// the light angle is stored with 360 degrees added.

namespace OpenGlass
{
	namespace
	{
		struct FluidGlassParameter
		{
			PCWSTR name;
			PCWSTR label;
			PCWSTR hint;
			PCWSTR option;	// set-fluid-glass.ps1 parameter, used by Copy values
			float minimum;
			float maximum;
			float step;
			float defaultValue;
			PCWSTR unit;
		};

		struct FluidGlassGroup
		{
			PCWSTR title;
			std::initializer_list<FluidGlassParameter> parameters;
		};

		const FluidGlassGroup c_fluidGlassGroups[]
		{
			{ L"Refraction", {
				{ L"FluidGlassBezel", L"Bezel width:", L"Width of the curved edge where the backdrop bends.", L"Bezel", 1.f, 200.f, 1.f, 26.f, L"px" },
				{ L"FluidGlassThickness", L"Glass thickness:", L"How far the backdrop shifts under the bezel.", L"Thickness", 0.f, 200.f, 1.f, 26.f, L"px" },
				{ L"FluidGlassScale", L"Refraction strength:", L"Multiplies the shift. 0 turns bending off.", L"Scale", 0.f, 5.f, 0.05f, 1.f, L"×" },
				{ L"FluidGlassIor", L"Index of refraction:", L"How dense the glass is. Window glass is about 1.5.", L"Ior", 1.f, 3.f, 0.01f, 1.5f, L"" },
				{ L"FluidGlassGap", L"Backdrop distance:", L"Extra depth that also shifts the flat area.", L"Gap", 0.f, 100.f, 1.f, 0.f, L"px" },
				{ L"FluidGlassRadius", L"Corner radius:", L"Rounding of the glass corners.", L"Radius", 0.f, 60.f, 1.f, 8.f, L"px" },
			} },
			{ L"Blur", {
				{ L"FluidGlassSoftness", L"Blur radius:", L"Blur of the backdrop seen through the glass. 0 is perfectly clear.", L"Softness", 0.f, 20.f, 0.5f, 1.5f, L"px" },
				{ L"FluidGlassClarity", L"Clarity:", L"How much clear view, softened by the radius above, shows over the heavy blur.", L"Clarity", 0.f, 1.f, 0.01f, 0.75f, L"" },
			} },
			{ L"Color", {
				{ L"FluidGlassChroma", L"Dispersion:", L"Red and blue split at the edges, like a prism.", L"Chroma", 0.f, 0.5f, 0.01f, 0.03f, L"" },
				{ L"FluidGlassSaturation", L"Saturation:", L"0 is grayscale, 1 leaves colors unchanged.", L"Saturation", 0.f, 4.f, 0.05f, 1.4f, L"×" },
				{ L"FluidGlassReadability", L"Text readability:", L"Darkens backdrops that are too bright for white text.", L"Readability", 0.f, 1.f, 0.01f, 0.5f, L"" },
				{ L"FluidGlassReadabilityLight", L"Readability in light mode:", L"When apps use light mode, lightens backdrops that are too dark for black text. Over a dark wallpaper, apps in dark mode keep the glass clearer.", L"ReadabilityLight", 0.f, 1.f, 0.01f, 0.8f, L"" },
				{ L"FluidGlassGrayTextStrength", L"Text halo in light mode:", L"When apps use light mode, draws a soft white halo behind the black and grey names in File Explorer so they stay readable on the glass. 0 turns it off.", L"TextHalo", 0.f, 1.f, 0.01f, 0.6f, L"" },
				{ L"FluidGlassGrayTextSize", L"Halo size in light mode:", L"How far the text halo reaches past the letters. 3 is a tight outline, larger values give a wide soft glow like the Windows 7 title bars and can lighten nearby icons.", L"HaloSize", 1.f, 10.f, 1.f, 3.f, L"px" },
				{ L"FluidGlassTextGlow", L"Text glow in dark mode:", L"When apps use dark mode, gives the light text in File Explorer a soft glow in its own color, like neon. 0 turns it off.", L"TextGlow", 0.f, 1.f, 0.01f, 0.f, L"" },
				{ L"FluidGlassTextGlowSize", L"Glow size in dark mode:", L"How far the text glow reaches past the letters. Larger values give a wider, softer light.", L"GlowSize", 1.f, 10.f, 1.f, 4.f, L"px" },
			} },
			{ L"Rim light", {
				{ L"FluidGlassRim", L"Rim light:", L"How brightly the edge catches the light.", L"Rim", 0.f, 5.f, 0.05f, 1.f, L"" },
				{ L"FluidGlassShine", L"Highlight focus:", L"Higher values narrow the highlight toward the lit corner.", L"Shine", 0.1f, 16.f, 0.1f, 2.f, L"" },
				{ L"FluidGlassLight", L"Light direction:", L"Angle of the light. -135 is from the top left.", L"Light", -180.f, 180.f, 1.f, -135.f, L"°" },
			} },
		};

		bool IsLightAngle(const FluidGlassParameter& parameter)
		{
			return wcscmp(parameter.name, L"FluidGlassLight") == 0;
		}

		DWORD EncodeFluidGlassValue(const FluidGlassParameter& parameter, float value)
		{
			if (IsLightAngle(parameter))
			{
				value = std::fmod(std::fmod(value, 360.f) + 360.f, 360.f) + 360.f;
			}
			return static_cast<DWORD>(std::lround(value * 100.f));
		}

		float DecodeFluidGlassValue(const FluidGlassParameter& parameter, DWORD raw)
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
		DWORD ReadFluidGlassDword(PCWSTR name, DWORD defaultValue)
		{
			wil::unique_hkey userKey{};
			wil::unique_hkey machineKey{};
			RegOpenKeyExW(HKEY_CURRENT_USER, DwmSettings::g_keyPath, 0, KEY_READ, userKey.put());
			RegOpenKeyExW(HKEY_LOCAL_MACHINE, DwmSettings::g_keyPath, 0, KEY_READ, machineKey.put());
			return DwmSettings::GetDword(userKey.get(), machineKey.get(), name, defaultValue);
		}

		void WriteFluidGlassDword(PCWSTR name, DWORD value)
		{
			RegSetKeyValueW(HKEY_CURRENT_USER, DwmSettings::g_keyPath, name, REG_DWORD, &value, sizeof(value));
		}

		void NotifyFluidGlassChange()
		{
			DwmSettings::NotifyChange(FindWindowW(L"Dwm", nullptr), DwmSettings::UpdateType::Backdrop);
		}

		wxString FormatFluidGlassValue(const FluidGlassParameter& parameter, float value)
		{
			const int decimals = parameter.step >= 1.f ? 0 : parameter.step >= 0.1f ? 1 : 2;
			wxString text = wxString::FromDouble(value, decimals);
			if (*parameter.unit)
			{
				text << L" " << parameter.unit;
			}
			return text;
		}

		// Apple publishes no numbers for Fluid Glass. These are estimates from how its two variants look:
		// Clear is nearly unblurred with a bright rim, Regular is frosted with richer color.
		struct FluidGlassPreset
		{
			PCWSTR label;
			PCWSTR hint;
			std::vector<std::pair<PCWSTR, float>> values;	// parameters left out take their default
		};
		const FluidGlassPreset c_fluidGlassPresets[]
		{
			{
				L"Apple Clear", L"Almost no blur, strong bending at the edge, bright rim.",
				{
					{ L"FluidGlassBezel", 30.f }, { L"FluidGlassThickness", 40.f }, { L"FluidGlassSoftness", 0.5f },
					{ L"FluidGlassClarity", 1.f }, { L"FluidGlassChroma", 0.02f }, { L"FluidGlassSaturation", 1.2f },
					{ L"FluidGlassReadability", 0.35f }, { L"FluidGlassRim", 1.4f }, { L"FluidGlassShine", 3.f }
				}
			},
			{
				L"Apple Regular", L"Frosted glass with richer color, softer bending.",
				{
					{ L"FluidGlassThickness", 30.f }, { L"FluidGlassSoftness", 4.f }, { L"FluidGlassClarity", 0.55f },
					{ L"FluidGlassChroma", 0.01f }, { L"FluidGlassSaturation", 1.8f }, { L"FluidGlassReadability", 0.6f },
					{ L"FluidGlassShine", 2.5f }
				}
			}
		};

		float GetPresetValue(const FluidGlassPreset& preset, const FluidGlassParameter& parameter)
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
		constexpr PCWSTR c_userPresetsKey = L"Software\\OpenGlass\\FluidGlassPresets";

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

		struct FluidGlassRow
		{
			const FluidGlassParameter* parameter;
			wxSlider* slider;
			wxStaticText* value;
		};
	}

	void MainFrame::CreateFluidGlassTab()
	{
		wxScrolledWindow* panel = new TabPage(m_notebook);
		panel->SetScrollRate(5, 5);
		panel->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
		wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

		auto rows = std::make_shared<std::vector<FluidGlassRow>>();
		auto pending = std::make_shared<std::map<std::wstring, DWORD>>();
		auto* applyTimer = PanelTimer(panel);

		auto* generalGroup = ModernTheme::MakeCard(panel, L"Fluid Glass");
		auto* note = new wxStaticText(panel, wxID_ANY,
			L"Refraction for the Direct3D renderer. Changes apply instantly and are saved to HKCU, so Save and Revert do not affect them.");
		note->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
		note->Wrap(FromDIP(560));
		generalGroup->Add(note, 0, wxEXPAND | wxALL, 4);

		auto* enabled = new wxCheckBox(panel, wxID_ANY, L"Enable Fluid Glass");
		enabled->SetToolTip(L"Turns the refraction effect on or off. Requires the Direct3D renderer.");
		generalGroup->Add(enabled, 0, wxALL, 4);

		auto* items = new wxCheckBox(panel, wxID_ANY, L"Glass hover and selection in File Explorer");
		items->SetToolTip(L"Replaces the grey hover, selection and tooltips in File Explorer with dark rounded glass tinted with the accent color.");
		generalGroup->Add(items, 0, wxALL, 4);

		auto* dialogs = new wxCheckBox(panel, wxID_ANY, L"Also in the Open and Save dialogs of other apps");
		dialogs->SetToolTip(L"Loads the same glass into any app that opens a file dialog. It runs inside that app, so leave it off if an app or game complains about injected code.");
		generalGroup->Add(dialogs, 0, wxLEFT | wxRIGHT | wxBOTTOM, 4);

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

		std::vector<std::pair<wxButton*, const FluidGlassPreset*>> presetButtons;
		{
			wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
			row->Add(new wxStaticText(panel, wxID_ANY, L"Presets:", wxDefaultPosition, FromDIP(wxSize(160, -1))), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
			for (const auto& preset : c_fluidGlassPresets)
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

		// These only act while apps use light mode. The window is rebuilt when Windows changes mode, so this check is enough.
		const bool darkApps = ModernTheme::Get().dark;
		auto lightOnly = [](const FluidGlassParameter& parameter)
		{
			return wcscmp(parameter.name, L"FluidGlassReadabilityLight") == 0 || wcscmp(parameter.name, L"FluidGlassGrayTextStrength") == 0
				|| wcscmp(parameter.name, L"FluidGlassGrayTextSize") == 0;
		};
		auto darkOnly = [](const FluidGlassParameter& parameter)
		{
			return wcscmp(parameter.name, L"FluidGlassTextGlow") == 0 || wcscmp(parameter.name, L"FluidGlassTextGlowSize") == 0;
		};
		for (const auto& group : c_fluidGlassGroups)
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

				if (darkApps ? lightOnly(parameter) : darkOnly(parameter))
				{
					const wxString locked = wxString(parameter.hint) + (darkApps ? L" Locked while Windows uses dark mode for apps." : L" Locked while Windows uses light mode for apps.");
					for (wxWindow* window : { static_cast<wxWindow*>(label), static_cast<wxWindow*>(slider), static_cast<wxWindow*>(value), static_cast<wxWindow*>(hint) })
					{
						window->Disable();
						window->SetToolTip(locked);
					}
				}

				rows->push_back({ &parameter, slider, value });
			}
			sizer->Add(groupSizer, 0, wxEXPAND | wxALL, 2);
		}

		// The user's colours for File Explorer's text and icon glyphs, one per mode. Explorer reads them about once a
		// second. A set colour is stored as 0x01000000 | COLORREF, so black stays possible, and 0 means the system's.
		// Light mode sits on the left and dark mode on the right. Each column has a palette list that fills its three
		// colours at once. The rows are stored light first: text, details, icons, then the same for dark mode.
		struct ColorRow { PCWSTR name; wxChoice* choice; wxColourPickerCtrl* picker; };
		struct Palette { PCWSTR name; unsigned colors[3]; };	// 0xRRGGBB for text, details, icons
		static constexpr Palette lightPalettes[]
		{
			{ L"Slate", { 0x1E293B, 0x64748B, 0x334155 } },
			{ L"Ocean", { 0x1A2B4C, 0x5A6E8F, 0x2563EB } },
			{ L"Indigo", { 0x1E1B4B, 0x6366A0, 0x4F46E5 } },
			{ L"Teal", { 0x134E4A, 0x5B7F7C, 0x0D9488 } },
			{ L"Forest", { 0x1F3A2E, 0x5E7D6B, 0x2F855A } },
			{ L"Olive", { 0x2F3A1F, 0x7A8065, 0x6B8E23 } },
			{ L"Sepia", { 0x3B2F2F, 0x8A7560, 0xA0522D } },
			{ L"Wine", { 0x4A1D24, 0x8C6A6F, 0xB23A48 } },
			{ L"Plum", { 0x3B1F3A, 0x7D6380, 0x8E44AD } },
			{ L"Graphite", { 0x202124, 0x6B6B6B, 0x3C4043 } },
		};
		static constexpr Palette darkPalettes[]
		{
			{ L"Slate", { 0xF1F5F9, 0x94A3B8, 0xCBD5E1 } },
			{ L"Ocean", { 0xE6F0FF, 0x9FB3D1, 0x7DB4FF } },
			{ L"Indigo", { 0xEEF0FF, 0xA5A9D6, 0x818CF8 } },
			{ L"Teal", { 0xE0F7F5, 0x98BDB9, 0x5EEAD4 } },
			{ L"Forest", { 0xE8F5EC, 0x9DBBA8, 0x6FCF97 } },
			{ L"Olive", { 0xF1F3E6, 0xB2B79A, 0xA8C66C } },
			{ L"Sepia", { 0xF5EDE3, 0xBFAE98, 0xE0A46E } },
			{ L"Wine", { 0xFBEAEC, 0xC9A5AA, 0xF28B9B } },
			{ L"Plum", { 0xF3EAF7, 0xB8A5C4, 0xC39BD3 } },
			{ L"Graphite", { 0xF2F2F2, 0xA0A0A0, 0xD0D0D0 } },
		};
		struct PaletteList { const Palette* items; size_t count; };
		static constexpr PaletteList paletteLists[2]{ { lightPalettes, std::size(lightPalettes) }, { darkPalettes, std::size(darkPalettes) } };
		auto toColour = [](unsigned value) { return wxColour((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF); };
		auto colorRows = std::make_shared<std::vector<ColorRow>>();
		wxButton* systemColorsButton{};
		auto paletteChoices = std::make_shared<std::vector<wxChoice*>>();
		// Shows the palette the three colours of a mode match, or the prompt when they are a mix of the user's own.
		auto syncPalettes = [colorRows, paletteChoices, toColour]()
		{
			for (size_t mode = 0; mode < paletteChoices->size(); ++mode)
			{
				int match = 0;
				for (size_t index = 0; index < paletteLists[mode].count && !match; ++index)
				{
					bool same = true;
					for (size_t slot = 0; slot < 3 && same; ++slot)
					{
						const ColorRow& row = (*colorRows)[mode * 3 + slot];
						same = row.choice->GetSelection() == 1 && row.picker->GetColour() == toColour(paletteLists[mode].items[index].colors[slot]);
					}
					if (same) match = static_cast<int>(index) + 1;
				}
				(*paletteChoices)[mode]->SetSelection(match);
			}
		};
		{
			auto* colorGroup = ModernTheme::MakeCard(panel, L"File Explorer colors");
			auto* colorNote = new wxStaticText(panel, wxID_ANY,
				L"Your own color for the text and the icons of File Explorer: the file list, the navigation pane, the address bar, the tabs and "
				L"the command bar. Details are the softer grey text: descriptions, sizes, dates and hints. File icons keep their pictures. "
				L"Colored names, such as compressed or encrypted files, keep their color.");
			colorNote->SetForegroundColour(hintColour);
			colorNote->SetFont(hintFont);
			colorNote->Wrap(FromDIP(560));
			wxBoxSizer* noteRow = new wxBoxSizer(wxHORIZONTAL);
			noteRow->Add(colorNote, 1, wxALIGN_TOP | wxRIGHT, FromDIP(12));
			systemColorsButton = new wxButton(panel, wxID_ANY, L"Use System Colors");
			systemColorsButton->SetToolTip(L"Sets all six File Explorer colors back to the system's. Other Fluid Glass settings stay as they are.");
			noteRow->Add(systemColorsButton, 0, wxALIGN_TOP);
			colorGroup->Add(noteRow, 0, wxEXPAND | wxALL, 4);
			const struct { PCWSTR title; PCWSTR names[3]; unsigned initial[3]; } modes[]
			{
				{ L"Light mode", { L"FluidGlassTextColorLight", L"FluidGlassSecondaryTextColorLight", L"FluidGlassIconColorLight" }, { 0x000000, 0x6D6D6D, 0x000000 } },
				{ L"Dark mode", { L"FluidGlassTextColorDark", L"FluidGlassSecondaryTextColorDark", L"FluidGlassIconColorDark" }, { 0xFFFFFF, 0xC5C5C5, 0xFFFFFF } },
			};
			const PCWSTR labels[3]{ L"Text:", L"Details:", L"Icons:" };
			wxBoxSizer* columns = new wxBoxSizer(wxHORIZONTAL);
			for (size_t mode = 0; mode < std::size(modes); ++mode)
			{
				wxBoxSizer* column = new wxBoxSizer(wxVERTICAL);
				auto* title = new wxStaticText(panel, wxID_ANY, modes[mode].title);
				title->SetFont(title->GetFont().Bold());
				column->Add(title, 0, wxALL, 4);

				wxBoxSizer* paletteRow = new wxBoxSizer(wxHORIZONTAL);
				paletteRow->Add(new wxStaticText(panel, wxID_ANY, L"Palette:", wxDefaultPosition, FromDIP(wxSize(70, -1))), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
				auto* palette = new wxChoice(panel, wxID_ANY);
				palette->Append(L"Choose a palette...");
				for (size_t index = 0; index < paletteLists[mode].count; ++index) palette->Append(paletteLists[mode].items[index].name);
				palette->SetToolTip(L"Fills the text, details and icon colors of this mode with a matching set and applies it.");
				paletteRow->Add(palette, 1, wxALIGN_CENTER_VERTICAL);
				column->Add(paletteRow, 0, wxEXPAND | wxALL, 4);
				paletteChoices->push_back(palette);

				for (size_t slot = 0; slot < 3; ++slot)
				{
					wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
					row->Add(new wxStaticText(panel, wxID_ANY, labels[slot], wxDefaultPosition, FromDIP(wxSize(70, -1))), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
					auto* choice = new wxChoice(panel, wxID_ANY);
					choice->Append(L"System");
					choice->Append(L"Custom");
					row->Add(choice, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
					auto* picker = new wxColourPickerCtrl(panel, wxID_ANY, toColour(modes[mode].initial[slot]));
					row->Add(picker, 0, wxALIGN_CENTER_VERTICAL);
					column->Add(row, 0, wxEXPAND | wxALL, 4);
					colorRows->push_back({ modes[mode].names[slot], choice, picker });
				}
				columns->Add(column, 1, wxEXPAND | (mode == 0 ? wxRIGHT : 0), FromDIP(24));
			}
			colorGroup->Add(columns, 0, wxEXPAND | wxALL, 0);
			sizer->Add(colorGroup, 0, wxEXPAND | wxALL, 2);
		}
		auto loadColors = [colorRows, syncPalettes]()
		{
			for (const auto& row : *colorRows)
			{
				const DWORD value = ReadFluidGlassDword(row.name, 0);
				const bool custom = (value & 0xFF000000) == 0x01000000;
				row.choice->SetSelection(custom ? 1 : 0);
				if (custom) row.picker->SetColour(wxColour(GetRValue(value), GetGValue(value), GetBValue(value)));
				row.picker->Enable(custom);
			}
			syncPalettes();
		};
		loadColors();
		for (const auto& row : *colorRows)
		{
			auto update = [this, row, syncPalettes]()
			{
				const bool custom = row.choice->GetSelection() == 1;
				row.picker->Enable(custom);
				const wxColour colour = row.picker->GetColour();
				WriteFluidGlassDword(row.name, custom ? 0x01000000 | RGB(colour.Red(), colour.Green(), colour.Blue()) : 0);
				syncPalettes();
				SetStatusText(L"File Explorer colors: applied " + wxDateTime::Now().FormatISOTime());
			};
			row.choice->Bind(wxEVT_CHOICE, [update](wxCommandEvent&) { update(); });
			row.picker->Bind(wxEVT_COLOURPICKER_CHANGED, [update](wxColourPickerEvent&) { update(); });
		}
		systemColorsButton->Bind(wxEVT_BUTTON, [this, colorRows, loadColors](wxCommandEvent&)
		{
			for (const auto& row : *colorRows) WriteFluidGlassDword(row.name, 0);
			loadColors();
			SetStatusText(L"File Explorer colors: system colors applied " + wxDateTime::Now().FormatISOTime());
		});
		for (size_t mode = 0; mode < paletteChoices->size(); ++mode)
		{
			(*paletteChoices)[mode]->Bind(wxEVT_CHOICE, [this, mode, colorRows, paletteChoices, toColour](wxCommandEvent&)
			{
				const int selection = (*paletteChoices)[mode]->GetSelection();
				if (selection < 1 || static_cast<size_t>(selection) > paletteLists[mode].count) return;
				const Palette& palette = paletteLists[mode].items[selection - 1];
				for (size_t slot = 0; slot < 3; ++slot)
				{
					const ColorRow& row = (*colorRows)[mode * 3 + slot];
					const wxColour colour = toColour(palette.colors[slot]);
					row.choice->SetSelection(1);
					row.picker->SetColour(colour);
					row.picker->Enable(true);
					WriteFluidGlassDword(row.name, 0x01000000 | RGB(colour.Red(), colour.Green(), colour.Blue()));
				}
				SetStatusText(wxString::Format(L"File Explorer colors: %s palette applied %s", palette.name, wxDateTime::Now().FormatISOTime()));
			});
		}

		wxBoxSizer* buttons = new wxBoxSizer(wxHORIZONTAL);
		auto* resetButton = new wxButton(panel, wxID_ANY, L"Reset to defaults");
		resetButton->SetToolTip(L"Write the default value of every Fluid Glass setting on this page to HKCU and apply them, File Explorer colors included.");
		auto* copyButton = new wxButton(panel, wxID_ANY, L"Copy values");
		copyButton->SetToolTip(L"Copy the current values as set-fluid-glass.ps1 arguments.");
		buttons->AddStretchSpacer();
		buttons->Add(resetButton, 0, wxRIGHT, 5);
		buttons->Add(copyButton, 0);
		sizer->Add(buttons, 0, wxEXPAND | wxALL, 6);

		auto showValue = [](const FluidGlassRow& row)
		{
			row.value->SetLabel(FormatFluidGlassValue(*row.parameter, static_cast<float>(row.slider->GetValue()) * row.parameter->step));
		};

		auto load = [rows, enabled, items, dialogs, profile, showValue]()
		{
			for (auto& row : *rows)
			{
				const auto& parameter = *row.parameter;
				const float value = DecodeFluidGlassValue(parameter, ReadFluidGlassDword(parameter.name, EncodeFluidGlassValue(parameter, parameter.defaultValue)));
				row.slider->SetValue(static_cast<int>(std::lround(value / parameter.step)));
				showValue(row);
			}
			enabled->SetValue(ReadFluidGlassDword(L"FluidGlass", 0) != 0);
			items->SetValue(ReadFluidGlassDword(L"FluidGlassItems", 1) != 0);
			dialogs->SetValue(ReadFluidGlassDword(L"FluidGlassDialogs", 0) != 0);
			profile->SetSelection(static_cast<int>(std::min(ReadFluidGlassDword(L"FluidGlassProfile", 0), 2ul)));
		};
		load();

		for (auto& row : *rows)
		{
			row.slider->Bind(wxEVT_SLIDER, [row, pending, applyTimer, showValue](wxCommandEvent&)
			{
				showValue(row);
				(*pending)[row.parameter->name] = EncodeFluidGlassValue(*row.parameter, static_cast<float>(row.slider->GetValue()) * row.parameter->step);
				applyTimer->StartOnce(100);
			});
		}

		panel->Bind(wxEVT_TIMER, [this, pending](wxTimerEvent&)
		{
			for (const auto& [name, value] : *pending)
			{
				WriteFluidGlassDword(name.c_str(), value);
			}
			pending->clear();
			NotifyFluidGlassChange();
			SetStatusText(L"Fluid Glass: applied " + wxDateTime::Now().FormatISOTime());
		}, applyTimer->GetId());

		enabled->Bind(wxEVT_CHECKBOX, [this, enabled](wxCommandEvent&)
		{
			WriteFluidGlassDword(L"FluidGlass", enabled->GetValue() ? 1 : 0);
			NotifyFluidGlassChange();
			SetStatusText(enabled->GetValue() ? L"Fluid Glass: on" : L"Fluid Glass: off");
		});

		// The item glass in explorer reads this value about once a second, so no notification is needed.
		items->Bind(wxEVT_CHECKBOX, [this, items](wxCommandEvent&)
		{
			WriteFluidGlassDword(L"FluidGlassItems", items->GetValue() ? 1 : 0);
			SetStatusText(items->GetValue() ? L"File Explorer glass: on" : L"File Explorer glass: off");
		});

		// Read when a dialog loads the glass, and about once a second after that.
		dialogs->Bind(wxEVT_CHECKBOX, [this, dialogs](wxCommandEvent&)
		{
			WriteFluidGlassDword(L"FluidGlassDialogs", dialogs->GetValue() ? 1 : 0);
			SetStatusText(dialogs->GetValue() ? L"Dialog glass: on for dialogs opened from now on" : L"Dialog glass: off");
		});

		profile->Bind(wxEVT_CHOICE, [this, profile](wxCommandEvent&)
		{
			WriteFluidGlassDword(L"FluidGlassProfile", static_cast<DWORD>(profile->GetSelection()));
			NotifyFluidGlassChange();
			SetStatusText(L"Fluid Glass: bezel shape applied");
		});

		resetButton->Bind(wxEVT_BUTTON, [this, rows, load, colorRows, loadColors](wxCommandEvent&)
		{
			for (const auto& row : *rows)
			{
				WriteFluidGlassDword(row.parameter->name, EncodeFluidGlassValue(*row.parameter, row.parameter->defaultValue));
			}
			WriteFluidGlassDword(L"FluidGlassProfile", 0);
			for (const auto& row : *colorRows) WriteFluidGlassDword(row.name, 0);
			NotifyFluidGlassChange();
			load();
			loadColors();
			SetStatusText(L"Fluid Glass: defaults applied");
		});

		for (const auto& [button, preset] : presetButtons)
		{
			button->Bind(wxEVT_BUTTON, [this, rows, load, preset](wxCommandEvent&)
			{
				for (const auto& row : *rows)
				{
					WriteFluidGlassDword(row.parameter->name, EncodeFluidGlassValue(*row.parameter, GetPresetValue(*preset, *row.parameter)));
				}
				WriteFluidGlassDword(L"FluidGlassProfile", 0);
				NotifyFluidGlassChange();
				load();
				SetStatusText(wxString(L"Fluid Glass: ") + preset->label + L" applied");
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
			wxTextEntryDialog dialog(this, L"Name for this preset:", L"Save Fluid Glass preset");
			if (dialog.ShowModal() != wxID_OK)
			{
				return;
			}
			const wxString name = dialog.GetValue().Strip(wxString::both);
			if (name.empty() || name.length() > 64 || name.find_first_of(L"\\/") != wxString::npos)
			{
				ModernTheme::ShowMessage(L"Use a name of 1 to 64 characters without \\ or /.", L"Save Fluid Glass preset", wxOK | wxICON_WARNING, this);
				return;
			}
			if (ListUserPresets().Index(name, false) != wxNOT_FOUND
				&& ModernTheme::ShowMessage(L"A preset named \"" + name + L"\" already exists. Replace it?", L"Save Fluid Glass preset", wxYES_NO | wxICON_QUESTION, this) != wxYES)
			{
				return;
			}
			wil::unique_hkey key;
			if (RegCreateKeyExW(HKEY_CURRENT_USER, UserPresetPath(name).c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, key.put(), nullptr) != ERROR_SUCCESS)
			{
				SetStatusText(L"Fluid Glass: the preset could not be saved");
				return;
			}
			for (const auto& row : *rows)
			{
				const DWORD value = EncodeFluidGlassValue(*row.parameter, static_cast<float>(row.slider->GetValue()) * row.parameter->step);
				RegSetValueExW(key.get(), row.parameter->name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
			}
			const DWORD shape = static_cast<DWORD>(std::max(profile->GetSelection(), 0));
			RegSetValueExW(key.get(), L"FluidGlassProfile", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&shape), sizeof(shape));
			refreshUserPresets(name);
			SetStatusText(L"Fluid Glass: saved preset " + name);
		});

		applyUserPreset->Bind(wxEVT_BUTTON, [this, rows, load, userPresets](wxCommandEvent&)
		{
			const wxString name = userPresets->GetStringSelection();
			wil::unique_hkey key;
			if (name.empty() || RegOpenKeyExW(HKEY_CURRENT_USER, UserPresetPath(name).c_str(), 0, KEY_READ, key.put()) != ERROR_SUCCESS)
			{
				SetStatusText(L"Fluid Glass: the preset could not be read");
				return;
			}
			const auto read = [&key](PCWSTR value, DWORD fallback)
			{
				DWORD data{}, size = sizeof(data), type{};
				return RegQueryValueExW(key.get(), value, nullptr, &type, reinterpret_cast<BYTE*>(&data), &size) == ERROR_SUCCESS && type == REG_DWORD ? data : fallback;
			};
			for (const auto& row : *rows)
			{
				WriteFluidGlassDword(row.parameter->name, read(row.parameter->name, EncodeFluidGlassValue(*row.parameter, row.parameter->defaultValue)));
			}
			WriteFluidGlassDword(L"FluidGlassProfile", std::min(read(L"FluidGlassProfile", 0), 2ul));
			NotifyFluidGlassChange();
			load();
			SetStatusText(L"Fluid Glass: " + name + L" applied");
		});

		deleteUserPreset->Bind(wxEVT_BUTTON, [this, userPresets, refreshUserPresets](wxCommandEvent&)
		{
			const wxString name = userPresets->GetStringSelection();
			if (name.empty()
				|| ModernTheme::ShowMessage(L"Delete the preset \"" + name + L"\"?", L"Delete Fluid Glass preset", wxYES_NO | wxICON_QUESTION, this) != wxYES)
			{
				return;
			}
			RegDeleteTreeW(HKEY_CURRENT_USER, UserPresetPath(name).c_str());
			refreshUserPresets(wxEmptyString);
			SetStatusText(L"Fluid Glass: deleted preset " + name);
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
				SetStatusText(L"Fluid Glass: values copied");
			}
		});

		panel->SetSizer(sizer);
		m_notebook->AddPage(panel, L"Fluid Glass");
	}
}
