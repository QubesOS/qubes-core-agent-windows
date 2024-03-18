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

// This program is used to trigger qrexec services in remote domains.
// It connects to local qrexec agent and sends it:
// trigger_service_params (service params), size_t (local handler path size), local handler path (WCHARs).
// Local handler will be launched as the local endpoint for the triggered service.

#include <windows.h>
#include <strsafe.h>

#include <qrexec.h>
#include <log.h>
#include <qubes-io.h>
#include <utf8-conv.h>
#include <pipe-server.h>
#include <exec.h>

int __cdecl wmain(int argc, WCHAR *argv[])
{
    HANDLE readPipe, writePipe;
    BOOL success = FALSE;
    PWSTR pipeName = L"\\\\.\\pipe\\qrexec_trigger";
    struct trigger_service_params triggerParams = { 0 };
    ULONG status;
    UCHAR *argumentUtf8;
    HRESULT hresult;
    size_t size;
    PWSTR domainName, serviceName, commandLine;

    domainName = GetArgument();
    serviceName = GetArgument();
    commandLine = GetArgument();

    if (!domainName || !serviceName || !commandLine)
    {
        LogError("Usage: %s domain name|qrexec service name|local program [local program arguments]\n", argv[0]);
        return ERROR_INVALID_PARAMETER;
    }

    LogDebug("domain '%s', service '%s', local command '%s'", domainName, serviceName, commandLine);

    // Prepare the parameter structure containing the first two arguments.
    argumentUtf8 = NULL;
    status = ConvertUTF16ToUTF8Static(serviceName, &argumentUtf8, NULL);
    if (ERROR_SUCCESS != status)
        return win_perror2(status, "ConvertUTF16ToUTF8Static(serviceName)");

    hresult = StringCchCopyA(triggerParams.service_name, sizeof(triggerParams.service_name), argumentUtf8);
    if (FAILED(hresult))
        return win_perror2(hresult, "StringCchCopyA");

    argumentUtf8 = NULL;

    status = ConvertUTF16ToUTF8Static(domainName, &argumentUtf8, NULL);
    if (ERROR_SUCCESS != status)
        return win_perror2(status, "ConvertUTF16ToUTF8Static(domainName)");

    hresult = StringCchCopyA(triggerParams.target_domain, sizeof(triggerParams.target_domain), argumentUtf8);
    if (FAILED(hresult))
        return win_perror2(hresult, "StringCchCopyA");

    argumentUtf8 = NULL;

    LogDebug("Connecting to qrexec-agent");

    if (ERROR_SUCCESS != QpsConnect(pipeName, &readPipe, &writePipe))
        return (int)GetLastError(); // win_perror("open agent pipe");

    CloseHandle(readPipe);
    LogDebug("Sending the parameters to qrexec-agent");

    if (!QioWriteBuffer(writePipe, &triggerParams, sizeof(triggerParams)))
        return win_perror("write trigger params to agent");

    size = (wcslen(commandLine) + 1) * sizeof(WCHAR);
    if (!QioWriteBuffer(writePipe, &size, sizeof(size)))
        return win_perror("write command size to agent");

    if (!QioWriteBuffer(writePipe, commandLine, (DWORD)size))
        return win_perror("write command to agent");

    CloseHandle(writePipe);

    LogVerbose("exiting");

    return ERROR_SUCCESS;
}
