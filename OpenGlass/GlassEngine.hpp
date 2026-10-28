#pragma once
#include "framework.hpp"
#include "cpprt.hpp"
#include "DwmSettings.hpp"
#include "uDwmProjection.hpp"

namespace OpenGlass::GlassEngine
{
	using DwmSettings::UpdateType;

	HKEY GetDwmKey();
	HKEY GetMachineDwmKey();
	HKEY GetPersonalizeKey();

	inline DWORD GetDwordFromRegistry(PCWSTR keyName, DWORD defaultValue = 0) noexcept
	{
		return DwmSettings::GetDword(GetDwmKey(), GetMachineDwmKey(), keyName, defaultValue);
	}

	inline DWORD GetOverridableDwordFromRegistry(
		PCWSTR baseKeyName,
		PCWSTR overrideKeyName,
		DWORD defaultValue = 0
	) noexcept
	{
		return DwmSettings::GetOverridableDword(GetDwmKey(), GetMachineDwmKey(), baseKeyName, overrideKeyName, defaultValue);
	}

	inline bool AppsUseLightTheme() noexcept
	{
		DWORD light{}, size{ sizeof(light) };
		const HKEY personalize = GetPersonalizeKey();
		return personalize && RegGetValueW(personalize, nullptr, L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size) == ERROR_SUCCESS && light;
	}

	// Light windows draw dark text and icons, which a dark tint would hide. Behind them the tint keeps its hue but is
	// lifted most of the way to white.
	inline D2D1_COLOR_F ForLightWindow(D2D1_COLOR_F color) noexcept
	{
		constexpr float lift = 0.9f;
		color.r += (1.f - color.r) * lift;
		color.g += (1.f - color.g) * lift;
		color.b += (1.f - color.b) * lift;
		return color;
	}

	template <size_t Length>
	inline void GetStringFromRegistry(PCWSTR keyName, WCHAR(&value)[Length]) noexcept
	{
		DwmSettings::GetString(GetDwmKey(), GetMachineDwmKey(), keyName, value);
	}

	void LoadRegistry(bool redrawNow = true);
	void UnloadRegistry();

	void SetDwmNotificationWindow(HWND hWnd);
	HWND GetDwmNotificationWindow();

	void Update(UpdateType type, bool redrawNow = true);
	void Startup();
	void Activate();
	void Shutdown();
}
