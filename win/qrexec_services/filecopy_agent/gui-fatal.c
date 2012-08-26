#include <windows.h>
#include <tchar.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <stdlib.h>
#include <stdarg.h>
#include <Strsafe.h>
#include "gui-progress.h"

typedef LONG NTSTATUS;

extern HANDLE STDERR;
extern HWND hDialog;

static void produce_message(int icon, const PTCHAR fmt, va_list args)
{
	char *dialog_msg;
	TCHAR buf[1024];
	PTCHAR  pMessage = NULL;
	ULONG	cchErrorTextSize;
	ULONG   nWritten;

	if (FAILED(StringCchVPrintf(buf, RTL_NUMBER_OF(buf), fmt, args))) {
		/* FIXME: some fallback method? */
		return;
	}

    cchErrorTextSize = FormatMessage(
                FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                NULL,
                GetLastError(),
                MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                pMessage,
                0,
                NULL);

	if (cchErrorTextSize > 0) {
		if (FAILED(StringCchCat(buf, RTL_NUMBER_OF(buf), TEXT(": ")))) {
			LocalFree(pMessage);
			return;
		}
		if (FAILED(StringCchCat(buf, RTL_NUMBER_OF(buf), pMessage))) {
			LocalFree(pMessage);
			return;
		}
	}
	if (FAILED(StringCchCat(buf, RTL_NUMBER_OF(buf), TEXT("\n")))) {
		LocalFree(pMessage);
		return;
	}

	if (STDERR != INVALID_HANDLE_VALUE) {
		// message for qrexec log in dom0
		WriteFile(STDERR, buf, _tcslen(buf)*sizeof(TCHAR), &nWritten, NULL);
	}
	MessageBox(hDialog, buf, TEXT("Qubes file copy error"), MB_OK | icon);
	LocalFree(pMessage);
}

void gui_fatal(const PTCHAR fmt, ...)
{
	va_list args;
	do_notify_progress(0, PROGRESS_FLAG_ERROR);
	va_start(args, fmt);
	produce_message(MB_ICONERROR, fmt, args);
	va_end(args);
	exit(1);
}

void gui_nonfatal(const PTCHAR fmt, ...)
{
	va_list args;
	do_notify_progress(0, PROGRESS_FLAG_ERROR);
	va_start(args, fmt);
	produce_message(MB_ICONWARNING, fmt, args);
	va_end(args);
	do_notify_progress(0, PROGRESS_FLAG_NORMAL);
}
