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
#include <Shlwapi.h>
#include <strsafe.h>
#include <stdlib.h>

#include <qubes-io.h>
#include <utf8-conv.h>
#include <log.h>

#define CLIPBOARD_FORMAT CF_UNICODETEXT

BOOL WriteClipboardText(IN HWND window, OUT HANDLE outputFile)
{
    HANDLE clipData;
    WCHAR *clipText;
    UCHAR *clipTextUtf8;
    size_t cbTextUtf8;

    if (!IsClipboardFormatAvailable(CLIPBOARD_FORMAT))
        return FALSE;

    if (!OpenClipboard(window))
    {
        perror("OpenClipboard");
        return FALSE;
    }

    clipData = GetClipboardData(CLIPBOARD_FORMAT);
    if (!clipData)
    {
        perror("GetClipboardData");
        CloseClipboard();
        return FALSE;
    }

    clipText = GlobalLock(clipData);
    if (!clipText)
    {
        perror("GlobalLock");
        CloseClipboard();
        return FALSE;
    }

    if (FAILED(ConvertUTF16ToUTF8(clipText, &clipTextUtf8, &cbTextUtf8)))
    {
        perror("ConvertUTF16ToUTF8");
        GlobalUnlock(clipData);
        CloseClipboard();
        return FALSE;
    }

    if (!QioWriteBuffer(outputFile, clipTextUtf8, (DWORD)cbTextUtf8))
    {
        perror("QioWriteBuffer");
        GlobalUnlock(clipData);
        CloseClipboard();
        return FALSE;
    }

    GlobalUnlock(clipData);
    CloseClipboard();
    return TRUE;
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE previousInstance, WCHAR *commandLine, int showFlags)
{
    HANDLE stdOut;

    stdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (stdOut == INVALID_HANDLE_VALUE)
    {
        perror("GetStdHandle");
        return 1;
    }
    if (!WriteClipboardText(NULL, stdOut))
    {
        return 1;
    }

    LogDebug("all ok");
    return 0;
}
