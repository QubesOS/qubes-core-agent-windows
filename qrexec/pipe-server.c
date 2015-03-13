#include "pipe-server.h"

extern HANDLE g_StopServiceEvent;
extern libvchan_t *g_DaemonVchan;

CRITICAL_SECTION g_PipesCriticalSection;
PIPE_INSTANCE g_Pipes[TRIGGER_PIPE_INSTANCES] = { 0 };
HANDLE g_Events[TRIGGER_PIPE_INSTANCES + 1];

ULONG64	g_DaemonRequestsCounter = 1;

static ULONG CreatePipeSecurityDescriptor(OUT SECURITY_DESCRIPTOR **pipeSecurityDescriptor, OUT ACL **pipeAcl)
{
    ULONG status;
    SID *everyoneSid = NULL;
    SID *adminSid = NULL;
    EXPLICIT_ACCESS	ea[2] = { 0 };
    SID_IDENTIFIER_AUTHORITY sidAuthWorld = SECURITY_WORLD_SID_AUTHORITY;

    LogVerbose("start");

    if (!pipeSecurityDescriptor || !pipeAcl)
        return ERROR_INVALID_PARAMETER;

    // Create a well-known SID for the Everyone group.
    if (!AllocateAndInitializeSid(
        &sidAuthWorld,
        1,
        SECURITY_WORLD_RID,
        0, 0, 0, 0, 0, 0, 0,
        &everyoneSid))
    {
        return perror("AllocateAndInitializeSid");
    }

    *pipeAcl = NULL;
    *pipeSecurityDescriptor = NULL;

    // Initialize an EXPLICIT_ACCESS structure for an ACE.
    // The ACE will allow Everyone read/write access to the pipe.
    ea[0].grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_CREATE_PIPE_INSTANCE | SYNCHRONIZE;
    ea[0].grfAccessMode = SET_ACCESS;
    ea[0].grfInheritance = NO_INHERITANCE;
    ea[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea[0].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea[0].Trustee.ptstrName = (WCHAR *)everyoneSid;

    // Create a new ACL that contains the new ACE.
    status = SetEntriesInAcl(1, ea, NULL, pipeAcl);

    if (ERROR_SUCCESS != status)
    {
        perror2(status, "SetEntriesInAcl");
        goto cleanup;
    }

    // Initialize a security descriptor.
    *pipeSecurityDescriptor = (SECURITY_DESCRIPTOR *)LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
    if (*pipeSecurityDescriptor == NULL)
    {
        perror("LocalAlloc");
        goto cleanup;
    }

    if (!InitializeSecurityDescriptor(*pipeSecurityDescriptor, SECURITY_DESCRIPTOR_REVISION))
    {
        status = perror("InitializeSecurityDescriptor");
        goto cleanup;
    }

    // Add the ACL to the security descriptor.
    if (!SetSecurityDescriptorDacl(*pipeSecurityDescriptor, TRUE, *pipeAcl, FALSE))
    {
        status = perror("SetSecurityDescriptorDacl");
        goto cleanup;
    }

    status = ERROR_SUCCESS;

    LogVerbose("success");

cleanup:
    if (everyoneSid)
        FreeSid(everyoneSid);
    if (status != ERROR_SUCCESS && (*pipeAcl))
        LocalFree(*pipeAcl);
    if (status != ERROR_SUCCESS && (*pipeSecurityDescriptor))
        LocalFree(*pipeSecurityDescriptor);
    return status;
}

// This function is called to start an overlapped connect operation.
// It sets pendingIo to TRUE if an operation is pending or to FALSE if the
// connection has been completed.
static ULONG ConnectToNewClient(IN HANDLE clientPipe, OUT OVERLAPPED *asyncState, IN HANDLE connectionEvent, OUT BOOL *pendingIo)
{
    LogVerbose("start");

    if (!pendingIo)
        return ERROR_INVALID_PARAMETER;

    *pendingIo = FALSE;
    ZeroMemory(asyncState, sizeof(*asyncState));

    asyncState->hEvent = connectionEvent;

    // Start an overlapped connection for this pipe instance.
    if (ConnectNamedPipe(clientPipe, asyncState))
    {
        return perror("ConnectNamedPipe");
    }

    switch (GetLastError())
    {
        // The overlapped connection in progress.
    case ERROR_IO_PENDING:
        *pendingIo = TRUE;
        break;

        // Client is already connected, so signal the event.
    case ERROR_PIPE_CONNECTED:
        SetEvent(asyncState->hEvent);
        break;

        // If an error occurs during the connect operation
    default:
        return perror("ConnectNamedPipe");
    }

    LogVerbose("success");

    return ERROR_SUCCESS;
}

// This function is called:
// - when an error occurs;
// - when the client closes its handle to the pipe;
// - when the server disconnects from the client.
// Disconnect from this client, then call ConnectNamedPipe to wait for another client to connect.
ULONG DisconnectAndReconnect(IN ULONG clientIndex)
{
    ULONG status;

    LogDebug("Disconnecting client %lu, state %d\n", clientIndex, g_Pipes[clientIndex].ConnectionState);

    if (g_Pipes[clientIndex].ClientProcess)
        CloseHandle(g_Pipes[clientIndex].ClientProcess);
    g_Pipes[clientIndex].ClientProcess = 0;

    g_Pipes[clientIndex].CreateProcessResponse.ResponseType = CPR_TYPE_NONE;

    if (g_Pipes[clientIndex].ClientInfo.WriteStdinPipe)
        CloseHandle(g_Pipes[clientIndex].ClientInfo.WriteStdinPipe);

    // There is no IO going in these pipes, so we can safely pass any
    // client_id to CloseReadPipeHandles - it will not be used anywhere.
    // Once a pipe becomes watched, these handles are moved to g_Clients,
    // and these structures are zeroed.
    if (g_Pipes[clientIndex].ClientInfo.StdoutData.ReadPipe)
        CloseReadPipeHandles(NULL, &g_Pipes[clientIndex].ClientInfo.StdoutData);

    if (g_Pipes[clientIndex].ClientInfo.StderrData.ReadPipe)
        CloseReadPipeHandles(NULL, &g_Pipes[clientIndex].ClientInfo.StderrData);

    ZeroMemory(&g_Pipes[clientIndex].ClientInfo, sizeof(g_Pipes[clientIndex].ClientInfo));
    ZeroMemory(&g_Pipes[clientIndex].RemoteHandles, sizeof(g_Pipes[clientIndex].RemoteHandles));
    ZeroMemory(&g_Pipes[clientIndex].ConnectParams, sizeof(g_Pipes[clientIndex].ConnectParams));
    g_Pipes[clientIndex].Vchan = NULL;

    // Disconnect the pipe instance.
    if (!DisconnectNamedPipe(g_Pipes[clientIndex].Pipe))
    {
        return perror("DisconnectNamedPipe");
    }

    // Call a subroutine to connect to the new client.
    status = ConnectToNewClient(g_Pipes[clientIndex].Pipe, &g_Pipes[clientIndex].AsyncState, g_Events[clientIndex], &g_Pipes[clientIndex].PendingIo);
    if (ERROR_SUCCESS != status)
    {
        return perror2(status, "ConnectToNewClient");
    }

    g_Pipes[clientIndex].ConnectionState = g_Pipes[clientIndex].PendingIo ? STATE_WAITING_FOR_CLIENT : STATE_SENDING_IO_HANDLES;

    LogVerbose("success");

    return ERROR_SUCCESS;
}

static ULONG ClosePipeHandles(void)
{
    ULONG i;

    LogVerbose("start");

    for (i = 0; i < TRIGGER_PIPE_INSTANCES; i++)
    {
        if (g_Pipes[i].PendingIo)
        {
            if (CancelIo(g_Pipes[i].Pipe))
            {
                // Must wait for the canceled IO to complete, otherwise a race condition may occur on the
                // OVERLAPPED structure.
                WaitForSingleObject(g_Pipes[i].AsyncState.hEvent, INFINITE);
            }
            else
            {
                perror("CancelIo");
            }
        }

        CloseHandle(g_Events[i]);
    }

    LogVerbose("success");

    return ERROR_SUCCESS;
}

static ULONG ConnectExisting(
    IN libvchan_t *vchan,
    IN HANDLE clientProcess,
    OUT CLIENT_INFO *clientInfo,
    IN const struct trigger_service_params *connectParams,
    IN const CREATE_PROCESS_RESPONSE *cpr
    )
{
    ULONG status;

    LogVerbose("vchan %p, ident '%S', vm '%S'", vchan, connectParams->service_name, connectParams->target_domain);

    if (!clientInfo || !connectParams || !cpr)
        return ERROR_INVALID_PARAMETER;

    LogDebug("vchan %p: service '%S', vm '%S'", vchan, connectParams->service_name, connectParams->target_domain);

    if (CPR_TYPE_ERROR_CODE == cpr->ResponseType)
    {
        LogWarning("vchan %p0: Process creation failed, got the error code %u", vchan, cpr->ResponseData.ErrorCode);

        status = SendExitCode(clientInfo, MAKE_ERROR_RESPONSE(ERROR_SET_WINDOWS, cpr->ResponseData.ErrorCode));
        if (ERROR_SUCCESS != status)
            return perror2(status, "SendExitCode");

        return status;
    }

    if (!DuplicateHandle(
        clientProcess,
        cpr->ResponseData.Process,
        GetCurrentProcess(),
        &clientInfo->ChildProcess,
        0,
        TRUE,
        DUPLICATE_SAME_ACCESS))
    {
        return perror("DuplicateHandle");
    }

    status = AddExistingClient(clientInfo);
    if (ERROR_SUCCESS != status)
    {
        return perror2(status, "AddExistingClient");
        // DisconnectAndReconnect will close all the handles later
    }

    // Clear the handles; now the WatchForEvents thread takes care of them.
    ZeroMemory(clientInfo, sizeof(*clientInfo));

    LogVerbose("success");

    return ERROR_SUCCESS;
}

// This routine will be called by a single thread only (WatchForTriggerEvents thread),
// and this is the only place where g_DaemonRequestsCounter is read and written, so
// there is no need to do InterlockedIncrement on the counter.

// However, g_Pipes list may be accessed by another thread (WatchForEvents thread),
// so it must be locked. This access happens when a daemon responds with a CONNECT_EXISTING
// message.
ULONG SendParametersToDaemon(IN ULONG clientIndex)
{
    HRESULT hresult;
    ULONG status;
    struct trigger_service_params connectParams;

    LogVerbose("client index %d", clientIndex);

    if (clientIndex >= TRIGGER_PIPE_INSTANCES)
        return ERROR_INVALID_PARAMETER;

    EnterCriticalSection(&g_PipesCriticalSection);

    hresult = StringCchPrintfA(
        (STRSAFE_LPSTR)&g_Pipes[clientIndex].ConnectParams.request_id.ident,
        sizeof(g_Pipes[clientIndex].ConnectParams.request_id.ident),
        "%I64x",
        g_DaemonRequestsCounter++);

    if (FAILED(hresult))
    {
        perror2(hresult, "StringCchPrintfA");
        LeaveCriticalSection(&g_PipesCriticalSection);
        return hresult;
    }

    connectParams = g_Pipes[clientIndex].ConnectParams;

    LeaveCriticalSection(&g_PipesCriticalSection);

    //uResult = send_msg_to_vchan(g_daemon_vchan, MSG_TRIGGER_SERVICE, &params, sizeof(params), NULL, TEXT("trigger_service_params"));
    status = SendMessageToVchan(g_DaemonVchan, MSG_TRIGGER_SERVICE, &connectParams, sizeof(connectParams), NULL, L"trigger_service_params");
    if (ERROR_SUCCESS != status)
    {
        return perror2(status, "ReturnData");
    }

    LogVerbose("success");

    return ERROR_SUCCESS;
}

ULONG FindClientByIdent(IN const char *ident, OUT ULONG *clientIndex)
{
    ULONG i;

    LogVerbose("ident '%S'", ident);

    if (!ident || !clientIndex)
        return ERROR_INVALID_PARAMETER;

    for (i = 0; i < TRIGGER_PIPE_INSTANCES; i++)
    {
        if (!strncmp(g_Pipes[i].ConnectParams.request_id.ident, ident, sizeof(g_Pipes[i].ConnectParams.request_id.ident)))
        {
            *clientIndex = i;
            LogVerbose("found index %d", i);
            return ERROR_SUCCESS;
        }
    }

    LogVerbose("not found");
    return ERROR_NOT_FOUND;
}

ULONG ProceedWithExecution(
    IN libvchan_t *vchan,
    IN const char *ident,
    BOOL isVchanServer
    )
{
    ULONG clientIndex;
    ULONG status;

    LogVerbose("vchan %p, ident '%S'", vchan, ident);

    if (!ident)
        return ERROR_INVALID_PARAMETER;

    EnterCriticalSection(&g_PipesCriticalSection);

    status = FindClientByIdent(ident, &clientIndex);
    if (ERROR_SUCCESS != status)
    {
        perror2(status, "FindPipeByIdent");
        LogError("id=%s", ident);
        LeaveCriticalSection(&g_PipesCriticalSection);
        return status;
    }

    if (STATE_WAITING_FOR_DAEMON_DECISION != g_Pipes[clientIndex].ConnectionState)
    {
        LogWarning("Wrong pipe state %d, should be %d", g_Pipes[clientIndex].ConnectionState, STATE_WAITING_FOR_DAEMON_DECISION);
        LeaveCriticalSection(&g_PipesCriticalSection);
        return ERROR_INVALID_PARAMETER;
    }

    g_Pipes[clientIndex].Vchan = vchan;

    // Signal that we're allowed to send io handles to qrexec_client_vm.
    SetEvent(g_Events[clientIndex]);

    LeaveCriticalSection(&g_PipesCriticalSection);

    LogVerbose("success");

    return ERROR_SUCCESS;
}

// fixme: function way too long
ULONG WINAPI WatchForTriggerEvents(IN void *param)
{
    DWORD waitResult, cbRet, cbToWrite, cbRead;
    ULONG clientIndex;
    ULONG status;
    BOOL success;
    IO_HANDLES localHandles;
    ULONG clientProcessId;
    SECURITY_ATTRIBUTES sa;
    SECURITY_DESCRIPTOR *pipeSecurityDescriptor;
    ACL *acl;

    LogDebug("start");
    ZeroMemory(&g_Pipes, sizeof(g_Pipes));

    status = CreatePipeSecurityDescriptor(&pipeSecurityDescriptor, &acl);
    if (ERROR_SUCCESS != status)
    {
        return perror2(status, "CreatePipeSecurityDescriptor");
    }

    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    sa.lpSecurityDescriptor = pipeSecurityDescriptor;

    // The initial loop creates several instances of a named pipe
    // along with an event object for each instance. An
    // overlapped ConnectNamedPipe operation is started for
    // each instance.

    for (clientIndex = 0; clientIndex < TRIGGER_PIPE_INSTANCES; clientIndex++)
    {
        // Create an event object for this instance.

        g_Events[clientIndex] = CreateEvent(
            NULL, // default security attribute
            FALSE, // auto-reset event
            FALSE, // initial state = not signaled
            NULL); // unnamed event object

        if (g_Events[clientIndex] == NULL)
        {
            status = GetLastError();
            LocalFree(pipeSecurityDescriptor);
            LocalFree(acl);
            return perror2(status, "CreateEvent");
        }

        g_Pipes[clientIndex].Pipe = CreateNamedPipe(
            TRIGGER_PIPE_NAME,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            TRIGGER_PIPE_INSTANCES, // number of instances
            PIPE_BUFFER_SIZE, // output buffer size
            PIPE_BUFFER_SIZE, // input buffer size
            PIPE_TIMEOUT, // client time-out
            &sa);

        if (INVALID_HANDLE_VALUE == g_Pipes[clientIndex].Pipe)
        {
            status = GetLastError();
            LocalFree(pipeSecurityDescriptor);
            LocalFree(acl);
            return perror2(status, "CreateNamedPipe");
        }

        // Call the subroutine to connect to the new client

        status = ConnectToNewClient(
            g_Pipes[clientIndex].Pipe,
            &g_Pipes[clientIndex].AsyncState,
            g_Events[clientIndex],
            &g_Pipes[clientIndex].PendingIo);

        if (ERROR_SUCCESS != status)
        {
            LocalFree(pipeSecurityDescriptor);
            LocalFree(acl);
            return perror2(status, "ConnectToNewClient");
        }

        g_Pipes[clientIndex].ConnectionState = g_Pipes[clientIndex].PendingIo ? STATE_WAITING_FOR_CLIENT : STATE_SENDING_IO_HANDLES;
    }

    LocalFree(pipeSecurityDescriptor);
    LocalFree(acl);

    // Last one will signal the service shutdown.
    g_Events[TRIGGER_PIPE_INSTANCES] = g_StopServiceEvent;

    LogVerbose("event loop");

    while (TRUE)
    {
        // Wait for the event object to be signaled, indicating
        // completion of an overlapped read, write, or
        // connect operation.

        LogVerbose("waiting for events");

        waitResult = WaitForMultipleObjects(
            TRIGGER_PIPE_INSTANCES + 1, // number of event objects
            g_Events, // array of event objects
            FALSE, // does not wait for all
            INFINITE); // waits indefinitely

        // dwWait shows which pipe completed the operation.

        clientIndex = waitResult - WAIT_OBJECT_0; // determines which pipe

        LogVerbose("event %d signaled", clientIndex);

        if (TRIGGER_PIPE_INSTANCES == clientIndex)
        {
            // Service is shuttiung down, close the pipe handles.
            LogInfo("Shutting down\n");
            ClosePipeHandles();

            return ERROR_SUCCESS;
        }

        if (clientIndex > (TRIGGER_PIPE_INSTANCES - 1))
        {
            return perror("WaitForMultipleObjects");
        }

        // Get the result of the pending operation that has just finished.
        if (g_Pipes[clientIndex].PendingIo)
        {
            if (!GetOverlappedResult(g_Pipes[clientIndex].Pipe, &g_Pipes[clientIndex].AsyncState, &cbRet, FALSE))
            {
                perror("GetOverlappedResult");
                DisconnectAndReconnect(clientIndex);
                continue;
            }

            // Clear the pending operation flag.
            g_Pipes[clientIndex].PendingIo = FALSE;

            switch (g_Pipes[clientIndex].ConnectionState)
            {
                // Pending connect operation
            case STATE_WAITING_FOR_CLIENT:
                LogVerbose("STATE_WAITING_FOR_CLIENT");

                if (!GetNamedPipeClientProcessId(g_Pipes[clientIndex].Pipe, &clientProcessId))
                {
                    perror("GetNamedPipeClientProcessId");
                    DisconnectAndReconnect(clientIndex);
                    continue;
                }

                LogDebug("STATE_WAITING_FOR_CLIENT (pending): Accepted connection from the process %d\n", clientProcessId);

                g_Pipes[clientIndex].ClientProcess = OpenProcess(PROCESS_DUP_HANDLE, FALSE, clientProcessId);
                if (!g_Pipes[clientIndex].ClientProcess)
                {
                    perror("OpenProcess");
                    DisconnectAndReconnect(clientIndex);
                    continue;
                }

                g_Pipes[clientIndex].ConnectionState = STATE_RECEIVING_PARAMETERS;
                break;

                // Make sure the incoming message has right size
            case STATE_RECEIVING_PARAMETERS:
                LogVerbose("STATE_RECEIVING_PARAMETERS");

                if (sizeof(g_Pipes[clientIndex].ConnectParams) != cbRet)
                {
                    LogWarning("Wrong incoming parameter size: %d instead of %d\n", cbRet, sizeof(g_Pipes[clientIndex].ConnectParams));
                    DisconnectAndReconnect(clientIndex);
                    continue;
                }

                LogDebug("STATE_RECEIVING_PARAMETERS (pending): Received the parameters, sending them to the daemon\n");
                status = SendParametersToDaemon(clientIndex);
                if (ERROR_SUCCESS != status)
                {
                    perror2(status, "SendParametersToDaemon");
                    DisconnectAndReconnect(clientIndex);
                    continue;
                }
                g_Pipes[clientIndex].ConnectionState = STATE_WAITING_FOR_DAEMON_DECISION;
                continue;

                // Pending write operation
            case STATE_SENDING_IO_HANDLES:
                LogVerbose("STATE_SENDING_IO_HANDLES");

                if (IO_HANDLES_SIZE != cbRet)
                {
                    LogWarning("Could not send the handles array: sent %d bytes instead of %d\n", cbRet, IO_HANDLES_SIZE);
                    DisconnectAndReconnect(clientIndex);
                    continue;
                }

                LogDebug("STATE_SENDING_IO_HANDLES (pending): IO handles have been sent, waiting for the process handle\n");
                g_Pipes[clientIndex].ConnectionState = STATE_RECEIVING_PROCESS_HANDLE;
                continue;

                // Pending read operation
            case STATE_RECEIVING_PROCESS_HANDLE:
                LogVerbose("STATE_RECEIVING_PROCESS_HANDLE");

                if (sizeof(CREATE_PROCESS_RESPONSE) != cbRet)
                {
                    LogWarning("Wrong incoming create process response size: %d\n", cbRet);
                    DisconnectAndReconnect(clientIndex);
                    continue;
                }

                LogDebug("STATE_RECEIVING_PROCESS_HANDLE (pending): Received the create process response\n");

                status = ConnectExisting(
                    g_Pipes[clientIndex].Vchan,
                    g_Pipes[clientIndex].ClientProcess,
                    &g_Pipes[clientIndex].ClientInfo,
                    &g_Pipes[clientIndex].ConnectParams,
                    &g_Pipes[clientIndex].CreateProcessResponse);

                if (ERROR_SUCCESS != status)
                    perror2(status, "ConnectExisting");

                DisconnectAndReconnect(clientIndex);
                continue;

            default:
                LogWarning("Invalid pipe state %d\n", g_Pipes[clientIndex].ConnectionState);
                continue;
            }
        }

        // The pipe state determines which operation to do next.
        switch (g_Pipes[clientIndex].ConnectionState)
        {
        case STATE_RECEIVING_PARAMETERS:
            LogVerbose("STATE_RECEIVING_PARAMETERS (immediate)");

            success = ReadFile(
                g_Pipes[clientIndex].Pipe,
                &g_Pipes[clientIndex].ConnectParams,
                sizeof(g_Pipes[clientIndex].ConnectParams),
                &cbRead,
                &g_Pipes[clientIndex].AsyncState);

            // The read operation completed successfully.

            if (success && sizeof(g_Pipes[clientIndex].ConnectParams) == cbRead)
            {
                // g_Events[clientIndex] is in the signaled state here, so we must reset it before sending anything to the daemon.
                // If the daemon allows the execution then it will be signaled in ProceedWithExecution() later,
                // if not, the pipe will be disconnected.
                ResetEvent(g_Events[clientIndex]);

                // Change the pipe state before calling SendParametersToDaemon() because another thread may call
                // ProceedWithExecution() even before the current thread returns from SendParametersToDaemon().
                // ProceedWithExecution() checks the pipe state to be STATE_WAITING_FOR_DAEMON_DECISION.
                g_Pipes[clientIndex].PendingIo = FALSE;
                g_Pipes[clientIndex].ConnectionState = STATE_WAITING_FOR_DAEMON_DECISION;

                LogDebug("STATE_RECEIVING_PARAMETERS: Immediately got the params %S, %S\n",
                    g_Pipes[clientIndex].ConnectParams.service_name, g_Pipes[clientIndex].ConnectParams.target_domain);

                status = SendParametersToDaemon(clientIndex);
                if (ERROR_SUCCESS != status)
                {
                    perror2(status, "SendParametersToDaemon");
                    DisconnectAndReconnect(clientIndex);
                    continue;
                }

                continue;
            }

            // The read operation is still pending.

            status = GetLastError();
            if (!success && (ERROR_IO_PENDING == status))
            {
                LogDebug("STATE_RECEIVING_PARAMETERS: Read is pending\n");
                g_Pipes[clientIndex].PendingIo = TRUE;
                continue;
            }

            // An error occurred; disconnect from the client.
            perror2(status, "STATE_RECEIVING_PARAMETERS: ReadFile");
            DisconnectAndReconnect(clientIndex);
            break;

        case STATE_WAITING_FOR_DAEMON_DECISION:
            if (!g_Pipes[clientIndex].Vchan)
            {
                LogInfo("Service request '%S' (request_id '%S') denied by daemon, disconnecting client-vm\n",
                    g_Pipes[clientIndex].ConnectParams.service_name, g_Pipes[clientIndex].ConnectParams.request_id.ident);
                DisconnectAndReconnect(clientIndex);
                continue;
            }
            else
            {
                LogDebug("Service request '%S' (request_id '%S') allowed by daemon, sending IO handles",
                    g_Pipes[clientIndex].ConnectParams.service_name, g_Pipes[clientIndex].ConnectParams.request_id.ident);
                // The pipe in this state should never have PendingIO flag set.
                g_Pipes[clientIndex].ConnectionState = STATE_SENDING_IO_HANDLES;
                // passthrough
            }

        case STATE_SENDING_IO_HANDLES:
            LogVerbose("STATE_SENDING_IO_HANDLES (immediate)");

            cbToWrite = IO_HANDLES_SIZE;

            status = CreateClientPipes(
                &g_Pipes[clientIndex].ClientInfo,
                &localHandles.StdinPipe,
                &localHandles.StdoutPipe,
                &localHandles.StderrPipe);

            if (ERROR_SUCCESS != status)
            {
                perror2(status, "CreateClientPipes");
                DisconnectAndReconnect(clientIndex);
                continue;
            }

            if (!DuplicateHandle(
                GetCurrentProcess(),
                localHandles.StdinPipe,
                g_Pipes[clientIndex].ClientProcess,
                &g_Pipes[clientIndex].RemoteHandles.StdinPipe,
                0,
                TRUE,
                DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE))
            {
                perror("DuplicateHandle(stdin)");
                CloseHandle(localHandles.StdoutPipe);
                CloseHandle(localHandles.StderrPipe);
                DisconnectAndReconnect(clientIndex);
                continue;
            }

            if (!DuplicateHandle(
                GetCurrentProcess(),
                localHandles.StdoutPipe,
                g_Pipes[clientIndex].ClientProcess,
                &g_Pipes[clientIndex].RemoteHandles.StdoutPipe,
                0,
                TRUE,
                DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE))
            {
                perror("DuplicateHandle(stdout)");
                CloseHandle(localHandles.StderrPipe);
                DisconnectAndReconnect(clientIndex);
                continue;
            }

            if (!DuplicateHandle(
                GetCurrentProcess(),
                localHandles.StderrPipe,
                g_Pipes[clientIndex].ClientProcess,
                &g_Pipes[clientIndex].RemoteHandles.StderrPipe,
                0,
                TRUE,
                DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE))
            {
                perror("DuplicateHandle(stderr)");
                DisconnectAndReconnect(clientIndex);
                continue;
            }

            success = WriteFile(
                g_Pipes[clientIndex].Pipe,
                &g_Pipes[clientIndex].RemoteHandles,
                IO_HANDLES_SIZE,
                &cbRet,
                &g_Pipes[clientIndex].AsyncState);

            if (!success || IO_HANDLES_SIZE != cbRet)
            {
                // The write operation is still pending.

                status = GetLastError();
                if ((ERROR_IO_PENDING == status) && !success)
                {
                    LogDebug("STATE_SENDING_IO_HANDLES: Write is pending\n");
                    g_Pipes[clientIndex].PendingIo = TRUE;
                    continue;
                }

                // An error occurred; disconnect from the client.
                perror("STATE_SENDING_IO_HANDLES: WriteFile");
                DisconnectAndReconnect(clientIndex);
                break;
            }

            // The write operation completed successfully.
            // g_hEvents[i] is in the signaled state here, but the upcoming ReadFile will change its state accordingly.
            LogDebug("STATE_SENDING_IO_HANDLES: IO handles have been sent, waiting for the process handle\n");
            g_Pipes[clientIndex].PendingIo = FALSE;
            g_Pipes[clientIndex].ConnectionState = STATE_RECEIVING_PROCESS_HANDLE;
            // passthrough

        case STATE_RECEIVING_PROCESS_HANDLE:
            LogVerbose("STATE_RECEIVING_PROCESS_HANDLE (immediate)");

            success = ReadFile(
                g_Pipes[clientIndex].Pipe,
                &g_Pipes[clientIndex].CreateProcessResponse,
                sizeof(CREATE_PROCESS_RESPONSE),
                &cbRead,
                &g_Pipes[clientIndex].AsyncState);

            // The read operation completed successfully.

            if (success && sizeof(CREATE_PROCESS_RESPONSE) == cbRead)
            {
                LogDebug("STATE_RECEIVING_PROCESS_HANDLE: Received the create process response\n");

                status = ConnectExisting(
                    g_Pipes[clientIndex].Vchan,
                    g_Pipes[clientIndex].ClientProcess,
                    &g_Pipes[clientIndex].ClientInfo,
                    &g_Pipes[clientIndex].ConnectParams,
                    &g_Pipes[clientIndex].CreateProcessResponse);

                if (ERROR_SUCCESS != status)
                    perror2(status, "ConnectExisting");

                DisconnectAndReconnect(clientIndex);
                continue;
            }

            // The read operation is still pending.

            status = GetLastError();
            if (!success && (ERROR_IO_PENDING == status))
            {
                LogDebug("STATE_RECEIVING_PROCESS_HANDLE: Read is pending\n");
                g_Pipes[clientIndex].PendingIo = TRUE;
                continue;
            }

            // An error occurred; disconnect from the client.
            perror("STATE_RECEIVING_PROCESS_HANDLE: ReadFile");
            DisconnectAndReconnect(clientIndex);
            break;

        default:
            LogWarning("Invalid pipe state %d", g_Pipes[clientIndex].ConnectionState);
            return ERROR_INVALID_PARAMETER;
        }
    }

    LogVerbose("success");

    return ERROR_SUCCESS;
}
