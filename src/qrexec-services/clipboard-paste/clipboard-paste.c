#include <windows.h>
#include <Shlwapi.h>
#include <strsafe.h>
#include <stdlib.h>

#include <qubes-io.h>
#include <utf8-conv.h>
#include <log.h>

#define CLIPBOARD_FORMAT CF_UNICODETEXT

#define MAX_CLIPBOARD_SIZE 65000

BOOL ReadClipboardText(IN HWND window, IN HANDLE inputFile)
{
    HANDLE clipData;
    WCHAR *text, *textLocked;
    char inputText[MAX_CLIPBOARD_SIZE + 1] = { 0 };
    size_t cchText;
    DWORD cbRead;

    cbRead = QioReadUntilEof(inputFile, inputText, sizeof(inputText) - 1);
    if (cbRead == 0)
    {
        LogError("QioReadUntilEof returned 0");
        return FALSE;
    }

    inputText[cbRead] = '\0';

    if (ERROR_SUCCESS != ConvertUTF8ToUTF16(inputText, &text, &cchText))
    {
        perror("ConvertUTF8ToUTF16");
        return FALSE;
    }

    clipData = GlobalAlloc(GMEM_MOVEABLE, (cchText + 1) * sizeof(WCHAR));
    if (!clipData)
    {
        perror("GlobalAlloc");
        return FALSE;
    }

    textLocked = GlobalLock(clipData);
    if (!textLocked)
    {
        perror("GlobalLock");
        GlobalFree(clipData);
        return FALSE;
    }

    memcpy(textLocked, text, (cchText + 1) * sizeof(WCHAR));
    free(text);

    GlobalUnlock(clipData);

    if (!OpenClipboard(window))
    {
        perror("OpenClipboard");
        GlobalFree(clipData);
        return FALSE;
    }

    if (!EmptyClipboard())
    {
        perror("EmptyClipboard");
        GlobalFree(clipData);
        CloseClipboard();
        return FALSE;
    }

    if (!SetClipboardData(CLIPBOARD_FORMAT, clipData))
    {
        perror("SetClipboardData");
        GlobalFree(clipData);
        CloseClipboard();
        return FALSE;
    }

    CloseClipboard();
    return TRUE;
}

HWND CreateMainWindow(IN HINSTANCE instance)
{
    WNDCLASSEX wc;
    ATOM windowClassAtom;

    wc.cbSize = sizeof(wc);
    wc.style = 0;
    wc.lpfnWndProc = (WNDPROC) DefWindowProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = instance;
    wc.hIcon = NULL;
    wc.hCursor = NULL;
    wc.hbrBackground = (HBRUSH) COLOR_WINDOW;
    wc.lpszMenuName = NULL;
    wc.lpszClassName = L"MainWindowClass";
    wc.hIconSm = NULL;

    windowClassAtom = RegisterClassEx(&wc);
    if (!windowClassAtom)
    {
        perror("RegisterClassEx");
        return NULL;
    }

    return CreateWindow(
        wc.lpszClassName, /* class */
        L"Qubes clipboard service", /* name */
        WS_OVERLAPPEDWINDOW, /* style */
        CW_USEDEFAULT, CW_USEDEFAULT, /* x,y */
        10, 10, /* w, h */
        HWND_MESSAGE, /* parent */
        NULL, /* menu */
        instance, /* instance */
        NULL);
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE previousInstance, WCHAR *commandLine, int showFlags)
{
    HANDLE stdIn;
    HWND window;

    stdIn = GetStdHandle(STD_INPUT_HANDLE);
    if (stdIn == INVALID_HANDLE_VALUE)
    {
        return perror("GetStdHandle");
    }

    window = CreateMainWindow(instance);
    if (!window)
    {
        return perror("createMainWindow");
    }

    if (!ReadClipboardText(window, stdIn))
    {
        return GetLastError();
    }

    DestroyWindow(window);
    UnregisterClass(L"MainWindowClass", instance);

    return 0;
}
