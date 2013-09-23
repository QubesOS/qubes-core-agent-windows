#pragma once
#include <windows.h>
#include <lmcons.h>
#include <aclapi.h>
#include <userenv.h>
#include <strsafe.h>
#include "log.h"

#define DESKTOP_ALL (DESKTOP_READOBJECTS | DESKTOP_CREATEWINDOW | \
DESKTOP_CREATEMENU | DESKTOP_HOOKCONTROL | DESKTOP_JOURNALRECORD | \
DESKTOP_JOURNALPLAYBACK | DESKTOP_ENUMERATE | DESKTOP_WRITEOBJECTS | \
DESKTOP_SWITCHDESKTOP | STANDARD_RIGHTS_REQUIRED)

#define WINSTA_ALL (WINSTA_ENUMDESKTOPS | WINSTA_READATTRIBUTES | \
WINSTA_ACCESSCLIPBOARD | WINSTA_CREATEDESKTOP | \
WINSTA_WRITEATTRIBUTES | WINSTA_ACCESSGLOBALATOMS | \
WINSTA_EXITWINDOWS | WINSTA_ENUMERATE | WINSTA_READSCREEN | \
STANDARD_RIGHTS_REQUIRED)

#define GENERIC_ACCESS (GENERIC_READ | GENERIC_WRITE | \
GENERIC_EXECUTE | GENERIC_ALL)


ULONG CreatePipedProcessAsUserW(
		PWCHAR pwszUserName,
		PWCHAR pwszUserPassword,
		PWCHAR pwszCommand,
		BOOLEAN bRunInteractively,
		HANDLE hPipeStdin,
		HANDLE hPipeStdout,
		HANDLE hPipeStderr,
		HANDLE *phProcess
);

ULONG CreateNormalProcessAsUserW(
		PWCHAR pwszUserName,
		PWCHAR pwszUserPassword,
		PWCHAR pwszCommand,
		BOOLEAN bRunInteractively,
		HANDLE *phProcess
);

ULONG CreatePipedProcessAsCurrentUserW(
		PWCHAR pwszCommand,
		BOOLEAN bRunInteractively,
		HANDLE hPipeStdin,
		HANDLE hPipeStdout,
		HANDLE hPipeStderr,
		HANDLE *phProcess
);

ULONG CreateNormalProcessAsCurrentUserW(
		PWCHAR pwszCommand,
		BOOLEAN bRunInteractively,
		HANDLE *phProcess
);

ULONG GrantDesktopAccess(
	LPCTSTR pszAccountName,
	LPCTSTR pszSystemName
);
