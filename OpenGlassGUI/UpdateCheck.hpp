#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace OpenGlass::UpdateCheck
{
	// Notify only: the check asks GitHub for the latest release and never downloads or installs anything.
	// It runs in the GUI process (Check now) or headless from the FluidGlassUpdateCheck task (--check-updates),
	// never in DWM, Explorer or the taskbar host.

	// The installer sets the same ID on the Start menu shortcut. Windows shows a desktop app's
	// notifications only when a Start menu shortcut carries its ID.
	inline constexpr auto c_appUserModelId = L"DreY85.OpenFluidGlass";

	struct Release
	{
		std::wstring tag;     // as published, for example "1.0.5"
		std::wstring version; // the tag without a leading v
		std::wstring url;     // the release page, always built from the validated tag
	};

	struct FetchResult
	{
		std::optional<Release> latest;
		std::wstring error;
	};

	// Per-user state in Software\OpenGlass\Update. An empty SID means the current user's hive.
	struct State
	{
		bool daily{ true };
		std::wstring latestTag;
		std::wstring notified;
		std::wstring skipped;
	};

	std::wstring InstalledVersion();
	int CompareVersions(std::wstring_view left, std::wstring_view right);
	std::optional<Release> ReleaseFromTag(std::wstring_view tag);

	// Blocking network call with short timeouts. Call it off the UI thread.
	FetchResult FetchLatest();

	State LoadState(const std::wstring& userSid);
	void SaveDaily(const std::wstring& userSid, bool daily);
	void SaveLatest(const std::wstring& userSid, const Release& release);
	void SaveSkipped(const std::wstring& userSid, const std::wstring& version);

	// A release newer than this build that the user has not skipped, from the last stored check.
	std::optional<Release> PendingUpdate(const State& state);

	// --check-updates. assumedVersion stands in for the installed version when testing, and allows a repeat notice.
	int RunScheduledCheck(std::wstring_view assumedVersion);
}
