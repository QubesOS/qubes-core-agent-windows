#include <windows.h>
#include <tchar.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <stdlib.h>
#include <stdarg.h>
#include <Strsafe.h>
#include "gui-fatal.h"
#include "log.h"

typedef LONG NTSTATUS;

HWND hDialog;

show_error_t show_error_cb;

void set_error_gui_callbacks(HWND hD, show_error_t cb) {
	hDialog = hD;
	show_error_cb = cb;
}

static void produce_message(int icon, const PTCHAR fmt, va_list args)
{
	TCHAR buf[1024];
	PTCHAR  pMessage = NULL;
	ULONG	cchErrorTextSize;

	if (FAILED(StringCchVPrintf(buf, RTL_NUMBER_OF(buf), fmt, args))) {
		/* FIXME: some fallback method? */
		perror("produce_message: StringCchVPrintf");
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
			perror("produce_message: StringCchCat");
			LocalFree(pMessage);
			return;
		}
		if (FAILED(StringCchCat(buf, RTL_NUMBER_OF(buf), pMessage))) {
			perror("produce_message: StringCchCat");
			LocalFree(pMessage);
			return;
		}
	}
	if (FAILED(StringCchCat(buf, RTL_NUMBER_OF(buf), TEXT("\n")))) {
		perror("produce_message: StringCchCat");
		LocalFree(pMessage);
		return;
	}

	// message for qrexec log in dom0
	errorf("%s", buf);
	MessageBox(hDialog, buf, TEXT("Qubes file copy error"), MB_OK | icon);
	LocalFree(pMessage);
}

void gui_fatal(TCHAR *fmt, ...)
{
	va_list args;
	show_error_cb(1);
	va_start(args, fmt);
	produce_message(MB_ICONERROR, fmt, args);
	va_end(args);
	exit(1);
}

void gui_nonfatal(TCHAR *fmt, ...)
{
	va_list args;
	show_error_cb(1);
	va_start(args, fmt);
	produce_message(MB_ICONWARNING, fmt, args);
	va_end(args);
	show_error_cb(0);
}
