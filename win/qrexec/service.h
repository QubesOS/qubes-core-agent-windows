#pragma once
#include <windows.h>
#include "log.h"


#define	HEARTBEAT_INTERVAL_IN_SECONDS	1

#define SERVICE_NAME	TEXT("qrexec_agent")



VOID WINAPI ServiceMain(DWORD dwArgc, LPTSTR *lpszArgv);
ULONG UpdateServiceStatus(DWORD dwCurrentState,
						 DWORD dwWin32ExitCode,
						 DWORD dwServiceSpecificExitCode,
						 DWORD dwWaitHint);
ULONG InstallService(PTCHAR pszServiceFileName, PTCHAR pszServiceName);
ULONG UninstallService(PTCHAR wszServiceName);
ULONG ReportErrorToEventLog(ULONG uErrorMessageId);

ULONG Init(HANDLE *phServiceThread);