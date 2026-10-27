#include "pch.h"
#include "MainFrame.hpp"
#include "UiControls.hpp"
#include "ShellLaunch.hpp"
#include "WindowsBuild.hpp"

// File Explorer tab: the settings of ExplorerBlurMica, the library that clears the background of the File Explorer
// content area (navigation pane and file list) so the glass behind the window shows through.
// Like the Taskbar tab these values are live: they go straight to its config.ini, so Save and Revert do not affect them.

namespace OpenGlass
{
	namespace
	{
		constexpr PCWSTR c_classKey = L"SOFTWARE\\Classes\\CLSID\\{887D3A6A-502E-4AF5-9CE6-D515E12AFE89}";
		constexpr PCWSTR c_libraryKey = L"SOFTWARE\\Classes\\CLSID\\{887D3A6A-502E-4AF5-9CE6-D515E12AFE89}\\InProcServer32";
		// This key is what makes explorer load the library. Turning it off removes only this one, so the class keeps
		// the DLL's path for turning it on again.
		constexpr PCWSTR c_folderExtensionKey = L"SOFTWARE\\Classes\\Drive\\shellex\\FolderExtensions\\{887D3A6A-502E-4AF5-9CE6-D515E12AFE89}";

		// How the glass looks is set in one place, the Fluid Glass tab. So the library always asks File Explorer for the
		// Mica backdrop (its effect 2) and lays no tint of its own: Open Fluid Glass then draws File Explorer on the same
		// path as every other modern window. Its Blur effect took another path, which came out darker and blurrier than
		// other windows, and its tint added a milky layer only File Explorer had.
		// Mica exists only on Windows 11, so Windows 10 keeps the Blur effect (0), the one that works there.
		PCWSTR ManagedEffect() noexcept
		{
			return IsWindows11() ? L"2" : L"0";
		}

		struct IniValue
		{
			PCWSTR section;
			PCWSTR key;
			PCWSTR value;	// null: the effect for this Windows
		};
		constexpr IniValue c_managedLook[]
		{
			{ L"config", L"effect", nullptr },
			{ L"light", L"a", L"0" },
			{ L"dark", L"a", L"0" },
		};

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

		bool IsLibraryEnabled()
		{
			wil::unique_hkey key;
			return RegOpenKeyExW(HKEY_LOCAL_MACHINE, c_folderExtensionKey, 0, KEY_READ, key.put()) == ERROR_SUCCESS;
		}

		// Writes what the installer (or regsvr32) writes. HKLM, so it needs an elevated OpenGlass.
		LSTATUS SetLibraryEnabled(bool enable)
		{
			if (!enable)
			{
				const LSTATUS status = RegDeleteKeyExW(HKEY_LOCAL_MACHINE, c_folderExtensionKey, KEY_WOW64_64KEY, 0);
				return status == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : status;
			}
			if (RegisteredLibrary().empty())
			{
				wchar_t self[MAX_PATH];
				GetModuleFileNameW(nullptr, self, MAX_PATH);
				const std::wstring library = (std::filesystem::path(self).parent_path() / L"ExplorerBlurMica" / L"ExplorerBlurMica.dll").wstring();
				if (!std::filesystem::exists(library))
				{
					return ERROR_FILE_NOT_FOUND;
				}
				constexpr wchar_t name[] = L"ExplorerBlurMica DLL";
				constexpr wchar_t threading[] = L"Apartment";
				LSTATUS status = RegSetKeyValueW(HKEY_LOCAL_MACHINE, c_classKey, nullptr, REG_SZ, name, sizeof(name));
				if (status == ERROR_SUCCESS)
				{
					status = RegSetKeyValueW(HKEY_LOCAL_MACHINE, c_libraryKey, nullptr, REG_SZ, library.c_str(), static_cast<DWORD>((library.size() + 1) * sizeof(wchar_t)));
				}
				if (status == ERROR_SUCCESS)
				{
					status = RegSetKeyValueW(HKEY_LOCAL_MACHINE, c_libraryKey, L"ThreadingModel", REG_SZ, threading, sizeof(threading));
				}
				if (status != ERROR_SUCCESS)
				{
					return status;
				}
			}
			const DWORD mask = 255;
			return RegSetKeyValueW(HKEY_LOCAL_MACHINE, c_folderExtensionKey, L"DriveMask", REG_DWORD, &mask, sizeof(mask));
		}

		// The library reads its file with GetPrivateProfileString, so the same API keeps the file just as it expects it.
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

		// Writes the managed look where config.ini holds something else (an older install, or a hand edit).
		// changed tells whether anything was written; the result is false when a write failed.
		bool ApplyManagedLook(const std::wstring& file, bool& changed)
		{
			changed = false;
			bool ok = true;
			for (const auto& entry : c_managedLook)
			{
				const PCWSTR value = entry.value ? entry.value : ManagedEffect();
				wchar_t current[16]{};
				GetPrivateProfileStringW(entry.section, entry.key, L"", current, ARRAYSIZE(current), file.c_str());
				if (wcscmp(current, value) != 0)
				{
					ok = WriteIni(file, entry.section, entry.key, value) && ok;
					changed = true;
				}
			}
			return ok;
		}

		enum class RestartResult { Restarted, NotBack, NoScript, NotStarted };

		bool ShellIsUp()
		{
			return FindWindowW(L"Shell_TrayWnd", nullptr) != nullptr;
		}

		// Runs Restart-Explorer.cmd, which the installer puts beside this program. Explorer itself starts it, as the
		// signed-in user, so the explorer it starts again is not elevated. Windows did not restart the shell by itself
		// after it was ended, so the script starts it when it has not come back within a few seconds.
		RestartResult RestartExplorerShell()
		{
			wchar_t self[MAX_PATH];
			GetModuleFileNameW(nullptr, self, MAX_PATH);
			const std::wstring script = (std::filesystem::path(self).parent_path() / L"Restart-Explorer.cmd").wstring();
			if (!std::filesystem::exists(script))
			{
				return RestartResult::NoScript;
			}
			const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
			const HRESULT started = StartAsShellUser(script, SW_HIDE);
			if (SUCCEEDED(initialized))
			{
				CoUninitialize();
			}
			if (FAILED(started))
			{
				return RestartResult::NotStarted;
			}
			// First the shell goes away, then the script brings it back, usually a second or two later.
			for (int attempt = 0; attempt < 20 && ShellIsUp(); ++attempt)
			{
				Sleep(250);
			}
			for (int attempt = 0; attempt < 60; ++attempt)
			{
				Sleep(250);
				if (ShellIsUp())
				{
					return RestartResult::Restarted;
				}
			}
			return RestartResult::NotBack;
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
			L"Makes the content of File Explorer windows (navigation pane and file list) see-through, so Open Fluid Glass draws "
			L"its glass there too, the same as in every other window. How the glass looks, its blur and colour, is set in the "
			L"Fluid Glass tab. The options below go to config.ini at once, so Save and Revert do not affect them. "
			L"They are used by File Explorer windows opened afterwards, and Restart Explorer applies them to all.");
		note->SetForegroundColour(hintColour);
		note->Wrap(FromDIP(560));
		generalGroup->Add(note, 0, wxEXPAND | wxALL, 4);

		auto* enabled = new wxCheckBox(panel, wxID_ANY, L"Use ExplorerBlurMica in File Explorer");
		enabled->SetToolTip(L"Turning it on or off takes effect when Explorer restarts. Needs Open Fluid Glass started as administrator.");
		generalGroup->Add(enabled, 0, wxALL, 4);

		wxStaticText* status{};
		{
			wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
			row->Add(new wxStaticText(panel, wxID_ANY, L"Status:", wxDefaultPosition, FromDIP(wxSize(160, -1))), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
			status = new wxStaticText(panel, wxID_ANY, wxEmptyString);
			row->Add(status, 1, wxALIGN_CENTER_VERTICAL);
			generalGroup->Add(row, 0, wxEXPAND | wxALL, 4);
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

		wxBoxSizer* buttons = new wxBoxSizer(wxHORIZONTAL);
		auto* resetButton = new wxButton(panel, wxID_ANY, L"Reset to defaults");
		resetButton->SetToolTip(L"Write the settings Open Fluid Glass ships with to config.ini.");
		auto* restartButton = new wxButton(panel, wxID_ANY, L"Restart Explorer");
		restartButton->SetToolTip(L"Closes all File Explorer windows and the taskbar for a moment, then applies the settings everywhere.");
		buttons->AddStretchSpacer();
		buttons->Add(resetButton, 0, wxRIGHT, 6);
		buttons->Add(restartButton, 0);
		sizer->Add(buttons, 0, wxEXPAND | wxALL, 6);

		auto showStatus = [status]()
		{
			const std::filesystem::path library = RegisteredLibrary();
			wxString text;
			if (library.empty())
			{
				text = L"Not installed";
			}
			else if (!std::filesystem::exists(library))
			{
				text = wxString::Format(L"The library is missing: %s", library.wstring());
			}
			else
			{
				text = wxString::Format(IsLibraryEnabled() ? L"On: %s" : L"Off: %s", library.wstring());
			}
			if (status->GetLabel() != text)
			{
				status->SetLabel(text);
			}
		};

		auto load = [enabled, toggles, showStatus]()
		{
			const std::wstring file = ExplorerConfigPath();
			enabled->SetValue(IsLibraryEnabled());
			for (const auto& [toggle, box] : *toggles)
			{
				box->SetValue(ReadIniBool(file, L"config", toggle->key, true));
			}
			showStatus();
		};
		load();

		// A write can fail when the settings sit in Program Files and OpenGlass was not started as administrator.
		auto report = [this](bool ok, const wxString& done)
		{
			SetStatusText(ok ? done : wxString(L"File Explorer: could not write config.ini (start Open Fluid Glass as administrator)"));
		};

		// An install from before the look moved to the Fluid Glass tab still has its own effect and tint in config.ini.
		// The status bar is not there yet while the tabs are built, so the result is shown once the window is up.
		if (!RegisteredLibrary().empty())
		{
			bool changed{};
			const bool ok = ApplyManagedLook(ExplorerConfigPath(), changed);
			if (changed)
			{
				CallAfter([report, ok] { report(ok, L"ExplorerBlurMica: the look now follows the Fluid Glass tab, restart Explorer to apply"); });
			}
		}

		enabled->Bind(wxEVT_CHECKBOX, [this, enabled, showStatus](wxCommandEvent&)
		{
			const LSTATUS result = SetLibraryEnabled(enabled->GetValue());
			if (result == ERROR_SUCCESS)
			{
				SetStatusText(enabled->GetValue() ? L"ExplorerBlurMica: on, restart Explorer to apply" : L"ExplorerBlurMica: off, restart Explorer to apply");
			}
			else
			{
				enabled->SetValue(IsLibraryEnabled());
				SetStatusText(result == ERROR_ACCESS_DENIED ? wxString(L"ExplorerBlurMica: start Open Fluid Glass as administrator to change this")
					: result == ERROR_FILE_NOT_FOUND ? wxString(L"ExplorerBlurMica: ExplorerBlurMica.dll was not found")
					: wxString::Format(L"ExplorerBlurMica: could not change the registration (error %ld)", static_cast<long>(result)));
			}
			showStatus();
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

		resetButton->Bind(wxEVT_BUTTON, [load, report](wxCommandEvent&)
		{
			const std::wstring file = ExplorerConfigPath();
			bool changed{};
			bool ok = ApplyManagedLook(file, changed);
			for (const auto& toggle : c_explorerToggles)
			{
				ok = WriteIni(file, L"config", toggle.key, L"true") && ok;
			}
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
				const RestartResult result = RestartExplorerShell();
				wxTheApp->CallAfter([result]
				{
					if (auto* frame = wxDynamicCast(wxTheApp->GetTopWindow(), wxFrame))
					{
						frame->SetStatusText(
							result == RestartResult::Restarted ? L"File Explorer: Explorer restarted"
							: result == RestartResult::NoScript ? L"File Explorer: Restart-Explorer.cmd is missing beside Open Fluid Glass, nothing was restarted"
							: result == RestartResult::NotStarted ? L"File Explorer: could not ask Explorer to restart, nothing was restarted"
							: L"File Explorer: Explorer did not come back (Ctrl+Shift+Esc, Run new task, explorer.exe)");
					}
				});
			}).detach();
		});

		panel->SetSizer(sizer);
		m_notebook->AddPage(panel, L"ExplorerBlurMica");
	}
}
