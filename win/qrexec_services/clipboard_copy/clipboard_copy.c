#include <windows.h>
#include <tchar.h>
#include <Shlwapi.h>
#include <strsafe.h>
#include <stdlib.h>

#define CLIPBOARD_FORMAT CF_UNICODETEXT

int write_all(HANDLE fd, void *buf, int size)
{
    int written = 0;
    int ret;
    while (written < size) {
        if (!WriteFile(fd, (char *) buf + written, size - written, &ret, NULL)) {
            // some error handler?
            return 0;
        }
        written += ret;
    }
    return 1;
}

PUCHAR ConvertUTF16ToUTF8(PWCHAR pwszUtf16, size_t *pcbUtf8) {
	PUCHAR pszUtf8;
	size_t cbUtf8;

	/* convert filename from UTF-16 to UTF-8 */
	/* calculate required size */
	cbUtf8 = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, pwszUtf16, -1, NULL, 0, NULL, NULL);
	if (!cbUtf8) {
		return NULL;
	}
	pszUtf8 = malloc(sizeof(PUCHAR)*cbUtf8);
	if (!pszUtf8) {
		return NULL;
	}
	if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, pwszUtf16, -1, pszUtf8, cbUtf8, NULL, NULL)) {
		free(pszUtf8);
		return NULL;
	}
	*pcbUtf8 = cbUtf8 - 1; /* without terminating NULL character */
	return pszUtf8;
}

BOOL getClipboard(HWND hWin, HANDLE hOutput)
{
	HANDLE hglb;
	PWCHAR lpwstr;
	PUCHAR lpstr;
	size_t cbStr;
	ULONG  uWritten;

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

	lpstr = ConvertUTF16ToUTF8(lpwstr, &cbStr);
	if (!lpstr) {
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
