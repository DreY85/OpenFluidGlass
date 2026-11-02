// The log files of pillhost.exe and the TAPs, in the user's (or the app container's) temp folder.
// Off in released builds: they exist only when built with FLUIDGLASS_LOGS defined (build.cmd logs).
#pragma once
#include <windows.h>
#include <cstdio>

constexpr ULONGLONG LogLimit = 256 * 1024;	// bytes; a log past this starts over, so it never grows for good

// Opens the named log for appending. Once it has passed LogLimit it moves to name.old, so the minutes before stay
// readable (a 256 KB start over once lost the Start opening being looked for). Null if it cannot be opened, and always
// null without FLUIDGLASS_LOGS.
inline FILE* OpenLog([[maybe_unused]] const wchar_t* name)
{
#ifdef FLUIDGLASS_LOGS
	wchar_t path[MAX_PATH];
	GetTempPathW(MAX_PATH, path);
	wcscat_s(path, name);
	WIN32_FILE_ATTRIBUTE_DATA data{};
	const bool full = GetFileAttributesExW(path, GetFileExInfoStandard, &data)
		&& ((static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) | data.nFileSizeLow) > LogLimit;
	if (full)
	{
		wchar_t old[MAX_PATH];
		swprintf_s(old, L"%s.old", path);
		MoveFileExW(path, old, MOVEFILE_REPLACE_EXISTING);
	}
	FILE* file{};
	return _wfopen_s(&file, path, full ? L"w, ccs=UTF-8" : L"a, ccs=UTF-8") ? nullptr : file;
#else
	return nullptr;
#endif
}
