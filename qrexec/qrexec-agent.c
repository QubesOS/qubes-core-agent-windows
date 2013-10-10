#include "qrexec-agent.h"
#include <utf8-conv.h>


HANDLE	g_hAddExistingClientEvent;

CLIENT_INFO	g_Clients[MAX_CLIENTS];
HANDLE	g_WatchedEvents[MAXIMUM_WAIT_OBJECTS];
HANDLE_INFO	g_HandlesInfo[MAXIMUM_WAIT_OBJECTS];

ULONG64	g_uPipeId = 0;

CRITICAL_SECTION	g_ClientsCriticalSection;
CRITICAL_SECTION	g_VchanCriticalSection;


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

#ifdef BACKEND_VMM_wni
	DWORD user_name_len = UNLEN + 1;
	TCHAR user_name[user_name_len];
#endif

	if (!phReadPipe || !phWritePipe)
		return ERROR_INVALID_PARAMETER;

#ifdef BACKEND_VMM_wni
    /* on WNI we don't have separate namespace for each VM (all is in the
     * single system) */

    if (!GetUserName(user_name, &user_name_len)) {
		uResult = GetLastError();
        perror("GetUserName");
        return uResult;
    }
	StringCchPrintf(szPipeName, MAX_PATH, TEXT("\\\\.\\pipe\\%s\\qrexec.%08x.%I64x"), user_name, GetCurrentProcessId(), g_uPipeId++);
#else
	StringCchPrintf(szPipeName, MAX_PATH, TEXT("\\\\.\\pipe\\qrexec.%08x.%I64x"), GetCurrentProcessId(), g_uPipeId++);
#endif

	hReadPipe = CreateNamedPipe(
			szPipeName,
			PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
			PIPE_TYPE_BYTE,
			1,
			4096,
			4096,
			50,	// the default timeout is 50ms
			pSecurityAttributes);
	if (!hReadPipe) {
		uResult = GetLastError();
		perror("CreateAsyncPipe(): CreateNamedPipe()");
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
		perror("CreateAsyncPipe(): CreateFile()");
		CloseHandle(hReadPipe);
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
		perror("InitReadPipe(): CreateAsyncPipe()");
		CloseHandle(pPipeData->olRead.hEvent);
		return uResult;
	}

	// Ensure the read handle to the pipe is not inherited.
	SetHandleInformation(pPipeData->hReadPipe, HANDLE_FLAG_INHERIT, 0);

	pPipeData->bPipeType = bPipeType;

	return ERROR_SUCCESS;
}

ULONG ReturnData(int client_id, int type, PVOID pData, ULONG uDataSize, PULONG puDataWritten)
{
	struct server_header s_hdr;
	unsigned int vchan_space_avail;
	ULONG uResult = ERROR_SUCCESS;

	debugf("ReturnData(%d), size %d\n", client_id, uDataSize);

	EnterCriticalSection(&g_VchanCriticalSection);

	if (puDataWritten) {
		// allow partial write only when puDataWritten given
		*puDataWritten = 0;
		vchan_space_avail = buffer_space_vchan_ext();
		if (vchan_space_avail < sizeof(s_hdr)) {
			LeaveCriticalSection(&g_VchanCriticalSection);
			return ERROR_INSUFFICIENT_BUFFER;
		}
		// inhibit zero-length write when not requested
		if (uDataSize && vchan_space_avail == sizeof(s_hdr)) {
			LeaveCriticalSection(&g_VchanCriticalSection);
			return ERROR_INSUFFICIENT_BUFFER;
		}

		if (vchan_space_avail < sizeof(s_hdr)+uDataSize) {
			uResult = ERROR_INSUFFICIENT_BUFFER;
			uDataSize = vchan_space_avail - sizeof(s_hdr);
		}

		*puDataWritten = uDataSize;
	}

	s_hdr.type = type;
	s_hdr.client_id = client_id;
	s_hdr.len = uDataSize;
	if (write_all_vchan_ext(&s_hdr, sizeof s_hdr) <= 0) {
		perror("ReturnData(): write_all_vchan_ext(s_hdr)");
		LeaveCriticalSection(&g_VchanCriticalSection);
		return ERROR_INVALID_FUNCTION;
	}

	if (!uDataSize) {
		LeaveCriticalSection(&g_VchanCriticalSection);
		return ERROR_SUCCESS;
	}

	if (write_all_vchan_ext(pData, uDataSize) <= 0) {
		perror("ReturnData(): write_all_vchan_ext()");
		LeaveCriticalSection(&g_VchanCriticalSection);
		return ERROR_INVALID_FUNCTION;
	}

	LeaveCriticalSection(&g_VchanCriticalSection);
	return uResult;
}

ULONG send_exit_code(int client_id, int status)
{
	ULONG	uResult;

	uResult = ReturnData(client_id, MSG_AGENT_TO_SERVER_EXIT_CODE, &status, sizeof(status), NULL);
	if (ERROR_SUCCESS != uResult) {
		perror("send_exit_code(): ReturnData()");
		return uResult;
	} else
		debugf("send_exit_code(): Send exit code %d for client_id %d\n", status, client_id);

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
	ULONG	uDataSent;

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
	if (!GetOverlappedResult(pPipeData->hReadPipe, &pPipeData->olRead, &dwRead, FALSE)) {
		perror("ReturnPipeData(): GetOverlappedResult");
		logf("ReturnPipeData(): GetOverlappedResult, client %d, dwRead %d\n", client_id, dwRead);
	}

	uResult = ReturnData(client_id, message_type, pPipeData->ReadBuffer+pPipeData->dwSentBytes, dwRead-pPipeData->dwSentBytes, &uDataSent);
	if (ERROR_INSUFFICIENT_BUFFER == uResult) {
		pPipeData->dwSentBytes += uDataSent;
		pPipeData->bVchanWritePending = TRUE;
		return uResult;
	} else if (ERROR_SUCCESS != uResult)
		perror("ReturnPipeData(): ReturnData()");

	pPipeData->bVchanWritePending = FALSE;

	if (!dwRead) {
		pPipeData->bPipeClosed = TRUE;
		uResult = ERROR_HANDLE_EOF;
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
			// asynchronous read has been issued.
			if (CancelIo(pPipeData->hReadPipe)) {

				// Must wait for the canceled IO to complete, otherwise a race condition may occur on the
				// OVERLAPPED structure.
				WaitForSingleObject(pPipeData->olRead.hEvent, INFINITE);

				// See if there is something to return.
				ReturnPipeData(client_id, pPipeData);

			} else {
				uResult = GetLastError();
				perror("CloseReadPipeHandles(): CancelIo()");
			}
		}

		CloseHandle(pPipeData->olRead.hEvent);
	}

	if (pPipeData->hReadPipe)
		// Can close the pipe only when there is no pending IO in progress.
		CloseHandle(pPipeData->hReadPipe);

	return uResult;
}

ULONG TextBOMToUTF16(char *pszBuf, size_t cbBufLen, PWCHAR *ppwszUtf16)
{
	size_t cbSkipChars = 0;
	PWCHAR  pwszUtf16 = NULL;
	ULONG	uResult;
	HRESULT	hResult;

	if (!pszBuf || !cbBufLen || !ppwszUtf16)
		return ERROR_INVALID_PARAMETER;

	*ppwszUtf16 = NULL;

	// see http://en.wikipedia.org/wiki/Byte-order_mark for explaination of the BOM
	// encoding
	if(cbBufLen >= 3 && pszBuf[0] == 0xEF && pszBuf[1] == 0xBB && pszBuf[2] == 0xBF)
	{
		// UTF-8
		cbSkipChars = 3;
	}
	else if(cbBufLen >= 2 && pszBuf[0] == 0xFE && pszBuf[1] == 0xFF)
	{
		// UTF-16BE
		return ERROR_NOT_SUPPORTED;
	}
	else if(cbBufLen >= 2 && pszBuf[0] == 0xFF && pszBuf[1] == 0xFE)
	{
		// UTF-16LE
		cbSkipChars = 2;

		pwszUtf16 = malloc(cbBufLen - cbSkipChars + sizeof(WCHAR));
		if (!pwszUtf16)
			return ERROR_NOT_ENOUGH_MEMORY;

		hResult = StringCbCopyW(pwszUtf16, cbBufLen - cbSkipChars + sizeof(WCHAR), (wchar_t*)(pszBuf + cbSkipChars));
		if (FAILED(hResult)) {
			perror("TextBOMToUTF16(): StringCbCopyW()");
			free(pwszUtf16);
			return hResult;
		}

		*ppwszUtf16 = pwszUtf16;
		return ERROR_SUCCESS;
	}
	else if(cbBufLen >= 4 && pszBuf[0] == 0 && pszBuf[1] == 0 && pszBuf[2] == 0xFE && pszBuf[3] == 0xFF)
	{
		// UTF-32BE
		return ERROR_NOT_SUPPORTED;
	}
	else if(cbBufLen >= 4 && pszBuf[0] == 0xFF && pszBuf[1] == 0xFE && pszBuf[2] == 0 && pszBuf[3] == 0)
	{
		// UTF-32LE
		return ERROR_NOT_SUPPORTED;
	}

	// Try UTF-8
	uResult = ConvertUTF8ToUTF16(pszBuf + cbSkipChars, ppwszUtf16, NULL);
	if (ERROR_SUCCESS != uResult) {
		perror("TextBOMToUTF16(): UTF8ToUTF16()");
		return uResult;
	}

	return ERROR_SUCCESS;
}

ULONG ParseUtf8Command(char *pszUtf8Command, PWCHAR *ppwszCommand, PWCHAR *ppwszUserName, PWCHAR *ppwszCommandLine, PBOOLEAN pbRunInteractively)
{
	ULONG	uResult;
	PWCHAR	pwszCommand = NULL;
	PWCHAR	pwSeparator = NULL;
	PWCHAR	pwszUserName = NULL;

	if (!pszUtf8Command || !pbRunInteractively)
		return ERROR_INVALID_PARAMETER;

	*ppwszCommand = NULL;
	*ppwszUserName = NULL;
	*ppwszCommandLine = NULL;
	*pbRunInteractively = TRUE;

	pwszCommand = NULL;
	uResult = ConvertUTF8ToUTF16(pszUtf8Command, &pwszCommand, NULL);
	if (ERROR_SUCCESS != uResult) {
		perror("ParseUtf8Command(): UTF8ToUTF16()");
		return uResult;
	}

	pwszUserName = pwszCommand;
	pwSeparator = wcschr(pwszCommand, L':');
	if (!pwSeparator) {
		free(pwszCommand);
		logf("ParseUtf8Command(): Command line is supposed to be in user:[nogui:]command form\n");
		return ERROR_INVALID_PARAMETER;
	}

	*pwSeparator = L'\0';
	pwSeparator++;

	if (!wcsncmp(pwSeparator, L"nogui:", 6)) {
		pwSeparator = wcschr(pwSeparator, L':');
		if (!pwSeparator) {
			free(pwszCommand);
			logf("ParseUtf8Command(): Command line is supposed to be in user:[nogui:]command form\n");
			return ERROR_INVALID_PARAMETER;
		}

		*pwSeparator = L'\0';
		pwSeparator++;

		*pbRunInteractively = FALSE;
	}

	if (!wcscmp(pwszUserName, L"SYSTEM") || !wcscmp(pwszUserName, L"root")) {
		pwszUserName = NULL;
	}
	
	*ppwszCommand = pwszCommand;
	*ppwszUserName = pwszUserName;
	*ppwszCommandLine = pwSeparator;

	return ERROR_SUCCESS;
}

ULONG CreateClientPipes(CLIENT_INFO *pClientInfo, HANDLE *phPipeStdin, HANDLE *phPipeStdout, HANDLE *phPipeStderr)
{
	ULONG	uResult;
	SECURITY_ATTRIBUTES	sa;
	HANDLE	hPipeStdin = INVALID_HANDLE_VALUE;
	HANDLE	hPipeStdout = INVALID_HANDLE_VALUE;
	HANDLE	hPipeStderr = INVALID_HANDLE_VALUE;

	if (!pClientInfo || !phPipeStdin || !phPipeStdout || !phPipeStderr)
		return ERROR_INVALID_PARAMETER;

	memset(&sa, 0, sizeof(sa));
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE; 
	sa.lpSecurityDescriptor = NULL; 

	uResult = InitReadPipe(&pClientInfo->Stdout, &hPipeStdout, PTYPE_STDOUT);
	if (ERROR_SUCCESS != uResult) {
		perror("CreateClientPipes(): InitReadPipe(STDOUT)");
		return uResult;
	}

	uResult = InitReadPipe(&pClientInfo->Stderr, &hPipeStderr, PTYPE_STDERR);
	if (ERROR_SUCCESS != uResult) {
		perror("CreateClientPipes(): InitReadPipe(STDERR)");

		CloseHandle(pClientInfo->Stdout.hReadPipe);
		CloseHandle(hPipeStdout);

		return uResult;
	}

	if (!CreatePipe(&hPipeStdin, &pClientInfo->hWriteStdinPipe, &sa, 0)) {
		uResult = GetLastError();
		perror("CreateClientPipes(): CreatePipe(STDIN)");

		CloseHandle(pClientInfo->Stdout.hReadPipe);
		CloseHandle(pClientInfo->Stderr.hReadPipe);
		CloseHandle(hPipeStdout);
		CloseHandle(hPipeStderr);

		return uResult;
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
ULONG ReserveClientNumber(int client_id, PULONG puClientNumber)
{
	ULONG	uClientNumber;

	EnterCriticalSection(&g_ClientsCriticalSection);

	for (uClientNumber = 0; uClientNumber < MAX_CLIENTS; uClientNumber++)
		if (FREE_CLIENT_SPOT_ID == g_Clients[uClientNumber].client_id)
			break;

	if (MAX_CLIENTS == uClientNumber) {
		// There is no space for watching for another process
		LeaveCriticalSection(&g_ClientsCriticalSection);
		logf("ReserveClientNumber(): The maximum number of running processes (%d) has been reached\n", MAX_CLIENTS);
		return ERROR_TOO_MANY_CMDS;
	}

	if (FindClientById(client_id)) {
		LeaveCriticalSection(&g_ClientsCriticalSection);
		logf("ReserveClientNumber(): A client with the same id (#%d) already exists\n", client_id);
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

ULONG AddFilledClientInfo(ULONG uClientNumber, PCLIENT_INFO pClientInfo)
{
	if (!pClientInfo || uClientNumber >= MAX_CLIENTS)
		return ERROR_INVALID_PARAMETER;

	EnterCriticalSection(&g_ClientsCriticalSection);

	g_Clients[uClientNumber] = *pClientInfo;
	g_Clients[uClientNumber].bClientIsReady = TRUE;

	LeaveCriticalSection(&g_ClientsCriticalSection);

	return ERROR_SUCCESS;
}

ULONG AddClient(int client_id, PWCHAR pwszUserName, PWCHAR pwszCommandLine, BOOLEAN bRunInteractively)
{
	ULONG	uResult;
	CLIENT_INFO	ClientInfo;
	HANDLE	hPipeStdout = INVALID_HANDLE_VALUE;
	HANDLE	hPipeStderr = INVALID_HANDLE_VALUE;
	HANDLE	hPipeStdin = INVALID_HANDLE_VALUE;
	ULONG	uClientNumber;

	// if pwszUserName is NULL we run the process on behalf of the current user.
	if (!pwszCommandLine)
		return ERROR_INVALID_PARAMETER;

	uResult = ReserveClientNumber(client_id, &uClientNumber);
	if (ERROR_SUCCESS != uResult) {
		perror("AddClient(): ReserveClientNumber()");
		return uResult;
	}

	if (pwszUserName)
		debugf("AddClient(): Running \"%s\" as user \"%s\"\n", pwszCommandLine, pwszUserName);
	else {
#ifdef BUILD_AS_SERVICE
		debugf("AddClient(): Running \"%s\" as SYSTEM\n", pwszCommandLine);
#else
		debugf("AddClient(): Running \"%s\" as current user\n", pwszCommandLine);
#endif
	}

	memset(&ClientInfo, 0, sizeof(ClientInfo));
	ClientInfo.client_id = client_id;

	uResult = CreateClientPipes(&ClientInfo, &hPipeStdin, &hPipeStdout, &hPipeStderr);
	if (ERROR_SUCCESS != uResult) {
		perror("AddClient(): CreateClientPipes()");
		ReleaseClientNumber(uClientNumber);
		return uResult;
	}

#ifdef BUILD_AS_SERVICE
	if (pwszUserName)
		uResult = CreatePipedProcessAsUser(
				pwszUserName,
				DEFAULT_USER_PASSWORD_UNICODE,
				pwszCommandLine,
				bRunInteractively,
				hPipeStdin,
				hPipeStdout,
				hPipeStderr,
				&ClientInfo.hProcess);
	else
		uResult = CreatePipedProcessAsCurrentUser(
				pwszCommandLine,
				bRunInteractively,
				hPipeStdin,
				hPipeStdout,
				hPipeStderr,
				&ClientInfo.hProcess);
#else
	uResult = CreatePipedProcessAsCurrentUser(
			pwszCommandLine,
			hPipeStdin,
			hPipeStdout,
			hPipeStderr,
			&ClientInfo.hProcess);
#endif

	CloseHandle(hPipeStdout);
	CloseHandle(hPipeStderr);
	CloseHandle(hPipeStdin);

	if (ERROR_SUCCESS != uResult) {
		ReleaseClientNumber(uClientNumber);

		CloseHandle(ClientInfo.hWriteStdinPipe);
		CloseHandle(ClientInfo.Stdout.hReadPipe);
		CloseHandle(ClientInfo.Stderr.hReadPipe);

#ifdef BUILD_AS_SERVICE
		if (pwszUserName)
			perror("AddClient(): CreatePipedProcessAsUser()");
		else
			perror("AddClient(): CreatePipedProcessAsCurrentUser()");
#else
		perror("AddClient(): CreatePipedProcessAsCurrentUser()");
#endif
		return uResult;
	}

	uResult = AddFilledClientInfo(uClientNumber, &ClientInfo);
	if (ERROR_SUCCESS != uResult) {
		perror("AddClient(): AddFilledClientInfo()");
		ReleaseClientNumber(uClientNumber);

		CloseHandle(ClientInfo.hWriteStdinPipe);
		CloseHandle(ClientInfo.Stdout.hReadPipe);
		CloseHandle(ClientInfo.Stderr.hReadPipe);
		CloseHandle(ClientInfo.hProcess);

		return uResult;
	}

	debugf("AddClient(): New client %d (local id #%d)\n", client_id, uClientNumber);

	return ERROR_SUCCESS;
}

ULONG AddExistingClient(int client_id, PCLIENT_INFO pClientInfo)
{
	ULONG	uClientNumber;
	ULONG	uResult;

	if (!pClientInfo)
		return ERROR_INVALID_PARAMETER;

	uResult = ReserveClientNumber(client_id, &uClientNumber);
	if (ERROR_SUCCESS != uResult) {
		perror("AddExistingClient(): ReserveClientNumber()");
		return uResult;
	}

	pClientInfo->client_id = client_id;

	uResult = AddFilledClientInfo(uClientNumber, pClientInfo);
	if (ERROR_SUCCESS != uResult) {
		perror("AddExistingClient(): AddFilledClientInfo()");
		ReleaseClientNumber(uClientNumber);
		return uResult;
	}

	debugf("AddExistingClient(): New client %d (local id #%d)\n", client_id, uClientNumber);

	SetEvent(g_hAddExistingClientEvent);

	return ERROR_SUCCESS;
}

VOID RemoveClientNoLocks(PCLIENT_INFO pClientInfo)
{
	if (!pClientInfo || (FREE_CLIENT_SPOT_ID == pClientInfo->client_id))
		return;

	CloseHandle(pClientInfo->hProcess);

	if (!pClientInfo->bStdinPipeClosed)
		CloseHandle(pClientInfo->hWriteStdinPipe);

	CloseReadPipeHandles(pClientInfo->client_id, &pClientInfo->Stdout);
	CloseReadPipeHandles(pClientInfo->client_id, &pClientInfo->Stderr);

	debugf("RemoveClientNoLocks(): Client %d removed\n", pClientInfo->client_id);

	pClientInfo->client_id = FREE_CLIENT_SPOT_ID;
	pClientInfo->bClientIsReady = FALSE;
}

VOID RemoveClient(PCLIENT_INFO pClientInfo)
{
	EnterCriticalSection(&g_ClientsCriticalSection);

	RemoveClientNoLocks(pClientInfo);

	LeaveCriticalSection(&g_ClientsCriticalSection);
}

VOID RemoveAllClients()
{
	ULONG	uClientNumber;

	EnterCriticalSection(&g_ClientsCriticalSection);

	for (uClientNumber = 0; uClientNumber < MAX_CLIENTS; uClientNumber++)
		if (FREE_CLIENT_SPOT_ID != g_Clients[uClientNumber].client_id)
			RemoveClientNoLocks(&g_Clients[uClientNumber]);

	LeaveCriticalSection(&g_ClientsCriticalSection);
}

// must be called with g_ClientsCriticalSection
ULONG PossiblyHandleTerminatedClientNoLocks(PCLIENT_INFO pClientInfo)
{
	ULONG uResult;

	if (pClientInfo->bChildExited && pClientInfo->Stdout.bPipeClosed && pClientInfo->Stderr.bPipeClosed) {
		uResult = send_exit_code(pClientInfo->client_id, pClientInfo->dwExitCode);
		// guaranted that all data was already sent (above bPipeClosed==TRUE)
		// so no worry about returning some data after exit code
		RemoveClientNoLocks(pClientInfo);
		return uResult;
	}
	return ERROR_SUCCESS;
}

ULONG PossiblyHandleTerminatedClient(PCLIENT_INFO pClientInfo)
{
	ULONG uResult;

	if (pClientInfo->bChildExited && pClientInfo->Stdout.bPipeClosed && pClientInfo->Stderr.bPipeClosed) {
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
ULONG InterceptRPCRequest(PWCHAR pwszCommandLine, PWCHAR *ppwszServiceCommandLine, PWCHAR *ppwszSourceDomainName)
{
	PWCHAR	pwszServiceName = NULL;
	PWCHAR	pwszSourceDomainName = NULL;
	PWCHAR	pwSeparator = NULL;
	char	szBuffer[sizeof(WCHAR) * (MAX_PATH + 1)];
	WCHAR	wszServiceFilePath[MAX_PATH + 1];
	PWCHAR	pwszRawServiceFilePath = NULL;
	PWCHAR  pwszServiceArgs = NULL;
	HANDLE	hServiceConfigFile;
	ULONG	uResult;
	ULONG	uBytesRead;
	ULONG	uPathLength;
	PWCHAR	pwszServiceCommandLine = NULL;

	if (!pwszCommandLine || !ppwszServiceCommandLine || !ppwszSourceDomainName)
		return ERROR_INVALID_PARAMETER;

	*ppwszServiceCommandLine = *ppwszSourceDomainName = NULL;

	if (wcsncmp(pwszCommandLine, RPC_REQUEST_COMMAND, wcslen(RPC_REQUEST_COMMAND))==0) {
		// RPC_REQUEST_COMMAND contains trailing space, so this must succeed
//#pragma prefast(suppress:28193, "RPC_REQUEST_COMMAND contains trailing space, so this must succeed")
		pwSeparator = wcschr(pwszCommandLine, L' ');
		pwSeparator++;
		pwszServiceName = pwSeparator;
		pwSeparator = wcschr(pwszServiceName, L' ');
		if (pwSeparator) {
			*pwSeparator = L'\0';
			pwSeparator++;
			pwszSourceDomainName = _wcsdup(pwSeparator);
			if (!pwszSourceDomainName) {
				perror("InterceptRPCRequest(): _wcsdup()");
				return ERROR_NOT_ENOUGH_MEMORY;
			}
		} else {
			logf("InterceptRPCRequest(): No source domain given\n");
			// Most qrexec services do not use source domain at all, so do not
			// abort if missing. This can be the case when RPC triggered
			// manualy using qvm-run (qvm-run -p vmname "QUBESRPC service_name").
		}

		// build RPC service config file path
		memset(wszServiceFilePath, 0, sizeof(wszServiceFilePath));
		if (!GetModuleFileNameW(NULL, wszServiceFilePath, MAX_PATH)) {
			uResult = GetLastError();
			perror("InterceptRPCRequest(): GetModuleFileName()");
			if (pwszSourceDomainName)
				free(pwszSourceDomainName);
			return uResult;
		}
		// cut off file name (qrexec-agent.exe)
		pwSeparator = wcsrchr(wszServiceFilePath, L'\\');
		if (!pwSeparator) {
			if (pwszSourceDomainName)
				free(pwszSourceDomainName);
			logf("InterceptRPCRequest(): Cannot find dir containing qrexec-agent.exe\n");
			return ERROR_INVALID_PARAMETER;
		}
		*pwSeparator = L'\0';
		// cut off one dir (bin)
		pwSeparator = wcsrchr(wszServiceFilePath, L'\\');
		if (!pwSeparator) {
			if (pwszSourceDomainName)
				free(pwszSourceDomainName);
			logf("InterceptRPCRequest(): Cannot find dir containing bin\\qrexec-agent.exe\n");
			return ERROR_INVALID_PARAMETER;
		}
		// Leave trailing backslash
		pwSeparator++;
		*pwSeparator = L'\0';
		if (wcslen(wszServiceFilePath) + wcslen(L"qubes-rpc\\") + wcslen(pwszServiceName) > MAX_PATH) {
			if (pwszSourceDomainName)
				free(pwszSourceDomainName);
			logf("InterceptRPCRequest(): RPC service config file path too long\n");
			return ERROR_NOT_ENOUGH_MEMORY;
		}
		PathAppendW(wszServiceFilePath, L"qubes-rpc");
		PathAppendW(wszServiceFilePath, pwszServiceName);

		hServiceConfigFile = CreateFileW(wszServiceFilePath,               // file to open
				GENERIC_READ,          // open for reading
				FILE_SHARE_READ,       // share for reading
				NULL,                  // default security
				OPEN_EXISTING,         // existing file only
				FILE_ATTRIBUTE_NORMAL, // normal file
				NULL);                 // no attr. template

		if (hServiceConfigFile == INVALID_HANDLE_VALUE)
		{
			uResult = GetLastError();
			perror("CreateFileW");
			logf("InterceptRPCRequest(): Failed to open RPC %s configuration file (%s)\n", pwszServiceName, wszServiceFilePath);
			if (pwszSourceDomainName)
				free(pwszSourceDomainName);
			return uResult;
		}
		uBytesRead = 0;
		memset(szBuffer, 0, sizeof(szBuffer));
		if (!ReadFile(hServiceConfigFile, szBuffer, sizeof(WCHAR) * MAX_PATH, &uBytesRead, NULL)) {
			uResult = GetLastError();
			perror("ReadFile");
			logf("InterceptRPCRequest(): Failed to read RPC %s configuration file (%s)\n", pwszServiceName, wszServiceFilePath);
			if (pwszSourceDomainName)
				free(pwszSourceDomainName);
			CloseHandle(hServiceConfigFile);
			return uResult;
		}
		CloseHandle(hServiceConfigFile);

		uResult = TextBOMToUTF16(szBuffer, uBytesRead, &pwszRawServiceFilePath);
		if (uResult != ERROR_SUCCESS) {
			perror("TextBOMToUTF16");
			logf("InterceptRPCRequest(): Failed to parse the encoding in RPC %s configuration file (%s)\n", pwszServiceName, wszServiceFilePath);
			if (pwszSourceDomainName)
				free(pwszSourceDomainName);
			return uResult;
		}

		// strip white chars (especially end-of-line) from string
		uPathLength = wcslen(pwszRawServiceFilePath);
		while (iswspace(pwszRawServiceFilePath[uPathLength-1])) {
			uPathLength--;
			pwszRawServiceFilePath[uPathLength]=L'\0';
		}

		pwszServiceArgs = PathGetArgsW(pwszRawServiceFilePath);
		PathRemoveArgsW(pwszRawServiceFilePath);
		PathUnquoteSpacesW(pwszRawServiceFilePath);
		if (PathIsRelativeW(pwszRawServiceFilePath)) {
			// relative path are based in qubes-rpc-services
			// reuse separator found when preparing previous file path
			*pwSeparator = L'\0';
			PathAppendW(wszServiceFilePath, L"qubes-rpc-services");
			PathAppendW(wszServiceFilePath, pwszRawServiceFilePath);
		} else {
			StringCchCopyW(wszServiceFilePath, MAX_PATH + 1, pwszRawServiceFilePath);
		}
		PathQuoteSpacesW(wszServiceFilePath);
		if (pwszServiceArgs && pwszServiceArgs[0] != L'\0') {
			StringCchCatW(wszServiceFilePath, MAX_PATH + 1, L" ");
			StringCchCatW(wszServiceFilePath, MAX_PATH + 1, pwszServiceArgs);
		}
		free(pwszRawServiceFilePath);
		pwszServiceCommandLine = malloc((wcslen(wszServiceFilePath) + 1) * sizeof(WCHAR));
		if (pwszServiceCommandLine == NULL) {
			perror("InterceptRPCRequest(): malloc()");
			if (pwszSourceDomainName)
				free(pwszSourceDomainName);
			return ERROR_NOT_ENOUGH_MEMORY;
		}
		debugf("InterceptRPCRequest(): RPC %s: %s\n", pwszServiceName, wszServiceFilePath);
		StringCchCopyW(pwszServiceCommandLine, wcslen(wszServiceFilePath) + 1, wszServiceFilePath);

		*ppwszServiceCommandLine = pwszServiceCommandLine;
		*ppwszSourceDomainName = pwszSourceDomainName;
	}
	return ERROR_SUCCESS;
}

// This will return error only if vchan fails.
ULONG handle_connect_existing(int client_id, int len)
{
	ULONG	uResult;
	char *buf;

	if (!len)
		return ERROR_SUCCESS;

	buf = malloc(len + 1);
	if (!buf)
		return ERROR_SUCCESS;
	buf[len] = 0;

	if (read_all_vchan_ext(buf, len) <= 0) {
		perror("handle_connect_existing(): read_all_vchan_ext()");
		free(buf);
		return ERROR_INVALID_FUNCTION;
	}

	debugf("handle_connect_existing(): client %d, ident %S\n", client_id, buf);

	uResult = ProceedWithExecution(client_id, buf);
	free(buf);

	if (ERROR_SUCCESS != uResult)
		perror("handle_connect_existing(): ProceedWithExecution()");

	return ERROR_SUCCESS;
}

// This will return error only if vchan fails.
ULONG handle_exec(int client_id, int len)
{
	char *buf;
	ULONG	uResult;
	PWCHAR	pwszCommand = NULL;
	PWCHAR	pwszUserName = NULL;
	PWCHAR	pwszCommandLine = NULL;
	PWCHAR	pwszServiceCommandLine = NULL;
	PWCHAR	pwszRemoteDomainName = NULL;
	BOOLEAN	bRunInteractively;

	buf = malloc(len + 1);
	if (!buf)
		return ERROR_SUCCESS;
	buf[len] = 0;

	if (read_all_vchan_ext(buf, len) <= 0) {
		perror("handle_exec(): read_all_vchan_ext()");
		free(buf);
		return ERROR_INVALID_FUNCTION;
	}

	bRunInteractively = TRUE;

	uResult = ParseUtf8Command(buf, &pwszCommand, &pwszUserName, &pwszCommandLine, &bRunInteractively);
	if (ERROR_SUCCESS != uResult) {
		perror("handle_just_exec(): ParseUtf8Command()");
		free(buf);
		send_exit_code(client_id, MAKE_ERROR_RESPONSE(ERROR_SET_WINDOWS, uResult));
		return ERROR_SUCCESS;
	}

	free(buf);
	buf = NULL;

	uResult = InterceptRPCRequest(pwszCommandLine, &pwszServiceCommandLine, &pwszRemoteDomainName);
	if (ERROR_SUCCESS != uResult) {
		perror("handle_exec(): InterceptRPCRequest()");
		free(pwszCommand);
		send_exit_code(client_id, MAKE_ERROR_RESPONSE(ERROR_SET_WINDOWS, uResult));
		return ERROR_SUCCESS;
	}

	if (pwszServiceCommandLine)
		pwszCommandLine = pwszServiceCommandLine;

	if (pwszRemoteDomainName)
		SetEnvironmentVariableW(L"QREXEC_REMOTE_DOMAIN", pwszRemoteDomainName);

	// Create a process and redirect its console IO to vchan.
	uResult = AddClient(client_id, pwszUserName, pwszCommandLine, bRunInteractively);
	if (ERROR_SUCCESS == uResult)
		debugf("handle_exec(): Executed %s\n", pwszCommandLine);
	else {
		send_exit_code(client_id, MAKE_ERROR_RESPONSE(ERROR_SET_WINDOWS, uResult));
		perror("send_exit_code");
		logf("handle_exec(): AddClient(\"%s\")\n", pwszCommandLine);
	}

	if (pwszRemoteDomainName) {
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
	ULONG	uResult;
	PWCHAR	pwszCommand = NULL;
	PWCHAR	pwszUserName = NULL;
	PWCHAR	pwszCommandLine = NULL;
	PWCHAR	pwszServiceCommandLine = NULL;
	PWCHAR	pwszRemoteDomainName = NULL;
	HANDLE	hProcess;
	BOOLEAN	bRunInteractively;

	buf = malloc(len + 1);
	if (!buf)
		return ERROR_SUCCESS;
	buf[len] = 0;

	if (read_all_vchan_ext(buf, len) <= 0) {
		perror("handle_just_exec(): read_all_vchan_ext()");
		free(buf);
		return ERROR_INVALID_FUNCTION;
	}

	bRunInteractively = TRUE;

	uResult = ParseUtf8Command(buf, &pwszCommand, &pwszUserName, &pwszCommandLine, &bRunInteractively);
	if (ERROR_SUCCESS != uResult) {
		perror("handle_just_exec(): ParseUtf8Command()");
		free(buf);
		send_exit_code(client_id, MAKE_ERROR_RESPONSE(ERROR_SET_WINDOWS, uResult));
		return ERROR_SUCCESS;
	}

	free(buf);
	buf = NULL;

	uResult = InterceptRPCRequest(pwszCommandLine, &pwszServiceCommandLine, &pwszRemoteDomainName);
	if (ERROR_SUCCESS != uResult) {
		perror("handle_just_exec(): InterceptRPCRequest()");
		free(pwszCommand);
		send_exit_code(client_id, MAKE_ERROR_RESPONSE(ERROR_SET_WINDOWS, uResult));
		return ERROR_SUCCESS;
	}

	if (pwszServiceCommandLine)
		pwszCommandLine = pwszServiceCommandLine;

	if (pwszRemoteDomainName)
		SetEnvironmentVariableW(L"QREXEC_REMOTE_DOMAIN", pwszRemoteDomainName);


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

	if (ERROR_SUCCESS == uResult) {
		CloseHandle(hProcess);
		debugf("handle_just_exec(): Executed (nowait) %s\n", pwszCommandLine);
	} else {
#ifdef BUILD_AS_SERVICE
		perror("CreateNormalProcessAsUserW");
		logf("handle_just_exec(): CreateNormalProcessAsUserW(\"%s\") failed\n", pwszCommandLine);
#else
		perror("CreateNormalProcessAsCurrentUserW");
		logf("handle_just_exec(): CreateNormalProcessAsCurrentUserW(\"%s\") failed\n", pwszCommandLine);
#endif
		send_exit_code(client_id, MAKE_ERROR_RESPONSE(ERROR_SET_WINDOWS, uResult));
	}

	if (pwszRemoteDomainName) {
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
	PCLIENT_INFO	pClientInfo;
	DWORD	dwWritten;

	// If pClientInfo is NULL after this it means we couldn't find a specified client.
	// Read and discard any data in the channel in this case.
	pClientInfo = FindClientById(client_id);

	if (!len) {
		if (pClientInfo) {
			CloseHandle(pClientInfo->hWriteStdinPipe);
			pClientInfo->bStdinPipeClosed = TRUE;
		}
		return ERROR_SUCCESS;
	}

	buf = malloc(len + 1);
	if (!buf)
		return ERROR_SUCCESS;
	buf[len] = 0;

	if (read_all_vchan_ext(buf, len) <= 0) {
		perror("handle_input(): read_all_vchan_ext()");
		free(buf);
		return ERROR_INVALID_FUNCTION;
	}

	if (pClientInfo && !pClientInfo->bStdinPipeClosed) {
		if (!WriteFile(pClientInfo->hWriteStdinPipe, buf, len, &dwWritten, NULL))
			perror("handle_input(): WriteFile()");
	}

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
		perror("handle_server_data(): read_all_vchan_ext()");
		return ERROR_INVALID_FUNCTION;
	}

//	debugf("got %x %x %x\n", s_hdr.type, s_hdr.client_id, s_hdr.len);

	switch (s_hdr.type) {
	case MSG_XON:
		debugf("MSG_XON\n");
		set_blocked_outerr(s_hdr.client_id, FALSE);
		break;
	case MSG_XOFF:
		debugf("MSG_XOFF\n");
		set_blocked_outerr(s_hdr.client_id, TRUE);
		break;
	case MSG_SERVER_TO_AGENT_CONNECT_EXISTING:
		debugf("MSG_SERVER_TO_AGENT_CONNECT_EXISTING\n");
		handle_connect_existing(s_hdr.client_id, s_hdr.len);
		break;
	case MSG_SERVER_TO_AGENT_EXEC_CMDLINE:
		debugf("MSG_SERVER_TO_AGENT_EXEC_CMDLINE\n");

		// This will return error only if vchan fails.
		uResult = handle_exec(s_hdr.client_id, s_hdr.len);
		if (ERROR_SUCCESS != uResult) {
			perror("handle_server_data(): handle_exec()");
			return uResult;
		}		
		break;

	case MSG_SERVER_TO_AGENT_JUST_EXEC:
		debugf("MSG_SERVER_TO_AGENT_JUST_EXEC\n");

		// This will return error only if vchan fails.
		uResult = handle_just_exec(s_hdr.client_id, s_hdr.len);
		if (ERROR_SUCCESS != uResult) {
			perror("handle_server_data(): handle_just_exec()");
			return uResult;
		}
		break;

	case MSG_SERVER_TO_AGENT_INPUT:
		debugf("MSG_SERVER_TO_AGENT_INPUT\n");

		// This will return error only if vchan fails.
		uResult = handle_input(s_hdr.client_id, s_hdr.len);
		if (ERROR_SUCCESS != uResult) {
			perror("handle_server_data(): handle_input()");
			return uResult;
		}
		break;

	case MSG_SERVER_TO_AGENT_CLIENT_END:
		debugf("MSG_SERVER_TO_AGENT_CLIENT_END\n");
		RemoveClient(FindClientById(s_hdr.client_id));
		break;
	default:
		logf("handle_server_data(): Msg type from daemon is %d ?\n", s_hdr.type);
		return ERROR_INVALID_FUNCTION;
	}

	return ERROR_SUCCESS;
}

// returns number of filled events (0 or 1)
ULONG FillAsyncIoData(ULONG uEventNumber, ULONG uClientNumber, UCHAR bHandleType, PIPE_DATA *pPipeData)
{
	ULONG	uResult;

	if (uEventNumber >= RTL_NUMBER_OF(g_WatchedEvents) || 
		uClientNumber >= RTL_NUMBER_OF(g_Clients) ||
		!pPipeData)
		return 0;

	if (!pPipeData->bReadInProgress && !pPipeData->bDataIsReady && !pPipeData->bPipeClosed && !pPipeData->bVchanWritePending) {

		memset(&pPipeData->ReadBuffer, 0, READ_BUFFER_SIZE);
		pPipeData->dwSentBytes = 0;

		if (!ReadFile(
			pPipeData->hReadPipe, 
			&pPipeData->ReadBuffer, 
			READ_BUFFER_SIZE, 
			NULL,
			&pPipeData->olRead)) {

			// Last error is usually ERROR_IO_PENDING here because of the asynchronous read.
			// But if the process has closed it would be ERROR_BROKEN_PIPE,
			// in this case ReturnPipeData will send EOF notification and set bPipeClosed.
			uResult = GetLastError();
			if (ERROR_IO_PENDING == uResult)
				pPipeData->bReadInProgress = TRUE;
			if (ERROR_BROKEN_PIPE == uResult) {
				SetEvent(pPipeData->olRead.hEvent);
				pPipeData->bDataIsReady = TRUE;
			}

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

	// when bVchanWritePending==TRUE, ReturnPipeData already reset bReadInProgress and bDataIsReady
	if (pPipeData->bReadInProgress || pPipeData->bDataIsReady) {

		g_HandlesInfo[uEventNumber].uClientNumber = uClientNumber;
		g_HandlesInfo[uEventNumber].bType = bHandleType;
		g_WatchedEvents[uEventNumber] = pPipeData->olRead.hEvent;
		return 1;
	}

	return 0;
}

ULONG WatchForEvents()
{
	HANDLE	evtchn;
	ULONG	uEventNumber, uClientNumber;
	DWORD	dwSignaledEvent;
	PCLIENT_INFO	pClientInfo;
	DWORD	dwExitCode;
	ULONG	uResult;
	BOOLEAN	bVchanReturnedError;
	BOOLEAN	bVchanClientConnected;
	
	// This will not block.
	uResult = peer_server_init(VCHAN_PORT);
	if (uResult) {
		perror("WatchForEvents(): peer_server_init()");
		return ERROR_INVALID_FUNCTION;
	}

	debugf("WatchForEvents(): Awaiting for a vchan client, write ring size: %d\n", buffer_space_vchan_ext());

	bVchanClientConnected = FALSE;
	bVchanReturnedError = FALSE;

	for (;;) {
		uEventNumber = 0;

		// Order matters.
		g_WatchedEvents[uEventNumber++] = g_hStopServiceEvent;
		g_WatchedEvents[uEventNumber++] = g_hAddExistingClientEvent;

		g_HandlesInfo[0].bType = g_HandlesInfo[1].bType = HTYPE_INVALID;

		evtchn = libvchan_fd_for_select(ctrl);
		if (INVALID_HANDLE_VALUE == evtchn)
		{
			perror("WatchForEvents(): libvchan_fd_for_select");
			break;
		}

		g_HandlesInfo[uEventNumber].uClientNumber = FREE_CLIENT_SPOT_ID;
		g_HandlesInfo[uEventNumber].bType = HTYPE_VCHAN;
		g_WatchedEvents[uEventNumber++] = evtchn;
		
		EnterCriticalSection(&g_ClientsCriticalSection);

		for (uClientNumber = 0; uClientNumber < MAX_CLIENTS; uClientNumber++) {

			if (g_Clients[uClientNumber].bClientIsReady) {

				if (!g_Clients[uClientNumber].bChildExited) {
					g_HandlesInfo[uEventNumber].uClientNumber = uClientNumber;
					g_HandlesInfo[uEventNumber].bType = HTYPE_PROCESS;
					g_WatchedEvents[uEventNumber++] = g_Clients[uClientNumber].hProcess;
				}

				if (!g_Clients[uClientNumber].bReadingIsDisabled) {
					// Skip those clients which have received MSG_XOFF.
					uEventNumber += FillAsyncIoData(uEventNumber, uClientNumber, HTYPE_STDOUT, &g_Clients[uClientNumber].Stdout);
					uEventNumber += FillAsyncIoData(uEventNumber, uClientNumber, HTYPE_STDERR, &g_Clients[uClientNumber].Stderr);
				}
			}
		}
		LeaveCriticalSection(&g_ClientsCriticalSection);

		dwSignaledEvent = WaitForMultipleObjects(uEventNumber, g_WatchedEvents, FALSE, INFINITE);

		//debugf("signaled\n");

		if (dwSignaledEvent >= MAXIMUM_WAIT_OBJECTS) {
			debugf("WatchForEvents(): dwSignaledEvent >= MAXIMUM_WAIT_OBJECTS");

			uResult = GetLastError();
			if (ERROR_INVALID_HANDLE != uResult) {
				perror("WatchForEvents(): WaitForMultipleObjects()");
				break;
			}

			// WaitForMultipleObjects() may fail with ERROR_INVALID_HANDLE if the process which just has been added
			// to the client list terminated before WaitForMultipleObjects(). In this case IO pipe handles are closed
			// and invalidated, while a process handle is in the signaled state.
			// Check if any of the processes in the client list is terminated, remove it from the list and try again.

			EnterCriticalSection(&g_ClientsCriticalSection);

			for (uClientNumber = 0; uClientNumber < MAX_CLIENTS; uClientNumber++) {

				pClientInfo = &g_Clients[uClientNumber];

				if (!g_Clients[uClientNumber].bClientIsReady)
					continue;

				if (!GetExitCodeProcess(pClientInfo->hProcess, &dwExitCode)) {
					perror("WatchForEvents(): GetExitCodeProcess()");
					dwExitCode = ERROR_SUCCESS;
				}

				if (STILL_ACTIVE != dwExitCode) {
					pClientInfo->bChildExited = TRUE;
					pClientInfo->dwExitCode = dwExitCode;
					// send exit code only when all data was sent to the daemon
					uResult = PossiblyHandleTerminatedClientNoLocks(pClientInfo);
					if (ERROR_SUCCESS != uResult) {
						bVchanReturnedError = TRUE;
						perror("WatchForEvents(): send_exit_code()");
					}
				}
			}
			LeaveCriticalSection(&g_ClientsCriticalSection);

			continue;

		} else {
			if (0 == dwSignaledEvent)
				// g_hStopServiceEvent is signaled
				break;

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

			debugf("client %d, type %d, signaled: %d, en %d\n", g_HandlesInfo[dwSignaledEvent].uClientNumber, g_HandlesInfo[dwSignaledEvent].bType, dwSignaledEvent, uEventNumber);
			switch (g_HandlesInfo[dwSignaledEvent].bType) {
				case HTYPE_VCHAN:

					// the following will never block; we need to do this to
					// clear libvchan_fd pending state
					libvchan_wait(ctrl);

					if (!bVchanClientConnected) {

						debugf("WatchForEvents(): A vchan client has connected\n");

						bVchanClientConnected = TRUE;
						break;
					}

					EnterCriticalSection(&g_VchanCriticalSection);

					if (!libvchan_is_open(ctrl)) {
						bVchanReturnedError = TRUE;
						LeaveCriticalSection(&g_VchanCriticalSection);
						break;
					}

					while (read_ready_vchan_ext()) {
						uResult = handle_server_data();
						if (ERROR_SUCCESS != uResult) {
							bVchanReturnedError = TRUE;
							perror("WatchForEvents(): handle_server_data()");
							LeaveCriticalSection(&g_VchanCriticalSection);
							break;
						}
					}

					LeaveCriticalSection(&g_VchanCriticalSection);

					EnterCriticalSection(&g_ClientsCriticalSection);

					for (uClientNumber = 0; uClientNumber < MAX_CLIENTS; uClientNumber++) {

						if (g_Clients[uClientNumber].bClientIsReady && !g_Clients[uClientNumber].bReadingIsDisabled) {
							if (g_Clients[uClientNumber].Stdout.bVchanWritePending) {
								uResult = ReturnPipeData(g_Clients[uClientNumber].client_id, &g_Clients[uClientNumber].Stdout);
								if (ERROR_HANDLE_EOF == uResult) {
									PossiblyHandleTerminatedClientNoLocks(&g_Clients[uClientNumber]);
								} else if (ERROR_INSUFFICIENT_BUFFER == uResult) {
									// no more space in vchan
									break;
								} else if (ERROR_SUCCESS != uResult) {
									bVchanReturnedError = TRUE;
									perror("WatchForEvents(): ReturnPipeData(STDOUT)");
								}
							}
							if (g_Clients[uClientNumber].Stderr.bVchanWritePending) {
								uResult = ReturnPipeData(g_Clients[uClientNumber].client_id, &g_Clients[uClientNumber].Stderr);
								if (ERROR_HANDLE_EOF == uResult) {
									PossiblyHandleTerminatedClientNoLocks(&g_Clients[uClientNumber]);
								} else if (ERROR_INSUFFICIENT_BUFFER == uResult) {
									// no more space in vchan
									break;
								} else if (ERROR_SUCCESS != uResult) {
									bVchanReturnedError = TRUE;
									perror("WatchForEvents(): ReturnPipeData(STDERR)");
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
					if (ERROR_HANDLE_EOF == uResult) {
						PossiblyHandleTerminatedClient(&g_Clients[g_HandlesInfo[dwSignaledEvent].uClientNumber]);
					} else if (ERROR_SUCCESS != uResult && ERROR_INSUFFICIENT_BUFFER != uResult) {
						bVchanReturnedError = TRUE;
						perror("WatchForEvents(): ReturnPipeData(STDOUT)");
					}
					break;

				case HTYPE_STDERR:
#ifdef DISPLAY_CONSOLE_OUTPUT
					printf("%s", &g_Clients[g_HandlesInfo[dwSignaledEvent].uClientNumber].Stderr.ReadBuffer);
#endif
					uResult = ReturnPipeData(
							g_Clients[g_HandlesInfo[dwSignaledEvent].uClientNumber].client_id,
							&g_Clients[g_HandlesInfo[dwSignaledEvent].uClientNumber].Stderr);
					if (ERROR_HANDLE_EOF == uResult) {
						PossiblyHandleTerminatedClient(&g_Clients[g_HandlesInfo[dwSignaledEvent].uClientNumber]);
					} else if (ERROR_SUCCESS != uResult && ERROR_INSUFFICIENT_BUFFER != uResult) {
						bVchanReturnedError = TRUE;
						perror("WatchForEvents(): ReturnPipeData(STDERR)");
					}
					break;

				case HTYPE_PROCESS:

					pClientInfo = &g_Clients[g_HandlesInfo[dwSignaledEvent].uClientNumber];

					if (!GetExitCodeProcess(pClientInfo->hProcess, &dwExitCode)) {
						perror("WatchForEvents(): GetExitCodeProcess()");
						dwExitCode = ERROR_SUCCESS;
					}

					pClientInfo->bChildExited = TRUE;
					pClientInfo->dwExitCode = dwExitCode;
					// send exit code only when all data was sent to the daemon
					uResult = PossiblyHandleTerminatedClient(pClientInfo);
					if (ERROR_SUCCESS != uResult) {
						bVchanReturnedError = TRUE;
						perror("WatchForEvents(): send_exit_code()");
					}

					break;

				default:
					logf("WatchForEvents(): invalid handle type %d for event %d\n",
						g_HandlesInfo[dwSignaledEvent].bType, dwSignaledEvent);
					break;
			}
		}

		if (bVchanReturnedError)
			break;
	}

	RemoveAllClients();

	if (bVchanClientConnected)
		libvchan_close(ctrl);

	return bVchanReturnedError ? ERROR_INVALID_FUNCTION : ERROR_SUCCESS;
}

VOID Usage()
{
	_tprintf(TEXT("\nqrexec agent service\n\nUsage: qrexec-agent <-i|-u>\n"));
}


ULONG CheckForXenInterface()
{
    // TODO?
#if 0
	EVTCHN	xc;


	xc = xc_evtchn_open();
	if (INVALID_HANDLE_VALUE == xc)
		return ERROR_NOT_SUPPORTED;

	xc_evtchn_close(xc);
#endif
	return ERROR_SUCCESS;
}

ULONG WINAPI ServiceExecutionThread(PVOID pParam)
{
	ULONG	uResult;
	HANDLE	hTriggerEventsThread;

	debugf("ServiceExecutionThread(): Service started\n");

	// Auto reset, initial state is not signaled
	g_hAddExistingClientEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
	if (!g_hAddExistingClientEvent) {
		uResult = GetLastError();
		perror("ServiceExecutionThread(): CreateEvent()");
		return uResult;
	}

	hTriggerEventsThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)WatchForTriggerEvents, NULL, 0, NULL);
	if (!hTriggerEventsThread) {
		uResult = GetLastError();
		perror("ServiceExecutionThread(): CreateThread()");
		CloseHandle(g_hAddExistingClientEvent);
		return uResult;
	}

	for (;;) {

		uResult = WatchForEvents();
		if (ERROR_SUCCESS != uResult)
			perror("ServiceExecutionThread(): WatchForEvents()");

		if (!WaitForSingleObject(g_hStopServiceEvent, 0))
			break;

		Sleep(1000);
	}

	debugf("ServiceExecutionThread(): Waiting for the trigger thread to exit\n");
	WaitForSingleObject(hTriggerEventsThread, INFINITE);
	CloseHandle(hTriggerEventsThread);
	CloseHandle(g_hAddExistingClientEvent);

	DeleteCriticalSection(&g_ClientsCriticalSection);
	DeleteCriticalSection(&g_VchanCriticalSection);

	debugf("ServiceExecutionThread(): Shutting down\n");

	return ERROR_SUCCESS;
}

void InitLog()
{
	TCHAR buffer[MAX_PATH];
	SYSTEMTIME st;
	DWORD len;

	GetLocalTime(&st);
	memset(buffer, 0, sizeof(buffer));
	if (FAILED(StringCchPrintf(buffer, RTL_NUMBER_OF(buffer), 
		TEXT("%s\\qrexec-agent-%04d%02d%02d-%02d%02d%02d-%08x.log"), LOG_DIR,
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId())))
	{
		perror("StringCchPrintf"); // this will just write to stderr before logfile is initialized
		exit(1);
	}

	log_init(buffer);

	logf("\nLog started: %s\n", buffer);
	memset(buffer, 0, sizeof(buffer));
	len = RTL_NUMBER_OF(buffer);
	if (!GetUserName(buffer, &len))
	{
		perror("GetUserName");
		exit(1);
	}
	logf("Running as user: %s\n", buffer);
}

#ifdef BUILD_AS_SERVICE

ULONG Init(HANDLE *phServiceThread)
{
	ULONG	uResult;
	HANDLE	hThread;
	ULONG	uClientNumber;

	*phServiceThread = INVALID_HANDLE_VALUE;

	uResult = CheckForXenInterface();
	if (ERROR_SUCCESS != uResult) {
		perror("Init(): CheckForXenInterface()");
		ReportErrorToEventLog(XEN_INTERFACE_NOT_FOUND);
		return ERROR_NOT_SUPPORTED;
	}

	// InitializeCriticalSection always succeeds in Vista and later OSes.
#if NTDDI_VERSION < NTDDI_VISTA
//	__try {
#endif
		InitializeCriticalSection(&g_ClientsCriticalSection);
		InitializeCriticalSection(&g_VchanCriticalSection);
		InitializeCriticalSection(&g_PipesCriticalSection);
#if NTDDI_VERSION < NTDDI_VISTA
//	} __except(EXCEPTION_EXECUTE_HANDLER) {
//		debugf("main(): InitializeCriticalSection() raised an exception %d\n", GetExceptionCode());
//		return ERROR_NOT_ENOUGH_MEMORY;
//	}
#endif

	for (uClientNumber = 0; uClientNumber < MAX_CLIENTS; uClientNumber++)
		g_Clients[uClientNumber].client_id = FREE_CLIENT_SPOT_ID;

	hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)ServiceExecutionThread, NULL, 0, NULL);
	if (!hThread) {
		uResult = GetLastError();
		perror("StartServiceThread(): CreateThread()");
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

	InitLog();

	SERVICE_TABLE_ENTRY	ServiceTable[] = {
		{SERVICE_NAME, (LPSERVICE_MAIN_FUNCTION)ServiceMain},
		{NULL,NULL}
	};

	memset(szUserName, 0, sizeof(szUserName));
	nSize = RTL_NUMBER_OF(szUserName);
	if (!GetUserName(szUserName, &nSize)) {
		uResult = GetLastError();
		perror("main(): GetUserName()");
		return uResult;
	}

	if ((1 == argc) && _tcscmp(szUserName, TEXT("SYSTEM"))) {
		Usage();
		return ERROR_INVALID_PARAMETER;
	}

	if (1 == argc) {
		debugf("main(): Running as SYSTEM\n");

		uResult = ERROR_SUCCESS;
		if (!StartServiceCtrlDispatcher(ServiceTable)) {
			uResult = GetLastError();
			perror("main(): StartServiceCtrlDispatcher()");
		}

		debugf("main(): Exiting\n");
		return uResult;
	}

	memset(szFullPath, 0, sizeof(szFullPath));
	if (!GetModuleFileName(NULL, szFullPath, RTL_NUMBER_OF(szFullPath) - 1)) {
		uResult = GetLastError();
		perror("main(): GetModuleFileName()");
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
		debugf("main(): GrantDesktopAccess(\"%s\")\n", pszAccountName);
		uResult = GrantDesktopAccess(pszAccountName, NULL);
		if (ERROR_SUCCESS != uResult)
		{
			perror("GrantDesktopAccess");
			logf("main(): GrantDesktopAccess(\"%s\") failed\n", pszAccountName);
		}

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

// Is not called when built without BUILD_AS_SERVICE definition.
ULONG Init(HANDLE *phServiceThread)
{
	return ERROR_SUCCESS;
}

BOOL WINAPI CtrlHandler(DWORD fdwCtrlType)
{
	debugf("CtrlHandler(): Got shutdown signal\n");

	SetEvent(g_hStopServiceEvent);

	WaitForSingleObject(g_hCleanupFinishedEvent, 2000);

	CloseHandle(g_hStopServiceEvent);
	CloseHandle(g_hCleanupFinishedEvent);

	debugf("CtrlHandler(): Shutdown complete\n");
	ExitProcess(0);
	return TRUE;
}

// This is the entry point for a console application (BUILD_AS_SERVICE not defined).
int __cdecl _tmain(ULONG argc, PTCHAR argv[])
{
	ULONG	uResult;
	ULONG	uClientNumber;

	_tprintf(TEXT("\nqrexec agent console application\n\n"));
	InitLog();

	if (ERROR_SUCCESS != CheckForXenInterface()) {
		debugf("main(): Could not find Xen interface\n");
		return ERROR_NOT_SUPPORTED;
	}

	// Manual reset, initial state is not signaled
	g_hStopServiceEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!g_hStopServiceEvent) {
		uResult = GetLastError();
		perror("main(): CreateEvent()");
		return uResult;
	}

	// Manual reset, initial state is not signaled
	g_hCleanupFinishedEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!g_hCleanupFinishedEvent) {
		uResult = GetLastError();
		perror("main(): CreateEvent()");
		CloseHandle(g_hStopServiceEvent);
		return uResult;
	}

	// InitializeCriticalSection always succeeds in Vista and later OSes.
#if NTDDI_VERSION < NTDDI_VISTA
//	__try {
#endif
		InitializeCriticalSection(&g_ClientsCriticalSection);
		InitializeCriticalSection(&g_VchanCriticalSection);
		InitializeCriticalSection(&g_PipesCriticalSection);
#if NTDDI_VERSION < NTDDI_VISTA
//	} __except(EXCEPTION_EXECUTE_HANDLER) {
//		debugf("main(): InitializeCriticalSection() raised an exception %d\n", GetExceptionCode());
//		return ERROR_NOT_ENOUGH_MEMORY;
//	}
#endif
	if (!SetConsoleCtrlHandler((PHANDLER_ROUTINE)CtrlHandler, TRUE))
		perror("main: SetConsoleCtrlHandler");

	for (uClientNumber = 0; uClientNumber < MAX_CLIENTS; uClientNumber++)
		g_Clients[uClientNumber].client_id = FREE_CLIENT_SPOT_ID;

	ServiceExecutionThread(NULL);
	SetEvent(g_hCleanupFinishedEvent);

	return ERROR_SUCCESS;
}
#endif
