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

#include <windows.h>
#include <stdlib.h>
#include <lmcons.h>
#include <shlwapi.h>
#include <assert.h>
#include <crtdbg.h>
#include <PathCch.h>
#include <strsafe.h>

#include "qrexec-agent.h"

#include <qrexec.h>
#include <libvchan.h>
#include <qubesdb-client.h>

#include <log.h>
#include <service.h>
#include <vchan-common.h>
#include <getopt.h>
#include <exec.h>
#include <utf8-conv.h>
#include <pipe-server.h>
#include <qubes-io.h>
#include <list.h>

libvchan_t *g_DaemonVchan;

CRITICAL_SECTION g_DaemonCriticalSection;
CRITICAL_SECTION g_RequestCriticalSection;
SRWLOCK g_ConnectionsHandlesLock;

PIPE_SERVER g_PipeServer = NULL; // for handling qrexec-client-vm requests
LIST_ENTRY g_RequestList; // pending service requests (local)
ULONG g_RequestId = 0;

#ifdef _DEBUG
void DumpRequestList(void)
{
    PLIST_ENTRY entry;

    LogDebug("Dumping requests");
    EnterCriticalSection(&g_RequestCriticalSection);
    entry = g_RequestList.Flink;
    while (entry != &g_RequestList)
    {
        PSERVICE_REQUEST context = (PSERVICE_REQUEST)CONTAINING_RECORD(entry, SERVICE_REQUEST, ListEntry);
        LogDebug("request %S, service %S, domain %S, user %s, cmd %s",
                 context->ServiceParams.request_id, context->ServiceParams.service_name,
            context->ServiceParams.target_domain, context->UserName, context->CommandLine);
        entry = entry->Flink;
    }
    LeaveCriticalSection(&g_RequestCriticalSection);
}
#endif

struct _connection_info
{
    HANDLE handle;
    int connect_domain;
    int connect_port;
};

#define MAX_FDS 128
static struct _connection_info connection_info[MAX_FDS];

/**
 * @brief Wait for qubesdb service to start.
 * @return TRUE if a connection could be opened, FALSE if timed out (60 seconds).
 */
BOOL WaitForQdb(void)
{
    qdb_handle_t qdb = NULL;
    ULONGLONG start = GetTickCount64();
    ULONGLONG tick = start;

    LogDebug("start");
    while (qdb == NULL && (tick - start) < 60 * 1000) // try for 60 seconds
    {
        qdb = qdb_open(NULL);
        if (qdb == NULL)
            Sleep(1000);
        tick = GetTickCount64();
    }

    if (qdb == NULL)
    {
        LogError("timed out: connect to qdb server");
        return FALSE;
    }

    qdb_close(qdb);
    LogDebug("qdb is running");
    return TRUE;
}

/**
 * @brief Send message to the vchan peer.
 * @param vchan Control vchan.
 * @param messageType Control message
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
    EnterCriticalSection(&g_DaemonCriticalSection);

    if (!VchanSendBuffer(vchan, &header, sizeof(header), L"header"))
    {
        LogError("VchanSendBuffer(header for %s) failed", what);
        goto cleanup;
    }

    if (cbData == 0) // EOF
    {
        status = TRUE;
        goto cleanup;
    }

    if (!VchanSendBuffer(vchan, data, cbData, what))
    {
        LogError("VchanSendBuffer(%s) failed", what);
        goto cleanup;
    }

    status = TRUE;

cleanup:
    LeaveCriticalSection(&g_DaemonCriticalSection);
    return status;
}

static void register_vchan_connection(HANDLE handle, int domain, int port)
{
    AcquireSRWLockExclusive(&g_ConnectionsHandlesLock);
    for (int i = 0; i < MAX_FDS; i++)
    {
        if (connection_info[i].handle == NULL)
        {
            LogVerbose("child %p, %d:%d", handle, domain, port);
            connection_info[i].handle = handle;
            connection_info[i].connect_domain = domain;
            connection_info[i].connect_port = port;
            ReleaseSRWLockExclusive(&g_ConnectionsHandlesLock);
            return;
        }
    }
    ReleaseSRWLockExclusive(&g_ConnectionsHandlesLock);
    LogError("No free slot for child %p (connection to %d:%d)", handle, domain, port);
    // FIXME: error
}

static void release_connection(int id)
{
    struct exec_params params;

    HANDLE handle = connection_info[id].handle;
    params.connect_domain = connection_info[id].connect_domain;
    params.connect_port = connection_info[id].connect_port;
    LogVerbose("child %p, %d:%d", handle, params.connect_domain, params.connect_port);

    // data size is just sizeof(struct exec_params) so no command line
    if (!VchanSendMessage(g_DaemonVchan, MSG_CONNECTION_TERMINATED, &params, sizeof(struct exec_params),
        L"terminate connection"))
    {
        LogError("Failed to send MSG_CONNECTION_TERMINATED for %d:%d", params.connect_domain, params.connect_port);
        // FIXME: error
    }

    CloseHandle(handle);
    connection_info[id].handle = NULL;
}

// based on https://stackoverflow.com/a/780024
// Caller must free the returned string.
WCHAR* StrReplace(WCHAR const * const original,
        WCHAR const * const pattern,
        WCHAR const * const replacement
    ) {
    size_t const replen = wcslen(replacement);
    size_t const patlen = wcslen(pattern);
    size_t const orilen = wcslen(original);

    size_t patcnt = 0;
    const WCHAR * oriptr;
    const WCHAR * patloc;

    WCHAR *returned = NULL;
    WCHAR *retptr = NULL;
    size_t retlen;

    // find how many times the pattern occurs in the original string
    for (oriptr = original; patloc = wcsstr(oriptr, pattern); oriptr = patloc + patlen)
    {
        patcnt++;
    }

    // allocate memory for the new string
    retlen = orilen + patcnt * (replen - patlen) + 1;
    returned = (WCHAR *) malloc( sizeof(WCHAR) * retlen);

    if (returned == NULL)
        goto fail;

    // copy the original string,
    // replacing all the instances of the pattern
    retptr = returned;
    for (oriptr = original; patloc = wcsstr(oriptr, pattern); oriptr = patloc + patlen)
    {
        size_t const skplen = patloc - oriptr;
        // copy the section until the occurence of the pattern
        if (FAILED(StringCchCopyN(retptr, retlen, oriptr, skplen)))
            goto fail;
        retptr += skplen;
        retlen -= skplen;
        // copy the replacement
        if (FAILED(StringCchCopyN(retptr, retlen, replacement, replen)))
            goto fail;
        retptr += replen;
        retlen -= replen;
    }
    // copy the rest of the string.
    if (FAILED(StringCchCopy(retptr, retlen, oriptr)))
        goto fail;

    return returned;
fail:
    if (returned)
        free(returned);
    return NULL;
}

/**
 * @brief Parse command line received via control vchan.
 * @param commandUtf8 Received command.
 * @param userName Requested user name. Must be freed by the caller on success.
 * @param commandLine Actual command line. Must be freed by the caller on success.
 * @param runInteractively Determines whether the command should be run interactively.
 * @return Error code.
 */
static DWORD ParseUtf8Command(IN const char *commandUtf8, OUT WCHAR **userName,
                              OUT WCHAR **commandLine, OUT BOOL *runInteractively)
{
    DWORD status;
    WCHAR *separator = NULL;
    WCHAR *commandUtf16 = NULL;

    if (!commandUtf8 || !runInteractively)
        return ERROR_INVALID_PARAMETER;

    *userName = NULL;
    *commandLine = NULL;
    *runInteractively = TRUE;

    commandUtf16 = (WCHAR*) malloc(CONVERT_MAX_BUFFER_SIZE_UTF16);
    if (!commandUtf16)
    {
        return win_perror2(ERROR_OUTOFMEMORY, "allocating utf16 conversion buffer");
    }

    status = ConvertUTF8ToUTF16(commandUtf8, commandUtf16, NULL);
    if (ERROR_SUCCESS != status)
    {
        free(commandUtf16);
        return win_perror2(status, "ConvertUTF8ToUTF16(command)");
    }

    LogInfo("Command: %s", commandUtf16);

    separator = wcschr(commandUtf16, L':');
    if (!separator)
    {
        free(commandUtf16);
        LogWarning("Command line is supposed to be in 'user:[nogui:]command' form");
        return ERROR_INVALID_PARAMETER;
    }

    *separator = L'\0';
    separator++;
    *userName = _wcsdup(commandUtf16);

    if (!wcsncmp(separator, L"nogui:", 6))
    {
        separator = wcschr(separator, L':');
#pragma warning (suppress:28182) // separator can't be NULL here
        *separator = L'\0';
        separator++;

        *runInteractively = FALSE;
    }

    if (!wcscmp(*userName, L"SYSTEM") || !wcscmp(*userName, L"root"))
    {
        free(*userName);
        *userName = NULL;
    }

    *commandLine = _wcsdup(separator);

    free(commandUtf16);
    LogVerbose("success");

    return ERROR_SUCCESS;
}

/**
 * @brief Recognize magic RPC request command ("QUBESRPC") and replace it with real
 *        command to be executed, after reading RPC service configuration.
 *        If no RPC request is present, do nothing and set output params to NULL.
 * @param commandLine Command line received from vchan, may be modified.
 * @param serviceCommandLine Parsed service handler command if successful. Must be freed by the caller.
 * @param sourceDomainName Source domain (if available) to be set in environment. Must be freed by caller.
 * @return Error code.
 */
static DWORD InterceptRPCRequest(IN OUT WCHAR *commandLine, OUT WCHAR **serviceCommandLine, OUT WCHAR **sourceDomainName)
{
    DWORD status = ERROR_INVALID_PARAMETER;
    HANDLE serviceConfigFile = INVALID_HANDLE_VALUE;
    WCHAR* serviceFilePath = NULL;
    char* serviceConfigContents = NULL;

    LogVerbose("cmd '%s'", commandLine);

    if (!commandLine || !serviceCommandLine || !sourceDomainName)
        goto end;

    *serviceCommandLine = *sourceDomainName = NULL;

    status = ERROR_SUCCESS;
    if (wcsncmp(commandLine, RPC_REQUEST_COMMAND, wcslen(RPC_REQUEST_COMMAND)) != 0)
        goto end;

    serviceFilePath = calloc(sizeof(WCHAR), MAX_PATH_LONG);
    if (!serviceFilePath)
        goto end;

    WCHAR* serviceName = NULL;
    WCHAR* separator = wcschr(commandLine, L' ');
    if (!separator)
    {
        LogError("malformed RPC request");
        status =  ERROR_INVALID_OPERATION;
        goto end;
    }

    separator++;
    serviceName = separator;
    separator = wcschr(serviceName, L' ');
    if (separator)
    {
        *separator = L'\0';
        separator++;
        status = ERROR_OUTOFMEMORY;
        *sourceDomainName = _wcsdup(separator);
        if (*sourceDomainName == NULL)
            goto end;

        LogDebug("source domain: '%s'", *sourceDomainName);
    }
    else
    {
        LogDebug("No source domain given");
        // Most qrexec services do not use source domain at all, so do not
        // abort if missing. This can be the case when RPC was triggered
        // manualy using qvm-run (qvm-run -p vmname "QUBESRPC service_name").
    }

    // build RPC service config file path
    // FIXME: use shell path APIs
    if (!GetModuleFileName(NULL, serviceFilePath, MAX_PATH_LONG))
    {
        status = win_perror("GetModuleFileName");
        goto end;
    }

    status = ERROR_PATH_NOT_FOUND;
    // FIXME hardcoded path
    // cut off file name (qrexec_agent.exe)
    separator = wcsrchr(serviceFilePath, L'\\');
    if (!separator)
    {
        LogError("Cannot find dir containing qrexec-agent.exe");
        goto end;
    }

    *separator = L'\0';
    // cut off one dir (bin)
    separator = wcsrchr(serviceFilePath, L'\\');
    if (!separator)
    {
        LogError("Cannot find dir containing bin\\qrexec-agent.exe");
        goto end;
    }

    // Leave trailing backslash
    separator++;
    *separator = L'\0';
    // FIXME hardcoded path
    if (wcslen(serviceFilePath) + wcslen(L"qubes-rpc\\") + wcslen(serviceName) > MAX_PATH_LONG - 1)
    {
        LogError("RPC service config file path too long");
        goto end;
    }

    // FIXME hardcoded path
    if (FAILED(status = PathCchAppendEx(serviceFilePath, MAX_PATH_LONG, L"qubes-rpc", PATHCCH_ALLOW_LONG_PATHS)))
        goto end;

    if (FAILED(status = PathCchAppendEx(serviceFilePath, MAX_PATH_LONG, serviceName, PATHCCH_ALLOW_LONG_PATHS)))
        goto end;

    LogDebug("service config file: %s", serviceFilePath);

    serviceConfigFile = CreateFile(
        serviceFilePath,       // file to open
        GENERIC_READ,          // open for reading
        FILE_SHARE_READ,       // share for reading
        NULL,                  // default security
        OPEN_EXISTING,         // existing file only
        FILE_ATTRIBUTE_NORMAL, // normal file
        NULL);                 // no attr. template

    WCHAR* serviceCallArg = NULL;
    if (serviceConfigFile == INVALID_HANDLE_VALUE)
    {
        // maybe there is an argument appended? look for a file with
        // +argument stripped
        WCHAR *newsep = NULL;
        newsep = wcschr(serviceName, L'+');
        if (newsep)
        {
            *newsep = L'\0';
            serviceCallArg = newsep+1;
            // strip service+arg
            newsep = wcsrchr(serviceFilePath, L'\\');
            assert(newsep != NULL);

            newsep++;
            *newsep = L'\0';
            if (FAILED(status = PathCchAppendEx(serviceFilePath, MAX_PATH_LONG, serviceName, PATHCCH_ALLOW_LONG_PATHS)))
                goto end;

            LogDebug("service config file (with args): %s", serviceFilePath);

            serviceConfigFile = CreateFile(
                serviceFilePath,       // file to open
                GENERIC_READ,          // open for reading
                FILE_SHARE_READ,       // share for reading
                NULL,                  // default security
                OPEN_EXISTING,         // existing file only
                FILE_ATTRIBUTE_NORMAL, // normal file
                NULL);                 // no attr. template
        }
    }

    if (serviceConfigFile == INVALID_HANDLE_VALUE)
    {
        status = win_perror("opening service config file");
        goto end;
    }

    status = ERROR_OUTOFMEMORY;
    serviceConfigContents = calloc(1, MAX_PATH_LONG);
    if (!serviceConfigContents)
        goto end;

    DWORD cbRead = QioReadUntilEof(serviceConfigFile, serviceConfigContents, MAX_PATH_LONG - 1);
    if (cbRead == 0)
    {
        status = win_perror("reading service config");
        goto end;
    }

    WCHAR* rawServiceFilePath = NULL;
    size_t cfg_size;
    status = ConvertUTF8ToUTF16Static(serviceConfigContents, &rawServiceFilePath, &cfg_size);
    if (status != ERROR_SUCCESS)
    {
        win_perror2(status, "ConvertUTF8ToUTF16Static(serviceConfigContents)");
        goto end;
    }

    // strip white chars (especially end-of-line) from string
    DWORD pathLength = (ULONG)wcslen(rawServiceFilePath);
    while (iswspace(rawServiceFilePath[pathLength - 1]))
    {
        pathLength--;
        rawServiceFilePath[pathLength] = L'\0';
    }

    WCHAR* serviceArgs = PathGetArgs(rawServiceFilePath);
    PathRemoveArgs(rawServiceFilePath);
    PathUnquoteSpaces(rawServiceFilePath);

    if (PathIsRelative(rawServiceFilePath))
    {
        // relative path are based in qubes-rpc-services
        // reuse separator found when preparing previous file path
        *separator = L'\0';
        if (FAILED(status = PathCchAppendEx(serviceFilePath, MAX_PATH_LONG, L"qubes-rpc-services", PATHCCH_ALLOW_LONG_PATHS))) // FIXME hardcoded path
            goto end;
        if (FAILED(status = PathCchAppendEx(serviceFilePath, MAX_PATH_LONG, rawServiceFilePath, PATHCCH_ALLOW_LONG_PATHS)))
            goto end;
    }
    else
    {
        if (FAILED(status = StringCchCopy(serviceFilePath, MAX_PATH_LONG, rawServiceFilePath)))
            goto end;
    }

    PathQuoteSpaces(serviceFilePath);
    if (serviceArgs && serviceArgs[0] != L'\0')
    {
        if (FAILED(status = StringCchCat(serviceFilePath, MAX_PATH_LONG, L" ")))
            goto end;
        if (FAILED(status = StringCchCat(serviceFilePath, MAX_PATH_LONG, serviceArgs)))
            goto end;
    }

    // replace "%1" with an argument, if there was any, otherwise remove it
    if (!serviceCallArg)
        serviceCallArg = L"";

    *serviceCommandLine = StrReplace(serviceFilePath, L"%1", serviceCallArg);
    if (*serviceCommandLine == NULL)
    {
        LogError("Failed to format service call with arguments");
        status = ERROR_INVALID_DATA;
        goto end;
    }
    LogDebug("RPC %s: %s\n", serviceName, *serviceCommandLine);

    status = ERROR_SUCCESS;
    LogVerbose("success");

end:
    if (status != ERROR_SUCCESS)
    {
        win_perror2(status, "");
        if (sourceDomainName)
        {
            free(*sourceDomainName);
            *sourceDomainName = NULL;
        }
    }
    free(serviceFilePath);
    free(serviceConfigContents);
    if (serviceConfigFile != INVALID_HANDLE_VALUE)
        CloseHandle(serviceConfigFile);
    return status;
}

/**
 * @brief Read exec_params from daemon after one of the EXEC messages has been received.
 * @param bufferSize Size of the exec params (variable, includes command line).
 * @return Exec params on success or NULL. Must be freed by the caller.
 */
struct exec_params *ReceiveExecParams(IN int bufferSize)
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

    LogDebug("domain %d, port %d, cmd: '%S'", params->connect_domain, params->connect_port, params->cmdline);
    return params;
}

/**
* @brief Send MSG_HELLO to the vchan peer.
* @param vchan Vchan.
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

#define MAX_PATH_LONG 32768

/**
 * @brief Start qrexec-wrapper process that will handle data vchan and child process I/O.
 * @param domain Data vchan domain.
 * @param port Data vchan port.
 * @param userName User name for the local executable.
 * @param commandLine Local executable to connect to data vchan.
 * @param isServer Determines whether qrexec-wrapper should act as a vchan server.
 * @param piped Determines whether the local executable's I/O should be connected to the data vchan.
 * @param interactive Determines whether the local executable should be run in the interactive session.
 * @return Error code.
 */
static DWORD StartChild(int domain, int port, PWSTR userName, PWSTR commandLine, BOOL isServer, BOOL piped, BOOL interactive)
{
    PWSTR command = malloc(MAX_PATH_LONG * sizeof(WCHAR));
    int flags = 0;
    HANDLE wrapper;
    DWORD status;
    /*
    * @param argv Expected arguments are: <domain> <port> <user_name> <flags> <command_line>
    *             domain:       remote domain for data vchan
    *             port:         remote port for data vchan
    *             user_name:    user name to use for the child process or (null) for current user
    *             flags:        bitmask of following possible values (decimal):
    *                      0x01 act as vchan server (default is client)
    *                      0x02 pipe child process' io to vchan (default is not)
    *                      0x04 run the child process in the interactive session (requires that a user is logged on)
    *             command_line: local program to execute
    */
    if (!command)
        return ERROR_OUTOFMEMORY;

    if (isServer)    flags |= 0x01;
    if (piped)       flags |= 0x02;
    if (interactive) flags |= 0x04;

    StringCchPrintf(command, MAX_PATH_LONG, L"qrexec-wrapper.exe %d%c%d%c%s%c%d%c%s",
                    domain, QUBES_ARGUMENT_SEPARATOR,
                    port, QUBES_ARGUMENT_SEPARATOR,
                    userName, QUBES_ARGUMENT_SEPARATOR,
                    flags, QUBES_ARGUMENT_SEPARATOR,
                    commandLine);

    LogDebug("domain %d, port %d, user '%s', isServer %d, piped %d, interactive %d, cmd '%s', final command '%s'",
        domain, port, userName, isServer, piped, interactive, commandLine, command);
    // wrapper will run as current user (SYSTEM, we're a service)
    status = CreateNormalProcessAsCurrentUser(command, &wrapper);
    if (status == ERROR_SUCCESS)
    {
        register_vchan_connection(wrapper, domain, port);
    }
    free(command);
    return status;
}

/**
 * @brief Find pending qrexec service request by request id.
 * @param requestId Service request id that was sent to qrexec daemon.
 * @return Service request data on success.
 */
static PSERVICE_REQUEST FindServiceRequest(
    IN  PCHAR requestId
    )
{
    PLIST_ENTRY entry;
    PSERVICE_REQUEST returnContext = NULL;

    LogVerbose("%S", requestId);
    EnterCriticalSection(&g_RequestCriticalSection);
    entry = g_RequestList.Flink;
    while (entry != &g_RequestList)
    {
        PSERVICE_REQUEST context = (PSERVICE_REQUEST)CONTAINING_RECORD(entry, SERVICE_REQUEST, ListEntry);
        if (0 == strcmp(context->ServiceParams.request_id.ident, requestId))
        {
            returnContext = context;
            break;
        }

        entry = entry->Flink;
    }
    LeaveCriticalSection(&g_RequestCriticalSection);

    if (returnContext)
    {
        LogDebug("found request: domain '%S', service '%S', user '%s', command '%s'",
                 returnContext->ServiceParams.target_domain, returnContext->ServiceParams.service_name,
            returnContext->UserName, returnContext->CommandLine);
    }
    else
        LogDebug("request for '%S' not found", requestId);

    return returnContext;
}

/**
 * @brief Handle qrexec service connect (allowed).
 * @param header Qrexec header with data connection parameters.
 * @return Error code.
 */
DWORD HandleServiceConnect(IN const struct msg_header *header)
{
    DWORD status;
    struct exec_params *params = NULL;
    PSERVICE_REQUEST context = NULL;

    LogVerbose("msg 0x%x, len %d", header->type, header->len);

    params = ReceiveExecParams(header->len);
    if (!params)
    {
        LogError("ReceiveExecParams failed");
        return ERROR_INVALID_FUNCTION;
    }

    // service request id is passed in the cmdline field
    LogDebug("domain %u, port %u, request id '%S'", params->connect_domain, params->connect_port, params->cmdline);

    context = FindServiceRequest(params->cmdline);
    if (!context)
    {
        LogError("request '%S' not pending", params->cmdline);
        status = ERROR_INVALID_PARAMETER;
        goto cleanup;
    }

    status = StartChild(params->connect_domain, params->connect_port, context->UserName, context->CommandLine, TRUE, TRUE, TRUE);
    if (ERROR_SUCCESS != status)
        win_perror("StartChild");

    EnterCriticalSection(&g_RequestCriticalSection);
    RemoveEntryList(&context->ListEntry);
    LeaveCriticalSection(&g_RequestCriticalSection);

cleanup:
    if (context)
    {
        free(context->UserName);
        free(context->CommandLine);
    }
    free(context);
    free(params);
    return status;
}

/**
* @brief Handle qrexec service connect (refused).
* @param header Qrexec header with refused request id.
* @return Error code.
*/
DWORD HandleServiceRefused(IN const struct msg_header *header)
{
    struct service_params serviceParams;
    PSERVICE_REQUEST context;

    LogDebug("msg 0x%x, len %d", header->type, header->len);

    if (!VchanReceiveBuffer(g_DaemonVchan, &serviceParams, header->len, L"service_params"))
        return ERROR_INVALID_FUNCTION;

    LogDebug("request id '%S'", serviceParams.ident);

    context = FindServiceRequest(serviceParams.ident);
    if (!context)
    {
        LogError("request '%S' not pending", serviceParams.ident);
        return ERROR_INVALID_PARAMETER;
    }

    LogInfo("Qrexec service refused by daemon: domain '%S', service '%S', user '%s, local command '%s'",
            context->ServiceParams.target_domain, context->ServiceParams.service_name, context->UserName, context->CommandLine);

    // TODO: notify user?

    EnterCriticalSection(&g_RequestCriticalSection);
    RemoveEntryList(&context->ListEntry);
    LeaveCriticalSection(&g_RequestCriticalSection);

    free(context->UserName);
    free(context->CommandLine);
    free(context);

    return ERROR_SUCCESS;
}

/**
 * @brief Handle common prologue for exec messages. Header is already processed.
 * @param bufferSize Size of the exec_params buffer.
 * @param userName Requested user name. Must be freed by the caller.
 * @param commandLine Actual command line to execute locally. Set to NULL if command line parsing fails. Must be freed by the caller.
 * @param runInteractively Determines whether the local command should be run in the interactive session.
 * @return Exec params on success (even if parsing command line fails). Must be freed by the caller.
 */
struct exec_params *HandleExecCommon(IN int bufferSize, OUT WCHAR **userName, OUT WCHAR **commandLine, OUT BOOL *runInteractively)
{
    struct exec_params *exec = NULL;
    DWORD status;
    WCHAR *remoteDomainName = NULL;
    WCHAR *serviceCommandLine = NULL;

    exec = ReceiveExecParams(bufferSize);
    if (!exec)
    {
        LogError("ReceiveExecParams failed");
        return NULL;
    }

    *runInteractively = TRUE;

    status = ParseUtf8Command(exec->cmdline, userName, commandLine, runInteractively);
    if (ERROR_SUCCESS != status)
    {
        LogError("ParseUtf8Command failed");
        return exec;
    }

    LogDebug("user: '%s', interactive: %d, parsed: '%s'", *userName, *runInteractively, *commandLine);

    // serviceCommandLine and remoteDomainName are allocated in the call
    status = InterceptRPCRequest(*commandLine, &serviceCommandLine, &remoteDomainName);
    if (ERROR_SUCCESS != status)
    {
        LogWarning("InterceptRPCRequest failed");
        free(*commandLine);
        *commandLine = NULL;
        return exec;
    }

    if (remoteDomainName)
    {
        // FIXME: this should be done by qrexec-wrapper as the target executable might be in another session etc.
        LogDebug("RPC domain: '%s'", remoteDomainName);
        SetEnvironmentVariableW(L"QREXEC_REMOTE_DOMAIN", remoteDomainName);
        free(remoteDomainName);
    }

    if (serviceCommandLine)
    {
        LogDebug("service command: '%s'", serviceCommandLine);
        free(*commandLine);
        *commandLine = serviceCommandLine;
    }
    else
    {
        LogDebug("no service");
    }

    LogDebug("success: cmd '%s', user '%s'", *commandLine, *userName);

    return exec;
}

/**
 * @brief Handle EXEC command from control vchan.
 * @param header Qrexec header.
 * @param piped Determines whether the local executable's I/O should be connected to data vchan.
 * @return Error code.
 */
static DWORD HandleExec(IN const struct msg_header *header, BOOL piped)
{
    DWORD status;
    WCHAR *userName = NULL;
    WCHAR *commandLine = NULL;
    BOOL interactive;
    struct exec_params *exec;

    LogVerbose("msg 0x%x, len %d", header->type, header->len);

    exec = HandleExecCommon(header->len, &userName, &commandLine, &interactive);
    if (!exec)
        return ERROR_INVALID_FUNCTION;

    if (commandLine)
    {
        // Start the wrapper that will take care of data vchan, launch the child and redirect child's IO to data vchan if piped==TRUE.
        status = StartChild(exec->connect_domain, exec->connect_port, userName, commandLine, FALSE, piped, interactive);
        if (ERROR_SUCCESS != status)
            LogError("StartChild(%s) failed", commandLine);
    }
    else
    {
        LogDebug("Parsing the command line failed");
        // parsing failed, most likely unknown service - start the wrapper with dummy command line to send non-zero exit code through data vchan
        StartChild(exec->connect_domain, exec->connect_port, userName, L"dummy", FALSE, piped, interactive);
        status = ERROR_SUCCESS;
    }

    free(commandLine);
    free(userName);
    free(exec);
    return status;
}

/**
 * @brief Handle HELLO from control vchan.
 * @param header Qrexec header.
 * @return Error code.
 */
static DWORD HandleDaemonHello(struct msg_header *header)
{
    struct peer_info info;

    if (header->len != sizeof(info))
    {
        LogError("header->len != sizeof(peer_info), protocol incompatible");
        return ERROR_INVALID_FUNCTION;
    }

    // read protocol version
    if (!VchanReceiveBuffer(g_DaemonVchan, &info, sizeof(info), L"peer info"))
        return ERROR_INVALID_FUNCTION;

    if (info.version < QREXEC_PROTOCOL_VERSION)
    {
        LogWarning("incompatible protocol version (%d instead of %d)",
                   info.version, QREXEC_PROTOCOL_VERSION);
        return ERROR_INVALID_FUNCTION;
    }

    LogDebug("received protocol version %d", info.version);

    return ERROR_SUCCESS;
}

/**
 * @brief Handle control message from vchan.
 * @return Error code.
 */
static DWORD HandleDaemonMessage(void)
{
    struct msg_header header;

    LogVerbose("start");

    if (!VchanReceiveBuffer(g_DaemonVchan, &header, sizeof header, L"daemon header"))
        return win_perror2(ERROR_INVALID_FUNCTION, "VchanReceiveBuffer");

    switch (header.type)
    {
    case MSG_HELLO:
        return HandleDaemonHello(&header);

    case MSG_EXEC_CMDLINE:
        return HandleExec(&header, TRUE);

    case MSG_JUST_EXEC:
        return HandleExec(&header, FALSE);

    case MSG_SERVICE_CONNECT:
        return HandleServiceConnect(&header);

    case MSG_SERVICE_REFUSED:
        return HandleServiceRefused(&header);

    default:
        LogWarning("unknown message type: 0x%x", header.type);
        return ERROR_INVALID_PARAMETER;
    }
}

static DWORD WINAPI ServiceCleanup(void);

/**
 * @brief Vchan event loop.
 * @param stopEvent When this event is signaled, the function should exit.
 * @return Error code.
 */
static DWORD WatchForEvents(HANDLE stopEvent)
{
    DWORD signaledEvent;
    DWORD status = ERROR_INVALID_FUNCTION;
    BOOL run = TRUE;
    BOOL daemonConnected = FALSE;
    HANDLE waitObjects[2 + MAX_FDS];
    HANDLE advertiseToolsProcess;
    WCHAR advertiseCommand[] = L"advertise-tools.exe 1"; // must be non-const

    LogVerbose("start");

    // Don't do anything before qdb is available, otherwise advertise-tools may fail.
    if (!WaitForQdb())
    {
        return win_perror("WaitForQdb");
    }

    // We give a 5 minute timeout here because xeniface can take some time
    // to load the first time after reboot after pvdrivers installation.
    g_DaemonVchan = VchanInitServer(0, VCHAN_BASE_PORT, VCHAN_BUFFER_SIZE, 5 * 60 * 1000);

    if (!g_DaemonVchan)
        return win_perror2(ERROR_INVALID_FUNCTION, "VchanInitServer");

    LogDebug("port %d: daemon vchan = %p", VCHAN_BASE_PORT, g_DaemonVchan);
    LogDebug("Waiting for qrexec daemon connection, write buffer size: %d", VchanGetWriteBufferSize(g_DaemonVchan));

    waitObjects[0] = stopEvent;
    waitObjects[1] = libvchan_fd_for_select(g_DaemonVchan);

    while (run)
    {
        LogVerbose("loop start");

        status = ERROR_SUCCESS;

        DWORD waitObjectsIndex = 2;
        // add registered processes's handles to watch
        for (int i = 0; i < MAX_FDS; i++)
        {
            if (connection_info[i].handle != NULL)
            {
                waitObjects[waitObjectsIndex++] = connection_info[i].handle;
            }
        }
        LogVerbose("waiting for event");

        signaledEvent = WaitForMultipleObjects(waitObjectsIndex, waitObjects, FALSE, INFINITE) - WAIT_OBJECT_0;

        LogVerbose("event %d", signaledEvent);

        if (WAIT_FAILED == signaledEvent)
        {
            status = win_perror("WaitForMultipleObjects");
            break;
        }

        if (0 == signaledEvent) // stop event
        {
            LogDebug("stopping");
            status = ERROR_SUCCESS;
            break;
        }

        if (1 == signaledEvent) // control vchan event
        {
            EnterCriticalSection(&g_DaemonCriticalSection);
            if (!daemonConnected)
            {
                LogDebug("qrexec-daemon has connected");

                if (!VchanSendHello(g_DaemonVchan))
                {
                    LogError("failed to send hello to daemon");
                    goto out;
                }

                daemonConnected = TRUE;
                LeaveCriticalSection(&g_DaemonCriticalSection);

                // advertise tools presence to dom0 by writing appropriate entries to qubesdb
                // it waits for user logon
                status = CreateNormalProcessAsCurrentUser(advertiseCommand, &advertiseToolsProcess);
                if (status == ERROR_SUCCESS)
                {
                    CloseHandle(advertiseToolsProcess);
                }
                else
                {
                    win_perror("Failed to create advertise-tools process");
                    // this is non-fatal?
                }
                continue;
            }

            if (!libvchan_is_open(g_DaemonVchan)) // vchan broken
            {
                LogWarning("daemon disconnected");
                LeaveCriticalSection(&g_DaemonCriticalSection);
                break;
            }

            // handle data from daemon
            while (VchanGetReadBufferSize(g_DaemonVchan) > 0)
            {
                status = HandleDaemonMessage();
                if (ERROR_SUCCESS != status)
                {
                    run = FALSE;
                    win_perror2(status, "HandleDaemonMessage");
                    goto out;
                }
            }
out:
            LeaveCriticalSection(&g_DaemonCriticalSection);
            continue;
        }

        if (signaledEvent < waitObjectsIndex) // wrapped processes
        {
            for (int i = 0; i < MAX_FDS; i++)
            {
                if (connection_info[i].handle == waitObjects[signaledEvent])
                {
                    release_connection(i);
                    break;
                }
            }
        }
    }

    LogVerbose("loop finished");

    ServiceCleanup();

    return status;
}

/**
 * @brief Print usage.
 */
void Usage(void)
{
    wprintf(L"qrexec agent service\n\nUsage: qrexec-agent <-i|-u>\n");
}

/**
 * @brief Log callback for xencontrol/libxenvchan.
 * @param level Message level.
 * @param function Function name.
 * @param format Message format.
 * @param args Message arguments.
 */
static void XifLogger(int level, const char *function, const WCHAR *format, va_list args)
{
    WCHAR buf[1024];

    StringCbVPrintfW(buf, sizeof(buf), format, args);
    _LogFormat(level, FALSE, function, buf);
}

/**
 * @brief Thread servicing a single qrexec-client-vm
 * @param param Pipe server client id.
 */
DWORD WINAPI PipeClientThread(PVOID param)
{
    LONGLONG clientId = (LONGLONG)param;
    DWORD status = ERROR_OUTOFMEMORY;
    PSERVICE_REQUEST context;
    size_t stringSize;

    context = malloc(sizeof(SERVICE_REQUEST));
    if (!context)
        goto cleanup;

    context->CommandLine = NULL;
    context->UserName = NULL;

    status = QpsRead(g_PipeServer, clientId, &context->ServiceParams, sizeof(context->ServiceParams));
    if (ERROR_SUCCESS != status)
    {
        win_perror2(status, "QpsRead(params)");
        goto cleanup;
    }

    // user size, including null terminator
    status = QpsRead(g_PipeServer, clientId, &stringSize, sizeof(stringSize));
    if (ERROR_SUCCESS != status)
    {
        win_perror2(status, "QpsRead(user size)");
        goto cleanup;
    }

    context->UserName = malloc(stringSize);
    if (!context->UserName)
    {
        goto cleanup;
    }

    status = QpsRead(g_PipeServer, clientId, context->UserName, (DWORD)stringSize);
    if (ERROR_SUCCESS != status)
    {
        win_perror2(status, "QpsRead(user)");
        goto cleanup;
    }

    // command line size, including null terminator
    status = QpsRead(g_PipeServer, clientId, &stringSize, sizeof(stringSize));
    if (ERROR_SUCCESS != status)
    {
        win_perror2(status, "QpsRead(cmd size)");
        goto cleanup;
    }

    context->CommandLine = malloc(stringSize);
    if (!context->CommandLine)
    {
        goto cleanup;
    }

    status = QpsRead(g_PipeServer, clientId, context->CommandLine, (DWORD)stringSize);
    if (ERROR_SUCCESS != status)
    {
        win_perror2(status, "QpsRead(cmd)");
        goto cleanup;
    }

    LogInfo("Received request from client %lu: domain '%S', service '%S', user '%s', local command '%s', request id %lu",
            clientId, context->ServiceParams.target_domain, context->ServiceParams.service_name,
            context->UserName, context->CommandLine, g_RequestId);

    QpsDisconnectClient(g_PipeServer, clientId);

    StringCbPrintfA(context->ServiceParams.request_id.ident, sizeof(context->ServiceParams.request_id.ident), "%lu", g_RequestId++);
    if (!VchanSendMessage(g_DaemonVchan, MSG_TRIGGER_SERVICE, &context->ServiceParams, sizeof(context->ServiceParams), L"trigger_service_params"))
    {
        LogError("sending trigger params to daemon failed");
        status = ERROR_INVALID_FUNCTION;
        goto cleanup;
    }

    // add to pending requests
    EnterCriticalSection(&g_RequestCriticalSection);
    InsertTailList(&g_RequestList, &context->ListEntry);
    LeaveCriticalSection(&g_RequestCriticalSection);

    status = ERROR_SUCCESS;
    // context and user/command line will be freed in HandleService*

cleanup:
    if (status != ERROR_SUCCESS)
    {
        if (context)
        {
            free(context->UserName);
            free(context->CommandLine);
        }
        free(context);
    }
    return status;
}

/**
 * @brief Pipe server callback: client connected.
 * @param server Pipe server.
 * @param id Client id.
 * @param context User context (unused).
 */
void ClientConnectedCallback(PIPE_SERVER server, LONGLONG id, PVOID context)
{
    HANDLE clientThread;

    clientThread = CreateThread(NULL, 0, PipeClientThread, (PVOID)id, 0, NULL);
    if (!clientThread)
    {
        win_perror("create client thread");
        return;
    }
    CloseHandle(clientThread);
    // the client thread will take care of processing client's data
}

/**
 * @brief Main pipe server processing loop.
 * @param param Unused.
 */
DWORD WINAPI PipeServerThread(PVOID param)
{
    // only returns on error
    LogVerbose("start");
    return QpsMainLoop(g_PipeServer);
}

/**
 * @brief Service worker thread.
 * @param param Worker context.
 * @return Error code.
 */
DWORD WINAPI ServiceExecutionThread(void *param)
{
    DWORD status;
    HANDLE pipeServerThread;
    PSERVICE_WORKER_CONTEXT ctx = param; // supplied by the common service code.
    PSECURITY_DESCRIPTOR sd;
    PACL acl;
    SECURITY_ATTRIBUTES sa = { 0 };

    LogInfo("Service started");

    libvchan_register_logger(XifLogger, LogGetLevel());

    status = CreatePublicPipeSecurityDescriptor(&sd, &acl);
    if (status != ERROR_SUCCESS)
        return win_perror("create pipe security descriptor");

    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    sa.lpSecurityDescriptor = sd;
    // initialize pipe server for local clients
    status = QpsCreate(TRIGGER_PIPE_NAME,
                       4096, // pipe buffers
                       4096, // read buffer
                       1000, // write timeout
                       ClientConnectedCallback,
                       NULL,
                       NULL,
                       NULL,
                       &sa,
                       &g_PipeServer);

    if (ERROR_SUCCESS != status)
        win_perror2(status, "create pipe server");

    LogVerbose("pipe server: %p", g_PipeServer);

    pipeServerThread = CreateThread(NULL, 0, PipeServerThread, NULL, 0, NULL);
    if (!pipeServerThread)
    {
        status = GetLastError();
        return win_perror2(status, "create pipe server thread");
    }

    LogVerbose("pipe thread: %p, entering loop", pipeServerThread);

    status = WatchForEvents(ctx->StopEvent);
    if (ERROR_SUCCESS != status)
        LogDebug("WatchForEvents failed");

    ServiceCleanup();
// FIXME: pipe server doesn't have the ability for graceful stop

    LogDebug("Waiting for the pipe server thread to exit");
    if (WaitForSingleObject(pipeServerThread, 1000) != WAIT_OBJECT_0)
        TerminateThread(pipeServerThread, 0);
    CloseHandle(pipeServerThread);

    LocalFree(acl);
    LocalFree(sd);

    LogInfo("Shutting down");

    return ERROR_SUCCESS;
}

static DWORD WINAPI ServiceCleanup(void)
{
    if (g_DaemonVchan)
    {
        libvchan_close(g_DaemonVchan);
        g_DaemonVchan = NULL;
    }
    return ERROR_SUCCESS;
}

int __cdecl wmain(int argc, WCHAR *argv[])
{
    DWORD status;

#ifdef _DEBUG
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_CHECK_ALWAYS_DF | _CRTDBG_CHECK_CRT_DF);
#endif
    LogVerbose("start");

    InitializeCriticalSection(&g_DaemonCriticalSection);
    InitializeCriticalSection(&g_RequestCriticalSection);
    InitializeSRWLock(&g_ConnectionsHandlesLock);
    InitializeListHead(&g_RequestList);

    status = SvcMainLoop(
        SERVICE_NAME,
        0,
        ServiceExecutionThread,
        NULL,
        NULL,
        NULL);

    LogVerbose("exiting");
    return status;
}
