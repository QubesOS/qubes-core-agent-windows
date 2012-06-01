#include "qrexec_agent.h"


CLIENT_INFO	g_Clients[MAX_CLIENTS];
HANDLE	g_WatchedEvents[MAXIMUM_WAIT_OBJECTS];
HANDLE_INFO	g_HandlesInfo[MAXIMUM_WAIT_OBJECTS];

ULONG64	g_uPipeId = 0;


extern HANDLE	g_hStopServiceEvent;
#ifndef BUILD_AS_SERVICE
HANDLE	g_hCleanupFinishedEvent;
#endif


ULONG CreateAsyncPipe(HANDLE *phReadPipe, HANDLE *phWritePipe, SECURITY_ATTRIBUTES *pSecurityAttributes)
{
	TCHAR	szPipeName[MAX_PATH + 1];
	HANDLE	hReadPipe;
	HANDLE	hWritePipe;
	ULONG	uResult;


	if (!phReadPipe || !phWritePipe)
		return ERROR_INVALID_PARAMETER;

	StringCchPrintf(szPipeName, MAX_PATH, TEXT("\\\\.\\pipe\\qrexec.%08x.%I64x"), GetCurrentProcessId(), g_uPipeId++);

	hReadPipe = CreateNamedPipe(
			szPipeName,
			PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
			PIPE_TYPE_BYTE,
			1,
			512,
			512,
			50,	// the default timeout is 50ms
			pSecurityAttributes);
	if (!hReadPipe) {
		uResult = GetLastError();
		lprintf_err(uResult, "CreateAsyncPipe(): CreateNamedPipe()");
		return uResult;
	}

	hWritePipe = CreateFile(
			szPipeName,
			GENERIC_WRITE,
			0,
			pSecurityAttributes,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
			NULL);
	if (INVALID_HANDLE_VALUE == hWritePipe) {
		uResult = GetLastError();
		CloseHandle(hReadPipe);
		lprintf_err(uResult, "CreateAsyncPipe(): CreateFile()");
		return uResult;
	}

	*phReadPipe = hReadPipe;
	*phWritePipe = hWritePipe;

	return ERROR_SUCCESS;
}


ULONG InitReadPipe(PIPE_DATA *pPipeData, HANDLE *phWritePipe, UCHAR bPipeType)
{
	SECURITY_ATTRIBUTES	sa;
	ULONG	uResult;


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
	if (ERROR_SUCCESS != uResult) {
		CloseHandle(pPipeData->olRead.hEvent);
		lprintf_err(uResult, "InitReadPipe(): CreateAsyncPipe()");
		return uResult;
	}

	// Ensure the read handle to the pipe is not inherited.
	SetHandleInformation(pPipeData->hReadPipe, HANDLE_FLAG_INHERIT, 0);

	pPipeData->bPipeType = bPipeType;

	return ERROR_SUCCESS;
}


ULONG ReturnData(int client_id, int type, PVOID pData, ULONG uDataSize)
{
	struct server_header s_hdr;


	s_hdr.type = type;
	s_hdr.client_id = client_id;
	s_hdr.len = uDataSize;
	if (write_all_vchan_ext(&s_hdr, sizeof s_hdr) <= 0) {
		lprintf_err(ERROR_INVALID_FUNCTION, "ReturnData(): write_all_vchan_ext(s_hdr)");
		return ERROR_INVALID_FUNCTION;
	}

	if (!uDataSize)
		return ERROR_SUCCESS;

	if (write_all_vchan_ext(pData, uDataSize) <= 0) {
		lprintf_err(ERROR_INVALID_FUNCTION, "ReturnData(): write_all_vchan_ext(data, %d)", uDataSize);
		return ERROR_INVALID_FUNCTION;
	}

	Sleep(1);

	return ERROR_SUCCESS;
}


ULONG send_exit_code(int client_id, int status)
{
	ULONG	uResult;


	uResult = ReturnData(client_id, MSG_AGENT_TO_SERVER_EXIT_CODE, &status, sizeof(status));
	if (ERROR_SUCCESS != uResult) {
		lprintf_err(uResult, "send_exit_code(): ReturnData()");
		return uResult;
	} else
		lprintf("send_exit_code(): Send exit code %d for client_id %d\n",
			status,
			client_id);

	return ERROR_SUCCESS;
}

PCLIENT_INFO FindClientById(int client_id)
{
	ULONG	uClientNumber;


	for (uClientNumber = 0; uClientNumber < MAX_CLIENTS; uClientNumber++)
		if (client_id == g_Clients[uClientNumber].client_id)
			return &g_Clients[uClientNumber];

	return NULL;
}


ULONG ReturnPipeData(int client_id, PIPE_DATA *pPipeData)
{
	DWORD	dwRead;
	int	message_type;
	PCLIENT_INFO	pClientInfo;
	ULONG	uResult;


	uResult = ERROR_SUCCESS;

	if (!pPipeData)
		return ERROR_INVALID_PARAMETER;

	pClientInfo = FindClientById(client_id);
	if (!pClientInfo)
		return ERROR_FILE_NOT_FOUND;

	if (pClientInfo->bReadingIsDisabled)
		// The client does not want to receive any data from this console.
		return ERROR_INVALID_FUNCTION;

	pPipeData->bReadInProgress = FALSE;
	pPipeData->bDataIsReady = FALSE;


	switch (pPipeData->bPipeType) {
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
	GetOverlappedResult(pPipeData->hReadPipe, &pPipeData->olRead, &dwRead, FALSE);

	uResult = ERROR_SUCCESS;

	if (dwRead) {
		uResult = ReturnData(client_id, message_type, pPipeData->ReadBuffer, dwRead);
		if (ERROR_SUCCESS != uResult)
			lprintf_err(uResult, "ReturnPipeData(): ReturnData()");
	}

	return uResult;
}


ULONG CloseReadPipeHandles(int client_id, PIPE_DATA *pPipeData)
{
	ULONG	uResult;


	if (!pPipeData)
		return ERROR_INVALID_PARAMETER;


	uResult = ERROR_SUCCESS;

	if (pPipeData->olRead.hEvent) {

		if (pPipeData->bDataIsReady)
			ReturnPipeData(client_id, pPipeData);

		// ReturnPipeData() clears both bDataIsReady and bReadInProgress, but they cannot be ever set to a non-FALSE value at the same time.
		// So, if the above ReturnPipeData() has been executed (bDataIsReady was not FALSE), then bReadInProgress was FALSE
		// and this branch wouldn't be executed anyways.
		if (pPipeData->bReadInProgress) {

			// If bReadInProgress is not FALSE then hReadPipe must be a valid handle for which an
			// asynchornous read has been issued.
			if (CancelIo(pPipeData->hReadPipe)) {

				// Must wait for the canceled IO to complete, otherwise a race condition may occur on the
				// OVERLAPPED structure.
				WaitForSingleObject(pPipeData->olRead.hEvent, INFINITE);

				// See if there is something to return.
				ReturnPipeData(client_id, pPipeData);

			} else {
				uResult = GetLastError();
				lprintf_err(uResult, "CloseReadPipeHandles(): CancelIo()");
			}
		}

		CloseHandle(pPipeData->olRead.hEvent);
	}

	if (pPipeData->hReadPipe)
		// Can close the pipe only when there is no pending IO in progress.
		CloseHandle(pPipeData->hReadPipe);

	return uResult;
}


ULONG UTF8ToUTF16(PUCHAR pszUtf8, PWCHAR *ppwszUtf16)
{
	HRESULT	hResult;
	ULONG	uResult;
	size_t	cchUTF8;
	int	cchUTF16;
	PWCHAR	pwszUtf16;


	hResult = StringCchLengthA(pszUtf8, STRSAFE_MAX_CCH, &cchUTF8);
	if (FAILED(hResult)) {
		lprintf_err(hResult, "UTF8ToUTF16(): StringCchLengthA()");
		return hResult;
	}

	cchUTF16 = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pszUtf8, cchUTF8 + 1, NULL, 0);
	if (!cchUTF16) {
		uResult = GetLastError();
		lprintf_err(uResult, "UTF8ToUTF16(): MultiByteToWideChar()");
		return uResult;
	}

	pwszUtf16 = malloc(cchUTF16 * sizeof(WCHAR));
	if (!pwszUtf16)
		return ERROR_NOT_ENOUGH_MEMORY;

	uResult = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pszUtf8, cchUTF8 + 1, pwszUtf16, cchUTF16);
	if (!uResult) {
		uResult = GetLastError();
		lprintf_err(uResult, "UTF8ToUTF16(): MultiByteToWideChar()");
		return uResult;
	}

	*ppwszUtf16 = pwszUtf16;

	return ERROR_SUCCESS;
}

ULONG AddClient(int client_id, PUCHAR pszUtf8Command)
{
	ULONG	uResult;
	CLIENT_INFO	ClientInfo;
	HANDLE	hPipeStdout = INVALID_HANDLE_VALUE;
	HANDLE	hPipeStderr = INVALID_HANDLE_VALUE;
	HANDLE	hPipeStdin = INVALID_HANDLE_VALUE;
	ULONG	uClientNumber;
	SECURITY_ATTRIBUTES	sa;
	PWCHAR	pwszCommand;
	PWCHAR	pwszCommandLine;
	PWCHAR	pwSeparator;
	PWCHAR	pwszUserName;


	if (!pszUtf8Command)
		return ERROR_INVALID_PARAMETER;


	for (uClientNumber = 0; uClientNumber < MAX_CLIENTS; uClientNumber++)
		if (FREE_CLIENT_SPOT_ID == g_Clients[uClientNumber].client_id)
			break;

	if (MAX_CLIENTS == uClientNumber) {
		// There is no space for watching for another process
		lprintf("AddClient(): The maximum number of running processes (%d) has been reached\n", MAX_CLIENTS);
		return ERROR_TOO_MANY_CMDS;
	}

	if (FindClientById(client_id)) {
		lprintf("AddClient(): A client with the same id (#%d) already exists\n", client_id);
		return ERROR_ALREADY_EXISTS;
	}


	pwszCommand = NULL;
	uResult = UTF8ToUTF16(pszUtf8Command, &pwszCommand);
	if (ERROR_SUCCESS != uResult) {
		lprintf_err(uResult, "AddClient(): UTF8ToUTF16()");
		return uResult;
	}

	pwSeparator = wcschr(pwszCommand, L':');
	if (!pwSeparator) {
		free(pwszCommand);
		lprintf("AddClient(): Command line is supposed to be in user:command form\n");
		return ERROR_INVALID_PARAMETER;
	}

	*pwSeparator = L'\0';
	pwszUserName = pwszCommand;
	pwszCommandLine = ++pwSeparator;

	lprintf("AddClient(): Running \"%S\" under user \"%S\"\n", pwszCommandLine, pwszUserName);

	memset(&sa, 0, sizeof(sa));
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE; 
	sa.lpSecurityDescriptor = NULL; 


	memset(&ClientInfo, 0, sizeof(ClientInfo));
	ClientInfo.client_id = client_id;

	uResult = InitReadPipe(&ClientInfo.Stdout, &hPipeStdout, PTYPE_STDOUT);
	if (ERROR_SUCCESS != uResult) {
		free(pwszCommand);
		lprintf_err(uResult, "AddClient(): InitReadPipe(STDOUT)");
		return uResult;
	}
	uResult = InitReadPipe(&ClientInfo.Stderr, &hPipeStderr, PTYPE_STDERR);
	if (ERROR_SUCCESS != uResult) {
		CloseReadPipeHandles(client_id, &ClientInfo.Stdout);
		free(pwszCommand);
		lprintf_err(uResult, "AddClient(): InitReadPipe(STDERR)");
		return uResult;
	}


	if (!CreatePipe(&hPipeStdin, &ClientInfo.hWriteStdinPipe, &sa, 0)) {
		uResult = GetLastError();

		CloseReadPipeHandles(client_id, &ClientInfo.Stdout);
		CloseReadPipeHandles(client_id, &ClientInfo.Stderr);
		CloseHandle(hPipeStdout);
		CloseHandle(hPipeStderr);
		free(pwszCommand);

		lprintf_err(uResult, "AddClient(): CreatePipe(STDIN)");
		return uResult;
	}

	// Ensure the write handle to the pipe for STDIN is not inherited.
	SetHandleInformation(ClientInfo.hWriteStdinPipe, HANDLE_FLAG_INHERIT, 0);

#ifdef BUILD_AS_SERVICE
	uResult = CreatePipedProcessAsUserW(
			pwszUserName,
			L"userpass",
			pwszCommandLine,
			hPipeStdin,
			hPipeStdout,
			hPipeStderr,
			&ClientInfo.hProcess);
#else
	uResult = CreatePipedProcessAsCurrentUserW(
			pwszCommandLine,
			hPipeStdin,
			hPipeStdout,
			hPipeStderr,
			&ClientInfo.hProcess);
#endif
	free(pwszCommand);

	CloseHandle(hPipeStdout);
	CloseHandle(hPipeStderr);
	CloseHandle(hPipeStdin);

	if (ERROR_SUCCESS != uResult) {
		CloseHandle(ClientInfo.hWriteStdinPipe);

		CloseReadPipeHandles(client_id, &ClientInfo.Stdout);
		CloseReadPipeHandles(client_id, &ClientInfo.Stderr);
		lprintf_err(uResult, "AddClient(): CreatePipedProcessAsUserW()");
		return uResult;
	}

	g_Clients[uClientNumber] = ClientInfo;
	lprintf("AddClient(): New client %d (local id #%d)\n", client_id, uClientNumber);

	return ERROR_SUCCESS;
}


VOID RemoveClient(PCLIENT_INFO pClientInfo)
{
	if (!pClientInfo || (FREE_CLIENT_SPOT_ID == pClientInfo->client_id))
		return;

	CloseHandle(pClientInfo->hProcess);
	CloseHandle(pClientInfo->hWriteStdinPipe);

	CloseReadPipeHandles(pClientInfo->client_id, &pClientInfo->Stdout);
	CloseReadPipeHandles(pClientInfo->client_id, &pClientInfo->Stderr);

	lprintf("RemoveClient(): Client %d removed\n", pClientInfo->client_id);

	pClientInfo->client_id = FREE_CLIENT_SPOT_ID;
}

VOID RemoveAllClients()
{
	ULONG	uClientNumber;


	for (uClientNumber = 0; uClientNumber < MAX_CLIENTS; uClientNumber++)
		if (FREE_CLIENT_SPOT_ID != g_Clients[uClientNumber].client_id)
			RemoveClient(&g_Clients[uClientNumber]);
}

// This will return error only if vchan fails.
ULONG handle_exec(int client_id, int len)
{
	char *buf;
	ULONG	uResult;


	buf = malloc(len + 1);
	if (!buf)
		return ERROR_SUCCESS;
	buf[len] = 0;


	if (read_all_vchan_ext(buf, len) <= 0) {
		free(buf);
		lprintf_err(ERROR_INVALID_FUNCTION, "handle_exec(): read_all_vchan_ext()");
		return ERROR_INVALID_FUNCTION;
	}

	uResult = AddClient(client_id, buf);
	if (ERROR_SUCCESS == uResult)
		lprintf("handle_exec(): Executed %s\n", buf);
	else {
		send_exit_code(client_id, MAKE_ERROR_RESPONSE(ERROR_SET_WINDOWS, uResult));
		lprintf_err(uResult, "handle_exec(): AddClient(\"%s\")", buf);
	}

	free(buf);
	return ERROR_SUCCESS;
}

// This will return error only if vchan fails.
ULONG handle_just_exec(int client_id, int len)
{
	char *buf;
	ULONG	uResult;


	buf = malloc(len + 1);
	if (!buf)
		return ERROR_SUCCESS;
	buf[len] = 0;


	if (read_all_vchan_ext(buf, len) <= 0) {
		free(buf);
		lprintf_err(ERROR_INVALID_FUNCTION, "handle_just_exec(): read_all_vchan_ext()");
		return ERROR_INVALID_FUNCTION;
	}

	uResult = AddClient(client_id, buf);
	if (ERROR_SUCCESS == uResult)
		lprintf("handle_just_exec(): Executed (nowait) %s\n", buf);
	else {
		send_exit_code(client_id, MAKE_ERROR_RESPONSE(ERROR_SET_WINDOWS, uResult));
		lprintf_err(uResult, "handle_just_exec(): AddClient(\"%s\")", buf);
	}

	free(buf);
	return ERROR_SUCCESS;
}

// This will return error only if vchan fails.
ULONG handle_input(int client_id, int len)
{
	char *buf;
	PCLIENT_INFO	pClientInfo;
	DWORD	dwWritten;


	pClientInfo = FindClientById(client_id);
	if (!pClientInfo)
		return ERROR_SUCCESS;

	if (!len) {
		RemoveClient(pClientInfo);
		return ERROR_SUCCESS;
	}

	buf = malloc(len + 1);
	if (!buf)
		return ERROR_SUCCESS;
	buf[len] = 0;

	if (read_all_vchan_ext(buf, len) <= 0) {
		free(buf);
		lprintf_err(ERROR_INVALID_FUNCTION, "handle_input(): read_all_vchan_ext()");
		return ERROR_INVALID_FUNCTION;
	}

	if (!WriteFile(pClientInfo->hWriteStdinPipe, buf, len, &dwWritten, NULL))
		lprintf_err(GetLastError(), "handle_input(): WriteFile()");


	free(buf);
	return ERROR_SUCCESS;
}

void set_blocked_outerr(int client_id, BOOLEAN bBlockOutput)
{
	PCLIENT_INFO	pClientInfo;


	pClientInfo = FindClientById(client_id);
	if (!pClientInfo)
		return;

	pClientInfo->bReadingIsDisabled = bBlockOutput;
}

ULONG handle_server_data()
{
	struct server_header s_hdr;
	ULONG	uResult;


	if (read_all_vchan_ext(&s_hdr, sizeof s_hdr) <= 0) {
		lprintf_err(ERROR_INVALID_FUNCTION, "handle_server_data(): read_all_vchan_ext()");
		return ERROR_INVALID_FUNCTION;
	}

//	lprintf("got %x %x %x\n", s_hdr.type, s_hdr.client_id, s_hdr.len);

	switch (s_hdr.type) {
	case MSG_XON:
		lprintf("MSG_XON\n");
		set_blocked_outerr(s_hdr.client_id, FALSE);
		break;
	case MSG_XOFF:
		lprintf("MSG_XOFF\n");
		set_blocked_outerr(s_hdr.client_id, TRUE);
		break;
	case MSG_SERVER_TO_AGENT_CONNECT_EXISTING:
		lprintf("MSG_SERVER_TO_AGENT_CONNECT_EXISTING\n");
//		handle_connect_existing(s_hdr.client_id, s_hdr.len);
		break;
	case MSG_SERVER_TO_AGENT_EXEC_CMDLINE:
		lprintf("MSG_SERVER_TO_AGENT_EXEC_CMDLINE\n");

		// This will return error only if vchan fails.
		uResult = handle_exec(s_hdr.client_id, s_hdr.len);
		if (ERROR_SUCCESS != uResult) {
			lprintf_err(uResult, "handle_server_data(): handle_exec()");
			return uResult;
		}		
		break;

	case MSG_SERVER_TO_AGENT_JUST_EXEC:
		lprintf("MSG_SERVER_TO_AGENT_JUST_EXEC\n");

		// This will return error only if vchan fails.
		uResult = handle_just_exec(s_hdr.client_id, s_hdr.len);
		if (ERROR_SUCCESS != uResult) {
			lprintf_err(uResult, "handle_server_data(): handle_just_exec()");
			return uResult;
		}
		break;

	case MSG_SERVER_TO_AGENT_INPUT:
		lprintf("MSG_SERVER_TO_AGENT_INPUT\n");

		// This will return error only if vchan fails.
		uResult = handle_input(s_hdr.client_id, s_hdr.len);
		if (ERROR_SUCCESS != uResult) {
			lprintf_err(uResult, "handle_server_data(): handle_input()");
			return uResult;
		}
		break;

	case MSG_SERVER_TO_AGENT_CLIENT_END:
		lprintf("MSG_SERVER_TO_AGENT_CLIENT_END\n");
		RemoveClient(FindClientById(s_hdr.client_id));
		break;
	default:
		lprintf("handle_server_data(): Msg type from daemon is %d ?\n",
			s_hdr.type);
		return ERROR_INVALID_FUNCTION;
	}

	return ERROR_SUCCESS;
}




ULONG FillAsyncIoData(ULONG uEventNumber, ULONG uClientNumber, UCHAR bHandleType, PIPE_DATA *pPipeData)
{
	ULONG	uResult;


	if (uEventNumber >= RTL_NUMBER_OF(g_WatchedEvents) || 
		uClientNumber >= RTL_NUMBER_OF(g_Clients) ||
		!pPipeData)
		return ERROR_INVALID_PARAMETER;


	uResult = ERROR_SUCCESS;

	if (!pPipeData->bReadInProgress && !pPipeData->bDataIsReady) {

		memset(&pPipeData->ReadBuffer, 0, READ_BUFFER_SIZE);

		if (!ReadFile(
			pPipeData->hReadPipe, 
			&pPipeData->ReadBuffer, 
			READ_BUFFER_SIZE, 
			NULL,
			&pPipeData->olRead)) {

			// Last error is usually ERROR_IO_PENDING here because of the asynchronous read.
			// But if the process has closed it would be ERROR_BROKEN_PIPE.
			uResult = GetLastError();
			if (ERROR_IO_PENDING == uResult)
				pPipeData->bReadInProgress = TRUE;

		} else {
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

	if (pPipeData->bReadInProgress || pPipeData->bDataIsReady) {

		g_HandlesInfo[uEventNumber].uClientNumber = uClientNumber;
		g_HandlesInfo[uEventNumber].bType = bHandleType;
		g_WatchedEvents[uEventNumber] = pPipeData->olRead.hEvent;
	}


	return uResult;
}




ULONG WatchForEvents()
{
	EVTCHN	evtchn;
	OVERLAPPED	ol;
	unsigned int fired_port;
	ULONG	i, uEventNumber, uClientNumber;
	DWORD	dwSignaledEvent;
	PCLIENT_INFO	pClientInfo;
	DWORD	dwExitCode;
	BOOLEAN	bVchanIoInProgress;
	ULONG	uResult;
	BOOLEAN	bVchanReturnedError;
	BOOLEAN	bVchanClientConnected;


	// This will not block.
	uResult = peer_server_init(REXEC_PORT);
	if (uResult) {
		lprintf_err(ERROR_INVALID_FUNCTION, "main(): peer_server_init()");
		return ERROR_INVALID_FUNCTION;
	}

	lprintf("main(): Awaiting for a vchan client\n");

	evtchn = libvchan_fd_for_select(ctrl);

	memset(&ol, 0, sizeof(ol));
	ol.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);


	for (uClientNumber = 0; uClientNumber < MAX_CLIENTS; uClientNumber++)
		g_Clients[uClientNumber].client_id = FREE_CLIENT_SPOT_ID;


	bVchanClientConnected = FALSE;
	bVchanIoInProgress = FALSE;
	bVchanReturnedError = FALSE;

	for (;;) {

		libvchan_prepare_to_select(ctrl);
		uEventNumber = 0;

		g_WatchedEvents[uEventNumber++] = g_hStopServiceEvent;

		uResult = ERROR_SUCCESS;
		if (!bVchanIoInProgress) {

			if (!ReadFile(evtchn, &fired_port, sizeof(fired_port), NULL, &ol)) {
				uResult = GetLastError();
				if (ERROR_IO_PENDING != uResult) {
					lprintf_err(uResult, "main(): Vchan async read");
					bVchanReturnedError = TRUE;
					break;
				}
			}

			bVchanIoInProgress = TRUE;
		}

		if (ERROR_SUCCESS == uResult || ERROR_IO_PENDING == uResult) {
			g_HandlesInfo[uEventNumber].uClientNumber = FREE_CLIENT_SPOT_ID;
			g_HandlesInfo[uEventNumber].bType = HTYPE_VCHAN;
			g_WatchedEvents[uEventNumber++] = ol.hEvent;
		}


		for (uClientNumber = 0; uClientNumber < MAX_CLIENTS; uClientNumber++) {

			if (FREE_CLIENT_SPOT_ID != g_Clients[uClientNumber].client_id) {

				g_HandlesInfo[uEventNumber].uClientNumber = uClientNumber;
				g_HandlesInfo[uEventNumber].bType = HTYPE_PROCESS;
				g_WatchedEvents[uEventNumber++] = g_Clients[uClientNumber].hProcess;

				if (!g_Clients[uClientNumber].bReadingIsDisabled) {
					// Skip those clients which have received MSG_XOFF.
					FillAsyncIoData(uEventNumber++, uClientNumber, HTYPE_STDOUT, &g_Clients[uClientNumber].Stdout);
					FillAsyncIoData(uEventNumber++, uClientNumber, HTYPE_STDERR, &g_Clients[uClientNumber].Stderr);
				}
			}
		}

		dwSignaledEvent = WaitForMultipleObjects(uEventNumber, g_WatchedEvents, FALSE, INFINITE);
		if (dwSignaledEvent < MAXIMUM_WAIT_OBJECTS) {

			if (0 == dwSignaledEvent)
				// g_hStopServiceEvent is signaled
				break;


//			lprintf("client %d, type %d, signaled: %d, en %d\n", g_HandlesInfo[dwSignaledEvent].uClientNumber, g_HandlesInfo[dwSignaledEvent].bType, dwSignaledEvent, uEventNumber);
			switch (g_HandlesInfo[dwSignaledEvent].bType) {
				case HTYPE_VCHAN:

					bVchanIoInProgress = FALSE;

					if (!bVchanClientConnected) {

						lprintf("main(): A vchan client has connected\n");

						// Remove the xenstore device/vchan/N entry.
						uResult = libvchan_server_handle_connected(ctrl);
						if (uResult) {
							lprintf_err(ERROR_INVALID_FUNCTION, "main(): libvchan_server_handle_connected()");
							bVchanReturnedError = TRUE;
							break;
						}

						bVchanClientConnected = TRUE;
						break;
					}

					if (libvchan_is_eof(ctrl)) {
						bVchanReturnedError = TRUE;
						break;
					}

					while (read_ready_vchan_ext()) {
						uResult = handle_server_data();
						if (ERROR_SUCCESS != uResult) {
							bVchanReturnedError = TRUE;
							lprintf_err(uResult, "main(): handle_server_data()");
							break;
						}
					}
					
					break;

				case HTYPE_STDOUT:
#ifdef DISPLAY_CONSOLE_OUTPUT
					printf("%s", &g_Clients[g_HandlesInfo[dwSignaledEvent].uClientNumber].Stdout.ReadBuffer);
#endif

					uResult = ReturnPipeData(
							g_Clients[g_HandlesInfo[dwSignaledEvent].uClientNumber].client_id,
							&g_Clients[g_HandlesInfo[dwSignaledEvent].uClientNumber].Stdout);
					if (ERROR_SUCCESS != uResult) {
						bVchanReturnedError = TRUE;
						lprintf_err(uResult, "main(): ReturnPipeData(STDOUT)");
					}
					break;

				case HTYPE_STDERR:
#ifdef DISPLAY_CONSOLE_OUTPUT
					printf("%s", &g_Clients[g_HandlesInfo[dwSignaledEvent].uClientNumber].Stderr.ReadBuffer);
#endif

					uResult = ReturnPipeData(
							g_Clients[g_HandlesInfo[dwSignaledEvent].uClientNumber].client_id,
							&g_Clients[g_HandlesInfo[dwSignaledEvent].uClientNumber].Stderr);
					if (ERROR_SUCCESS != uResult) {
						bVchanReturnedError = TRUE;
						lprintf_err(uResult, "main(): ReturnPipeData(STDERR)");
					}
					break;

				case HTYPE_PROCESS:

					pClientInfo = &g_Clients[g_HandlesInfo[dwSignaledEvent].uClientNumber];

					if (!GetExitCodeProcess(pClientInfo->hProcess, &dwExitCode)) {
						lprintf_err(GetLastError(), "main(): GetExitCodeProcess()");
						dwExitCode = ERROR_SUCCESS;
					}

					uResult = send_exit_code(pClientInfo->client_id, dwExitCode);
					if (ERROR_SUCCESS != uResult) {
						bVchanReturnedError = TRUE;
						lprintf_err(uResult, "main(): send_exit_code()");
					}

					RemoveClient(pClientInfo);
					break;
			}

		} else {
			lprintf_err(GetLastError(), "main(): WaitForMultipleObjects()");
			break;
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
	_tprintf(TEXT("\nqrexec agent service\n\nUsage: qrexec_agent <-i|-u>\n"));
}


ULONG CheckForXenInterface()
{
	EVTCHN	xc;


	xc = xc_evtchn_open();
	if (INVALID_HANDLE_VALUE == xc)
		return ERROR_NOT_SUPPORTED;

	xc_evtchn_close(xc);
	return ERROR_SUCCESS;
}



ULONG WINAPI ServiceExecutionThread(PVOID pParam)
{
	ULONG	uResult;



	lprintf("ServiceExecutionThread(): Service started\n");


	for (;;) {

		uResult = WatchForEvents();
		if (ERROR_SUCCESS != uResult)
			lprintf_err(uResult, "ServiceExecutionThread(): WatchForEvents()");

		if (!WaitForSingleObject(g_hStopServiceEvent, 0))
			break;

		Sleep(1000);
	}

	lprintf("ServiceExecutionThread(): Shutting down\n");

	return ERROR_SUCCESS;
}

#ifdef BUILD_AS_SERVICE

ULONG Init(HANDLE *phServiceThread)
{
	ULONG	uResult;
	HANDLE	hThread;



	*phServiceThread = INVALID_HANDLE_VALUE;

	uResult = CheckForXenInterface();
	if (ERROR_SUCCESS != uResult) {
		lprintf_err(uResult, "Init(): CheckForXenInterface()");
		ReportErrorToEventLog(XEN_INTERFACE_NOT_FOUND);
		return ERROR_NOT_SUPPORTED;
	}


	hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)ServiceExecutionThread, NULL, 0, NULL);
	if (!hThread) {
		uResult = GetLastError();
		lprintf_err(uResult, "StartServiceThread(): CreateThread()");
		return uResult;
	}

	*phServiceThread = hThread;

	return ERROR_SUCCESS;
}



// This is the entry point for a service module (BUILD_AS_SERVICE defined).
int __cdecl _tmain(ULONG argc, PTCHAR argv[])
{

	ULONG	uOption;
	PTCHAR	pszParam = NULL;
	TCHAR	szUserName[UNLEN + 1];
	TCHAR	szFullPath[MAX_PATH + 1];
	DWORD	nSize;
	ULONG	uResult;
	BOOL	bStop;
	TCHAR	bCommand;
	PTCHAR	pszAccountName = NULL;

	SERVICE_TABLE_ENTRY	ServiceTable[] = {
		{SERVICE_NAME, (LPSERVICE_MAIN_FUNCTION)ServiceMain},
		{NULL,NULL}
	};



	memset(szUserName, 0, sizeof(szUserName));
	nSize = RTL_NUMBER_OF(szUserName);
	if (!GetUserName(szUserName, &nSize)) {
		uResult = GetLastError();
		lprintf_err(uResult, "main(): GetUserName()");
		return uResult;
	}


	if ((1 == argc) && _tcscmp(szUserName, TEXT("SYSTEM"))) {
		Usage();
		return ERROR_INVALID_PARAMETER;
	}

	if (1 == argc) {

		lprintf("main(): Running as SYSTEM\n");

		uResult = ERROR_SUCCESS;
		if (!StartServiceCtrlDispatcher(ServiceTable)) {
			uResult = GetLastError();
			lprintf_err(uResult, "main(): StartServiceCtrlDispatcher()");
		}

		lprintf("main(): Exiting\n");
		return uResult;
	}

	memset(szFullPath, 0, sizeof(szFullPath));
	if (!GetModuleFileName(NULL, szFullPath, RTL_NUMBER_OF(szFullPath) - 1)) {
		uResult = GetLastError();
		lprintf_err(uResult, "main(): GetModuleFileName()");
		return uResult;
	}


	uResult = ERROR_SUCCESS;
	bStop = FALSE;
	bCommand = 0;

	while (!bStop) {

		uOption = GetOption(argc, argv, TEXT("iua:"), &pszParam);
		switch (uOption) {
		case 0:
			bStop = TRUE;
			break;

		case _T('i'):
		case _T('u'):
			if (bCommand) {
				bCommand = 0;
				bStop = TRUE;
			} else
				bCommand = (TCHAR)uOption;

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

	if (pszAccountName) {

		lprintf("main(): GrantDesktopAccess(\"%S\")\n", pszAccountName);
		uResult = GrantDesktopAccess(pszAccountName, NULL);
		if (ERROR_SUCCESS != uResult)
			lprintf_err(uResult, "main(): GrantDesktopAccess(\"%S\")", pszAccountName);

		return uResult;
	}

	switch (bCommand) {
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

ULONG Init(HANDLE *phServiceThread)
{
	return ERROR_SUCCESS;
}

BOOL WINAPI CtrlHandler(DWORD fdwCtrlType)
{
	lprintf("CtrlHandler(): Got shutdown signal\n");

	SetEvent(g_hStopServiceEvent);

	WaitForSingleObject(g_hCleanupFinishedEvent, 2000);

	CloseHandle(g_hStopServiceEvent);
	CloseHandle(g_hCleanupFinishedEvent);

	lprintf("CtrlHandler(): Shutdown complete\n");
	return TRUE;
}

// This is the entry point for a console application (BUILD_AS_SERVICE not defined).
int __cdecl _tmain(ULONG argc, PTCHAR argv[])
{
	ULONG	uResult;


	_tprintf(TEXT("\nqrexec agent console application\n\n"));

	if (ERROR_SUCCESS != CheckForXenInterface()) {
		lprintf("main(): Could not find Xen interface\n");
		return ERROR_NOT_SUPPORTED;
	}

	g_hStopServiceEvent = CreateEvent(0, TRUE, FALSE, 0);
	if (!g_hStopServiceEvent) {
		uResult = GetLastError();
		lprintf_err(uResult, "main(): CreateEvent()");
		return uResult;
	}

	g_hCleanupFinishedEvent = CreateEvent(0, TRUE, FALSE, 0);
	if (!g_hCleanupFinishedEvent) {
		uResult = GetLastError();
		CloseHandle(g_hStopServiceEvent);
		lprintf_err(uResult, "main(): CreateEvent()");
		return uResult;
	}


	SetConsoleCtrlHandler((PHANDLER_ROUTINE)CtrlHandler, TRUE);

	ServiceExecutionThread(NULL);
	SetEvent(g_hCleanupFinishedEvent);

	return ERROR_SUCCESS;
}
#endif