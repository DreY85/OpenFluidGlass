#include "pch.h"
#include "MainFrame.hpp"
#include "ModernTheme.hpp"

namespace OpenGlass
{
	// The update check is per user and saved right away. It is not part of Save and Revert,
	// because it changes nothing in DWM.
	void MainFrame::CreateUpdatesCard(wxWindow* panel, wxSizer* sizer)
	{
		const auto state = UpdateCheck::LoadState(m_targetUserSid.ToStdWstring());
		auto* card = ModernTheme::MakeCard(panel, L"Updates");

		m_chkCheckUpdates = new wxCheckBox(panel, wxID_ANY, L"Check for updates daily");
		m_chkCheckUpdates->SetValue(state.daily);
		m_chkCheckUpdates->SetToolTip(
			L"Once a day and at sign-in, Fluid Glass asks GitHub for the latest release and shows a notice when it is newer.\n"
			L"Nothing is downloaded or installed. GitHub sees your IP address, as with any visit to a website.\n"
			L"This is saved right away, for your account only.");
		m_chkCheckUpdates->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent& event)
		{
			UpdateCheck::SaveDaily(m_targetUserSid.ToStdWstring(), event.IsChecked());
			SetStatusText(event.IsChecked() ? L"Daily update check turned on." : L"Daily update check turned off.");
		});
		card->Add(m_chkCheckUpdates, 0, wxALL, 2);

		wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
		m_btnCheckUpdates = new wxButton(panel, wxID_ANY, L"Check now");
		m_btnCheckUpdates->SetToolTip(L"Ask GitHub for the latest release now. Nothing is downloaded.");
		ModernTheme::StyleButton(m_btnCheckUpdates);
		m_btnCheckUpdates->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { CheckForUpdatesNow(); });
		row->Add(m_btnCheckUpdates, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
		m_updateStatus = new wxStaticText(panel, wxID_ANY, wxEmptyString);
		row->Add(m_updateStatus, 1, wxALIGN_CENTER_VERTICAL);
		card->Add(row, 0, wxEXPAND | wxALL, 2);

		sizer->Add(card, 0, wxEXPAND | wxALL, 2);
	}

	// Shows what the last stored check found. It reads the registry only and never goes online.
	void MainFrame::RefreshUpdateNotice()
	{
		const auto state = UpdateCheck::LoadState(m_targetUserSid.ToStdWstring());
		m_pendingUpdate = UpdateCheck::PendingUpdate(state);
		if (m_updateStatus)
		{
			const auto installed = UpdateCheck::InstalledVersion();
			std::wstring text = installed.empty() ? std::wstring{} : std::format(L"You have {}.", installed);
			const auto latest = UpdateCheck::ReleaseFromTag(state.latestTag);
			if (m_pendingUpdate) text += std::format(L" Version {} is available.", m_pendingUpdate->version);
			else if (latest && latest->version == state.skipped && UpdateCheck::CompareVersions(latest->version, installed) > 0)
				text += std::format(L" Version {} is out, and you skipped it.", latest->version);
			m_updateStatus->SetLabel(text);
			m_updateStatus->GetParent()->Layout();
		}
		if (m_btnUpdate)
		{
			if (m_pendingUpdate)
			{
				m_btnUpdate->SetLabel(std::format(L"Update to {} ▴", m_pendingUpdate->version));
				m_btnUpdate->SetToolTip(L"A newer Open Fluid Glass release is out. Open its page or skip it.");
			}
			m_btnUpdate->Show(m_pendingUpdate.has_value());
			if (GetSizer()) Layout();
		}
	}

	void MainFrame::CheckForUpdatesNow()
	{
		m_btnCheckUpdates->Enable(false);
		m_updateStatus->SetLabel(L"Asking GitHub...");
		std::thread([]
		{
			auto result = UpdateCheck::FetchLatest();
			if (!wxTheApp) return;
			wxTheApp->CallAfter([result = std::move(result)]
			{
				if (auto* frame = dynamic_cast<MainFrame*>(wxTheApp->GetTopWindow())) frame->ApplyUpdateCheckResult(result);
			});
		}).detach();
	}

	void MainFrame::ApplyUpdateCheckResult(const UpdateCheck::FetchResult& result)
	{
		if (m_btnCheckUpdates) m_btnCheckUpdates->Enable(true);
		if (!result.latest)
		{
			if (m_updateStatus)
			{
				m_updateStatus->SetLabel(std::format(L"The check failed: {}.", result.error));
				m_updateStatus->GetParent()->Layout();
			}
			return;
		}
		UpdateCheck::SaveLatest(m_targetUserSid.ToStdWstring(), *result.latest);
		RefreshUpdateNotice();
		if (m_pendingUpdate) SetStatusText(std::format(L"Version {} is available.", m_pendingUpdate->version));
		else if (UpdateCheck::CompareVersions(result.latest->version, UpdateCheck::InstalledVersion()) > 0)
			SetStatusText(std::format(L"Version {} is out, and you skipped it.", result.latest->version));
		else SetStatusText(L"You have the latest release.");
	}
}
