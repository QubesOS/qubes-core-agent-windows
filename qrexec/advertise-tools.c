#include <windows.h>
#include <shlwapi.h>
#include <tchar.h>
#include <xenstore.h>
#include <Wtsapi32.h>
#include "log.h"

#define XS_TOOLS_PREFIX "qubes-tools/"

BOOL get_current_user(PCHAR *ppUserName) {
	PWTS_SESSION_INFOA   pSessionInfo;
	DWORD               dSessionCount;
	DWORD i;
	DWORD				cbUserName;
	BOOL				bFound;

	if (FAILED(WTSEnumerateSessionsA(WTS_CURRENT_SERVER_HANDLE, 0, 1, &pSessionInfo, &dSessionCount))) {
        perror("WTSEnumerateSessionsA");
		return FALSE;
	}
	bFound = FALSE;
	for (i = 0; i < dSessionCount; i++) {
		if (pSessionInfo[i].State == WTSActive) {
			if (FAILED(WTSQuerySessionInformationA(WTS_CURRENT_SERVER_HANDLE,
							pSessionInfo[i].SessionId, WTSUserName,
							ppUserName,
							&cbUserName))) {
                perror("WTSQuerySessionInformationA");
                goto cleanup;
			}
			LogDebug("Found session: %S\n", *ppUserName);
			bFound = TRUE;
		}
	}

cleanup:
	WTSFreeMemory(pSessionInfo);
	return bFound;
}


/* just a helper function, the buffer needs to be at least MAX_PATH+1 length */
BOOL prepare_exe_path(PTCHAR buffer, PTCHAR exe_name) {
	PTCHAR  ptSeparator = NULL;

	memset(buffer, 0, sizeof(buffer));
	if (!GetModuleFileName(NULL, buffer, MAX_PATH)) {
		perror("GetModuleFileName");
		return FALSE;
	}
	// cut off file name (qrexec_agent.exe)
	ptSeparator = _tcsrchr(buffer, L'\\');
	if (!ptSeparator) {
		LogError("Cannot find dir containing qrexec_agent.exe\n");
		return FALSE;
	}
	// Leave trailing backslash 
	ptSeparator++; 
	*ptSeparator = L'\0'; 
	// add an executable filename
	PathAppendW(buffer, exe_name);
	return TRUE;
}

/* TODO - make this configurable? */
BOOL check_gui_presence() {
	TCHAR   szServiceFilePath[MAX_PATH + 1];

	if (!prepare_exe_path(szServiceFilePath, TEXT("wga.exe")))
		return FALSE;

	return PathFileExists(szServiceFilePath);
}

BOOL notify_dom0() {
	TCHAR   szQrxecClientVmPath[MAX_PATH + 1];
	HANDLE  hProcess;
	STARTUPINFO si;
	PROCESS_INFORMATION pi;

	if (!prepare_exe_path(szQrxecClientVmPath, TEXT("qrexec-client-vm.exe")))
		return FALSE;

	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	si.wShowWindow = SW_HIDE;
	si.dwFlags = STARTF_USESHOWWINDOW;

	if (!CreateProcess(
			szQrxecClientVmPath,
			TEXT("qrexec-client-vm.exe dom0 qubes.NotifyTools dummy"),
			NULL,
			NULL,
			FALSE,
			CREATE_NO_WINDOW,
			NULL,
			NULL,
			&si,
			&pi)) {
        perror("CreateProcess(qrexec-client-vm.exe)");
		return FALSE;
	}

	/* fire and forget */
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);

	return TRUE;
}

LONG advertise_tools() {
	struct xs_handle *xs;
	LONG ret = ERROR_INVALID_FUNCTION;
	BOOL gui_present;
	PCHAR	pszUserName;

	xs = xs_domain_open();
	if (!xs) {
		/* error message already printed to stderr */
		goto cleanup;
	}

	/* for now mostly hardcoded values, but this can change in the future */
	if (!xs_write(xs, XBT_NULL, XS_TOOLS_PREFIX "version", "1", 1)) {
		perror("write 'version' entry");
		goto cleanup;
	}
	if (!xs_write(xs, XBT_NULL, XS_TOOLS_PREFIX "os", "Windows", strlen("Windows"))) {
        perror("write 'os' entry");
        goto cleanup;
	}
	if (!xs_write(xs, XBT_NULL, XS_TOOLS_PREFIX "qrexec", "1", 1)) {
        perror("write 'qrexec' entry");
        goto cleanup;
	}

	gui_present = check_gui_presence();

	if (!xs_write(xs, XBT_NULL, XS_TOOLS_PREFIX "gui", gui_present ? "1" : "0", 1)) {
        perror("write 'gui' entry");
        goto cleanup;
	}

	if (get_current_user(&pszUserName)) {
		if (!xs_write(xs, XBT_NULL, XS_TOOLS_PREFIX "default-user", pszUserName, strlen(pszUserName))) {
            perror("write 'default-user' entry");
            WTSFreeMemory(pszUserName);
			goto cleanup;
		}
		WTSFreeMemory(pszUserName);
	}

	if (!notify_dom0()) {
		/* error already reported */
		goto cleanup;
	}

	ret = ERROR_SUCCESS;

cleanup:
	if (xs)
		xs_daemon_close(xs);
	return ret;
}
