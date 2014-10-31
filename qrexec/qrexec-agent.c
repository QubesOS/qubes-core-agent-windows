#include "qrexec-agent.h"
#include <Shlwapi.h>
#include "utf8-conv.h"

HANDLE g_AddExistingClientEvent;

CLIENT_INFO g_Clients[MAX_CLIENTS];
HANDLE g_WatchedEvents[MAXIMUM_WAIT_OBJECTS];
HANDLE_INFO	g_HandlesInfo[MAXIMUM_WAIT_OBJECTS];

ULONG64	g_PipeId = 0;

CRITICAL_SECTION g_ClientsCriticalSection;
CRITICAL_SECTION g_VchanCriticalSection;

extern HANDLE g_StopServiceEvent;
#ifndef BUILD_AS_SERVICE
HANDLE g_CleanupFinishedEvent;
#endif

// from advertise_tools.c
ULONG AdvertiseTools(void);

static ULONG CreateAsyncPipe(OUT HANDLE *readPipe, OUT HANDLE *writePipe, IN SECURITY_ATTRIBUTES *securityAttributes)
{
    WCHAR pipeName[MAX_PATH + 1];
    ULONG status;

    LogVerbose("start");
    if (!readPipe || !writePipe)
        return ERROR_INVALID_PARAMETER;

    StringCchPrintf(pipeName, MAX_PATH, L"\\\\.\\pipe\\qrexec.%08x.%I64x", GetCurrentProcessId(), g_PipeId++);

    *readPipe = CreateNamedPipe(
        pipeName,
        PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE,
        1,
        4096,
        4096,
        50,	// the default timeout is 50ms
        securityAttributes);

    if (*readPipe == NULL)
    {
        return perror("CreateNamedPipe");
    }

    *writePipe = CreateFile(
        pipeName,
        GENERIC_WRITE,
        0,
        securityAttributes,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        NULL);

    if (INVALID_HANDLE_VALUE == *writePipe)
    {
        status = perror("CreateFile");
        CloseHandle(*readPipe);
        return status;
    }

    LogVerbose("success");
    return ERROR_SUCCESS;
}

static ULONG InitReadPipe(OUT PIPE_DATA *pipeData, OUT HANDLE *writePipe, IN PIPE_TYPE pipeType)
{
    SECURITY_ATTRIBUTES sa = { 0 };
    ULONG status;

    LogVerbose("pipe type %d", pipeType);

    ZeroMemory(pipeData, sizeof(*pipeData));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    if (!pipeData || !writePipe)
        return ERROR_INVALID_PARAMETER;

    pipeData->ReadState.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!pipeData->ReadState.hEvent)
        return GetLastError();

    status = CreateAsyncPipe(&pipeData->ReadPipe, writePipe, &sa);
    if (ERROR_SUCCESS != status)
    {
        CloseHandle(pipeData->ReadState.hEvent);
        return perror2(status, "CreateAsyncPipe");
    }

    // Ensure the read handle to the pipe is not inherited.
    SetHandleInformation(pipeData->ReadPipe, HANDLE_FLAG_INHERIT, 0);

    pipeData->PipeType = pipeType;
    LogVerbose("success");

    return ERROR_SUCCESS;
}

ULONG SendMessageToDaemon(IN ULONG clientId, IN UINT messageType, IN const void *data, IN ULONG cbData, OUT ULONG *cbWritten)
{
    struct server_header serverHeader;
    int vchanFreeSpace;
    ULONG status = ERROR_SUCCESS;

    LogVerbose("client %d, msg type %d, data %p, size %d", clientId, messageType, data, cbData);

    EnterCriticalSection(&g_VchanCriticalSection);

    if (cbWritten)
    {
        // allow partial write only when puDataWritten given
        *cbWritten = 0;
        vchanFreeSpace = VchanGetWriteBufferSize();
        if (vchanFreeSpace < sizeof(serverHeader))
        {
            LeaveCriticalSection(&g_VchanCriticalSection);
            return ERROR_INSUFFICIENT_BUFFER;
        }
        // inhibit zero-length write when not requested
        if (cbData && vchanFreeSpace == sizeof(serverHeader))
        {
            LeaveCriticalSection(&g_VchanCriticalSection);
            return ERROR_INSUFFICIENT_BUFFER;
        }

        if (vchanFreeSpace < sizeof(serverHeader) + cbData)
        {
            status = ERROR_INSUFFICIENT_BUFFER;
            cbData = vchanFreeSpace - sizeof(serverHeader);
        }

        *cbWritten = cbData;
    }

    serverHeader.type = messageType;
    serverHeader.client_id = clientId;
    serverHeader.len = cbData;
    if (VchanSendBuffer(&serverHeader, sizeof serverHeader) <= 0)
    {
        LogError("write_all_vchan_ext(s_hdr)");
        LeaveCriticalSection(&g_VchanCriticalSection);
        return ERROR_INVALID_FUNCTION;
    }

    if (!cbData)
    {
        LeaveCriticalSection(&g_VchanCriticalSection);
        return ERROR_SUCCESS;
    }

    if (VchanSendBuffer(data, cbData) <= 0)
    {
        LogError("write_all_vchan_ext(data, %d)", cbData);
        LeaveCriticalSection(&g_VchanCriticalSection);
        return ERROR_INVALID_FUNCTION;
    }

    LeaveCriticalSection(&g_VchanCriticalSection);

    LogVerbose("success");

    return status;
}

ULONG SendExitCode(IN ULONG clientId, IN int exitCode)
{
    ULONG status;

    LogVerbose("client %d, code %d", clientId, exitCode);

    status = SendMessageToDaemon(clientId, MSG_AGENT_TO_SERVER_EXIT_CODE, &exitCode, sizeof(exitCode), NULL);
    if (ERROR_SUCCESS != status)
    {
        return perror2(status, "ReturnData");
    }
    else
        LogDebug("Send exit code %d for client_id %d\n", exitCode, clientId);

    LogVerbose("success");
    return ERROR_SUCCESS;
}

static CLIENT_INFO *FindClientById(IN ULONG clientId)
{
    ULONG clientIndex;

    LogVerbose("client %d", clientId);
    for (clientIndex = 0; clientIndex < MAX_CLIENTS; clientIndex++)
        if (clientId == g_Clients[clientIndex].ClientId)
            return &g_Clients[clientIndex];

    LogVerbose("failed");
    return NULL;
}

static ULONG SendDataToDaemon(IN ULONG clientId, IN OUT PIPE_DATA *data)
{
    DWORD cbRead, cbSent;
    UINT messageType;
    CLIENT_INFO *clientInfo;
    ULONG status = ERROR_SUCCESS;

    LogVerbose("client %d", clientId);

    if (!data)
        return ERROR_INVALID_PARAMETER;

    clientInfo = FindClientById(clientId);
    if (!clientInfo)
        return ERROR_NOT_FOUND;

    if (clientInfo->ReadingDisabled)
        // The client does not want to receive any data from this console.
        return ERROR_INVALID_FUNCTION;

    data->ReadInProgress = FALSE;
    data->DataIsReady = FALSE;

    switch (data->PipeType)
    {
    case PTYPE_STDOUT:
        messageType = MSG_AGENT_TO_SERVER_STDOUT;
        break;
    case PTYPE_STDERR:
        messageType = MSG_AGENT_TO_SERVER_STDERR;
        break;
    default:
        return ERROR_INVALID_FUNCTION;
    }

    cbRead = 0;
    if (!GetOverlappedResult(data->ReadPipe, &data->ReadState, &cbRead, FALSE))
    {
        perror("GetOverlappedResult");
        LogError("client %d, dwRead %d", clientId, cbRead);
    }

    status = SendMessageToDaemon(clientId, messageType, data->ReadBuffer + data->cbSentBytes, cbRead - data->cbSentBytes, &cbSent);
    if (ERROR_INSUFFICIENT_BUFFER == status)
    {
        data->cbSentBytes += cbSent;
        data->VchanWritePending = TRUE;
        return status;
    }
    else if (ERROR_SUCCESS != status)
        perror2(status, "SendMessageToDaemon");

    data->VchanWritePending = FALSE;

    if (!cbRead)
    {
        data->PipeClosed = TRUE;
        status = ERROR_HANDLE_EOF;
    }

    LogVerbose("status %d", status);
    return status;
}

ULONG CloseReadPipeHandles(IN ULONG clientId, IN OUT PIPE_DATA *data)
{
    ULONG status;

    LogVerbose("client %d, pipedata %p", clientId, data);

    if (!data)
        return ERROR_INVALID_PARAMETER;

    status = ERROR_SUCCESS;

    if (data->ReadState.hEvent)
    {
        if (data->DataIsReady)
            SendDataToDaemon(clientId, data);

        // ReturnPipeData() clears both bDataIsReady and bReadInProgress, but they cannot be ever set to a non-FALSE value at the same time.
        // So, if the above ReturnPipeData() has been executed (bDataIsReady was not FALSE), then bReadInProgress was FALSE
        // and this branch wouldn't be executed anyways.
        if (data->ReadInProgress)
        {
            // If bReadInProgress is not FALSE then hReadPipe must be a valid handle for which an
            // asynchornous read has been issued.
            if (CancelIo(data->ReadPipe))
            {
                // Must wait for the canceled IO to complete, otherwise a race condition may occur on the
                // OVERLAPPED structure.
                WaitForSingleObject(data->ReadState.hEvent, INFINITE);

                // See if there is something to return.
                SendDataToDaemon(clientId, data);
            }
            else
            {
                perror("CancelIo");
            }
        }

        CloseHandle(data->ReadState.hEvent);
    }

    if (data->ReadPipe)
        // Can close the pipe only when there is no pending IO in progress.
        CloseHandle(data->ReadPipe);

    LogVerbose("status %d", status);
    return status;
}

static ULONG Utf8WithBomToUtf16(IN const char *stringUtf8, IN size_t cbStringUtf8, OUT WCHAR **stringUtf16)
{
    size_t cbSkipChars = 0;
    WCHAR *bufferUtf16 = NULL;
    ULONG status;
    HRESULT hresult;

    LogVerbose("utf8 '%S', size %d", stringUtf8, cbStringUtf8);

    if (!stringUtf8 || !cbStringUtf8 || !stringUtf16)
        return ERROR_INVALID_PARAMETER;

    *stringUtf16 = NULL;

    // see http://en.wikipedia.org/wiki/Byte-order_mark for explaination of the BOM encoding
    if (cbStringUtf8 >= 3 && stringUtf8[0] == 0xEF && stringUtf8[1] == 0xBB && stringUtf8[2] == 0xBF)
    {
        // UTF-8
        cbSkipChars = 3;
    }
    else if (cbStringUtf8 >= 2 && stringUtf8[0] == 0xFE && stringUtf8[1] == 0xFF)
    {
        // UTF-16BE
        return ERROR_NOT_SUPPORTED;
    }
    else if (cbStringUtf8 >= 2 && stringUtf8[0] == 0xFF && stringUtf8[1] == 0xFE)
    {
        // UTF-16LE
        cbSkipChars = 2;

        bufferUtf16 = malloc(cbStringUtf8 - cbSkipChars + sizeof(WCHAR));
        if (!bufferUtf16)
            return ERROR_NOT_ENOUGH_MEMORY;

        hresult = StringCbCopyW(bufferUtf16, cbStringUtf8 - cbSkipChars + sizeof(WCHAR), (STRSAFE_LPCWSTR) (stringUtf8 + cbSkipChars));
        if (FAILED(hresult))
        {
            perror2(hresult, "StringCbCopyW");
            free(bufferUtf16);
            return hresult;
        }

        *stringUtf16 = bufferUtf16;
        return ERROR_SUCCESS;
    }
    else if (cbStringUtf8 >= 4 && stringUtf8[0] == 0 && stringUtf8[1] == 0 && stringUtf8[2] == 0xFE && stringUtf8[3] == 0xFF)
    {
        // UTF-32BE
        return ERROR_NOT_SUPPORTED;
    }
    else if (cbStringUtf8 >= 4 && stringUtf8[0] == 0xFF && stringUtf8[1] == 0xFE && stringUtf8[2] == 0 && stringUtf8[3] == 0)
    {
        // UTF-32LE
        return ERROR_NOT_SUPPORTED;
    }

    // Try UTF-8

    status = ConvertUTF8ToUTF16(stringUtf8 + cbSkipChars, stringUtf16, NULL);
    if (ERROR_SUCCESS != status)
    {
        return perror2(status, "ConvertUTF8ToUTF16");
    }

    LogVerbose("success");

    return ERROR_SUCCESS;
}

static ULONG ParseUtf8Command(IN const char *commandUtf8, OUT WCHAR **commandUtf16, OUT WCHAR **userName, OUT WCHAR **commandLine, OUT BOOL *runInteractively)
{
    ULONG status;
    WCHAR *separator = NULL;

    LogVerbose("command '%S'", commandUtf8);

    if (!commandUtf8 || !runInteractively)
        return ERROR_INVALID_PARAMETER;

    *commandUtf16 = NULL;
    *userName = NULL;
    *commandLine = NULL;
    *runInteractively = TRUE;

    status = ConvertUTF8ToUTF16(commandUtf8, commandUtf16, NULL);
    if (ERROR_SUCCESS != status)
    {
        return perror2(status, "ConvertUTF8ToUTF16");
    }

    LogInfo("Command: %s", *commandUtf16);

    *userName = *commandUtf16;
    separator = wcschr(*commandUtf16, L':');
    if (!separator)
    {
        free(*commandUtf16);
        LogWarning("Command line is supposed to be in 'user:[nogui:]command' form\n");
        return ERROR_INVALID_PARAMETER;
    }

    *separator = L'\0';
    separator++;

    if (!wcsncmp(separator, L"nogui:", 6))
    {
        separator = wcschr(separator, L':');
        if (!separator)
        {
            free(*commandUtf16);
            LogWarning("Command line is supposed to be in user:[nogui:]command form\n");
            return ERROR_INVALID_PARAMETER;
        }

        *separator = L'\0';
        separator++;

        *runInteractively = FALSE;
    }

    if (!wcscmp(*userName, L"SYSTEM") || !wcscmp(*userName, L"root"))
    {
        *userName = NULL;
    }

    *commandLine = separator;

    LogVerbose("success");

    return ERROR_SUCCESS;
}

ULONG CreateClientPipes(IN OUT CLIENT_INFO *clientInfo, OUT HANDLE *pipeStdin, OUT HANDLE *pipeStdout, OUT HANDLE *pipeStderr)
{
    ULONG status;
    SECURITY_ATTRIBUTES sa = { 0 };

    LogVerbose("start");

    if (!clientInfo || !pipeStdin || !pipeStdout || !pipeStderr)
        return ERROR_INVALID_PARAMETER;

    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    status = InitReadPipe(&clientInfo->StdoutData, pipeStdout, PTYPE_STDOUT);
    if (ERROR_SUCCESS != status)
    {
        return perror2(status, "InitReadPipe(STDOUT)");
    }

    status = InitReadPipe(&clientInfo->StderrData, pipeStderr, PTYPE_STDERR);
    if (ERROR_SUCCESS != status)
    {
        CloseHandle(clientInfo->StdoutData.ReadPipe);
        CloseHandle(*pipeStdout);
        return perror2(status, "InitReadPipe(STDERR)");
    }

    if (!CreatePipe(pipeStdin, &clientInfo->WriteStdinPipe, &sa, 0))
    {
        status = GetLastError();

        CloseHandle(clientInfo->StdoutData.ReadPipe);
        CloseHandle(clientInfo->StderrData.ReadPipe);
        CloseHandle(*pipeStdout);
        CloseHandle(*pipeStderr);

        return perror2(status, "CreatePipe(STDIN)");
    }

    clientInfo->StdinPipeClosed = FALSE;

    // Ensure the write handle to the pipe for STDIN is not inherited.
    SetHandleInformation(clientInfo->WriteStdinPipe, HANDLE_FLAG_INHERIT, 0);

    LogVerbose("success");

    return ERROR_SUCCESS;
}

// This routine may be called by pipe server threads, hence the critical section around g_Clients array is required.
static ULONG ReserveClientIndex(IN ULONG clientId, OUT ULONG *clientIndex)
{
    LogVerbose("client %d", clientId);

    EnterCriticalSection(&g_ClientsCriticalSection);

    for (*clientIndex = 0; *clientIndex < MAX_CLIENTS; *clientIndex++)
        if (FREE_CLIENT_SPOT_ID == g_Clients[*clientIndex].ClientId)
            break;

    if (MAX_CLIENTS == *clientIndex)
    {
        // There is no space for watching for another process
        LeaveCriticalSection(&g_ClientsCriticalSection);
        LogWarning("The maximum number of running processes (%d) has been reached\n", MAX_CLIENTS);
        return ERROR_TOO_MANY_CMDS;
    }

    if (FindClientById(clientId))
    {
        LeaveCriticalSection(&g_ClientsCriticalSection);
        LogWarning("A client with the same id (%d) already exists\n", clientId);
        return ERROR_ALREADY_EXISTS;
    }

    g_Clients[*clientIndex].ClientIsReady = FALSE;
    g_Clients[*clientIndex].ClientId = clientId;

    LeaveCriticalSection(&g_ClientsCriticalSection);

    LogVerbose("success, index = %d", *clientIndex);
    return ERROR_SUCCESS;
}

static ULONG ReleaseClientIndex(IN ULONG clientIndex)
{
    LogVerbose("client index %d", clientIndex);

    if (clientIndex >= MAX_CLIENTS)
        return ERROR_INVALID_PARAMETER;

    EnterCriticalSection(&g_ClientsCriticalSection);

    g_Clients[clientIndex].ClientIsReady = FALSE;
    g_Clients[clientIndex].ClientId = FREE_CLIENT_SPOT_ID;

    LeaveCriticalSection(&g_ClientsCriticalSection);

    LogVerbose("success");

    return ERROR_SUCCESS;
}

static ULONG AddFilledClientInfo(IN ULONG clientIndex, IN const CLIENT_INFO *clientInfo)
{
    LogVerbose("client index %d", clientIndex);

    if (!clientInfo || clientIndex >= MAX_CLIENTS)
        return ERROR_INVALID_PARAMETER;

    EnterCriticalSection(&g_ClientsCriticalSection);

    g_Clients[clientIndex] = *clientInfo;
    g_Clients[clientIndex].ClientIsReady = TRUE;

    LeaveCriticalSection(&g_ClientsCriticalSection);

    LogVerbose("success");

    return ERROR_SUCCESS;
}

static ULONG StartClient(IN ULONG clientId, IN const WCHAR *userName, IN WCHAR *commandLine, IN BOOL runInteractively)
{
    ULONG status;
    CLIENT_INFO clientInfo;
    HANDLE pipeStdout = INVALID_HANDLE_VALUE;
    HANDLE pipeStderr = INVALID_HANDLE_VALUE;
    HANDLE pipeStdin = INVALID_HANDLE_VALUE;
    ULONG clientIndex;

    LogVerbose("client %d, user '%s', cmd '%s', interactive %d", clientId, userName, commandLine, runInteractively);

    // if userName is NULL we run the process on behalf of the current user.
    if (!commandLine)
        return ERROR_INVALID_PARAMETER;

    status = ReserveClientIndex(clientId, &clientIndex);
    if (ERROR_SUCCESS != status)
    {
        return perror2(status, "ReserveClientNumber");
    }

    if (userName)
        LogInfo("Running '%s' as user '%s'\n", commandLine, userName);
    else
    {
#ifdef BUILD_AS_SERVICE
        LogInfo("Running '%s' as SYSTEM\n", commandLine);
#else
        LogInfo("Running '%s' as current user\n", commandLine);
#endif
    }

    ZeroMemory(&clientInfo, sizeof(clientInfo));
    clientInfo.ClientId = clientId;

    status = CreateClientPipes(&clientInfo, &pipeStdin, &pipeStdout, &pipeStderr);
    if (ERROR_SUCCESS != status)
    {
        ReleaseClientIndex(clientIndex);
        return perror2(status, "CreateClientPipes");
    }

#ifdef BUILD_AS_SERVICE
    if (userName)
    {
        status = CreatePipedProcessAsUser(
            userName,
            DEFAULT_USER_PASSWORD_UNICODE,
            commandLine,
            runInteractively,
            pipeStdin,
            pipeStdout,
            pipeStderr,
            &clientInfo.ChildProcess);
    }
    else
    {
        status = CreatePipedProcessAsCurrentUser(
            commandLine,
            pipeStdin,
            pipeStdout,
            pipeStderr,
            &clientInfo.ChildProcess);
    }
#else
    status = CreatePipedProcessAsCurrentUser(
        commandLine,
        runInteractively,
        pipeStdin,
        pipeStdout,
        pipeStderr,
        &clientInfo.ChildProcess);
#endif

    CloseHandle(pipeStdout);
    CloseHandle(pipeStderr);
    CloseHandle(pipeStdin);

    if (ERROR_SUCCESS != status)
    {
        ReleaseClientIndex(clientIndex);

        CloseHandle(clientInfo.WriteStdinPipe);
        CloseHandle(clientInfo.StdoutData.ReadPipe);
        CloseHandle(clientInfo.StderrData.ReadPipe);

#ifdef BUILD_AS_SERVICE
        if (userName)
            return perror2(status, "CreatePipedProcessAsUser");
        else
            return perror2(status, "ACreatePipedProcessAsCurrentUser");
#else
        return perror2(status, "CreatePipedProcessAsCurrentUser");
#endif
    }

    status = AddFilledClientInfo(clientIndex, &clientInfo);
    if (ERROR_SUCCESS != status)
    {
        ReleaseClientIndex(clientIndex);

        CloseHandle(clientInfo.WriteStdinPipe);
        CloseHandle(clientInfo.StdoutData.ReadPipe);
        CloseHandle(clientInfo.StderrData.ReadPipe);
        CloseHandle(clientInfo.ChildProcess);

        return perror2(status, "AddFilledClientInfo");
    }

    LogDebug("New client %d (index %d)\n", clientId, clientIndex);

    return ERROR_SUCCESS;
}

ULONG AddExistingClient(IN ULONG clientId, IN CLIENT_INFO *clientInfo)
{
    ULONG clientIndex;
    ULONG status;

    LogVerbose("client %d", clientId);

    if (!clientInfo)
        return ERROR_INVALID_PARAMETER;

    status = ReserveClientIndex(clientId, &clientIndex);
    if (ERROR_SUCCESS != status)
    {
        return perror2(status, "ReserveClientNumber");
    }

    clientInfo->ClientId = clientId;

    status = AddFilledClientInfo(clientIndex, clientInfo);
    if (ERROR_SUCCESS != status)
    {
        ReleaseClientIndex(clientIndex);
        return perror2(status, "AddFilledClientInfo");
    }

    LogDebug("New client %d (local id %d)\n", clientId, clientIndex);

    SetEvent(g_AddExistingClientEvent);

    LogVerbose("success");

    return ERROR_SUCCESS;
}

static void RemoveClientNoLocks(IN OUT CLIENT_INFO *clientInfo OPTIONAL)
{
    if (clientInfo)
        LogVerbose("client %d", clientInfo->ClientId);
    else
        LogVerbose("clientInfo NULL");

    if (!clientInfo || (FREE_CLIENT_SPOT_ID == clientInfo->ClientId))
        return;

    CloseHandle(clientInfo->ChildProcess);

    if (!clientInfo->StdinPipeClosed)
        CloseHandle(clientInfo->WriteStdinPipe);

    CloseReadPipeHandles(clientInfo->ClientId, &clientInfo->StdoutData);
    CloseReadPipeHandles(clientInfo->ClientId, &clientInfo->StderrData);

    LogDebug("Client %d removed\n", clientInfo->ClientId);

    clientInfo->ClientId = FREE_CLIENT_SPOT_ID;
    clientInfo->ClientIsReady = FALSE;

    LogVerbose("success");
}

static void RemoveClient(IN OUT CLIENT_INFO *clientInfo OPTIONAL)
{
    if (clientInfo)
        LogVerbose("client %d", clientInfo->ClientId);
    else
        LogVerbose("clientInfo NULL");

    EnterCriticalSection(&g_ClientsCriticalSection);

    RemoveClientNoLocks(clientInfo);

    LeaveCriticalSection(&g_ClientsCriticalSection);

    LogVerbose("success");
}

static void RemoveAllClients(void)
{
    ULONG clientIndex;

    LogVerbose("start");

    EnterCriticalSection(&g_ClientsCriticalSection);

    for (clientIndex = 0; clientIndex < MAX_CLIENTS; clientIndex++)
        if (FREE_CLIENT_SPOT_ID != g_Clients[clientIndex].ClientId)
            RemoveClientNoLocks(&g_Clients[clientIndex]);

    LeaveCriticalSection(&g_ClientsCriticalSection);

    LogVerbose("success");
}

// must be called with g_ClientsCriticalSection
static ULONG HandleTerminatedClientNoLocks(IN OUT CLIENT_INFO *clientInfo)
{
    ULONG status;

    LogVerbose("client %d", clientInfo->ClientId);

    if (clientInfo->ChildExited && clientInfo->StdoutData.PipeClosed && clientInfo->StderrData.PipeClosed)
    {
        status = SendExitCode(clientInfo->ClientId, clientInfo->ExitCode);
        // guaranted that all data was already sent (above bPipeClosed==TRUE)
        // so no worry about returning some data after exit code
        RemoveClientNoLocks(clientInfo);
        return status;
    }

    LogVerbose("success");

    return ERROR_SUCCESS;
}

ULONG HandleTerminatedClient(IN OUT CLIENT_INFO *clientInfo)
{
    ULONG status;

    LogVerbose("client %d", clientInfo->ClientId);

    if (clientInfo->ChildExited && clientInfo->StdoutData.PipeClosed && clientInfo->StderrData.PipeClosed)
    {
        status = SendExitCode(clientInfo->ClientId, clientInfo->ExitCode);
        // guaranted that all data was already sent (above bPipeClosed==TRUE)
        // so no worry about returning some data after exit code
        RemoveClient(clientInfo);
        return status;
    }

    LogVerbose("success");

    return ERROR_SUCCESS;
}

// Recognize magic RPC request command ("QUBESRPC") and replace it with real
// command to be executed, after reading RPC service configuration.
// commandLine will be modified (and possibly reallocated)
// sourceDomainName will contain source domain (if available) to be set in
// environment; must be freed by caller
static ULONG InterceptRPCRequest(IN OUT WCHAR *commandLine, OUT WCHAR **serviceCommandLine, OUT WCHAR **sourceDomainName)
{
    WCHAR *serviceName = NULL;
    WCHAR *separator = NULL;
    char serviceConfigContents[sizeof(WCHAR) * (MAX_PATH + 1)];
    WCHAR serviceFilePath[MAX_PATH + 1];
    WCHAR *rawServiceFilePath = NULL;
    WCHAR *serviceArgs = NULL;
    HANDLE serviceConfigFile;
    ULONG status;
    ULONG cbRead;
    ULONG pathLength;

    LogVerbose("cmd '%s'", commandLine);

    if (!commandLine || !serviceCommandLine || !sourceDomainName)
        return ERROR_INVALID_PARAMETER;

    *serviceCommandLine = *sourceDomainName = NULL;

    if (wcsncmp(commandLine, RPC_REQUEST_COMMAND, wcslen(RPC_REQUEST_COMMAND)) == 0)
    {
        // RPC_REQUEST_COMMAND contains trailing space, so this must succeed
#pragma prefast(suppress:28193, "RPC_REQUEST_COMMAND contains trailing space, so this must succeed")
        separator = wcschr(commandLine, L' ');
        separator++;
        serviceName = separator;
        separator = wcschr(serviceName, L' ');
        if (separator)
        {
            *separator = L'\0';
            separator++;
            *sourceDomainName = _wcsdup(separator);
            if (*sourceDomainName == NULL)
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
        ZeroMemory(serviceFilePath, sizeof(serviceFilePath));
        if (!GetModuleFileNameW(NULL, serviceFilePath, MAX_PATH))
        {
            status = GetLastError();
            free(*sourceDomainName);
            return perror2(status, "GetModuleFileName");
        }
        // cut off file name (qrexec_agent.exe)
        separator = wcsrchr(serviceFilePath, L'\\');
        if (!separator)
        {
            free(*sourceDomainName);
            LogError("Cannot find dir containing qrexec_agent.exe\n");
            return ERROR_PATH_NOT_FOUND;
        }
        *separator = L'\0';
        // cut off one dir (bin)
        separator = wcsrchr(serviceFilePath, L'\\');
        if (!separator)
        {
            free(*sourceDomainName);
            LogError("Cannot find dir containing bin\\qrexec_agent.exe\n");
            return ERROR_PATH_NOT_FOUND;
        }
        // Leave trailing backslash
        separator++;
        *separator = L'\0';
        if (wcslen(serviceFilePath) + wcslen(L"qubes-rpc\\") + wcslen(serviceName) > MAX_PATH)
        {
            free(*sourceDomainName);
            LogError("RPC service config file path too long\n");
            return ERROR_PATH_NOT_FOUND;
        }

        PathAppendW(serviceFilePath, L"qubes-rpc");
        PathAppendW(serviceFilePath, serviceName);

        serviceConfigFile = CreateFileW(
            serviceFilePath,    // file to open
            GENERIC_READ,          // open for reading
            FILE_SHARE_READ,       // share for reading
            NULL,                  // default security
            OPEN_EXISTING,         // existing file only
            FILE_ATTRIBUTE_NORMAL, // normal file
            NULL);                 // no attr. template

        if (serviceConfigFile == INVALID_HANDLE_VALUE)
        {
            status = perror("CreateFile");
            free(*sourceDomainName);
            LogError("Failed to open RPC %s configuration file (%s)", serviceName, serviceFilePath);
            return status;
        }

        cbRead = 0;
        ZeroMemory(serviceConfigContents, sizeof(serviceConfigContents));

        if (!ReadFile(serviceConfigFile, serviceConfigContents, sizeof(WCHAR) * MAX_PATH, &cbRead, NULL))
        {
            status = perror("ReadFile");
            free(*sourceDomainName);
            LogError("Failed to read RPC %s configuration file (%s)", serviceName, serviceFilePath);
            CloseHandle(serviceConfigFile);
            return status;
        }
        CloseHandle(serviceConfigFile);

        status = Utf8WithBomToUtf16(serviceConfigContents, cbRead, &rawServiceFilePath);
        if (status != ERROR_SUCCESS)
        {
            perror2(status, "TextBOMToUTF16");
            free(*sourceDomainName);
            LogError("Failed to parse the encoding in RPC %s configuration file (%s)", serviceName, serviceFilePath);
            return status;
        }

        // strip white chars (especially end-of-line) from string
        pathLength = wcslen(rawServiceFilePath);
        while (iswspace(rawServiceFilePath[pathLength - 1]))
        {
            pathLength--;
            rawServiceFilePath[pathLength] = L'\0';
        }

        serviceArgs = PathGetArgsW(rawServiceFilePath);
        PathRemoveArgsW(rawServiceFilePath);
        PathUnquoteSpacesW(rawServiceFilePath);
        if (PathIsRelativeW(rawServiceFilePath))
        {
            // relative path are based in qubes-rpc-services
            // reuse separator found when preparing previous file path
            *separator = L'\0';
            PathAppendW(serviceFilePath, L"qubes-rpc-services");
            PathAppendW(serviceFilePath, rawServiceFilePath);
        }
        else
        {
            StringCchCopyW(serviceFilePath, MAX_PATH + 1, rawServiceFilePath);
        }

        PathQuoteSpacesW(serviceFilePath);
        if (serviceArgs && serviceArgs[0] != L'\0')
        {
            StringCchCatW(serviceFilePath, MAX_PATH + 1, L" ");
            StringCchCatW(serviceFilePath, MAX_PATH + 1, serviceArgs);
        }

        free(rawServiceFilePath);
        *serviceCommandLine = malloc((wcslen(serviceFilePath) + 1) * sizeof(WCHAR));
        if (*serviceCommandLine == NULL)
        {
            free(*sourceDomainName);
            return perror2(ERROR_NOT_ENOUGH_MEMORY, "malloc");
        }
        LogDebug("RPC %s: %s\n", serviceName, serviceFilePath);
        StringCchCopyW(*serviceCommandLine, wcslen(serviceFilePath) + 1, serviceFilePath);
    }

    LogVerbose("success");

    return ERROR_SUCCESS;
}

// This will return error only if vchan fails.
// RPC service connect, receives connection ident from vchan
ULONG HandleConnectExisting(IN ULONG clientId, IN int cbIdent)
{
    ULONG status;
    char *ident;

    LogVerbose("client %d, ident size %d", clientId, cbIdent);

    if (!cbIdent)
        return ERROR_SUCCESS;

    ident = malloc(cbIdent + 1);
    if (!ident)
        return ERROR_SUCCESS;
    ident[cbIdent] = 0;

    if (VchanReceiveBuffer(ident, cbIdent) <= 0)
    {
        free(ident);
        return perror2(ERROR_INVALID_FUNCTION, "VchanReceiveBuffer");
    }

    LogDebug("client %d, ident %S\n", clientId, ident);

    status = ProceedWithExecution(clientId, ident);
    free(ident);

    if (ERROR_SUCCESS != status)
        perror2(status, "ProceedWithExecution");

    LogVerbose("success");

    return ERROR_SUCCESS;
}

// This will return error only if vchan fails.
// handle execute command with piped IO
static ULONG HandleExec(IN ULONG clientId, IN int cbCommandUtf8)
{
    char *commandUtf8;
    ULONG status;
    WCHAR *command = NULL;
    WCHAR *userName = NULL;
    WCHAR *commandLine = NULL;
    WCHAR *serviceCommandLine = NULL;
    WCHAR *remoteDomainName = NULL;
    BOOL runInteractively;

    LogVerbose("client %d, cmd size %d", clientId, cbCommandUtf8);

    commandUtf8 = malloc(cbCommandUtf8 + 1);
    if (!commandUtf8)
        return ERROR_SUCCESS;
    commandUtf8[cbCommandUtf8] = 0;

    if (VchanReceiveBuffer(commandUtf8, cbCommandUtf8) <= 0)
    {
        free(commandUtf8);
        return perror2(ERROR_INVALID_FUNCTION, "VchanReceiveBuffer");
    }

    runInteractively = TRUE;

    status = ParseUtf8Command(commandUtf8, &command, &userName, &commandLine, &runInteractively);
    if (ERROR_SUCCESS != status)
    {
        free(commandUtf8);
        SendExitCode(clientId, MAKE_ERROR_RESPONSE(ERROR_SET_WINDOWS, status));
        perror2(status, "ParseUtf8Command");
        return ERROR_SUCCESS;
    }

    free(commandUtf8);
    commandUtf8 = NULL;

    status = InterceptRPCRequest(commandLine, &serviceCommandLine, &remoteDomainName);
    if (ERROR_SUCCESS != status)
    {
        free(command);
        SendExitCode(clientId, MAKE_ERROR_RESPONSE(ERROR_SET_WINDOWS, status));
        perror2(status, "InterceptRPCRequest");
        return ERROR_SUCCESS;
    }

    if (serviceCommandLine)
        commandLine = serviceCommandLine;

    if (remoteDomainName)
        SetEnvironmentVariableW(L"QREXEC_REMOTE_DOMAIN", remoteDomainName);

    // Create a process and redirect its console IO to vchan.
    status = StartClient(clientId, userName, commandLine, runInteractively);
    if (ERROR_SUCCESS == status)
        LogInfo("Executed: %s\n", commandLine);
    else
    {
        SendExitCode(clientId, MAKE_ERROR_RESPONSE(ERROR_SET_WINDOWS, status));
        LogWarning("AddClient('%s') failed: %d", commandLine, status);
    }

    if (remoteDomainName)
    {
        SetEnvironmentVariableW(L"QREXEC_REMOTE_DOMAIN", NULL);
        free(remoteDomainName);
    }
    if (serviceCommandLine)
        free(serviceCommandLine);

    free(command);

    LogVerbose("success");

    return ERROR_SUCCESS;
}

// This will return error only if vchan fails.
// handle execute command without piped IO
static ULONG HandleJustExec(IN ULONG clientId, IN int cbCommandUtf8)
{
    char *commandUtf8;
    ULONG status;
    WCHAR *command = NULL;
    WCHAR *userName = NULL;
    WCHAR *commandLine = NULL;
    WCHAR *serviceCommandLine = NULL;
    WCHAR *remoteDomainName = NULL;
    HANDLE process;
    BOOL runInteractively;

    LogVerbose("client %d, cmd size %d", clientId, cbCommandUtf8);

    commandUtf8 = malloc(cbCommandUtf8 + 1);
    if (!commandUtf8)
        return ERROR_SUCCESS;

    commandUtf8[cbCommandUtf8] = 0;

    if (VchanReceiveBuffer(commandUtf8, cbCommandUtf8) <= 0)
    {
        free(commandUtf8);
        return perror2(ERROR_INVALID_FUNCTION, "VchanReceiveBuffer");
    }

    runInteractively = TRUE;

    status = ParseUtf8Command(commandUtf8, &command, &userName, &commandLine, &runInteractively);
    if (ERROR_SUCCESS != status)
    {
        free(commandUtf8);
        SendExitCode(clientId, MAKE_ERROR_RESPONSE(ERROR_SET_WINDOWS, status));
        perror2(status, "ParseUtf8Command");
        return ERROR_SUCCESS;
    }

    free(commandUtf8);
    commandUtf8 = NULL;

    status = InterceptRPCRequest(commandLine, &serviceCommandLine, &remoteDomainName);
    if (ERROR_SUCCESS != status)
    {
        free(command);
        SendExitCode(clientId, MAKE_ERROR_RESPONSE(ERROR_SET_WINDOWS, status));
        perror2(status, "InterceptRPCRequest");
        return ERROR_SUCCESS;
    }

    if (serviceCommandLine)
        commandLine = serviceCommandLine;

    if (remoteDomainName)
        SetEnvironmentVariableW(L"QREXEC_REMOTE_DOMAIN", remoteDomainName);

    LogDebug("Command line: %s", commandLine);

#ifdef BUILD_AS_SERVICE
    // Create a process which IO is not redirected anywhere.
    status = CreateNormalProcessAsUser(
        userName,
        DEFAULT_USER_PASSWORD_UNICODE,
        commandLine,
        runInteractively,
        &process);
#else
    status = CreateNormalProcessAsCurrentUser(
        commandLine,
        &process);
#endif

    if (ERROR_SUCCESS == status)
    {
        CloseHandle(process);
        LogInfo("Executed: %s\n", commandLine);
    }
    else
    {
        SendExitCode(clientId, MAKE_ERROR_RESPONSE(ERROR_SET_WINDOWS, status));
#ifdef BUILD_AS_SERVICE
        perror2(status, "CreateNormalProcessAsUser");
#else
        perror2(status, "CreateNormalProcessAsCurrentUser");
#endif
    }

    if (remoteDomainName)
    {
        SetEnvironmentVariableW(L"QREXEC_REMOTE_DOMAIN", NULL);
        free(remoteDomainName);
    }
    if (serviceCommandLine)
        free(serviceCommandLine);

    free(command);

    LogVerbose("success");

    return ERROR_SUCCESS;
}

// This will return error only if vchan fails.
// receive input data from daemon and pipe it to client
static ULONG HandleInput(IN ULONG clientId, IN int cbInput)
{
    char *input;
    CLIENT_INFO *clientInfo;
    DWORD cbWritten;

    LogVerbose("client %d, input size %d", clientId, cbInput);
    // If clientInfo is NULL after this it means we couldn't find a specified client.
    // Read and discard any data in the channel in this case.
    clientInfo = FindClientById(clientId);

    if (cbInput == 0)
    {
        if (clientInfo)
        {
            CloseHandle(clientInfo->WriteStdinPipe);
            clientInfo->StdinPipeClosed = TRUE;
        }
        return ERROR_SUCCESS;
    }

    input = malloc(cbInput + 1);
    if (!input)
        return ERROR_NOT_ENOUGH_MEMORY;

    input[cbInput] = 0;

    if (VchanReceiveBuffer(input, cbInput) <= 0)
    {
        free(input);
        return perror2(ERROR_INVALID_FUNCTION, "VchanReceiveBuffer");
    }

    if (clientInfo && !clientInfo->StdinPipeClosed)
    {
        if (!WriteFile(clientInfo->WriteStdinPipe, input, cbInput, &cbWritten, NULL))
            perror("WriteFile");
    }

    free(input);

    LogVerbose("success");

    return ERROR_SUCCESS;
}

static void SetReadingDisabled(IN ULONG clientId, IN BOOLEAN blockOutput)
{
    CLIENT_INFO *clientInfo;

    LogVerbose("client %d, block %d", clientId, blockOutput);
    
    clientInfo = FindClientById(clientId);
    if (!clientInfo)
        return;

    clientInfo->ReadingDisabled = blockOutput;

    LogVerbose("success");
}

static ULONG HandleServerMessage(void)
{
    struct server_header header;
    ULONG status;

    LogVerbose("start");

    if (VchanReceiveBuffer(&header, sizeof header) <= 0)
    {
        return perror2(ERROR_INVALID_FUNCTION, "VchanReceiveBuffer");
    }

    switch (header.type)
    {
    case MSG_XON:
        LogDebug("MSG_XON\n");
        SetReadingDisabled(header.client_id, FALSE);
        break;
    case MSG_XOFF:
        LogDebug("MSG_XOFF\n");
        SetReadingDisabled(header.client_id, TRUE);
        break;
    case MSG_SERVER_TO_AGENT_CONNECT_EXISTING:
        LogDebug("MSG_SERVER_TO_AGENT_CONNECT_EXISTING\n");
        HandleConnectExisting(header.client_id, header.len);
        break;
    case MSG_SERVER_TO_AGENT_EXEC_CMDLINE:
        LogDebug("MSG_SERVER_TO_AGENT_EXEC_CMDLINE\n");

        // This will return error only if vchan fails.
        status = HandleExec(header.client_id, header.len);
        if (ERROR_SUCCESS != status)
        {
            return perror2(status, "HandleExec");
        }
        break;

    case MSG_SERVER_TO_AGENT_JUST_EXEC:
        LogDebug("MSG_SERVER_TO_AGENT_JUST_EXEC\n");

        // This will return error only if vchan fails.
        status = HandleJustExec(header.client_id, header.len);
        if (ERROR_SUCCESS != status)
        {
            return perror2(status, "HandleJustExec");
        }
        break;

    case MSG_SERVER_TO_AGENT_INPUT:
        LogDebug("MSG_SERVER_TO_AGENT_INPUT\n");

        // This will return error only if vchan fails.
        status = HandleInput(header.client_id, header.len);
        if (ERROR_SUCCESS != status)
        {
            return perror2(status, "HandleInput");
        }
        break;

    case MSG_SERVER_TO_AGENT_CLIENT_END:
        LogDebug("MSG_SERVER_TO_AGENT_CLIENT_END\n");
        RemoveClient(FindClientById(header.client_id));
        break;
    default:
        LogWarning("Unknown msg type from daemon: %d\n", header.type);
        return ERROR_INVALID_FUNCTION;
    }

    LogVerbose("success");

    return ERROR_SUCCESS;
}

// returns number of filled events (0 or 1)
ULONG FillAsyncIoData(IN ULONG eventIndex, IN ULONG clientIndex, IN HANDLE_TYPE handleType, IN OUT PIPE_DATA *pipeData)
{
    ULONG status;

    LogVerbose("event %d, client %d, handle type %d", eventIndex, clientIndex, handleType);

    if (eventIndex >= RTL_NUMBER_OF(g_WatchedEvents) ||
        clientIndex >= RTL_NUMBER_OF(g_Clients) ||
        !pipeData)
        return 0;

    if (!pipeData->ReadInProgress && !pipeData->DataIsReady && !pipeData->PipeClosed && !pipeData->VchanWritePending)
    {
        ZeroMemory(&pipeData->ReadBuffer, READ_BUFFER_SIZE);
        pipeData->cbSentBytes = 0;

        if (!ReadFile(
            pipeData->ReadPipe,
            &pipeData->ReadBuffer,
            READ_BUFFER_SIZE,
            NULL,
            &pipeData->ReadState))
        {
            // Last error is usually ERROR_IO_PENDING here because of the asynchronous read.
            // But if the process has closed it would be ERROR_BROKEN_PIPE,
            // in this case ReturnPipeData will send EOF notification and set bPipeClosed.
            status = GetLastError();
            if (ERROR_IO_PENDING == status)
                pipeData->ReadInProgress = TRUE;

            if (ERROR_BROKEN_PIPE == status)
            {
                SetEvent(pipeData->ReadState.hEvent);
                pipeData->DataIsReady = TRUE;
            }
        }
        else
        {
            // The read has completed synchronously.
            // The event in the OVERLAPPED structure should be signalled by now.
            pipeData->DataIsReady = TRUE;

            // Do not set bReadInProgress to TRUE in this case because if the pipes are to be closed
            // before the next read IO starts then there will be no IO to cancel.
            // bReadInProgress indicates to the CloseReadPipeHandles() that the IO should be canceled.

            // If after the WaitFormultipleObjects() this event is not chosen because of
            // some other event is also signaled, we will not rewrite the data in the buffer
            // on the next iteration of FillAsyncIoData() because bDataIsReady is set.
        }
    }

    // when bVchanWritePending==TRUE, ReturnPipeData already reset bReadInProgress and bDataIsReady
    if (pipeData->ReadInProgress || pipeData->DataIsReady)
    {
        g_HandlesInfo[eventIndex].ClientIndex = clientIndex;
        g_HandlesInfo[eventIndex].Type = handleType;
        g_WatchedEvents[eventIndex] = pipeData->ReadState.hEvent;
        return 1;
    }

    LogVerbose("success");

    return 0;
}

// main event loop
// fixme: this function is way too long
ULONG WatchForEvents(void)
{
    EVTCHN vchanHandle;
    OVERLAPPED olVchan;
    UINT firedPort;
    ULONG cbRead, eventIndex, clientIndex;
    DWORD signaledEvent;
    CLIENT_INFO *clientInfo;
    DWORD exitCode;
    BOOLEAN vchanIoInProgress;
    ULONG status;
    BOOLEAN vchanReturnedError;
    BOOLEAN vchanClientConnected;

    LogVerbose("start");

    // This will not block.
    if (!VchanInitServer(QREXEC_PORT))
    {
        return perror2(ERROR_INVALID_FUNCTION, "WatchForEvents(): peer_server_init()");
    }

    LogInfo("Awaiting for a vchan client, write ring size: %d\n", VchanGetWriteBufferSize());

    vchanHandle = libvchan_fd_for_select(g_Vchan);

    ZeroMemory(&olVchan, sizeof(olVchan));
    olVchan.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    vchanClientConnected = FALSE;
    vchanIoInProgress = FALSE;
    vchanReturnedError = FALSE;

    while (TRUE)
    {
        eventIndex = 0;

        // Order matters.
        g_WatchedEvents[eventIndex++] = g_StopServiceEvent;
        g_WatchedEvents[eventIndex++] = g_AddExistingClientEvent;

        g_HandlesInfo[0].Type = g_HandlesInfo[1].Type = HTYPE_INVALID;

        status = ERROR_SUCCESS;

        libvchan_prepare_to_select(g_Vchan);
        // read 1 byte instead of sizeof(fired_port) to not flush fired port
        // from evtchn buffer; evtchn driver will read only whole fired port
        // numbers (sizeof(fired_port)), so this will end in zero-length read
        if (!ReadFile(vchanHandle, &firedPort, 1, NULL, &olVchan))
        {
            status = GetLastError();
            if (ERROR_IO_PENDING != status)
            {
                perror("Vchan async read");
                vchanReturnedError = TRUE;
                break;
            }
        }

        vchanIoInProgress = TRUE;

        if (ERROR_SUCCESS == status || ERROR_IO_PENDING == status)
        {
            g_HandlesInfo[eventIndex].ClientIndex = FREE_CLIENT_SPOT_ID;
            g_HandlesInfo[eventIndex].Type = HTYPE_VCHAN;
            g_WatchedEvents[eventIndex++] = olVchan.hEvent;
        }

        EnterCriticalSection(&g_ClientsCriticalSection);

        for (clientIndex = 0; clientIndex < MAX_CLIENTS; clientIndex++)
        {
            if (g_Clients[clientIndex].ClientIsReady)
            {
                if (!g_Clients[clientIndex].ChildExited)
                {
                    g_HandlesInfo[eventIndex].ClientIndex = clientIndex;
                    g_HandlesInfo[eventIndex].Type = HTYPE_PROCESS;
                    g_WatchedEvents[eventIndex++] = g_Clients[clientIndex].ChildProcess;
                }

                if (!g_Clients[clientIndex].ReadingDisabled)
                {
                    // Skip those clients which have received MSG_XOFF.
                    eventIndex += FillAsyncIoData(eventIndex, clientIndex, HTYPE_STDOUT, &g_Clients[clientIndex].StdoutData);
                    eventIndex += FillAsyncIoData(eventIndex, clientIndex, HTYPE_STDERR, &g_Clients[clientIndex].StderrData);
                }
            }
        }
        LeaveCriticalSection(&g_ClientsCriticalSection);

        LogVerbose("waiting for event");

        signaledEvent = WaitForMultipleObjects(eventIndex, g_WatchedEvents, FALSE, INFINITE);
        if (signaledEvent >= MAXIMUM_WAIT_OBJECTS)
        {
            status = GetLastError();
            if (ERROR_INVALID_HANDLE != status)
            {
                perror2(status, "WaitForMultipleObjects");
                break;
            }

            // WaitForMultipleObjects() may fail with ERROR_INVALID_HANDLE if the process which just has been added
            // to the client list terminated before WaitForMultipleObjects(). In this case IO pipe handles are closed
            // and invalidated, while a process handle is in the signaled state.
            // Check if any of the processes in the client list is terminated, remove it from the list and try again.

            EnterCriticalSection(&g_ClientsCriticalSection);

            for (clientIndex = 0; clientIndex < MAX_CLIENTS; clientIndex++)
            {
                clientInfo = &g_Clients[clientIndex];

                if (!g_Clients[clientIndex].ClientIsReady)
                    continue;

                if (!GetExitCodeProcess(clientInfo->ChildProcess, &exitCode))
                {
                    perror("GetExitCodeProcess");
                    exitCode = ERROR_SUCCESS;
                }

                if (STILL_ACTIVE != exitCode)
                {
                    ULONG clientId;

                    clientId = clientInfo->ClientId;
                    clientInfo->ChildExited = TRUE;
                    clientInfo->ExitCode = exitCode;
                    // send exit code only when all data was sent to the daemon
                    status = HandleTerminatedClientNoLocks(clientInfo);
                    if (ERROR_SUCCESS != status)
                    {
                        vchanReturnedError = TRUE;
                        perror2(status, "HandleTerminatedClientNoLocks");
                    }
                }
            }
            LeaveCriticalSection(&g_ClientsCriticalSection);

            continue;
        }
        else
        {
            LogVerbose("event %d", signaledEvent);

            if (0 == signaledEvent)
            {
                LogVerbose("stopping");

                break;
            }

            if (HTYPE_VCHAN != g_HandlesInfo[signaledEvent].Type)
            {
                // If this is not a vchan event, cancel the event channel read so that libvchan_write() calls
                // could issue their own libvchan_wait on the same channel, and not interfere with the
                // ReadFile(vchanHandle, ...) above.
                if (CancelIo(vchanHandle))
                    // Must wait for the canceled IO to complete, otherwise a race condition may occur on the
                    // OVERLAPPED structure.
                    WaitForSingleObject(olVchan.hEvent, INFINITE);
                vchanIoInProgress = FALSE;
            }

            if (1 == signaledEvent)
                // g_AddExistingClientEvent is signaled. Since Vchan IO has been canceled,
                // safely re-iterate the loop and pick up the new handles to watch.
                continue;

            // Do not have to lock g_Clients here because other threads may only call
            // ReserveClientIndex()/ReleaseClientIndex()/AddFilledClientInfo()
            // which operate on different indices than those specified for WaitForMultipleObjects().

            // The other threads cannot call RemoveClient(), for example, they
            // operate only on newly allocated indices.

            // So here in this thread we may call FindByClientId() with no locks safely.

            // When this thread (in this switch) calls RemoveClient() later the g_Clients
            // list will be locked as usual.

            switch (g_HandlesInfo[signaledEvent].Type)
            {
            case HTYPE_VCHAN:

                LogVerbose("HTYPE_VCHAN");

                // the following will never block; we need to do this to
                // clear libvchan_fd pending state
                //
                // using libvchan_wait here instead of reading fired
                // port at the beginning of the loop (ReadFile call) to be
                // sure that we clear pending state _only_
                // when handling vchan data in this loop iteration (not any
                // other process)
                libvchan_wait(g_Vchan);

                vchanIoInProgress = FALSE;

                if (!vchanClientConnected)
                {
                    LogInfo("A vchan client has connected\n");

                    // Remove the xenstore device/vchan/N entry.
                    status = libvchan_server_handle_connected(g_Vchan);
                    if (status)
                    {
                        perror2(ERROR_INVALID_FUNCTION, "libvchan_server_handle_connected");
                        vchanReturnedError = TRUE;
                        break;
                    }

                    vchanClientConnected = TRUE;

                    // ignore error - perhaps core-admin too old and didn't
                    // create appropriate xenstore directory?
                    AdvertiseTools();

                    break;
                }

                if (!GetOverlappedResult(vchanHandle, &olVchan, &cbRead, FALSE))
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
                        vchanReturnedError = TRUE;
                        break;
                    }
                }

                EnterCriticalSection(&g_VchanCriticalSection);

                if (libvchan_is_eof(g_Vchan))
                {
                    vchanReturnedError = TRUE;
                    LeaveCriticalSection(&g_VchanCriticalSection);
                    break;
                }

                while (VchanGetReadBufferSize())
                {
                    status = HandleServerMessage();
                    if (ERROR_SUCCESS != status)
                    {
                        vchanReturnedError = TRUE;
                        perror2(status, "HandleServerMessage");
                        LeaveCriticalSection(&g_VchanCriticalSection);
                        break;
                    }
                }

                LeaveCriticalSection(&g_VchanCriticalSection);

                EnterCriticalSection(&g_ClientsCriticalSection);

                for (clientIndex = 0; clientIndex < MAX_CLIENTS; clientIndex++)
                {
                    if (g_Clients[clientIndex].ClientIsReady && !g_Clients[clientIndex].ReadingDisabled)
                    {
                        if (g_Clients[clientIndex].StdoutData.VchanWritePending)
                        {
                            status = SendDataToDaemon(g_Clients[clientIndex].ClientId, &g_Clients[clientIndex].StdoutData);
                            if (ERROR_HANDLE_EOF == status)
                            {
                                HandleTerminatedClientNoLocks(&g_Clients[clientIndex]);
                            }
                            else if (ERROR_INSUFFICIENT_BUFFER == status)
                            {
                                // no more space in vchan
                                break;
                            }
                            else if (ERROR_SUCCESS != status)
                            {
                                vchanReturnedError = TRUE;
                                perror2(status, "SendDataToDaemon(STDOUT)");
                            }
                        }
                        if (g_Clients[clientIndex].StderrData.VchanWritePending)
                        {
                            status = SendDataToDaemon(g_Clients[clientIndex].ClientId, &g_Clients[clientIndex].StderrData);
                            if (ERROR_HANDLE_EOF == status)
                            {
                                HandleTerminatedClientNoLocks(&g_Clients[clientIndex]);
                            }
                            else if (ERROR_INSUFFICIENT_BUFFER == status)
                            {
                                // no more space in vchan
                                break;
                            }
                            else if (ERROR_SUCCESS != status)
                            {
                                vchanReturnedError = TRUE;
                                perror2(status, "SendDataToDaemon(STDERR)");
                            }
                        }
                    }
                }

                LeaveCriticalSection(&g_ClientsCriticalSection);

                break;

            case HTYPE_STDOUT:
                LogVerbose("HTYPE_STDOUT");
#ifdef DISPLAY_CONSOLE_OUTPUT
                printf("%s", &g_Clients[g_HandlesInfo[signaledEvent].ClientIndex].StdoutData.ReadBuffer);
#endif

                status = SendDataToDaemon(
                    g_Clients[g_HandlesInfo[signaledEvent].ClientIndex].ClientId,
                    &g_Clients[g_HandlesInfo[signaledEvent].ClientIndex].StdoutData);

                if (ERROR_HANDLE_EOF == status)
                {
                    HandleTerminatedClient(&g_Clients[g_HandlesInfo[signaledEvent].ClientIndex]);
                }
                else if (ERROR_SUCCESS != status && ERROR_INSUFFICIENT_BUFFER != status)
                {
                    vchanReturnedError = TRUE;
                    perror2(status, "SendDataToDaemon(STDOUT)");
                }
                break;

            case HTYPE_STDERR:
                LogVerbose("HTYPE_STDERR");
#ifdef DISPLAY_CONSOLE_OUTPUT
                printf("%s", &g_Clients[g_HandlesInfo[signaledEvent].ClientIndex].StderrData.ReadBuffer);
#endif

                status = SendDataToDaemon(
                    g_Clients[g_HandlesInfo[signaledEvent].ClientIndex].ClientId,
                    &g_Clients[g_HandlesInfo[signaledEvent].ClientIndex].StderrData);

                if (ERROR_HANDLE_EOF == status)
                {
                    HandleTerminatedClient(&g_Clients[g_HandlesInfo[signaledEvent].ClientIndex]);
                }
                else if (ERROR_SUCCESS != status && ERROR_INSUFFICIENT_BUFFER != status)
                {
                    vchanReturnedError = TRUE;
                    perror2(status, "SendDataToDaemon(STDERR)");
                }
                break;

            case HTYPE_PROCESS:

                LogVerbose("HTYPE_PROCESS");
                clientInfo = &g_Clients[g_HandlesInfo[signaledEvent].ClientIndex];

                if (!GetExitCodeProcess(clientInfo->ChildProcess, &exitCode))
                {
                    perror("GetExitCodeProcess");
                    exitCode = ERROR_SUCCESS;
                }

                clientInfo->ChildExited = TRUE;
                clientInfo->ExitCode = exitCode;
                // send exit code only when all data was sent to the daemon
                status = HandleTerminatedClient(clientInfo);
                if (ERROR_SUCCESS != status)
                {
                    vchanReturnedError = TRUE;
                    perror2(status, "HandleTerminatedClient");
                }

                break;
            }
        }

        if (vchanReturnedError)
            break;
    }

    LogVerbose("loop finished");

    if (vchanIoInProgress)
    {
        if (CancelIo(vchanHandle))
        {
            // Must wait for the canceled IO to complete, otherwise a race condition may occur on the
            // OVERLAPPED structure.
            WaitForSingleObject(olVchan.hEvent, INFINITE);
        }
    }

    if (!vchanClientConnected)
    {
        // Remove the xenstore device/vchan/N entry.
        libvchan_server_handle_connected(g_Vchan);
    }

    // Cancel all other pending IO.
    RemoveAllClients();

    if (vchanClientConnected)
        libvchan_close(g_Vchan);

    // This is actually CloseHandle(evtchn)
    xc_evtchn_close(g_Vchan->evfd);

    CloseHandle(olVchan.hEvent);

    LogVerbose("exiting");

    return vchanReturnedError ? ERROR_INVALID_FUNCTION : ERROR_SUCCESS;
}

void Usage(void)
{
    LogError("qrexec agent service\n\nUsage: qrexec_agent <-i|-u>\n");
}

ULONG CheckForXenInterface(void)
{
    EVTCHN xc;

    LogVerbose("start");

    xc = xc_evtchn_open();
    if (INVALID_HANDLE_VALUE == xc)
        return ERROR_NOT_SUPPORTED;

    xc_evtchn_close(xc);

    LogVerbose("success");

    return ERROR_SUCCESS;
}

ULONG WINAPI ServiceExecutionThread(void *param)
{
    ULONG status;
    HANDLE triggerEventsThread;

    LogInfo("Service started\n");

    // Auto reset, initial state is not signaled
    g_AddExistingClientEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_AddExistingClientEvent)
    {
        return perror("CreateEvent");
    }

    triggerEventsThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE) WatchForTriggerEvents, NULL, 0, NULL);
    if (!triggerEventsThread)
    {
        status = GetLastError();
        CloseHandle(g_AddExistingClientEvent);
        return perror2(status, "CreateThread");
    }

    LogVerbose("entering loop");

    while (TRUE)
    {
        status = WatchForEvents();
        if (ERROR_SUCCESS != status)
            perror2(status, "WatchForEvents");

        if (!WaitForSingleObject(g_StopServiceEvent, 0))
            break;

        Sleep(1000);
    }

    LogDebug("Waiting for the trigger thread to exit\n");
    WaitForSingleObject(triggerEventsThread, INFINITE);
    CloseHandle(triggerEventsThread);
    CloseHandle(g_AddExistingClientEvent);

    DeleteCriticalSection(&g_ClientsCriticalSection);
    DeleteCriticalSection(&g_VchanCriticalSection);

    LogInfo("Shutting down\n");

    return ERROR_SUCCESS;
}

#ifdef BUILD_AS_SERVICE

ULONG Init(OUT HANDLE *serviceThread)
{
    ULONG status;
    ULONG clientIndex;

    LogVerbose("start");

    *serviceThread = INVALID_HANDLE_VALUE;

    status = CheckForXenInterface();
    if (ERROR_SUCCESS != status)
    {
        return perror2(status, "CheckForXenInterface");
    }

    InitializeCriticalSection(&g_ClientsCriticalSection);
    InitializeCriticalSection(&g_VchanCriticalSection);
    InitializeCriticalSection(&g_PipesCriticalSection);

    for (clientIndex = 0; clientIndex < MAX_CLIENTS; clientIndex++)
        g_Clients[clientIndex].ClientId = FREE_CLIENT_SPOT_ID;

    *serviceThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE) ServiceExecutionThread, NULL, 0, NULL);
    if (*serviceThread == NULL)
    {
        return perror("CreateThread");
    }

    LogVerbose("success");

    return ERROR_SUCCESS;
}

// This is the entry point for a service module (BUILD_AS_SERVICE defined).
int __cdecl wmain(int argc, WCHAR *argv[])
{
    ULONG option;
    WCHAR *param = NULL;
    WCHAR userName[UNLEN + 1] = { 0 };
    WCHAR fullPath[MAX_PATH + 1];
    DWORD cchUserName;
    ULONG status;
    BOOL stopParsing;
    WCHAR command;
    WCHAR *accountName = NULL;

    SERVICE_TABLE_ENTRY	serviceTable[] = {
            { SERVICE_NAME, (LPSERVICE_MAIN_FUNCTION) ServiceMain },
            { NULL, NULL }
    };

    LogVerbose("start");

    cchUserName = RTL_NUMBER_OF(userName);
    if (!GetUserName(userName, &cchUserName))
    {
        return perror("GetUserName");
    }

    if ((1 == argc) && wcscmp(userName, L"SYSTEM"))
    {
        Usage();
        return ERROR_INVALID_PARAMETER;
    }

    if (1 == argc)
    {
        LogInfo("Running as SYSTEM\n");

        status = ERROR_SUCCESS;
        if (!StartServiceCtrlDispatcher(serviceTable))
        {
            return perror("StartServiceCtrlDispatcher");
        }
    }

    ZeroMemory(fullPath, sizeof(fullPath));
    if (!GetModuleFileName(NULL, fullPath, RTL_NUMBER_OF(fullPath) - 1))
    {
        return perror("GetModuleFileName");
    }

    status = ERROR_SUCCESS;
    stopParsing = FALSE;
    command = 0;

    while (!stopParsing)
    {
        option = GetOption(argc, argv, TEXT("iua:"), &param);
        switch (option)
        {
        case 0:
            stopParsing = TRUE;
            break;

        case L'i':
        case L'u':
            if (command)
            {
                command = 0;
                stopParsing = TRUE;
            }
            else
                command = (TCHAR) option;

            break;

        case L'a':
            if (param)
                accountName = param;
            break;

        default:
            command = 0;
            stopParsing = TRUE;
        }
    }

    if (accountName)
    {
        LogDebug("GrantDesktopAccess('%s')\n", accountName);
        status = GrantDesktopAccess(accountName, NULL);
        if (ERROR_SUCCESS != status)
            perror2(status, "GrantDesktopAccess");

        return status;
    }

    switch (command)
    {
    case L'i':
        status = InstallService(fullPath, SERVICE_NAME);
        break;

    case L'u':
        status = UninstallService(SERVICE_NAME);
        break;
    default:
        Usage();
    }

    LogVerbose("status %d", status);

    return status;
}

#else

// Is not called when built without BUILD_AS_SERVICE definition.
ULONG Init(HANDLE *serviceThread)
{
    return ERROR_SUCCESS;
}

BOOL WINAPI CtrlHandler(IN DWORD ctrlType)
{
    LogInfo("Got shutdown signal\n");

    SetEvent(g_StopServiceEvent);

    WaitForSingleObject(g_CleanupFinishedEvent, 2000);

    CloseHandle(g_StopServiceEvent);
    CloseHandle(g_CleanupFinishedEvent);

    LogInfo("Shutdown complete\n");
    ExitProcess(0);
    return TRUE;
}

// This is the entry point for a console application (BUILD_AS_SERVICE not defined).
int __cdecl wmain(int argc, WCHAR *argv[])
{
    ULONG clientIndex;

    wprintf(L"\nqrexec agent console application\n\n");

    if (ERROR_SUCCESS != CheckForXenInterface())
    {
        LogError("Could not find Xen interface\n");
        return ERROR_NOT_SUPPORTED;
    }

    // Manual reset, initial state is not signaled
    g_StopServiceEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!g_StopServiceEvent)
        return perror("CreateEvent");

    // Manual reset, initial state is not signaled
    g_CleanupFinishedEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!g_CleanupFinishedEvent)
        return perror("CreateEvent");

    // InitializeCriticalSection always succeeds in Vista and later OSes.
    InitializeCriticalSection(&g_ClientsCriticalSection);
    InitializeCriticalSection(&g_VchanCriticalSection);
    InitializeCriticalSection(&g_PipesCriticalSection);

    SetConsoleCtrlHandler((PHANDLER_ROUTINE) CtrlHandler, TRUE);

    for (clientIndex = 0; clientIndex < MAX_CLIENTS; clientIndex++)
        g_Clients[clientIndex].ClientId = FREE_CLIENT_SPOT_ID;

    ServiceExecutionThread(NULL);
    SetEvent(g_CleanupFinishedEvent);

    LogVerbose("exiting");

    return ERROR_SUCCESS;
}
#endif
