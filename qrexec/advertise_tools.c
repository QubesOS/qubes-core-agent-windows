#include <windows.h>
#include <shlwapi.h>
#include <tchar.h>
#include <xenstore.h>
#include "log.h"

#define XS_TOOLS_PREFIX "qubes-tools/"

/* just a helper function, the buffer needs to be at least MAX_PATH+1 length */
BOOL prepare_exe_path(PTCHAR buffer, PTCHAR exe_name) {
	PTCHAR  ptSeparator = NULL;

	memset(buffer, 0, sizeof(buffer));
	if (!GetModuleFileName(NULL, buffer, MAX_PATH)) {
		lprintf_err(GetLastError(), __FUNCTION__ "(): GetModuleFileName()");
		return FALSE;
	}
	// cut off file name (qrexec_agent.exe)
	ptSeparator = _tcsrchr(buffer, L'\\');
	if (!ptSeparator) {
		lprintf(__FUNCTION__ "(): Cannot find dir containing qrexec_agent.exe\n");
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

	if (!prepare_exe_path(szQrxecClientVmPath, TEXT("qrexec_client_vm.exe")))
		return FALSE;

	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	si.wShowWindow = SW_HIDE;
	si.dwFlags = STARTF_USESHOWWINDOW;

	if (!CreateProcess(
			szQrxecClientVmPath,
			TEXT("qrexec_client_vm.exe dom0 qubes.NotifyTools dummy"),
			NULL,
			NULL,
			FALSE,
			CREATE_NO_WINDOW,
			NULL,
			NULL,
			&si,
			&pi)) {
		lprintf_err(GetLastError(), __FUNCTION__ "(): Failed to start qrexec_client_vm.exe");
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

	xs = xs_domain_open();
	if (!xs) {
		/* error message already printed to stderr */
		goto cleanup;
	}

	/* for now mostly hardcoded values, but this can change in the future */
	if (!xs_write(xs, XBT_NULL, XS_TOOLS_PREFIX "version", "1", 1)) {
		lprintf_err(GetLastError(), __FUNCTION__ "(): failed to write 'version' entry");
		goto cleanup;
	}
	if (!xs_write(xs, XBT_NULL, XS_TOOLS_PREFIX "os", "Windows", strlen("Windows"))) {
		lprintf_err(GetLastError(), __FUNCTION__ "(): failed to write 'os' entry");
		goto cleanup;
	}
	if (!xs_write(xs, XBT_NULL, XS_TOOLS_PREFIX "qrexec", "1", 1)) {
		lprintf_err(GetLastError(), __FUNCTION__ "(): failed to write 'qrexec' entry");
		goto cleanup;
	}

	gui_present = check_gui_presence();

	if (!xs_write(xs, XBT_NULL, XS_TOOLS_PREFIX "gui", gui_present ? "1" : "0", 1)) {
		lprintf_err(GetLastError(), __FUNCTION__ "(): failed to write 'gui' entry");
		goto cleanup;
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
