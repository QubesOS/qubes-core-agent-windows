/*
 * Launches a process with full SYSTEM privileges.
 * For some reason exe launched as a local group policy's startup script
 * doesn't have SeCreateSymbolicLinkPrivilege on Qubes Windows VMs,
 * even though it runs as SYSTEM.
 * That ensures that the child process has the necessary privileges.
 * This process needs to have SeImpersonatePrivilege.
 */
 
#include <windows.h>
#include <Psapi.h>

#include "log.h"

#define LOG_NAME L"bootstrap"

// Process from which we "borrow" a LocalSystem token with full privileges.
// TODO: get the first process with all required privileges
#define SMSS_NAME L"smss.exe"

WCHAR *symlinkPrivilege = L"SeCreateSymbolicLinkPrivilege";

BOOL EnablePrivilege(HANDLE token, PWCHAR privilegeName)
{
	TOKEN_PRIVILEGES tp;
	LUID luid;

	if (!LookupPrivilegeValue(NULL, privilegeName, &luid))
	{
		perror("LookupPrivilegeValue");
		return FALSE;
	}

	tp.PrivilegeCount = 1;
	tp.Privileges[0].Luid = luid;
	tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

	if (!AdjustTokenPrivileges(
		token,
		FALSE,
		&tp,
		sizeof(TOKEN_PRIVILEGES),
		(PTOKEN_PRIVILEGES)NULL,
		(PDWORD)NULL))
	{
		perror("AdjustTokenPrivileges");
		return FALSE;
	}

	if (GetLastError() == ERROR_NOT_ALL_ASSIGNED)

	{
		errorf("The token does not have the specified privilege.");
		return FALSE;
	}

	logf("Privilege %s enabled", privilegeName);

	return TRUE;
}

PWCHAR AttrToStr(DWORD attr)
{
	switch (attr)
	{
	case 0:
		return L"DISABLED";
	case SE_PRIVILEGE_ENABLED:
		return L"SE_PRIVILEGE_ENABLED";
	case SE_PRIVILEGE_ENABLED_BY_DEFAULT:
		return L"SE_PRIVILEGE_ENABLED_BY_DEFAULT";
	case SE_PRIVILEGE_ENABLED | SE_PRIVILEGE_ENABLED_BY_DEFAULT:
		return L"SE_PRIVILEGE_ENABLED | SE_PRIVILEGE_ENABLED_BY_DEFAULT";
	case SE_PRIVILEGE_REMOVED:
		return L"SE_PRIVILEGE_REMOVED";
	case SE_PRIVILEGE_USED_FOR_ACCESS:
		return L"SE_PRIVILEGE_USED_FOR_ACCESS";
	}
	return L"<UNKNOWN>";
}

// Returns TRUE if the token contains given privilege.
BOOL CheckTokenPrivilege(HANDLE token, PWCHAR privilegeName)
{
	BYTE info[4096] = { 0 };
	DWORD size, returned;
	PTOKEN_PRIVILEGES privs = (PTOKEN_PRIVILEGES)info;
	DWORD i;
	WCHAR privName[256];
	LUID luid;
	BOOL retval = FALSE;

	if (!LookupPrivilegeValue(NULL, privilegeName, &luid))
	{
		perror("LookupPrivilegeValue");
		return FALSE;
	}

	GetTokenInformation(token, TokenPrivileges, info, sizeof(info), &size);
	logf("Privs: %d", privs->PrivilegeCount);
	for (i = 0; i<privs->PrivilegeCount; i++)
	{
		returned = RTL_NUMBER_OF(privName);
		LookupPrivilegeName(NULL, &privs->Privileges[i].Luid, privName, &returned);
		logf("%d: %s (%x.%x) %x %s", i, privName,
			privs->Privileges[i].Luid.HighPart, privs->Privileges[i].Luid.LowPart,
			privs->Privileges[i].Attributes, AttrToStr(privs->Privileges[i].Attributes));

		if (privs->Privileges[i].Luid.HighPart == luid.HighPart && privs->Privileges[i].Luid.LowPart == luid.LowPart)
			retval = TRUE;
	}

	return retval;
}

// Acquire a LocalSystem token with full privileges.
HANDLE GetLocalSystemToken(void)
{
	DWORD pidArray[1024];
	DWORD returned, i;
	HANDLE process;
	WCHAR path[MAX_PATH];
	HANDLE token = NULL;

	EnumProcesses(pidArray, RTL_NUMBER_OF(pidArray), &returned);
	for (i = 0; i < returned; i++)
	{
		process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_DUP_HANDLE, FALSE, pidArray[i]);
		if (!process)
		{
			errorf("%d: OpenProcess failed, %d", pidArray[i], GetLastError());
			continue;
		}
		// Look for smss.exe
		if (GetProcessImageFileName(process, path, RTL_NUMBER_OF(path)))
		{
			logf("%d: %s", pidArray[i], path);
			if (wcslen(path) >= wcslen(SMSS_NAME))
			{
				if (0 == _wcsnicmp(SMSS_NAME, path + wcslen(path) - wcslen(SMSS_NAME), wcslen(SMSS_NAME))) // match
				{
					if (OpenProcessToken(process, TOKEN_DUPLICATE | TOKEN_READ, &token))
					{
						if (CheckTokenPrivilege(token, symlinkPrivilege))
						{
							CloseHandle(process);
							return token;
						}
						CloseHandle(token);
					}
					else
					{
						perror("OpenProcessToken");
					}
				}
			}
			else
			{
				perror("GetProcessImageFileName");
			}
		}

		CloseHandle(process);
	}
	return NULL;
}

int wmain(int argc, PWCHAR argv[])
{
	HANDLE token = NULL, newToken = NULL;
	STARTUPINFO si = { 0 };
	PROCESS_INFORMATION pi = { 0 };
	PWCHAR cmdline = argv[1];

    log_init_default(LOG_NAME);

	if (argc < 2)
	{
		errorf("No command line to execute given");
		return 1;
	}

	logf("Command line to run: %s", cmdline);
	logf("Current process token:");
	OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &token);
	
	if (!CheckTokenPrivilege(token, symlinkPrivilege))
	{
		CloseHandle(token);

		// Get more privileged token from another process.
		token = GetLocalSystemToken();
		if (!token)
		{
			errorf("Failed to get system token");
			return 2;
		}
	}

	if (!DuplicateTokenEx(token, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenPrimary, &newToken))
	{
		perror("DuplicateTokenEx");
		return 3;
	}
	CloseHandle(token);

	logf("System token duplicated");

	si.cb = sizeof(si);
	if (!CreateProcessWithTokenW(newToken, 0, NULL, cmdline, 0, NULL, NULL, &si, &pi))
	{
		perror("CreateProcessWithTokenW");
		return 4;
	}

	logf("Process created, waiting for exit");

	if (WaitForSingleObject(pi.hProcess, INFINITE) != WAIT_OBJECT_0)
	{
		perror("WaitForSingleObject(process)");
		return 5;
	}

	logf("done");

	return 0;
}
