#include "qrexec-agent.h"
#include <Shlwapi.h>
#include "utf8-conv.h"

HANDLE g_hAddExistingClientEvent;

CLIENT_INFO g_Clients[MAX_CLIENTS];
HANDLE g_WatchedEvents[MAXIMUM_WAIT_OBJECTS];
HANDLE_INFO	g_HandlesInfo[MAXIMUM_WAIT_OBJECTS];

ULONG64	g_uPipeId = 0;

CRITICAL_SECTION g_ClientsCriticalSection;
CRITICAL_SECTION g_VchanCriticalSection;

extern HANDLE g_hStopServiceEvent;
#ifndef BUILD_AS_SERVICE
HANDLE g_hCleanupFinishedEvent;
#endif

// from advertise_tools.c
LONG advertise_tools();

ULONG CreateAsyncPipe(HANDLE *phReadPipe, HANDLE *phWritePipe, SECURITY_ATTRIBUTES *pSecurityAttributes)
{
    TCHAR szPipeName[MAX_PATH + 1];
    HANDLE hReadPipe;
    HANDLE hWritePipe;
    ULONG uResult;

    if (!phReadPipe || !phWritePipe)
        return ERROR_INVALID_PARAMETER;

    StringCchPrintf(szPipeName, MAX_PATH, TEXT("\\\\.\\pipe\\qrexec.%08x.%I64x"), GetCurrentProcessId(), g_uPipeId++);

    hReadPipe = CreateNamedPipe(
        szPipeName,
        PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE,
        1,
        4096,
        4096,
        50,	// the default timeout is 50ms
        pSecurityAttributes);

    if (!hReadPipe)
    {
        return perror("CreateNamedPipe");
    }

    hWritePipe = CreateFile(
        szPipeName,
        GENERIC_WRITE,
        0,
        pSecurityAttributes,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        NULL);

    if (INVALID_HANDLE_VALUE == hWritePipe)
    {
        uResult = perror("CreateFile");
        CloseHandle(hReadPipe);
        return uResult;
    }

    *phReadPipe = hReadPipe;
    *phWritePipe = hWritePipe;

    return ERROR_SUCCESS;
}

ULONG InitReadPipe(PIPE_DATA *pPipeData, HANDLE *phWritePipe, UCHAR bPipeType)
{
    SECURITY_ATTRIBUTES sa;
    ULONG uResult;

    memset(pPipeData, 0, sizeof(PIPE_DATA));
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    if (!pPipeData || !phWritePipe)
        return ERROR_INVALID_PARAMETER;

    pPipeData->olRead.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!pPipeData->olRead.hEvent)
        return GetLastError();

    uResult = CreateAsyncPipe(&pPipeData->hReadPipe, phWritePipe, &sa);
    if (ERROR_SUCCESS != uResult)
    {
        CloseHandle(pPipeData->olRead.hEvent);
        return perror2(uResult, "CreateAsyncPipe");
    }

    // Ensure the read handle to the pipe is not inherited.
    SetHandleInformation(pPipeData->hReadPipe, HANDLE_FLAG_INHERIT, 0);

    pPipeData->bPipeType = bPipeType;

    return ERROR_SUCCESS;
}

ULONG ReturnData(int client_id, int type, void *pData, ULONG uDataSize, ULONG *puDataWritten)
{
    struct server_header s_hdr;
    unsigned int vchan_space_avail;
    ULONG uResult = ERROR_SUCCESS;

    EnterCriticalSection(&g_VchanCriticalSection);

    if (puDataWritten)
    {
        // allow partial write only when puDataWritten given
        *puDataWritten = 0;
        vchan_space_avail = buffer_space_vchan_ext();
        if (vchan_space_avail < sizeof(s_hdr))
        {
            LeaveCriticalSection(&g_VchanCriticalSection);
            return ERROR_INSUFFICIENT_BUFFER;
        }
        // inhibit zero-length write when not requested
        if (uDataSize && vchan_space_avail == sizeof(s_hdr))
        {
            LeaveCriticalSection(&g_VchanCriticalSection);
            return ERROR_INSUFFICIENT_BUFFER;
        }

        if (vchan_space_avail < sizeof(s_hdr) + uDataSize)
        {
            uResult = ERROR_INSUFFICIENT_BUFFER;
            uDataSize = vchan_space_avail - sizeof(s_hdr);
        }

        *puDataWritten = uDataSize;
    }

    s_hdr.type = type;
    s_hdr.client_id = client_id;
    s_hdr.len = uDataSize;
    if (write_all_vchan_ext(&s_hdr, sizeof s_hdr) <= 0)
    {
        LogError("write_all_vchan_ext(s_hdr)");
        LeaveCriticalSection(&g_VchanCriticalSection);
        return ERROR_INVALID_FUNCTION;
    }

    if (!uDataSize)
    {
        LeaveCriticalSection(&g_VchanCriticalSection);
        return ERROR_SUCCESS;
    }

    if (write_all_vchan_ext(pData, uDataSize) <= 0)
    {
        LogError("write_all_vchan_ext(data, %d)", uDataSize);
        LeaveCriticalSection(&g_VchanCriticalSection);
        return ERROR_INVALID_FUNCTION;
    }

    LeaveCriticalSection(&g_VchanCriticalSection);
    return uResult;
}

ULONG send_exit_code(int client_id, int status)
{
    ULONG uResult;

    uResult = ReturnData(client_id, MSG_AGENT_TO_SERVER_EXIT_CODE, &status, sizeof(status), NULL);
    if (ERROR_SUCCESS != uResult)
    {
        return perror2(uResult, "ReturnData");
    }
    else
        LogDebug("Send exit code %d for client_id %d\n", status, client_id);

    return ERROR_SUCCESS;
}

CLIENT_INFO *FindClientById(int client_id)
{
    ULONG uClientNumber;

    for (uClientNumber = 0; uClientNumber < MAX_CLIENTS; uClientNumber++)
        if (client_id == g_Clients[uClientNumber].client_id)
            return &g_Clients[uClientNumber];

    return NULL;
}

ULONG ReturnPipeData(int client_id, PIPE_DATA *pPipeData)
{
    DWORD dwRead;
    int	message_type;
    CLIENT_INFO *pClientInfo;
    ULONG uResult;
    ULONG uDataSent;

    uResult = ERROR_SUCCESS;

    if (!pPipeData)
        return ERROR_INVALID_PARAMETER;

    pClientInfo = FindClientById(client_id);
    if (!pClientInfo)
        return ERROR_NOT_FOUND;

    if (pClientInfo->bReadingIsDisabled)
        // The client does not want to receive any data from this console.
        return ERROR_INVALID_FUNCTION;

    pPipeData->bReadInProgress = FALSE;
    pPipeData->bDataIsReady = FALSE;

    switch (pPipeData->bPipeType)
    {
    case PTYPE_STDOUT:
        message_type = MSG_AGENT_TO_SERVER_STDOUT;
        break;
    case PTYPE_STDERR:
        message_type = MSG_AGENT_TO_SERVER_STDERR;
        break;
    default:
        return ERROR_INVALID_FUNCTION;
    }

    dwRead = 0;
    if (!GetOverlappedResult(pPipeData->hReadPipe, &pPipeData->olRead, &dwRead, FALSE))
    {
        perror("GetOverlappedResult");
        LogError("client %d, dwRead %d", client_id, dwRead);
    }

    uResult = ReturnData(client_id, message_type, pPipeData->ReadBuffer + pPipeData->dwSentBytes, dwRead - pPipeData->dwSentBytes, &uDataSent);
    if (ERROR_INSUFFICIENT_BUFFER == uResult)
    {
        pPipeData->dwSentBytes += uDataSent;
        pPipeData->bVchanWritePending = TRUE;
        return uResult;
    }
    else if (ERROR_SUCCESS != uResult)
        perror2(uResult, "ReturnData");

    pPipeData->bVchanWritePending = FALSE;

    if (!dwRead)
    {
        pPipeData->bPipeClosed = TRUE;
        uResult = ERROR_HANDLE_EOF;
    }

    return uResult;
}

ULONG CloseReadPipeHandles(int client_id, PIPE_DATA *pPipeData)
{
    ULONG uResult;

    if (!pPipeData)
        return ERROR_INVALID_PARAMETER;

    uResult = ERROR_SUCCESS;

    if (pPipeData->olRead.hEvent)
    {
        if (pPipeData->bDataIsReady)
            ReturnPipeData(client_id, pPipeData);

        // ReturnPipeData() clears both bDataIsReady and bReadInProgress, but they cannot be ever set to a non-FALSE value at the same time.
        // So, if the above ReturnPipeData() has been executed (bDataIsReady was not FALSE), then bReadInProgress was FALSE
        // and this branch wouldn't be executed anyways.
        if (pPipeData->bReadInProgress)
        {
            // If bReadInProgress is not FALSE then hReadPipe must be a valid handle for which an
            // asynchornous read has been issued.
            if (CancelIo(pPipeData->hReadPipe))
            {
                // Must wait for the canceled IO to complete, otherwise a race condition may occur on the
                // OVERLAPPED structure.
                WaitForSingleObject(pPipeData->olRead.hEvent, INFINITE);

                // See if there is something to return.
                ReturnPipeData(client_id, pPipeData);
            }
            else
            {
                perror("CancelIo");
            }
        }

        CloseHandle(pPipeData->olRead.hEvent);
    }

    if (pPipeData->hReadPipe)
        // Can close the pipe only when there is no pending IO in progress.
        CloseHandle(pPipeData->hReadPipe);

    return uResult;
}

ULONG TextBOMToUTF16(unsigned char *pszBuf, size_t cbBufLen, WCHAR **ppwszUtf16)
{
    size_t cbSkipChars = 0;
    WCHAR *pwszUtf16 = NULL;
    ULONG uResult;
    HRESULT hResult;

    if (!pszBuf || !cbBufLen || !ppwszUtf16)
        return ERROR_INVALID_PARAMETER;

    *ppwszUtf16 = NULL;

    // see http://en.wikipedia.org/wiki/Byte-order_mark for explaination of the BOM
    // encoding
    if (cbBufLen >= 3 && pszBuf[0] == 0xEF && pszBuf[1] == 0xBB && pszBuf[2] == 0xBF)
    {
        // UTF-8
        cbSkipChars = 3;
    }
    else if (cbBufLen >= 2 && pszBuf[0] == 0xFE && pszBuf[1] == 0xFF)
    {
        // UTF-16BE
        return ERROR_NOT_SUPPORTED;
    }
    else if (cbBufLen >= 2 && pszBuf[0] == 0xFF && pszBuf[1] == 0xFE)
    {
        // UTF-16LE
        cbSkipChars = 2;

        pwszUtf16 = malloc(cbBufLen - cbSkipChars + sizeof(WCHAR));
        if (!pwszUtf16)
            return ERROR_NOT_ENOUGH_MEMORY;

        hResult = StringCbCopyW(pwszUtf16, cbBufLen - cbSkipChars + sizeof(WCHAR), (STRSAFE_LPCWSTR) (pszBuf + cbSkipChars));
        if (FAILED(hResult))
        {
            perror2(hResult, "StringCbCopyW");
            free(pwszUtf16);
            return hResult;
        }

        *ppwszUtf16 = pwszUtf16;
        return ERROR_SUCCESS;
    }
    else if (cbBufLen >= 4 && pszBuf[0] == 0 && pszBuf[1] == 0 && pszBuf[2] == 0xFE && pszBuf[3] == 0xFF)
    {
        // UTF-32BE
        return ERROR_NOT_SUPPORTED;
    }
    else if (cbBufLen >= 4 && pszBuf[0] == 0xFF && pszBuf[1] == 0xFE && pszBuf[2] == 0 && pszBuf[3] == 0)
    {
        // UTF-32LE
        return ERROR_NOT_SUPPORTED;
    }

    // Try UTF-8

    uResult = ConvertUTF8ToUTF16(pszBuf + cbSkipChars, ppwszUtf16, NULL);
    if (ERROR_SUCCESS != uResult)
    {
        return perror2(uResult, "ConvertUTF8ToUTF16");
    }

    return ERROR_SUCCESS;
}

ULONG ParseUtf8Command(UCHAR *pszUtf8Command, WCHAR **ppwszCommand, WCHAR **ppwszUserName, WCHAR **ppwszCommandLine, BOOLEAN *pbRunInteractively)
{
    ULONG uResult;
    WCHAR *pwszCommand = NULL;
    WCHAR *pwszCommandLine = NULL;
    WCHAR *pwSeparator = NULL;
    WCHAR *pwszUserName = NULL;

    if (!pszUtf8Command || !pbRunInteractively)
        return ERROR_INVALID_PARAMETER;

    *ppwszCommand = NULL;
    *ppwszUserName = NULL;
    *ppwszCommandLine = NULL;
    *pbRunInteractively = TRUE;

    pwszCommand = NULL;
    uResult = ConvertUTF8ToUTF16(pszUtf8Command, &pwszCommand, NULL);
    if (ERROR_SUCCESS != uResult)
    {
        return perror2(uResult, "ConvertUTF8ToUTF16");
    }

    LogInfo("Command: %s", pwszCommand);

    pwszUserName = pwszCommand;
    pwSeparator = wcschr(pwszCommand, L':');
    if (!pwSeparator)
    {
        free(pwszCommand);
        LogWarning("Command line is supposed to be in user:[nogui:]command form\n");
        return ERROR_INVALID_PARAMETER;
    }

    *pwSeparator = L'\0';
    pwSeparator++;

    if (!wcsncmp(pwSeparator, L"nogui:", 6))
    {
        pwSeparator = wcschr(pwSeparator, L':');
        if (!pwSeparator)
        {
            free(pwszCommand);
            LogWarning("Command line is supposed to be in user:[nogui:]command form\n");
            return ERROR_INVALID_PARAMETER;
        }

        *pwSeparator = L'\0';
        pwSeparator++;

        *pbRunInteractively = FALSE;
    }

    if (!wcscmp(pwszUserName, L"SYSTEM") || !wcscmp(pwszUserName, L"root"))
    {
        pwszUserName = NULL;
    }

    *ppwszCommand = pwszCommand;
    *ppwszUserName = pwszUserName;
    *ppwszCommandLine = pwSeparator;

    return ERROR_SUCCESS;
}

ULONG CreateClientPipes(CLIENT_INFO *pClientInfo, HANDLE *phPipeStdin, HANDLE *phPipeStdout, HANDLE *phPipeStderr)
{
    ULONG uResult;
    SECURITY_ATTRIBUTES sa;
    HANDLE hPipeStdin = INVALID_HANDLE_VALUE;
    HANDLE hPipeStdout = INVALID_HANDLE_VALUE;
    HANDLE hPipeStderr = INVALID_HANDLE_VALUE;

    if (!pClientInfo || !phPipeStdin || !phPipeStdout || !phPipeStderr)
        return ERROR_INVALID_PARAMETER;

    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    uResult = InitReadPipe(&pClientInfo->Stdout, &hPipeStdout, PTYPE_STDOUT);
    if (ERROR_SUCCESS != uResult)
    {
        return perror2(uResult, "InitReadPipe(STDOUT)");
    }
    uResult = InitReadPipe(&pClientInfo->Stderr, &hPipeStderr, PTYPE_STDERR);
    if (ERROR_SUCCESS != uResult)
    {
        CloseHandle(pClientInfo->Stdout.hReadPipe);
        CloseHandle(hPipeStdout);

        return perror2(uResult, "InitReadPipe(STDERR)");
    }

    if (!CreatePipe(&hPipeStdin, &pClientInfo->hWriteStdinPipe, &sa, 0))
    {
        uResult = GetLastError();

        CloseHandle(pClientInfo->Stdout.hReadPipe);
        CloseHandle(pClientInfo->Stderr.hReadPipe);
        CloseHandle(hPipeStdout);
        CloseHandle(hPipeStderr);

        return perror2(uResult, "CreatePipe(STDIN)");
    }

    pClientInfo->bStdinPipeClosed = FALSE;

    // Ensure the write handle to the pipe for STDIN is not inherited.
    SetHandleInformation(pClientInfo->hWriteStdinPipe, HANDLE_FLAG_INHERIT, 0);

    *phPipeStdin = hPipeStdin;
    *phPipeStdout = hPipeStdout;
    *phPipeStderr = hPipeStderr;

    return ERROR_SUCCESS;
}

// This routine may be called by pipe server threads, hence the critical section around g_Clients array is required.
ULONG ReserveClientNumber(int client_id, ULONG *puClientNumber)
{
    ULONG uClientNumber;

    EnterCriticalSection(&g_ClientsCriticalSection);

    for (uClientNumber = 0; uClientNumber < MAX_CLIENTS; uClientNumber++)
        if (FREE_CLIENT_SPOT_ID == g_Clients[uClientNumber].client_id)
            break;

    if (MAX_CLIENTS == uClientNumber)
    {
        // There is no space for watching for another process
        LeaveCriticalSection(&g_ClientsCriticalSection);
        LogWarning("The maximum number of running processes (%d) has been reached\n", MAX_CLIENTS);
        return ERROR_TOO_MANY_CMDS;
    }

    if (FindClientById(client_id))
    {
        LeaveCriticalSection(&g_ClientsCriticalSection);
        LogWarning("A client with the same id (%d) already exists\n", client_id);
        return ERROR_ALREADY_EXISTS;
    }

    g_Clients[uClientNumber].bClientIsReady = FALSE;
    g_Clients[uClientNumber].client_id = client_id;
    *puClientNumber = uClientNumber;

    LeaveCriticalSection(&g_ClientsCriticalSection);

    return ERROR_SUCCESS;
}

ULONG ReleaseClientNumber(ULONG uClientNumber)
{
    if (uClientNumber >= MAX_CLIENTS)
        return ERROR_INVALID_PARAMETER;

    EnterCriticalSection(&g_ClientsCriticalSection);

    g_Clients[uClientNumber].bClientIsReady = FALSE;
    g_Clients[uClientNumber].client_id = FREE_CLIENT_SPOT_ID;

    LeaveCriticalSection(&g_ClientsCriticalSection);

    return ERROR_SUCCESS;
}

ULONG AddFilledClientInfo(ULONG uClientNumber, CLIENT_INFO *pClientInfo)
{
    if (!pClientInfo || uClientNumber >= MAX_CLIENTS)
        return ERROR_INVALID_PARAMETER;

    EnterCriticalSection(&g_ClientsCriticalSection);

    g_Clients[uClientNumber] = *pClientInfo;
    g_Clients[uClientNumber].bClientIsReady = TRUE;

    LeaveCriticalSection(&g_ClientsCriticalSection);

    return ERROR_SUCCESS;
}

ULONG AddClient(int client_id, WCHAR *pwszUserName, WCHAR *pwszCommandLine, BOOLEAN bRunInteractively)
{
    ULONG uResult;
    CLIENT_INFO ClientInfo;
    HANDLE hPipeStdout = INVALID_HANDLE_VALUE;
    HANDLE hPipeStderr = INVALID_HANDLE_VALUE;
    HANDLE hPipeStdin = INVALID_HANDLE_VALUE;
    ULONG uClientNumber;

    // if pwszUserName is NULL we run the process on behalf of the current user.
    if (!pwszCommandLine)
        return ERROR_INVALID_PARAMETER;

    uResult = ReserveClientNumber(client_id, &uClientNumber);
    if (ERROR_SUCCESS != uResult)
    {
        return perror2(uResult, "ReserveClientNumber");
    }

    if (pwszUserName)
        LogInfo("Running '%s' as user '%s'\n", pwszCommandLine, pwszUserName);
    else
    {
#ifdef BUILD_AS_SERVICE
        LogInfo("Running '%s' as SYSTEM\n", pwszCommandLine);
#else
        LogInfo("Running '%s' as current user\n", pwszCommandLine);
#endif
    }

    memset(&ClientInfo, 0, sizeof(ClientInfo));
    ClientInfo.client_id = client_id;

    uResult = CreateClientPipes(&ClientInfo, &hPipeStdin, &hPipeStdout, &hPipeStderr);
    if (ERROR_SUCCESS != uResult)
    {
        ReleaseClientNumber(uClientNumber);
        return perror2(uResult, "CreateClientPipes");
    }

#ifdef BUILD_AS_SERVICE
    if (pwszUserName)
    {
        uResult = CreatePipedProcessAsUser(
            pwszUserName,
            DEFAULT_USER_PASSWORD_UNICODE,
            pwszCommandLine,
            bRunInteractively,
            hPipeStdin,
            hPipeStdout,
            hPipeStderr,
            &ClientInfo.hProcess);
    }
    else
    {
        uResult = CreatePipedProcessAsCurrentUser(
            pwszCommandLine,
            hPipeStdin,
            hPipeStdout,
            hPipeStderr,
            &ClientInfo.hProcess);
    }
#else
    uResult = CreatePipedProcessAsCurrentUser(
        pwszCommandLine,
        bRunInteractively,
        hPipeStdin,
        hPipeStdout,
        hPipeStderr,
        &ClientInfo.hProcess);
#endif

    CloseHandle(hPipeStdout);
    CloseHandle(hPipeStderr);
    CloseHandle(hPipeStdin);

    if (ERROR_SUCCESS != uResult)
    {
        ReleaseClientNumber(uClientNumber);

        CloseHandle(ClientInfo.hWriteStdinPipe);
        CloseHandle(ClientInfo.Stdout.hReadPipe);
        CloseHandle(ClientInfo.Stderr.hReadPipe);

#ifdef BUILD_AS_SERVICE
        if (pwszUserName)
            return perror2(uResult, "CreatePipedProcessAsUser");
        else
            return perror2(uResult, "ACreatePipedProcessAsCurrentUser");
#else
        return perror2(uResult, "CreatePipedProcessAsCurrentUser");
#endif
    }

    uResult = AddFilledClientInfo(uClientNumber, &ClientInfo);
    if (ERROR_SUCCESS != uResult)
    {
        ReleaseClientNumber(uClientNumber);

        CloseHandle(ClientInfo.hWriteStdinPipe);
        CloseHandle(ClientInfo.Stdout.hReadPipe);
        CloseHandle(ClientInfo.Stderr.hReadPipe);
        CloseHandle(ClientInfo.hProcess);

        return perror2(uResult, "AddFilledClientInfo");
    }

    LogDebug("New client %d (local id %d)\n", client_id, uClientNumber);

    return ERROR_SUCCESS;
}

ULONG AddExistingClient(int client_id, CLIENT_INFO *pClientInfo)
{
    ULONG uClientNumber;
    ULONG uResult;

    if (!pClientInfo)
        return ERROR_INVALID_PARAMETER;

    uResult = ReserveClientNumber(client_id, &uClientNumber);
    if (ERROR_SUCCESS != uResult)
    {
        return perror2(uResult, "ReserveClientNumber");
    }

    pClientInfo->client_id = client_id;

    uResult = AddFilledClientInfo(uClientNumber, pClientInfo);
    if (ERROR_SUCCESS != uResult)
    {
        ReleaseClientNumber(uClientNumber);
        return perror2(uResult, "AddFilledClientInfo");
    }

    LogDebug("New client %d (local id %d)\n", client_id, uClientNumber);

    SetEvent(g_hAddExistingClientEvent);

    return ERROR_SUCCESS;
}

VOID RemoveClientNoLocks(CLIENT_INFO *pClientInfo)
{
    if (!pClientInfo || (FREE_CLIENT_SPOT_ID == pClientInfo->client_id))
        return;

    CloseHandle(pClientInfo->hProcess);

    if (!pClientInfo->bStdinPipeClosed)
        CloseHandle(pClientInfo->hWriteStdinPipe);

    CloseReadPipeHandles(pClientInfo->client_id, &pClientInfo->Stdout);
    CloseReadPipeHandles(pClientInfo->client_id, &pClientInfo->Stderr);

    LogDebug("Client %d removed\n", pClientInfo->client_id);

    pClientInfo->client_id = FREE_CLIENT_SPOT_ID;
    pClientInfo->bClientIsReady = FALSE;
}

VOID RemoveClient(CLIENT_INFO *pClientInfo)
{
    EnterCriticalSection(&g_ClientsCriticalSection);

    RemoveClientNoLocks(pClientInfo);

    LeaveCriticalSection(&g_ClientsCriticalSection);
}

VOID RemoveAllClients()
{
    ULONG uClientNumber;

    EnterCriticalSection(&g_ClientsCriticalSection);

    for (uClientNumber = 0; uClientNumber < MAX_CLIENTS; uClientNumber++)
        if (FREE_CLIENT_SPOT_ID != g_Clients[uClientNumber].client_id)
            RemoveClientNoLocks(&g_Clients[uClientNumber]);

    LeaveCriticalSection(&g_ClientsCriticalSection);
}

// must be called with g_ClientsCriticalSection
ULONG PossiblyHandleTerminatedClientNoLocks(CLIENT_INFO *pClientInfo)
{
    ULONG uResult;

    if (pClientInfo->bChildExited && pClientInfo->Stdout.bPipeClosed && pClientInfo->Stderr.bPipeClosed)
    {
        uResult = send_exit_code(pClientInfo->client_id, pClientInfo->dwExitCode);
        // guaranted that all data was already sent (above bPipeClosed==TRUE)
        // so no worry about returning some data after exit code
        RemoveClientNoLocks(pClientInfo);
        return uResult;
    }
    return ERROR_SUCCESS;
}

ULONG PossiblyHandleTerminatedClient(CLIENT_INFO *pClientInfo)
{
    ULONG uResult;

    if (pClientInfo->bChildExited && pClientInfo->Stdout.bPipeClosed && pClientInfo->Stderr.bPipeClosed)
    {
        uResult = send_exit_code(pClientInfo->client_id, pClientInfo->dwExitCode);
        // guaranted that all data was already sent (above bPipeClosed==TRUE)
        // so no worry about returning some data after exit code
        RemoveClient(pClientInfo);
        return uResult;
    }
    return ERROR_SUCCESS;
}

// Recognize magic RPC request command ("QUBESRPC") and replace it with real
// command to be executed, after reading RPC service configuration.
// pwszCommandLine will be modified (and possibly reallocated)
// ppwszSourceDomainName will contain source domain (if available) to be set in
// environment; must be freed by caller
ULONG InterceptRPCRequest(WCHAR *pwszCommandLine, WCHAR **ppwszServiceCommandLine, WCHAR **ppwszSourceDomainName)
{
    WCHAR *pwszServiceName = NULL;
    WCHAR *pwszSourceDomainName = NULL;
    WCHAR *pwSeparator = NULL;
    UCHAR szBuffer[sizeof(WCHAR) * (MAX_PATH + 1)];
    WCHAR wszServiceFilePath[MAX_PATH + 1];
    WCHAR *pwszRawServiceFilePath = NULL;
    WCHAR *pwszServiceArgs = NULL;
    HANDLE hServiceConfigFile;
    ULONG uResult;
    ULONG uBytesRead;
    ULONG uPathLength;
    WCHAR *pwszServiceCommandLine = NULL;

    if (!pwszCommandLine || !ppwszServiceCommandLine || !ppwszSourceDomainName)
        return ERROR_INVALID_PARAMETER;

    *ppwszServiceCommandLine = *ppwszSourceDomainName = NULL;

    if (wcsncmp(pwszCommandLine, RPC_REQUEST_COMMAND, wcslen(RPC_REQUEST_COMMAND)) == 0)
    {
        // RPC_REQUEST_COMMAND contains trailing space, so this must succeed
#pragma prefast(suppress:28193, "RPC_REQUEST_COMMAND contains trailing space, so this must succeed")
        pwSeparator = wcschr(pwszCommandLine, L' ');
        pwSeparator++;
        pwszServiceName = pwSeparator;
        pwSeparator = wcschr(pwszServiceName, L' ');
        if (pwSeparator)
        {
            *pwSeparator = L'\0';
            pwSeparator++;
            pwszSourceDomainName = _wcsdup(pwSeparator);
            if (!pwszSourceDomainName)
            {
                return perror2(ERROR_NOT_ENOUGH_MEMORY, "_wcsdup");
            }
        }
        else
        {
            LogInfo("No source domain given\n");
            // Most qrexec services do not use source domain at all, so do not
            // abort if missing. This can be the case when RPC triggered
            // manualy using qvm-run (qvm-run -p vmname "QUBESRPC service_name").
        }

        // build RPC service config file path
        memset(wszServiceFilePath, 0, sizeof(wszServiceFilePath));
        if (!GetModuleFileNameW(NULL, wszServiceFilePath, MAX_PATH))
        {
            uResult = GetLastError();
            if (pwszSourceDomainName)
                free(pwszSourceDomainName);
            return perror2(uResult, "GetModuleFileName");
        }
        // cut off file name (qrexec_agent.exe)
        pwSeparator = wcsrchr(wszServiceFilePath, L'\\');
        if (!pwSeparator)
        {
            if (pwszSourceDomainName)
                free(pwszSourceDomainName);
            LogError("Cannot find dir containing qrexec_agent.exe\n");
            return ERROR_PATH_NOT_FOUND;
        }
        *pwSeparator = L'\0';
        // cut off one dir (bin)
        pwSeparator = wcsrchr(wszServiceFilePath, L'\\');
        if (!pwSeparator)
        {
            if (pwszSourceDomainName)
                free(pwszSourceDomainName);
            LogError("Cannot find dir containing bin\\qrexec_agent.exe\n");
            return ERROR_PATH_NOT_FOUND;
        }
        // Leave trailing backslash
        pwSeparator++;
        *pwSeparator = L'\0';
        if (wcslen(wszServiceFilePath) + wcslen(L"qubes-rpc\\") + wcslen(pwszServiceName) > MAX_PATH)
        {
            if (pwszSourceDomainName)
                free(pwszSourceDomainName);
            LogError("RPC service config file path too long\n");
            return ERROR_PATH_NOT_FOUND;
        }
        PathAppendW(wszServiceFilePath, L"qubes-rpc");
        PathAppendW(wszServiceFilePath, pwszServiceName);

        hServiceConfigFile = CreateFileW(
            wszServiceFilePath,    // file to open
            GENERIC_READ,          // open for reading
            FILE_SHARE_READ,       // share for reading
            NULL,                  // default security
            OPEN_EXISTING,         // existing file only
            FILE_ATTRIBUTE_NORMAL, // normal file
            NULL);                 // no attr. template

        if (hServiceConfigFile == INVALID_HANDLE_VALUE)
        {
            uResult = perror("CreateFile");
            if (pwszSourceDomainName)
                free(pwszSourceDomainName);
            LogError("Failed to open RPC %s configuration file (%s)", pwszServiceName, wszServiceFilePath);
            return uResult;
        }

        uBytesRead = 0;
        memset(szBuffer, 0, sizeof(szBuffer));

        if (!ReadFile(hServiceConfigFile, szBuffer, sizeof(WCHAR) * MAX_PATH, &uBytesRead, NULL))
        {
            uResult = perror("ReadFile");
            if (pwszSourceDomainName)
                free(pwszSourceDomainName);
            LogError("Failed to read RPC %s configuration file (%s)", pwszServiceName, wszServiceFilePath);
            CloseHandle(hServiceConfigFile);
            return uResult;
        }
        CloseHandle(hServiceConfigFile);

        uResult = TextBOMToUTF16(szBuffer, uBytesRead, &pwszRawServiceFilePath);
        if (uResult != ERROR_SUCCESS)
        {
            perror2(uResult, "TextBOMToUTF16");
            if (pwszSourceDomainName)
                free(pwszSourceDomainName);
            LogError("Failed to parse the encoding in RPC %s configuration file (%s)", pwszServiceName, wszServiceFilePath);
            return uResult;
        }

        // strip white chars (especially end-of-line) from string
        uPathLength = wcslen(pwszRawServiceFilePath);
        while (iswspace(pwszRawServiceFilePath[uPathLength - 1]))
        {
            uPathLength--;
            pwszRawServiceFilePath[uPathLength] = L'\0';
        }

        pwszServiceArgs = PathGetArgsW(pwszRawServiceFilePath);
        PathRemoveArgsW(pwszRawServiceFilePath);
        PathUnquoteSpacesW(pwszRawServiceFilePath);
        if (PathIsRelativeW(pwszRawServiceFilePath))
        {
            // relative path are based in qubes-rpc-services
            // reuse separator found when preparing previous file path
            *pwSeparator = L'\0';
            PathAppendW(wszServiceFilePath, L"qubes-rpc-services");
            PathAppendW(wszServiceFilePath, pwszRawServiceFilePath);
        }
        else
        {
            StringCchCopyW(wszServiceFilePath, MAX_PATH + 1, pwszRawServiceFilePath);
        }
        PathQuoteSpacesW(wszServiceFilePath);
        if (pwszServiceArgs && pwszServiceArgs[0] != L'\0')
        {
            StringCchCatW(wszServiceFilePath, MAX_PATH + 1, L" ");
            StringCchCatW(wszServiceFilePath, MAX_PATH + 1, pwszServiceArgs);
        }
        free(pwszRawServiceFilePath);
        pwszServiceCommandLine = malloc((wcslen(wszServiceFilePath) + 1) * sizeof(WCHAR));
        if (pwszServiceCommandLine == NULL)
        {
            if (pwszSourceDomainName)
                free(pwszSourceDomainName);
            return perror2(ERROR_NOT_ENOUGH_MEMORY, "malloc");
        }
        LogDebug("RPC %s: %s\n", pwszServiceName, wszServiceFilePath);
        StringCchCopyW(pwszServiceCommandLine, wcslen(wszServiceFilePath) + 1, wszServiceFilePath);

        *ppwszServiceCommandLine = pwszServiceCommandLine;
        *ppwszSourceDomainName = pwszSourceDomainName;
    }
    return ERROR_SUCCESS;
}

// This will return error only if vchan fails.
ULONG handle_connect_existing(int client_id, int len)
{
    ULONG uResult;
    char *buf;

    if (!len)
        return ERROR_SUCCESS;

    buf = malloc(len + 1);
    if (!buf)
        return ERROR_SUCCESS;
    buf[len] = 0;

    if (read_all_vchan_ext(buf, len) <= 0)
    {
        free(buf);
        return perror2(ERROR_INVALID_FUNCTION, "read_all_vchan_ext");
    }

    LogDebug("client %d, ident %S\n", client_id, buf);

    uResult = ProceedWithExecution(client_id, buf);
    free(buf);

    if (ERROR_SUCCESS != uResult)
        perror2(uResult, "ProceedWithExecution");

    return ERROR_SUCCESS;
}

// This will return error only if vchan fails.
ULONG handle_exec(int client_id, int len)
{
    char *buf;
    ULONG uResult;
    WCHAR *pwszCommand = NULL;
    WCHAR *pwszUserName = NULL;
    WCHAR *pwszCommandLine = NULL;
    WCHAR *pwszServiceCommandLine = NULL;
    WCHAR *pwszRemoteDomainName = NULL;
    BOOLEAN bRunInteractively;

    buf = malloc(len + 1);
    if (!buf)
        return ERROR_SUCCESS;
    buf[len] = 0;

    if (read_all_vchan_ext(buf, len) <= 0)
    {
        free(buf);
        return perror2(ERROR_INVALID_FUNCTION, "read_all_vchan_ext");
    }

    bRunInteractively = TRUE;

    uResult = ParseUtf8Command(buf, &pwszCommand, &pwszUserName, &pwszCommandLine, &bRunInteractively);
    if (ERROR_SUCCESS != uResult)
    {
        free(buf);
        send_exit_code(client_id, MAKE_ERROR_RESPONSE(ERROR_SET_WINDOWS, uResult));
        perror2(uResult, "ParseUtf8Command");
        return ERROR_SUCCESS;
    }

    free(buf);
    buf = NULL;

    uResult = InterceptRPCRequest(pwszCommandLine, &pwszServiceCommandLine, &pwszRemoteDomainName);
    if (ERROR_SUCCESS != uResult)
    {
        free(pwszCommand);
        send_exit_code(client_id, MAKE_ERROR_RESPONSE(ERROR_SET_WINDOWS, uResult));
        perror2(uResult, "InterceptRPCRequest");
        return ERROR_SUCCESS;
    }

    if (pwszServiceCommandLine)
        pwszCommandLine = pwszServiceCommandLine;

    if (pwszRemoteDomainName)
        SetEnvironmentVariableW(L"QREXEC_REMOTE_DOMAIN", pwszRemoteDomainName);

    // Create a process and redirect its console IO to vchan.
    uResult = AddClient(client_id, pwszUserName, pwszCommandLine, bRunInteractively);
    if (ERROR_SUCCESS == uResult)
        LogInfo("Executed: %s\n", pwszCommandLine);
    else
    {
        send_exit_code(client_id, MAKE_ERROR_RESPONSE(ERROR_SET_WINDOWS, uResult));
        LogWarning("AddClient('%s') failed: %d", pwszCommandLine, uResult);
    }

    if (pwszRemoteDomainName)
    {
        SetEnvironmentVariableW(L"QREXEC_REMOTE_DOMAIN", NULL);
        free(pwszRemoteDomainName);
    }
    if (pwszServiceCommandLine)
        free(pwszServiceCommandLine);

    free(pwszCommand);
    return ERROR_SUCCESS;
}

// This will return error only if vchan fails.
ULONG handle_just_exec(int client_id, int len)
{
    char *buf;
    ULONG uResult;
    WCHAR *pwszCommand = NULL;
    WCHAR *pwszUserName = NULL;
    WCHAR *pwszCommandLine = NULL;
    WCHAR *pwszServiceCommandLine = NULL;
    WCHAR *pwszRemoteDomainName = NULL;
    HANDLE hProcess;
    BOOLEAN bRunInteractively;

    buf = malloc(len + 1);
    if (!buf)
        return ERROR_SUCCESS;
    buf[len] = 0;

    if (read_all_vchan_ext(buf, len) <= 0)
    {
        free(buf);
        return perror2(ERROR_INVALID_FUNCTION, "read_all_vchan_ext");
    }

    bRunInteractively = TRUE;

    uResult = ParseUtf8Command(buf, &pwszCommand, &pwszUserName, &pwszCommandLine, &bRunInteractively);
    if (ERROR_SUCCESS != uResult)
    {
        free(buf);
        send_exit_code(client_id, MAKE_ERROR_RESPONSE(ERROR_SET_WINDOWS, uResult));
        perror2(uResult, "ParseUtf8Command");
        return ERROR_SUCCESS;
    }

    free(buf);
    buf = NULL;

    uResult = InterceptRPCRequest(pwszCommandLine, &pwszServiceCommandLine, &pwszRemoteDomainName);
    if (ERROR_SUCCESS != uResult)
    {
        free(pwszCommand);
        send_exit_code(client_id, MAKE_ERROR_RESPONSE(ERROR_SET_WINDOWS, uResult));
        perror2(uResult, "InterceptRPCRequest");
        return ERROR_SUCCESS;
    }

    if (pwszServiceCommandLine)
        pwszCommandLine = pwszServiceCommandLine;

    if (pwszRemoteDomainName)
        SetEnvironmentVariableW(L"QREXEC_REMOTE_DOMAIN", pwszRemoteDomainName);

    LogDebug("Command line: %s", pwszCommandLine);

#ifdef BUILD_AS_SERVICE
    // Create a process which IO is not redirected anywhere.
    uResult = CreateNormalProcessAsUser(
        pwszUserName,
        DEFAULT_USER_PASSWORD_UNICODE,
        pwszCommandLine,
        bRunInteractively,
        &hProcess);
#else
    uResult = CreateNormalProcessAsCurrentUser(
        pwszCommandLine,
        &hProcess);
#endif

    if (ERROR_SUCCESS == uResult)
    {
        CloseHandle(hProcess);
        LogInfo("Executed: %s\n", pwszCommandLine);
    }
    else
    {
        send_exit_code(client_id, MAKE_ERROR_RESPONSE(ERROR_SET_WINDOWS, uResult));
#ifdef BUILD_AS_SERVICE
        perror2(uResult, "CreateNormalProcessAsUser");
#else
        perror2(uResult, "CreateNormalProcessAsCurrentUser");
#endif
    }

    if (pwszRemoteDomainName)
    {
        SetEnvironmentVariableW(L"QREXEC_REMOTE_DOMAIN", NULL);
        free(pwszRemoteDomainName);
    }
    if (pwszServiceCommandLine)
        free(pwszServiceCommandLine);

    free(pwszCommand);
    return ERROR_SUCCESS;
}

// This will return error only if vchan fails.
ULONG handle_input(int client_id, int len)
{
    char *buf;
    CLIENT_INFO *pClientInfo;
    DWORD dwWritten;

    // If pClientInfo is NULL after this it means we couldn't find a specified client.
    // Read and discard any data in the channel in this case.
    pClientInfo = FindClientById(client_id);

    if (!len)
    {
        if (pClientInfo)
        {
            CloseHandle(pClientInfo->hWriteStdinPipe);
            pClientInfo->bStdinPipeClosed = TRUE;
        }
        return ERROR_SUCCESS;
    }

    buf = malloc(len + 1);
    if (!buf)
        return ERROR_SUCCESS;
    buf[len] = 0;

    if (read_all_vchan_ext(buf, len) <= 0)
    {
        free(buf);
        return perror2(ERROR_INVALID_FUNCTION, "read_all_vchan_ext");
    }

    if (pClientInfo && !pClientInfo->bStdinPipeClosed)
    {
        if (!WriteFile(pClientInfo->hWriteStdinPipe, buf, len, &dwWritten, NULL))
            perror("WriteFile");
    }

    free(buf);
    return ERROR_SUCCESS;
}

void set_blocked_outerr(int client_id, BOOLEAN bBlockOutput)
{
    CLIENT_INFO *pClientInfo;

    pClientInfo = FindClientById(client_id);
    if (!pClientInfo)
        return;

    pClientInfo->bReadingIsDisabled = bBlockOutput;
}

ULONG handle_server_data()
{
    struct server_header s_hdr;
    ULONG uResult;

    if (read_all_vchan_ext(&s_hdr, sizeof s_hdr) <= 0)
    {
        return perror2(ERROR_INVALID_FUNCTION, "read_all_vchan_ext");
    }

    //	lprintf("got %x %x %x\n", s_hdr.type, s_hdr.client_id, s_hdr.len);

    switch (s_hdr.type)
    {
    case MSG_XON:
        LogDebug("MSG_XON\n");
        set_blocked_outerr(s_hdr.client_id, FALSE);
        break;
    case MSG_XOFF:
        LogDebug("MSG_XOFF\n");
        set_blocked_outerr(s_hdr.client_id, TRUE);
        break;
    case MSG_SERVER_TO_AGENT_CONNECT_EXISTING:
        LogDebug("MSG_SERVER_TO_AGENT_CONNECT_EXISTING\n");
        handle_connect_existing(s_hdr.client_id, s_hdr.len);
        break;
    case MSG_SERVER_TO_AGENT_EXEC_CMDLINE:
        LogDebug("MSG_SERVER_TO_AGENT_EXEC_CMDLINE\n");

        // This will return error only if vchan fails.
        uResult = handle_exec(s_hdr.client_id, s_hdr.len);
        if (ERROR_SUCCESS != uResult)
        {
            return perror2(uResult, "handle_exec");
        }
        break;

    case MSG_SERVER_TO_AGENT_JUST_EXEC:
        LogDebug("MSG_SERVER_TO_AGENT_JUST_EXEC\n");

        // This will return error only if vchan fails.
        uResult = handle_just_exec(s_hdr.client_id, s_hdr.len);
        if (ERROR_SUCCESS != uResult)
        {
            return perror2(uResult, "handle_just_exec");
        }
        break;

    case MSG_SERVER_TO_AGENT_INPUT:
        LogDebug("MSG_SERVER_TO_AGENT_INPUT\n");

        // This will return error only if vchan fails.
        uResult = handle_input(s_hdr.client_id, s_hdr.len);
        if (ERROR_SUCCESS != uResult)
        {
            return perror2(uResult, "handle_input");
        }
        break;

    case MSG_SERVER_TO_AGENT_CLIENT_END:
        LogDebug("MSG_SERVER_TO_AGENT_CLIENT_END\n");
        RemoveClient(FindClientById(s_hdr.client_id));
        break;
    default:
        LogWarning("Unknown msg type from daemon: %d\n", s_hdr.type);
        return ERROR_INVALID_FUNCTION;
    }

    return ERROR_SUCCESS;
}

// returns number of filled events (0 or 1)
ULONG FillAsyncIoData(ULONG uEventNumber, ULONG uClientNumber, UCHAR bHandleType, PIPE_DATA *pPipeData)
{
    ULONG uResult;

    if (uEventNumber >= RTL_NUMBER_OF(g_WatchedEvents) ||
        uClientNumber >= RTL_NUMBER_OF(g_Clients) ||
        !pPipeData)
        return 0;

    if (!pPipeData->bReadInProgress && !pPipeData->bDataIsReady && !pPipeData->bPipeClosed && !pPipeData->bVchanWritePending)
    {
        memset(&pPipeData->ReadBuffer, 0, READ_BUFFER_SIZE);
        pPipeData->dwSentBytes = 0;

        if (!ReadFile(
            pPipeData->hReadPipe,
            &pPipeData->ReadBuffer,
            READ_BUFFER_SIZE,
            NULL,
            &pPipeData->olRead))
        {
            // Last error is usually ERROR_IO_PENDING here because of the asynchronous read.
            // But if the process has closed it would be ERROR_BROKEN_PIPE,
            // in this case ReturnPipeData will send EOF notification and set bPipeClosed.
            uResult = GetLastError();
            if (ERROR_IO_PENDING == uResult)
                pPipeData->bReadInProgress = TRUE;
            if (ERROR_BROKEN_PIPE == uResult)
            {
                SetEvent(pPipeData->olRead.hEvent);
                pPipeData->bDataIsReady = TRUE;
            }
        }
        else
        {
            // The read has completed synchronously.
            // The event in the OVERLAPPED structure should be signalled by now.
            pPipeData->bDataIsReady = TRUE;

            // Do not set bReadInProgress to TRUE in this case because if the pipes are to be closed
            // before the next read IO starts then there will be no IO to cancel.
            // bReadInProgress indicates to the CloseReadPipeHandles() that the IO should be canceled.

            // If after the WaitFormultipleObjects() this event is not chosen because of
            // some other event is also signaled, we will not rewrite the data in the buffer
            // on the next iteration of FillAsyncIoData() because bDataIsReady is set.
        }
    }

    // when bVchanWritePending==TRUE, ReturnPipeData already reset bReadInProgress and bDataIsReady
    if (pPipeData->bReadInProgress || pPipeData->bDataIsReady)
    {
        g_HandlesInfo[uEventNumber].uClientNumber = uClientNumber;
        g_HandlesInfo[uEventNumber].bType = bHandleType;
        g_WatchedEvents[uEventNumber] = pPipeData->olRead.hEvent;
        return 1;
    }

    return 0;
}

ULONG WatchForEvents()
{
    EVTCHN evtchn;
    OVERLAPPED ol;
    unsigned int fired_port;
    ULONG i, uEventNumber, uClientNumber;
    DWORD dwSignaledEvent;
    CLIENT_INFO *pClientInfo;
    DWORD dwExitCode;
    BOOLEAN bVchanIoInProgress;
    ULONG uResult;
    BOOLEAN bVchanReturnedError;
    BOOLEAN bVchanClientConnected;

    // This will not block.
    uResult = peer_server_init(REXEC_PORT);
    if (uResult)
    {
        return perror2(ERROR_INVALID_FUNCTION, "WatchForEvents(): peer_server_init()");
    }

    LogInfo("Awaiting for a vchan client, write ring size: %d\n", buffer_space_vchan_ext());

    evtchn = libvchan_fd_for_select(ctrl);

    memset(&ol, 0, sizeof(ol));
    ol.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    bVchanClientConnected = FALSE;
    bVchanIoInProgress = FALSE;
    bVchanReturnedError = FALSE;

    for (;;)
    {
        uEventNumber = 0;

        // Order matters.
        g_WatchedEvents[uEventNumber++] = g_hStopServiceEvent;
        g_WatchedEvents[uEventNumber++] = g_hAddExistingClientEvent;

        g_HandlesInfo[0].bType = g_HandlesInfo[1].bType = HTYPE_INVALID;

        uResult = ERROR_SUCCESS;

        libvchan_prepare_to_select(ctrl);
        // read 1 byte instead of sizeof(fired_port) to not flush fired port
        // from evtchn buffer; evtchn driver will read only whole fired port
        // numbers (sizeof(fired_port)), so this will end in zero-length read
        if (!ReadFile(evtchn, &fired_port, 1, NULL, &ol))
        {
            uResult = GetLastError();
            if (ERROR_IO_PENDING != uResult)
            {
                perror("Vchan async read");
                bVchanReturnedError = TRUE;
                break;
            }
        }

        bVchanIoInProgress = TRUE;

        if (ERROR_SUCCESS == uResult || ERROR_IO_PENDING == uResult)
        {
            g_HandlesInfo[uEventNumber].uClientNumber = FREE_CLIENT_SPOT_ID;
            g_HandlesInfo[uEventNumber].bType = HTYPE_VCHAN;
            g_WatchedEvents[uEventNumber++] = ol.hEvent;
        }

        EnterCriticalSection(&g_ClientsCriticalSection);

        for (uClientNumber = 0; uClientNumber < MAX_CLIENTS; uClientNumber++)
        {
            if (g_Clients[uClientNumber].bClientIsReady)
            {
                if (!g_Clients[uClientNumber].bChildExited)
                {
                    g_HandlesInfo[uEventNumber].uClientNumber = uClientNumber;
                    g_HandlesInfo[uEventNumber].bType = HTYPE_PROCESS;
                    g_WatchedEvents[uEventNumber++] = g_Clients[uClientNumber].hProcess;
                }

                if (!g_Clients[uClientNumber].bReadingIsDisabled)
                {
                    // Skip those clients which have received MSG_XOFF.
                    uEventNumber += FillAsyncIoData(uEventNumber, uClientNumber, HTYPE_STDOUT, &g_Clients[uClientNumber].Stdout);
                    uEventNumber += FillAsyncIoData(uEventNumber, uClientNumber, HTYPE_STDERR, &g_Clients[uClientNumber].Stderr);
                }
            }
        }
        LeaveCriticalSection(&g_ClientsCriticalSection);

        dwSignaledEvent = WaitForMultipleObjects(uEventNumber, g_WatchedEvents, FALSE, INFINITE);
        if (dwSignaledEvent >= MAXIMUM_WAIT_OBJECTS)
        {
            uResult = GetLastError();
            if (ERROR_INVALID_HANDLE != uResult)
            {
                perror2(uResult, "WaitForMultipleObjects");
                break;
            }

            // WaitForMultipleObjects() may fail with ERROR_INVALID_HANDLE if the process which just has been added
            // to the client list terminated before WaitForMultipleObjects(). In this case IO pipe handles are closed
            // and invalidated, while a process handle is in the signaled state.
            // Check if any of the processes in the client list is terminated, remove it from the list and try again.

            EnterCriticalSection(&g_ClientsCriticalSection);

            for (uClientNumber = 0; uClientNumber < MAX_CLIENTS; uClientNumber++)
            {
                pClientInfo = &g_Clients[uClientNumber];

                if (!g_Clients[uClientNumber].bClientIsReady)
                    continue;

                if (!GetExitCodeProcess(pClientInfo->hProcess, &dwExitCode))
                {
                    perror("GetExitCodeProcess");
                    dwExitCode = ERROR_SUCCESS;
                }

                if (STILL_ACTIVE != dwExitCode)
                {
                    int client_id;

                    client_id = pClientInfo->client_id;
                    pClientInfo->bChildExited = TRUE;
                    pClientInfo->dwExitCode = dwExitCode;
                    // send exit code only when all data was sent to the daemon
                    uResult = PossiblyHandleTerminatedClientNoLocks(pClientInfo);
                    if (ERROR_SUCCESS != uResult)
                    {
                        bVchanReturnedError = TRUE;
                        perror2(uResult, "send_exit_code");
                    }
                }
            }
            LeaveCriticalSection(&g_ClientsCriticalSection);

            continue;
        }
        else
        {
            if (0 == dwSignaledEvent)
                // g_hStopServiceEvent is signaled
                break;

            if (HTYPE_VCHAN != g_HandlesInfo[dwSignaledEvent].bType)
            {
                // If this is not a vchan event, cancel the event channel read so that libvchan_write() calls
                // could issue their own libvchan_wait on the same channel, and not interfere with the
                // ReadFile(evtchn, ...) above.
                if (CancelIo(evtchn))
                    // Must wait for the canceled IO to complete, otherwise a race condition may occur on the
                    // OVERLAPPED structure.
                    WaitForSingleObject(ol.hEvent, INFINITE);
                bVchanIoInProgress = FALSE;
            }

            if (1 == dwSignaledEvent)
                // g_hAddExistingClientEvent is signaled. Since Vchan IO has been canceled,
                // safely re-iterate the loop and pick up the new handles to watch.
                continue;

            // Do not have to lock g_Clients here because other threads may only call
            // ReserveClientNumber()/ReleaseClientNumber()/AddFilledClientInfo()
            // which operate on different uClientNumbers than those specified for WaitForMultipleObjects().

            // The other threads cannot call RemoveClient(), for example, they
            // operate only on newly allocated uClientNumbers.

            // So here in this thread we may call FindByClientId() with no locks safely.

            // When this thread (in this switch) calls RemoveClient() later the g_Clients
            // list will be locked as usual.

            // lprintf("client %d, type %d, signaled: %d, en %d\n", g_HandlesInfo[dwSignaledEvent].uClientNumber, g_HandlesInfo[dwSignaledEvent].bType, dwSignaledEvent, uEventNumber);
            switch (g_HandlesInfo[dwSignaledEvent].bType)
            {
            case HTYPE_VCHAN:

                // the following will never block; we need to do this to
                // clear libvchan_fd pending state
                //
                // using libvchan_wait here instead of reading fired
                // port at the beginning of the loop (ReadFile call) to be
                // sure that we clear pending state _only_
                // when handling vchan data in this loop iteration (not any
                // other process)
                libvchan_wait(ctrl);

                bVchanIoInProgress = FALSE;

                if (!bVchanClientConnected)
                {
                    LogInfo("A vchan client has connected\n");

                    // Remove the xenstore device/vchan/N entry.
                    uResult = libvchan_server_handle_connected(ctrl);
                    if (uResult)
                    {
                        perror2(ERROR_INVALID_FUNCTION, "libvchan_server_handle_connected");
                        bVchanReturnedError = TRUE;
                        break;
                    }

                    bVchanClientConnected = TRUE;

                    // ignore error - perhaps core-admin too old and didn't
                    // create appropriate xenstore directory?
                    advertise_tools();

                    break;
                }

                if (!GetOverlappedResult(evtchn, &ol, &i, FALSE))
                {
                    if (GetLastError() == ERROR_IO_DEVICE)
                    {
                        // in case of ring overflow, above libvchan_wait
                        // already reseted the evtchn ring, so ignore this
                        // error as already handled
                        //
                        // Overflow can happen when below loop ("while
                        // (read_ready_vchan_ext())") handle a lot of data
                        // in the same time as qrexec-daemon writes it -
                        // there where be no libvchan_wait call (which
                        // receive the events from the ring), but one will
                        // be signaled after each libvchan_write in
                        // qrexec-daemon. I don't know how to fix it
                        // properly (without introducing any race
                        // condition), so reset the evtchn ring (do not
                        // confuse with vchan ring, which stays untouched)
                        // in case of overflow.
                    }
                    else if (GetLastError() != ERROR_OPERATION_ABORTED)
                    {
                        perror("GetOverlappedResult(evtchn)");
                        bVchanReturnedError = TRUE;
                        break;
                    }
                }

                EnterCriticalSection(&g_VchanCriticalSection);

                if (libvchan_is_eof(ctrl))
                {
                    bVchanReturnedError = TRUE;
                    LeaveCriticalSection(&g_VchanCriticalSection);
                    break;
                }

                while (read_ready_vchan_ext())
                {
                    uResult = handle_server_data();
                    if (ERROR_SUCCESS != uResult)
                    {
                        bVchanReturnedError = TRUE;
                        perror2(uResult, "handle_server_data");
                        LeaveCriticalSection(&g_VchanCriticalSection);
                        break;
                    }
                }

                LeaveCriticalSection(&g_VchanCriticalSection);

                EnterCriticalSection(&g_ClientsCriticalSection);

                for (uClientNumber = 0; uClientNumber < MAX_CLIENTS; uClientNumber++)
                {
                    if (g_Clients[uClientNumber].bClientIsReady && !g_Clients[uClientNumber].bReadingIsDisabled)
                    {
                        if (g_Clients[uClientNumber].Stdout.bVchanWritePending)
                        {
                            uResult = ReturnPipeData(g_Clients[uClientNumber].client_id, &g_Clients[uClientNumber].Stdout);
                            if (ERROR_HANDLE_EOF == uResult)
                            {
                                PossiblyHandleTerminatedClientNoLocks(&g_Clients[uClientNumber]);
                            }
                            else if (ERROR_INSUFFICIENT_BUFFER == uResult)
                            {
                                // no more space in vchan
                                break;
                            }
                            else if (ERROR_SUCCESS != uResult)
                            {
                                bVchanReturnedError = TRUE;
                                perror2(uResult, "ReturnPipeData(STDOUT)");
                            }
                        }
                        if (g_Clients[uClientNumber].Stderr.bVchanWritePending)
                        {
                            uResult = ReturnPipeData(g_Clients[uClientNumber].client_id, &g_Clients[uClientNumber].Stderr);
                            if (ERROR_HANDLE_EOF == uResult)
                            {
                                PossiblyHandleTerminatedClientNoLocks(&g_Clients[uClientNumber]);
                            }
                            else if (ERROR_INSUFFICIENT_BUFFER == uResult)
                            {
                                // no more space in vchan
                                break;
                            }
                            else if (ERROR_SUCCESS != uResult)
                            {
                                bVchanReturnedError = TRUE;
                                perror2(uResult, "ReturnPipeData(STDERR)");
                            }
                        }
                    }
                }

                LeaveCriticalSection(&g_ClientsCriticalSection);

                break;

            case HTYPE_STDOUT:
#ifdef DISPLAY_CONSOLE_OUTPUT
                printf("%s", &g_Clients[g_HandlesInfo[dwSignaledEvent].uClientNumber].Stdout.ReadBuffer);
#endif

                uResult = ReturnPipeData(
                    g_Clients[g_HandlesInfo[dwSignaledEvent].uClientNumber].client_id,
                    &g_Clients[g_HandlesInfo[dwSignaledEvent].uClientNumber].Stdout);
                if (ERROR_HANDLE_EOF == uResult)
                {
                    PossiblyHandleTerminatedClient(&g_Clients[g_HandlesInfo[dwSignaledEvent].uClientNumber]);
                }
                else if (ERROR_SUCCESS != uResult && ERROR_INSUFFICIENT_BUFFER != uResult)
                {
                    bVchanReturnedError = TRUE;
                    perror2(uResult, "ReturnPipeData(STDOUT)");
                }
                break;

            case HTYPE_STDERR:
#ifdef DISPLAY_CONSOLE_OUTPUT
                printf("%s", &g_Clients[g_HandlesInfo[dwSignaledEvent].uClientNumber].Stderr.ReadBuffer);
#endif

                uResult = ReturnPipeData(
                    g_Clients[g_HandlesInfo[dwSignaledEvent].uClientNumber].client_id,
                    &g_Clients[g_HandlesInfo[dwSignaledEvent].uClientNumber].Stderr);
                if (ERROR_HANDLE_EOF == uResult)
                {
                    PossiblyHandleTerminatedClient(&g_Clients[g_HandlesInfo[dwSignaledEvent].uClientNumber]);
                }
                else if (ERROR_SUCCESS != uResult && ERROR_INSUFFICIENT_BUFFER != uResult)
                {
                    bVchanReturnedError = TRUE;
                    perror2(uResult, "ReturnPipeData(STDERR)");
                }
                break;

            case HTYPE_PROCESS:

                pClientInfo = &g_Clients[g_HandlesInfo[dwSignaledEvent].uClientNumber];

                if (!GetExitCodeProcess(pClientInfo->hProcess, &dwExitCode))
                {
                    perror("GetExitCodeProcess");
                    dwExitCode = ERROR_SUCCESS;
                }

                pClientInfo->bChildExited = TRUE;
                pClientInfo->dwExitCode = dwExitCode;
                // send exit code only when all data was sent to the daemon
                uResult = PossiblyHandleTerminatedClient(pClientInfo);
                if (ERROR_SUCCESS != uResult)
                {
                    bVchanReturnedError = TRUE;
                    perror2(uResult, "send_exit_code");
                }

                break;
            }
        }

        if (bVchanReturnedError)
            break;
    }

    if (bVchanIoInProgress)
        if (CancelIo(evtchn))
            // Must wait for the canceled IO to complete, otherwise a race condition may occur on the
            // OVERLAPPED structure.
            WaitForSingleObject(ol.hEvent, INFINITE);

    if (!bVchanClientConnected)
        // Remove the xenstore device/vchan/N entry.
        libvchan_server_handle_connected(ctrl);

    // Cancel all the other pending IO.
    RemoveAllClients();

    if (bVchanClientConnected)
        libvchan_close(ctrl);

    // This is actually CloseHandle(evtchn)
    xc_evtchn_close(ctrl->evfd);

    CloseHandle(ol.hEvent);

    return bVchanReturnedError ? ERROR_INVALID_FUNCTION : ERROR_SUCCESS;
}

VOID Usage()
{
    LogError("qrexec agent service\n\nUsage: qrexec_agent <-i|-u>\n");
}

ULONG CheckForXenInterface()
{
    EVTCHN xc;

    xc = xc_evtchn_open();
    if (INVALID_HANDLE_VALUE == xc)
        return ERROR_NOT_SUPPORTED;

    xc_evtchn_close(xc);
    return ERROR_SUCCESS;
}

ULONG WINAPI ServiceExecutionThread(void *pParam)
{
    ULONG uResult;
    HANDLE hTriggerEventsThread;

    LogInfo("Service started\n");

    // Auto reset, initial state is not signaled
    g_hAddExistingClientEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_hAddExistingClientEvent)
    {
        return perror("CreateEvent");
    }

    hTriggerEventsThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE) WatchForTriggerEvents, NULL, 0, NULL);
    if (!hTriggerEventsThread)
    {
        uResult = GetLastError();
        CloseHandle(g_hAddExistingClientEvent);
        return perror2(uResult, "CreateThread");
    }

    for (;;)
    {
        uResult = WatchForEvents();
        if (ERROR_SUCCESS != uResult)
            perror2(uResult, "WatchForEvents");

        if (!WaitForSingleObject(g_hStopServiceEvent, 0))
            break;

        Sleep(1000);
    }

    LogDebug("Waiting for the trigger thread to exit\n");
    WaitForSingleObject(hTriggerEventsThread, INFINITE);
    CloseHandle(hTriggerEventsThread);
    CloseHandle(g_hAddExistingClientEvent);

    DeleteCriticalSection(&g_ClientsCriticalSection);
    DeleteCriticalSection(&g_VchanCriticalSection);

    LogInfo("Shutting down\n");

    return ERROR_SUCCESS;
}

#ifdef BUILD_AS_SERVICE

ULONG Init(HANDLE *phServiceThread)
{
    ULONG uResult;
    HANDLE hThread;
    ULONG uClientNumber;

    *phServiceThread = INVALID_HANDLE_VALUE;

    uResult = CheckForXenInterface();
    if (ERROR_SUCCESS != uResult)
    {
        return perror2(uResult, "CheckForXenInterface");
    }

    InitializeCriticalSection(&g_ClientsCriticalSection);
    InitializeCriticalSection(&g_VchanCriticalSection);
    InitializeCriticalSection(&g_PipesCriticalSection);

    for (uClientNumber = 0; uClientNumber < MAX_CLIENTS; uClientNumber++)
        g_Clients[uClientNumber].client_id = FREE_CLIENT_SPOT_ID;

    hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE) ServiceExecutionThread, NULL, 0, NULL);
    if (!hThread)
    {
        return perror("CreateThread");
    }

    *phServiceThread = hThread;

    return ERROR_SUCCESS;
}

// This is the entry point for a service module (BUILD_AS_SERVICE defined).
int __cdecl _tmain(ULONG argc, TCHAR *argv[])
{
    ULONG uOption;
    TCHAR *pszParam = NULL;
    TCHAR szUserName[UNLEN + 1];
    TCHAR szFullPath[MAX_PATH + 1];
    DWORD nSize;
    ULONG uResult;
    BOOL bStop;
    TCHAR bCommand;
    TCHAR *pszAccountName = NULL;

    SERVICE_TABLE_ENTRY	ServiceTable[] = {
        { SERVICE_NAME, (LPSERVICE_MAIN_FUNCTION) ServiceMain },
        { NULL, NULL }
    };

    memset(szUserName, 0, sizeof(szUserName));
    nSize = RTL_NUMBER_OF(szUserName);
    if (!GetUserName(szUserName, &nSize))
    {
        return perror("GetUserName");
    }

    if ((1 == argc) && _tcscmp(szUserName, TEXT("SYSTEM")))
    {
        Usage();
        return ERROR_INVALID_PARAMETER;
    }

    if (1 == argc)
    {
        LogInfo("Running as SYSTEM\n");

        uResult = ERROR_SUCCESS;
        if (!StartServiceCtrlDispatcher(ServiceTable))
        {
            return perror("StartServiceCtrlDispatcher");
        }
    }

    memset(szFullPath, 0, sizeof(szFullPath));
    if (!GetModuleFileName(NULL, szFullPath, RTL_NUMBER_OF(szFullPath) - 1))
    {
        return perror("GetModuleFileName");
    }

    uResult = ERROR_SUCCESS;
    bStop = FALSE;
    bCommand = 0;

    while (!bStop)
    {
        uOption = GetOption(argc, argv, TEXT("iua:"), &pszParam);
        switch (uOption)
        {
        case 0:
            bStop = TRUE;
            break;

        case _T('i'):
        case _T('u'):
            if (bCommand)
            {
                bCommand = 0;
                bStop = TRUE;
            }
            else
                bCommand = (TCHAR) uOption;

            break;

        case _T('a'):
            if (pszParam)
                pszAccountName = pszParam;
            break;

        default:
            bCommand = 0;
            bStop = TRUE;
        }
    }

    if (pszAccountName)
    {
        LogDebug("GrantDesktopAccess('%s')\n", pszAccountName);
        uResult = GrantDesktopAccess(pszAccountName, NULL);
        if (ERROR_SUCCESS != uResult)
            perror2(uResult, "GrantDesktopAccess");

        return uResult;
    }

    switch (bCommand)
    {
    case _T('i'):
        uResult = InstallService(szFullPath, SERVICE_NAME);
        break;

    case _T('u'):
        uResult = UninstallService(SERVICE_NAME);
        break;
    default:
        Usage();
    }

    return uResult;
}

#else

// Is not called when built without BUILD_AS_SERVICE definition.
ULONG Init(HANDLE *phServiceThread)
{
    return ERROR_SUCCESS;
}

BOOL WINAPI CtrlHandler(DWORD fdwCtrlType)
{
    LogInfo("Got shutdown signal\n");

    SetEvent(g_hStopServiceEvent);

    WaitForSingleObject(g_hCleanupFinishedEvent, 2000);

    CloseHandle(g_hStopServiceEvent);
    CloseHandle(g_hCleanupFinishedEvent);

    LogInfo("Shutdown complete\n");
    ExitProcess(0);
    return TRUE;
}

// This is the entry point for a console application (BUILD_AS_SERVICE not defined).
int __cdecl _tmain(ULONG argc, PTCHAR argv[])
{
    ULONG uResult;
    ULONG uClientNumber;

    _tprintf(TEXT("\nqrexec agent console application\n\n"));

    if (ERROR_SUCCESS != CheckForXenInterface()) {
        LogError("Could not find Xen interface\n");
        return ERROR_NOT_SUPPORTED;
    }

    // Manual reset, initial state is not signaled
    g_hStopServiceEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!g_hStopServiceEvent) {
        return perror("CreateEvent");
    }

    // Manual reset, initial state is not signaled
    g_hCleanupFinishedEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!g_hCleanupFinishedEvent) {
        return perror("CreateEvent");
    }

    // InitializeCriticalSection always succeeds in Vista and later OSes.
    InitializeCriticalSection(&g_ClientsCriticalSection);
    InitializeCriticalSection(&g_VchanCriticalSection);
    InitializeCriticalSection(&g_PipesCriticalSection);

    SetConsoleCtrlHandler((HANDLER_ROUTINE *)CtrlHandler, TRUE);

    for (uClientNumber = 0; uClientNumber < MAX_CLIENTS; uClientNumber++)
        g_Clients[uClientNumber].client_id = FREE_CLIENT_SPOT_ID;

    ServiceExecutionThread(NULL);
    SetEvent(g_hCleanupFinishedEvent);

    return ERROR_SUCCESS;
}
#endif
