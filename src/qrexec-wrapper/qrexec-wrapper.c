/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (c) Invisible Things Lab
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

// This program is launched by qrexec agent as a wrapper for arbitrary executable
// (qrexec services or just anything).
// It's role is communication with the data vchan peer and handling child program's I/O.

#include "qrexec-wrapper.h"
#include <stdlib.h>
#include <shlwapi.h>
#include <assert.h>
#include <strsafe.h>

#include <libvchan.h>
#include <qrexec.h>

#include <log.h>
#include <vchan-common.h>
#include <exec.h>
#include <utf8-conv.h>
#include <qubes-io.h>

static CRITICAL_SECTION g_VchanCs;
static BOOL g_exitCodeReceived = FALSE;

/**
 * @brief Create an anonymous pipe that will be used as one of the std handles for a child process.
 * @param pipeData Pipe data to initialize.
 * @param pipeType Pipe type.
 * @param securityDescriptor Security descriptor for the pipe.
 * @return Error code.
 */
DWORD InitPipe(
    _Out_ PPIPE_DATA pipeData,
    _In_ PIPE_TYPE pipeType,
    _In_ PSECURITY_DESCRIPTOR securityDescriptor
    )
{
    SECURITY_ATTRIBUTES sa = { 0 };

    assert(pipeData);

    LogVerbose("pipe type %d", pipeType);

    ZeroMemory(pipeData, sizeof(*pipeData));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = securityDescriptor;

    if (!CreatePipe(&pipeData->ReadEndpoint, &pipeData->WriteEndpoint, &sa, PIPE_BUFFER_SIZE))
        return win_perror("CreatePipe");

    return ERROR_SUCCESS;
}

/**
 * @brief Close given pipes and set them to NULL.
 * @param pipeData Pipe data to close.
 */
void ClosePipe(
    _Inout_ PPIPE_DATA pipeData
    )
{
    assert(pipeData);

    if (pipeData->ReadEndpoint)
    {
        LogDebug("closed read pipe %p", pipeData->ReadEndpoint);
        CloseHandle(pipeData->ReadEndpoint);
        pipeData->ReadEndpoint = NULL;
    }

    if (pipeData->WriteEndpoint)
    {
        LogDebug("closed write pipe %p", pipeData->WriteEndpoint);
        CloseHandle(pipeData->WriteEndpoint);
        pipeData->WriteEndpoint = NULL;
    }
}

/**
* @brief Create pipes that will be used as std* handles for the child process.
* @param child Child state.
* @return Error code.
*/
DWORD CreateChildPipes(
    _Inout_ PCHILD_STATE child
    )
{
    DWORD status;

    assert(child);

    LogVerbose("start");

    // create pipes and make sure endpoints we use are not inherited
    status = InitPipe(&child->Stdout, PTYPE_STDOUT, child->PipeSd);
    if (ERROR_SUCCESS != status)
        return win_perror2(status, "InitPipe(STDOUT)");

    SetHandleInformation(child->Stdout.ReadEndpoint, HANDLE_FLAG_INHERIT, 0);

    status = InitPipe(&child->Stderr, PTYPE_STDERR, child->PipeSd);
    if (ERROR_SUCCESS != status)
    {
        ClosePipe(&child->Stdout);
        return win_perror2(status, "InitPipe(STDERR)");
    }

    SetHandleInformation(child->Stderr.ReadEndpoint, HANDLE_FLAG_INHERIT, 0);

    status = InitPipe(&child->Stdin, PTYPE_STDIN, child->PipeSd);
    if (ERROR_SUCCESS != status)
    {
        ClosePipe(&child->Stdout);
        ClosePipe(&child->Stderr);
        return win_perror2(status, "InitPipe(STDIN)");
    }

    SetHandleInformation(child->Stdin.WriteEndpoint, HANDLE_FLAG_INHERIT, 0);

    return ERROR_SUCCESS;
}

/**
* @brief Create the child process that's optionally piped with data peer's vchan for i/o exchange.
* @param child Child state.
* @param userName User name to run the child as. If NULL, this process' user token will be used (normally SYSTEM).
* @param commandLine Command line of the child process.
* @param interactive Run the child in the interactive session (a user must be logged in).
* @param piped Connect the child's standard I/O handles to pipes.
* @return Error code.
*/
DWORD StartChild(
    _Inout_ PCHILD_STATE child,
    _In_opt_ const PWSTR userName,
    _Inout_ PWSTR commandLine, // CreateProcess* can modify this
    _In_ BOOL interactive,
    _In_ BOOL piped
    )
{
    DWORD status;

    assert(child);
    assert(commandLine);

    LogDebug("user '%s', cmd '%s', interactive %d, piped %d", userName, commandLine, interactive, piped);

    // if userName is NULL we run the process on behalf of the current user.
    if (userName)
        LogInfo("Running '%s' as user '%s'", commandLine, userName);
    else
        LogInfo("Running '%s' as SYSTEM", commandLine);

    if (piped)
    {
        status = CreateChildPipes(child);
        if (ERROR_SUCCESS != status)
            return win_perror2(status, "CreateChildPipes");
    }

    if (userName)
    {
        if (piped)
        {
            status = CreatePipedProcessAsUser(
                userName,
                DEFAULT_USER_PASSWORD_UNICODE, // password will only be required if the requested user is not logged on
                commandLine,
                interactive,
                child->Stdin.ReadEndpoint, // child will use stdin for reading and stdout/stderr for writing
                child->Stdout.WriteEndpoint,
                child->Stderr.WriteEndpoint,
                &child->Process);

            if (ERROR_SUCCESS != status)
            {
                win_perror2(status, "CreatePipedProcessAsUser");
                status = CreatePipedProcessAsCurrentUser(
                    commandLine,
                    interactive,
                    child->Stdin.ReadEndpoint,
                    child->Stdout.WriteEndpoint,
                    child->Stderr.WriteEndpoint,
                    &child->Process);
            }
        }
        else // piped
        {
            status = CreateNormalProcessAsUser(
                userName,
                DEFAULT_USER_PASSWORD_UNICODE,
                commandLine,
                interactive,
                &child->Process);

            if (ERROR_SUCCESS != status)
            {
                win_perror2(status, "CreateNormalProcessAsUser");
                status = CreateNormalProcessAsCurrentUser(
                    commandLine,
                    &child->Process);
            }
        }
    }
    else // userName
    {
        if (piped)
        {
            status = CreatePipedProcessAsCurrentUser(
                commandLine,
                interactive,
                child->Stdin.ReadEndpoint,
                child->Stdout.WriteEndpoint,
                child->Stderr.WriteEndpoint,
                &child->Process);
        }
        else
        {
            status = CreateNormalProcessAsCurrentUser(
                commandLine,
                &child->Process);
        }
    }

    if (piped)
    {
        // we won't be using these pipe endpoints, only the child will
        CloseHandle(child->Stdin.ReadEndpoint);
        child->Stdin.ReadEndpoint = NULL;
        CloseHandle(child->Stdout.WriteEndpoint);
        child->Stdout.WriteEndpoint = NULL;
        CloseHandle(child->Stderr.WriteEndpoint);
        child->Stderr.WriteEndpoint = NULL;
    }

    if (ERROR_SUCCESS != status)
    {
        if (piped)
        {
            // close *our* endpoints
            CloseHandle(child->Stdin.WriteEndpoint);
            CloseHandle(child->Stdout.ReadEndpoint);
            CloseHandle(child->Stderr.ReadEndpoint);
            return win_perror2(status, "CreatePipedProcessAsCurrentUser");
        }

        return win_perror2(status, "CreateNormalProcessAsCurrentUser");
    }

    return ERROR_SUCCESS;
}

/**
 * @brief Send message to the vchan peer.
 * @param vchan Data vchan.
 * @param messageType Data message type (MSG_DATA_*).
 * @param data Buffer to send.
 * @param cbData Size of the @a data buffer, in bytes.
 * @param what Description of the buffer (for logging).
 * @return TRUE on success.
 */
BOOL VchanSendMessage(
    _Inout_ libvchan_t *vchan,
    _In_ ULONG messageType,
    _In_reads_bytes_opt_(cbData) const void *data,
    _In_ ULONG cbData,
    _In_ const PWSTR what
    )
{
    struct msg_header header;
    BOOL status = FALSE;

    assert(vchan);

    LogDebug("msg 0x%x, data %p, size %u (%s)", messageType, data, cbData, what);

    header.type = messageType;
    header.len = cbData;
    EnterCriticalSection(&g_VchanCs);

    if (!libvchan_is_open(vchan))
    {
        goto cleanup;
    }
    if (!VchanSendBuffer(vchan, &header, sizeof(header), L"header"))
    {
        LogError("VchanSendBuffer(header for %s) failed", what);
        goto cleanup;
    }

    status = TRUE;
    if (cbData == 0) // EOF
        goto cleanup;

    status = FALSE;
    if (!VchanSendBuffer(vchan, data, cbData, what))
    {
        LogError("VchanSendBuffer(%s) failed", what);
        goto cleanup;
    }
    status = TRUE;

cleanup:
    LeaveCriticalSection(&g_VchanCs);
    return status;
}

/**
 * @brief Send child output to the vchan data peer.
 * @param child Child state.
 * @param data Stdout data to send.
 * @param cbData Size of the @a data buffer, in bytes.
 * @param pipeType Pipe type (stdout/stderr).
 * @return TRUE on success.
 */
BOOL VchanSendData(
    _In_ const PCHILD_STATE child,
    _In_reads_bytes_opt_(cbData) const BYTE *data,
    _In_ DWORD cbData,
    _In_ PIPE_TYPE pipeType
    )
{
    ULONG messageType;

    assert(child && child->Vchan);
    if (!child || !child->Vchan)
        return FALSE;

    LogVerbose("data %p, size %lu, type %d", data, cbData, pipeType);

    switch (pipeType)
    {
    case PTYPE_STDOUT:
        messageType = MSG_DATA_STDOUT;
        break;
    case PTYPE_STDERR:
        messageType = MSG_DATA_STDERR;
        break;
    default:
        LogError("invalid pipe type %d", pipeType);
        return FALSE;
    }

    return VchanSendMessage(child->Vchan, messageType, data, cbData, L"output data");
}

/**
 * @brief Send MSG_DATA_EXIT_CODE to the vchan peer if we're not the vchan server.
 * @param child Child state.
 * @param exitCode Exit code.
 * @return TRUE on success.
 */
BOOL VchanSendExitCode(
    _In_ const PCHILD_STATE child,
    _In_ int exitCode
    )
{
    assert(child && child->Vchan);

    LogVerbose("code %d", exitCode);

    // don't send anything if we're the vchan server.
    if (child->IsVchanServer)
        return TRUE;

    // EOF should be sent before exit code because peer closes vchan after receiving exit code
    LogDebug("sending stderr EOF");
    VchanSendData(child, NULL, 0, PTYPE_STDERR);
    LogDebug("sending stdout EOF");
    VchanSendData(child, NULL, 0, PTYPE_STDOUT);

    if (!VchanSendMessage(child->Vchan, MSG_DATA_EXIT_CODE, &exitCode, sizeof(exitCode), L"exit code"))
        return FALSE;

    LogDebug("Sent exit code %d", exitCode);
    return TRUE;
}

/**
 * @brief Send MSG_HELLO to the vchan peer.
 * @param vchan Data vchan.
 * @return TRUE on success.
 */
BOOL VchanSendHello(
    _Inout_ libvchan_t *vchan
    )
{
    struct peer_info info;

    assert(vchan);

    info.version = QREXEC_PROTOCOL_VERSION;

    return VchanSendMessage(vchan, MSG_HELLO, &info, sizeof(info), L"hello");
}

/**
 * @brief Read stdin/stdout/stderr from data vchan. Send to child's stdin or just log if stderr.
 * @param header Vchan message header that was already read.
 * @param child Child state.
 * @return Error code.
 */
DWORD HandleRemoteData(
    _In_ const struct msg_header *header,
    _Inout_ PCHILD_STATE child
    )
{
    static void *buffer = NULL;
    DWORD status = ERROR_UNIDENTIFIED_ERROR;

    assert(header);
    assert(child && child->Vchan);

    LogVerbose("msg 0x%x, len %d, vchan data ready %d",
               header->type, header->len, VchanGetReadBufferSize(child->Vchan));

    if (!buffer)
    {
        status = ERROR_NOT_ENOUGH_MEMORY;
        buffer = malloc(MAX_DATA_CHUNK);
        if (!buffer)
            goto cleanup;
    }

    status = ERROR_INVALID_FUNCTION;
    if (!VchanReceiveBuffer(child->Vchan, buffer, header->len, header->type == MSG_DATA_STDERR ? L"stderr data" : L"inbound data"))
        goto cleanup;

    if (header->type != MSG_DATA_STDERR)
    {
        assert(child->Stdin.WriteEndpoint);
        LogVerbose("writing %d bytes of inbound data to child", header->len);
        if (!QioWriteBuffer(child->Stdin.WriteEndpoint, buffer, header->len))
        {
            status = win_perror("writing stdin data");
            goto cleanup;
        }
    }
    else
    {
        // write to log file (also to our stderr)
        // FIXME: is this unicode or ascii or what? assuming ascii
        LogInfo("STDERR from vchan: %S", buffer);
    }

    status = ERROR_SUCCESS;

cleanup:
    return status;
}

/**
 * @brief Handle MSG_DATA_EXIT_CODE (we're the vchan server). Just log it.
 * @param child Child state.
 * @return Error code.
 */
DWORD HandleExitCode(
    _Inout_ PCHILD_STATE child
    )
{
    int code;

    assert(child && child->Vchan && child->IsVchanServer);

    if (!VchanReceiveBuffer(child->Vchan, &code, sizeof(code), L"peer exit code"))
        return ERROR_INVALID_FUNCTION;

    LogDebug("remote exit code: %d", code);

    g_exitCodeReceived = TRUE;
    return ERROR_SUCCESS;
}

/**
 * @brief Handle data vchan message. Read header and dispatch accordingly.
 * @param child Child state.
 * @return Error code.
 */
DWORD HandleDataMessage(
    _Inout_ PCHILD_STATE child
    )
{
    struct msg_header header;
    struct peer_info peerInfo;

    assert(child && child->Vchan);

    if (VchanGetReadBufferSize(child->Vchan) == 0)
    {
        LogVerbose("no data");
        return ERROR_SUCCESS;
    }

    LogDebug("is server %d", child->IsVchanServer);
    if (!VchanReceiveBuffer(child->Vchan, &header, sizeof(header), L"data header"))
    {
        LogError("VchanReceiveBuffer(header) failed");
        return ERROR_INVALID_FUNCTION;
    }

    if (header.len > MAX_DATA_CHUNK)
    {
        LogError("msg 0x%x, size too big: %d (max %d)", header.type, header.len, MAX_DATA_CHUNK);
        return ERROR_INVALID_FUNCTION;
    }

    // stdin and stdout messages are basically interchangeable depending on which role we're in (vchan server or client)
    if (header.len == 0) // EOF
    {
        if (header.type == MSG_DATA_STDIN || header.type == MSG_DATA_STDOUT)
        {
            LogDebug("EOF from vchan (msg 0x%x)", header.type);
            ClosePipe(&child->Stdin);
            return ERROR_SUCCESS;
        }
        if (header.type == MSG_DATA_STDERR)
        {
            LogDebug("stderr EOF from vchan (msg 0x%x)", header.type);
            return ERROR_SUCCESS;
        }
    }

    /*
    * qrexec-client is the vchan server
    * sends: MSG_HELLO, MSG_DATA_STDIN
    * expects: MSG_HELLO, MSG_DATA_STDOUT, MSG_DATA_STDERR, MSG_DATA_EXIT_CODE
    *
    * if CLIENT_INFO.IsVchanServer is set, we act as a qrexec-client (vchan server)
    * (service connection to another agent that is the usual vchan client)
    */

    switch (header.type)
    {
    case MSG_HELLO:
        LogVerbose("MSG_HELLO");
        if (!VchanReceiveBuffer(child->Vchan, &peerInfo, sizeof(peerInfo), L"peer info"))
            return ERROR_INVALID_FUNCTION;

        LogDebug("protocol version %d", peerInfo.version);

        if (peerInfo.version < QREXEC_PROTOCOL_VERSION)
        {
            LogWarning("incompatible protocol version (got %d, expected %d)", peerInfo.version, QREXEC_PROTOCOL_VERSION);
            return ERROR_INVALID_FUNCTION;
        }

        if (!child->IsVchanServer) // we're vchan client, reply with HELLO
        {
            if (!VchanSendHello(child->Vchan))
                return ERROR_INVALID_FUNCTION;
        }
        break;

    case MSG_DATA_STDIN:
        LogVerbose("MSG_DATA_STDIN");
        return HandleRemoteData(&header, child);

    case MSG_DATA_STDOUT:
        LogVerbose("MSG_DATA_STDOUT");
        return HandleRemoteData(&header, child);

    case MSG_DATA_STDERR:
        LogVerbose("MSG_DATA_STDERR");
        return HandleRemoteData(&header, child);

    case MSG_DATA_EXIT_CODE:
        LogVerbose("MSG_DATA_EXIT_CODE");
        return HandleExitCode(child);

    default:
        LogError("unknown message type: 0x%x", header.type);
        return ERROR_INVALID_PARAMETER;
    }

    return ERROR_SUCCESS;
}

/**
 * @brief Read data from child's pipe, send to vchan
 */
static DWORD handle_child_output(
    _Inout_ PCHILD_STATE child,
    _In_    PIPE_TYPE pipe_type
    )
{
    PPIPE_DATA pipe;
    PBYTE buffer;
    DWORD eof;

    assert(child);

    pipe = pipe_type == PTYPE_STDOUT ? &child->Stdout : &child->Stderr;

    buffer = malloc(MAX_DATA_CHUNK);
    if (!buffer)
    {
        LogError("no memory");
        goto cleanup;
    }

    LogVerbose("start");

    eof = FALSE;
    while (!eof)
    {
        DWORD nread, ok;

        LogVerbose("reading...");
        ok = ReadFile(pipe->ReadEndpoint, buffer, MAX_DATA_CHUNK,
                      &nread, NULL); // this can block
        //
        // EOF is signaled by either:
        // - ok and nread == 0
        // - !ok and GetLastError() == ERROR_BROKEN_PIPE.
        //
        // ReadFile of an anonymous pipe returns FALSE and GetLastError
        // returns ERROR_BROKE_PIPE when the corresponding write handle 
        // has been closed.
        if (!ok && GetLastError() != ERROR_BROKEN_PIPE)
        {
            // if !ok, then nread == 0
            // Signal error condition by sending EOF
            win_perror("ReadFile");
        }
        eof = nread == 0;
        // EOF is signaled to the remote via zero count
        LogVerbose("read %lu 0x%lx", nread, nread);
        if (!VchanSendData(child, buffer, nread, pipe_type))
        {
            LogError("VchanSendData failed");
            goto cleanup;
        }
    }

cleanup:
    ClosePipe(pipe);
    LogVerbose("exiting");
    free(buffer);
    return 1;
}

DWORD WINAPI StdoutThread(
    PVOID param
    )
{
    PCHILD_STATE child = param;

    return handle_child_output(child, PTYPE_STDOUT);
}

DWORD WINAPI StderrThread(
    PVOID param
    )
{
    PCHILD_STATE child = param;

    return handle_child_output(child, PTYPE_STDERR);
}

static void XifLogger(int level, const char *function, const WCHAR *format, va_list args)
{
    WCHAR buf[1024];

    StringCbVPrintfW(buf, sizeof(buf), format, args);
    _LogFormat(level, FALSE, function, buf);
}

/**
 * @brief Create data vchan connection to the remote peer. Send MSG_HELLO if acting as server.
 * @param domain Remote vchan domain.
 * @param port Remote vchan port.
 * @param isServer Determines if we're acting as the vchan server.
 * @return Pointer to the vchan structure or NULL if failed.
 */
_Ret_maybenull_
libvchan_t *InitVchan(
    _In_ int domain,
    _In_ int port,
    _In_ BOOL isServer
    )
{
    libvchan_t *vchan;

    if (isServer)
    {
        vchan = libvchan_server_init(domain, port, VCHAN_BUFFER_SIZE, VCHAN_BUFFER_SIZE);
        if (!vchan)
        {
            LogError("libvchan_server_init(%d, %d) failed", domain, port);
            return NULL;
        }

        LogVerbose("server vchan: %p, waiting for data client", vchan);
        if (libvchan_wait(vchan) < 0)
        {
            LogError("libvchan_wait(%p) failed", vchan);
            libvchan_close(vchan);
            return NULL;
        }

        LogVerbose("remote peer (data client) connected");
        if (!VchanSendHello(vchan))
        {
            LogError("SendHelloToVchan(%p) failed", vchan);
            libvchan_close(vchan);
            return NULL;
        }

        LogDebug("vchan %p: hello sent", vchan);
    }
    else
    {
        vchan = libvchan_client_init(domain, port);
    }

    return vchan;
}

/**
 * @brief Process vchan events, wait for process/threads exit.
 * @param child Child state.
 * @return Error code.
 */
DWORD EventLoop(
    _Inout_ PCHILD_STATE child
    )
{
    DWORD status = ERROR_NOT_ENOUGH_MEMORY;
    HANDLE waitObjects[2];
    DWORD signaled;
    BOOL run = TRUE;

    waitObjects[0] = libvchan_fd_for_select(child->Vchan);
    waitObjects[1] = child->Process;

    // event loop
    while (run)
    {
        LogVerbose("waiting");
        signaled = WaitForMultipleObjects(2, waitObjects, FALSE, INFINITE) - WAIT_OBJECT_0;

        status = ERROR_INVALID_FUNCTION;

        switch (signaled)
        {
        case 0: // vchan data ready or disconnected
        {
            if (!libvchan_is_open(child->Vchan))
            {
                LogDebug("vchan closed");
                run = FALSE;
                break;
            }

            while (VchanGetReadBufferSize(child->Vchan) > 0)
            {
                status = HandleDataMessage(child);
                if (status != ERROR_SUCCESS)
                    run = FALSE;
            }
            break;
        }

        case 1: // child process terminated
        {
            int exitCode;

            if (!GetExitCodeProcess(child->Process, &exitCode))
            {
                win_perror("GetExitCodeProcess");
                exitCode = 0;
            }

            LogDebug("child process exited with code %d", exitCode);
            if (!VchanSendExitCode(child, exitCode))
                LogError("sending exit code failed");

            status = ERROR_SUCCESS;
            CloseHandle(child->Process);
            child->Process = NULL;
            run = FALSE;
            break;
        }
        }
    }

    // wait for the threads to finish
    waitObjects[0] = child->StdoutThread;
    waitObjects[1] = child->StderrThread;

    WaitForMultipleObjects(2, waitObjects, TRUE, 100);

    CloseHandle(child->StdoutThread);
    child->StdoutThread = NULL;
    CloseHandle(child->StderrThread);
    child->StderrThread = NULL;

    return status;
}

/**
 * @brief Print usage info.
 * @param name Executable name.
 */
void Usage(
    _In_ const PWSTR name
    )
{
    wprintf(L"Usage: %s domain|port|user_name|flags|command_line\n", name);
    wprintf(L"domain:       remote domain for data vchan\n");
    wprintf(L"port:         remote port for data vchan\n");
    wprintf(L"user_name:    user name to use for the child process or (null) for current user\n");
    wprintf(L"flags:        bitmask of following possible values (decimal):\n");
    wprintf(L"         0x01 act as vchan server (default is client)\n");
    wprintf(L"         0x02 pipe child process' io to vchan (default is not)\n");
    wprintf(L"         0x04 run the child process in the interactive session (requires that a user is logged on)\n");
    wprintf(L"command_line: local program to execute and connect to data vchan or (null) if local program is not needed\n");
}

/**
 * @brief Entry point.
 * @param argc Number of command line arguments.
 * @param argv Expected arguments are: <domain> <port> <user_name> <flags> <command_line>
 *             domain:       remote domain for data vchan
 *             port:         remote port for data vchan
 *             user_name:    user name to use for the child process or (null) for current user
 *             flags:        bitmask of following possible values (decimal):
 *                      0x01 act as vchan server (default is client)
 *                      0x02 pipe child process' io to vchan (default is not)
 *                      0x04 run the child process in the interactive session (requires that a user is logged on)
 *             command_line: local program to execute and connect to data vchan
 * @return Error code.
 */
int __cdecl wmain(int argc, WCHAR *argv[])
{
    PCHILD_STATE child = NULL;
    int domain, port, flags;
    BOOL piped, interactive;
    PWSTR domainName, portStr, flagsStr, userName, commandLine;
    DWORD status = ERROR_NOT_ENOUGH_MEMORY;
    BOOL startLocalProcess = TRUE;

    LogVerbose("start");

    domainName = GetArgument();
    portStr = GetArgument();
    userName = GetArgument();
    flagsStr = GetArgument();
    commandLine = GetArgument();

    if (!domainName || !portStr || !userName || !flagsStr || !commandLine)
    {
        Usage(argv[0]);
        return ERROR_INVALID_PARAMETER;
    }

    child = malloc(sizeof(CHILD_STATE));
    if (!child)
        goto cleanup;

    ZeroMemory(child, sizeof(*child));

    InitializeCriticalSection(&g_VchanCs);
    libvchan_register_logger(XifLogger, LogGetLevel());

    domain = _wtoi(domainName);
    port = _wtoi(portStr);
    flags = _wtoi(flagsStr);

    child->IsVchanServer = !!(flags & 0x01);
    piped = !!(flags & 0x02);
    interactive = !!(flags & 0x04);

    LogDebug("domain %d, port %d, user %s, flags 0x%x, cmd '%s'", domain, port, userName, flags, commandLine);

    status = ERROR_INVALID_FUNCTION;
    child->Vchan = InitVchan(domain, port, child->IsVchanServer);
    if (!child->Vchan)
        goto cleanup;

    if (wcscmp(userName, L"(null)") == 0)
        userName = NULL;
    if (wcsncmp(commandLine, L"(null)", 6) == 0)
        startLocalProcess = FALSE;

    if (!startLocalProcess)
    {
        status = ERROR_SUCCESS;
        BOOL run = TRUE;
        while (run)
        {
            DWORD signaled = WaitForSingleObject(libvchan_fd_for_select(child->Vchan), INFINITE);
            if (signaled != WAIT_OBJECT_0)
            {
                status = (DWORD)-1;
                goto cleanup;
            }
            // vchan data ready or disconnected
            if (!libvchan_is_open(child->Vchan))
            {
                LogDebug("vchan closed");
                status = (DWORD)-1;
                break;
            }

            while (VchanGetReadBufferSize(child->Vchan) > 0)
            {
                status = HandleDataMessage(child);
                if (status != ERROR_SUCCESS)
                {
                    status = (DWORD)-2;
                    run = FALSE;
                    break;
                }
            }
            if (g_exitCodeReceived)
                break;
        }
        if (child)
        {
            if (child->Vchan)
            {
                libvchan_close(child->Vchan);
            }
            free(child);
            child = NULL;
        }
        goto cleanup;
    }

    status = CreatePublicPipeSecurityDescriptor(&child->PipeSd, &child->PipeAcl);
    if (ERROR_SUCCESS != status)
    {
        win_perror2(status, "create pipe security descriptor");
        goto cleanup;
    }

    status = StartChild(child, userName, commandLine, interactive, piped);
    if (ERROR_SUCCESS != status)
        goto cleanup;

    if (piped)
    {
        child->StdoutThread = CreateThread(NULL, 0, StdoutThread, child, 0, NULL);
        if (!child->StdoutThread)
        {
            status = win_perror("create stdout thread");
            goto cleanup;
        }

        child->StderrThread = CreateThread(NULL, 0, StderrThread, child, 0, NULL);
        if (!child->StderrThread)
        {
            status = win_perror("create stderr thread");
            goto cleanup;
        }

        status = EventLoop(child);
    }

cleanup:
    LogVerbose("exiting");
    DeleteCriticalSection(&g_VchanCs);

    if (child)
    {
        if (child->Vchan && libvchan_is_open(child->Vchan))
        {
            // send "exit code" (creation status really) if the io isn't piped or child creation failed
            if (!piped || status != ERROR_SUCCESS)
            {
                VchanSendHello(child->Vchan);
                VchanSendExitCode(child, status);
            }
            libvchan_close(child->Vchan);
        }
        free(child);
    }

    return status;
}
