#pragma once
#include "pch.h"
#include "SettingsCatalog.hpp"

namespace OpenGlass::Elevation
{
	struct StartupResult
	{
		bool continueStartup{};
		std::wstring userSid;
	};

	// uiPreview skips elevation so the window can be opened for screenshots; saving HKLM then fails.
	[[nodiscard]] StartupResult PrepareElevatedStartup(Settings::Scope scope, bool systemTransparency, bool uiPreview = false);
	[[nodiscard]] bool IsProcessElevated() noexcept;
}
