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
#include <Shlwapi.h>
#include <strsafe.h>
#include <wtsapi32.h>

#include <exec.h>
#include <qubesdb-client.h>
#include <log.h>
#include <config.h>
#include <qubes-io.h>

// FIXME this should be in qubesdb
#define QDB_PATH_PREFIX "/qubes-tools/"

// userName needs to be freed with WtsFreeMemory
BOOL GetCurrentUser(OUT char **userName)
{
    WTS_SESSION_INFOA *sessionInfo;
    DWORD sessionCount;
    DWORD i;
    DWORD cbUserName;
    BOOL found;

    LogVerbose("start");

    if (!WTSEnumerateSessionsA(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessionInfo, &sessionCount))
    {
        win_perror("WTSEnumerateSessionsA");
        return FALSE;
    }

    found = FALSE;

    for (i = 0; i < sessionCount; i++)
    {
        if (sessionInfo[i].State == WTSActive)
        {
            if (!WTSQuerySessionInformationA(
                WTS_CURRENT_SERVER_HANDLE,
                sessionInfo[i].SessionId, WTSUserName,
                userName,
                &cbUserName))
            {
                win_perror("WTSQuerySessionInformationA");
                goto cleanup;
            }
            LogDebug("Found session: %S\n", *userName);
            found = TRUE;
        }
    }

cleanup:
    WTSFreeMemory(sessionInfo);

    LogVerbose("found=%d", found);

    return found;
}

// append a binary exe name to the tools installation directory
BOOL PrepareExePath(OUT WCHAR *fullPath, DWORD pathSize, IN const WCHAR *exeName)
{
    DWORD status = CfgReadString(NULL, L"InstallDir", fullPath, pathSize, NULL);
    if (status != ERROR_SUCCESS)
    {
        win_perror2(status, "CfgReadString(InstallDir)");
        return FALSE;
    }

    LogVerbose("exe: '%s', install dir: '%s'", exeName, fullPath);

    if (FAILED(status = PathCchAppendEx(fullPath, pathSize, L"bin", PATHCCH_ALLOW_LONG_PATHS)))
    {
        win_perror2(status, "appending bin to path");
        return FALSE;
    }
    if (FAILED(status = PathCchAppendEx(fullPath, pathSize, exeName, PATHCCH_ALLOW_LONG_PATHS)))
    {
        win_perror2(status, "appending exe name to path");
        return FALSE;
    }

    LogVerbose("success, path: '%s'", fullPath);

    return TRUE;
}

/* TODO - make this configurable? */
BOOL CheckGuiAgentPresence(void)
{
    // this is a one-shot program, no need to cleanup allocations
    WCHAR* serviceFilePath = malloc(MAX_PATH_LONG_WSIZE);
    if (!serviceFilePath)
        return FALSE;

    LogVerbose("start");

    // FIXME hardcoded path
    if (!PrepareExePath(serviceFilePath, MAX_PATH_LONG_WSIZE, L"gui-agent.exe"))
        return FALSE;

    return PathFileExists(serviceFilePath);
}

BOOL NotifyDom0(void)
{
    STARTUPINFO si = { 0 };
    PROCESS_INFORMATION pi;
    WCHAR* qrexecClientVmPath = malloc(MAX_PATH_LONG_WSIZE);
    if (!qrexecClientVmPath)
        return FALSE;

    LogVerbose("start");

    // FIXME hardcoded path
    if (!PrepareExePath(qrexecClientVmPath, MAX_PATH_LONG_WSIZE, L"qrexec-client-vm.exe"))
        return FALSE;

    si.cb = sizeof(si);
    si.wShowWindow = SW_HIDE;
    si.dwFlags = STARTF_USESHOWWINDOW;

    WCHAR* cmdline = malloc(MAX_PATH_LONG_WSIZE);
    if (!cmdline)
        return FALSE;

    // FIXME hardcoded path
    DWORD status = StringCchPrintf(cmdline, MAX_PATH_LONG, L"qrexec-client-vm.exe dom0%cqubes.NotifyTools%c(null)%c(null)",
        QUBES_ARGUMENT_SEPARATOR, QUBES_ARGUMENT_SEPARATOR, QUBES_ARGUMENT_SEPARATOR);
    if (FAILED(status))
    {
        win_perror2(status, "formatting qrexec-client-vm command line");
        return FALSE;
    }

    LogDebug("Child command: %s", cmdline);
    if (!CreateProcess(
        qrexecClientVmPath,
        cmdline,
        NULL,
        NULL,
        FALSE,
        CREATE_NO_WINDOW,
        NULL,
        NULL,
        &si,
        &pi))
    {
        win_perror("CreateProcess(qrexec-client-vm.exe)");
        return FALSE;
    }

    /* fire and forget */
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    LogVerbose("success");

    return TRUE;
}

BOOL QdbWrite(qdb_handle_t qdb, char *path, char *value)
{
    return qdb_write(qdb, path, value, (int)strlen(value));
}

int wmain(int argc, WCHAR *argv[])
{
    qdb_handle_t qdb = NULL;
    ULONG status = ERROR_UNIDENTIFIED_ERROR;
    BOOL guiAgentPresent;
    BOOL qrexecAgentPresent = TRUE;
    CHAR *userName = NULL;

    if (argc < 2)
    {
        LogError("Usage: %s {0|1}", argv[0]);
        LogError("0 means tools are not installed");
        LogError("1 means tools are installed");
        return ERROR_BAD_ARGUMENTS;
    }

    qdb = qdb_open(NULL);
    if (!qdb)
    {
        win_perror("qdb_open");
        goto cleanup;
    }

    if (argv[1][0] == '0')
    {
        LogDebug("setting tools presence to not installed");
        qrexecAgentPresent = FALSE;
        guiAgentPresent = FALSE;
    }
    else
    {
        guiAgentPresent = CheckGuiAgentPresence();
    }

    // advertise tools presence
    LogDebug("waiting for user logon");

    while (!GetCurrentUser(&userName))
        Sleep(100);

    LogDebug("logged on user: %S", userName);

    /* for now mostly hardcoded values, but this can change in the future */
    if (!QdbWrite(qdb, QDB_PATH_PREFIX "version", "1"))
    {
        win_perror("write 'version' entry");
        goto cleanup;
    }

    if (!QdbWrite(qdb, QDB_PATH_PREFIX "os", "Windows"))
    {
        win_perror("write 'os' entry");
        goto cleanup;
    }

    if (!QdbWrite(qdb, QDB_PATH_PREFIX "qrexec", qrexecAgentPresent ? "1" : "0"))
    {
        win_perror("write 'qrexec' entry");
        goto cleanup;
    }

    if (!QdbWrite(qdb, QDB_PATH_PREFIX "gui", guiAgentPresent ? "1" : "0"))
    {
        win_perror("write 'gui' entry");
        goto cleanup;
    }

    if (!QdbWrite(qdb, QDB_PATH_PREFIX "gui-emulated", guiAgentPresent ? "0" : "1"))
    {
        win_perror("write 'gui-emulated' entry");
        goto cleanup;
    }

    if (!QdbWrite(qdb, QDB_PATH_PREFIX "default-user", userName))
    {
        win_perror("write 'default-user' entry");
        goto cleanup;
    }

    if (!NotifyDom0())
    {
        /* error already reported */
        goto cleanup;
    }

    status = ERROR_SUCCESS;
    LogVerbose("success");

cleanup:
    if (qdb)
        qdb_close(qdb);
    if (userName)
        WTSFreeMemory(userName);
    return status;
}
