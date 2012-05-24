#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <tchar.h>
#include "qrexec.h"
#include "libvchan.h"
#include "glue.h"


CLIENT_INFO	g_Clients[MAX_CLIENTS];
HANDLE	g_WatchedEvents[MAXIMUM_WAIT_OBJECTS];
HANDLE_INFO	g_HandlesInfo[MAXIMUM_WAIT_OBJECTS];

ULONG64	g_uPipeId = 0;


//#define DISPLAY_CONSOLE_OUTPUT

ULONG ExecutePiped(PUCHAR pszCommand, HANDLE hPipeStdin, HANDLE hPipeStdout, HANDLE hPipeStderr, HANDLE *phProcess)
{
	PROCESS_INFORMATION	pi;
	STARTUPINFOA	si;
	ULONG	uResult;


	if (!pszCommand || !phProcess)
		return ERROR_INVALID_PARAMETER;

	*phProcess = INVALID_HANDLE_VALUE;

	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);

	si.dwFlags = STARTF_USESTDHANDLES;

	si.hStdInput = hPipeStdin;
	si.hStdOutput = hPipeStdout;
	si.hStdError = hPipeStderr;

	if (!CreateProcessA(
			NULL, 
			pszCommand, 
			NULL, 
			NULL, 
			TRUE, // handles are inherited
			0, 
			NULL, 
			NULL, 
			&si, 
			&pi)) {

		uResult = GetLastError();
		fprintf(stderr, "ExecutePiped(): CreateProcessA(\"%s\") failed with error %d\n", pszCommand, uResult);
		return uResult;
	}

	fprintf(stderr, "ExecutePiped(): pid %d\n", pi.dwProcessId);

	*phProcess = pi.hProcess;
	CloseHandle(pi.hThread);

	return ERROR_SUCCESS;
}



ULONG CreateAsyncPipe(HANDLE *phReadPipe, HANDLE *phWritePipe, SECURITY_ATTRIBUTES *pSecurityAttributes)
{
	TCHAR	szPipeName[MAX_PATH];
	HANDLE	hReadPipe;
	HANDLE	hWritePipe;
	ULONG	uResult;


	if (!phReadPipe || !phWritePipe)
		return ERROR_INVALID_PARAMETER;

	_stprintf(szPipeName, TEXT("\\\\.\\pipe\\qrexec.%08x.%I64x"), GetCurrentProcessId(), g_uPipeId++);

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
		fprintf(stderr, "CreateAsyncPipe(): CreateNamedPipe() failed with error %d\n", uResult);
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
		fprintf(stderr, "CreateAsyncPipe(): CreateFile() failed with error %d\n", uResult);
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
		fprintf(stderr, "InitReadPipe(): CreateAsyncPipe() returned %d\n", uResult);
		return uResult;
	}

	// Ensure the read handle to the pipe is not inherited.
	SetHandleInformation(pPipeData->hReadPipe, HANDLE_FLAG_INHERIT, 0);

	pPipeData->bPipeType = bPipeType;

	return ERROR_SUCCESS;
}


VOID ReturnData(int client_id, int type, PVOID pData, ULONG uDataSize)
{
	struct server_header s_hdr;


	s_hdr.type = type;
	s_hdr.client_id = client_id;
	s_hdr.len = uDataSize;
	write_all_vchan_ext(&s_hdr, sizeof s_hdr);
	write_all_vchan_ext(pData, uDataSize);
}


void send_exit_code(int client_id, int status)
{
	ReturnData(client_id, MSG_AGENT_TO_SERVER_EXIT_CODE, &status, sizeof(status));
	fprintf(stderr, "send_exit_code(): Send exit code %d for client_id %d\n",
		status,
		client_id);
}

PCLIENT_INFO FindClientById(int client_id)
{
	ULONG	uClientNumber;


	for (uClientNumber = 0; uClientNumber < MAX_CLIENTS; uClientNumber++)
		if (client_id == g_Clients[uClientNumber].client_id)
			return &g_Clients[uClientNumber];

	return NULL;
}


VOID ReturnPipeData(int client_id, PIPE_DATA *pPipeData)
{
	DWORD	dwRead;
	int	message_type;
	PCLIENT_INFO	pClientInfo;


	if (!pPipeData)
		return;

	pClientInfo = FindClientById(client_id);
	if (!pClientInfo)
		return;

	if (pClientInfo->bReadingIsDisabled)
		// The client does not want to receive any data from this console.
		return;

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
		return;
	}


	dwRead = 0;
	GetOverlappedResult(pPipeData->hReadPipe, &pPipeData->olRead, &dwRead, FALSE);

	if (dwRead)
		ReturnData(client_id, message_type, pPipeData->ReadBuffer, dwRead);
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
				fprintf(stderr, "CloseReadPipeHandles(): CancelIo() failed with error %d\n", uResult);
			}
		}

		CloseHandle(pPipeData->olRead.hEvent);
	}

	if (pPipeData->hReadPipe)
		// Can close the pipe only when there is no pending IO in progress.
		CloseHandle(pPipeData->hReadPipe);

	return uResult;
}



ULONG AddClient(int client_id, PUCHAR pszCommand)
{
	ULONG	uResult;
	CLIENT_INFO	ClientInfo;
	HANDLE	hPipeStdout = INVALID_HANDLE_VALUE;
	HANDLE	hPipeStderr = INVALID_HANDLE_VALUE;
	HANDLE	hPipeStdin = INVALID_HANDLE_VALUE;
	ULONG	uClientNumber;
	SECURITY_ATTRIBUTES	sa;


	if (!pszCommand)
		return ERROR_INVALID_PARAMETER;


	for (uClientNumber = 0; uClientNumber < MAX_CLIENTS; uClientNumber++)
		if (FREE_CLIENT_SPOT_ID == g_Clients[uClientNumber].client_id)
			break;

	if (MAX_CLIENTS == uClientNumber)
		// There is no space for watching for another process
		return ERROR_TOO_MANY_CMDS;

	if (FindClientById(client_id))
		return ERROR_ALREADY_EXISTS;


	memset(&sa, 0, sizeof(sa));
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE; 
	sa.lpSecurityDescriptor = NULL; 


	memset(&ClientInfo, 0, sizeof(ClientInfo));
	ClientInfo.client_id = client_id;

	uResult = InitReadPipe(&ClientInfo.Stdout, &hPipeStdout, PTYPE_STDOUT);
	if (ERROR_SUCCESS != uResult) {
		fprintf(stderr, "AddClient(): InitReadPipe(STDOUT) failed with error %d\n", uResult);
		return uResult;
	}
	uResult = InitReadPipe(&ClientInfo.Stderr, &hPipeStderr, PTYPE_STDERR);
	if (ERROR_SUCCESS != uResult) {
		CloseReadPipeHandles(client_id, &ClientInfo.Stdout);
		fprintf(stderr, "AddClient(): InitReadPipe(STDERR) failed with error %d\n", uResult);
		return uResult;
	}


	if (!CreatePipe(&hPipeStdin, &ClientInfo.hWriteStdinPipe, &sa, 0)) {
		uResult = GetLastError();

		CloseReadPipeHandles(client_id, &ClientInfo.Stdout);
		CloseReadPipeHandles(client_id, &ClientInfo.Stderr);
		CloseHandle(hPipeStdout);
		CloseHandle(hPipeStderr);

		fprintf(stderr, "AddClient(): CreatePipe(STDIN) failed with error %d\n", uResult);
		return uResult;
	}

	// Ensure the write handle to the pipe for STDIN is not inherited.
	SetHandleInformation(ClientInfo.hWriteStdinPipe, HANDLE_FLAG_INHERIT, 0);

	uResult = ExecutePiped(pszCommand, hPipeStdin, hPipeStdout, hPipeStderr, &ClientInfo.hProcess);

	CloseHandle(hPipeStdout);
	CloseHandle(hPipeStderr);
	CloseHandle(hPipeStdin);

	if (ERROR_SUCCESS != uResult) {
		CloseHandle(ClientInfo.hWriteStdinPipe);

		CloseReadPipeHandles(client_id, &ClientInfo.Stdout);
		CloseReadPipeHandles(client_id, &ClientInfo.Stderr);
		fprintf(stderr, "AddClient(): ExecutePiped() failed with error %d\n", uResult);
		return uResult;
	}

	g_Clients[uClientNumber] = ClientInfo;
	fprintf(stderr, "AddClient(): New client %d (local id #%d)\n", client_id, uClientNumber);

	return ERROR_SUCCESS;
}


VOID RemoveClient(PCLIENT_INFO pClientInfo)
{
	if (!pClientInfo)
		return;

	CloseHandle(pClientInfo->hProcess);
	CloseHandle(pClientInfo->hWriteStdinPipe);

	CloseReadPipeHandles(pClientInfo->client_id, &pClientInfo->Stdout);
	CloseReadPipeHandles(pClientInfo->client_id, &pClientInfo->Stderr);

	fprintf(stderr, "RemoveClient(): Client %d removed\n", pClientInfo->client_id);

	pClientInfo->client_id = FREE_CLIENT_SPOT_ID;
}


void handle_exec(int client_id, int len)
{
	char *buf;
	ULONG	uResult;


	buf = malloc(len + 1);
	if (!buf)
		return;
	buf[len] = 0;


	read_all_vchan_ext(buf, len);

	uResult = AddClient(client_id, buf);
	if (ERROR_SUCCESS == uResult)
		fprintf(stderr, "handle_exec(): Executed %s\n", buf);
	else
		fprintf(stderr, "handle_exec(): AddClient(\"%s\") failed with error %d\n", buf, uResult);

	free(buf);
}

void handle_just_exec(int client_id, int len)
{
	char *buf;
	ULONG	uResult;


	buf = malloc(len + 1);
	if (!buf)
		return;
	buf[len] = 0;


	read_all_vchan_ext(buf, len);

	uResult = AddClient(client_id, buf);
	if (ERROR_SUCCESS == uResult)
		fprintf(stderr, "handle_just_exec(): Executed (nowait) %s\n", buf);
	else
		fprintf(stderr, "handle_just_exec(): AddClient(\"%s\") failed with error %d\n", buf, uResult);

	free(buf);
}


void handle_input(int client_id, int len)
{
	char *buf;
	PCLIENT_INFO	pClientInfo;
	DWORD	dwWritten;


	pClientInfo = FindClientById(client_id);
	if (!pClientInfo)
		return;

	if (!len) {
		RemoveClient(pClientInfo);
		return;
	}

	buf = malloc(len + 1);
	if (!buf)
		return;
	buf[len] = 0;

	read_all_vchan_ext(buf, len);

	if (!WriteFile(pClientInfo->hWriteStdinPipe, buf, len, &dwWritten, NULL))
		fprintf(stderr, "handle_input(): WriteFile() failed with error %d\n", GetLastError());


	free(buf);
}

void set_blocked_outerr(int client_id, BOOLEAN bBlockOutput)
{
	PCLIENT_INFO	pClientInfo;


	pClientInfo = FindClientById(client_id);
	if (!pClientInfo)
		return;

	pClientInfo->bReadingIsDisabled = bBlockOutput;
}

void handle_server_data()
{
	struct server_header s_hdr;


	read_all_vchan_ext(&s_hdr, sizeof s_hdr);
//	fprintf(stderr, "got %x %x %x\n", s_hdr.type, s_hdr.client_id, s_hdr.len);

	switch (s_hdr.type) {
	case MSG_XON:
		fprintf(stderr, "MSG_XON\n");
		set_blocked_outerr(s_hdr.client_id, FALSE);
		break;
	case MSG_XOFF:
		fprintf(stderr, "MSG_XOFF\n");
		set_blocked_outerr(s_hdr.client_id, TRUE);
		break;
	case MSG_SERVER_TO_AGENT_CONNECT_EXISTING:
		fprintf(stderr, "MSG_SERVER_TO_AGENT_CONNECT_EXISTING\n");
//		handle_connect_existing(s_hdr.client_id, s_hdr.len);
		break;
	case MSG_SERVER_TO_AGENT_EXEC_CMDLINE:
		fprintf(stderr, "MSG_SERVER_TO_AGENT_EXEC_CMDLINE\n");
		handle_exec(s_hdr.client_id, s_hdr.len);
		break;
	case MSG_SERVER_TO_AGENT_JUST_EXEC:
		fprintf(stderr, "MSG_SERVER_TO_AGENT_JUST_EXEC\n");
		handle_just_exec(s_hdr.client_id, s_hdr.len);
		break;
	case MSG_SERVER_TO_AGENT_INPUT:
		fprintf(stderr, "MSG_SERVER_TO_AGENT_INPUT\n");
		handle_input(s_hdr.client_id, s_hdr.len);
		break;
	case MSG_SERVER_TO_AGENT_CLIENT_END:
		fprintf(stderr, "MSG_SERVER_TO_AGENT_CLIENT_END\n");
		RemoveClient(FindClientById(s_hdr.client_id));
		break;
	default:
		fprintf(stderr, "msg type from daemon is %d ?\n",
			s_hdr.type);
		exit(1);
	}
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




VOID __cdecl main()
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

	peer_server_init(REXEC_PORT);
	evtchn = libvchan_fd_for_select(ctrl);

	memset(&ol, 0, sizeof(ol));
	ol.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);


	for (uClientNumber = 0; uClientNumber < MAX_CLIENTS; uClientNumber++)
		g_Clients[uClientNumber].client_id = FREE_CLIENT_SPOT_ID;



	bVchanIoInProgress = FALSE;

	for (;;) {

		libvchan_prepare_to_select(ctrl);
		uEventNumber = 0;

		uResult = ERROR_SUCCESS;
		if (!bVchanIoInProgress) {

			if (!ReadFile(evtchn, &fired_port, sizeof(fired_port), NULL, &ol)) {
				uResult = GetLastError();
				if (ERROR_IO_PENDING != uResult) {
					fprintf(stderr, "Vchan async read failed, last error: %d\n", GetLastError());
//					CloseHandle(ol.hEvent);
//					return -1;
				}
			}

			bVchanIoInProgress = TRUE;
		}

		if (ERROR_SUCCESS == uResult || ERROR_IO_PENDING == uResult)
			g_WatchedEvents[uEventNumber++] = ol.hEvent;


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

			if (0 == dwSignaledEvent) {
				// vchan overlapped io has finished
				if (libvchan_is_eof(ctrl))
					break;

				while (read_ready_vchan_ext())
					handle_server_data();

				bVchanIoInProgress = FALSE;
				continue;
			}


//			fprintf(stderr, "client %d, type %d, signaled: %d, en %d\n", g_HandlesInfo[dwSignaledEvent].uClientNumber, g_HandlesInfo[dwSignaledEvent].bType, dwSignaledEvent, uEventNumber);
			switch (g_HandlesInfo[dwSignaledEvent].bType) {
				case HTYPE_STDOUT:
#ifdef DISPLAY_CONSOLE_OUTPUT
					printf("%s", &g_Clients[g_HandlesInfo[dwSignaledEvent].uClientNumber].Stdout.ReadBuffer);
#endif

					ReturnPipeData(
						g_Clients[g_HandlesInfo[dwSignaledEvent].uClientNumber].client_id,
						&g_Clients[g_HandlesInfo[dwSignaledEvent].uClientNumber].Stdout);
					break;

				case HTYPE_STDERR:
#ifdef DISPLAY_CONSOLE_OUTPUT
					printf("%s", &g_Clients[g_HandlesInfo[dwSignaledEvent].uClientNumber].Stderr.ReadBuffer);
#endif

					ReturnPipeData(
						g_Clients[g_HandlesInfo[dwSignaledEvent].uClientNumber].client_id,
						&g_Clients[g_HandlesInfo[dwSignaledEvent].uClientNumber].Stderr);
					break;

				case HTYPE_PROCESS:

					pClientInfo = &g_Clients[g_HandlesInfo[dwSignaledEvent].uClientNumber];

					dwExitCode = ERROR_SUCCESS;
					GetExitCodeProcess(pClientInfo->hProcess, &dwExitCode);
					send_exit_code(pClientInfo->client_id, dwExitCode);

					RemoveClient(pClientInfo);
					break;
			}
		} else {
			fprintf(stderr, "WaitForMultipleObjects() failed, last error %d\n", GetLastError());
			break;
		}

	}


	libvchan_close(ctrl);
	CloseHandle(ol.hEvent);


	return;
}