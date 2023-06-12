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

#include <winsock2.h>
#include <ws2tcpip.h>
#include <ws2ipdef.h>
#include <ws2def.h>
#include <windows.h>
#include <qubes-io.h>
#include <log.h>
#include <exec.h>

#define MAX_SIZE_BUFFER 65536

SOCKET g_ClientSocket = INVALID_SOCKET;

DWORD WINAPI ReadStdinWriteSocketThread(PVOID param)
{
    HANDLE stdinHandle = GetStdHandle(STD_INPUT_HANDLE);
    BYTE buffer[MAX_SIZE_BUFFER];
    DWORD bytesRead;
    int bytesSend;

    while (ReadFile(stdinHandle, &buffer, MAX_SIZE_BUFFER, &bytesRead, NULL))
    {
        if (!bytesRead)
            break;

        bytesSend = send(g_ClientSocket, buffer, bytesRead, 0);
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

    // received an eof on the reading pipe (or encountered an error), closing stdin
    CloseHandle(stdinHandle);
    // the remote has closed the connection, disable socket sending to propogate FIN
    if (shutdown(g_ClientSocket, SD_SEND) != ERROR_SUCCESS)
        return win_perror2(WSAGetLastError(), "close socket send");

    return ERROR_SUCCESS;
}

ULONG ReadSocketWriteStdout()
{
    HANDLE stdoutHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    ULONG status;
    BYTE buffer[MAX_SIZE_BUFFER];
    int bytesRead;

    while (TRUE)
    {
        bytesRead = recv(g_ClientSocket, buffer, MAX_SIZE_BUFFER, 0);
        if (bytesRead == SOCKET_ERROR)
        {
            LogDebug("socket recv resulted in error: %d", WSAGetLastError());
            break;
        }

        if (!bytesRead || !QioWriteBuffer(stdoutHandle, &buffer, bytesRead))
            break;
    }

    // received an FIN on socket (or encountered an error when reading from the socket), close socket receiving
    status = shutdown(g_ClientSocket, SD_RECEIVE);
    // to propogate the eof from the socket to the remote, explicitly close stdout
    CloseHandle(stdoutHandle);

    if (status != ERROR_SUCCESS)
        return win_perror2(WSAGetLastError(), "close socket receive");

    return ERROR_SUCCESS;
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE previousInstance, WCHAR *commandLine, int showState)
{
    ULONG status;
    PWSTR portStr;
    CHAR finalPortStr[10];
    long port;

    // winsock data
    WSADATA wsaData;
    struct addrinfo* addrInfo = NULL;
    struct addrinfo hints;
    
    portStr = GetArgument();
    port = wcstol(portStr, NULL, 10);

    if (port < 1 || port > 65535)
    {
        LogError("Invalid port (%d) number provided to connect to.", port);
        return win_perror2(ERROR_BAD_ARGUMENTS, "wcstol");
    }

    // for added safety using the port number converted to a string instead of the original argument
    snprintf(finalPortStr, 10, "%u", port);

    // init winsock2
    status = WSAStartup(MAKEWORD(2,2), &wsaData);
    if (status != ERROR_SUCCESS)
        return win_perror2(status, "WSAStartup");

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    // convert hostname:port to address, only connect to localhost
    status = getaddrinfo("localhost", finalPortStr, &hints, &addrInfo);
    if (status != ERROR_SUCCESS)
    {
        WSACleanup();
        return win_perror2(status, "getaddrinfo");
    }

    // create client socket
    g_ClientSocket = socket(addrInfo->ai_family, addrInfo->ai_socktype, addrInfo->ai_protocol);
    if (g_ClientSocket == INVALID_SOCKET)
    {
        status = WSAGetLastError();
        freeaddrinfo(addrInfo);
        WSACleanup();
        return win_perror2(status, "create socket");
    }

    // connect client socket to tcp server
    if (connect(g_ClientSocket, addrInfo->ai_addr, (int)addrInfo->ai_addrlen) == SOCKET_ERROR)
    {
        status = WSAGetLastError();
        closesocket(g_ClientSocket);
        freeaddrinfo(addrInfo);
        WSACleanup();
        return win_perror2(status, "connect socket");
    }

    // free as no longer needed
    freeaddrinfo(addrInfo);

    HANDLE readStdinThread = CreateThread(NULL, 0, ReadStdinWriteSocketThread, NULL, 0, NULL);
    if (!readStdinThread)
    {
        status = GetLastError();
        // even if shutdown fails, we still need to close the socket
        shutdown(g_ClientSocket, SD_BOTH);

        // explicitly close stdio handles to propogate eof to the caller of this service
        // not really necessary as the handles will be closed on the imminent closure of the service
        CloseHandle(GetStdHandle(STD_INPUT_HANDLE));
        CloseHandle(GetStdHandle(STD_OUTPUT_HANDLE));

        if (closesocket(g_ClientSocket) != ERROR_SUCCESS)
            return win_perror2(WSAGetLastError(), "closing socket");

        return win_perror2(status, "creating read pipe thread");
    }

    // this is blocking till an eof is received on the socket
    // ignore returned status, wait for full socket shutdown before closing the socket
    ReadSocketWriteStdout();

    // block till eof is received on the stdin (remote closed connection)
    WaitForSingleObject(readStdinThread, INFINITE);
    CloseHandle(readStdinThread);

    // socket send and receive have been closed already, close socket
    closesocket(g_ClientSocket);
    WSACleanup();

    return ERROR_SUCCESS;
}
