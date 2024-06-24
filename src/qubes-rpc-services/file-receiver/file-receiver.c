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
#include <PathCch.h>
#include <stdio.h>
#include <stdlib.h>
#include <shlobj.h>
#include <strsafe.h>

#include <exec.h>
#include <log.h>
#include <qubes-io.h>

HANDLE g_stdin = INVALID_HANDLE_VALUE;
HANDLE g_stdout = INVALID_HANDLE_VALUE;
HANDLE g_stderr = INVALID_HANDLE_VALUE;

// FIXME hardcoded path
#define INCOMING_DIR_ROOT L"QubesIncoming"

int ReceiveFiles(IN const WCHAR* incomingDir);

int __cdecl wmain(int argc, WCHAR *argv[])
{
    g_stderr = GetStdHandle(STD_ERROR_HANDLE);
    if (g_stderr == NULL || g_stderr == INVALID_HANDLE_VALUE)
    {
        return win_perror("GetStdHandle(STD_ERROR_HANDLE)");
    }

    g_stdin = GetStdHandle(STD_INPUT_HANDLE);
    if (g_stdin == NULL || g_stdin == INVALID_HANDLE_VALUE)
    {
        return win_perror("GetStdHandle(STD_INPUT_HANDLE)");
    }

    g_stdout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (g_stdout == NULL || g_stdout == INVALID_HANDLE_VALUE)
    {
        return win_perror("GetStdHandle(STD_OUTPUT_HANDLE)");
    }

    WCHAR remoteDomainName[64];
    if (!GetEnvironmentVariable(L"QREXEC_REMOTE_DOMAIN", remoteDomainName, ARRAYSIZE(remoteDomainName)))
    {
        return win_perror("GetEnvironmentVariable(QREXEC_REMOTE_DOMAIN)");
    }

    WCHAR* incomingDir = malloc(MAX_PATH_LONG_WSIZE);
    if (!incomingDir)
        return ERROR_OUTOFMEMORY;

    DWORD status;
    WCHAR* arg = GetArgument();
    if (arg)
    {
        if (FAILED(status = StringCchPrintf(incomingDir, MAX_PATH_LONG, L"%s", arg)))
            return win_perror2(status, "formatting incoming dir path");
    }
    else
    {
        WCHAR* documentsPath = NULL;
        status = SHGetKnownFolderPath(&FOLDERID_Documents, KF_FLAG_CREATE, NULL, &documentsPath);
        if (FAILED(status))
            return win_perror2(status, "getting Documents path");

        LogDebug("Documents: %s", documentsPath);

        if (FAILED(status = StringCchCopy(incomingDir, MAX_PATH_LONG, documentsPath)))
            return win_perror2(status, "formatting incoming dir path");

        CoTaskMemFree(documentsPath);

        if (FAILED(status = PathCchAppendEx(incomingDir, MAX_PATH_LONG, INCOMING_DIR_ROOT, PATHCCH_ALLOW_LONG_PATHS)))
            return win_perror2(status, "formatting incoming dir path");
    }

    LogDebug("Incoming dir: %s", incomingDir);
    BOOL success = CreateDirectory(incomingDir, NULL);
    if (!success && (status = GetLastError()) != ERROR_ALREADY_EXISTS)
        return win_perror2(status, "creating incoming directory");

    if (FAILED(status = PathCchAppendEx(incomingDir, MAX_PATH_LONG, remoteDomainName, PATHCCH_ALLOW_LONG_PATHS)))
        return win_perror2(status, "Formatting incoming dir path");

    success = CreateDirectory(incomingDir, NULL);
    if (!success && (status = GetLastError()) != ERROR_ALREADY_EXISTS)
        return win_perror2(status, "creating incoming directory");

    return ReceiveFiles(incomingDir);
}
