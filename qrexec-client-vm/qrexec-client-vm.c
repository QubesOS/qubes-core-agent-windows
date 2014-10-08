#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <tchar.h>
#include <strsafe.h>
#include "qrexec.h"
#include "log.h"
#include "utf8-conv.h"

ULONG CreatePipedProcessAsCurrentUser(
    TCHAR *pszCommand,
    BOOLEAN bRunInteractively,
    HANDLE hPipeStdin,
    HANDLE hPipeStdout,
    HANDLE hPipeStderr,
    HANDLE *phProcess)
{
    PROCESS_INFORMATION	pi;
    STARTUPINFO	si;
    BOOLEAN	bInheritHandles;

    if (!pszCommand || !phProcess)
        return ERROR_INVALID_PARAMETER;

    *phProcess = INVALID_HANDLE_VALUE;

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);

    bInheritHandles = FALSE;

    if (INVALID_HANDLE_VALUE != hPipeStdin &&
        INVALID_HANDLE_VALUE != hPipeStdout &&
        INVALID_HANDLE_VALUE != hPipeStderr)
    {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = hPipeStdin;
        si.hStdOutput = hPipeStdout;
        si.hStdError = hPipeStderr;

        bInheritHandles = TRUE;
    }

    if (!CreateProcess(
        NULL,
        pszCommand,
        NULL,
        NULL,
        bInheritHandles, // inherit handles if IO is piped
        0,
        NULL,
        NULL,
        &si,
        &pi))
    {
        return perror("CreateProcess");
    }

    LogDebug("pid %d\n", pi.dwProcessId);

    *phProcess = pi.hProcess;
    CloseHandle(pi.hThread);

    return ERROR_SUCCESS;
}

ULONG SendCreateProcessResponse(HANDLE hPipe, CREATE_PROCESS_RESPONSE *pCpr)
{
    DWORD cbWritten;
    DWORD cbRead;

    if (!pCpr)
        return ERROR_INVALID_PARAMETER;

    if (!WriteFile(
        hPipe,
        pCpr,
        sizeof(CREATE_PROCESS_RESPONSE),
        &cbWritten,
        NULL))
    {
        return perror("WriteFile");
    }

    if (CPR_TYPE_HANDLE == pCpr->bType)
    {
        LogDebug("Waiting for the server to read the handle, duplicate it and close the pipe\n");

        // Issue a blocking dummy read that will finish when the server disconnects the pipe
        // after the process handle duplication is complete. We have to wait here because for
        // the handle to be duplicated successfully this process must be present.
        ReadFile(hPipe, &cbWritten, 1, &cbRead, NULL);
    }

    return ERROR_SUCCESS;
}

int __cdecl _tmain(ULONG argc, TCHAR *argv[])
{
    HANDLE hPipe;
    BOOL fSuccess = FALSE;
    DWORD cbRead, cbWritten, dwMode;
    LPTSTR lpszPipename = TEXT("\\\\.\\pipe\\qrexec_trigger");
    struct trigger_connect_params params;
    ULONG uResult;
    UCHAR *pszParameter;
    TCHAR *pszLocalProgram;
    HRESULT hResult;
    IO_HANDLES_ARRAY IoHandles;
    HANDLE hProcess;
    CREATE_PROCESS_RESPONSE CreateProcessResponse;

    if (argc < 4)
    {
        _tprintf(TEXT("usage: %s target_vmname program_ident local_program [local program arguments]\n"), argv[0]);
        return 1;
    }

    // Prepare the parameter structure containing the first two arguments.
    memset(&params, 0, sizeof(params));

#ifdef UNICODE
    pszParameter = NULL;
    uResult = ConvertUTF16ToUTF8(argv[2], &pszParameter, NULL);
    if (ERROR_SUCCESS != uResult)
    {
        return perror2(uResult, "ConvertUTF16ToUTF8");
    }
#else
    pszParameter = argv[2];
#endif

    hResult = StringCchCopyA(params.exec_index, sizeof(params.exec_index), pszParameter);
    if (FAILED(hResult))
    {
        return perror2(hResult, "StringCchCopyA");
    }

#ifdef UNICODE
    free(pszParameter);
    pszParameter = NULL;

    uResult = ConvertUTF16ToUTF8(argv[1], &pszParameter, NULL);
    if (ERROR_SUCCESS != uResult)
    {
        return perror2(uResult, "ConvertUTF16ToUTF8");
    }
#else
    pszParameter = argv[1];
#endif

    hResult = StringCchCopyA(params.target_vmname, sizeof(params.target_vmname), pszParameter);
    if (FAILED(hResult))
    {
        return perror2(hResult, "StringCchCopyA");
    }

#ifdef UNICODE
    free(pszParameter);
#endif
    pszParameter = NULL;

    LogDebug("Connecting to the pipe server\n");

    // Try to open a named pipe; wait for it, if necessary.

    while (TRUE)
    {
        hPipe = CreateFile(
            lpszPipename,
            GENERIC_READ | GENERIC_WRITE,
            0,
            NULL,
            OPEN_EXISTING,
            0,
            NULL);

        // Break if the pipe handle is valid.

        if (hPipe != INVALID_HANDLE_VALUE)
            break;

        // Exit if an error other than ERROR_PIPE_BUSY occurs.
        uResult = GetLastError();
        if (ERROR_PIPE_BUSY != uResult)
        {
            return perror2(uResult, "CreateFile(agent pipe)");
        }

        // All pipe instances are busy, so wait for 10 seconds.

        if (!WaitNamedPipe(lpszPipename, 10000))
        {
            return perror("WaitNamedPipe");
        }
    }

    // The pipe connected; change to message-read mode.

    dwMode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(
        hPipe, // pipe handle
        &dwMode, // new pipe mode
        NULL, // don't set maximum bytes
        NULL))
    { // don't set maximum time
        return perror("SetNamedPipeHandleState");
    }

    // Send the params to the pipe server.
    LogDebug("Sending the parameters to the server\n");

    if (!WriteFile(hPipe, &params, sizeof(params), &cbWritten, NULL))
    {
        return perror("WriteFile");
    }

    LogDebug("Receiving the IO handles\n");

    // Read the handle array from the pipe.
    fSuccess = ReadFile(
        hPipe,
        &IoHandles,
        sizeof(IoHandles),
        &cbRead,
        NULL);

    if (!fSuccess || cbRead != sizeof(IoHandles))
    {
        // If the message is too large to fit in a buffer, treat it as an error as well:
        // this shouldn't happen if the pipe server operates correctly.
        return perror("ReadFile");
    }

    LogInfo("Starting the local program '%s'\n", argv[3]);

    // find command line starting at third parameter _including_ quotes
    pszLocalProgram = _tcsstr(GetCommandLine(), argv[2]);
    pszLocalProgram += _tcslen(argv[2]);
    while (pszLocalProgram[0] == TEXT(' '))
        pszLocalProgram++;

    uResult = CreatePipedProcessAsCurrentUser(
        pszLocalProgram,	// local program
        TRUE,
        IoHandles.hPipeStdin,
        IoHandles.hPipeStdout,
        IoHandles.hPipeStderr,
        &hProcess);

    CloseHandle(IoHandles.hPipeStdin);
    CloseHandle(IoHandles.hPipeStdout);
    CloseHandle(IoHandles.hPipeStderr);

    if (ERROR_SUCCESS != uResult)
    {
        perror2(uResult, "CreatePipedProcessAsCurrentUser");

        LogDebug("Sending the error code to the server\n");

        CreateProcessResponse.bType = CPR_TYPE_ERROR_CODE;
        CreateProcessResponse.ResponseData.dwErrorCode = uResult;
    }
    else
    {
        LogDebug("Sending the process handle of the local program to the server\n");

        CreateProcessResponse.bType = CPR_TYPE_HANDLE;
        CreateProcessResponse.ResponseData.hProcess = hProcess;
    }

    SendCreateProcessResponse(hPipe, &CreateProcessResponse);

    LogDebug("Closing the pipe\n");

    CloseHandle(hPipe);

    if (ERROR_SUCCESS == uResult)
        CloseHandle(hProcess);

    return 0;
}
