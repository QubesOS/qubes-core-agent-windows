#include "pipe_server.h"



extern HANDLE	g_hStopServiceEvent; 
 
CRITICAL_SECTION	g_PipesCriticalSection;
PIPEINST g_Pipes[INSTANCES];
HANDLE g_hEvents[INSTANCES + 1];

ULONG64	g_uDaemonRequestsCounter = 1;

// This function is called to start an overlapped connect operation.
// It sets *pbPendingIO to TRUE if an operation is pending or to FALSE if the
// connection has been completed.

ULONG ConnectToNewClient(HANDLE hPipe, LPOVERLAPPED lpo, HANDLE hEvent, BOOLEAN *pbPendingIO)
{
	BOOLEAN	bPendingIO = FALSE;
	ULONG	uResult;


	if (!pbPendingIO)
		return ERROR_INVALID_PARAMETER;


	memset(lpo, 0, sizeof(OVERLAPPED));
	lpo->hEvent = hEvent;

	// Start an overlapped connection for this pipe instance.
	if (ConnectNamedPipe(hPipe, lpo)) {
		uResult = GetLastError();
		lprintf_err(uResult, "ConnectToNewClient(): ConnectNamedPipe()");
		return uResult;
	}

	switch (GetLastError()) {
		// The overlapped connection in progress.
		case ERROR_IO_PENDING:
			bPendingIO = TRUE;
			break;

		// Client is already connected, so signal an event.
		case ERROR_PIPE_CONNECTED:
			SetEvent(lpo->hEvent);
			break;

		// If an error occurs during the connect operation
		default:
			uResult = GetLastError();
			lprintf_err(uResult, "ConnectToNewClient(): ConnectNamedPipe()");
			return uResult;
	}

	*pbPendingIO = bPendingIO;
	return ERROR_SUCCESS;
}


// ULONG DisconnectAndReconnect(ULONG) 
// This function is called:
// - when an error occurs;
// - when the client closes its handle to the pipe;
// - when the server disconnects from the client.
// Disconnect from this client, then call ConnectNamedPipe to wait for another client to connect. 
 
ULONG DisconnectAndReconnect(ULONG i)
{ 
	ULONG	uResult;


	lprintf("disconnecting pipe %d, state %d\n", i, g_Pipes[i].uState);

	memset(&g_Pipes[i].params, 0, sizeof(g_Pipes[i].params));


	if (g_Pipes[i].hClientProcess)
		CloseHandle(g_Pipes[i].hClientProcess);
	g_Pipes[i].hClientProcess = 0;

	if (g_Pipes[i].hReceivedProcessHandle)
		CloseHandle(g_Pipes[i].hReceivedProcessHandle);
	g_Pipes[i].hReceivedProcessHandle = 0;

	if (g_Pipes[i].ClientInfo.hWriteStdinPipe)
		CloseHandle(g_Pipes[i].ClientInfo.hWriteStdinPipe);

	// There is no IO going in these pipes, so we can safely pass any
	// client_id to CloseReadPipeHandles - it will not be used anywhere.
	// Once a pipe becomes watched, these handles are moved to g_Clients,
	// and these structures are zeroed.
	if (g_Pipes[i].ClientInfo.Stdout.hReadPipe)
		CloseReadPipeHandles(-1, &g_Pipes[i].ClientInfo.Stdout);

	if (g_Pipes[i].ClientInfo.Stderr.hReadPipe)
		CloseReadPipeHandles(-1, &g_Pipes[i].ClientInfo.Stderr);

	memset(&g_Pipes[i].ClientInfo, 0, sizeof(g_Pipes[i].ClientInfo));

	// Disconnect the pipe instance. 
 
	if (!DisconnectNamedPipe(g_Pipes[i].hPipeInst)) {
		uResult = GetLastError();
		lprintf_err(uResult, "DisconnectAndReconnect(): DisconnectNamedPipe()");
		return uResult;
	}

	// Call a subroutine to connect to the new client.	

	uResult = ConnectToNewClient(g_Pipes[i].hPipeInst, &g_Pipes[i].oOverlapped, g_hEvents[i], &g_Pipes[i].fPendingIO);
	if (ERROR_SUCCESS != uResult) {
		lprintf_err(uResult, "DisconnectAndReconnect(): ConnectToNewClient()");
		return uResult;
	}

	g_Pipes[i].uState = g_Pipes[i].fPendingIO ? STATE_WAITING_FOR_CLIENT : STATE_SENDING_IO_HANDLES;
	return ERROR_SUCCESS;
}


ULONG ClosePipeHandles()
{
	ULONG	i;
	ULONG	uResult;


	for (i = 0; i < INSTANCES; i++) {
		if (g_Pipes[i].fPendingIO) {
		
			if (CancelIo(g_Pipes[i].hPipeInst)) {

				// Must wait for the canceled IO to complete, otherwise a race condition may occur on the
				// OVERLAPPED structure.
				WaitForSingleObject(g_Pipes[i].oOverlapped.hEvent, INFINITE);

			} else {
				uResult = GetLastError();
				lprintf_err(uResult, "ClosePipeHandles(): CancelIo()");
			}
		}

		CloseHandle(g_hEvents[i]);
	}

	return ERROR_SUCCESS;
}


ULONG ConnectExisting(HANDLE hClientProcess, PCLIENT_INFO pClientInfo, struct trigger_connect_params *pparams, HANDLE hReceivedProcessHandle)
{
	ULONG	uResult;
	HANDLE	hLocalProcessHandle;


	if (!pClientInfo || !pparams)
		return ERROR_INVALID_PARAMETER;

	lprintf("ConnectExisting(): Got the params %s, %s\n", pparams->exec_index, pparams->target_vmname);
	lprintf("ConnectExisting(): Got the handle 0x%X\n", hReceivedProcessHandle);


	if (!DuplicateHandle(
		hClientProcess,
		hReceivedProcessHandle,
		GetCurrentProcess(),
		&hLocalProcessHandle,
		0,
		TRUE,
		DUPLICATE_SAME_ACCESS)) {

		uResult = GetLastError();

		lprintf_err(uResult, "ConnectExisting(): DuplicateHandle()");
		return uResult;				
	}


//	uResult = AddExistingClient(client_id, pClientInfo);
	CloseHandle(hLocalProcessHandle);

	return ERROR_SUCCESS;
}


// This routine will be called by a single thread only (WatchForTriggerEvents thread),
// and this is the only place where g_uDaemonRequestsCounter is read and written, so
// there is no need to do InterlockedIncrement on the counter.

// However, g_Pipes list may be accessed by another thread (WatchForEvents thread), 
// so it must be locked. This access happens when a daemon responds with a CONNECT_EXISTING 
// message.
ULONG SendParametersToDaemon(ULONG i)
{
	HRESULT	hResult;


	if (i >= INSTANCES)
		return ERROR_INVALID_PARAMETER;

	EnterCriticalSection(&g_PipesCriticalSection);

	hResult = StringCchPrintfA(
			(STRSAFE_LPSTR)&g_Pipes[i].params.process_fds.ident, 
			sizeof(g_Pipes[i].params.process_fds.ident), 
			"%I64x", 
			g_uDaemonRequestsCounter++);
	if (FAILED(hResult)) {
		lprintf_err(hResult, "SendParametersToDaemon(): StringCchPrintfA()");
		LeaveCriticalSection(&g_PipesCriticalSection);
		return hResult;
	}

	printf("ident (%d): %s\n", i, g_Pipes[i].params.process_fds.ident);

	LeaveCriticalSection(&g_PipesCriticalSection);


	ProceedWithExecution(g_Pipes[i].params.process_fds.ident);

	return ERROR_SUCCESS;
}


ULONG FindPipeByIdent(PUCHAR pszIdent, PULONG puPipeNumber)
{
	ULONG	i;


	if (!pszIdent || !puPipeNumber)
		return ERROR_INVALID_PARAMETER;


	for (i = 0; i < INSTANCES; i++) {
		if (!strcmp(g_Pipes[i].params.process_fds.ident, pszIdent)) {
			*puPipeNumber = i;
			return ERROR_SUCCESS;
		}
	}


	return ERROR_NOT_FOUND;
}


ULONG ProceedWithExecution(PUCHAR pszIdent)
{
	ULONG	uPipeNumber;
	ULONG	uResult;


	if (!pszIdent)
		return ERROR_INVALID_PARAMETER;

	EnterCriticalSection(&g_PipesCriticalSection);

	uResult = FindPipeByIdent(pszIdent, &uPipeNumber);
	if (ERROR_SUCCESS != uResult) {
		lprintf_err(uResult, "ProceedWithExecution(): FindPipeByIdent(%s)", pszIdent);
		LeaveCriticalSection(&g_PipesCriticalSection);
		return uResult;
	}

	if (STATE_WAITING_FOR_DAEMON_DECISION != g_Pipes[uPipeNumber].uState) {
		lprintf("ProceedWithExecution(): Wrong pipe state %d, should be %d\n", g_Pipes[uPipeNumber].uState, STATE_WAITING_FOR_DAEMON_DECISION);
		LeaveCriticalSection(&g_PipesCriticalSection);
		return ERROR_INVALID_PARAMETER;
	}

	// Signalize that we're allowed to send io handles to qrexec_client_vm.
	SetEvent(g_hEvents[uPipeNumber]);

	LeaveCriticalSection(&g_PipesCriticalSection);

	return ERROR_SUCCESS;
}

ULONG WINAPI WatchForTriggerEvents(PVOID pParam)
{ 
	DWORD	dwWait, cbRet, cbToWrite, cbRead; 
	ULONG	i;
	ULONG	uResult;
	BOOL	fSuccess; 
	IO_HANDLES_ARRAY	LocalHandles;
	ULONG	uClientProcessId;


	lprintf("WatchForTriggerEvents(): Init\n");
	memset(&g_Pipes, 0, sizeof(g_Pipes));
 
	// The initial loop creates several instances of a named pipe 
	// along with an event object for each instance. An 
	// overlapped ConnectNamedPipe operation is started for 
	// each instance.

	for (i = 0; i < INSTANCES; i++) { 
 
		// Create an event object for this instance. 

		g_hEvents[i] = CreateEvent( 
				NULL, // default security attribute 
				FALSE, // auto-reset event 
				FALSE, // initial state = not signaled 
				NULL); // unnamed event object 

		if (g_hEvents[i] == NULL) {
			uResult = GetLastError();
			lprintf_err(uResult, "WatchForTriggerEvents(): CreateEvent()");
			return uResult;
		} 


		g_Pipes[i].hPipeInst = CreateNamedPipe( 
					TRIGGER_PIPE_NAME,
					PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
					PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
					INSTANCES, // number of instances 
					512, // output buffer size 
					512, // input buffer size 
					PIPE_TIMEOUT, // client time-out 
					NULL); // default security attributes 

		if (INVALID_HANDLE_VALUE == g_Pipes[i].hPipeInst) {
			uResult = GetLastError();
			lprintf_err(uResult, "WatchForTriggerEvents(): CreateNamedPipe()");
			return uResult;
		}

		// Call the subroutine to connect to the new client

		uResult = ConnectToNewClient(
				g_Pipes[i].hPipeInst,
				&g_Pipes[i].oOverlapped,
				g_hEvents[i],
				&g_Pipes[i].fPendingIO);

		if (ERROR_SUCCESS != uResult) {
			lprintf_err(uResult, "WatchForTriggerEvents(): ConnectToNewClient()");
			return uResult;
		}

		g_Pipes[i].uState = g_Pipes[i].fPendingIO ? STATE_WAITING_FOR_CLIENT : STATE_SENDING_IO_HANDLES;
	}

	// Last one will signal the service shutdown.
	g_hEvents[INSTANCES] = g_hStopServiceEvent;
 
	while (TRUE) { 
		// Wait for the event object to be signaled, indicating 
		// completion of an overlapped read, write, or 
		// connect operation. 

		dwWait = WaitForMultipleObjects( 
				INSTANCES + 1, // number of event objects 
				g_hEvents, // array of event objects 
				FALSE, // does not wait for all 
				INFINITE); // waits indefinitely 
 
		// dwWait shows which pipe completed the operation. 
 
		i = dwWait - WAIT_OBJECT_0; // determines which pipe

		if (INSTANCES == i) {

			// Service is shuttiung down, close the pipe handles.
			lprintf("WatchForTriggerEvents(): Shutting down\n");
			ClosePipeHandles();

			return ERROR_SUCCESS;
		}

		if (i > (INSTANCES - 1)) {
			lprintf_err(dwWait, "WatchForTriggerEvents(): WaitForMultipleObjects()"); 
			return dwWait;
		}

		lprintf("signaled pipe %d, original state %d\n", i, g_Pipes[i].uState);

		// Get the result of the pending operation that has just finished. 
		if (g_Pipes[i].fPendingIO) {

			if (!GetOverlappedResult(g_Pipes[i].hPipeInst, &g_Pipes[i].oOverlapped,	&cbRet,	FALSE)) {

				lprintf_err(GetLastError(), "WatchForTriggerEvents(): GetOverlappedResult()");
				DisconnectAndReconnect(i);
				continue;
			}

			// Clear the pending operation flag.
			g_Pipes[i].fPendingIO = FALSE;

			switch (g_Pipes[i].uState) {
			// Pending connect operation
			case STATE_WAITING_FOR_CLIENT:
				lprintf("STATE_WAITING_FOR_CLIENT (pending): Accepted connection\n");

				if (!GetNamedPipeClientProcessId(g_Pipes[i].hPipeInst, &uClientProcessId)) {
					lprintf_err(GetLastError(), "WatchForTriggerEvents(): GetNamedPipeClientProcessId()");
					DisconnectAndReconnect(i);
					continue;
				}

				g_Pipes[i].hClientProcess = OpenProcess(PROCESS_DUP_HANDLE, FALSE, uClientProcessId);
				if (!g_Pipes[i].hClientProcess) {
					lprintf_err(GetLastError(), "WatchForTriggerEvents(): OpenProcess()");
					DisconnectAndReconnect(i);
					continue;
				}

				g_Pipes[i].uState = STATE_RECEIVING_PARAMETERS;
				break;
 
			// Make sure the incoming message has a right size
			case STATE_RECEIVING_PARAMETERS:
				if (sizeof(g_Pipes[i].params) != cbRet) {
					lprintf("WatchForTriggerEvents(): Wrong incoming parameter size: %d instead of %d\n", cbRet, sizeof(g_Pipes[i].params));
					DisconnectAndReconnect(i);
					continue;
				}

				lprintf("STATE_RECEIVING_PARAMETERS (pending): Received the parameters, sending them to the daemon\n");
				uResult = SendParametersToDaemon(i);
				if (ERROR_SUCCESS != uResult) {
					lprintf_err(uResult, "WatchForTriggerEvents(): SendParametersToDaemon()");
					DisconnectAndReconnect(i); 
					continue;
				}
				g_Pipes[i].uState = STATE_WAITING_FOR_DAEMON_DECISION;
				continue;

			// Pending write operation
			case STATE_SENDING_IO_HANDLES:
				if (IO_HANDLES_ARRAY_SIZE != cbRet) {
					lprintf("WatchForTriggerEvents(): Could not send the handles array: sent %d bytes instead of %d\n", cbRet, IO_HANDLES_ARRAY_SIZE);
					DisconnectAndReconnect(i);
					continue;
				}

				lprintf("STATE_SENDING_IO_HANDLES (pending): IO handles have been sent, waiting for the process handle\n");
				g_Pipes[i].uState = STATE_RECEIVING_PROCESS_HANDLE;
				continue;

			// Pending read operation
			case STATE_RECEIVING_PROCESS_HANDLE:
				if (sizeof(HANDLE) != cbRet) {
					lprintf("WatchForTriggerEvents(): Wrong incoming process handle size: %d\n", cbRet);
					DisconnectAndReconnect(i);
					continue;
				}

				lprintf("STATE_RECEIVING_PROCESS_HANDLE (pending): Received the process handle\n");

				uResult = ConnectExisting(
						g_Pipes[i].hClientProcess, 
						&g_Pipes[i].ClientInfo, 
						&g_Pipes[i].params, 
						g_Pipes[i].hReceivedProcessHandle);
				if (ERROR_SUCCESS != uResult)
					lprintf_err(uResult, "WatchForTriggerEvents(): ConnectExisting()");


				DisconnectAndReconnect(i);
				continue;

			default:
				lprintf("WatchForTriggerEvents(): Invalid pipe state %d\n", g_Pipes[i].uState);
				continue;
			}
		}


		lprintf("pipe %d, state %d\n", i, g_Pipes[i].uState);

		// The pipe state determines which operation to do next. 
		switch (g_Pipes[i].uState) { 

		case STATE_RECEIVING_PARAMETERS: 

			fSuccess = ReadFile( 
					g_Pipes[i].hPipeInst, 
					&g_Pipes[i].params,
					sizeof(g_Pipes[i].params),
					&cbRead,
					&g_Pipes[i].oOverlapped);

			// The read operation completed successfully.
 
			if (fSuccess && sizeof(g_Pipes[i].params) == cbRead) {
				// g_hEvents[i] is in the signaled state here, so we must reset it before sending anything to the daemon.
				// If the daemon allows the execution then it will be signaled in ProceedWithExecution() later,
				// if not, the pipe will be disconnected.
				ResetEvent(g_hEvents[i]);

				// Change the pipe state before calling SendParametersToDaemon() because another thread may call 
				// ProceedWithExecution() even before the current thread returns from SendParametersToDaemon().
				// ProceedWithExecution() checks the pipe state to be STATE_WAITING_FOR_DAEMON_DECISION.
				g_Pipes[i].fPendingIO = FALSE;
				g_Pipes[i].uState = STATE_WAITING_FOR_DAEMON_DECISION;

				lprintf("STATE_RECEIVING_PARAMETERS: Immediately got the params %s, %s\n", g_Pipes[i].params.exec_index, g_Pipes[i].params.target_vmname);

				uResult = SendParametersToDaemon(i);
				if (ERROR_SUCCESS != uResult) {
					lprintf_err(uResult, "WatchForTriggerEvents(): SendParametersToDaemon()");
					DisconnectAndReconnect(i); 
					continue;
				}

				continue;
			}
 
			// The read operation is still pending. 
 
			uResult = GetLastError(); 
			if (!fSuccess && (ERROR_IO_PENDING == uResult)) { 
				lprintf("STATE_RECEIVING_PARAMETERS: Read is pending\n");
				g_Pipes[i].fPendingIO = TRUE; 
				continue; 
			}
 
			// An error occurred; disconnect from the client.
			lprintf_err(uResult, "WatchForTriggerEvents(): STATE_RECEIVING_PARAMETERS: ReadFile()");
			DisconnectAndReconnect(i); 
			break; 

		case STATE_WAITING_FOR_DAEMON_DECISION:
			lprintf("STATE_WAITING_FOR_DAEMON_DECISION: Daemon allowed to proceed, sending the IO handles\n");
			// The pipe in this state should never have fPendingIO flag set.
			g_Pipes[i].uState = STATE_SENDING_IO_HANDLES;
			// passthrough

		case STATE_SENDING_IO_HANDLES:

			cbToWrite = IO_HANDLES_ARRAY_SIZE;

			uResult = CreateClientPipes(
					&g_Pipes[i].ClientInfo, 
					&LocalHandles.hPipeStdin, 
					&LocalHandles.hPipeStdout, 
					&LocalHandles.hPipeStderr);

			if (ERROR_SUCCESS != uResult) {
				lprintf_err(uResult, "WatchForTriggerEvents(): CreateClientPipes()");
				DisconnectAndReconnect(i);
				continue;
			}


			if (!DuplicateHandle(
				GetCurrentProcess(),
				LocalHandles.hPipeStdin,
				g_Pipes[i].hClientProcess,
				&g_Pipes[i].RemoteHandles.hPipeStdin,
				0,
				TRUE,
				DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE)) {

				uResult = GetLastError();

				CloseHandle(LocalHandles.hPipeStdout);
				CloseHandle(LocalHandles.hPipeStderr);

				lprintf_err(uResult, "WatchForTriggerEvents(): DuplicateHandle(stdin)");
				DisconnectAndReconnect(i);
				continue;
			}

			if (!DuplicateHandle(
				GetCurrentProcess(),
				LocalHandles.hPipeStdout,
				g_Pipes[i].hClientProcess,
				&g_Pipes[i].RemoteHandles.hPipeStdout,
				0,
				TRUE,
				DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE)) {

				uResult = GetLastError();

				CloseHandle(LocalHandles.hPipeStderr);

				lprintf_err(uResult, "WatchForTriggerEvents(): DuplicateHandle(stdout)");
				DisconnectAndReconnect(i);
				continue;
			}

			if (!DuplicateHandle(
				GetCurrentProcess(),
				LocalHandles.hPipeStderr,
				g_Pipes[i].hClientProcess,
				&g_Pipes[i].RemoteHandles.hPipeStderr,
				0,
				TRUE,
				DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE)) {

				lprintf_err(GetLastError(), "WatchForTriggerEvents(): DuplicateHandle(stderr)");
				DisconnectAndReconnect(i);
				continue;
			}


			fSuccess = WriteFile(
					g_Pipes[i].hPipeInst,
					&g_Pipes[i].RemoteHandles,
					IO_HANDLES_ARRAY_SIZE,
					&cbRet,
					&g_Pipes[i].oOverlapped);

			if (!fSuccess || IO_HANDLES_ARRAY_SIZE != cbRet) {
				// The write operation is still pending. 

				uResult = GetLastError(); 
				if ((ERROR_IO_PENDING == uResult) && !fSuccess) { 
					lprintf("STATE_SENDING_IO_HANDLES: Write is pending\n");
					g_Pipes[i].fPendingIO = TRUE; 
					continue; 
				}

				// An error occurred; disconnect from the client.
				lprintf_err(uResult, "WatchForTriggerEvents(): STATE_SENDING_IO_HANDLES: WriteFile()");
				DisconnectAndReconnect(i);
				break;
			}

			// The write operation completed successfully. 
			// g_hEvents[i] is in the signaled state here, but the upcoming ReadFile will change its state accordingly.
			lprintf("STATE_SENDING_IO_HANDLES: IO handles have been sent, waiting for the process handle\n");
			g_Pipes[i].fPendingIO = FALSE;
			g_Pipes[i].uState = STATE_RECEIVING_PROCESS_HANDLE;
			// passthrough


		case STATE_RECEIVING_PROCESS_HANDLE: 

			fSuccess = ReadFile( 
					g_Pipes[i].hPipeInst, 
					&g_Pipes[i].hReceivedProcessHandle, 
					sizeof(HANDLE), 
					&cbRead,
					&g_Pipes[i].oOverlapped); 

			// The read operation completed successfully. 

			if (fSuccess && sizeof(HANDLE) == cbRead) {
				lprintf("STATE_RECEIVING_PROCESS_HANDLE: Received the process handle\n");

				uResult = ConnectExisting(
						g_Pipes[i].hClientProcess, 
						&g_Pipes[i].ClientInfo, 
						&g_Pipes[i].params, 
						g_Pipes[i].hReceivedProcessHandle);
				if (ERROR_SUCCESS != uResult)
					lprintf_err(uResult, "WatchForTriggerEvents(): ConnectExisting()");

				DisconnectAndReconnect(i);
				continue;
			}
 
			// The read operation is still pending. 
 
			uResult = GetLastError(); 
			if (!fSuccess && (ERROR_IO_PENDING == uResult)) { 
				lprintf("STATE_RECEIVING_PROCESS_HANDLE(): Read is pending\n");
				g_Pipes[i].fPendingIO = TRUE; 
				continue; 
			}
 
			// An error occurred; disconnect from the client.
			lprintf_err(uResult, "WatchForTriggerEvents(): STATE_RECEIVING_PROCESS_HANDLE: ReadFile()");
			DisconnectAndReconnect(i); 
			break; 


		default:
			lprintf_err(ERROR_INVALID_PARAMETER, "WatchForTriggerEvents(): Invalid pipe state");
			return ERROR_INVALID_PARAMETER;
		}
	}

	return ERROR_SUCCESS;
}
