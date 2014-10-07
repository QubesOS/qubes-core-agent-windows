#include <windows.h>
#include <tchar.h>
#include <Shlwapi.h>
#include <strsafe.h>
#include <stdlib.h>
#include "utf8-conv.h"
#include "log.h"

#define CLIPBOARD_FORMAT CF_UNICODETEXT

#define MAX_CLIPBOARD_SIZE 65000

int ReadUntilEOF(HANDLE fd, void *buf, int size)
{
    int got_read = 0;
    int ret;

    while (got_read < size)
    {
        if (!ReadFile(fd, (char *) buf + got_read, size - got_read, &ret, NULL))
        {
            if (GetLastError() == ERROR_BROKEN_PIPE)
                return got_read;
            return -1;
        }
        if (ret == 0)
        {
            return got_read;
        }
        got_read += ret;
    }
    return got_read;
}

BOOL setClipboard(HWND hWin, HANDLE hInput)
{
    HANDLE hglb;
    PWCHAR pwszUtf16, pwszUtf16dest;
    UCHAR lpStr[MAX_CLIPBOARD_SIZE + 1];
    size_t cchwStr;
    int  uRead;

    uRead = ReadUntilEOF(hInput, lpStr, sizeof(lpStr) - 1);
    if (uRead < 0)
    {
        perror("ReadUntilEOF");
        return FALSE;
    }

    lpStr[uRead] = '\0';

    if (FAILED(ConvertUTF8ToUTF16(lpStr, &pwszUtf16, &cchwStr)))
    {
        perror("ConvertUTF8ToUTF16");
        return FALSE;
    }

    hglb = GlobalAlloc(GMEM_MOVEABLE, (cchwStr + 1) * sizeof(WCHAR));
    if (!hglb)
    {
        perror("GlobalAlloc");
        return FALSE;
    }

    pwszUtf16dest = GlobalLock(hglb);
    if (!pwszUtf16dest)
    {
        perror("GlobalLock");
        GlobalFree(hglb);
        return FALSE;
    }

    memcpy(pwszUtf16dest, pwszUtf16, (cchwStr + 1) * sizeof(WCHAR));
    free(pwszUtf16);

    GlobalUnlock(hglb);

    if (!OpenClipboard(hWin))
    {
        perror("OpenClipboard");
        GlobalFree(hglb);
        return FALSE;
    }

    if (!EmptyClipboard())
    {
        perror("EmptyClipboard");
        GlobalFree(hglb);
        CloseClipboard();
        return FALSE;
    }

    if (!SetClipboardData(CLIPBOARD_FORMAT, hglb))
    {
        perror("SetClipboardData");
        GlobalFree(hglb);
        CloseClipboard();
        return FALSE;
    }

    CloseClipboard();
    return TRUE;
}

HWND createMainWindow(HINSTANCE hInst)
{
    WNDCLASSEX wc;
    ATOM windowClass;

    wc.cbSize = sizeof(wc);
    wc.style = 0;
    wc.lpfnWndProc = (WNDPROC) DefWindowProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = hInst;
    wc.hIcon = NULL;
    wc.hCursor = NULL;
    wc.hbrBackground = (HBRUSH) COLOR_WINDOW;
    wc.lpszMenuName = NULL;
    wc.lpszClassName = TEXT("MainWindowClass");
    wc.hIconSm = NULL;

    windowClass = RegisterClassEx(&wc);
    if (!windowClass)
    {
        perror("RegisterClassEx");
        return NULL;
    }

    return CreateWindow(
        wc.lpszClassName, /* class */
        TEXT("Qubes clipboard service"), /* name */
        WS_OVERLAPPEDWINDOW, /* style */
        CW_USEDEFAULT, CW_USEDEFAULT, /* x,y */
        10, 10, /* w, h */
        HWND_MESSAGE, /* parent */
        NULL, /* menu */
        hInst, /* instance */
        NULL);
}

int APIENTRY _tWinMain(HINSTANCE hInst, HINSTANCE hPrevInst, LPTSTR lpCommandLine, int nCmdShow)
{
    HANDLE hStdIn;
    HWND hWin;

    hStdIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hStdIn == INVALID_HANDLE_VALUE)
    {
        perror("GetStdHandle");
        return 1;
    }

    hWin = createMainWindow(hInst);
    if (!hWin)
    {
        perror("createMainWindow");
        return 1;
    }

    if (!setClipboard(hWin, hStdIn))
    {
        return 1;
    }

    DestroyWindow(hWin);
    UnregisterClass(TEXT("MainWindowClass"), hInst);

    return 0;
}
