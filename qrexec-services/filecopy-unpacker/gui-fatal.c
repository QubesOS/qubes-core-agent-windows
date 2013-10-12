#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <stdlib.h>
#include <stdarg.h>
#include <Strsafe.h>
#include "log.h"

typedef LONG NTSTATUS;

extern HANDLE STDERR;

// fixme: use TCHARs

static void produce_message(int icon, const PWCHAR fmt, va_list args)
{
	WCHAR buf[1024];
	PWCHAR  pMessage = NULL;
	ULONG	cchErrorTextSize;
	ULONG   nWritten;

	if (FAILED(StringCbVPrintfW(buf, sizeof(buf), fmt, args))) {
		perror("produce_message: StringCbVPrintfW");
		return;
	}

	cchErrorTextSize = FormatMessageW(
                FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                NULL,
                GetLastError(),
                MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                pMessage,
                0,
                NULL);

	if (cchErrorTextSize > 0) {
		if (FAILED(StringCbCat(buf, sizeof(buf), L": "))) {
			perror("produce_message: StringCbCat");
			LocalFree(pMessage);
			return;
		}
		if (FAILED(StringCbCat(buf, sizeof(buf), pMessage))) {
			perror("produce_message: StringCbCat");
			LocalFree(pMessage);
			return;
		}
	}
	if (FAILED(StringCbCat(buf, sizeof(buf), L"\n"))) {
		perror("produce_message: StringCbCat");
		LocalFree(pMessage);
		return;
	}

	if (STDERR != INVALID_HANDLE_VALUE) {
		// message for qrexec log in dom0
		errorf("produce_message: %s\n", buf);
		WriteFile(STDERR, buf, wcslen(buf) * sizeof(WCHAR), &nWritten, NULL);
	}
	MessageBoxW(NULL, buf, L"Qubes file copy error", MB_OK | icon); 
	LocalFree(pMessage);
}

void gui_fatal(const PWCHAR fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	produce_message(MB_ICONERROR, fmt, args);
	va_end(args);
	exit(1);
}

void gui_nonfatal(const PWCHAR fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	produce_message(MB_ICONWARNING, fmt, args);
	va_end(args);
}
