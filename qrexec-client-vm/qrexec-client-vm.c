#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <strsafe.h>
#include "qrexec.h"
#include "log.h"
#include "exec.h"
#include "utf8-conv.h"

ULONG SendCreateProcessResponse(IN HANDLE pipe, IN CREATE_PROCESS_RESPONSE *response)
{
    DWORD cbWritten;
    DWORD cbRead;

    if (!response)
        return ERROR_INVALID_PARAMETER;

    if (!WriteFile(
        pipe,
        response,
        sizeof(*response),
        &cbWritten,
        NULL))
    {
        return perror("WriteFile");
    }

    if (CPR_TYPE_HANDLE == response->ResponseType)
    {
        LogDebug("Waiting for the server to read the handle, duplicate it and close the pipe\n");

        // Issue a blocking dummy read that will finish when the server disconnects the pipe
        // after the process handle duplication is complete. We have to wait here because for
        // the handle to be duplicated successfully this process must be present.
        ReadFile(pipe, &cbWritten, 1, &cbRead, NULL);
    }

    return ERROR_SUCCESS;
}

int __cdecl wmain(int argc, WCHAR *argv[])
{
    HANDLE agentPipe;
    BOOL success = FALSE;
    DWORD cbRead, cbWritten, pipeMode;
    LPTSTR pipeName = L"\\\\.\\pipe\\qrexec_trigger";
    struct trigger_service_params connectParams = { 0 };
    ULONG status;
    UCHAR *argumentUtf8;
    WCHAR *localProgram;
    HRESULT hresult;
    IO_HANDLES ioHandles;
    HANDLE process;
    CREATE_PROCESS_RESPONSE createProcessResponse;

    if (argc < 4)
    {
        wprintf(L"usage: %s target_vmname program_ident local_program [local program arguments]\n", argv[0]);
        return ERROR_INVALID_PARAMETER;
    }

    // Prepare the parameter structure containing the first two arguments.
    argumentUtf8 = NULL;
    status = ConvertUTF16ToUTF8(argv[2], &argumentUtf8, NULL); // local program
    if (ERROR_SUCCESS != status)
    {
        return perror2(status, "ConvertUTF16ToUTF8");
    }

    hresult = StringCchCopyA(connectParams.service_name, sizeof(connectParams.service_name), argumentUtf8);
    if (FAILED(hresult))
    {
        return perror2(hresult, "StringCchCopyA");
    }

    free(argumentUtf8);
    argumentUtf8 = NULL;

    status = ConvertUTF16ToUTF8(argv[1], &argumentUtf8, NULL); // vm name
    if (ERROR_SUCCESS != status)
    {
        return perror2(status, "ConvertUTF16ToUTF8");
    }

    hresult = StringCchCopyA(connectParams.target_domain, sizeof(connectParams.target_domain), argumentUtf8);
    if (FAILED(hresult))
    {
        return perror2(hresult, "StringCchCopyA");
    }

    free(argumentUtf8);
    argumentUtf8 = NULL;

    LogDebug("Connecting to qrexec-agent\n");

    // Try to open a named pipe; wait for it, if necessary.

    while (TRUE)
    {
        agentPipe = CreateFile(
            pipeName,
            GENERIC_READ | GENERIC_WRITE,
            0,
            NULL,
            OPEN_EXISTING,
            0,
            NULL);

        // Break if the pipe handle is valid.

        if (agentPipe != INVALID_HANDLE_VALUE)
            break;

        // Exit if an error other than ERROR_PIPE_BUSY occurs.
        status = GetLastError();
        if (ERROR_PIPE_BUSY != status)
        {
            return perror2(status, "CreateFile(agent pipe)");
        }

        // All pipe instances are busy, so wait for 10 seconds.

        if (!WaitNamedPipe(pipeName, 10000))
        {
            return perror("WaitNamedPipe");
        }
    }

    // The pipe connected; change to message-read mode.

    pipeMode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(
        agentPipe, // pipe handle
        &pipeMode, // new pipe mode
        NULL, // don't set maximum bytes
        NULL))
    { // don't set maximum time
        return perror("SetNamedPipeHandleState");
    }

    // Send the params to the pipe server.
    LogDebug("Sending the parameters to the server\n");

    if (!WriteFile(agentPipe, &connectParams, sizeof(connectParams), &cbWritten, NULL))
    {
        return perror("WriteFile");
    }

    LogDebug("Receiving the IO handles\n");

    // Read the handle array from the pipe.
    success = ReadFile(
        agentPipe,
        &ioHandles,
        sizeof(ioHandles),
        &cbRead,
        NULL);

    if (!success || cbRead != sizeof(ioHandles))
    {
        // If the message is too large to fit in a buffer, treat it as an error as well:
        // this shouldn't happen if the pipe server operates correctly.
        return perror("ReadFile");
    }

    LogInfo("Starting the local program '%s'\n", argv[3]);

    // find command line starting at third parameter _including_ quotes
    localProgram = wcsstr(GetCommandLine(), argv[2]);
    localProgram += wcslen(argv[2]);
    while (localProgram[0] == L' ')
        localProgram++;

    status = CreatePipedProcessAsCurrentUser(
        localProgram,	// local program
        ioHandles.StdinPipe,
        ioHandles.StdoutPipe,
        ioHandles.StderrPipe,
        &process);

    CloseHandle(ioHandles.StdinPipe);
    CloseHandle(ioHandles.StdoutPipe);
    CloseHandle(ioHandles.StderrPipe);

    if (ERROR_SUCCESS != status)
    {
        perror2(status, "CreatePipedProcessAsCurrentUser");

        LogDebug("Sending the error code to the server\n");

        createProcessResponse.ResponseType = CPR_TYPE_ERROR_CODE;
        createProcessResponse.ResponseData.ErrorCode = status;
    }
    else
    {
        LogDebug("Sending the process handle of the local program to the server\n");

        createProcessResponse.ResponseType = CPR_TYPE_HANDLE;
        createProcessResponse.ResponseData.Process = process;
    }

    SendCreateProcessResponse(agentPipe, &createProcessResponse);

    LogDebug("Closing the pipe\n");

    CloseHandle(agentPipe);

    if (ERROR_SUCCESS == status)
        CloseHandle(process);

    return ERROR_SUCCESS;
}
