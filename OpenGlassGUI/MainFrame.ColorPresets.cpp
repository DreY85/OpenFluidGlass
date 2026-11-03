#include "pch.h"
#include "MainFrame.hpp"
#include "ColorPresetList.hpp"
#include "EffectiveConfiguration.hpp"
#include "ModernTheme.hpp"

namespace OpenGlass
{
	void MainFrame::ApplyColorizationBalances(DWORD argb)
	{
		const auto parameters = ColorizationPresets::CalculateWindows7Parameters(
			argb,
			!m_chkEnableTransparency->IsChecked());
		const std::pair<Settings::Id, DWORD> values[]
		{
			{ Settings::Id::ColorizationColorBalanceOverride, parameters.colorBalance },
			{ Settings::Id::ColorizationAfterglowBalanceOverride, parameters.afterglowBalance },
			{ Settings::Id::ColorizationBlurBalanceOverride, parameters.blurBalance }
		};
		for (const auto& [id, value] : values)
		{
			TrackSettingChange(id);
			const std::wstring name(Settings::Get(id).name);
			THROW_IF_FAILED(m_config->SetDword(name, value));
		}
	}

	// The user's own glass color: both overrides take its hue and keep the alpha Windows gave each base color.
	void MainFrame::WriteGlassColor(DWORD argb)
	{
		const auto writeOverride = [this, rgb = argb & 0xFFFFFF](Settings::Id id, PCWSTR base)
		{
			TrackSettingChange(Settings::Scope::User, id);
			THROW_IF_FAILED(m_userConfig->SetDword(std::wstring(Settings::Get(id).name), (m_userConfig->GetDword(base, 0xFF000000) & 0xFF000000) | rgb));
		};
		writeOverride(Settings::Id::ColorizationColorOverride, L"ColorizationColor");
		writeOverride(Settings::Id::ColorizationAfterglowOverride, L"ColorizationAfterglow");
	}

	// The glass color belongs to OpenGlass alone. A fixed or custom color goes into the user's
	// ColorizationColorOverride and ColorizationAfterglowOverride, which OpenGlass reads before the
	// Windows accent, so the accent color set in Windows Settings stays untouched. Automatic removes
	// both overrides: the glass then follows the Windows accent color again.
	void MainFrame::ApplyColorizationColor(std::optional<DWORD> argb, ColorizationPresets::Family family)
	{
		if (!m_config)
		{
			return;
		}

		if (RunPreview([&]
		{
			auto setDword = [this](Settings::Id id, DWORD value) {
				const std::wstring name(Settings::Get(id).name);
				TrackSettingChange(id);
				THROW_IF_FAILED(m_config->SetDword(name, value));
			};

			// Overrides in either hive would mask the choice; the user's own are rewritten below.
			const auto cleanup = EffectiveConfiguration::PlanColorCleanup(EffectiveConfiguration::Read(*m_userConfig, false), EffectiveConfiguration::Read(*m_systemConfig));
			THROW_IF_FAILED(m_userConfig->CheckDeleteAccess());
			THROW_IF_FAILED(m_systemConfig->CheckDeleteAccess());
			for (const auto& change : cleanup)
			{
				if (argb && change.scope == Settings::Scope::User) continue;
				THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_RETRY), GetConfigForScope(change.scope)->ReadRaw(change.Name()) != change.before);
				TrackSettingChange(change.scope, change.id);
				THROW_IF_FAILED(GetConfigForScope(change.scope)->WriteRaw(change.Name(), change.after));
			}

			const DWORD color = argb.value_or(m_userConfig->GetDword(L"ColorizationColor", 0xFF000000));
			if (argb) WriteGlassColor(*argb);
			setDword(Settings::Id::GlassOpacity, ColorizationPresets::CalculateVistaOpacity(color));
			if (family == ColorizationPresets::Family::Windows7) ApplyColorizationBalances(color);
		}, Settings::UpdateType::Backdrop)) LoadSettings();
	}

	void MainFrame::ApplyColorizationPreset(const ColorizationPresets::Preset& preset)
	{
		ApplyColorizationColor(preset.argb, preset.family);
	}

	const ColorizationPresets::Preset* MainFrame::FindMatchingWindows7Preset(bool opaque) const
	{
		if (!m_config || !m_rbGlassType || m_rbGlassType->GetSelection() != 1)
		{
			return nullptr;
		}

		const auto glass = [this](PCWSTR overrideName, PCWSTR baseName, DWORD fallback)
		{
			return m_userConfig->ReadRaw(overrideName).present ? m_userConfig->GetDword(overrideName, fallback) : m_userConfig->GetDword(baseName, fallback);
		};
		const DWORD color = glass(L"ColorizationColorOverride", L"ColorizationColor", 0xFF000000);
		const DWORD afterglow = glass(L"ColorizationAfterglowOverride", L"ColorizationAfterglow", 0);
		const DWORD colorBalance = m_config->GetDword(L"ColorizationColorBalanceOverride", 10);
		const DWORD afterglowBalance = m_config->GetDword(L"ColorizationAfterglowBalanceOverride", 10);
		const DWORD blurBalance = m_config->GetDword(L"ColorizationBlurBalanceOverride", 50);

		for (const auto& preset : ColorizationPresets::Windows7)
		{
			if (ColorizationPresets::MatchesWindows7Preset(preset,
				{ color, afterglow, colorBalance, afterglowBalance, blurBalance },
				m_config->GetDword(L"GlassOpacity", 63), opaque))
			{
				return &preset;
			}
		}

		return nullptr;
	}

	WXLRESULT MainFrame::MSWWindowProc(WXUINT message, WXWPARAM wParam, WXLPARAM lParam)
	{
		if (message == WM_SETTINGCHANGE && wParam == 0 && lParam
			&& std::wstring_view(reinterpret_cast<const wchar_t*>(lParam)) == L"ImmersiveColorSet")
		{
			// Light or dark may have changed. The next idle time checks it.
			m_appModeChangePending = true;
			if (!m_frozenForAppMode && !IsBeingDeleted() && IsEnabled() && CanSwitchEditingScope()
				&& wxSystemSettings::GetAppearance().AreAppsDark() != ModernTheme::Get().dark)
			{
				HoldPicture(true);
			}
			QueueColorizationRefresh();
			if (IsBeingDeleted() || m_systemColorChangePending) return 0;
			// wxWidgets 3.3.3 still refreshes synchronously on every accent broadcast.
			// Merge queued notifications while retaining its normal system-colour handling.
			m_systemColorChangePending = ::PostMessageW(GetHandle(), WM_SYSCOLORCHANGE, 0, 0) != FALSE;
			if (m_systemColorChangePending) return 0;
			// If posting failed, let the original notification take the normal path.
		}
		if (message == WM_SYSCOLORCHANGE) m_systemColorChangePending = false;

		const auto result = wxFrame::MSWWindowProc(message, wParam, lParam);
		if (message == WM_DWMCOLORIZATIONCOLORCHANGED || message == WM_SETTINGCHANGE)
		{
			QueueColorizationRefresh();
		}
		return result;
	}

	void MainFrame::QueueColorizationRefresh()
	{
		if (!m_customColorsInitialized || m_colorizationRefreshPending || IsBeingDeleted()) return;
		m_colorizationRefreshPending = true;
		CallAfter([this]
		{
			m_colorizationRefreshPending = false;
			// Our own attempt refreshes controls when complete; don't read intermediate writes.
			if (IsBeingDeleted() || m_preview.IsAttemptActive()) return;
			// Read only: external color changes must not alter the preview journal or saved presets.
			UpdateColorizationPresetSelection();
		});
	}

	void MainFrame::UpdateColorizationPresetSelection()
	{
		if (!m_config || !m_rbGlassType)
		{
			return;
		}

		// The glass has its own color while the user's override exists; without it, it follows Windows.
		const auto own = m_userConfig->ReadRaw(L"ColorizationColorOverride");
		const bool automatic = !own.present;
		const bool manual = !automatic;
		const HRESULT modeResult = S_OK;
		const DWORD glassColor = m_userConfig->GetDword(manual ? L"ColorizationColorOverride" : L"ColorizationColor", 0xFF000000);
		m_colorPresetList->SetAutomaticDescription(L"Follow the Windows accent color");
		const ColorizationPresets::Preset* selectedPreset = nullptr;
		if (m_rbGlassType->GetSelection() == 0)
		{
			const DWORD color = glassColor;
			const DWORD opacity = m_config->GetDword(L"GlassOpacity", 63);
			for (const auto& preset : ColorizationPresets::Vista)
			{
				if (
					(color & 0xFFFFFF) == (preset.argb & 0xFFFFFF)
					&& opacity == ColorizationPresets::CalculateVistaOpacity(preset.argb)
				)
				{
					selectedPreset = &preset;
					break;
				}
			}
		}
		else
		{
			selectedPreset = FindMatchingWindows7Preset(
				m_chkEnableTransparency && !m_chkEnableTransparency->IsChecked()
			);
		}
		DWORD color = glassColor;
		bool colorAvailable = true;
		if (automatic)
		{
			// Read the original user's current accent, never the elevated account or message payload.
			colorAvailable = SUCCEEDED(m_colorPreference.ReadRgb(m_targetUserSid.ToStdWstring(), color));
		}
		if (colorAvailable && (!m_customColorsInitialized || automatic || selectedPreset == nullptr))
		{
			m_colorPresetList->SetCustomColor((color & 0xFFFFFF) | (ColorizationPresets::CalculateIntensityAlpha(
				std::min<DWORD>(m_config->GetDword(L"GlassOpacity", 63), 100)) << 24));
		}
		long selection = -1;
		if (SUCCEEDED(modeResult) && automatic) selection = 0;
		else if (manual)
		{
			selection = m_colorPresetList->GetCustomIndex();
			for (long index = 1; index < m_colorPresetList->GetCustomIndex(); ++index)
			{
				if (m_colorPresetList->GetPreset(index) == selectedPreset)
				{
					selection = index;
					break;
				}
			}
		}
		m_colorPresetList->SetCurrentSelection(selection);
		m_customColorsInitialized = true;

		// The clear glass slider shows black and white tints; any hue leaves it in the middle.
		if (m_slClearGlass)
		{
			const int opacity = std::clamp(static_cast<int>(m_config->GetDword(L"GlassOpacity", 63)),
				ColorizationPresets::ClassicIntensityMinimum, ColorizationPresets::ClassicIntensityMaximum);
			const int strength = (opacity - ColorizationPresets::ClassicIntensityMinimum) * 100
				/ (ColorizationPresets::ClassicIntensityMaximum - ColorizationPresets::ClassicIntensityMinimum);
			const DWORD rgb = color & 0xFFFFFF;
			m_slClearGlass->SetValue(!manual ? 0 : rgb == 0x000000 ? -strength : rgb == 0xFFFFFF ? strength : 0);
		}
	}
}
