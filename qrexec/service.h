#pragma once
#include <windows.h>
#include "common.h"
#include "log.h"

void WINAPI ServiceMain(IN DWORD argc, IN WCHAR *argv[]);

ULONG UpdateServiceStatus(
    IN DWORD currentState,
    IN DWORD win32ExitCode,
    IN DWORD serviceSpecificExitCode,
    IN DWORD waitHint);

ULONG InstallService(IN const WCHAR *serviceFileName, IN const WCHAR *serviceName);

ULONG UninstallService(IN const WCHAR *serviceName);

ULONG Init(IN const HANDLE *serviceThread);
