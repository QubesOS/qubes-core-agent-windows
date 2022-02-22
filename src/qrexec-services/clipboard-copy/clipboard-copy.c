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

static BOOL PrepareClipText(IN WCHAR *clipText, OUT WCHAR **clipTextPrepared)
{
    if (!clipText || !clipTextPrepared)
        return FALSE;
    // replace \r\n -> \r
    *clipTextPrepared = (WCHAR*)calloc((wcslen(clipText) + 1) * sizeof(WCHAR), 1);
    if (!*clipTextPrepared)
        return FALSE;
    WCHAR* src = clipText;
    WCHAR* dst = *clipTextPrepared;
    size_t strLen = wcslen(clipText);
    for (size_t i = 0; i < strLen; i++)
    {
        if ((*src) != L'\r')
        {
            *dst++ = *src;
        }
        src++;
    }
    *dst = 0;
    return TRUE;
}

static BOOL WriteClipboardText(IN HWND window, OUT HANDLE outputFile)
{
    HANDLE clipData;
    WCHAR *clipText;
    WCHAR *clipTextPrepared;
    CHAR *clipTextUtf8;
    size_t cbTextUtf8;

    if (!IsClipboardFormatAvailable(CLIPBOARD_FORMAT))
        return FALSE;

    if (!OpenClipboard(window))
    {
        win_perror("OpenClipboard");
        return FALSE;
    }

    clipData = GetClipboardData(CLIPBOARD_FORMAT);
    if (!clipData)
    {
        win_perror("GetClipboardData");
        CloseClipboard();
        return FALSE;
    }

    clipText = GlobalLock(clipData);
    if (!clipText)
    {
        win_perror("GlobalLock");
        CloseClipboard();
        return FALSE;
    }

    if (!PrepareClipText(clipText, &clipTextPrepared))
    {
        win_perror("PrepareClipText");
        free(clipTextPrepared);
        CloseClipboard();
        GlobalUnlock(clipData);
        return FALSE;
    }

    CloseClipboard();
    GlobalUnlock(clipData);

    if (FAILED(ConvertUTF16ToUTF8(clipTextPrepared, &clipTextUtf8, &cbTextUtf8)))
    {
        win_perror("ConvertUTF16ToUTF8");
        free(clipTextPrepared);
        free(clipTextUtf8);
        return FALSE;
    }

    if (!QioWriteBuffer(outputFile, clipTextUtf8, (DWORD)cbTextUtf8))
    {
        win_perror("QioWriteBuffer");
        free(clipTextPrepared);
        free(clipTextUtf8);
        return FALSE;
    }

    free(clipTextPrepared);
    free(clipTextUtf8);
    return TRUE;
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE previousInstance, WCHAR *commandLine, int showFlags)
{
    HANDLE stdOut;

    stdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (stdOut == INVALID_HANDLE_VALUE)
    {
        win_perror("GetStdHandle");
        return 1;
    }
    if (!WriteClipboardText(NULL, stdOut))
    {
        return 1;
    }

    LogDebug("all ok");
    return 0;
}
