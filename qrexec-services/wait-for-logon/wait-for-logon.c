#include <Windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <Wtsapi32.h>
#include <Shlwapi.h>

#include "utf8-conv.h"
#include "log.h"

TCHAR *g_expectedUser;
BOOL g_sessionFound = FALSE;

BOOL CheckSession(DWORD	sessionId)
{
    WCHAR *userName;
    DWORD cbUserName;

    if (!WTSQuerySessionInformation(WTS_CURRENT_SERVER_HANDLE, sessionId, WTSUserName, &userName, &cbUserName))
    {
        perror("WTSQuerySessionInformation");
        return FALSE;
    }

    LogDebug("Found session: %s", userName);

    if (wcscmp(userName, g_expectedUser) == 0)
    {
        g_sessionFound = TRUE;
    }

    WTSFreeMemory(userName);
    return g_sessionFound;
}

BOOL CheckIfUserLoggedIn(void)
{
    WTS_SESSION_INFO *sessionInfo;
    DWORD sessionCount;
    DWORD sessionIndex;

    if (!WTSEnumerateSessions(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessionInfo, &sessionCount))
    {
        perror("WTSEnumerateSessions");
        return FALSE;
    }

    for (sessionIndex = 0; sessionIndex < sessionCount; sessionIndex++)
    {
        if (sessionInfo[sessionIndex].State == WTSActive)
        {
            if (CheckSession(sessionInfo[sessionIndex].SessionId))
            {
                g_sessionFound = TRUE;
            }
        }
    }

    WTSFreeMemory(sessionInfo);
    return g_sessionFound;
}

LRESULT CALLBACK WindowProc(
    HWND window,     // handle to window
    UINT message,    // WM_WTSSESSION_CHANGE
    WPARAM wParam,   // session state change event
    LPARAM lParam    // session ID
    )
{
    switch (message)
    {
    case WM_WTSSESSION_CHANGE:
        if (wParam == WTS_SESSION_LOGON)
        {
            if (CheckSession((DWORD) lParam))
                g_sessionFound = TRUE;
        }
        return TRUE;

    default:
        return DefWindowProc(window, message, wParam, lParam);
    }

    return FALSE;
}

HWND CreateMainWindow(HINSTANCE instance)
{
    WNDCLASSEX wc;
    ATOM windowClassAtom;

    wc.cbSize = sizeof(wc);
    wc.style = 0;
    wc.lpfnWndProc = (WNDPROC) WindowProc;
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
        return NULL;
    }

    return CreateWindow(
        wc.lpszClassName, /* class */
        TEXT("Qubes session wait service"), /* name */
        WS_OVERLAPPEDWINDOW, /* style */
        CW_USEDEFAULT, CW_USEDEFAULT, /* x,y */
        10, 10, /* w, h */
        HWND_MESSAGE, /* parent */
        NULL, /* menu */
        instance, /* instance */
        NULL);
}

int FcReadUntilEof(HANDLE fd, void *buf, int size)
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

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE hPrevInst, WCHAR *pszCommandLine, int nCmdShow)
{
    HWND hMainWindow;
    size_t cbExpectedUserUtf8;
    size_t cchExpectedUser = 0;
    UCHAR pszExpectedUserUtf8[USERNAME_LENGTH + 1];
    HANDLE hStdIn;

    // first try command parameter
    g_expectedUser = PathGetArgs(pszCommandLine);

    // if none was given, read from stdin
    if (!g_expectedUser || g_expectedUser[0] == TEXT('\0'))
    {
        hStdIn = GetStdHandle(STD_INPUT_HANDLE);
        if (hStdIn == INVALID_HANDLE_VALUE)
        {
            // some error handler?
            return 1;
        }

        cbExpectedUserUtf8 = FcReadUntilEof(hStdIn, pszExpectedUserUtf8, sizeof(pszExpectedUserUtf8));
        if (cbExpectedUserUtf8 <= 0 || cbExpectedUserUtf8 >= sizeof(pszExpectedUserUtf8))
        {
            fprintf(stderr, "Failed to read the user name\n");
            return 1;
        }

        // strip end of line and spaces
        // FIXME: what about multibyte char with one byte '\r'/'\n'/' '?
        while (cbExpectedUserUtf8 > 0 &&
            (pszExpectedUserUtf8[cbExpectedUserUtf8 - 1] == '\n' ||
            pszExpectedUserUtf8[cbExpectedUserUtf8 - 1] == '\r' ||
            pszExpectedUserUtf8[cbExpectedUserUtf8 - 1] == ' '))
            cbExpectedUserUtf8--;
        pszExpectedUserUtf8[cbExpectedUserUtf8] = '\0';

#ifdef UNICODE
        if (FAILED(ConvertUTF8ToUTF16(pszExpectedUserUtf8, &g_expectedUser, &cchExpectedUser)))
        {
            fprintf(stderr, "Failed to convert the user name to UTF16\n");
            return 1;
        }
#else
        g_expectedUser = pszExpectedUserUtf8;
        cchExpectedUser = cbExpectedUserUtf8;
#endif
    }

    hMainWindow = CreateMainWindow(hInst);

    if (hMainWindow == INVALID_HANDLE_VALUE)
    {
        fprintf(stderr, "Failed to create main window: 0x%x\n", GetLastError());
        return 1;
    }

    if (FAILED(WTSRegisterSessionNotification(hMainWindow, NOTIFY_FOR_ALL_SESSIONS)))
    {
        fprintf(stderr, "Failed to register session notification: 0x%x\n", GetLastError());
        DestroyWindow(hMainWindow);
        return 1;
    }

    g_sessionFound = FALSE;
    if (!CheckIfUserLoggedIn())
    {
        BOOL bRet;
        MSG msg;

#ifdef DBG
# ifdef UNICODE
        fprintf(stderr, "Waiting for user %S\n", g_expectedUser);
# else
        fprintf(stderr, "Waiting for user %s\n", g_expectedUser);
# endif
#endif
        while ((bRet = GetMessage(&msg, hMainWindow, 0, 0)) != 0)
        {
            if (bRet == -1)
            {
                // handle the error and possibly exit
                break;
            }
            else
            {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            if (g_sessionFound)
                break;
        }
    }

    WTSUnRegisterSessionNotification(hMainWindow);
    DestroyWindow(hMainWindow);

#ifdef UNICODE
    if (cchExpectedUser)
        free(g_expectedUser);
#endif
    return 0;
}
