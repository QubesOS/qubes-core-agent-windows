#include <windows.h>
#include <tchar.h>
#include <Shlwapi.h>
#include <strsafe.h>
#include <stdlib.h>
#include "utf8-conv.h"

#define CLIPBOARD_FORMAT CF_UNICODETEXT

int write_all(HANDLE fd, void *buf, int size)
{
    int written = 0;
    DWORD ret;
    while (written < size) {
        if (!WriteFile(fd, (char *) buf + written, size - written, &ret, NULL)) {
            // some error handler?
            return 0;
        }
        written += ret;
    }
    return 1;
}

BOOL getClipboard(HWND hWin, HANDLE hOutput)
{
	HANDLE hglb;
	PWCHAR lpwstr;
	char *lpstr;
	size_t cbStr;
	
	if (!IsClipboardFormatAvailable(CLIPBOARD_FORMAT))
		return FALSE;

	if (!OpenClipboard(hWin))
		return FALSE;

	hglb = GetClipboardData(CLIPBOARD_FORMAT);
	if (!hglb) {
		CloseClipboard();
		return FALSE;
	}

	lpwstr = GlobalLock(hglb);
	if (!lpwstr) {
		CloseClipboard();
		return FALSE;
	}

	if (FAILED(ConvertUTF16ToUTF8(lpwstr, &lpstr, &cbStr))) {
		GlobalUnlock(hglb);
		CloseClipboard();
		return FALSE;
	}

	if (!write_all(hOutput, lpstr, cbStr)) {
		// some error handler?
		GlobalUnlock(hglb); 
		CloseClipboard();
		return FALSE;
	}

	GlobalUnlock(hglb); 
	CloseClipboard(); 
	return TRUE;
}

int APIENTRY _tWinMain(HINSTANCE hInst, HINSTANCE hPrevInst, LPTSTR lpCommandLine, int nCmdShow)
{
	HANDLE hStdOut;

	hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
	if (hStdOut == INVALID_HANDLE_VALUE) {
		// some error handler?
		return 1;
	}
	if (!getClipboard(NULL, hStdOut)) {
		// some error handler?
		return 1;
	}

	return 0; 	
}
