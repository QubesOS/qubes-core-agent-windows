#include "qrexec-agent.h"
#include <Shlwapi.h>

#include <utf8-conv.h>
#include <service.h>

libvchan_t *g_DaemonVchan;
HANDLE g_AddExistingClientEvent;

CLIENT_INFO g_Clients[MAX_CLIENTS];
HANDLE g_WatchedEvents[MAXIMUM_WAIT_OBJECTS];
HANDLE_INFO	g_HandlesInfo[MAXIMUM_WAIT_OBJECTS];

ULONG64	g_PipeId = 0;

CRITICAL_SECTION g_ClientsCriticalSection;
CRITICAL_SECTION g_DaemonCriticalSection;

// from advertise_tools.c
ULONG AdvertiseTools(void);

// vchan for daemon communication
libvchan_t *VchanServerInit(IN int port)
{
    libvchan_t *vchan;
    // FIXME: "0" here is remote domain id
    vchan = libvchan_server_init(0, port, VCHAN_BUFFER_SIZE, VCHAN_BUFFER_SIZE);

    LogDebug("port %d: daemon vchan = %p", port, vchan);

    return vchan;
}

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
        1, // instances
        PIPE_BUFFER_SIZE,
        PIPE_BUFFER_SIZE,
        PIPE_DEFAULT_TIMEOUT,
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

static ULONG InitReadPipe(IN OUT PIPE_DATA *pipeData, OUT HANDLE *writePipe, IN PIPE_TYPE pipeType)
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

ULONG SendMessageToVchan(
    IN libvchan_t *vchan,
    IN UINT messageType,
    IN const void *data,
    IN ULONG cbData,
    OUT ULONG *cbWritten,
    IN const WCHAR *what
    )
{
    struct msg_header header;
    int vchanFreeSpace;
    ULONG status = ERROR_SUCCESS;

    LogDebug("vchan %p, msg type 0x%x, data %p, size %d (%s)", vchan, messageType, data, cbData, what);

    // FIXME: this function is not only called for daemon communication
    EnterCriticalSection(&g_DaemonCriticalSection);

    if (cbWritten)
    {
        // allow partial write only when cbWritten given
        *cbWritten = 0;
        vchanFreeSpace = VchanGetWriteBufferSize(vchan);
        if (vchanFreeSpace < sizeof(header))
        {
            LogWarning("vchan %p full (%d available)", vchan, vchanFreeSpace);
            LeaveCriticalSection(&g_DaemonCriticalSection);
            return ERROR_INSUFFICIENT_BUFFER;
        }

        // FIXME?
        // inhibit zero-length write when not requested
        if (cbData && (vchanFreeSpace == sizeof(header)))
        {
            LogDebug("vchan %p: inhibiting zero size write", vchan);
            LeaveCriticalSection(&g_DaemonCriticalSection);
            return ERROR_INSUFFICIENT_BUFFER;
        }

        if (vchanFreeSpace < sizeof(header) + cbData)
        {
            status = ERROR_INSUFFICIENT_BUFFER;
            cbData = vchanFreeSpace - sizeof(header);
            LogDebug("vchan %p: partial write (%d)", vchan, cbData);
        }

        *cbWritten = cbData;
    }

    header.type = messageType;
    header.len = cbData;

    if (!VchanSendBuffer(vchan, &header, sizeof(header), L"header"))
    {
        LogError("VchanSendBuffer(header for %s) failed", what);
        LeaveCriticalSection(&g_DaemonCriticalSection);
        return ERROR_INVALID_FUNCTION;
    }

    if (cbData == 0)
    {
        LeaveCriticalSection(&g_DaemonCriticalSection);
        return ERROR_SUCCESS;
    }

    if (!VchanSendBuffer(vchan, data, cbData, what))
    {
        LogError("VchanSendBuffer(%s, %d)", what, cbData);
        LeaveCriticalSection(&g_DaemonCriticalSection);
        return ERROR_INVALID_FUNCTION;
    }

    LeaveCriticalSection(&g_DaemonCriticalSection);

    LogVerbose("success");

    return status;
}

ULONG SendExitCode(IN const CLIENT_INFO *clientInfo, IN int exitCode)
{
    LogVerbose("client %p, code %d", clientInfo, exitCode);

    // don't send anything if we act as a qrexec-client (data server)
    if (clientInfo->IsVchanServer)
        return ERROR_SUCCESS;

    return SendExitCodeVchan(clientInfo->Vchan, exitCode);
}

// Send to data peer and close vchan.
// Should be used only if the CLIENT_INFO struct is not properly initialized
// (process creation failed etc).
ULONG SendExitCodeVchan(IN libvchan_t *vchan, IN int exitCode)
{
    LogVerbose("vchan %p, code %d", vchan, exitCode);
    ULONG status = SendMessageToVchan(vchan, MSG_DATA_EXIT_CODE, &exitCode, sizeof(exitCode), NULL, L"exit code");
    if (ERROR_SUCCESS != status)
    {
        return perror2(status, "SendMessageToVchan");
    }
    else
    {
        LogDebug("Sent exit code %d to vchan %p", exitCode, vchan);
    }

    LogVerbose("success");
    return ERROR_SUCCESS;
}

static CLIENT_INFO *FindClientByVchan(IN const libvchan_t *vchan)
{
    ULONG clientIndex;

    LogVerbose("vchan %p", vchan);
    for (clientIndex = 0; clientIndex < MAX_CLIENTS; clientIndex++)
        if (vchan == g_Clients[clientIndex].Vchan)
            return &g_Clients[clientIndex];

    LogVerbose("failed");
    return NULL;
}

// send output to vchan data peer
static ULONG SendDataToPeer(IN CLIENT_INFO *clientInfo, IN OUT PIPE_DATA *data)
{
    DWORD cbRead, cbSent;
    UINT messageType;
    libvchan_t *vchan = clientInfo->Vchan;
    ULONG status = ERROR_SUCCESS;

    LogVerbose("client %p, vchan %p", clientInfo, clientInfo->Vchan);

    if (!data)
        return ERROR_INVALID_PARAMETER;

    if (clientInfo->ReadingDisabled)
        // The client does not want to receive any data from this console.
        return ERROR_INVALID_FUNCTION;

    if (clientInfo->StdinPipeClosed)
    {
        LogDebug("trying to send after peer sent EOF, probably broken vchan connection");
        return ERROR_SUCCESS;
    }

    data->ReadInProgress = FALSE;
    data->DataIsReady = FALSE;

    switch (data->PipeType)
    {
    case PTYPE_STDOUT:
        messageType = MSG_DATA_STDOUT;
        break;
    case PTYPE_STDERR:
        messageType = MSG_DATA_STDERR;
        break;
    default:
        return ERROR_INVALID_FUNCTION;
    }

    cbRead = 0;
    if (!GetOverlappedResult(data->ReadPipe, &data->ReadState, &cbRead, FALSE))
    {
        // pipe closed
        perror("GetOverlappedResult");
        LogError("client %p, msg 0x%x, cbRead %d", clientInfo, messageType, cbRead);

        // send EOF
        SendMessageToVchan(vchan, messageType, NULL, 0, NULL, L"EOF");

        data->PipeClosed = TRUE;
        return ERROR_HANDLE_EOF;
    }

    // if we act as a vchan data server (qrexec-client) then only send MSG_DATA_STDIN
    if (clientInfo->IsVchanServer)
    {
        if (messageType == MSG_DATA_STDERR)
        {
            LogWarning("tried to send MSG_DATA_STDERR (size %d) while being a vchan server", cbRead);
            return ERROR_SUCCESS;
        }
        messageType = MSG_DATA_STDIN;
    }

    LogVerbose("read %d bytes from client pipe", cbRead);
    status = SendMessageToVchan(vchan, messageType, data->ReadBuffer + data->cbSentBytes, cbRead - data->cbSentBytes, &cbSent, L"output data");
    if (ERROR_INSUFFICIENT_BUFFER == status)
    {
        data->cbSentBytes += cbSent;
        data->VchanWritePending = TRUE;
        return status;
    }
    else if (ERROR_SUCCESS != status)
        perror2(status, "SendMessageToVchan");

    data->VchanWritePending = FALSE;

    LogVerbose("status 0x%x", status);
    return status;
}

ULONG CloseReadPipeHandles(IN CLIENT_INFO *clientInfo OPTIONAL, IN OUT PIPE_DATA *data)
{
    ULONG status;

    LogVerbose("client %p, vchan %p, pipe data %p", clientInfo, clientInfo ? clientInfo->Vchan : 0, data);

    if (!data)
        return ERROR_INVALID_PARAMETER;

    status = ERROR_SUCCESS;

    if (clientInfo && data->ReadState.hEvent)
    {
        if (data->DataIsReady)
            SendDataToPeer(clientInfo, data);

        // SendDataToPeer() clears both DataIsReady and ReadInProgress, but they cannot be ever set to a non-FALSE value at the same time.
        // So, if the above call has been executed (DataIsReady was not FALSE), then ReadInProgress was FALSE
        // and this branch wouldn't be executed anyways.
        if (data->ReadInProgress)
        {
            // If ReadInProgress is not FALSE then ReadPipe must be a valid handle for which an
            // asynchronous read has been issued.
            if (CancelIo(data->ReadPipe))
            {
                // Must wait for the canceled IO to complete, otherwise a race condition may occur on the
                // OVERLAPPED structure.
                WaitForSingleObject(data->ReadState.hEvent, INFINITE);

                // See if there is something to return.
                SendDataToPeer(clientInfo, data);
            }
            else
            {
                status = perror("CancelIo");
            }
        }

        CloseHandle(data->ReadState.hEvent);
    }

    if (data->ReadPipe)
        // Can close the pipe only when there is no pending IO in progress.
        CloseHandle(data->ReadPipe);

    LogVerbose("status 0x%x", status);
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

    // see http://en.wikipedia.org/wiki/Byte-order_mark for explanation of the BOM encoding
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

// caller must free commandUtf16 on success, userName and commandLine are pointers inside commandUtf16
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
static ULONG ReserveClientIndex(IN libvchan_t *vchan, OUT ULONG *clientIndex)
{
    LogVerbose("vchan %p", vchan);

    EnterCriticalSection(&g_ClientsCriticalSection);

    for (*clientIndex = 0; *clientIndex < MAX_CLIENTS; (*clientIndex)++)
        if (NULL == g_Clients[*clientIndex].Vchan)
            break;

    if (MAX_CLIENTS == *clientIndex)
    {
        // There is no space for another client
        LeaveCriticalSection(&g_ClientsCriticalSection);
        LogWarning("The maximum number of running processes (%d) has been reached", MAX_CLIENTS);
        return ERROR_BCD_TOO_MANY_ELEMENTS;
    }

    if (FindClientByVchan(vchan))
    {
        LeaveCriticalSection(&g_ClientsCriticalSection);
        LogWarning("A client with the same vchan (%p) already exists", vchan);
        return ERROR_ALREADY_EXISTS;
    }

    g_Clients[*clientIndex].ClientIsReady = FALSE;
    g_Clients[*clientIndex].Vchan = vchan;

    LeaveCriticalSection(&g_ClientsCriticalSection);

    LogDebug("success, index = %u for vchan %p", *clientIndex, vchan);
    return ERROR_SUCCESS;
}

static ULONG ReleaseClientIndex(IN ULONG clientIndex)
{
    LogVerbose("client index %u", clientIndex);

    if (clientIndex >= MAX_CLIENTS)
        return ERROR_INVALID_PARAMETER;

    EnterCriticalSection(&g_ClientsCriticalSection);

    g_Clients[clientIndex].ClientIsReady = FALSE;
    g_Clients[clientIndex].Vchan = NULL;

    LeaveCriticalSection(&g_ClientsCriticalSection);

    LogVerbose("success");

    return ERROR_SUCCESS;
}

static ULONG AddFilledClientInfo(IN ULONG clientIndex, IN const CLIENT_INFO *clientInfo)
{
    LogVerbose("client index %d, client %p, vchan %p", clientIndex, clientInfo, clientInfo->Vchan);

    if (!clientInfo || clientIndex >= MAX_CLIENTS)
        return ERROR_INVALID_PARAMETER;

    EnterCriticalSection(&g_ClientsCriticalSection);

    memcpy(&g_Clients[clientIndex], clientInfo, sizeof(*clientInfo));
    g_Clients[clientIndex].ClientIsReady = TRUE;

    LeaveCriticalSection(&g_ClientsCriticalSection);

    LogVerbose("success");

    return ERROR_SUCCESS;
}

// creates child process that's associated with data peer's vchan for i/o exchange
static ULONG StartClient(IN libvchan_t *vchan, IN const WCHAR *userName, IN WCHAR *commandLine, IN BOOL runInteractively)
{
    ULONG status;
    CLIENT_INFO *clientInfo = NULL;
    HANDLE pipeStdout = INVALID_HANDLE_VALUE;
    HANDLE pipeStderr = INVALID_HANDLE_VALUE;
    HANDLE pipeStdin = INVALID_HANDLE_VALUE;
    ULONG clientIndex;

    LogVerbose("vchan %p, user '%s', cmd '%s', interactive %d", vchan, userName, commandLine, runInteractively);

    // if userName is NULL we run the process on behalf of the current user.
    if (!commandLine)
        return ERROR_INVALID_PARAMETER;

    clientInfo = malloc(sizeof(*clientInfo));
    if (!clientInfo)
        return ERROR_OUTOFMEMORY;

    status = ReserveClientIndex(vchan, &clientIndex);
    if (ERROR_SUCCESS != status)
    {
        return perror2(status, "ReserveClientNumber");
    }

    if (userName)
        LogInfo("Running '%s' as user '%s'", commandLine, userName);
    else
    {
#ifdef BUILD_AS_SERVICE
        LogInfo("Running '%s' as SYSTEM", commandLine);
#else
        LogInfo("Running '%s' as current user", commandLine);
#endif
    }

    ZeroMemory(clientInfo, sizeof(*clientInfo));
    clientInfo->Vchan = vchan;

    status = CreateClientPipes(clientInfo, &pipeStdin, &pipeStdout, &pipeStderr);
    if (ERROR_SUCCESS != status)
    {
        ReleaseClientIndex(clientIndex);
        free(clientInfo);
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
            &clientInfo->ChildProcess);
    }
    else
    {
        status = CreatePipedProcessAsCurrentUser(
            commandLine,
            pipeStdin,
            pipeStdout,
            pipeStderr,
            &clientInfo->ChildProcess);
    }
#else
    status = CreatePipedProcessAsCurrentUser(
        commandLine,
        pipeStdin,
        pipeStdout,
        pipeStderr,
        &clientInfo->ChildProcess);
#endif

    CloseHandle(pipeStdout);
    CloseHandle(pipeStderr);
    CloseHandle(pipeStdin);

    if (ERROR_SUCCESS != status)
    {
        ReleaseClientIndex(clientIndex);

        CloseHandle(clientInfo->WriteStdinPipe);
        CloseHandle(clientInfo->StdoutData.ReadPipe);
        CloseHandle(clientInfo->StderrData.ReadPipe);

        free(clientInfo);
#ifdef BUILD_AS_SERVICE
        if (userName)
            return perror2(status, "CreatePipedProcessAsUser");
        else
            return perror2(status, "CreatePipedProcessAsCurrentUser");
#else
        return perror2(status, "CreatePipedProcessAsCurrentUser");
#endif
    }

    // we're the data client
    clientInfo->IsVchanServer = FALSE;
    status = AddFilledClientInfo(clientIndex, clientInfo);
    if (ERROR_SUCCESS != status)
    {
        ReleaseClientIndex(clientIndex);

        CloseHandle(clientInfo->WriteStdinPipe);
        CloseHandle(clientInfo->StdoutData.ReadPipe);
        CloseHandle(clientInfo->StderrData.ReadPipe);
        CloseHandle(clientInfo->ChildProcess);

        free(clientInfo);
        return perror2(status, "AddFilledClientInfo");
    }

    LogDebug("New client vchan %p (index %d)", vchan, clientIndex);

    free(clientInfo);
    return ERROR_SUCCESS;
}

// add process created by client-vm to watched clients
ULONG AddExistingClient(IN CLIENT_INFO *clientInfo)
{
    ULONG clientIndex;
    ULONG status;

    LogVerbose("client %p, vchan %p", clientInfo, clientInfo->Vchan);

    if (!clientInfo)
        return ERROR_INVALID_PARAMETER;

    status = ReserveClientIndex(clientInfo->Vchan, &clientIndex);
    if (ERROR_SUCCESS != status)
    {
        return perror2(status, "ReserveClientIndex");
    }

    status = AddFilledClientInfo(clientIndex, clientInfo);
    if (ERROR_SUCCESS != status)
    {
        ReleaseClientIndex(clientIndex);
        return perror2(status, "AddFilledClientInfo");
    }

    LogDebug("Added client %p (local idx %d, vchan %p)", clientInfo, clientIndex, clientInfo->Vchan);

    SetEvent(g_AddExistingClientEvent);

    LogVerbose("success");

    return ERROR_SUCCESS;
}

static void RemoveClientNoLocks(IN OUT CLIENT_INFO *clientInfo OPTIONAL)
{
    struct msg_header header = { 0 };

    if (clientInfo)
        LogVerbose("client vchan %p", clientInfo->Vchan);
    else
        LogVerbose("clientInfo NULL");

    if (!clientInfo || (NULL == clientInfo->Vchan))
        return;

    CloseHandle(clientInfo->ChildProcess);

    if (!clientInfo->StdinPipeClosed)
    {
        CloseHandle(clientInfo->WriteStdinPipe);
        clientInfo->StdinPipeClosed = TRUE;
    }

    CloseReadPipeHandles(clientInfo, &clientInfo->StdoutData);
    CloseReadPipeHandles(clientInfo, &clientInfo->StderrData);

    LogDebug("Client with vchan %p removed", clientInfo->Vchan);

    // if we're data server, send EOF if we can
    if (clientInfo->IsVchanServer && clientInfo->Vchan)
    {
        header.type = MSG_DATA_STDIN;
        header.len = 0;
        VchanSendBuffer(clientInfo->Vchan, &header, sizeof(header), L"EOF");
    }

    if (clientInfo->Vchan)
    {
        libvchan_close(clientInfo->Vchan);
        clientInfo->Vchan = NULL;
    }

    clientInfo->ClientIsReady = FALSE;

    LogVerbose("success");
}

static void RemoveClient(IN OUT CLIENT_INFO *clientInfo OPTIONAL)
{
    if (clientInfo)
        LogVerbose("client vchan %p", clientInfo->Vchan);
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
        if (NULL != g_Clients[clientIndex].Vchan)
            RemoveClientNoLocks(&g_Clients[clientIndex]);

    LeaveCriticalSection(&g_ClientsCriticalSection);

    LogVerbose("success");
}

// must be called with g_ClientsCriticalSection
static ULONG HandleTerminatedClientNoLocks(IN OUT CLIENT_INFO *clientInfo)
{
    ULONG status;

    LogVerbose("client %p, vchan %p", clientInfo, clientInfo->Vchan);

    if (clientInfo->ChildExited && clientInfo->StdoutData.PipeClosed && clientInfo->StderrData.PipeClosed)
    {
        status = SendExitCode(clientInfo, clientInfo->ExitCode);
        // guaranteed that all data was already sent (above PipeClosed==TRUE)
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

    LogVerbose("client vchan %p", clientInfo->Vchan);

    if (clientInfo->ChildExited && clientInfo->StdoutData.PipeClosed && clientInfo->StderrData.PipeClosed)
    {
        status = SendExitCode(clientInfo, clientInfo->ExitCode);
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
            LogDebug("source domain: '%s'", *sourceDomainName);
        }
        else
        {
            LogInfo("No source domain given");
            // Most qrexec services do not use source domain at all, so do not
            // abort if missing. This can be the case when RPC was triggered
            // manualy using qvm-run (qvm-run -p vmname "QUBESRPC service_name").
        }

        // build RPC service config file path
        // FIXME: use shell path APIs
        ZeroMemory(serviceFilePath, sizeof(serviceFilePath));
        if (!GetModuleFileName(NULL, serviceFilePath, MAX_PATH))
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
            LogError("Cannot find dir containing qrexec_agent.exe");
            return ERROR_PATH_NOT_FOUND;
        }
        *separator = L'\0';
        // cut off one dir (bin)
        separator = wcsrchr(serviceFilePath, L'\\');
        if (!separator)
        {
            free(*sourceDomainName);
            LogError("Cannot find dir containing bin\\qrexec_agent.exe");
            return ERROR_PATH_NOT_FOUND;
        }
        // Leave trailing backslash
        separator++;
        *separator = L'\0';
        if (wcslen(serviceFilePath) + wcslen(L"qubes-rpc\\") + wcslen(serviceName) > MAX_PATH)
        {
            free(*sourceDomainName);
            LogError("RPC service config file path too long");
            return ERROR_PATH_NOT_FOUND;
        }

        PathAppendW(serviceFilePath, L"qubes-rpc");
        PathAppendW(serviceFilePath, serviceName);

        serviceConfigFile = CreateFile(
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
            LogError("Failed to open service '%s' configuration file (%s)", serviceName, serviceFilePath);
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
            LogError("Failed to parse the encoding in RPC '%s' configuration file (%s)", serviceName, serviceFilePath);
            return status;
        }

        // strip white chars (especially end-of-line) from string
        pathLength = (ULONG) wcslen(rawServiceFilePath);
        while (iswspace(rawServiceFilePath[pathLength - 1]))
        {
            pathLength--;
            rawServiceFilePath[pathLength] = L'\0';
        }

        serviceArgs = PathGetArgs(rawServiceFilePath);
        PathRemoveArgs(rawServiceFilePath);
        PathUnquoteSpaces(rawServiceFilePath);
        if (PathIsRelative(rawServiceFilePath))
        {
            // relative path are based in qubes-rpc-services
            // reuse separator found when preparing previous file path
            *separator = L'\0';
            PathAppend(serviceFilePath, L"qubes-rpc-services");
            PathAppend(serviceFilePath, rawServiceFilePath);
        }
        else
        {
            StringCchCopy(serviceFilePath, MAX_PATH + 1, rawServiceFilePath);
        }

        PathQuoteSpaces(serviceFilePath);
        if (serviceArgs && serviceArgs[0] != L'\0')
        {
            StringCchCat(serviceFilePath, MAX_PATH + 1, L" ");
            StringCchCat(serviceFilePath, MAX_PATH + 1, serviceArgs);
        }

        free(rawServiceFilePath);
        *serviceCommandLine = malloc((wcslen(serviceFilePath) + 1) * sizeof(WCHAR));
        if (*serviceCommandLine == NULL)
        {
            free(*sourceDomainName);
            return perror2(ERROR_NOT_ENOUGH_MEMORY, "malloc");
        }
        LogDebug("RPC %s: %s\n", serviceName, serviceFilePath);
        StringCchCopy(*serviceCommandLine, wcslen(serviceFilePath) + 1, serviceFilePath);
    }

    LogVerbose("success");

    return ERROR_SUCCESS;
}

// Read exec_params from daemon after one of the EXEC messages has been received.
// Caller must free the returned value.
struct exec_params *ReceiveCmdline(IN int bufferSize)
{
    struct exec_params *params;

    if (bufferSize == 0)
        return NULL;

    params = (struct exec_params *) malloc(bufferSize);
    if (!params)
        return NULL;

    if (!VchanReceiveBuffer(g_DaemonVchan, params, bufferSize, L"exec_params"))
    {
        free(params);
        return NULL;
    }

    return params;
}

BOOL SendHelloToVchan(IN libvchan_t *vchan)
{
    struct peer_info info;

    info.version = QREXEC_PROTOCOL_VERSION;

    if (ERROR_SUCCESS != SendMessageToVchan(vchan, MSG_HELLO, &info, sizeof(info), NULL, L"hello"))
        return FALSE;
    return TRUE;
}

// This will return error only if vchan fails.
// RPC service connect, receives connection ident from vchan
ULONG HandleServiceConnect(IN const struct msg_header *header)
{
    ULONG status;
    struct exec_params *params = NULL;
    libvchan_t *peerVchan = NULL;

    LogDebug("msg 0x%x, len %d", header->type, header->len);

    params = ReceiveCmdline(header->len);
    if (!params)
    {
        LogError("recv_cmdline failed");
        return ERROR_INVALID_FUNCTION;
    }

    LogDebug("domain %u, port %u, cmdline '%S'", params->connect_domain, params->connect_port, params->cmdline);
    // this is a service connection, we act as a qrexec-client (data server)
    LogDebug("service, starting vchan server (%d, %d)",
             params->connect_domain, params->connect_port);

    peerVchan = libvchan_server_init(params->connect_domain, params->connect_port, VCHAN_BUFFER_SIZE, VCHAN_BUFFER_SIZE);
    if (!peerVchan)
    {
        LogError("libvchan_server_init(%d, %d) failed",
            params->connect_domain, params->connect_port);
        free(params);
        return ERROR_INVALID_FUNCTION;
    }

    LogDebug("server vchan: %p, waiting for remote peer (data client)", peerVchan);
    if (libvchan_wait(peerVchan) < 0)
    {
        LogError("libvchan_wait failed");
        libvchan_close(peerVchan);
        return ERROR_INVALID_FUNCTION;
    }

    LogDebug("remote peer (data client) connected");
    if (!SendHelloToVchan(peerVchan))
    {
        libvchan_close(peerVchan);
        return ERROR_INVALID_FUNCTION;
    }

    LogDebug("vchan %p, request id '%S'", peerVchan, params->cmdline);

    // peerVchan will be closed by DisconnectAndReconnect in pipe-server.c
    status = ProceedWithExecution(peerVchan, params->cmdline);
    free(params);

    if (ERROR_SUCCESS != status)
        perror("ProceedWithExecution");

    return ERROR_SUCCESS;
}

// This will return error only if vchan fails.
ULONG HandleServiceRefused(IN const struct msg_header *header)
{
    ULONG status;
    struct service_params serviceParams;

    LogDebug("msg 0x%x, len %d", header->type, header->len);

    if (!VchanReceiveBuffer(g_DaemonVchan, &serviceParams, header->len, L"service_params"))
    {
        return ERROR_INVALID_FUNCTION;
    }

    LogDebug("ident '%S'", serviceParams.ident);

    status = ProceedWithExecution(NULL, serviceParams.ident);

    if (ERROR_SUCCESS != status)
        perror("ProceedWithExecution");

    return ERROR_SUCCESS;
}

// Returns vchan for qrexec-client/peer that initiated the request.
// Fails only if daemon vchan fails.
// Returns TRUE and sets vchan to NULL if cmdline parsing failed or data vchan fails (caller should return with success status).
BOOL HandleExecCommon(IN int len, OUT WCHAR **userName, OUT WCHAR **commandLine, OUT BOOL *runInteractively, OUT libvchan_t **peerVchan)
{
    struct exec_params *exec = NULL;
    ULONG status;
    WCHAR *command = NULL;
    WCHAR *remoteDomainName = NULL;
    WCHAR *serviceCommandLine = NULL;

    // qrexec-client always listens on a vchan (details in exec_params)
    // it may be another agent (for vm/vm connection), but then it acts just like a qrexec-client
    *peerVchan = NULL;
    exec = ReceiveCmdline(len);
    if (!exec)
    {
        LogError("ReceiveCmdline failed");
        return FALSE;
    }

    *runInteractively = TRUE;

    LogDebug("cmdline: '%S'", exec->cmdline);
    LogDebug("connecting to vchan server (domain %d, port %d)...", exec->connect_domain, exec->connect_port);

    *peerVchan = libvchan_client_init(exec->connect_domain, exec->connect_port);
    if (*peerVchan)
    {
        LogDebug("connected to vchan server (%d, %d): 0x%x",
            exec->connect_domain, exec->connect_port, *peerVchan);
    }
    else
    {
        LogError("connection to vchan server (%d, %d) failed",
            exec->connect_domain, exec->connect_port);
        free(exec);
        return TRUE; // this is not fatal
    }

    // command is allocated in the call, userName and commandLine are pointers to inside command
    status = ParseUtf8Command(exec->cmdline, &command, userName, commandLine, runInteractively);
    if (ERROR_SUCCESS != status)
    {
        LogError("ParseUtf8Command failed");
        free(exec);
        SendExitCodeVchan(*peerVchan, MAKE_ERROR_RESPONSE(ERROR_SET_WINDOWS, status));
        libvchan_close(*peerVchan);
        *peerVchan = NULL;
        return TRUE;
    }

    free(exec);
    LogDebug("command: '%s', user: '%s', parsed: '%s'", command, *userName, *commandLine);

    // serviceCommandLine and remoteDomainName are allocated in the call
    status = InterceptRPCRequest(*commandLine, &serviceCommandLine, &remoteDomainName);
    if (ERROR_SUCCESS != status)
    {
        LogError("InterceptRPCRequest failed");
        SendExitCodeVchan(*peerVchan, MAKE_ERROR_RESPONSE(ERROR_SET_WINDOWS, status));
        libvchan_close(*peerVchan);
        *peerVchan = NULL;
        free(command);
        return TRUE;
    }

    if (remoteDomainName)
    {
        LogDebug("RPC domain: '%s'", remoteDomainName);
        SetEnvironmentVariableW(L"QREXEC_REMOTE_DOMAIN", remoteDomainName);
        free(remoteDomainName);
    }

    if (serviceCommandLine)
    {
        LogDebug("RPC command: '%s'", serviceCommandLine);
        *commandLine = serviceCommandLine;
    }
    else
    {
        // so caller can always free this
        *commandLine = _wcsdup(*commandLine); // FIXME: memory leak
    }

    *userName = _wcsdup(*userName);
    free(command);
    LogDebug("success: cmd '%s', user '%s'", *commandLine, *userName);

    return TRUE;
}

// This will return error only if vchan fails.
// handle execute command with piped IO
static ULONG HandleExec(IN const struct msg_header *header)
{
    ULONG status;
    WCHAR *userName = NULL;
    WCHAR *commandLine = NULL;
    BOOL runInteractively;
    libvchan_t *vchan = NULL;

    LogVerbose("msg 0x%x, len %d", header->type, header->len);

    if (!HandleExecCommon(header->len, &userName, &commandLine, &runInteractively, &vchan))
        return ERROR_INVALID_FUNCTION;

    if (!vchan) // cmdline parsing failed
        return ERROR_SUCCESS;

    // Create a process and redirect its console IO to vchan.
    status = StartClient(vchan, userName, commandLine, runInteractively);
    if (ERROR_SUCCESS == status)
    {
        LogInfo("Executed '%s'", commandLine);
    }
    else
    {
        SendExitCodeVchan(vchan, MAKE_ERROR_RESPONSE(ERROR_SET_WINDOWS, status));
        LogError("CreateChild(%s) failed", commandLine);
    }

    free(commandLine);
    free(userName);

    return ERROR_SUCCESS;
}

// This will return error only if vchan fails.
// handle execute command without piped IO
static ULONG HandleJustExec(IN const struct msg_header *header)
{
    ULONG status;
    WCHAR *userName = NULL;
    WCHAR *commandLine = NULL;
    HANDLE process;
    BOOL runInteractively;
    libvchan_t *vchan;

    LogDebug("msg 0x%x, len %d", header->type, header->len);

    if (!HandleExecCommon(header->len, &userName, &commandLine, &runInteractively, &vchan))
        return ERROR_INVALID_FUNCTION;

    if (!vchan) // cmdline parsing failed
        return ERROR_SUCCESS;

    LogDebug("executing '%s' as '%s'", commandLine, userName);

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
        LogInfo("Executed: '%s'", commandLine);
    }
    else
    {
#ifdef BUILD_AS_SERVICE
        perror2(status, "CreateNormalProcessAsUser");
#else
        perror2(status, "CreateNormalProcessAsCurrentUser");
#endif
    }

    // send status to qrexec-client (not real *exit* code, but we can at least return that process creation failed)
    SendExitCodeVchan(vchan, MAKE_ERROR_RESPONSE(ERROR_SET_WINDOWS, status));
    libvchan_close(vchan);

    free(commandLine);
    free(userName);

    LogVerbose("success");

    return ERROR_SUCCESS;
}

ULONG HandleDaemonHello(struct msg_header *header)
{
    struct peer_info info;

    if (header->len != sizeof(info))
    {
        LogError("header->len != sizeof(peer_info), protocol incompatible");
        return ERROR_INVALID_FUNCTION;
    }

    // read protocol version
    VchanReceiveBuffer(g_DaemonVchan, &info, sizeof(info), L"peer info");
    if (info.version != QREXEC_PROTOCOL_VERSION)
    {
        LogError("incompatible protocol version (%d instead of %d)",
            info.version, QREXEC_PROTOCOL_VERSION);
        return ERROR_INVALID_FUNCTION;
    }

    LogDebug("received protocol version %d", info.version);

    return ERROR_SUCCESS;
}

// entry for all qrexec-daemon messages
static ULONG HandleDaemonMessage(void)
{
    struct msg_header header;
    ULONG status;

    LogVerbose("start");

    if (!VchanReceiveBuffer(g_DaemonVchan, &header, sizeof header, L"daemon header"))
    {
        return perror2(ERROR_INVALID_FUNCTION, "VchanReceiveBuffer");
    }

    switch (header.type)
    {
    case MSG_HELLO:
        return HandleDaemonHello(&header);

    case MSG_SERVICE_CONNECT:
        return HandleServiceConnect(&header);

    case MSG_SERVICE_REFUSED:
        return HandleServiceRefused(&header);

    case MSG_EXEC_CMDLINE:
        // This will return error only if vchan fails.
        status = HandleExec(&header);
        if (ERROR_SUCCESS != status)
            return perror2(status, "HandleExec");
        break;

    case MSG_JUST_EXEC:
        // This will return error only if vchan fails.
        status = HandleJustExec(&header);
        if (ERROR_SUCCESS != status)
            return perror2(status, "HandleJustExec");
        break;

    default:
        LogWarning("unknown message type: 0x%x", header.type);
        return ERROR_INVALID_FUNCTION; // FIXME: should this be error?
    }

    LogVerbose("success");

    return ERROR_SUCCESS;
}

ULONG HandleStdin(IN const struct msg_header *header, IN CLIENT_INFO *clientInfo);
ULONG HandleStdout(IN const struct msg_header *header, IN CLIENT_INFO *clientInfo);
ULONG HandleStderr(IN const struct msg_header *header, IN CLIENT_INFO *clientInfo);
BOOL HandleClientExitCode(IN CLIENT_INFO *clientInfo);

// entry for all qrexec-client messages (or peer agent if we're the vchan server)
ULONG HandleDataMessage(IN libvchan_t *vchan)
{
    struct msg_header header;
    ULONG status;
    struct peer_info peerInfo;
    CLIENT_INFO *clientInfo;

    clientInfo = FindClientByVchan(vchan);
    LogDebug("vchan %p, IsVchanServer=%d", vchan, clientInfo->IsVchanServer);
    if (!VchanReceiveBuffer(vchan, &header, sizeof(header), L"client header"))
    {
        return ERROR_INVALID_FUNCTION;
    }

    /*
    * qrexec-client is the vchan server
    * sends: MSG_HELLO, MSG_DATA_STDIN
    * expects: MSG_HELLO, MSG_DATA_STDOUT, MSG_DATA_STDERR, MSG_DATA_EXIT_CODE
    *
    * if CLIENT_INFO.IsVchanServer is set, we act as a qrexec-client (vchan server)
    * (vm/vm connection to another agent that is the usual vchan client)
    */

    switch (header.type)
    {
    case MSG_HELLO:
        LogVerbose("MSG_HELLO");
        if (!VchanReceiveBuffer(vchan, &peerInfo, sizeof(peerInfo), L"peer info"))
            return ERROR_INVALID_FUNCTION;
        LogDebug("protocol version %d", peerInfo.version);

        if (peerInfo.version != QREXEC_PROTOCOL_VERSION)
        {
            LogWarning("incompatible protocol version (got %d, expected %d)", peerInfo.version, QREXEC_PROTOCOL_VERSION);
            return ERROR_INVALID_FUNCTION;
        }

        if (!clientInfo->IsVchanServer) // we're vchan client, reply with HELLO
        {
            if (!SendHelloToVchan(vchan))
                return ERROR_INVALID_FUNCTION;

        }
        break;

    case MSG_DATA_STDIN:
        LogVerbose("MSG_DATA_STDIN");
        // This will return error only if vchan fails.
        status = HandleStdin(&header, clientInfo);
        if (ERROR_SUCCESS != status)
            return perror2(status, "HandleStdin");
        break;

    case MSG_DATA_STDOUT:
        LogVerbose("MSG_DATA_STDOUT");
        // This will return error only if vchan fails.
        status = HandleStdout(&header, clientInfo);
        if (ERROR_SUCCESS != status)
            return perror2(status, "HandleStdout");
        break;

    case MSG_DATA_STDERR:
        LogVerbose("MSG_DATA_STDERR");
        // This will return error only if vchan fails.
        status = HandleStderr(&header, clientInfo);
        if (ERROR_SUCCESS != status)
            return perror2(status, "HandleStderr");
        break;

    case MSG_DATA_EXIT_CODE:
        LogVerbose("MSG_DATA_EXIT_CODE");
        if (!clientInfo->IsVchanServer)
        {
            LogWarning("got MSG_DATA_EXIT_CODE while being a vchan client");
            return ERROR_INVALID_FUNCTION;
        }
        if (!HandleClientExitCode(clientInfo))
            return ERROR_INVALID_FUNCTION;
        break;

    default:
        LogWarning("unknown message type: 0x%x", header.type);
        return ERROR_INVALID_FUNCTION;
    }

    return ERROR_SUCCESS;
}

// Read input from vchan (data server), send to peer.
// This will return error only if vchan fails.
ULONG HandleStdin(IN const struct msg_header *header, IN CLIENT_INFO *clientInfo)
{
    void *buffer;
    DWORD cbWritten;

    LogVerbose("vchan %p: msg 0x%x, len %d, vchan data ready %d",
        clientInfo->Vchan, header->type, header->len, VchanGetReadBufferSize(clientInfo->Vchan));

    if (!header->len)
    {
        LogDebug("EOF from vchan %p", clientInfo->Vchan);
        if (clientInfo)
        {
            CloseHandle(clientInfo->WriteStdinPipe);
            clientInfo->StdinPipeClosed = TRUE;
            RemoveClient(clientInfo);
        }
        return ERROR_SUCCESS;
    }

    buffer = malloc(header->len);
    if (!buffer)
        return ERROR_NOT_ENOUGH_MEMORY;

    if (!VchanReceiveBuffer(clientInfo->Vchan, buffer, header->len, L"stdin data"))
    {
        free(buffer);
        return ERROR_INVALID_FUNCTION;
    }

    // send to peer
    LogDebug("writing stdin data to client %p (vchan %p)", clientInfo, clientInfo->Vchan);
    if (clientInfo)
    {
        if (!clientInfo->StdinPipeClosed)
    {
        if (!WriteFile(clientInfo->WriteStdinPipe, buffer, header->len, &cbWritten, NULL))
                perror("WriteFile(stdin data)");
        }
        else
            LogDebug("stdin closed for client %p (vchan %p)", clientInfo, clientInfo->Vchan);
    }

    free(buffer);
    return ERROR_SUCCESS;
}

ULONG HandleStdout(IN const struct msg_header *header, IN CLIENT_INFO *clientInfo)
{
    void *buffer;
    DWORD cbWritten;

    LogVerbose("vchan %p: msg 0x%x, len %d, vchan data ready %d",
        clientInfo->Vchan, header->type, header->len, VchanGetReadBufferSize(clientInfo->Vchan));

    // we expect this only if we're a vchan server (vm/vm connection)
    if (!header->len)
    {
        LogDebug("EOF from vchan %p", clientInfo->Vchan);
        return ERROR_SUCCESS;
    }

    buffer = malloc(header->len);
    if (!buffer)
        return ERROR_NOT_ENOUGH_MEMORY;

    if (!VchanReceiveBuffer(clientInfo->Vchan, buffer, header->len, L"stdout data"))
    {
        free(buffer);
        return ERROR_INVALID_FUNCTION;
    }

    // send to peer
    // this is a vm/vm connection and we're the server so the roles are reversed from the usual
    if (clientInfo && !clientInfo->StdinPipeClosed)
    {
        if (!WriteFile(clientInfo->WriteStdinPipe, buffer, header->len, &cbWritten, NULL))
            perror("WriteFile");
    }

    free(buffer);
    return ERROR_SUCCESS;
}

ULONG HandleStderr(IN const struct msg_header *header, IN CLIENT_INFO *clientInfo)
{
    void *buffer;

    LogVerbose("vchan %p: msg 0x%x, len %d, vchan data ready %d",
        clientInfo->Vchan, header->type, header->len, VchanGetReadBufferSize(clientInfo->Vchan));

    // we expect this only if we're a vchan server (vm/vm connection)
    if (!header->len)
    {
        LogDebug("EOF from vchan %p", clientInfo->Vchan);
        return ERROR_SUCCESS;
    }

    buffer = malloc(header->len);
    if (!buffer)
        return ERROR_NOT_ENOUGH_MEMORY;

    if (!VchanReceiveBuffer(clientInfo->Vchan, buffer, header->len, L"stderr data"))
    {
        free(buffer);
        return ERROR_INVALID_FUNCTION;
    }

    // write to log file
    LogInfo("STDERR from client vchan %p (size %d):", clientInfo->Vchan, header->len);
    // FIXME: is this unicode or ascii or what?
    LogInfo("%s", buffer);

    free(buffer);
    return ERROR_SUCCESS;
}

BOOL HandleClientExitCode(IN CLIENT_INFO *clientInfo)
{
    int code;

    if (!VchanReceiveBuffer(clientInfo->Vchan, &code, sizeof(code), L"vchan client exit code"))
        return FALSE;

    EnterCriticalSection(&g_ClientsCriticalSection);
    LogDebug("vchan %p: code 0x%x, client %p", clientInfo->Vchan, code, clientInfo);
    // peer is closing vchan on their side so we shouldn't attempt to use it
    LogVerbose("closing vchan %p", clientInfo->Vchan);
    libvchan_close(clientInfo->Vchan);
    clientInfo->Vchan = NULL;
    RemoveClientNoLocks(clientInfo);
    LeaveCriticalSection(&g_ClientsCriticalSection);

    return TRUE;
}

// reads child io, returns number of filled events (0 or 1)
ULONG FillAsyncIoData(IN ULONG eventIndex, IN ULONG clientIndex, IN HANDLE_TYPE handleType, IN OUT PIPE_DATA *pipeData)
{
    ULONG status;

    LogVerbose("event %d, client %d, handle type %d", eventIndex, clientIndex, handleType);

    if (eventIndex >= RTL_NUMBER_OF(g_WatchedEvents) ||
        clientIndex >= RTL_NUMBER_OF(g_Clients) ||
        !pipeData)
    {
        LogWarning("eventIndex=%d, clientIndex=%d (out of range)", eventIndex, clientIndex);
        return 0;
    }

    if (!pipeData->ReadInProgress && !pipeData->DataIsReady && !pipeData->PipeClosed && !pipeData->VchanWritePending)
    {
        ZeroMemory(&pipeData->ReadBuffer, MAX_DATA_CHUNK);
        pipeData->cbSentBytes = 0;

        if (!ReadFile(
            pipeData->ReadPipe,
            &pipeData->ReadBuffer,
            MAX_DATA_CHUNK,
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
            // The event in the OVERLAPPED structure should be signaled by now.
            pipeData->DataIsReady = TRUE;

            // Do not set ReadInProgress to TRUE in this case because if the pipes are to be closed
            // before the next read IO starts then there will be no IO to cancel.
            // ReadInProgress indicates to the CloseReadPipeHandles() that the IO should be canceled.

            // If after the WaitFormultipleObjects() this event is not chosen because of
            // some other event is also signaled, we will not rewrite the data in the buffer
            // on the next iteration of FillAsyncIoData() because DataIsReady is set.
        }
    }

    // when vchanWritePending==TRUE, SendDataToPeer already reset ReadInProgress and DataIsReady
    if (pipeData->ReadInProgress || pipeData->DataIsReady)
    {
        LogVerbose("adding event %d: IO for client %p", eventIndex, &g_Clients[clientIndex]);
        g_HandlesInfo[eventIndex].ClientIndex = clientIndex;
        g_HandlesInfo[eventIndex].Type = handleType;
        g_WatchedEvents[eventIndex] = pipeData->ReadState.hEvent;
        return 1;
    }

    LogVerbose("success");

    return 0;
}

// main event loop for children, daemon and clients
// FIXME: this function is way too long
ULONG WatchForEvents(HANDLE stopEvent)
{
    ULONG eventIndex, clientIndex;
    DWORD signaledEvent;
    CLIENT_INFO *clientInfo;
    DWORD exitCode;
    ULONG status;
    BOOL vchanIoInProgress = FALSE;
    BOOL vchanReturnedError = FALSE;
    BOOL vchanClientConnected = FALSE;
    BOOL daemonConnected = FALSE;
    HANDLE vchanEvent;

    LogVerbose("start");

    g_DaemonVchan = VchanServerInit(VCHAN_BASE_PORT);

    if (!g_DaemonVchan)
    {
        return perror2(ERROR_INVALID_FUNCTION, "VchanInitServer");
    }

    LogInfo("Waiting for qrexec daemon connection, write buffer size: %d", VchanGetWriteBufferSize(g_DaemonVchan));

    while (TRUE)
    {
        LogVerbose("loop start");

        g_WatchedEvents[0] = stopEvent;
        g_WatchedEvents[1] = g_AddExistingClientEvent;

        g_HandlesInfo[0].Type = g_HandlesInfo[1].Type = HTYPE_INVALID;

        status = ERROR_SUCCESS;

        vchanEvent = libvchan_fd_for_select(g_DaemonVchan);
        if (INVALID_HANDLE_VALUE == vchanEvent)
        {
            status = perror("libvchan_fd_for_select");
            break;
        }

        g_HandlesInfo[2].ClientIndex = -1;
        g_HandlesInfo[2].Type = HTYPE_CONTROL_VCHAN;
        g_WatchedEvents[2] = vchanEvent;

        EnterCriticalSection(&g_ClientsCriticalSection);

        eventIndex = 3;
        // prepare child events
        for (clientIndex = 0; clientIndex < MAX_CLIENTS; clientIndex++)
        {
            clientInfo = &g_Clients[clientIndex];
            if (clientInfo->ClientIsReady)
            {
                // process exit event
                if (!clientInfo->ChildExited)
                {
                    LogVerbose("adding event %d: process exit for client %p", eventIndex, clientInfo);
                    g_HandlesInfo[eventIndex].ClientIndex = clientIndex;
                    g_HandlesInfo[eventIndex].Type = HTYPE_PROCESS;
                    g_WatchedEvents[eventIndex++] = clientInfo->ChildProcess;
                }

                // event for associated data vchan peer
                if (clientInfo->Vchan && !clientInfo->StdinPipeClosed)
                {
                    LogVerbose("adding event %d: vchan %p for client %p", eventIndex, clientInfo->Vchan, clientInfo);
                    g_HandlesInfo[eventIndex].Type = HTYPE_DATA_VCHAN;
                    g_HandlesInfo[eventIndex].ClientIndex = clientIndex;
                    g_WatchedEvents[eventIndex++] = libvchan_fd_for_select(clientInfo->Vchan);
                }

                // Skip those clients which have received MSG_XOFF.
                if (!clientInfo->ReadingDisabled)
                {
                    // process output from child
                    eventIndex += FillAsyncIoData(eventIndex, clientIndex, HTYPE_STDOUT, &clientInfo->StdoutData);
                    eventIndex += FillAsyncIoData(eventIndex, clientIndex, HTYPE_STDERR, &clientInfo->StderrData);
                }
            }
        }
        LeaveCriticalSection(&g_ClientsCriticalSection);

        LogVerbose("waiting for event (%d events registered)", eventIndex);

        signaledEvent = WaitForMultipleObjects(eventIndex, g_WatchedEvents, FALSE, INFINITE);


#ifdef _DEBUG
            for (clientIndex = 0; clientIndex < RTL_NUMBER_OF(g_HandlesInfo); clientIndex++)
            {
            if (g_WatchedEvents[clientIndex])
                LogVerbose("Event %02d: 0x%x, type %d, child idx %d", clientIndex,
                    g_WatchedEvents[clientIndex], g_HandlesInfo[clientIndex].Type, g_HandlesInfo[clientIndex].ClientIndex);
            }
            for (clientIndex = 0; clientIndex < RTL_NUMBER_OF(g_Clients); clientIndex++)
            {
            LogVerbose("Client %d (%p): vchan %p, stdin closed=%d", clientIndex, &g_Clients[clientIndex],
                    g_Clients[clientIndex].Vchan, g_Clients[clientIndex].StdinPipeClosed);
            }
#endif

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

            LogDebug("removing terminated clients");
            EnterCriticalSection(&g_ClientsCriticalSection);

            for (clientIndex = 0; clientIndex < MAX_CLIENTS; clientIndex++)
            {
                clientInfo = &g_Clients[clientIndex];

                if (!clientInfo->ClientIsReady)
                    continue;

                if (!GetExitCodeProcess(clientInfo->ChildProcess, &exitCode))
                {
                    perror("GetExitCodeProcess");
                    exitCode = ERROR_SUCCESS;
                }

                if (STILL_ACTIVE != exitCode)
                {
                    clientInfo->ChildExited = TRUE;
                    clientInfo->ExitCode = exitCode;
                    // send exit code only when all data was sent to the daemon
                    status = HandleTerminatedClientNoLocks(clientInfo);
                    if (ERROR_SUCCESS != status)
                    {
                        vchanReturnedError = TRUE;
                        perror("HandleTerminatedClientNoLocks");
                    }
                }
            }
            LeaveCriticalSection(&g_ClientsCriticalSection);

            continue;
        }

        LogVerbose("event %d", signaledEvent);

        if (0 == signaledEvent) // stop event
        {
            LogDebug("stopping");
            break;
        }

        if (1 == signaledEvent)
            // g_AddExistingClientEvent is signaled. Since vchan IO has been canceled,
            // safely re-iterate the loop and pick up new handles to watch.
            continue;

        // Do not have to lock g_Clients here because other threads may only call
        // ReserveClientIndex()/ReleaseClientIndex()/AddFilledClientInfo()
        // which operate on different indices than those specified for WaitForMultipleObjects().

        // The other threads cannot call RemoveClient(), for example, they
        // operate only on newly allocated indices.

        // So here in this thread we may call FindClient...() with no locks safely.

        // When this thread (in this switch) calls RemoveClient() later the g_Clients
        // list will be locked as usual.

        LogDebug("child %d, type %d, signaled index %d",
            g_HandlesInfo[signaledEvent].ClientIndex, g_HandlesInfo[signaledEvent].Type, signaledEvent);

        switch (g_HandlesInfo[signaledEvent].Type)
        {
        case HTYPE_CONTROL_VCHAN:
        {
            LogVerbose("HTYPE_CONTROL_VCHAN");

            vchanIoInProgress = FALSE;

            EnterCriticalSection(&g_DaemonCriticalSection);
            if (!daemonConnected)
            {
                LogInfo("qrexec-daemon has connected (event %d)", signaledEvent);

                if (!SendHelloToVchan(g_DaemonVchan))
                {
                    LogError("failed to send hello to daemon");
                    vchanReturnedError = TRUE;
                    LeaveCriticalSection(&g_DaemonCriticalSection);
                    break;
                }

                daemonConnected = TRUE;
                LeaveCriticalSection(&g_DaemonCriticalSection);

                // ignore errors - perhaps core-admin too old and didn't
                // create appropriate xenstore directory?
                AdvertiseTools();

                break;
            }

            if (!libvchan_is_open(g_DaemonVchan))
            {
                vchanReturnedError = TRUE;
                LeaveCriticalSection(&g_DaemonCriticalSection);
                break;
            }

            if (VchanGetReadBufferSize(g_DaemonVchan) > 0)
            {
                // handle data from daemon
                while (VchanGetReadBufferSize(g_DaemonVchan) > 0)
                {
                    status = HandleDaemonMessage();
                    if (ERROR_SUCCESS != status)
                    {
                        vchanReturnedError = TRUE;
                        perror2(status, "handle_daemon_message");
                        LeaveCriticalSection(&g_DaemonCriticalSection);
                        break;
                    }
                }
            }
            else
            {
                LogDebug("HTYPE_CONTROL_VCHAN event: no data");
                LeaveCriticalSection(&g_DaemonCriticalSection);
                break;
            }

            LeaveCriticalSection(&g_DaemonCriticalSection);
            break;
        }

        case HTYPE_DATA_VCHAN:
        {
            libvchan_t *dataVchan;

            EnterCriticalSection(&g_ClientsCriticalSection);
            clientInfo = &g_Clients[g_HandlesInfo[signaledEvent].ClientIndex];
            dataVchan = clientInfo->Vchan;

            LogVerbose("HTYPE_DATA_VCHAN client %p, vchan %p", clientInfo, dataVchan);
            if (dataVchan == NULL)
            {
                LogWarning("HTYPE_DATA_VCHAN: vchan for client %d (%p) is NULL", g_HandlesInfo[signaledEvent].ClientIndex, clientInfo);
                LeaveCriticalSection(&g_ClientsCriticalSection);
                break;
            }

            if (!libvchan_is_open(dataVchan))
            {
                LogWarning("HTYPE_DATA_VCHAN: vchan %p is closed, removing client %p", dataVchan, clientInfo);
                RemoveClientNoLocks(clientInfo);
                LeaveCriticalSection(&g_ClientsCriticalSection);
                break;
            }

            if (clientInfo->StdinPipeClosed)
            {
                LogWarning("HTYPE_DATA_VCHAN: stdin for vchan %p, client %p is closed", dataVchan, clientInfo);
                LeaveCriticalSection(&g_ClientsCriticalSection);
                break;
            }

            while (dataVchan && VchanGetReadBufferSize(dataVchan) > 0)
            {
                // handle data from vchan peer

                status = HandleDataMessage(dataVchan);
                if (ERROR_SUCCESS != status)
                {
                    vchanReturnedError = TRUE;
                    perror2(status, "HandleDataMessage");
                    LeaveCriticalSection(&g_ClientsCriticalSection);
                    break;
                }
                // recheck vchan in case the client was disconnected
                dataVchan = clientInfo->Vchan;
            }

            // if there is pending output from children, pass it to data vchan

            for (clientIndex = 0; clientIndex < MAX_CLIENTS; clientIndex++)
            {
                clientInfo = &g_Clients[clientIndex];
                if (clientInfo->ClientIsReady && !clientInfo->ReadingDisabled)
                {
                    if (clientInfo->StdoutData.VchanWritePending)
                    {
                        status = SendDataToPeer(clientInfo, &clientInfo->StdoutData);
                        if (ERROR_HANDLE_EOF == status)
                        {
                            HandleTerminatedClientNoLocks(clientInfo);
                        }
                        else if (ERROR_INSUFFICIENT_BUFFER == status)
                        {
                            // no more space in vchan
                            break;
                        }
                        else if (ERROR_SUCCESS != status)
                        {
                            //vchanReturnedError = TRUE; // not a critical error
                            perror2(status, "SendDataToPeer(STDOUT)");
                        }
                    }
                    if (clientInfo->StderrData.VchanWritePending)
                    {
                        status = SendDataToPeer(clientInfo, &clientInfo->StderrData);
                        if (ERROR_HANDLE_EOF == status)
                        {
                            HandleTerminatedClientNoLocks(clientInfo);
                        }
                        else if (ERROR_INSUFFICIENT_BUFFER == status)
                        {
                            // no more space in vchan
                            break;
                        }
                        else if (ERROR_SUCCESS != status)
                        {
                            //vchanReturnedError = TRUE; // not a critical error
                            perror2(status, "SendDataToPeer(STDERR)");
                        }
                    }
                }
            }

            LeaveCriticalSection(&g_ClientsCriticalSection);

            break;
        }

        case HTYPE_STDOUT:
        {
            clientInfo = &g_Clients[g_HandlesInfo[signaledEvent].ClientIndex];
            LogVerbose("HTYPE_STDOUT: client %d (%p)", g_HandlesInfo[signaledEvent].ClientIndex, clientInfo);

            // pass to vchan
            status = SendDataToPeer(clientInfo, &clientInfo->StdoutData);
            if (ERROR_HANDLE_EOF == status)
            {
                HandleTerminatedClient(clientInfo);
            }
            else if (ERROR_SUCCESS != status && ERROR_INSUFFICIENT_BUFFER != status)
            {
                //vchanReturnedError = TRUE; // not a critical error
                perror2(status, "SendDataToPeer(STDOUT)");
            }
            break;
        }

        case HTYPE_STDERR:
        {
            clientInfo = &g_Clients[g_HandlesInfo[signaledEvent].ClientIndex];
            LogVerbose("HTYPE_STDERR: client %d (%p)", g_HandlesInfo[signaledEvent].ClientIndex, clientInfo);

            // pass to vchan
            status = SendDataToPeer(clientInfo, &clientInfo->StderrData);
            if (ERROR_HANDLE_EOF == status)
            {
                HandleTerminatedClient(clientInfo);
            }
            else if (ERROR_SUCCESS != status && ERROR_INSUFFICIENT_BUFFER != status)
            {
                //vchanReturnedError = TRUE; // not a critical error
                perror2(status, "SendDataToPeer(STDERR)");
            }
            break;
        }

        case HTYPE_PROCESS:
        {
            // child process exited
            clientInfo = &g_Clients[g_HandlesInfo[signaledEvent].ClientIndex];
            LogVerbose("HTYPE_PROCESS: client %d (%p)", g_HandlesInfo[signaledEvent].ClientIndex, clientInfo);

            if (!GetExitCodeProcess(clientInfo->ChildProcess, &exitCode))
            {
                perror("GetExitCodeProcess");
                exitCode = ERROR_SUCCESS;
            }

            clientInfo->ChildExited = TRUE;
            clientInfo->ExitCode = exitCode;
            // send exit code only when all data was sent to the data peer
            status = HandleTerminatedClient(clientInfo);
            if (ERROR_SUCCESS != status)
            {
                vchanReturnedError = TRUE;
                perror2(status, "HandleTerminatedClient");
            }

            break;
        }

        default:
        {
            LogWarning("invalid handle type %d for event %d",
                g_HandlesInfo[signaledEvent].Type, signaledEvent);
            break;
        }
        }

        if (vchanReturnedError)
        {
            LogError("vchan error");
            break;
        }
    }

    LogVerbose("loop finished");
    /*
    if (vchanIoInProgress)
    {
    if (CancelIo(vchanHandle))
    {
    // Must wait for the canceled IO to complete, otherwise a race condition may occur on the
    // OVERLAPPED structure.
    WaitForSingleObject(olVchan.hEvent, INFINITE);
    }
    }
    */

    RemoveAllClients();

    if (daemonConnected)
        libvchan_close(g_DaemonVchan);

    return vchanReturnedError ? ERROR_INVALID_FUNCTION : ERROR_SUCCESS;
}

void Usage(void)
{
    LogError("qrexec agent service\n\nUsage: qrexec_agent <-i|-u>\n");
}

static void XifLogger(int level, const char *function, const WCHAR *format, va_list args)
{
    WCHAR buf[1024];

    StringCbVPrintfW(buf, sizeof(buf), format, args);
    // our log levels start at 0, xif's at 1
    _LogFormat(level - 1, FALSE, function, buf);
}

ULONG WINAPI ServiceExecutionThread(void *param)
{
    ULONG status;
    HANDLE triggerEventsThread;
    PSERVICE_WORKER_CONTEXT ctx = param;

    LogInfo("Service started");

    libvchan_register_logger(XifLogger);

    // Auto reset, initial state is not signaled
    g_AddExistingClientEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_AddExistingClientEvent)
    {
        return perror("CreateEvent");
    }

    triggerEventsThread = CreateThread(NULL, 0, WatchForTriggerEvents, ctx->StopEvent, 0, NULL);
    if (!triggerEventsThread)
    {
        status = GetLastError();
        CloseHandle(g_AddExistingClientEvent);
        return perror2(status, "CreateThread");
    }

    LogVerbose("entering loop");

    status = WatchForEvents(ctx->StopEvent);
    if (ERROR_SUCCESS != status)
        perror2(status, "WatchForEvents");

    // this will stop the trigger thread in case WatchForEvents terminates with error
    SetEvent(ctx->StopEvent);

    LogDebug("Waiting for the trigger thread to exit");
    WaitForSingleObject(triggerEventsThread, INFINITE);
    CloseHandle(triggerEventsThread);
    CloseHandle(g_AddExistingClientEvent);

    DeleteCriticalSection(&g_ClientsCriticalSection);
    DeleteCriticalSection(&g_DaemonCriticalSection);

    LogInfo("Shutting down");

    return ERROR_SUCCESS;
}

#ifdef BUILD_AS_SERVICE

// This is the entry point for a service module (BUILD_AS_SERVICE defined).
int __cdecl wmain(int argc, WCHAR *argv[])
{
    WCHAR option;
    WCHAR userName[UNLEN + 1] = { 0 };
    WCHAR fullPath[MAX_PATH + 1];
    DWORD cchUserName;
    ULONG status;
    BOOL stopParsing;
    WCHAR command;
    WCHAR *accountName = NULL;
    DWORD clientIndex;

    LogVerbose("start");

    InitializeCriticalSection(&g_ClientsCriticalSection);
    InitializeCriticalSection(&g_DaemonCriticalSection);
    InitializeCriticalSection(&g_PipesCriticalSection);

    for (clientIndex = 0; clientIndex < MAX_CLIENTS; clientIndex++)
        g_Clients[clientIndex].Vchan = NULL;

    cchUserName = RTL_NUMBER_OF(userName);
    if (!GetUserName(userName, &cchUserName))
        return perror("GetUserName");

    if ((1 == argc) && wcscmp(userName, L"SYSTEM"))
    {
        Usage();
        return ERROR_INVALID_PARAMETER;
    }

    if (1 == argc)
    {
        LogInfo("Running as SYSTEM");

        status = SvcMainLoop(
            SERVICE_NAME,
            0,
            ServiceExecutionThread,
            NULL,
            NULL,
            NULL);

        return status;
    }

    // handle (un)install command line
    ZeroMemory(fullPath, sizeof(fullPath));
    if (!GetModuleFileName(NULL, fullPath, RTL_NUMBER_OF(fullPath) - 1))
        return perror("GetModuleFileName");

    status = ERROR_SUCCESS;
    stopParsing = FALSE;
    command = 0;

    while (!stopParsing)
    {
        option = getopt(argc, argv, L"iua:");
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
                command = option;

            break;

        case L'a':
            if (optarg)
                accountName = optarg;
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
        status = SvcCreate(SERVICE_NAME, NULL, fullPath);
        break;

    case L'u':
        status = SvcDelete(SERVICE_NAME);
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

// This is the entry point for a console application (BUILD_AS_SERVICE not defined).
int __cdecl wmain(int argc, WCHAR *argv[])
{
    ULONG clientIndex;
    SERVICE_WORKER_CONTEXT ctx;

    wprintf(L"qrexec agent console application\n\n");

    // Manual reset, initial state is not signaled
    ctx.StopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!ctx.StopEvent)
        return perror("Create stop event");

    InitializeCriticalSection(&g_ClientsCriticalSection);
    InitializeCriticalSection(&g_DaemonCriticalSection);
    InitializeCriticalSection(&g_PipesCriticalSection);

    for (clientIndex = 0; clientIndex < MAX_CLIENTS; clientIndex++)
        g_Clients[clientIndex].Vchan = NULL;

    ServiceExecutionThread(&ctx);

    LogVerbose("exiting");

    return ERROR_SUCCESS;
}
#endif
