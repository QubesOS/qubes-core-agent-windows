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
#include <stdio.h>
#include <stdlib.h>
#include <strsafe.h>

#include <log.h>

#include "filecopy-error.h"

HWND g_parentWindow;

fErrorCallback g_ErrorCallback;

void FcSetErrorCallback(IN HWND dialog, IN fErrorCallback callback)
{
    g_parentWindow = dialog;
    g_ErrorCallback = callback;
}

static void ProduceMessage(IN DWORD errorCode, IN DWORD icon, IN const WCHAR *format, IN va_list args)
{
    WCHAR buffer[1024];
    WCHAR *message = NULL;
    ULONG cchErrorText;
    HRESULT hresult;

    hresult = StringCchVPrintf(buffer, RTL_NUMBER_OF(buffer), format, args);
    if (FAILED(hresult))
    {
        LogError("StringCchVPrintf failed: 0x%x", hresult);
        /* FIXME: some fallback method? */
        return;
    }

    cchErrorText = FormatMessage(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        errorCode,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        message,
        0,
        NULL);

    if (cchErrorText > 0)
    {
        hresult = StringCchCat(buffer, RTL_NUMBER_OF(buffer), L": ");
        if (FAILED(hresult))
        {
            LogError("StringCchCat failed: 0x%x", hresult);
            LocalFree(message);
            return;
        }

        hresult = StringCchCat(buffer, RTL_NUMBER_OF(buffer), message);
        if (FAILED(hresult))
        {
            LogError("StringCchCat failed: 0x%x", hresult);
            LocalFree(message);
            return;
        }
    }

    hresult = StringCchCat(buffer, RTL_NUMBER_OF(buffer), L"\n");
    if (FAILED(hresult))
    {
        LogError("StringCchCat failed: 0x%x", hresult);
        LocalFree(message);
        return;
    }

    // message for qrexec log in dom0
    fwprintf(stderr, L"%s", buffer);
    MessageBox(g_parentWindow, buffer, L"Qubes file copy error", MB_OK | icon);
    LocalFree(message);
}

void FcReportError(IN DWORD errorCode, IN BOOL fatal, IN const WCHAR *format, ...)
{
    va_list args;

    g_ErrorCallback(TRUE);
    va_start(args, format);
    ProduceMessage(errorCode, fatal ? MB_ICONERROR : MB_ICONWARNING, format, args);
    va_end(args);

    if (fatal)
    {
        exit(1);
    }
    else
    {
        g_ErrorCallback(FALSE);
    }
}
