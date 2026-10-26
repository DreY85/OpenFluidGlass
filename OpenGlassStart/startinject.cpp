// Probe only: loads a fresh copy of starttap.dll into StartMenuExperienceHost (or the exe named first), the way pillhost loads its TAP into explorer.
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <cstdio>

// App container processes read only files granted to ALL APPLICATION PACKAGES and ALL RESTRICTED APPLICATION PACKAGES.
static bool GrantAppContainers(const wchar_t* path)
{
	PSECURITY_DESCRIPTOR descriptor{};
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GRGX;;;S-1-15-2-1)(A;;GRGX;;;S-1-15-2-2)", SDDL_REVISION_1, &descriptor, nullptr)) return false;
	BOOL present{}, defaulted{};
	PACL added{}, current{}, merged{};
	PSECURITY_DESCRIPTOR old{};
	GetSecurityDescriptorDacl(descriptor, &present, &added, &defaulted);
	bool ok = GetNamedSecurityInfoW(path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &current, nullptr, &old) == ERROR_SUCCESS;
	if (ok)
	{
		ACL_SIZE_INFORMATION info{};
		GetAclInformation(added, &info, sizeof(info), AclSizeInformation);
		EXPLICIT_ACCESSW entries[2]{};
		for (DWORD index = 0; index < info.AceCount && index < 2; ++index)
		{
			ACCESS_ALLOWED_ACE* ace{};
			GetAce(added, index, reinterpret_cast<void**>(&ace));
			entries[index].grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
			entries[index].grfAccessMode = GRANT_ACCESS;
			entries[index].Trustee.TrusteeForm = TRUSTEE_IS_SID;
			entries[index].Trustee.ptstrName = reinterpret_cast<LPWSTR>(&ace->SidStart);
		}
		ok = SetEntriesInAclW(2, entries, current, &merged) == ERROR_SUCCESS
			&& SetNamedSecurityInfoW(const_cast<LPWSTR>(path), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, merged, nullptr) == ERROR_SUCCESS;
	}
	LocalFree(merged);
	LocalFree(old);
	LocalFree(descriptor);
	return ok;
}

static DWORD Find(const wchar_t* exe)
{
	const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	PROCESSENTRY32W entry{ sizeof(entry) };
	DWORD pid{};
	for (BOOL more = Process32FirstW(snapshot, &entry); more && !pid; more = Process32NextW(snapshot, &entry))
	{
		if (_wcsicmp(entry.szExeFile, exe) == 0) pid = entry.th32ProcessID;
	}
	CloseHandle(snapshot);
	return pid;
}

int wmain(int argc, wchar_t** argv)
{
	wchar_t folder[MAX_PATH], source[MAX_PATH], copy[MAX_PATH];
	GetModuleFileNameW(nullptr, folder, MAX_PATH);
	*wcsrchr(folder, L'\\') = L'\0';
	swprintf_s(source, L"%s\\starttap.dll", folder);
	swprintf_s(copy, L"%s\\starttap-%llu.dll", folder, GetTickCount64());
	if (!CopyFileW(source, copy, FALSE)) { wprintf(L"cannot copy starttap.dll, error %lu\n", GetLastError()); return 1; }
	if (!GrantAppContainers(copy)) { wprintf(L"cannot grant app containers access\n"); return 1; }
	const wchar_t* exe = argc > 1 ? argv[1] : L"StartMenuExperienceHost.exe";
	const DWORD pid = Find(exe);
	if (!pid) { wprintf(L"StartMenuExperienceHost is not running\n"); return 1; }
	const HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE, FALSE, pid);
	if (!process) { wprintf(L"cannot open %lu, error %lu\n", pid, GetLastError()); return 1; }
	const SIZE_T size = (wcslen(copy) + 1) * sizeof(wchar_t);
	void* remote = VirtualAllocEx(process, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	const auto load = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
	HANDLE thread{};
	if (remote && WriteProcessMemory(process, remote, copy, size, nullptr)) thread = CreateRemoteThread(process, nullptr, 0, load, remote, 0, nullptr);
	if (!thread) { wprintf(L"cannot start a thread, error %lu\n", GetLastError()); return 1; }
	wprintf(L"thread started in %lu, waiting (open Start if it stays here)...\n", pid);
	const DWORD wait = WaitForSingleObject(thread, 60000);
	DWORD code{};
	GetExitCodeThread(thread, &code);
	wprintf(wait == WAIT_OBJECT_0 ? (code ? L"loaded\n" : L"LoadLibraryW failed\n") : L"still waiting after 60 s\n");
	return 0;
}
