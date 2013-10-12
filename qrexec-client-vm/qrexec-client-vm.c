#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <tchar.h>
#include <strsafe.h>
#include <qrexec.h>
#include "log.h"
#include "agent.h"
#include "utf8-conv.h"
#include "exec.h"
#ifdef BACKEND_VMM_wni
#include <lmcons.h>  // for UNLEN
#endif

ULONG SendCreateProcessResponse(HANDLE hPipe, CREATE_PROCESS_RESPONSE *pCpr)
{
	DWORD	cbWritten;
	DWORD	cbRead;
	ULONG	uResult;


	if (!pCpr)
		return ERROR_INVALID_PARAMETER;

	if (!WriteFile(
			hPipe,
			pCpr,
			sizeof(CREATE_PROCESS_RESPONSE),
			&cbWritten,
			NULL)) {

		uResult = GetLastError();
		perror("SendCreateProcessResponse(): WriteFile()");
		return uResult;
	}

	if (CPR_TYPE_HANDLE == pCpr->bType) {

		debugf("Waiting for the server to read the handle, duplicate it and close the pipe\n");

		// Issue a blocking dummy read that will finish when the server disconnects the pipe
		// after the process handle duplication is complete. We have to wait here because for
		// the handle to be duplicated successfully this process must be present.
		ReadFile(hPipe, &cbWritten, 1, &cbRead, NULL);
	}

	return ERROR_SUCCESS;
}


int __cdecl _tmain(ULONG argc, PTCHAR argv[])
{
	HANDLE	hPipe;
	BOOL	fSuccess = FALSE;
	DWORD	cbRead, cbWritten, dwMode;
#ifdef BACKEND_VMM_wni
#define MAX_PIPENAME_LEN (RTL_NUMBER_OF(TRIGGER_PIPE_NAME) + UNLEN)
	TCHAR	lpszPipename[MAX_PIPENAME_LEN];
	DWORD	user_name_len = UNLEN + 1;
	TCHAR	user_name[user_name_len];
#else
	LPTSTR	lpszPipename = TRIGGER_PIPE_NAME;
#endif
	struct	trigger_connect_params params;
	ULONG	uResult;
	char	*pszParameter;
	PTCHAR	pszLocalProgram;
	HRESULT	hResult;
	IO_HANDLES_ARRAY	IoHandles;
	HANDLE	hProcess;
	CREATE_PROCESS_RESPONSE	CreateProcessResponse;

	log_init(NULL, TEXT("qrexec-client-vm"));

	if (argc < 4) {
		errorf("usage: %s target_vmname service_id local_program [local program arguments]\n", argv[0]);
		exit(1);
	}

	// Prepare the parameter structure containing the first two arguments.
	memset(&params, 0, sizeof(params));


#ifdef UNICODE
	pszParameter = NULL;
	uResult = ConvertUTF16ToUTF8(argv[2], &pszParameter, NULL);
	if (ERROR_SUCCESS != uResult) {
		perror("UTF16ToUTF8(): WideCharToMultiByte()");
		return uResult;
	}
#else
	pszParameter = argv[2];
#endif

	hResult = StringCchCopyA(params.exec_index, sizeof(params.exec_index), pszParameter);
	if (FAILED(hResult)) {
		perror("StringCchCopyA()");
		return hResult;
	}

#ifdef UNICODE
	free(pszParameter);
	pszParameter = NULL;

	uResult = ConvertUTF16ToUTF8(argv[1], &pszParameter, NULL);
	if (ERROR_SUCCESS != uResult) {
		perror("UTF16ToUTF8(): WideCharToMultiByte()");
		return uResult;
	}
#else
	pszParameter = argv[1];
#endif

	hResult = StringCchCopyA(params.target_vmname, sizeof(params.target_vmname), pszParameter);
	if (FAILED(hResult)) {
		perror("StringCchCopyA()");
		return hResult;
	}

#ifdef UNICODE
	free(pszParameter);
#endif
	pszParameter = NULL;

#ifdef BACKEND_VMM_wni
    /* on WNI we don't have separate namespace for each VM (all is in the
     * single system) */
    if (!GetUserName(user_name, &user_name_len)) {
        perror("GetUserName");
        return GetLastError();
    }
    if (FAILED(StringCchPrintf(lpszPipename, MAX_PIPENAME_LEN, TRIGGER_PIPE_NAME, user_name)))
        return ERROR_NOT_ENOUGH_MEMORY;
#endif

	// Try to open a named pipe; wait for it, if necessary.
	logf("Connecting to the pipe server: %s\n", lpszPipename);

	while (TRUE) {
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
		if (ERROR_PIPE_BUSY != uResult) {
			perror("qrexec-agent pipe not found, CreateFile()");
			return uResult;
		}

		// All pipe instances are busy, so wait for 10 seconds.

		if (!WaitNamedPipe(lpszPipename, 10000)) {
			uResult = GetLastError();
			perror("qrexec-agent pipe is busy, WaitNamedPipe()");
			return uResult;
		}
	}

	// The pipe connected; change to message-read mode.

	dwMode = PIPE_READMODE_MESSAGE;
	if (!SetNamedPipeHandleState(
			hPipe, // pipe handle
			&dwMode, // new pipe mode
			NULL, // don't set maximum bytes
			NULL)) { // don't set maximum time
		uResult = GetLastError();
		perror("SetNamedPipeHandleState()");
		CloseHandle(hPipe);
		return uResult;
	}

	// Send params to the pipe server.
	debugf("Sending parameters to the server\n");

	if (!WriteFile(hPipe, &params, sizeof(params), &cbWritten, NULL)) {
		uResult = GetLastError();
		perror("WriteFile()");
		CloseHandle(hPipe);
		return uResult;
	}

	debugf("Receiving IO handles\n");

	// Read the handle array from the pipe.
	fSuccess = ReadFile(
			hPipe,
			&IoHandles,
			sizeof(IoHandles),
			&cbRead,
			NULL);

	if (!fSuccess || cbRead != sizeof(IoHandles)) {
		// If the message is too large to fit in a buffer, treat it as an error as well:
		// this shouldn't happen if the pipe server operates correctly.
		uResult = GetLastError();
		perror("ReadFile()");
		CloseHandle(hPipe);
		return uResult;
	}

	logf("Starting local program \"%s\"\n", argv[3]);

	// find command line staring at third parameter _including_ quotes
	pszLocalProgram = _tcsstr(GetCommandLine(), argv[2]);
	pszLocalProgram += _tcslen(argv[2]);
	while (pszLocalProgram[0] == TEXT(' '))
		pszLocalProgram++;

	uResult = CreatePipedProcessAsCurrentUser(
			pszLocalProgram,	// local program
			IoHandles.hPipeStdin,
			IoHandles.hPipeStdout,
			IoHandles.hPipeStderr,
			&hProcess);

	CloseHandle(IoHandles.hPipeStdin);
	CloseHandle(IoHandles.hPipeStdout);
	CloseHandle(IoHandles.hPipeStderr);

	if (ERROR_SUCCESS != uResult) {

		perror("CreatePipedProcessAsCurrentUser()");

		debugf("Sending error code to the server\n");

		CreateProcessResponse.bType = CPR_TYPE_ERROR_CODE;
		CreateProcessResponse.ResponseData.dwErrorCode = uResult;

	} else {

		debugf("Sending process handle of the local program to the server\n");

		CreateProcessResponse.bType = CPR_TYPE_HANDLE;
		CreateProcessResponse.ResponseData.hProcess = hProcess;
	}

	SendCreateProcessResponse(hPipe, &CreateProcessResponse);

	debugf("Closing the pipe\n");

	CloseHandle(hPipe);

	if (ERROR_SUCCESS == uResult)
		CloseHandle(hProcess);

	return 0;
}
