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

// This program is used to trigger qrexec services in remote domains.
// It connects to local qrexec agent and sends it:
// trigger_service_params (service params), size_t (local handler path size), local handler path (WCHARs).
// Local handler will be launched as the local endpoint for the triggered service.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <ws2ipdef.h>
#include <ws2def.h>
#include <windows.h>
#include <strsafe.h>
#include <process.h>

#include <qrexec.h>
#include <log.h>
#include <qubes-io.h>
#include <utf8-conv.h>
#include <pipe-server.h>
#include <exec.h>

#define MAX_SIZE_FORWARDER_COMMANDLINE 256
#define MAX_SIZE_PIPE_NAME 256
#define MAX_SIZE_BUFFER 65536

typedef struct _CONNECTION_DATA
{
    ULONG id;
    SOCKET clientSocket;
    HANDLE writePipe;
    HANDLE readPipe;
} CONNECTION_DATA, *PCONNECTION_DATA;

PWSTR g_DomainName, g_ServiceName, g_CommandLine;
struct trigger_service_params g_TriggerParams = { 0 };

ULONG CreatePipes(ULONG id, WCHAR* pipeServerName, WCHAR* pipeClientName, HANDLE* writePipe, HANDLE* readPipe)
{
    ULONG status = ERROR_SUCCESS;
    StringCchPrintf(pipeServerName, MAX_SIZE_PIPE_NAME, L"\\\\.\\pipe\\stdio_forward_%d-%d", _getpid(), id);
    StringCchPrintf(pipeClientName, MAX_SIZE_PIPE_NAME, L"\\\\.\\pipe\\stdio_forward_%d-%d_client", _getpid(), id);

    LogDebug("Creating pipes to forward stdio");

    PSECURITY_DESCRIPTOR sd;
    PACL acl;
    SECURITY_ATTRIBUTES sa = { 0 };

    status = CreatePublicPipeSecurityDescriptor(&sd, &acl);
    if (status != ERROR_SUCCESS)
        return win_perror2(status, "create pipe security descriptor");

    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    sa.lpSecurityDescriptor = sd;

    *writePipe = CreateNamedPipe(pipeServerName,
                                 PIPE_ACCESS_OUTBOUND,
                                 PIPE_TYPE_BYTE | PIPE_WAIT,
                                 PIPE_UNLIMITED_INSTANCES,
                                 MAX_SIZE_BUFFER,
                                 MAX_SIZE_BUFFER,
                                 0,
                                 &sa);

    *readPipe = CreateNamedPipe(pipeClientName,
                                PIPE_ACCESS_INBOUND,
                                PIPE_TYPE_BYTE | PIPE_WAIT,
                                PIPE_UNLIMITED_INSTANCES,
                                MAX_SIZE_BUFFER,
                                MAX_SIZE_BUFFER,
                                0,
                                &sa);

    return ERROR_SUCCESS;
}

ULONG ConnectPipes(WCHAR* pipeServerName, WCHAR* pipeClientName, HANDLE writePipe, HANDLE readPipe)
{
    DWORD cbPipeName;
    DWORD written;

    // wait for connection
    while (!ConnectNamedPipe(writePipe, NULL) && GetLastError() != ERROR_PIPE_CONNECTED)
    {
        LogVerbose("waiting for outbound connection, pipe %s", pipeServerName);
        Sleep(10);
    }

    LogVerbose("outbound pipe connected");

    // send the name to the client
    cbPipeName = (DWORD) (wcslen(pipeClientName) + 1) * sizeof(WCHAR);
    if (!WriteFile(writePipe, &cbPipeName, sizeof(cbPipeName), &written, NULL))
        return win_perror("writing size of inbound pipe name");

    if (!WriteFile(writePipe, pipeClientName, cbPipeName, &written, NULL))
        return win_perror("writing name of inbound pipe");

    while (!ConnectNamedPipe(readPipe, NULL) && GetLastError() != ERROR_PIPE_CONNECTED)
    {
        LogVerbose("waiting for inbound connection, pipe %s", pipeClientName);
        Sleep(10);
    }

    LogVerbose("inbound pipe connected");

    return ERROR_SUCCESS;
}

DWORD WINAPI ReadPipeWriteStdoutThread(HANDLE readPipe)
{
    HANDLE stdoutHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    BYTE buffer[MAX_SIZE_BUFFER];
    DWORD bytesRead;

    while (ReadFile(readPipe, &buffer, MAX_SIZE_BUFFER, &bytesRead, NULL))
        if (!bytesRead || !QioWriteBuffer(stdoutHandle, &buffer, bytesRead))
            break;

    // received an eof on the reading pipe, closing the pipe
    CloseHandle(readPipe);
    // the remote has closed the connection, close stdout to propogate the eof
    CloseHandle(stdoutHandle);

    return ERROR_SUCCESS;
}

void ReadStdinWritePipe(HANDLE writePipe)
{
    HANDLE stdinHandle = GetStdHandle(STD_INPUT_HANDLE);
    BYTE buffer[MAX_SIZE_BUFFER];
    DWORD bytesRead;

    while (ReadFile(stdinHandle, &buffer, MAX_SIZE_BUFFER, &bytesRead, NULL))
        if (!bytesRead || !QioWriteBuffer(writePipe, &buffer, bytesRead))
            break;

    // received an eof on stdin, close the handle
    CloseHandle(stdinHandle);
    // to propogate the eof from stdin via the write pipe, explicitly close the pipe
    CloseHandle(writePipe);
}

ULONG ProcessArguments(WCHAR *argv[], BOOL *stdioMode, ULONG *tcpListenPort)
{
    PWSTR qubesListener = L"qubes.tcp-listen+";
    UCHAR *argumentUtf8;
    HRESULT hresult;
    ULONG status = ERROR_BAD_ARGUMENTS;

    g_DomainName = GetArgument();
    g_ServiceName = GetArgument();
    g_CommandLine = GetArgument();

    if (!g_DomainName || !g_ServiceName)
    {
        LogError("Usage: %s domain name|qrexec service name[|local program [local program arguments]]\n", argv[0]);
        return ERROR_INVALID_PARAMETER;
    }

    LogDebug("domain '%s', service '%s'", g_DomainName, g_ServiceName);
    if (!g_CommandLine || wcsncmp(g_CommandLine, qubesListener, wcslen(qubesListener)) == 0)
    {
        if (g_CommandLine)
        {
            LogDebug("tcp listen local command '%s'", g_CommandLine);

            long port = wcstol(g_CommandLine + wcslen(qubesListener), NULL, 10);
            if (port < 1 || port > 65535)
            {
                LogError("Invalid port (%d) number provided for TCP listen mode.", port);
                return win_perror2(ERROR_BAD_ARGUMENTS, "wcstol");
            }

            *tcpListenPort = port;
        }
        else
        {
            LogDebug("no local command, defaulting to piping stdin/stdout");
            *stdioMode = TRUE;
        }
    }
    else
        LogDebug("local command '%s'", g_CommandLine);

    // Prepare the parameter structure containing the first two arguments.
    argumentUtf8 = NULL;
    status = ConvertUTF16ToUTF8(g_ServiceName, &argumentUtf8, NULL);
    if (ERROR_SUCCESS != status)
        return win_perror2(status, "ConvertUTF16ToUTF8");

    hresult = StringCchCopyA(g_TriggerParams.service_name, sizeof(g_TriggerParams.service_name), argumentUtf8);
    if (FAILED(hresult))
        return win_perror2(hresult, "StringCchCopyA");

    ConvertFree(argumentUtf8);
    argumentUtf8 = NULL;

    status = ConvertUTF16ToUTF8(g_DomainName, &argumentUtf8, NULL);
    if (ERROR_SUCCESS != status)
        return win_perror2(status, "ConvertUTF16ToUTF8");

    hresult = StringCchCopyA(g_TriggerParams.target_domain, sizeof(g_TriggerParams.target_domain), argumentUtf8);
    if (FAILED(hresult))
        return win_perror2(hresult, "StringCchCopyA");

    ConvertFree(argumentUtf8);
    argumentUtf8 = NULL;

    return ERROR_SUCCESS;
}

ULONG RequestQrexec(PWSTR commandLine)
{
    HANDLE readPipe, writePipe;
    size_t size;
    PWSTR pipeName = L"\\\\.\\pipe\\qrexec_trigger";

    LogDebug("Connecting to qrexec-agent");

    if (ERROR_SUCCESS != QpsConnect(pipeName, &readPipe, &writePipe))
        return (int)GetLastError(); // win_perror("open agent pipe");

    CloseHandle(readPipe);
    LogDebug("Sending the parameters to qrexec-agent");

    if (!QioWriteBuffer(writePipe, &g_TriggerParams, sizeof(g_TriggerParams)))
    {
        CloseHandle(writePipe);
        return win_perror("write trigger params to agent");
    }

    size = (wcslen(commandLine) + 1) * sizeof(WCHAR);
    if (!QioWriteBuffer(writePipe, &size, sizeof(size)))
    {
        CloseHandle(writePipe);
        return win_perror("write command size to agent");
    }

    if (!QioWriteBuffer(writePipe, commandLine, (DWORD)size))
    {
        CloseHandle(writePipe);
        return win_perror("write command to agent");
    }

    CloseHandle(writePipe);

    return ERROR_SUCCESS;
}

ULONG SetupPipes(ULONG id, HANDLE* writePipe, HANDLE* readPipe)
{
    ULONG status;
    WCHAR pipeServerName[MAX_SIZE_PIPE_NAME];
    WCHAR pipeClientName[MAX_SIZE_PIPE_NAME];
    WCHAR commandLine[MAX_SIZE_FORWARDER_COMMANDLINE];

    // create pipes to forward tcp stream to qrexec-wrapper
    status = CreatePipes(id, pipeServerName, pipeClientName, writePipe, readPipe);
    if (ERROR_SUCCESS != status)
        return win_perror2(status, "creating pipe server");

    StringCchPrintf(commandLine, MAX_SIZE_FORWARDER_COMMANDLINE, L"qrexec-stdio-forwarder.exe %d-%d", _getpid(), id);
    status = RequestQrexec(commandLine);
    if (ERROR_SUCCESS != status)
    {
        CloseHandle(*writePipe);
        CloseHandle(*readPipe);
        return win_perror2(status, "requesting command execution");
    }

    status = ConnectPipes(pipeServerName, pipeClientName, *writePipe, *readPipe);
    if (ERROR_SUCCESS != status)
    {
        CloseHandle(*writePipe);
        CloseHandle(*readPipe);
        return win_perror2(status, "connecting pipes");
    }

    return ERROR_SUCCESS;
}

ULONG RunStdioMode()
{
    ULONG status;
    HANDLE writePipe, readPipe;

    status = SetupPipes(0, &writePipe, &readPipe);
    if (status != ERROR_SUCCESS)
        return win_perror2(status, "setting up pipes");

    // the reading pipe/thread will be closed when the remote closes it's side of the connection
    HANDLE readPipeThread = CreateThread(NULL, 0, ReadPipeWriteStdoutThread, readPipe, 0, NULL);
    if (!readPipeThread)
    {
        CloseHandle(writePipe);
        CloseHandle(readPipe);
        return win_perror("creating read pipe thread");
    }

    // this is blocking till an eof is received on stdin
    ReadStdinWritePipe(writePipe);

    // block till eof is received on the read pipe (remote closed connection)
    WaitForSingleObject(readPipeThread, INFINITE);
    CloseHandle(readPipeThread);

    return ERROR_SUCCESS;
}

DWORD WINAPI ReadPipeWriteSocketThread(PVOID param)
{
    PCONNECTION_DATA connectionData = param;

    BYTE buffer[MAX_SIZE_BUFFER];
    DWORD bytesRead;
    int bytesSend;

    while (ReadFile(connectionData->readPipe, &buffer, MAX_SIZE_BUFFER, &bytesRead, NULL))
    {
        if (!bytesRead)
            break;

        bytesSend = send(connectionData->clientSocket, buffer, bytesRead, 0);
        if (bytesSend == SOCKET_ERROR)
        {
            LogDebug("socket send resulted in error: %d", WSAGetLastError());
            break;
        }

        if (bytesSend != bytesRead)
        {
            LogError("socket send less than expected: %d - %d", bytesRead, bytesSend);
            break;
        }
    }

    // received an eof on the reading pipe (or encountered an error), closing the pipe
    CloseHandle(connectionData->readPipe);
    // the remote has closed the connection, disable socket sending to propogate FIN
    if (shutdown(connectionData->clientSocket, SD_SEND) != ERROR_SUCCESS)
        return win_perror2(WSAGetLastError(), "close socket send");

    return ERROR_SUCCESS;
}

ULONG ReadSocketWritePipe(PCONNECTION_DATA connectionData)
{
    ULONG status;
    BYTE buffer[MAX_SIZE_BUFFER];
    int bytesRead;

    while (TRUE)
    {
        bytesRead = recv(connectionData->clientSocket, buffer, MAX_SIZE_BUFFER, 0);
        if (bytesRead == SOCKET_ERROR)
        {
            LogDebug("socket recv resulted in error: %d", WSAGetLastError());
            break;
        }

        if (!bytesRead || !QioWriteBuffer(connectionData->writePipe, &buffer, bytesRead))
            break;
    }

    // received an FIN on socket (or encountered an error when reading from the socket), close socket receiving
    status = shutdown(connectionData->clientSocket, SD_RECEIVE);
    // to propogate the eof from stdin via the write pipe, explicitly close the pipe
    CloseHandle(connectionData->writePipe);

    if (status != ERROR_SUCCESS)
        return win_perror2(WSAGetLastError(), "close socket receive");

    return ERROR_SUCCESS;
}

// this method is responsible for freeing the connectionData memory
DWORD WINAPI HandleConnection(PVOID param)
{
    ULONG status;
    PCONNECTION_DATA connectionData = param;

    status = SetupPipes(connectionData->id, &connectionData->writePipe, &connectionData->readPipe);
    if (status != ERROR_SUCCESS)
    {
        // even if shutdown fails, we still need to close the socket
        shutdown(connectionData->clientSocket, SD_BOTH);

        if (closesocket(connectionData->clientSocket) != ERROR_SUCCESS)
        {
            // free connectionData
            free(connectionData);

            return win_perror2(WSAGetLastError(), "closing socket");
        }

        // free connectionData
        free(connectionData);

        return win_perror2(status, "setting up pipes");
    }

    // the reading pipe/thread will be closed when the remote closes it's side of the connection
    // before the thread closes down it will close the send side of the socket
    HANDLE readPipeThread = CreateThread(NULL, 0, ReadPipeWriteSocketThread, param, 0, NULL);
    if (!readPipeThread)
    {
        status = GetLastError();
        // even if shutdown fails, we still need to close the socket
        shutdown(connectionData->clientSocket, SD_BOTH);

        CloseHandle(connectionData->writePipe);
        CloseHandle(connectionData->readPipe);

        if (closesocket(connectionData->clientSocket) != ERROR_SUCCESS)
        {
            // free connectionData
            free(connectionData);

            return win_perror2(WSAGetLastError(), "closing socket");
        }

        // free connectionData
        free(connectionData);

        return win_perror2(status, "creating read pipe thread");
    }

    // this is blocking till an eof is received on the socket
    // ignore returned status, wait for full socket shutdown before closing the socket
    ReadSocketWritePipe(connectionData);

    // block till eof is received on the read pipe (remote closed connection)
    WaitForSingleObject(readPipeThread, INFINITE);
    CloseHandle(readPipeThread);

    // socket send and receive have been closed already, close socket
    closesocket(connectionData->clientSocket);

    // free connectionData
    free(connectionData);

    return ERROR_SUCCESS;
}

ULONG RunTcpListenMode(ULONG tcpListenPort)
{
    ULONG status;
    WSADATA wsaData;
    SOCKET listenSocket = INVALID_SOCKET;
    struct addrinfo* addrInfo = NULL;
    struct addrinfo hints;
    CHAR portStr[10];
    ULONG connectionId = 1;

    // get a string representation of the port
    snprintf(portStr, 10, "%u", tcpListenPort);

    // init winsock2
    status = WSAStartup(MAKEWORD(2,2), &wsaData);
    if (status != ERROR_SUCCESS)
        return win_perror2(status, "WSAStartup");

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    // convert hostname:port to address, only listen for connections on localhost
    status = getaddrinfo("localhost", portStr, &hints, &addrInfo);
    if (status != ERROR_SUCCESS)
    {
        WSACleanup();
        return win_perror2(status, "getaddrinfo");
    }

    // create listen socket on the specified address
    listenSocket = socket(addrInfo->ai_family, addrInfo->ai_socktype, addrInfo->ai_protocol);
    if (listenSocket == INVALID_SOCKET)
    {
        status = WSAGetLastError();
        freeaddrinfo(addrInfo);
        WSACleanup();
        return win_perror2(status, "create socket");
    }

    // bind socket to network address
    status = bind(listenSocket, addrInfo->ai_addr, (int)addrInfo->ai_addrlen);
    if (status != ERROR_SUCCESS)
    {
        status = WSAGetLastError();
        closesocket(listenSocket);
        freeaddrinfo(addrInfo);
        WSACleanup();
        return win_perror2(status, "binding socket");
    }

    freeaddrinfo(addrInfo);

    // start listeing on the socket
    status = listen(listenSocket, SOMAXCONN);
    if (status != ERROR_SUCCESS)
    {
        status = WSAGetLastError();
        closesocket(listenSocket);
        WSACleanup();
        return win_perror2(status, "listening on socket");
    }

    // loop indefinitely to accept new connections
    while (TRUE)
    {
        SOCKET clientSocket = accept(listenSocket, NULL, NULL);
        if (clientSocket == INVALID_SOCKET)
        {
            status = WSAGetLastError();
            closesocket(listenSocket);
            WSACleanup();
            return win_perror2(status, "accept socket");
        }

        PCONNECTION_DATA connectionData = malloc(sizeof(CONNECTION_DATA));
        if (!connectionData)
        {
            closesocket(clientSocket);
            closesocket(listenSocket);
            WSACleanup();
            return win_perror2(ERROR_NOT_ENOUGH_MEMORY, "allocating memory connection data");
        }

        connectionData->clientSocket = clientSocket;
        connectionData->id = connectionId;

        connectionId++;

        HANDLE connectionThread = CreateThread(NULL, 0, HandleConnection, connectionData, 0, NULL);
        if (!connectionThread)
        {
            closesocket(clientSocket);
            closesocket(listenSocket);
            WSACleanup();
            return win_perror("creating tcp connection handle thread");
        }

        // close the handle, the threads finish on their own
        CloseHandle(connectionThread);
    }

    // never should come here, but just in case cleanup
    closesocket(listenSocket);
    WSACleanup();

    return ERROR_SUCCESS;
}

int __cdecl wmain(int argc, WCHAR *argv[])
{
    BOOL stdioMode = FALSE;
    ULONG tcpListenPort = 0;
    ULONG status;

    status = ProcessArguments(argv, &stdioMode, &tcpListenPort);
    if (status != ERROR_SUCCESS)
        return win_perror2(status, "ProcessArguments");

    if (stdioMode)
    {
        status = RunStdioMode();
        if (status != ERROR_SUCCESS)
            return win_perror2(status, "running qrexec-client-vm in stdio mode");
    }
    else if (tcpListenPort)
    {
        status = RunTcpListenMode(tcpListenPort);
        if (status != ERROR_SUCCESS)
            return win_perror2(status, "running qrexec-client-vm in tcp listen mode");
    }
    else
    {
        status = RequestQrexec(g_CommandLine);
        if (ERROR_SUCCESS != status)
            return win_perror2(status, "requesting local command execution");
    }

    LogVerbose("exiting");

    return ERROR_SUCCESS;
}
