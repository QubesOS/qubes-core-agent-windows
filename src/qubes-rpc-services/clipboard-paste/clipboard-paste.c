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

#include <windows.h>
#include <shlwapi.h>
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
    char *inputText;
    size_t cchText;
    DWORD cbRead;

    inputText = malloc(MAX_CLIPBOARD_SIZE + 1);
    if (!inputText)
        goto fail;

    ZeroMemory(inputText, MAX_CLIPBOARD_SIZE + 1);

    cbRead = QioReadUntilEof(inputFile, inputText, MAX_CLIPBOARD_SIZE);
    if (cbRead == 0)
    {
        LogError("QioReadUntilEof returned 0");
        goto fail;
    }

    inputText[cbRead] = '\0';

    if (ERROR_SUCCESS != ConvertUTF8ToUTF16(inputText, &text, &cchText))
    {
        win_perror("ConvertUTF8ToUTF16");
        goto fail;
    }

    clipData = GlobalAlloc(GMEM_MOVEABLE, (cchText + 1) * sizeof(WCHAR));
    if (!clipData)
    {
        win_perror("GlobalAlloc");
        goto fail;
    }

    textLocked = GlobalLock(clipData);
    if (!textLocked)
    {
        win_perror("GlobalLock");
        GlobalFree(clipData);
        goto fail;
    }

    memcpy(textLocked, text, (cchText + 1) * sizeof(WCHAR));
    free(text);

    GlobalUnlock(clipData);

    if (!OpenClipboard(window))
    {
        win_perror("OpenClipboard");
        GlobalFree(clipData);
        goto fail;
    }

    if (!EmptyClipboard())
    {
        win_perror("EmptyClipboard");
        GlobalFree(clipData);
        CloseClipboard();
        goto fail;
    }

    if (!SetClipboardData(CLIPBOARD_FORMAT, clipData))
    {
        win_perror("SetClipboardData");
        GlobalFree(clipData);
        CloseClipboard();
        goto fail;
    }

    CloseClipboard();
    return TRUE;

fail:
    free(inputText);
    return FALSE;
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
        win_perror("RegisterClassEx");
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
        return win_perror("GetStdHandle");
    }

    window = CreateMainWindow(instance);
    if (!window)
    {
        return win_perror("createMainWindow");
    }

    if (!ReadClipboardText(window, stdIn))
    {
        return GetLastError();
    }

    DestroyWindow(window);
    UnregisterClass(L"MainWindowClass", instance);

    return 0;
}
