#pragma once
#include <tchar.h>
#include <windows.h>
#include "common.h"
#include "log.h"
#include <strsafe.h>

VOID WINAPI ServiceMain(DWORD dwArgc, LPTSTR *lpszArgv);
ULONG UpdateServiceStatus(DWORD dwCurrentState,
						 DWORD dwWin32ExitCode,
						 DWORD dwServiceSpecificExitCode,
						 DWORD dwWaitHint);
ULONG InstallService(PTCHAR pszServiceFileName, PTCHAR pszServiceName);
ULONG UninstallService(PTCHAR wszServiceName);
ULONG ReportErrorToEventLog(ULONG uErrorMessageId);

ULONG Init(HANDLE *phServiceThread);
