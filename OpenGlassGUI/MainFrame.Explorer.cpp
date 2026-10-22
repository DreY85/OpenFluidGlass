#include "pch.h"
#include "MainFrame.hpp"
#include "UiControls.hpp"

// File Explorer tab: the settings of ExplorerBlurMica, the library that clears the background of the File Explorer
// content area (navigation pane and file list) so the glass behind the window shows through.
// Like the Taskbar tab these values are live: they go straight to its config.ini, so Save and Revert do not affect them.

namespace OpenGlass
{
	namespace
	{
		constexpr PCWSTR c_libraryKey = L"SOFTWARE\\Classes\\CLSID\\{887D3A6A-502E-4AF5-9CE6-D515E12AFE89}\\InProcServer32";

		// The effect numbers are the library's own: 0 Blur, 1 Acrylic, 2 Mica, 3 Blur (clear), 4 Mica Alt.
		constexpr PCWSTR c_effectNames[] = { L"Blur", L"Acrylic", L"Mica", L"Blur (clear)", L"Mica Alt" };
		constexpr int c_defaultEffect = 0;

		struct Tint
		{
			int r, g, b, a;
		};
		constexpr Tint c_defaultLight{ 255, 255, 255, 200 };
		constexpr Tint c_defaultDark{ 0, 0, 0, 120 };

		struct ExplorerToggle
		{
			PCWSTR key;
			PCWSTR label;
			PCWSTR hint;
		};

		const ExplorerToggle c_explorerToggles[]
		{
			{ L"clearAddress", L"Clear the address bar background", L"Lets the glass show through the address bar. With StartAllBack, turn its classic search box off or it overrides this." },
			{ L"clearBarBg", L"Clear the scrollbar background", L"Draws the scrollbars without their own background." },
			{ L"clearWinUIBg", L"Clear the toolbar background (Windows 11)", L"Removes the background of the WinUI and XAML toolbar at the top of the window." },
			{ L"showLine", L"Hide the line between the navigation pane and the file list", L"Stored as showLine=true, which the library's author documents as hiding that separator." },
		};

		// The DLL explorer loads: the one the installer registered (or the one set up by hand with regsvr32).
		std::filesystem::path RegisteredLibrary()
		{
			wchar_t path[MAX_PATH]{};
			DWORD size = sizeof(path);
			if (RegGetValueW(HKEY_LOCAL_MACHINE, c_libraryKey, nullptr, RRF_RT_REG_SZ, nullptr, path, &size) == ERROR_SUCCESS)
			{
				return path;
			}
			return {};
		}

		// config.ini sits beside the registered DLL, which is where the library reads it from.
		std::wstring ExplorerConfigPath()
		{
			const std::filesystem::path library = RegisteredLibrary();
			if (!library.empty())
			{
				return (library.parent_path() / L"config.ini").wstring();
			}
			wchar_t self[MAX_PATH];
			GetModuleFileNameW(nullptr, self, MAX_PATH);
			return (std::filesystem::path(self).parent_path() / L"ExplorerBlurMica" / L"config.ini").wstring();
		}

		// The library reads its file with GetPrivateProfileString, so the same API keeps the file just as it expects it.
		int ReadIni(const std::wstring& file, PCWSTR section, PCWSTR key, int fallback)
		{
			return static_cast<int>(GetPrivateProfileIntW(section, key, fallback, file.c_str()));
		}

		bool ReadIniBool(const std::wstring& file, PCWSTR section, PCWSTR key, bool fallback)
		{
			wchar_t text[16]{};
			GetPrivateProfileStringW(section, key, fallback ? L"true" : L"false", text, ARRAYSIZE(text), file.c_str());
			return _wcsicmp(text, L"true") == 0 || wcscmp(text, L"1") == 0;
		}

		bool WriteIni(const std::wstring& file, PCWSTR section, PCWSTR key, const std::wstring& value)
		{
			return WritePrivateProfileStringW(section, key, value.c_str(), file.c_str()) != FALSE;
		}

		Tint ReadTint(const std::wstring& file, PCWSTR section, const Tint& fallback)
		{
			return {
				std::clamp(ReadIni(file, section, L"r", fallback.r), 0, 255), std::clamp(ReadIni(file, section, L"g", fallback.g), 0, 255),
				std::clamp(ReadIni(file, section, L"b", fallback.b), 0, 255), std::clamp(ReadIni(file, section, L"a", fallback.a), 0, 255) };
		}

		bool WriteTint(const std::wstring& file, PCWSTR section, const Tint& tint)
		{
			bool ok = WriteIni(file, section, L"r", std::to_wstring(tint.r));
			ok = WriteIni(file, section, L"g", std::to_wstring(tint.g)) && ok;
			ok = WriteIni(file, section, L"b", std::to_wstring(tint.b)) && ok;
			return WriteIni(file, section, L"a", std::to_wstring(tint.a)) && ok;
		}

		struct TintRow
		{
			PCWSTR section;
			const Tint* fallback;
			wxColourPickerCtrl* picker;
			wxSlider* alpha;
			wxStaticText* value;
		};

		// Ends every explorer of this user's session; Windows starts the shell again by itself, as that user.
		// Returns whether a shell window came back within the wait.
		bool RestartExplorerShell()
		{
			wchar_t system[MAX_PATH];
			GetSystemDirectoryW(system, MAX_PATH);
			DWORD session{};
			ProcessIdToSessionId(GetCurrentProcessId(), &session);
			std::wstring command = std::format(L"\"{}\\taskkill.exe\" /F /IM explorer.exe /FI \"SESSION eq {}\"", system, session);
			STARTUPINFOW startup{ sizeof(startup) };
			PROCESS_INFORMATION process{};
			if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
			{
				return false;
			}
			WaitForSingleObject(process.hProcess, 10000);
			CloseHandle(process.hThread);
			CloseHandle(process.hProcess);
			for (int attempt = 0; attempt < 40; ++attempt)
			{
				Sleep(250);
				if (FindWindowW(L"Shell_TrayWnd", nullptr))
				{
					return true;
				}
			}
			return false;
		}

		wxString FormatAlpha(int alpha)
		{
			return wxString::Format(L"%d (%d%%)", alpha, (alpha * 100 + 127) / 255);
		}
	}

	void MainFrame::CreateExplorerTab()
	{
		wxScrolledWindow* panel = new wxScrolledWindow(m_notebook);
		panel->SetScrollRate(5, 5);
		panel->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
		wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

		const wxColour hintColour = wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);
		wxFont hintFont = panel->GetFont();
		hintFont.SetFractionalPointSize(hintFont.GetFractionalPointSize() * 0.9);

		auto addHint = [&](wxSizer* group, PCWSTR text)
		{
			auto* hint = new wxStaticText(panel, wxID_ANY, text);
			hint->SetForegroundColour(hintColour);
			hint->SetFont(hintFont);
			hint->Wrap(FromDIP(560));
			group->Add(hint, 0, wxLEFT | wxBOTTOM, FromDIP(8));
		};

		auto* generalGroup = ModernTheme::MakeCard(panel, L"File Explorer glass");
		auto* note = new wxStaticText(panel, wxID_ANY,
			L"Makes the content of File Explorer windows (navigation pane and file list) see-through, so OpenGlass can turn "
			L"what is behind them into Liquid Glass. The window and toolbar glass itself comes from OpenGlass. "
			L"Changes go to config.ini at once, so Save and Revert do not affect them. "
			L"They are used by File Explorer windows opened afterwards, and Restart Explorer applies them to all.");
		note->SetForegroundColour(hintColour);
		note->Wrap(FromDIP(560));
		generalGroup->Add(note, 0, wxEXPAND | wxALL, 4);

		wxStaticText* status{};
		{
			wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
			row->Add(new wxStaticText(panel, wxID_ANY, L"Status:", wxDefaultPosition, FromDIP(wxSize(160, -1))), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
			status = new wxStaticText(panel, wxID_ANY, wxEmptyString);
			row->Add(status, 1, wxALIGN_CENTER_VERTICAL);
			generalGroup->Add(row, 0, wxEXPAND | wxALL, 4);
		}

		wxChoice* effect{};
		{
			wxArrayString names;
			for (PCWSTR name : c_effectNames)
			{
				names.Add(name);
			}
			wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
			auto* label = new wxStaticText(panel, wxID_ANY, L"Effect:", wxDefaultPosition, FromDIP(wxSize(160, -1)));
			row->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
			effect = new wxChoice(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, names);
			row->Add(effect, 0, wxALIGN_CENTER_VERTICAL);
			constexpr PCWSTR hintText = L"What sits behind the content. Blur is the one OpenGlass turns into Liquid Glass.";
			label->SetToolTip(hintText);
			effect->SetToolTip(hintText);
			generalGroup->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 4);
			addHint(generalGroup, hintText);
		}

		auto toggles = std::make_shared<std::vector<std::pair<const ExplorerToggle*, wxCheckBox*>>>();
		for (const auto& toggle : c_explorerToggles)
		{
			auto* box = new wxCheckBox(panel, wxID_ANY, toggle.label);
			box->SetToolTip(toggle.hint);
			generalGroup->Add(box, 0, wxALL, 4);
			addHint(generalGroup, toggle.hint);
			toggles->push_back({ &toggle, box });
		}
		sizer->Add(generalGroup, 0, wxEXPAND | wxALL, 2);

		auto* tintGroup = ModernTheme::MakeCard(panel, L"Tint");
		auto tints = std::make_shared<std::vector<TintRow>>();
		const struct { PCWSTR section; PCWSTR label; const Tint* fallback; } tintSpecs[] =
		{
			{ L"light", L"Light mode:", &c_defaultLight },
			{ L"dark", L"Dark mode:", &c_defaultDark },
		};
		for (const auto& spec : tintSpecs)
		{
			wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
			row->Add(new wxStaticText(panel, wxID_ANY, spec.label, wxDefaultPosition, FromDIP(wxSize(160, -1))), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
			auto* picker = new wxColourPickerCtrl(panel, wxID_ANY, *wxWHITE);
			row->Add(picker, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
			auto* alpha = new NativeSlider(panel, wxID_ANY, spec.fallback->a, 0, 255);
			row->Add(alpha, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
			auto* value = new wxStaticText(panel, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(84, -1)), wxALIGN_RIGHT | wxST_NO_AUTORESIZE);
			row->Add(value, 0, wxALIGN_CENTER_VERTICAL);
			tintGroup->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 4);
			tints->push_back({ spec.section, spec.fallback, picker, alpha, value });
		}
		addHint(tintGroup, L"The colour laid over the glass and how strongly (alpha), for Windows in light and in dark mode.");
		sizer->Add(tintGroup, 0, wxEXPAND | wxALL, 2);

		wxBoxSizer* buttons = new wxBoxSizer(wxHORIZONTAL);
		auto* resetButton = new wxButton(panel, wxID_ANY, L"Reset to defaults");
		resetButton->SetToolTip(L"Write the settings OpenGlass ships with to config.ini.");
		auto* restartButton = new wxButton(panel, wxID_ANY, L"Restart Explorer");
		restartButton->SetToolTip(L"Closes all File Explorer windows and the taskbar for a moment, then applies the settings everywhere.");
		buttons->AddStretchSpacer();
		buttons->Add(resetButton, 0, wxRIGHT, 6);
		buttons->Add(restartButton, 0);
		sizer->Add(buttons, 0, wxEXPAND | wxALL, 6);

		auto showAlpha = [](const TintRow& row) { row.value->SetLabel(FormatAlpha(row.alpha->GetValue())); };

		auto showStatus = [status]()
		{
			const std::filesystem::path library = RegisteredLibrary();
			wxString text;
			if (library.empty())
			{
				text = L"Not registered with File Explorer";
			}
			else if (!std::filesystem::exists(library))
			{
				text = wxString::Format(L"Registered, but the library is missing: %s", library.wstring());
			}
			else
			{
				text = wxString::Format(L"Registered: %s", library.wstring());
			}
			if (status->GetLabel() != text)
			{
				status->SetLabel(text);
			}
		};

		auto load = [effect, toggles, tints, showAlpha, showStatus]()
		{
			const std::wstring file = ExplorerConfigPath();
			effect->SetSelection(std::clamp(ReadIni(file, L"config", L"effect", c_defaultEffect), 0, static_cast<int>(ARRAYSIZE(c_effectNames)) - 1));
			for (const auto& [toggle, box] : *toggles)
			{
				box->SetValue(ReadIniBool(file, L"config", toggle->key, true));
			}
			for (auto& row : *tints)
			{
				const Tint tint = ReadTint(file, row.section, *row.fallback);
				row.picker->SetColour(wxColour(tint.r, tint.g, tint.b));
				row.alpha->SetValue(tint.a);
				showAlpha(row);
			}
			showStatus();
		};
		load();

		// A write can fail when the settings sit in Program Files and OpenGlass was not started as administrator.
		auto report = [this](bool ok, const wxString& done)
		{
			SetStatusText(ok ? done : wxString(L"File Explorer: could not write config.ini (start OpenGlass as administrator)"));
		};

		effect->Bind(wxEVT_CHOICE, [effect, report](wxCommandEvent&)
		{
			report(WriteIni(ExplorerConfigPath(), L"config", L"effect", std::to_wstring(effect->GetSelection())), L"File Explorer: effect saved");
		});

		for (const auto& entry : *toggles)
		{
			PCWSTR key = entry.first->key;
			wxCheckBox* box = entry.second;
			box->Bind(wxEVT_CHECKBOX, [key, box, report](wxCommandEvent&)
			{
				report(WriteIni(ExplorerConfigPath(), L"config", key, box->GetValue() ? L"true" : L"false"), L"File Explorer: setting saved");
			});
		}

		auto saveTint = [report](const TintRow& row)
		{
			const wxColour colour = row.picker->GetColour();
			const Tint tint{ colour.Red(), colour.Green(), colour.Blue(), row.alpha->GetValue() };
			report(WriteTint(ExplorerConfigPath(), row.section, tint), L"File Explorer: tint saved");
		};
		for (auto& row : *tints)
		{
			row.picker->Bind(wxEVT_COLOURPICKER_CHANGED, [row, saveTint](wxColourPickerEvent&) { saveTint(row); });
			row.alpha->Bind(wxEVT_SLIDER, [row, showAlpha](wxCommandEvent&) { showAlpha(row); });
			// The file is written when the slider is let go, not on every step of the drag.
			row.alpha->Bind(wxEVT_SCROLL_CHANGED, [row, saveTint](wxScrollEvent&) { saveTint(row); });
		}

		resetButton->Bind(wxEVT_BUTTON, [load, report](wxCommandEvent&)
		{
			const std::wstring file = ExplorerConfigPath();
			bool ok = WriteIni(file, L"config", L"effect", std::to_wstring(c_defaultEffect));
			for (const auto& toggle : c_explorerToggles)
			{
				ok = WriteIni(file, L"config", toggle.key, L"true") && ok;
			}
			ok = WriteTint(file, L"light", c_defaultLight) && ok;
			ok = WriteTint(file, L"dark", c_defaultDark) && ok;
			load();
			report(ok, L"File Explorer: defaults written");
		});

		restartButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&)
		{
			if (wxMessageBox(L"Windows Explorer will restart. Open File Explorer windows close and the taskbar disappears for a few seconds. Continue?",
				L"Restart Explorer", wxYES_NO | wxICON_QUESTION, this) != wxYES)
			{
				return;
			}
			SetStatusText(L"File Explorer: restarting Explorer");
			// Off the UI thread: it waits for the shell to come back.
			std::thread([]
			{
				const bool back = RestartExplorerShell();
				wxTheApp->CallAfter([back]
				{
					if (auto* frame = wxDynamicCast(wxTheApp->GetTopWindow(), wxFrame))
					{
						frame->SetStatusText(back ? L"File Explorer: Explorer restarted"
							: L"File Explorer: Explorer did not come back by itself (Task Manager, Run new task, explorer.exe)");
					}
				});
			}).detach();
		});

		panel->SetSizer(sizer);
		m_notebook->AddPage(panel, L"File Explorer");
	}
}
