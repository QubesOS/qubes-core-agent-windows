#include <windows.h>
#include <tchar.h>
#include <Shlwapi.h>
#include <strsafe.h>
#include <stdlib.h>
#include "utf8-conv.h"
#include "log.h"

#define CLIPBOARD_FORMAT CF_UNICODETEXT

int write_all(HANDLE fd, void *buf, DWORD size)
{
    DWORD written = 0;
    int ret;

    while (written < size)
    {
        if (!WriteFile(fd, (BYTE *) buf + written, size - written, &ret, NULL))
        {
            perror("WriteFile");
            return 0;
        }
        written += ret;
    }
    return 1;
}

BOOL getClipboard(HWND hWin, HANDLE hOutput)
{
    HANDLE hglb;
    WCHAR *lpwstr;
    UCHAR *lpstr;
    size_t cbStr;

    if (!IsClipboardFormatAvailable(CLIPBOARD_FORMAT))
        return FALSE;

    if (!OpenClipboard(hWin))
    {
        perror("OpenClipboard");
        return FALSE;
    }

    hglb = GetClipboardData(CLIPBOARD_FORMAT);
    if (!hglb)
    {
        perror("GetClipboardData");
        CloseClipboard();
        return FALSE;
    }

    lpwstr = GlobalLock(hglb);
    if (!lpwstr)
    {
        perror("GlobalLock");
        CloseClipboard();
        return FALSE;
    }

    if (FAILED(ConvertUTF16ToUTF8(lpwstr, &lpstr, &cbStr)))
    {
        perror("ConvertUTF16ToUTF8");
        GlobalUnlock(hglb);
        CloseClipboard();
        return FALSE;
    }

    if (!write_all(hOutput, lpstr, cbStr))
    {
        LogError("write failed");
        GlobalUnlock(hglb);
        CloseClipboard();
        return FALSE;
    }

    GlobalUnlock(hglb);
    CloseClipboard();
    return TRUE;
}

int APIENTRY _tWinMain(HINSTANCE hInst, HINSTANCE hPrevInst, TCHAR *lpCommandLine, int nCmdShow)
{
    HANDLE hStdOut;

    hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hStdOut == INVALID_HANDLE_VALUE)
    {
        perror("GetStdHandle");
        return 1;
    }
    if (!getClipboard(NULL, hStdOut))
    {
        return 1;
    }

    LogDebug("all ok");
    return 0;
}
