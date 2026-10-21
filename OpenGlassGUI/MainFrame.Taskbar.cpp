#include "pch.h"
#include "MainFrame.hpp"
#include "UiControls.hpp"
#include "DwmSettings.hpp"
#include "..\OpenGlassTaskbar\pillshare.h"
#include <exdisp.h>
#include <shldisp.h>
#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")

// Taskbar tab: the floating Liquid Glass taskbar run by pillhost.exe (OpenGlassTaskbar).
// Like the Liquid Glass tab these values are live: they go straight to HKCU, where the host reads
// them twice a second, so they stay outside Save and Revert.

namespace OpenGlass
{
	namespace
	{
		void WriteTaskbarSetting(const PillSetting& setting, DWORD value)
		{
			RegSetKeyValueW(HKEY_CURRENT_USER, PillSettingsKey, setting.name, REG_DWORD, &value, sizeof(value));
		}

		HWND FindTaskbarHost()
		{
			return FindWindowW(PillHostClass, nullptr);
		}

		// pillhost.exe next to this program (where the installer puts it), else where it last ran from.
		std::wstring FindTaskbarHostExe()
		{
			wchar_t self[MAX_PATH];
			GetModuleFileNameW(nullptr, self, MAX_PATH);
			std::filesystem::path besideUs = std::filesystem::path(self).parent_path() / L"taskbar" / L"pillhost.exe";
			if (std::filesystem::exists(besideUs))
			{
				return besideUs.wstring();
			}
			wchar_t recorded[MAX_PATH]{};
			DWORD size = sizeof(recorded);
			if (RegGetValueW(HKEY_CURRENT_USER, PillSettingsKey, PillHostPathValue, RRF_RT_REG_SZ, nullptr, recorded, &size) == ERROR_SUCCESS
				&& std::filesystem::exists(recorded))
			{
				return recorded;
			}
			return {};
		}

		// This program runs elevated, but the host must not: explorer could not post to an elevated window.
		// So explorer starts it, through the desktop's shell automation object, as the signed-in user.
		HRESULT StartAsShellUser(const std::wstring& path)
		{
			wil::com_ptr_nothrow<IShellWindows> windows;
			RETURN_IF_FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(windows.put())));
			VARIANT location{};
			location.vt = VT_I4;
			location.lVal = CSIDL_DESKTOP;
			VARIANT empty{};
			long window{};
			wil::com_ptr_nothrow<IDispatch> desktop;
			RETURN_IF_FAILED(windows->FindWindowSW(&location, &empty, SWC_DESKTOP, &window, SWFO_NEEDDISPATCH, desktop.put()));
			RETURN_HR_IF_NULL(E_FAIL, desktop);
			wil::com_ptr_nothrow<IShellBrowser> browser;
			RETURN_IF_FAILED(IUnknown_QueryService(desktop.get(), SID_STopLevelBrowser, IID_PPV_ARGS(browser.put())));
			wil::com_ptr_nothrow<IShellView> view;
			RETURN_IF_FAILED(browser->QueryActiveShellView(view.put()));
			wil::com_ptr_nothrow<IDispatch> background;
			RETURN_IF_FAILED(view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(background.put())));
			wil::com_ptr_nothrow<IShellFolderViewDual> folderView;
			RETURN_IF_FAILED(background->QueryInterface(IID_PPV_ARGS(folderView.put())));
			wil::com_ptr_nothrow<IDispatch> application;
			RETURN_IF_FAILED(folderView->get_Application(application.put()));
			wil::com_ptr_nothrow<IShellDispatch2> shell;
			RETURN_IF_FAILED(application->QueryInterface(IID_PPV_ARGS(shell.put())));
			wil::unique_bstr file{ SysAllocString(path.c_str()) };
			RETURN_IF_NULL_ALLOC(file);
			return shell->ShellExecute(file.get(), empty, empty, empty, empty);
		}

		// The corners are drawn by OpenGlass, so they are a DWM value like the Liquid Glass tab's:
		// pixels times 100. The slider's last step stands for fully round, any radius past half the height.
		constexpr PCWSTR c_pillRadiusValue = L"LiquidGlassPillRadius";
		constexpr int c_roundCorners = 25;
		constexpr DWORD c_roundRadius = 1000;

		int ReadCornerPosition()
		{
			wil::unique_hkey userKey{};
			wil::unique_hkey machineKey{};
			RegOpenKeyExW(HKEY_CURRENT_USER, DwmSettings::g_keyPath, 0, KEY_READ, userKey.put());
			RegOpenKeyExW(HKEY_LOCAL_MACHINE, DwmSettings::g_keyPath, 0, KEY_READ, machineKey.put());
			const DWORD radius = DwmSettings::GetDword(userKey.get(), machineKey.get(), c_pillRadiusValue, c_roundRadius * 100) / 100;
			return static_cast<int>(std::min<DWORD>(radius, c_roundCorners));
		}

		void WriteCornerPosition(int position)
		{
			const DWORD value = (position >= c_roundCorners ? c_roundRadius : static_cast<DWORD>(position)) * 100;
			RegSetKeyValueW(HKEY_CURRENT_USER, DwmSettings::g_keyPath, c_pillRadiusValue, REG_DWORD, &value, sizeof(value));
			DwmSettings::NotifyChange(FindWindowW(L"Dwm", nullptr), DwmSettings::UpdateType::Backdrop);
		}

		wxString FormatCornerPosition(int position)
		{
			return position >= c_roundCorners ? wxString(L"Round") : wxString::Format(L"%d px", position);
		}

		struct TaskbarSlider
		{
			const PillSetting* setting;
			PCWSTR label;
			PCWSTR hint;
		};

		const TaskbarSlider c_taskbarSliders[]
		{
			{ &PillHeight, L"Pill height:", L"Height of every pill. The taskbar itself is 48 px tall." },
			{ &PillIconSize, L"Icon size:", L"Size of the app icons. The taskbar draws them at 24 px; their buttons widen to fit." },
			{ &PillPadding, L"Side padding:", L"Room between the icons and the rounded ends of a pill." },
			{ &PillGap, L"Tray to clock gap:", L"Space between the tray icons pill and the clock pill." },
		};

		struct TaskbarSliderRow
		{
			const TaskbarSlider* slider;
			wxSlider* control;
			wxStaticText* value;
		};
	}

	void MainFrame::CreateTaskbarTab()
	{
		wxScrolledWindow* panel = new wxScrolledWindow(m_notebook);
		panel->SetScrollRate(5, 5);
		panel->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
		wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

		const wxColour hintColour = wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);
		wxFont hintFont = panel->GetFont();
		hintFont.SetFractionalPointSize(hintFont.GetFractionalPointSize() * 0.9);

		auto* generalGroup = ModernTheme::MakeCard(panel, L"Floating taskbar");
		auto* note = new wxStaticText(panel, wxID_ANY,
			L"Splits the taskbar into floating Liquid Glass pills: one for the apps, one for the tray icons and one for the clock. "
			L"Changes apply instantly and are saved to HKCU, so Save and Revert do not affect them.");
		note->SetForegroundColour(hintColour);
		note->Wrap(FromDIP(560));
		generalGroup->Add(note, 0, wxEXPAND | wxALL, 4);

		auto* enabled = new wxCheckBox(panel, wxID_ANY, L"Show the floating taskbar");
		enabled->SetToolTip(L"Turning it off brings the normal taskbar back at once.");
		generalGroup->Add(enabled, 0, wxALL, 4);

		wxStaticText* status{};
		{
			wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
			row->Add(new wxStaticText(panel, wxID_ANY, L"Status:", wxDefaultPosition, FromDIP(wxSize(160, -1))), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
			status = new wxStaticText(panel, wxID_ANY, wxEmptyString);
			row->Add(status, 1, wxALIGN_CENTER_VERTICAL);
			generalGroup->Add(row, 0, wxEXPAND | wxALL, 4);
		}
		sizer->Add(generalGroup, 0, wxEXPAND | wxALL, 2);

		auto* pillsGroup = ModernTheme::MakeCard(panel, L"Pills");
		auto rows = std::make_shared<std::vector<TaskbarSliderRow>>();
		for (const auto& slider : c_taskbarSliders)
		{
			wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
			auto* label = new wxStaticText(panel, wxID_ANY, slider.label, wxDefaultPosition, FromDIP(wxSize(160, -1)));
			row->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
			auto* control = new NativeSlider(panel, wxID_ANY, static_cast<int>(slider.setting->fallback),
				static_cast<int>(slider.setting->minimum), static_cast<int>(slider.setting->maximum));
			row->Add(control, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
			auto* value = new wxStaticText(panel, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(64, -1)), wxALIGN_RIGHT | wxST_NO_AUTORESIZE);
			row->Add(value, 0, wxALIGN_CENTER_VERTICAL);
			label->SetToolTip(slider.hint);
			control->SetToolTip(slider.hint);
			pillsGroup->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 4);

			auto* hint = new wxStaticText(panel, wxID_ANY, slider.hint);
			hint->SetForegroundColour(hintColour);
			hint->SetFont(hintFont);
			pillsGroup->Add(hint, 0, wxLEFT | wxBOTTOM, FromDIP(8));

			rows->push_back({ &slider, control, value });
		}

		wxSlider* corners{};
		wxStaticText* cornersValue{};
		{
			constexpr PCWSTR hintText = L"Rounding of the pill corners. The last step makes the ends fully round.";
			wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
			auto* label = new wxStaticText(panel, wxID_ANY, L"Corner radius:", wxDefaultPosition, FromDIP(wxSize(160, -1)));
			row->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
			corners = new NativeSlider(panel, wxID_ANY, c_roundCorners, 0, c_roundCorners);
			row->Add(corners, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
			cornersValue = new wxStaticText(panel, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(64, -1)), wxALIGN_RIGHT | wxST_NO_AUTORESIZE);
			row->Add(cornersValue, 0, wxALIGN_CENTER_VERTICAL);
			label->SetToolTip(hintText);
			corners->SetToolTip(hintText);
			pillsGroup->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 4);

			auto* hint = new wxStaticText(panel, wxID_ANY, hintText);
			hint->SetForegroundColour(hintColour);
			hint->SetFont(hintFont);
			pillsGroup->Add(hint, 0, wxLEFT | wxBOTTOM, FromDIP(8));
		}

		auto* join = new wxCheckBox(panel, wxID_ANY, L"Put the tray icons and the clock in one pill");
		join->SetToolTip(L"One pill around both instead of two side by side.");
		pillsGroup->Add(join, 0, wxALL, 4);
		sizer->Add(pillsGroup, 0, wxEXPAND | wxALL, 2);

		wxBoxSizer* buttons = new wxBoxSizer(wxHORIZONTAL);
		auto* resetButton = new wxButton(panel, wxID_ANY, L"Reset to defaults");
		resetButton->SetToolTip(L"Write the default pill sizes and round corners to HKCU and apply them.");
		buttons->AddStretchSpacer();
		buttons->Add(resetButton, 0);
		sizer->Add(buttons, 0, wxEXPAND | wxALL, 6);

		auto showValue = [](const TaskbarSliderRow& row)
		{
			row.value->SetLabel(wxString::Format(L"%d px", row.control->GetValue()));
		};

		// The gap only separates two pills, so it means nothing while they are joined.
		auto findGapRow = [rows]() -> TaskbarSliderRow*
		{
			for (auto& row : *rows)
			{
				if (row.slider->setting == &PillGap)
				{
					return &row;
				}
			}
			return nullptr;
		};

		auto showStatus = [status, enabled]()
		{
			wxString text;
			if (FindTaskbarHost())
			{
				text = L"Running";
			}
			else if (FindTaskbarHostExe().empty())
			{
				text = L"Not installed: pillhost.exe was not found";
			}
			else
			{
				text = enabled->GetValue() ? L"Not running" : L"Off";
			}
			if (status->GetLabel() != text)
			{
				status->SetLabel(text);
			}
		};

		auto load = [rows, enabled, join, corners, cornersValue, showValue, findGapRow, showStatus]()
		{
			for (auto& row : *rows)
			{
				row.control->SetValue(static_cast<int>(ReadPillSetting(*row.slider->setting)));
				showValue(row);
			}
			corners->SetValue(ReadCornerPosition());
			cornersValue->SetLabel(FormatCornerPosition(corners->GetValue()));
			enabled->SetValue(ReadPillSetting(PillEnabled) != 0);
			join->SetValue(ReadPillSetting(PillJoin) != 0);
			if (auto* gap = findGapRow())
			{
				gap->control->Enable(!join->GetValue());
			}
			showStatus();
		};
		load();

		for (auto& row : *rows)
		{
			row.control->Bind(wxEVT_SLIDER, [row, showValue](wxCommandEvent&)
			{
				showValue(row);
				WriteTaskbarSetting(*row.slider->setting, static_cast<DWORD>(row.control->GetValue()));
			});
		}

		// DWM reloads all of its settings on each change, so dragging writes at most every 100 ms.
		auto* cornersTimer = PanelTimer(panel);
		corners->Bind(wxEVT_SLIDER, [corners, cornersValue, cornersTimer](wxCommandEvent&)
		{
			cornersValue->SetLabel(FormatCornerPosition(corners->GetValue()));
			cornersTimer->StartOnce(100);
		});
		panel->Bind(wxEVT_TIMER, [this, corners](wxTimerEvent&)
		{
			WriteCornerPosition(corners->GetValue());
			SetStatusText(L"Taskbar: corners applied");
		}, cornersTimer->GetId());

		join->Bind(wxEVT_CHECKBOX, [this, join, findGapRow](wxCommandEvent&)
		{
			WriteTaskbarSetting(PillJoin, join->GetValue() ? 1 : 0);
			if (auto* gap = findGapRow())
			{
				gap->control->Enable(!join->GetValue());
			}
			SetStatusText(join->GetValue() ? L"Taskbar: tray and clock joined" : L"Taskbar: tray and clock apart");
		});

		enabled->Bind(wxEVT_CHECKBOX, [this, enabled, showStatus](wxCommandEvent&)
		{
			WriteTaskbarSetting(PillEnabled, enabled->GetValue() ? 1 : 0);
			if (!enabled->GetValue())
			{
				// The host also sees the setting within half a second; closing it now is just quicker.
				if (const HWND host = FindTaskbarHost())
				{
					PostMessageW(host, WM_CLOSE, 0, 0);
				}
				SetStatusText(L"Taskbar: off");
			}
			else if (!FindTaskbarHost())
			{
				const std::wstring exe = FindTaskbarHostExe();
				if (exe.empty())
				{
					SetStatusText(L"Taskbar: pillhost.exe was not found");
				}
				else
				{
					// Off the UI thread: it waits on explorer, which may be slow to answer (once, in testing, it never did).
					SetStatusText(L"Taskbar: starting");
					std::thread([exe]
					{
						const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
						const HRESULT hr = StartAsShellUser(exe);
						if (SUCCEEDED(initialized))
						{
							CoUninitialize();
						}
						if (FAILED(hr))
						{
							wxTheApp->CallAfter([hr]
							{
								if (auto* frame = wxDynamicCast(wxTheApp->GetTopWindow(), wxFrame))
								{
									frame->SetStatusText(wxString::Format(L"Taskbar: could not start pillhost.exe (0x%08X)", static_cast<unsigned>(hr)));
								}
							});
						}
					}).detach();
				}
			}
			showStatus();
		});

		resetButton->Bind(wxEVT_BUTTON, [this, rows, load](wxCommandEvent&)
		{
			for (const auto& row : *rows)
			{
				WriteTaskbarSetting(*row.slider->setting, row.slider->setting->fallback);
			}
			WriteTaskbarSetting(PillJoin, PillJoin.fallback);
			WriteCornerPosition(c_roundCorners);
			load();
			SetStatusText(L"Taskbar: defaults applied");
		});

		// The host can start or stop on its own (at sign-in, with explorer), so the status follows it.
		auto* statusTimer = PanelTimer(panel);
		panel->Bind(wxEVT_TIMER, [showStatus](wxTimerEvent&) { showStatus(); }, statusTimer->GetId());
		statusTimer->Start(1000);

		panel->SetSizer(sizer);
		m_notebook->AddPage(panel, L"Taskbar");
	}
}
