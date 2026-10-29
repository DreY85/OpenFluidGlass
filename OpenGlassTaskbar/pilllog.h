// The log files of pillhost.exe and the TAPs, in the user's (or the app container's) temp folder.
// Off in released builds: they exist only when built with FLUIDGLASS_LOGS defined (build.cmd logs).
#pragma once
#include <windows.h>
#include <cstdio>

constexpr ULONGLONG LogLimit = 256 * 1024;	// bytes; a log past this starts over, so it never grows for good

// Opens the named log for appending, or empties it first once it has passed LogLimit. Null if it cannot be opened, and always null without FLUIDGLASS_LOGS.
inline FILE* OpenLog([[maybe_unused]] const wchar_t* name)
{
#ifdef FLUIDGLASS_LOGS
	wchar_t path[MAX_PATH];
	GetTempPathW(MAX_PATH, path);
	wcscat_s(path, name);
	WIN32_FILE_ATTRIBUTE_DATA data{};
	const bool full = GetFileAttributesExW(path, GetFileExInfoStandard, &data)
		&& ((static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) | data.nFileSizeLow) > LogLimit;
	FILE* file{};
	return _wfopen_s(&file, path, full ? L"w, ccs=UTF-8" : L"a, ccs=UTF-8") ? nullptr : file;
#else
	return nullptr;
#endif
}
