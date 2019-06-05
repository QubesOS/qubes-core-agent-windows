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
#include <stdio.h>
#include <stdlib.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <strsafe.h>

#include <log.h>

#ifdef __MINGW32__
#include "customddkinc.h"
#endif
#include "wdk.h"

HANDLE g_stdin = INVALID_HANDLE_VALUE;
HANDLE g_stdout = INVALID_HANDLE_VALUE;
HANDLE g_stderr = INVALID_HANDLE_VALUE;

#define INCOMING_DIR_ROOT L"QubesIncoming"

int ReceiveFiles();

WCHAR g_mappedDriveLetter = L'\0';

ULONG MapDriveLetter(IN const WCHAR *targetDirectory, OUT WCHAR *driveLetter)
{
    UNICODE_STRING directoryNameU;
    UNICODE_STRING driveLetterU;
    UNICODE_STRING targetDirectoryU;
    OBJECT_ATTRIBUTES oa;
    HANDLE directoryObject;
    HANDLE linkObject;
    NTSTATUS status;
    WCHAR driveLetterBuffer[3];
    WCHAR objectDirectoryName[100];
    WCHAR devicePath[MAX_PATH];
    WCHAR targetDirectoryPath[MAX_PATH * 2];	// may be longer than MAX_PATH, but not much
    WCHAR targetDirectoryDriveLetter[3];
    HRESULT hresult;
    DWORD logicalDrives;
    UCHAR i;

    if (!targetDirectory || !driveLetter)
        return ERROR_INVALID_PARAMETER;

    hresult = StringCchPrintf(
        objectDirectoryName,
        RTL_NUMBER_OF(objectDirectoryName),
        L"\\BaseNamedObjects\\filecopy-unpacker-%d",
        GetCurrentProcessId());

    if (FAILED(hresult))
    {
        return perror2(hresult, "StringCchPrintf");
    }

    hresult = StringCchCopyN(targetDirectoryDriveLetter, RTL_NUMBER_OF(targetDirectoryDriveLetter), targetDirectory, 2);
    if (FAILED(hresult))
    {
        return perror2(hresult, "StringCchCopyN");
    }

    ZeroMemory(&devicePath, sizeof(devicePath));
    if (!QueryDosDevice(targetDirectoryDriveLetter, devicePath, RTL_NUMBER_OF(devicePath)))
    {
        return perror("QueryDosDevice");
    }

    // Translate the directory path to a form of \Device\HarddiskVolumeN\path
    hresult = StringCchPrintf(
        targetDirectoryPath,
        RTL_NUMBER_OF(targetDirectoryPath),
        L"%s%s",
        devicePath,
        (WCHAR *) &targetDirectory[2]);

    if (FAILED(hresult))
    {
        return perror2(hresult, "StringCchPrintf");
    }

    logicalDrives = GetLogicalDrives();
    i = 'Z';
    while ((logicalDrives & (1 << (i - 'A'))) && i)
        i--;

    if (!i)
    {
        LogError("Could not find a spare drive letter\n");
        return ERROR_ALREADY_EXISTS;
    }

    ZeroMemory(&driveLetterBuffer, sizeof(driveLetterBuffer));
    driveLetterBuffer[0] = i;
    driveLetterBuffer[1] = L':';

    RtlInitUnicodeString(&directoryNameU, objectDirectoryName);
    InitializeObjectAttributes(&oa, &directoryNameU, OBJ_CASE_INSENSITIVE, NULL, NULL);

    status = ZwCreateDirectoryObject(&directoryObject, DIRECTORY_ALL_ACCESS, &oa);
    if (!NT_SUCCESS(status))
    {
        LogError("ZwCreateDirectoryObject failed with status 0x%08X\n", status);
        return status;
    }

    RtlInitUnicodeString(&driveLetterU, driveLetterBuffer);
    RtlInitUnicodeString(&targetDirectoryU, targetDirectoryPath);
    InitializeObjectAttributes(&oa, &driveLetterU, OBJ_CASE_INSENSITIVE, directoryObject, NULL);

    status = ZwCreateSymbolicLinkObject(&linkObject, 0, &oa, &targetDirectoryU);
    if (!NT_SUCCESS(status))
    {
        ZwClose(directoryObject);
        LogError("ZwCreateSymbolicLinkObject failed with status 0x%08X\n", status);
        return status;
    }

    status = ZwSetInformationProcess(GetCurrentProcess(), ProcessDeviceMap, &directoryObject, sizeof(directoryObject));
    if (!NT_SUCCESS(status))
    {
        LogError("ZwSetInformationProcess failed with status 0x%08X\n", status);
        ZwClose(linkObject);
        ZwClose(directoryObject);
        return status;
    }

    *driveLetter = driveLetterBuffer[0];

    return ERROR_SUCCESS;
}

int __cdecl wmain(int argc, WCHAR *argv[])
{
    WCHAR incomingDir[MAX_PATH + 1];
    WCHAR *documentsPath = NULL;
    HRESULT hresult;
    ULONG errorCode;
    WCHAR remoteDomainName[MAX_PATH];

    g_stderr = GetStdHandle(STD_ERROR_HANDLE);
    if (g_stderr == NULL || g_stderr == INVALID_HANDLE_VALUE)
    {
        return perror("GetStdHandle(STD_ERROR_HANDLE)");
    }

    g_stdin = GetStdHandle(STD_INPUT_HANDLE);
    if (g_stdin == NULL || g_stdin == INVALID_HANDLE_VALUE)
    {
        return perror("GetStdHandle(STD_INPUT_HANDLE)");
    }

    g_stdout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (g_stdout == NULL || g_stdout == INVALID_HANDLE_VALUE)
    {
        return perror("GetStdHandle(STD_OUTPUT_HANDLE)");
    }

    if (!GetEnvironmentVariable(L"QREXEC_REMOTE_DOMAIN", remoteDomainName, RTL_NUMBER_OF(remoteDomainName)))
    {
        return perror("GetEnvironmentVariable(QREXEC_REMOTE_DOMAIN)");
    }

    hresult = SHGetKnownFolderPath(&FOLDERID_Documents, KF_FLAG_CREATE, NULL, &documentsPath);
    if (FAILED(hresult))
    {
        return perror2(hresult, "SHGetKnownFolderPath");
    }

    hresult = StringCchPrintf(
        incomingDir,
        RTL_NUMBER_OF(incomingDir),
        L"%s\\%s\\%s",
        documentsPath,
        INCOMING_DIR_ROOT,
        remoteDomainName);

    CoTaskMemFree(documentsPath);

    if (FAILED(hresult))
    {
        return perror2(hresult, "StringCchPrintf");
    }

    errorCode = SHCreateDirectoryEx(NULL, incomingDir, NULL);
    if (ERROR_SUCCESS != errorCode && ERROR_ALREADY_EXISTS != errorCode)
    {
        return perror2(hresult, "SHCreateDirectoryEx");
    }

    errorCode = MapDriveLetter(incomingDir, &g_mappedDriveLetter);
    if (ERROR_SUCCESS != errorCode)
    {
        return perror2(errorCode, "MapDriveLetter");
    }

    return ReceiveFiles();
}
