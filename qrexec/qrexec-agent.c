#include "qrexec-agent.h"
#include "utf8-conv.h"

libvchan_t *g_daemon_vchan;

HANDLE g_hAddExistingClienEvent;

CHILD_INFO g_Children[MAX_CHILDREN];
HANDLE g_WatchedEvents[MAXIMUM_WAIT_OBJECTS];
HANDLE_INFO g_HandlesInfo[MAXIMUM_WAIT_OBJECTS];

ULONG64	g_uPipeId = 0;

CRITICAL_SECTION g_ChildrenCriticalSection;
CRITICAL_SECTION g_DaemonCriticalSection;

HANDLE g_hStopServiceEvent;
#ifndef BUILD_AS_SERVICE
HANDLE g_hCleanupFinishedEvent;
#endif

// vchan for daemon
libvchan_t *vchan_server_init(int port)
{
    libvchan_t *vchan;
    /* FIXME: "0" here is remote domain id */
    vchan = libvchan_server_init(0, port, VCHAN_BUFFER_SIZE, VCHAN_BUFFER_SIZE);

    debugf("vchan_server_init(%d): daemon vchan = 0x%x\n", port, vchan);

    return vchan;
}

ULONG CreateAsyncPipe(HANDLE *phReadPipe, HANDLE *phWritePipe, SECURITY_ATTRIBUTES *pSecurityAttributes)
{
    TCHAR	szPipeName[MAX_PATH + 1];
    HANDLE	hReadPipe;
    HANDLE	hWritePipe;
    ULONG	uResult;

#ifdef BACKEND_VMM_wni
    DWORD user_name_len = UNLEN + 1;
    TCHAR user_name[UNLEN + 1];
#endif

    if (!phReadPipe || !phWritePipe)
        return ERROR_INVALID_PARAMETER;

#ifdef BACKEND_VMM_wni
    /* on WNI we don't have separate namespace for each VM (all is in the
     * single system) */

    if (!GetUserName(user_name, &user_name_len)) {
        uResult = GetLastError();
        perror("CreateAsyncPipe: GetUserName");
        return uResult;
    }
    StringCchPrintf(szPipeName, MAX_PATH, TEXT("\\\\.\\pipe\\%s\\qrexec.%08x.%I64x"), user_name, GetCurrentProcessId(), g_uPipeId++);
#else
    StringCchPrintf(szPipeName, MAX_PATH, TEXT("\\\\.\\pipe\\qrexec.%08x.%I64x"), GetCurrentProcessId(), g_uPipeId++);
#endif

    debugf("CreateAsyncPipe: %s\n", szPipeName);

    hReadPipe = CreateNamedPipe(
        szPipeName,
        PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE,
        1, // instances
        PIPE_BUFFER_SIZE,
        PIPE_BUFFER_SIZE,
        PIPE_DEFAULT_TIMEOUT,	// the default timeout is 50ms
        pSecurityAttributes);
    if (!hReadPipe) {
        uResult = GetLastError();
        perror("CreateAsyncPipe: CreateNamedPipe");
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
        perror("CreateAsyncPipe: CreateFile");
        CloseHandle(hReadPipe);
        return uResult;
    }

    *phReadPipe = hReadPipe;
    *phWritePipe = hWritePipe;

    return ERROR_SUCCESS;
}

ULONG InitReadPipe(PIPE_DATA *pPipeData, HANDLE *phWritePipe, PIPE_TYPE bPipeType)
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
        perror("InitReadPipe: CreateAsyncPipe");
        CloseHandle(pPipeData->olRead.hEvent);
        return uResult;
    }

    // Ensure the read handle to the pipe is not inherited.
    SetHandleInformation(pPipeData->hReadPipe, HANDLE_FLAG_INHERIT, 0);

    pPipeData->bPipeType = bPipeType;

    return ERROR_SUCCESS;
}

ULONG send_msg_to_vchan(libvchan_t *vchan, int type, void *pData, ULONG uDataSize, ULONG *puDataWritten)
{
    int vchan_space_avail;
    ULONG uResult = ERROR_SUCCESS;
    struct msg_header hdr;

    EnterCriticalSection(&g_DaemonCriticalSection);

    if (puDataWritten) {
        // allow partial write only when puDataWritten given
        *puDataWritten = 0;
        vchan_space_avail = libvchan_buffer_space(vchan);
        debugf("send_msg_to_vchan: libvchan_buffer_space=%d\n", vchan_space_avail);

        if (vchan_space_avail < sizeof(hdr)) {
            LeaveCriticalSection(&g_DaemonCriticalSection);
            debugf("send_msg_to_vchan: vchan full (%d)\n", vchan_space_avail);
            return ERROR_INSUFFICIENT_BUFFER;
        }

        // inhibit zero-length write when not requested
        if (uDataSize && vchan_space_avail == sizeof(hdr)) {
            LeaveCriticalSection(&g_DaemonCriticalSection);
            debugf("send_msg_to_vchan: inhibit zero-length write when not requested\n");
            return ERROR_INSUFFICIENT_BUFFER;
        }

        if (vchan_space_avail < sizeof(hdr)+uDataSize) {
            // partial write
            uResult = ERROR_INSUFFICIENT_BUFFER;
            uDataSize = vchan_space_avail - sizeof(hdr);
        }

        *puDataWritten = uDataSize;
    }

    hdr.type = type;
    hdr.len = uDataSize;
    if (!send_to_vchan(vchan, &hdr, sizeof(hdr), "header")) {
        LeaveCriticalSection(&g_DaemonCriticalSection);
        return ERROR_INVALID_FUNCTION;
    }

    if (!uDataSize) {
        LeaveCriticalSection(&g_DaemonCriticalSection);
        return ERROR_SUCCESS;
    }

    if (!send_to_vchan(vchan, pData, uDataSize, "data")) {
        LeaveCriticalSection(&g_DaemonCriticalSection);
        return ERROR_INVALID_FUNCTION;
    }

    LeaveCriticalSection(&g_DaemonCriticalSection);
    return uResult;
}

// send to qrexec-client and close vchan
ULONG send_exit_code(libvchan_t *vchan, int status)
{
    ULONG uResult;

    uResult = send_msg_to_vchan(vchan, MSG_DATA_EXIT_CODE, &status, sizeof(status), NULL);
    if (ERROR_SUCCESS != uResult) {
        perror("send_exit_code: send_msg_to_vchan");
        return uResult;
    } else
        debugf("send_exit_code: sent %d to 0x%x\n", status, vchan);

    libvchan_close(vchan);

    return ERROR_SUCCESS;
}

CHILD_INFO *FindChildByVchan(libvchan_t *vchan)
{
    ULONG uChildIndex;

    for (uChildIndex = 0; uChildIndex < MAX_CHILDREN; uChildIndex++)
        if (vchan == g_Children[uChildIndex].vchan)
            return &g_Children[uChildIndex];

    return NULL;
}

// send i/o to qrexec-client
ULONG ReturnPipeData(libvchan_t *vchan, PIPE_DATA *pPipeData)
{
    DWORD dwRead;
    int message_type;
    CHILD_INFO *pChildInfo;
    ULONG uResult;
    ULONG uDataSent;

    uResult = ERROR_SUCCESS;

    if (!pPipeData)
        return ERROR_INVALID_PARAMETER;

    pChildInfo = FindChildByVchan(vchan);
    if (!pChildInfo)
        return ERROR_FILE_NOT_FOUND;

    if (pChildInfo->bReadingIsDisabled)
        // The client does not want to receive any data from this console.
        return ERROR_INVALID_FUNCTION;

    pPipeData->bReadInProgress = FALSE;
    pPipeData->bDataIsReady = FALSE;

    switch (pPipeData->bPipeType) {
    case PTYPE_STDOUT:
        message_type = MSG_DATA_STDOUT;
        break;
    case PTYPE_STDERR:
        message_type = MSG_DATA_STDERR;
        break;
    default:
        return ERROR_INVALID_FUNCTION;
    }

    dwRead = 0;
    if (!GetOverlappedResult(pPipeData->hReadPipe, &pPipeData->olRead, &dwRead, FALSE)) {
        perror("ReturnPipeData: GetOverlappedResult");
        errorf("ReturnPipeData: GetOverlappedResult, client 0x%x, dwRead %d\n", vchan, dwRead);
    }

    uResult = send_msg_to_vchan(vchan, message_type, pPipeData->ReadBuffer+pPipeData->dwSentBytes, dwRead-pPipeData->dwSentBytes, &uDataSent);
    if (ERROR_INSUFFICIENT_BUFFER == uResult) {
        pPipeData->dwSentBytes += uDataSent;
        pPipeData->bVchanWritePending = TRUE;
        return uResult;
    } else if (ERROR_SUCCESS != uResult)
        perror("ReturnPipeData: send_msg_to_vchan");

    pPipeData->bVchanWritePending = FALSE;

    if (!dwRead) {
        pPipeData->bPipeClosed = TRUE;
        uResult = ERROR_HANDLE_EOF;
    }

    return uResult;
}

ULONG CloseReadPipeHandles(libvchan_t *vchan, PIPE_DATA *pPipeData)
{
    ULONG uResult;

    if (!pPipeData)
        return ERROR_INVALID_PARAMETER;

    uResult = ERROR_SUCCESS;

    if (pPipeData->olRead.hEvent) {
        if (pPipeData->bDataIsReady)
            ReturnPipeData(vchan, pPipeData);

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
                ReturnPipeData(vchan, pPipeData);
            } else {
                uResult = GetLastError();
                perror("CloseReadPipeHandles: CancelIo");
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
    WCHAR  *pwszUtf16 = NULL;
    ULONG   uResult;
    HRESULT hResult;

    if (!pszBuf || !cbBufLen || !ppwszUtf16)
        return ERROR_INVALID_PARAMETER;

    *ppwszUtf16 = NULL;

    // see http://en.wikipedia.org/wiki/Byte-order_mark for explanation of the BOM
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

        pwszUtf16 = (WCHAR*) malloc(cbBufLen - cbSkipChars + sizeof(WCHAR));
        if (!pwszUtf16)
            return ERROR_NOT_ENOUGH_MEMORY;

        hResult = StringCbCopyW(pwszUtf16, cbBufLen - cbSkipChars + sizeof(WCHAR), (WCHAR*)(pszBuf + cbSkipChars));
        if (FAILED(hResult)) {
            perror("TextBOMToUTF16: StringCbCopyW");
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
        perror("TextBOMToUTF16: UTF8ToUTF16");
        return uResult;
    }

    return ERROR_SUCCESS;
}

ULONG ParseUtf8Command(char *pszUtf8Command, PWCHAR *ppwszCommand, PWCHAR *ppwszUserName, PWCHAR *ppwszCommandLine, PBOOL pbRunInteractively)
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
        perror("ParseUtf8Command: UTF8ToUTF16");
        return uResult;
    }

    pwszUserName = pwszCommand;
    pwSeparator = wcschr(pwszCommand, L':');
    if (!pwSeparator) {
        free(pwszCommand);
        errorf("ParseUtf8Command: Command line is supposed to be in user:[nogui:]command form\n");
        return ERROR_INVALID_PARAMETER;
    }

    *pwSeparator = L'\0';
    pwSeparator++;

    if (!wcsncmp(pwSeparator, L"nogui:", 6)) {
        pwSeparator = wcschr(pwSeparator, L':');
        if (!pwSeparator) {
            free(pwszCommand);
            errorf("ParseUtf8Command: Command line is supposed to be in user:[nogui:]command form\n");
            return ERROR_INVALID_PARAMETER;
        }

        *pwSeparator = L'\0';
        pwSeparator++;

        *pbRunInteractively = FALSE;
    }

    if (!wcscmp(pwszUserName, L"SYSTEM") || !wcscmp(pwszUserName, L"root")) {
        pwszUserName = NULL;
    }

    debugf("ParseUtf8Command: pwszCommand=%s, pwszUserName=%s, cmd=%s\n", pwszCommand, pwszUserName, pwSeparator);

    *ppwszCommand = pwszCommand;
    *ppwszUserName = pwszUserName;
    *ppwszCommandLine = pwSeparator;

    return ERROR_SUCCESS;
}

ULONG CreateChildPipes(CHILD_INFO *pChildInfo, HANDLE *phPipeStdin, HANDLE *phPipeStdout, HANDLE *phPipeStderr)
{
    ULONG	uResult;
    SECURITY_ATTRIBUTES	sa;
    HANDLE	hPipeStdin = INVALID_HANDLE_VALUE;
    HANDLE	hPipeStdout = INVALID_HANDLE_VALUE;
    HANDLE	hPipeStderr = INVALID_HANDLE_VALUE;

    if (!pChildInfo || !phPipeStdin || !phPipeStdout || !phPipeStderr)
        return ERROR_INVALID_PARAMETER;

    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    uResult = InitReadPipe(&pChildInfo->Stdout, &hPipeStdout, PTYPE_STDOUT);
    if (ERROR_SUCCESS != uResult) {
        perror("CreateChildPipes: InitReadPipe(STDOUT)");
        return uResult;
    }

    uResult = InitReadPipe(&pChildInfo->Stderr, &hPipeStderr, PTYPE_STDERR);
    if (ERROR_SUCCESS != uResult) {
        perror("CreateChildPipes: InitReadPipe(STDERR)");

        CloseHandle(pChildInfo->Stdout.hReadPipe);
        CloseHandle(hPipeStdout);

        return uResult;
    }

    if (!CreatePipe(&hPipeStdin, &pChildInfo->hWriteStdinPipe, &sa, 0)) {
        uResult = GetLastError();
        perror("CreateChildPipes: CreatePipe(STDIN)");

        CloseHandle(pChildInfo->Stdout.hReadPipe);
        CloseHandle(pChildInfo->Stderr.hReadPipe);
        CloseHandle(hPipeStdout);
        CloseHandle(hPipeStderr);

        return uResult;
    }

    pChildInfo->bStdinPipeClosed = FALSE;

    // Ensure the write handle to the pipe for STDIN is not inherited.
    SetHandleInformation(pChildInfo->hWriteStdinPipe, HANDLE_FLAG_INHERIT, 0);

    *phPipeStdin = hPipeStdin;
    *phPipeStdout = hPipeStdout;
    *phPipeStderr = hPipeStderr;

    return ERROR_SUCCESS;
}

// This routine may be called by pipe server threads, hence the critical section around g_Children array is required.
ULONG ReserveChildIndex(libvchan_t *vchan, PULONG puChildIndex)
{
    ULONG uChildIndex;

    EnterCriticalSection(&g_ChildrenCriticalSection);

    for (uChildIndex = 0; uChildIndex < MAX_CHILDREN; uChildIndex++)
        if (NULL == g_Children[uChildIndex].vchan)
            break;

    if (MAX_CHILDREN == uChildIndex) {
        // There is no space for another child
        LeaveCriticalSection(&g_ChildrenCriticalSection);
        logf("ReserveChildIndex: The maximum number of running processes (%d) has been reached\n", MAX_CHILDREN);
        return ERROR_TOO_MANY_CMDS;
    }

    if (FindChildByVchan(vchan)) {
        LeaveCriticalSection(&g_ChildrenCriticalSection);
        errorf("ReserveChildIndex: A client with the same vchan (0x%x) already exists\n", vchan);
        return ERROR_ALREADY_EXISTS;
    }

    g_Children[uChildIndex].bChildIsReady = FALSE;
    g_Children[uChildIndex].vchan = vchan;
    *puChildIndex = uChildIndex;

    LeaveCriticalSection(&g_ChildrenCriticalSection);

    return ERROR_SUCCESS;
}

ULONG ReleaseChildIndex(ULONG uChildIndex)
{
    if (uChildIndex >= MAX_CHILDREN)
        return ERROR_INVALID_PARAMETER;

    EnterCriticalSection(&g_ChildrenCriticalSection);

    g_Children[uChildIndex].bChildIsReady = FALSE;
    g_Children[uChildIndex].vchan = NULL;

    LeaveCriticalSection(&g_ChildrenCriticalSection);

    return ERROR_SUCCESS;
}

ULONG AddFilledChildInfo(ULONG uChildIndex, CHILD_INFO *pChildInfo)
{
    if (!pChildInfo || uChildIndex >= MAX_CHILDREN)
        return ERROR_INVALID_PARAMETER;

    EnterCriticalSection(&g_ChildrenCriticalSection);

    g_Children[uChildIndex] = *pChildInfo;
    g_Children[uChildIndex].bChildIsReady = TRUE;

    LeaveCriticalSection(&g_ChildrenCriticalSection);

    return ERROR_SUCCESS;
}

// creates child process that's associated with qrexec-client's vchan for i/o exchange
ULONG CreateChild(libvchan_t *vchan, PWCHAR pwszUserName, PWCHAR pwszCommandLine, BOOL bRunInteractively)
{
    ULONG uResult;
    CHILD_INFO ChildInfo;
    HANDLE hPipeStdout = INVALID_HANDLE_VALUE;
    HANDLE hPipeStderr = INVALID_HANDLE_VALUE;
    HANDLE hPipeStdin = INVALID_HANDLE_VALUE;
    ULONG uChildIndex;

    // if pwszUserName is NULL we run the process on behalf of the current user.
    if (!pwszCommandLine)
        return ERROR_INVALID_PARAMETER;

    uResult = ReserveChildIndex(vchan, &uChildIndex);
    if (ERROR_SUCCESS != uResult) {
        perror("CreateChild: ReserveChildIndex");
        return uResult;
    }

    if (pwszUserName)
        logf("CreateChild: Running \"%s\" as user \"%s\"\n", pwszCommandLine, pwszUserName);
    else {
#ifdef BUILD_AS_SERVICE
        logf("CreateChild: Running \"%s\" as service user\n", pwszCommandLine);
#else
        logf("CreateChild: Running \"%s\" as current user\n", pwszCommandLine);
#endif
    }

    memset(&ChildInfo, 0, sizeof(ChildInfo));
    ChildInfo.vchan = vchan;

    uResult = CreateChildPipes(&ChildInfo, &hPipeStdin, &hPipeStdout, &hPipeStderr);
    if (ERROR_SUCCESS != uResult) {
        perror("CreateChild: CreateChildPipes");
        ReleaseChildIndex(uChildIndex);
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
            &ChildInfo.hProcess);
    else
        uResult = CreatePipedProcessAsCurrentUser(
            pwszCommandLine,
            bRunInteractively,
            hPipeStdin,
            hPipeStdout,
            hPipeStderr,
            &ChildInfo.hProcess);
#else
    uResult = CreatePipedProcessAsCurrentUser(
        pwszCommandLine,
        hPipeStdin,
        hPipeStdout,
        hPipeStderr,
        &ChildInfo.hProcess);
#endif

    CloseHandle(hPipeStdout);
    CloseHandle(hPipeStderr);
    CloseHandle(hPipeStdin);

    if (ERROR_SUCCESS != uResult) {
        ReleaseChildIndex(uChildIndex);

        CloseHandle(ChildInfo.hWriteStdinPipe);
        CloseHandle(ChildInfo.Stdout.hReadPipe);
        CloseHandle(ChildInfo.Stderr.hReadPipe);

#ifdef BUILD_AS_SERVICE
        if (pwszUserName)
            perror("CreateChild: CreatePipedProcessAsUser");
        else
            perror("CreateChild: CreatePipedProcessAsCurrentUser");
#else
        perror("CreateChild: CreatePipedProcessAsCurrentUser");
#endif
        return uResult;
    }

    uResult = AddFilledChildInfo(uChildIndex, &ChildInfo);
    if (ERROR_SUCCESS != uResult) {
        perror("CreateChild: AddFilledChildInfo");
        ReleaseChildIndex(uChildIndex);

        CloseHandle(ChildInfo.hWriteStdinPipe);
        CloseHandle(ChildInfo.Stdout.hReadPipe);
        CloseHandle(ChildInfo.Stderr.hReadPipe);
        CloseHandle(ChildInfo.hProcess);

        return uResult;
    }

    debugf("CreateChild: New child 0x%x (local id %d)\n", vchan, uChildIndex);

    return ERROR_SUCCESS;
}

ULONG AddExistingClient(libvchan_t *vchan, CHILD_INFO *pChildInfo)
{
    ULONG uChildIndex;
    ULONG uResult;

    if (!pChildInfo)
        return ERROR_INVALID_PARAMETER;

    uResult = ReserveChildIndex(vchan, &uChildIndex);
    if (ERROR_SUCCESS != uResult) {
        perror("AddExistingClient: ReserveChildIndex");
        return uResult;
    }

    pChildInfo->vchan = vchan;

    uResult = AddFilledChildInfo(uChildIndex, pChildInfo);
    if (ERROR_SUCCESS != uResult) {
        perror("AddExistingClient: AddFilledChildInfo");
        ReleaseChildIndex(uChildIndex);
        return uResult;
    }

    debugf("AddExistingClient: added 0x%x (local id %d)\n", vchan, uChildIndex);

    SetEvent(g_hAddExistingClienEvent);

    return ERROR_SUCCESS;
}

VOID RemoveChildNoLocks(CHILD_INFO *pChildInfo)
{
    if (!pChildInfo || (NULL == pChildInfo->vchan))
        return;

    CloseHandle(pChildInfo->hProcess);

    if (!pChildInfo->bStdinPipeClosed)
        CloseHandle(pChildInfo->hWriteStdinPipe);

    CloseReadPipeHandles(pChildInfo->vchan, &pChildInfo->Stdout);
    CloseReadPipeHandles(pChildInfo->vchan, &pChildInfo->Stderr);

    debugf("RemoveChildNoLocks: Child 0x%x removed\n", pChildInfo->vchan);

    pChildInfo->vchan = NULL;
    pChildInfo->bChildIsReady = FALSE;
}

VOID RemoveChild(CHILD_INFO *pChildInfo)
{
    EnterCriticalSection(&g_ChildrenCriticalSection);

    RemoveChildNoLocks(pChildInfo);

    LeaveCriticalSection(&g_ChildrenCriticalSection);
}

VOID RemoveAllChildren()
{
    ULONG uChildIndex;

    EnterCriticalSection(&g_ChildrenCriticalSection);

    for (uChildIndex = 0; uChildIndex < MAX_CHILDREN; uChildIndex++)
        if (NULL != g_Children[uChildIndex].vchan)
            RemoveChildNoLocks(&g_Children[uChildIndex]);

    LeaveCriticalSection(&g_ChildrenCriticalSection);
}

// must be called with g_ChildrenCriticalSection
ULONG PossiblyHandleTerminatedChildNoLocks(CHILD_INFO *pChildInfo)
{
    ULONG uResult;

    if (pChildInfo->bChildExited && pChildInfo->Stdout.bPipeClosed && pChildInfo->Stderr.bPipeClosed) {
        uResult = send_exit_code(pChildInfo->vchan, pChildInfo->dwExitCode);
        // guaranteed that all data was already sent (above bPipeClosed==TRUE)
        // so no worry about returning some data after exit code
        RemoveChildNoLocks(pChildInfo);
        return uResult;
    }
    return ERROR_SUCCESS;
}

ULONG PossiblyHandleTerminatedChild(CHILD_INFO *pChildInfo)
{
    ULONG uResult;

    if (pChildInfo->bChildExited && pChildInfo->Stdout.bPipeClosed && pChildInfo->Stderr.bPipeClosed) {
        uResult = send_exit_code(pChildInfo->vchan, pChildInfo->dwExitCode);
        // guaranteed that all data was already sent (above bPipeClosed==TRUE)
        // so no worry about returning some data after exit code
        RemoveChild(pChildInfo);
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
    size_t	uPathLength;
    PWCHAR	pwszServiceCommandLine = NULL;

    if (!pwszCommandLine || !ppwszServiceCommandLine || !ppwszSourceDomainName)
        return ERROR_INVALID_PARAMETER;

    *ppwszServiceCommandLine = *ppwszSourceDomainName = NULL;

    //debugf("InterceptRPCRequest: %s\n", pwszCommandLine);

    if (wcsncmp(pwszCommandLine, TEXT(RPC_REQUEST_COMMAND), wcslen(TEXT(RPC_REQUEST_COMMAND)))==0) {
        // RPC_REQUEST_COMMAND contains trailing space, so this must succeed
        pwSeparator = wcschr(pwszCommandLine, L' ');
        pwSeparator++;
        pwszServiceName = pwSeparator;
        pwSeparator = wcschr(pwszServiceName, L' ');
        if (pwSeparator) {
            *pwSeparator = L'\0';
            pwSeparator++;
            pwszSourceDomainName = _wcsdup(pwSeparator);
            if (!pwszSourceDomainName) {
                perror("InterceptRPCRequest: _wcsdup");
                return ERROR_NOT_ENOUGH_MEMORY;
            }
            debugf("InterceptRPCRequest: source domain: %s\n", pwszSourceDomainName);
        } else {
            logf("InterceptRPCRequest: No source domain given\n");
            // Most qrexec services do not use source domain at all, so do not
            // abort if missing. This can be the case when RPC triggered
            // manualy using qvm-run (qvm-run -p vmname "QUBESRPC service_name").
        }

        // build RPC service config file path
        memset(wszServiceFilePath, 0, sizeof(wszServiceFilePath));
        if (!GetModuleFileNameW(NULL, wszServiceFilePath, MAX_PATH)) {
            uResult = GetLastError();
            perror("InterceptRPCRequest: GetModuleFileName");
            if (pwszSourceDomainName)
                free(pwszSourceDomainName);
            return uResult;
        }
        // cut off file name (qrexec-agent.exe)
        pwSeparator = wcsrchr(wszServiceFilePath, L'\\');
        if (!pwSeparator) {
            if (pwszSourceDomainName)
                free(pwszSourceDomainName);
            errorf("InterceptRPCRequest: Cannot find dir containing qrexec-agent.exe\n");
            return ERROR_INVALID_PARAMETER;
        }
        *pwSeparator = L'\0';
        // cut off one dir (bin)
        pwSeparator = wcsrchr(wszServiceFilePath, L'\\');
        if (!pwSeparator) {
            if (pwszSourceDomainName)
                free(pwszSourceDomainName);
            errorf("InterceptRPCRequest: Cannot find dir containing bin\\qrexec-agent.exe\n");
            return ERROR_INVALID_PARAMETER;
        }
        // Leave trailing backslash
        pwSeparator++;
        *pwSeparator = L'\0';
        if (wcslen(wszServiceFilePath) + wcslen(L"qubes-rpc\\") + wcslen(pwszServiceName) > MAX_PATH) {
            if (pwszSourceDomainName)
                free(pwszSourceDomainName);
            errorf("InterceptRPCRequest: RPC service config file path too long\n");
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
            errorf("InterceptRPCRequest: Failed to open RPC %s configuration file (%s)\n", pwszServiceName, wszServiceFilePath);
            if (pwszSourceDomainName)
                free(pwszSourceDomainName);
            return uResult;
        }
        uBytesRead = 0;
        memset(szBuffer, 0, sizeof(szBuffer));
        if (!ReadFile(hServiceConfigFile, szBuffer, sizeof(WCHAR) * MAX_PATH, &uBytesRead, NULL)) {
            uResult = GetLastError();
            perror("ReadFile");
            errorf("InterceptRPCRequest: Failed to read RPC %s configuration file (%s)\n", pwszServiceName, wszServiceFilePath);
            if (pwszSourceDomainName)
                free(pwszSourceDomainName);
            CloseHandle(hServiceConfigFile);
            return uResult;
        }
        CloseHandle(hServiceConfigFile);

        uResult = TextBOMToUTF16(szBuffer, uBytesRead, &pwszRawServiceFilePath);
        if (uResult != ERROR_SUCCESS) {
            perror("TextBOMToUTF16");
            errorf("InterceptRPCRequest: Failed to parse the encoding in RPC %s configuration file (%s)\n", pwszServiceName, wszServiceFilePath);
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
        pwszServiceCommandLine = (WCHAR*) malloc((wcslen(wszServiceFilePath) + 1) * sizeof(WCHAR));
        if (pwszServiceCommandLine == NULL) {
            perror("InterceptRPCRequest(): malloc()");
            if (pwszSourceDomainName)
                free(pwszSourceDomainName);
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        StringCchCopyW(pwszServiceCommandLine, wcslen(wszServiceFilePath) + 1, wszServiceFilePath);

        *ppwszServiceCommandLine = pwszServiceCommandLine;
        *ppwszSourceDomainName = pwszSourceDomainName;
        logf("InterceptRPCRequest: RPC %s, domain %s, path: %s\n", 
            pwszServiceName, *ppwszSourceDomainName, *ppwszServiceCommandLine);
    }
    return ERROR_SUCCESS;
}

// read exec_params from daemon after one of the EXEC messages has been received
struct exec_params *recv_cmdline(int len)
{
    struct exec_params *exec;

    if (len == 0)
        return NULL;

    exec = (struct exec_params *) malloc(len);
    if (!exec)
        return NULL;

    if (!recv_from_vchan(g_daemon_vchan, exec, len, "exec_params")) {
        free(exec);
        return NULL;
    }

    return exec;
}

// This will return error only if vchan fails.
ULONG handle_service_connect(struct msg_header *hdr)
{
    ULONG uResult;
    struct exec_params *exec = NULL;
    libvchan_t *vchan = NULL;

    debugf("handle_service_connect: msg 0x%x, len %d\n", hdr->type, hdr->len);

    // qrexec-client always listens on a vchan (details in exec_params)
    vchan = NULL;
    exec = recv_cmdline(hdr->len);
    if (!exec)
    {
        errorf("handle_service_connect: recv_cmdline failed\n");
        return ERROR_INVALID_FUNCTION;
    }

    vchan = libvchan_client_init(exec->connect_domain, exec->connect_port);
    if (vchan)
    {
        debugf("handle_service_connect: connected to qrexec-client over vchan (%d, %d)\n",
            exec->connect_domain, exec->connect_port);
    }
    else
    {
        errorf("handle_service_connect: connect to qrexec-client over vchan (%d, %d) failed\n",
            exec->connect_domain, exec->connect_port);
        free(exec);
        return FALSE;
    }

#ifdef UNICODE
    debugf("handle_service_connect: client 0x%x, ident %S\n", vchan, exec->cmdline);
#else
    debugf("handle_service_connect: client 0x%x, ident %s\n", vchan, exec->cmdline);
#endif

    uResult = ProceedWithExecution(vchan, exec->cmdline);
    free(exec);

    if (ERROR_SUCCESS != uResult)
        perror("handle_service_connect: ProceedWithExecution");

    return ERROR_SUCCESS;
}

// This will return error only if vchan fails.
ULONG handle_service_refused(struct msg_header *hdr)
{
    ULONG uResult;
    struct service_params params;

    debugf("handle_service_refused: msg 0x%x, len %d\n", hdr->type, hdr->len);

    if (!recv_from_vchan(g_daemon_vchan, &params, hdr->len, "service_params")) {
        return ERROR_INVALID_FUNCTION;
    }

#ifdef UNICODE
    debugf("handle_service_refused: ident %S\n", params.ident);
#else
    debugf("handle_service_refused: ident %s\n", params.ident);
#endif

    uResult = ProceedWithExecution(NULL, params.ident);
    
    if (ERROR_SUCCESS != uResult)
        perror("handle_service_refused: ProceedWithExecution");

    return ERROR_SUCCESS;
}

// returns vchan for qrexec-client that initiated the request
// fails only if vchan fails
// returns TRUE and sets pVchan to NULL if cmdline parsing failed (caller should return with success status)
BOOL handle_exec_common(int len, PWCHAR *ppwszUserName, PWCHAR *ppwszCommandLine, BOOL *pbRunInteractively, 
                        libvchan_t **pVchan)
{
    struct exec_params *exec = NULL;
    ULONG uResult;
    PWCHAR pwszCommand = NULL;
    PWCHAR pwszRemoteDomainName = NULL;
    PWCHAR pwszServiceCommandLine = NULL;

    // qrexec-client always listens on a vchan (details in exec_params)
    *pVchan = NULL;
    exec = recv_cmdline(len);
    if (!exec)
    {
        errorf("handle_exec_common: recv_cmdline failed\n");
        return FALSE;
    }

    *pbRunInteractively = TRUE;
#ifdef UNICODE
    debugf("handle_exec_common: cmdline: %S\n", exec->cmdline);
#else
    debugf("handle_exec_common: cmdline: %s\n", exec->cmdline);
#endif
    debugf("handle_exec_common: connecting to qrexec-client (domain %d, port %d)...\n",
        exec->connect_domain, exec->connect_port);
    *pVchan = libvchan_client_init(exec->connect_domain, exec->connect_port);
    if (*pVchan)
    {
        debugf("handle_exec_common: connected to qrexec-client over vchan (%d, %d)\n",
            exec->connect_domain, exec->connect_port);
    }
    else
    {
        errorf("handle_exec_common: connect to qrexec-client over vchan (%d, %d) failed\n",
            exec->connect_domain, exec->connect_port);
        free(exec);
        return FALSE;
    }

    // pwszCommand is allocated in the call, ppwszUserName and ppwszCommandLine are pointers inside pwszCommand
    uResult = ParseUtf8Command(exec->cmdline, &pwszCommand, ppwszUserName, ppwszCommandLine, pbRunInteractively);
    if (ERROR_SUCCESS != uResult) {
        errorf("handle_exec_common: ParseUtf8Command failed\n");
        free(exec);
        send_exit_code(*pVchan, MAKE_ERROR_RESPONSE(ERROR_SET_WINDOWS, uResult));
        *pVchan = NULL;
        return TRUE;
    }

    free(exec);

    // pwszServiceCommandLine and pwszRemoteDomainName are allocated in the call
    uResult = InterceptRPCRequest(*ppwszCommandLine, &pwszServiceCommandLine, &pwszRemoteDomainName);
    if (ERROR_SUCCESS != uResult) {
        errorf("handle_exec_common: InterceptRPCRequest failed\n");
        send_exit_code(*pVchan, MAKE_ERROR_RESPONSE(ERROR_SET_WINDOWS, uResult));
        *pVchan = NULL;
        free(pwszCommand);
        return TRUE;
    }
    
    if (pwszRemoteDomainName) {
        SetEnvironmentVariableW(L"QREXEC_REMOTE_DOMAIN", pwszRemoteDomainName);
        free(pwszRemoteDomainName);
    }

    if (pwszServiceCommandLine)
    {
        *ppwszCommandLine = pwszServiceCommandLine;
    }
    else
    {
        // so caller can always free this
        *ppwszCommandLine = _wcsdup(*ppwszCommandLine);
    }

    *ppwszUserName = _wcsdup(*ppwszUserName);
    free(pwszCommand);

    return TRUE;
}

// This will return error only if vchan fails.
ULONG handle_exec(struct msg_header *hdr)
{
    ULONG uResult;
    PWCHAR pwszUserName = NULL;
    PWCHAR pwszCommandLine = NULL;
    BOOL bRunInteractively;
    libvchan_t *vchan = NULL;

    debugf("handle_exec: msg 0x%x, len %d\n", hdr->type, hdr->len);

    if (!handle_exec_common(hdr->len, &pwszUserName, &pwszCommandLine, &bRunInteractively, &vchan))
        return ERROR_INVALID_FUNCTION;

    if (!vchan) // cmdline parsing failed
        return ERROR_SUCCESS;

    // Create a process and redirect its console IO to vchan.
    uResult = CreateChild(vchan, pwszUserName, pwszCommandLine, bRunInteractively);
    if (ERROR_SUCCESS == uResult)
        logf("handle_exec: Executed %s\n", pwszCommandLine);
    else {
        send_exit_code(vchan, MAKE_ERROR_RESPONSE(ERROR_SET_WINDOWS, uResult));
        errorf("handle_exec: CreateChild(%s) failed\n", pwszCommandLine);
    }

    free(pwszCommandLine);
    free(pwszUserName);

    return ERROR_SUCCESS;
}

// This will return error only if vchan fails.
ULONG handle_just_exec(struct msg_header *hdr)
{
    ULONG uResult;
    PWCHAR pwszUserName = NULL;
    PWCHAR pwszCommandLine = NULL;
    HANDLE hProcess;
    BOOL bRunInteractively;
    libvchan_t *vchan;

    debugf("handle_just_exec: msg 0x%x, len %d\n", hdr->type, hdr->len);

    if (!handle_exec_common(hdr->len, &pwszUserName, &pwszCommandLine, &bRunInteractively, &vchan))
        return ERROR_INVALID_FUNCTION;

    if (!vchan) // cmdline parsing failed
        return ERROR_SUCCESS;

    debugf("handle_just_exec: executing %s\n", pwszCommandLine);

#ifdef BUILD_AS_SERVICE
    // Create a process which IO is not redirected anywhere.
    uResult = CreateNormalProcessAsUser(
        pwszUserName,
        DEFAULT_USER_PASSWORD_UNICODE,
        pwszCommandLine,
        bRunInteractively,
        &hProcess);
#else
    uResult = CreateNormalProcessAsCurrentUser(pwszCommandLine, &hProcess);
#endif

    if (ERROR_SUCCESS == uResult) {
        CloseHandle(hProcess);
        logf("handle_just_exec: Executed (nowait) %s\n", pwszCommandLine);
    } else {
#ifdef BUILD_AS_SERVICE
        perror("CreateNormalProcessAsUser");
        errorf("handle_just_exec: CreateNormalProcessAsUser(%s) failed\n", pwszCommandLine);
#else
        perror("CreateNormalProcessAsCurrentUser");
        errorf("handle_just_exec: CreateNormalProcessAsCurrentUser(%s) failed\n", pwszCommandLine);
#endif
    }

    // send status to qrexec-client (not real *exit* code, but we can at least return that process creation failed)
    send_exit_code(vchan, MAKE_ERROR_RESPONSE(ERROR_SET_WINDOWS, uResult));

    free(pwszCommandLine);
    free(pwszUserName);

    return ERROR_SUCCESS;
}

void set_blocked_outerr(libvchan_t *vchan, BOOL bBlockOutput)
{
    CHILD_INFO *pChildInfo;

    pChildInfo = FindChildByVchan(vchan);
    if (!pChildInfo)
        return;

    pChildInfo->bReadingIsDisabled = bBlockOutput;
}

ULONG handle_daemon_hello(struct msg_header *hdr)
{
    struct peer_info info;

    if (hdr->len != sizeof(info))
    {
        errorf("handle_daemon_hello: hdr->len != sizeof(struct peer_info), protocol incompatible");
        return ERROR_INVALID_FUNCTION;
    }
    // read protocol version
    recv_from_vchan(g_daemon_vchan, &info, sizeof(info), "peer info");
    if (info.version != QREXEC_PROTOCOL_VERSION)
    {
        errorf("handle_daemon_hello: incompatible protocol version (%d instead of %d)\n",
            info.version, QREXEC_PROTOCOL_VERSION);
        return ERROR_INVALID_FUNCTION;
    }

    debugf("handle_daemon_hello: protocol version %d\n", info.version);

    return ERROR_SUCCESS;
}

// entry for all qrexec-daemon messages
ULONG handle_daemon_message()
{
    struct msg_header hdr;
    ULONG uResult;

    debugf("handle_daemon_message: data ready = %d\n", libvchan_data_ready(g_daemon_vchan));
    if (!recv_from_vchan(g_daemon_vchan, &hdr, sizeof(hdr), "daemon header")) {
        return ERROR_INVALID_FUNCTION;
    }

    debugf("handle_daemon_message: msg 0x%x, len %d\n", hdr.type, hdr.len);

    switch (hdr.type) {
    case MSG_HELLO:
        return handle_daemon_hello(&hdr);

    case MSG_SERVICE_CONNECT:
        return handle_service_connect(&hdr);

    case MSG_SERVICE_REFUSED:
        return handle_service_refused(&hdr);

    case MSG_EXEC_CMDLINE:
        // This will return error only if vchan fails.
        uResult = handle_exec(&hdr);
        if (ERROR_SUCCESS != uResult) {
            perror("handle_daemon_message: handle_exec");
            return uResult;
        }
        break;

    case MSG_JUST_EXEC:
        // This will return error only if vchan fails.
        uResult = handle_just_exec(&hdr);
        if (ERROR_SUCCESS != uResult) {
            perror("handle_daemon_message: handle_just_exec");
            return uResult;
        }
        break;

    default:
        errorf("handle_daemon_message: unknown message type: 0x%x\n", hdr.type);
        return ERROR_INVALID_FUNCTION;
    }

    return ERROR_SUCCESS;
}

// entry for all qrexec-client messages
ULONG handle_client_message(libvchan_t *vchan)
{
    struct msg_header hdr;
    ULONG uResult;

    debugf("handle_client_message(0x%x)\n", vchan);
    if (!recv_from_vchan(vchan, &hdr, sizeof(hdr), "client header")) {
        return ERROR_INVALID_FUNCTION;
    }

    switch (hdr.type) {
    /*
    case MSG_XON:
        debugf("MSG_XON\n");
        set_blocked_outerr(s_hdr.client_id, FALSE);
        break;
    case MSG_XOFF:
        debugf("MSG_XOFF\n");
        set_blocked_outerr(s_hdr.client_id, TRUE);
        break;
    */
    case MSG_DATA_STDIN:
        debugf("MSG_DATA_STDIN\n");

        // This will return error only if vchan fails.
        uResult = handle_input(&hdr, vchan);
        if (ERROR_SUCCESS != uResult) {
            perror("handle_client_message: handle_input");
            return uResult;
        }
        break;
    /*
    case MSG_SERVER_TO_AGENT_CLIENT_END:
        debugf("MSG_SERVER_TO_AGENT_CLIENT_END\n");
        RemoveChild(FindChildByVchan(vchan));
        break;
    */
    default:
        errorf("handle_client_message: unknown message type: 0x%x\n", hdr.type);
        return ERROR_INVALID_FUNCTION;
    }

    return ERROR_SUCCESS;
}

// read input from vchan (qrexec-client), send to child
// This will return error only if vchan fails.
ULONG handle_input(struct msg_header *hdr, libvchan_t *vchan)
{
    char *buf;
    CHILD_INFO *pChildInfo;
    DWORD dwWritten;

    debugf("handle_input(0x%x): msg 0x%x, len %d, vchan data ready %d\n",
        vchan, hdr->type, hdr->len, libvchan_data_ready(vchan));
    // If pChildInfo is NULL after this it means we couldn't find a specified client.
    // Read and discard any data in the channel in this case.
    pChildInfo = FindChildByVchan(vchan);

    if (!hdr->len) {
        debugf("handle_input: EOF from client 0x%x\n", vchan);
        if (pChildInfo) {
            CloseHandle(pChildInfo->hWriteStdinPipe);
            pChildInfo->bStdinPipeClosed = TRUE;
        }
        return ERROR_SUCCESS;
    }

    buf = (char*) malloc(hdr->len);
    if (!buf)
        return ERROR_NOT_ENOUGH_MEMORY;

    if (!recv_from_vchan(vchan, buf, hdr->len, "stdin data")) {
        free(buf);
        return ERROR_INVALID_FUNCTION;
    }

    // send to child
    if (pChildInfo && !pChildInfo->bStdinPipeClosed) {
        if (!WriteFile(pChildInfo->hWriteStdinPipe, buf, hdr->len, &dwWritten, NULL))
            perror("handle_input: WriteFile");
    }

    free(buf);
    return ERROR_SUCCESS;
}

// reads child io, returns number of filled events (0 or 1)
ULONG FillAsyncIoData(ULONG uEventNumber, ULONG uChildIndex, HANDLE_TYPE bHandleType, PIPE_DATA *pPipeData)
{
    ULONG uResult;

    if (uEventNumber >= RTL_NUMBER_OF(g_WatchedEvents) ||
        uChildIndex >= RTL_NUMBER_OF(g_Children) ||
        !pPipeData)
        return 0;

    if (!pPipeData->bReadInProgress && !pPipeData->bDataIsReady && !pPipeData->bPipeClosed && !pPipeData->bVchanWritePending) {
        memset(&pPipeData->ReadBuffer, 0, READ_BUFFER_SIZE);
        pPipeData->dwSentBytes = 0;

        if (!ReadFile(pPipeData->hReadPipe, &pPipeData->ReadBuffer, READ_BUFFER_SIZE, NULL, &pPipeData->olRead)) {
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
            // The event in the OVERLAPPED structure should be signaled by now.
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
        g_HandlesInfo[uEventNumber].uChildIndex = uChildIndex;
        g_HandlesInfo[uEventNumber].bType = bHandleType;
        g_WatchedEvents[uEventNumber] = pPipeData->olRead.hEvent;
        return 1;
    }

    return 0;
}

// main event loop for children, daemon and clients
ULONG WatchForEvents()
{
    HANDLE	vchan_event;
    ULONG	uEventCount, uChildIndex;
    DWORD	dwSignaledEvent;
    CHILD_INFO	*pChildInfo;
    DWORD	dwExitCode;
    ULONG	uResult;
    BOOL	bVchanReturnedError;
    BOOL	bDaemonConnected;
    libvchan_t *client_vchan;
    struct peer_info info;

    info.version = QREXEC_PROTOCOL_VERSION;

    g_daemon_vchan = vchan_server_init(VCHAN_BASE_PORT);
    if (!g_daemon_vchan) {
        perror("WatchForEvents: vchan_server_init(daemon)");
        return ERROR_INVALID_FUNCTION;
    }

    logf("WatchForEvents: waiting for daemon, vchan buffer size: %d\n", libvchan_buffer_space(g_daemon_vchan));

    bDaemonConnected = FALSE;
    bVchanReturnedError = FALSE;

    while (TRUE) {
        uEventCount = 0;

        debugf("WatchForEvents: loop start\n");
        // Order matters.
        g_WatchedEvents[uEventCount++] = g_hStopServiceEvent;
        g_WatchedEvents[uEventCount++] = g_hAddExistingClienEvent;

        g_HandlesInfo[0].bType = g_HandlesInfo[1].bType = HTYPE_INVALID;

        vchan_event = libvchan_fd_for_select(g_daemon_vchan);
        if (INVALID_HANDLE_VALUE == vchan_event)
        {
            perror("WatchForEvents: libvchan_fd_for_select");
            break;
        }

        g_HandlesInfo[uEventCount].uChildIndex = -1;
        g_HandlesInfo[uEventCount].bType = HTYPE_DAEMON_VCHAN;
        g_WatchedEvents[uEventCount++] = vchan_event;

        EnterCriticalSection(&g_ChildrenCriticalSection);

        // prepare child events
        for (uChildIndex = 0; uChildIndex < MAX_CHILDREN; uChildIndex++) {
            if (g_Children[uChildIndex].bChildIsReady) {
                // process exit event
                if (!g_Children[uChildIndex].bChildExited) {
                    g_HandlesInfo[uEventCount].uChildIndex = uChildIndex;
                    g_HandlesInfo[uEventCount].bType = HTYPE_PROCESS;
                    g_WatchedEvents[uEventCount++] = g_Children[uChildIndex].hProcess;
                }

                // event for associated qrexec-client vchan
                if (g_Children[uChildIndex].vchan)
                {
                    g_HandlesInfo[uEventCount].bType = HTYPE_CLIENT_VCHAN;
                    g_HandlesInfo[uEventCount].uChildIndex = uChildIndex;
                    g_WatchedEvents[uEventCount++] = libvchan_fd_for_select(g_Children[uChildIndex].vchan);
                }

                // Skip those clients which have received MSG_XOFF.
                if (!g_Children[uChildIndex].bReadingIsDisabled) {
                    // process output from child
                    uEventCount += FillAsyncIoData(uEventCount, uChildIndex, HTYPE_STDOUT, &g_Children[uChildIndex].Stdout);
                    uEventCount += FillAsyncIoData(uEventCount, uChildIndex, HTYPE_STDERR, &g_Children[uChildIndex].Stderr);
                }
            }
        }
        LeaveCriticalSection(&g_ChildrenCriticalSection);

        debugf("WatchForEvents: waiting (%d events)\n", uEventCount);
        dwSignaledEvent = WaitForMultipleObjects(uEventCount, g_WatchedEvents, FALSE, INFINITE);

        //debugf("signaled\n");

        if (dwSignaledEvent > MAXIMUM_WAIT_OBJECTS) {
            perror("WatchForEvents: WaitForMultipleObjects");
            logf("WatchForEvents: dwSignaledEvent (%d) >= MAXIMUM_WAIT_OBJECTS\n", dwSignaledEvent);

            uResult = GetLastError();
            if (ERROR_INVALID_HANDLE != uResult) {
                perror("WatchForEvents: WaitForMultipleObjects");
                break;
            }

            // WaitForMultipleObjects() may fail with ERROR_INVALID_HANDLE if the process which just has been added
            // to the client list terminated before WaitForMultipleObjects(). In this case IO pipe handles are closed
            // and invalidated, while a process handle is in the signaled state.
            // Check if any of the processes in the client list is terminated, remove it from the list and try again.

            debugf("WatchForEvents: removing terminated clients\n");
            EnterCriticalSection(&g_ChildrenCriticalSection);

            for (uChildIndex = 0; uChildIndex < MAX_CHILDREN; uChildIndex++) {
                pChildInfo = &g_Children[uChildIndex];

                if (!g_Children[uChildIndex].bChildIsReady)
                    continue;

                if (!GetExitCodeProcess(pChildInfo->hProcess, &dwExitCode)) {
                    perror("WatchForEvents: GetExitCodeProcess");
                    dwExitCode = ERROR_SUCCESS;
                }

                if (STILL_ACTIVE != dwExitCode) {
                    pChildInfo->bChildExited = TRUE;
                    pChildInfo->dwExitCode = dwExitCode;
                    // send exit code only when all data was sent to the daemon
                    uResult = PossiblyHandleTerminatedChildNoLocks(pChildInfo);
                    if (ERROR_SUCCESS != uResult) {
                        bVchanReturnedError = TRUE;
                        perror("WatchForEvents: send_exit_code");
                    }
                }
            }
            LeaveCriticalSection(&g_ChildrenCriticalSection);

            continue;
        } else {
            if (0 == dwSignaledEvent)
                // g_hStopServiceEvent is signaled
                break;

            if (1 == dwSignaledEvent)
                // g_hAddExistingClientEvent is signaled. Since vchan IO has been canceled,
                // safely re-iterate the loop and pick up new handles to watch.
                continue;

            // Do not have to lock g_Children here because other threads may only call
            // ReserveChildNumber()/ReleaseChildNumber()/AddFilledChildInfo()
            // which operate on different indices than those specified for WaitForMultipleObjects().

            // The other threads cannot call RemoveChild(), for example, they
            // operate only on newly allocated indices.

            // So here in this thread we may call FindChildByVchan() with no locks safely.

            // When this thread (in this switch) calls RemoveChild() later the g_Children
            // array will be locked as usual.

            debugf("child %d, type %d, signaled index %d\n",
                g_HandlesInfo[dwSignaledEvent].uChildIndex, g_HandlesInfo[dwSignaledEvent].bType, dwSignaledEvent);

            switch (g_HandlesInfo[dwSignaledEvent].bType) {
                case HTYPE_DAEMON_VCHAN:

                    EnterCriticalSection(&g_DaemonCriticalSection);
                    if (!bDaemonConnected) {
                        libvchan_wait(g_daemon_vchan); // ACK
                        logf("WatchForEvents: qrexec-daemon has connected (event %d)\n", dwSignaledEvent);

                        if (ERROR_SUCCESS != send_msg_to_vchan(g_daemon_vchan, MSG_HELLO, &info, sizeof(info), NULL))
                        {
                            errorf("WatchForEvents: failed to send hello to daemon\n");
                            bVchanReturnedError = TRUE;
                            LeaveCriticalSection(&g_DaemonCriticalSection);
                            break;
                        }
                        bDaemonConnected = TRUE;
                        LeaveCriticalSection(&g_DaemonCriticalSection);
                        break;
                    }

                    if (!libvchan_is_open(g_daemon_vchan)) {
                        bVchanReturnedError = TRUE;
                        LeaveCriticalSection(&g_DaemonCriticalSection);
                        break;
                    }

                    // libvchan_wait can block if there is no data available
                    if (libvchan_data_ready(g_daemon_vchan) > 0)
                    {
                        debugf("HTYPE_DAEMON_VCHAN event: libvchan_wait...");
                        libvchan_wait(g_daemon_vchan);
                        debugf("done\n");

                        // handle data from daemon
                        while (libvchan_data_ready(g_daemon_vchan) > 0) {
                            uResult = handle_daemon_message();
                            if (ERROR_SUCCESS != uResult) {
                                bVchanReturnedError = TRUE;
                                perror("WatchForEvents: handle_daemon_message");
                                LeaveCriticalSection(&g_DaemonCriticalSection);
                                break;
                            }
                        }
                    }
                    else
                    {
                        debugf("HTYPE_DAEMON_VCHAN event: no data\n");
                        LeaveCriticalSection(&g_DaemonCriticalSection);
                        break;
                    }

                    LeaveCriticalSection(&g_DaemonCriticalSection);
                    break;

                case HTYPE_CLIENT_VCHAN:
                    // FIXME: critical section for client vchans?
                    client_vchan = g_Children[g_HandlesInfo[dwSignaledEvent].uChildIndex].vchan;

                    if (!libvchan_is_open(client_vchan)) {
                        bVchanReturnedError = TRUE;
                        break;
                    }

                    // libvchan_wait can block if there is no data available
                    if (libvchan_data_ready(client_vchan) > 0)
                    {
                        debugf("HTYPE_CLIENT_VCHAN event: libvchan_wait...");
                        libvchan_wait(client_vchan);
                        debugf("done\n");
                        // handle data from qrexec-client
                        while (libvchan_data_ready(client_vchan) > 0) {
                            uResult = handle_client_message(client_vchan);
                            if (ERROR_SUCCESS != uResult) {
                                bVchanReturnedError = TRUE;
                                perror("WatchForEvents: handle_input");
                                break;
                            }
                        }
                    }
                    else
                    {
                        debugf("HTYPE_CLIENT_VCHAN event: no data\n");
                    }

                    // if there is pending output from children, pass it to qrexec-client vchan
                    EnterCriticalSection(&g_ChildrenCriticalSection);

                    for (uChildIndex = 0; uChildIndex < MAX_CHILDREN; uChildIndex++) {
                        if (g_Children[uChildIndex].bChildIsReady && !g_Children[uChildIndex].bReadingIsDisabled) {
                            if (g_Children[uChildIndex].Stdout.bVchanWritePending) {
                                uResult = ReturnPipeData(g_Children[uChildIndex].vchan, &g_Children[uChildIndex].Stdout);
                                if (ERROR_HANDLE_EOF == uResult) {
                                    PossiblyHandleTerminatedChildNoLocks(&g_Children[uChildIndex]);
                                } else if (ERROR_INSUFFICIENT_BUFFER == uResult) {
                                    // no more space in vchan
                                    break;
                                } else if (ERROR_SUCCESS != uResult) {
                                    bVchanReturnedError = TRUE;
                                    perror("WatchForEvents: ReturnPipeData(STDOUT)");
                                }
                            }
                            if (g_Children[uChildIndex].Stderr.bVchanWritePending) {
                                uResult = ReturnPipeData(g_Children[uChildIndex].vchan, &g_Children[uChildIndex].Stderr);
                                if (ERROR_HANDLE_EOF == uResult) {
                                    PossiblyHandleTerminatedChildNoLocks(&g_Children[uChildIndex]);
                                } else if (ERROR_INSUFFICIENT_BUFFER == uResult) {
                                    // no more space in vchan
                                    break;
                                } else if (ERROR_SUCCESS != uResult) {
                                    bVchanReturnedError = TRUE;
                                    perror("WatchForEvents: ReturnPipeData(STDERR)");
                                }
                            }
                        }
                    }

                    LeaveCriticalSection(&g_ChildrenCriticalSection);

                    break;

                case HTYPE_STDOUT:
                    // output from child
#ifdef DISPLAY_CONSOLE_OUTPUT
#ifdef UNICODE
                    _tprintf("%S", &g_Children[g_HandlesInfo[dwSignaledEvent].uChildIndex].Stdout.ReadBuffer);
#else
                    _tprintf("%s", &g_Children[g_HandlesInfo[dwSignaledEvent].uChildIndex].Stdout.ReadBuffer);
#endif
#endif
                    // pass to vchan
                    uResult = ReturnPipeData(
                            g_Children[g_HandlesInfo[dwSignaledEvent].uChildIndex].vchan,
                            &g_Children[g_HandlesInfo[dwSignaledEvent].uChildIndex].Stdout);
                    if (ERROR_HANDLE_EOF == uResult) {
                        PossiblyHandleTerminatedChild(&g_Children[g_HandlesInfo[dwSignaledEvent].uChildIndex]);
                    } else if (ERROR_SUCCESS != uResult && ERROR_INSUFFICIENT_BUFFER != uResult) {
                        bVchanReturnedError = TRUE;
                        perror("WatchForEvents: ReturnPipeData(STDOUT)");
                    }
                    break;

                case HTYPE_STDERR:
                    // stderr from child
#ifdef DISPLAY_CONSOLE_OUTPUT
#ifdef UNICODE
                    _ftprintf(stderr, "%S", &g_Children[g_HandlesInfo[dwSignaledEvent].uChildIndex].Stderr.ReadBuffer);
#else
                    _ftprintf(stderr, "%s", &g_Children[g_HandlesInfo[dwSignaledEvent].uChildIndex].Stderr.ReadBuffer);
#endif
#endif
                    // pass to vchan
                    uResult = ReturnPipeData(
                            g_Children[g_HandlesInfo[dwSignaledEvent].uChildIndex].vchan,
                            &g_Children[g_HandlesInfo[dwSignaledEvent].uChildIndex].Stderr);
                    if (ERROR_HANDLE_EOF == uResult) {
                        PossiblyHandleTerminatedChild(&g_Children[g_HandlesInfo[dwSignaledEvent].uChildIndex]);
                    } else if (ERROR_SUCCESS != uResult && ERROR_INSUFFICIENT_BUFFER != uResult) {
                        bVchanReturnedError = TRUE;
                        perror("WatchForEvents: ReturnPipeData(STDERR)");
                    }
                    break;

                case HTYPE_PROCESS:
                    // child process exited
                    pChildInfo = &g_Children[g_HandlesInfo[dwSignaledEvent].uChildIndex];

                    if (!GetExitCodeProcess(pChildInfo->hProcess, &dwExitCode)) {
                        perror("WatchForEvents: GetExitCodeProcess");
                        dwExitCode = ERROR_SUCCESS;
                    }

                    pChildInfo->bChildExited = TRUE;
                    pChildInfo->dwExitCode = dwExitCode;
                    // send exit code only when all data was sent to the daemon
                    uResult = PossiblyHandleTerminatedChild(pChildInfo);
                    if (ERROR_SUCCESS != uResult) {
                        bVchanReturnedError = TRUE;
                        perror("WatchForEvents: send_exit_code");
                    }

                    break;

                default:
                    errorf("WatchForEvents: invalid handle type %d for event %d\n",
                        g_HandlesInfo[dwSignaledEvent].bType, dwSignaledEvent);
                    break;
            }
        }

        if (bVchanReturnedError)
        {
            errorf("vchan error\n");
            break;
        }
    }

    RemoveAllChildren();

    if (bDaemonConnected)
        libvchan_close(g_daemon_vchan);

    return bVchanReturnedError ? ERROR_INVALID_FUNCTION : ERROR_SUCCESS;
}

VOID Usage()
{
    _ftprintf(stderr, TEXT("qrexec agent service\n\nUsage: qrexec-agent <-i|-u>\n"));
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

    logf("ServiceExecutionThread: Service started\n");

    // Auto reset, initial state is not signaled
    g_hAddExistingClienEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_hAddExistingClienEvent) {
        uResult = GetLastError();
        perror("ServiceExecutionThread: CreateEvent");
        return uResult;
    }

    hTriggerEventsThread = CreateThread(NULL, 0, WatchForTriggerEvents, NULL, 0, NULL);
    if (!hTriggerEventsThread) {
        uResult = GetLastError();
        perror("ServiceExecutionThread: CreateThread");
        CloseHandle(g_hAddExistingClienEvent);
        return uResult;
    }

    while (TRUE) {
        uResult = WatchForEvents();
        if (ERROR_SUCCESS != uResult)
            perror("ServiceExecutionThread: WatchForEvents");

        if (!WaitForSingleObject(g_hStopServiceEvent, 0)) // don't wait, just check if it's signaled
            break;

        Sleep(10);
    }

    debugf("ServiceExecutionThread: Waiting for the trigger thread to exit\n");
    WaitForSingleObject(hTriggerEventsThread, INFINITE);
    CloseHandle(hTriggerEventsThread);
    CloseHandle(g_hAddExistingClienEvent);

    DeleteCriticalSection(&g_ChildrenCriticalSection);
    DeleteCriticalSection(&g_DaemonCriticalSection);

    logf("ServiceExecutionThread: Shutting down\n");

    return ERROR_SUCCESS;
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
        perror("Init: CheckForXenInterface");
        ReportErrorToEventLog(XEN_INTERFACE_NOT_FOUND);
        return ERROR_NOT_SUPPORTED;
    }

    // InitializeCriticalSection always succeeds in Vista and later OSes.
    InitializeCriticalSection(&g_ChildrenCriticalSection);
    InitializeCriticalSection(&g_DaemonCriticalSection);
    InitializeCriticalSection(&g_PipesCriticalSection);

    for (uClientNumber = 0; uClientNumber < MAX_CHILDREN; uClientNumber++)
        g_Children[uClientNumber].client_id = FREE_CLIENT_SPOT_ID;

    hThread = CreateThread(NULL, 0, ServiceExecutionThread, NULL, 0, NULL);
    if (!hThread) {
        uResult = GetLastError();
        perror("StartServiceThread: CreateThread");
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

    log_init(NULL, TEXT("qrexec-agent")); // use default log dir

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
            errorf("main(): GrantDesktopAccess(\"%s\") failed\n", pszAccountName);
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

#else /* BUILD_AS_SERVICE */

// Is not called when built without BUILD_AS_SERVICE definition.
ULONG Init(HANDLE *phServiceThread)
{
    return ERROR_SUCCESS;
}

BOOL WINAPI CtrlHandler(DWORD fdwCtrlType)
{
    logf("CtrlHandler: Got shutdown signal\n");

    SetEvent(g_hStopServiceEvent);

    WaitForSingleObject(g_hCleanupFinishedEvent, 2000);

    CloseHandle(g_hStopServiceEvent);
    CloseHandle(g_hCleanupFinishedEvent);

    logf("CtrlHandler: Shutdown complete\n");
    ExitProcess(0);
    return TRUE;
}

// This is the entry point for a console application (BUILD_AS_SERVICE not defined).
int __cdecl _tmain(ULONG argc, TCHAR *argv[])
{
    ULONG	uClientNumber;

    log_init(NULL, TEXT("qrexec-agent"));
    logf("qrexec agent start (console app)\n");

    if (ERROR_SUCCESS != CheckForXenInterface()) {
        errorf("main: Could not find Xen interface\n");
        return ERROR_NOT_SUPPORTED;
    }

    // Manual reset, initial state is not signaled
    g_hStopServiceEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!g_hStopServiceEvent) {
        perror("main: CreateEvent");
        return 1;
    }

    // Manual reset, initial state is not signaled
    g_hCleanupFinishedEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!g_hCleanupFinishedEvent) {
        perror("main: CreateEvent");
        CloseHandle(g_hStopServiceEvent);
        return 1;
    }

    InitializeCriticalSection(&g_ChildrenCriticalSection);
    InitializeCriticalSection(&g_DaemonCriticalSection);
    InitializeCriticalSection(&g_PipesCriticalSection);

    if (!SetConsoleCtrlHandler(CtrlHandler, TRUE))
        perror("main: SetConsoleCtrlHandler");

    for (uClientNumber = 0; uClientNumber < MAX_CHILDREN; uClientNumber++)
        g_Children[uClientNumber].vchan = NULL;

    ServiceExecutionThread(NULL);
    SetEvent(g_hCleanupFinishedEvent);

    return 0;
}
#endif /* BUILD_AS_SERVICE */
