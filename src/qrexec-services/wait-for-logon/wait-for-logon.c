/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (c) Invisible Things Lab
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include <Windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <Wtsapi32.h>
#include <Shlwapi.h>

#include <qubes-io.h>
#include <utf8-conv.h>
#include <log.h>

WCHAR *g_expectedUser;
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
        perror("RegisterClassEx");
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

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE hPrevInst, WCHAR *pszCommandLine, int nCmdShow)
{
    HWND hMainWindow;
    size_t cbExpectedUserUtf8;
    size_t cchExpectedUser = 0;
    UCHAR pszExpectedUserUtf8[USERNAME_LENGTH + 1];
    HANDLE hStdIn;

    LogVerbose("start");
    // first try command parameter
    g_expectedUser = PathGetArgs(pszCommandLine);

    // if none was given, read from stdin
    if (!g_expectedUser || g_expectedUser[0] == TEXT('\0'))
    {
        LogDebug("reading user from stdin");
        hStdIn = GetStdHandle(STD_INPUT_HANDLE);
        if (hStdIn == INVALID_HANDLE_VALUE)
        {
            perror("GetStdHandle");
            return 1;
        }

        cbExpectedUserUtf8 = QioReadUntilEof(hStdIn, pszExpectedUserUtf8, sizeof(pszExpectedUserUtf8));
        if (cbExpectedUserUtf8 == 0)
        {
            LogError("Failed to read the user name");
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

        if (ERROR_SUCCESS != ConvertUTF8ToUTF16(pszExpectedUserUtf8, &g_expectedUser, &cchExpectedUser))
        {
            perror("Converting user name to UTF16");
            return 1;
        }
    }

    LogDebug("creating window");
    hMainWindow = CreateMainWindow(hInst);

    if (hMainWindow == INVALID_HANDLE_VALUE)
    {
        perror("Creating main window");
        return 1;
    }

    if (!WTSRegisterSessionNotification(hMainWindow, NOTIFY_FOR_ALL_SESSIONS))
    {
        perror("WTSRegisterSessionNotification");
        DestroyWindow(hMainWindow);
        return 1;
    }

    g_sessionFound = FALSE;
    if (!CheckIfUserLoggedIn())
    {
        BOOL bRet;
        MSG msg;

        LogDebug("Waiting for user '%s'", g_expectedUser);
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

    if (cchExpectedUser)
        free(g_expectedUser);
    return 0;
}
