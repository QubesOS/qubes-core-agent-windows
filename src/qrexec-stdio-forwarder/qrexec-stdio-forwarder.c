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

// This program is used to forward stdin from qrexec-client-vm to the stdin from qrexec-wrapper
// and forward the stdout from qrexec-wrapper to the stdout from qrexec-client-vm
// Both programs communicate via a pipe created by qrexec-client-vm

#include <windows.h>
#include <strsafe.h>

#include <qrexec.h>
#include <log.h>
#include <qubes-io.h>
#include <pipe-server.h>
#include <exec.h>

#define MAX_SIZE_NAME_FORWARDING_PIPE_SERVER 256
#define MAX_SIZE_BUFFER 65536

DWORD WINAPI ReadPipeWriteStdoutThread(HANDLE readPipe)
{
    HANDLE stdoutHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    BYTE buffer[MAX_SIZE_BUFFER];
    DWORD bytesRead;

    while (ReadFile(readPipe, &buffer, MAX_SIZE_BUFFER, &bytesRead, NULL))
        if (!bytesRead || !QioWriteBuffer(stdoutHandle, &buffer, bytesRead))
            break;

    // close read pipe, it reached eof
    CloseHandle(readPipe);
    // explicitly close stdout to propogate the eof
    CloseHandle(stdoutHandle);
}

void ReadStdinWritePipe(HANDLE writePipe)
{
    HANDLE stdinHandle = GetStdHandle(STD_INPUT_HANDLE);
    BYTE buffer[MAX_SIZE_BUFFER];
    DWORD bytesRead;

    while (ReadFile(stdinHandle, &buffer, MAX_SIZE_BUFFER, &bytesRead, NULL))
        if (!bytesRead || !QioWriteBuffer(writePipe, &buffer, bytesRead))
            break;

    // close stdin, received an eof
    CloseHandle(stdinHandle);
    // close write pipe to propogate the eof
    CloseHandle(writePipe);
}

int __cdecl wmain(int argc, WCHAR *argv[])
{
    PWSTR targetPidAndId;
    HANDLE readPipe, writePipe;

    targetPidAndId = GetArgument();

    if (!targetPidAndId)
    {
        LogError("Usage: %s target pid-id\n", argv[0]);
        return ERROR_BAD_ARGUMENTS;
    }

    LogDebug("target pid-id '%s'", targetPidAndId);
    WCHAR pipeName[MAX_SIZE_NAME_FORWARDING_PIPE_SERVER];
    StringCchPrintf(pipeName, MAX_SIZE_NAME_FORWARDING_PIPE_SERVER, L"\\\\.\\pipe\\stdio_forward_%s", targetPidAndId);

    LogDebug("Connecting to qrexec-client-vm");

    if (ERROR_SUCCESS != QpsConnect(pipeName, &readPipe, &writePipe))
        return win_perror("connect to qrexec-client-vm");

    // start processing data coming in from the read pipe (local) in its own thread
    HANDLE readPipeThread = CreateThread(NULL, 0, ReadPipeWriteStdoutThread, readPipe, 0, NULL);
    if (!readPipeThread)
    {
        CloseHandle(writePipe);
        CloseHandle(readPipe);
        return win_perror("creating read pipe thread");
    }

    // start processing data coming in from stdin (remote) in the main thread
    // block till eof is received on stdin (remote closed connection)
    ReadStdinWritePipe(writePipe);

    // block till eof is received on the read pipe (local closed connection)
    WaitForSingleObject(readPipeThread, INFINITE);
    CloseHandle(readPipeThread);

    LogVerbose("exiting");

    return ERROR_SUCCESS;
}
