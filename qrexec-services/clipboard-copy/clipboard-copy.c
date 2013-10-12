#include <windows.h>
#include <tchar.h>
#include <Shlwapi.h>
#include <strsafe.h>
#include <stdlib.h>
#include "utf8-conv.h"
#include "log.h"

#define CLIPBOARD_FORMAT CF_UNICODETEXT

int write_all(HANDLE fd, void *buf, int size)
{
    int written = 0;
    DWORD ret;
    while (written < size) {
        if (!WriteFile(fd, (char *) buf + written, size - written, &ret, NULL)) {
            perror("write_all: WriteFile");
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
	
	if (!IsClipboardFormatAvailable(CLIPBOARD_FORMAT)) {
		perror("IsClipboardFormatAvailable");
		return FALSE;
	}

	if (!OpenClipboard(hWin)) {
		perror("OpenClipboard");
		return FALSE;
	}

	hglb = GetClipboardData(CLIPBOARD_FORMAT);
	if (!hglb) {
		perror("GetClipboardData");
		CloseClipboard();
		return FALSE;
	}

	lpwstr = GlobalLock(hglb);
	if (!lpwstr) {
		perror("GlobalLock");
		CloseClipboard();
		return FALSE;
	}

	if (FAILED(ConvertUTF16ToUTF8(lpwstr, &lpstr, &cbStr))) {
		errorf("ConvertUTF16ToUTF8 failed\n");
		GlobalUnlock(hglb);
		CloseClipboard();
		return FALSE;
	}

	if (!write_all(hOutput, lpstr, cbStr)) {
		errorf("write_all failed\n");
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

	log_init(NULL, TEXT("clipboard-copy"));

	hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
	if (hStdOut == INVALID_HANDLE_VALUE) {
		perror("GetStdHandle");
		return 1;
	}
	if (!getClipboard(NULL, hStdOut)) {
		errorf("getClipboard failed\n");
		return 1;
	}

	return 0; 	
}
