// Adds specified command as a local group policy startup script.

#include <Windows.h>
#include <ShlObj.h>
#include <strsafe.h>

#include "log.h"

#define LOG_NAME L"register-startup-script"
#define REG_CONFIG_KEY L"Software\\Invisible Things Lab\\Qubes Tools"
#define REG_CONFIG_LOG_VALUE L"LogDir"

// TODO: does order matter?
WCHAR scriptGuids[] = L"{42B5FAAE-6536-11D2-AE5A-0000F87571E3}{40B6664F-4972-11D1-A7CA-0000F87571E3}";
WCHAR scriptGuidsFull[] = L"[{42B5FAAE-6536-11D2-AE5A-0000F87571E3}{40B6664F-4972-11D1-A7CA-0000F87571E3}]";

// TODO: move registry reading to windows-utils
ULONG ReadRegistryConfig(void)
{
	HKEY key = NULL;
	DWORD status = ERROR_SUCCESS;
	DWORD type;
	DWORD size;
	WCHAR logPath[MAX_PATH];

	// Read the log directory.
	SetLastError(status = RegOpenKey(HKEY_LOCAL_MACHINE, REG_CONFIG_KEY, &key));
	if (status != ERROR_SUCCESS)
	{
		// failed, use some safe default
		// todo: use event log
		log_init(L"c:\\", LOG_NAME);
		logf("registry config: '%s'", REG_CONFIG_KEY);
		return perror("RegOpenKey");
	}

	size = sizeof(logPath)-sizeof(TCHAR);
	RtlZeroMemory(logPath, sizeof(logPath));
	SetLastError(status = RegQueryValueEx(key, REG_CONFIG_LOG_VALUE, NULL, &type, (PBYTE)logPath, &size));
	if (status != ERROR_SUCCESS)
	{
		log_init(L"c:\\", LOG_NAME);
		errorf("Failed to read log path from '%s\\%s'", REG_CONFIG_KEY, REG_CONFIG_LOG_VALUE);
		perror("RegQueryValueEx");
		status = ERROR_SUCCESS; // don't fail
		goto cleanup;
	}

	if (type != REG_SZ)
	{
		log_init(L"c:\\", LOG_NAME);
		errorf("Invalid type of config value '%s', 0x%x instead of REG_SZ", REG_CONFIG_LOG_VALUE, type);
		status = ERROR_SUCCESS; // don't fail
		goto cleanup;
	}

	log_init(logPath, LOG_NAME);

cleanup:
	if (key)
		RegCloseKey(key);

	return status;
}

int wmain(int argc, WCHAR* argv[])
{
	PWCHAR systemPath;
	WCHAR gptPath[MAX_PATH];
	WCHAR scriptsPath[MAX_PATH];
	WCHAR buf[4096] = { 0 };
	ULONG gptVersion = 0;
	PWCHAR policyBuf;
	DWORD policySize;
	PWCHAR str;

    if (ReadRegistryConfig() != ERROR_SUCCESS)
    	return -1;

	if (S_OK != SHGetKnownFolderPath(&FOLDERID_System, 0, NULL, &systemPath))
	{
		perror("SHGetKnownFolderPath(FOLDERID_System)");
		return 1;
	}

	if (argc < 3)
	{
		errorf("usage: %s <startup script path> <script arguments>", argv[0]);
		return 2;
	}

	if (S_OK != StringCchPrintf(gptPath, RTL_NUMBER_OF(gptPath), L"%s\\GroupPolicy\\gpt.ini", systemPath))
	{
		perror("StringCchPrintf(gptPath)");
		return 3;
	}
	if (S_OK != StringCchPrintf(scriptsPath, RTL_NUMBER_OF(scriptsPath), L"%s\\GroupPolicy\\Machine\\Scripts\\scripts.ini", systemPath))
	{
		perror("StringCchPrintf(scriptsPath)");
		return 3;
	}

	logf("gpt.ini: %s", gptPath);
	logf("scripts.ini: %s", scriptsPath);

	// Write startup script information.
	// TODO: check if there are any existing entries
	if (!WritePrivateProfileString(L"Startup", L"0CmdLine", argv[1], scriptsPath))
	{
		perror("WritePrivateProfileString(0CmdLine)");
		return 4;
	}
	if (!WritePrivateProfileString(L"Startup", L"0Parameters", argv[2], scriptsPath))
	{
		perror("WritePrivateProfileString(0Parameters)");
		return 4;
	}

	// Get active policies.
	GetPrivateProfileString(L"General", L"gPCMachineExtensionNames", NULL, buf, RTL_NUMBER_OF(buf), gptPath);
	logf("policies: '%s'", buf);
	// Append script guids if not there.
	if (0 == wcsstr(buf, scriptGuids))
	{
		// The value format is [{guid1}{guid2}...{guidN}]
		str = wcsrchr(buf, L']');
		if (NULL == str) // no [], create the whole thing
		{
			if (!WritePrivateProfileString(L"General", L"gPCMachineExtensionNames", scriptGuidsFull, gptPath))
			{
				perror("WritePrivateProfileString(gPCMachineExtensionNames)");
				return 4;
			}
		}
		else
		{
			// Append script guids.
			policySize = sizeof(WCHAR) * (wcslen(buf) + wcslen(scriptGuids) + 1);
			policyBuf = malloc(policySize);
			if (!policyBuf)
			{
				errorf("No memory");
				return 5;
			}
			ZeroMemory(policyBuf, policySize);
			StringCbCopy(policyBuf, policySize, buf);
			str = wcsrchr(policyBuf, L']'); // rescan in the new buffer
			memcpy(str, scriptGuids, sizeof(scriptGuids)); // add script guids
			policyBuf[wcslen(policyBuf)] = L']'; // add trailing bracket
			logf("gPCMachineExtensionNames=%s", policyBuf);
			if (!WritePrivateProfileString(L"General", L"gPCMachineExtensionNames", policyBuf, gptPath))
			{
				perror("WritePrivateProfileString(gPCMachineExtensionNames)");
				return 4;
			}
		}
	}

	// Increment the version to force policy refresh.
	gptVersion = GetPrivateProfileInt(L"General", L"Version", 0, gptPath);
	// if the key is not present this will be 0
	gptVersion++;
	StringCchPrintf(buf, RTL_NUMBER_OF(buf), L"%lu", gptVersion);
	logf("Version: %lu", gptVersion);
	if (!WritePrivateProfileString(L"General", L"Version", buf, gptPath))
	{
		perror("WritePrivateProfileString(Version)");
		return 4;
	}

	return 0;
}
