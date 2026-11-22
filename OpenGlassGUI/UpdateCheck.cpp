#include "pch.h"
#include "UpdateCheck.hpp"
#include <array>
#include <format>
#include <vector>
#include <winhttp.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.UI.Notifications.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "windowsapp.lib")

namespace OpenGlass::UpdateCheck
{
	namespace
	{
		constexpr auto c_host = L"api.github.com";
		constexpr auto c_path = L"/repos/DreY85/OpenFluidGlass/releases/latest";
		constexpr auto c_releasePage = L"https://github.com/DreY85/OpenFluidGlass/releases/tag/";
		constexpr auto c_stateKey = L"Software\\OpenGlass\\Update";
		constexpr DWORD c_timeoutMs = 10000;
		constexpr size_t c_maxBody = 1024 * 1024;

		struct InternetHandle
		{
			HINTERNET handle{};
			~InternetHandle() { if (handle) WinHttpCloseHandle(handle); }
		};

		std::wstring StateKeyPath(const std::wstring& userSid, HKEY& root)
		{
			if (userSid.empty())
			{
				root = HKEY_CURRENT_USER;
				return c_stateKey;
			}
			root = HKEY_USERS;
			return userSid + L"\\" + c_stateKey;
		}

		wil::unique_hkey CreateStateKey(const std::wstring& userSid)
		{
			HKEY root{};
			const auto path = StateKeyPath(userSid, root);
			wil::unique_hkey key;
			RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, key.put(), nullptr);
			return key;
		}

		std::wstring ReadString(HKEY key, const wchar_t* name)
		{
			wchar_t buffer[128]{};
			DWORD size = sizeof(buffer);
			if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) return {};
			return buffer;
		}

		void WriteString(const std::wstring& userSid, const wchar_t* name, const std::wstring& value)
		{
			const auto key = CreateStateKey(userSid);
			if (!key) return;
			RegSetValueExW(key.get(), name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
				static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
		}

		std::wstring Utf8ToWide(const std::string& text)
		{
			if (text.empty() || text.size() > 64) return {};
			const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
			if (length <= 0) return {};
			std::wstring wide(static_cast<size_t>(length), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
			return wide;
		}

		bool ShowNotification(const Release& release, std::wstring_view installed)
		{
			namespace Notifications = winrt::Windows::UI::Notifications;
			try
			{
				// Protocol activation opens the release page from the notification, also after this process has exited.
				// The tag is validated to digits and dots, so the strings need no XML escaping.
				const auto xml = std::format(
					L"<toast activationType=\"protocol\" launch=\"{0}\">"
					L"<visual><binding template=\"ToastGeneric\">"
					L"<text>Open Fluid Glass {1} is available</text>"
					L"<text>You have {2}. Open the release page to see what is new and download it.</text>"
					L"</binding></visual>"
					L"<actions><action content=\"Open release page\" activationType=\"protocol\" arguments=\"{0}\"/></actions>"
					L"</toast>",
					release.url, release.version, installed);
				winrt::Windows::Data::Xml::Dom::XmlDocument document;
				document.LoadXml(xml);
				Notifications::ToastNotificationManager::CreateToastNotifier(c_appUserModelId).Show(Notifications::ToastNotification{ document });
				return true;
			}
			catch (const winrt::hresult_error&)
			{
				return false;
			}
		}
	}

	std::wstring InstalledVersion()
	{
		wchar_t path[MAX_PATH]{};
		if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return {};
		DWORD handle{};
		const DWORD size = GetFileVersionInfoSizeW(path, &handle);
		if (!size) return {};
		std::vector<BYTE> data(size);
		if (!GetFileVersionInfoW(path, 0, size, data.data())) return {};
		VS_FIXEDFILEINFO* info{};
		UINT length{};
		if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info) return {};
		const auto version = std::format(L"{}.{}.{}", HIWORD(info->dwProductVersionMS), LOWORD(info->dwProductVersionMS), HIWORD(info->dwProductVersionLS));
		return LOWORD(info->dwProductVersionLS) ? std::format(L"{}.{}", version, LOWORD(info->dwProductVersionLS)) : version;
	}

	int CompareVersions(std::wstring_view left, std::wstring_view right)
	{
		const auto parse = [](std::wstring_view text)
		{
			std::array<unsigned long, 4> parts{};
			if (!text.empty() && (text.front() == L'v' || text.front() == L'V')) text.remove_prefix(1);
			size_t index = 0;
			for (const auto ch : text)
			{
				if (ch == L'.')
				{
					if (++index == parts.size()) break;
				}
				else if (ch >= L'0' && ch <= L'9' && parts[index] < 100000)
				{
					parts[index] = parts[index] * 10 + static_cast<unsigned long>(ch - L'0');
				}
				else
				{
					break;
				}
			}
			return parts;
		};
		const auto a = parse(left);
		const auto b = parse(right);
		return a < b ? -1 : a > b ? 1 : 0;
	}

	// A tag is one to four dot-separated numbers with an optional leading v. Anything else is refused.
	std::optional<Release> ReleaseFromTag(std::wstring_view tag)
	{
		if (tag.empty() || tag.size() > 32) return std::nullopt;
		auto version = tag;
		if (version.front() == L'v' || version.front() == L'V') version.remove_prefix(1);
		if (version.empty()) return std::nullopt;
		int parts = 1;
		bool digit = false;
		for (const auto ch : version)
		{
			if (ch >= L'0' && ch <= L'9') digit = true;
			else if (ch == L'.' && digit && parts < 4)
			{
				++parts;
				digit = false;
			}
			else return std::nullopt;
		}
		if (!digit) return std::nullopt;
		return Release{ std::wstring(tag), std::wstring(version), c_releasePage + std::wstring(tag) };
	}

	FetchResult FetchLatest()
	{
		FetchResult result;
		InternetHandle session{ WinHttpOpen(L"OpenFluidGlass-UpdateCheck", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
			WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0) };
		if (!session.handle)
		{
			result.error = L"could not start WinHTTP";
			return result;
		}
		WinHttpSetTimeouts(session.handle, c_timeoutMs, c_timeoutMs, c_timeoutMs, c_timeoutMs);
		InternetHandle connection{ WinHttpConnect(session.handle, c_host, INTERNET_DEFAULT_HTTPS_PORT, 0) };
		InternetHandle request{ connection.handle
			? WinHttpOpenRequest(connection.handle, L"GET", c_path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
			: nullptr };
		if (!request.handle)
		{
			result.error = L"could not open the request";
			return result;
		}
		constexpr wchar_t headers[] = L"Accept: application/vnd.github+json\r\n";
		if (!WinHttpSendRequest(request.handle, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
			|| !WinHttpReceiveResponse(request.handle, nullptr))
		{
			result.error = L"GitHub could not be reached";
			return result;
		}
		DWORD status{};
		DWORD statusSize = sizeof(status);
		WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
			&status, &statusSize, WINHTTP_NO_HEADER_INDEX);
		if (status != 200)
		{
			result.error = std::format(L"GitHub answered {}", status);
			return result;
		}
		std::string body;
		for (;;)
		{
			DWORD available{};
			if (!WinHttpQueryDataAvailable(request.handle, &available))
			{
				result.error = L"the answer was cut off";
				return result;
			}
			if (!available) break;
			if (body.size() + available > c_maxBody)
			{
				result.error = L"the answer was too large";
				return result;
			}
			const auto offset = body.size();
			body.resize(offset + available);
			DWORD read{};
			if (!WinHttpReadData(request.handle, body.data() + offset, available, &read))
			{
				result.error = L"the answer was cut off";
				return result;
			}
			body.resize(offset + read);
		}
		const auto json = nlohmann::json::parse(body, nullptr, false);
		if (json.is_discarded() || !json.is_object() || !json.contains("tag_name") || !json["tag_name"].is_string())
		{
			result.error = L"the answer had no release tag";
			return result;
		}
		result.latest = ReleaseFromTag(Utf8ToWide(json["tag_name"].get<std::string>()));
		if (!result.latest) result.error = L"the release tag is not a version number";
		return result;
	}

	State LoadState(const std::wstring& userSid)
	{
		State state;
		HKEY root{};
		const auto path = StateKeyPath(userSid, root);
		wil::unique_hkey key;
		if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, key.put()) != ERROR_SUCCESS) return state;
		DWORD daily{};
		DWORD size = sizeof(daily);
		if (RegGetValueW(key.get(), nullptr, L"CheckDaily", RRF_RT_REG_DWORD, nullptr, &daily, &size) == ERROR_SUCCESS)
			state.daily = daily != 0;
		state.latestTag = ReadString(key.get(), L"LatestTag");
		state.notified = ReadString(key.get(), L"NotifiedVersion");
		state.skipped = ReadString(key.get(), L"SkippedVersion");
		return state;
	}

	void SaveDaily(const std::wstring& userSid, bool daily)
	{
		const auto key = CreateStateKey(userSid);
		if (!key) return;
		const DWORD value = daily ? 1 : 0;
		RegSetValueExW(key.get(), L"CheckDaily", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
	}

	void SaveLatest(const std::wstring& userSid, const Release& release)
	{
		WriteString(userSid, L"LatestTag", release.tag);
	}

	void SaveSkipped(const std::wstring& userSid, const std::wstring& version)
	{
		WriteString(userSid, L"SkippedVersion", version);
	}

	std::optional<Release> PendingUpdate(const State& state)
	{
		auto release = ReleaseFromTag(state.latestTag);
		if (!release || release->version == state.skipped) return std::nullopt;
		const auto installed = InstalledVersion();
		if (installed.empty() || CompareVersions(release->version, installed) <= 0) return std::nullopt;
		return release;
	}

	int RunScheduledCheck(std::wstring_view assumedVersion)
	{
		const auto state = LoadState({});
		if (!state.daily) return 0;
		const auto result = FetchLatest();
		if (!result.latest) return 1;
		SaveLatest({}, *result.latest);
		const auto installed = assumedVersion.empty() ? InstalledVersion() : std::wstring(assumedVersion);
		if (installed.empty() || CompareVersions(result.latest->version, installed) <= 0) return 0;
		if (result.latest->version == state.skipped) return 0;
		// One notice per version. The GUI keeps showing the update until it is installed or skipped.
		if (assumedVersion.empty() && result.latest->version == state.notified) return 0;
		if (!ShowNotification(*result.latest, installed)) return 2;
		WriteString({}, L"NotifiedVersion", result.latest->version);
		return 0;
	}
}
