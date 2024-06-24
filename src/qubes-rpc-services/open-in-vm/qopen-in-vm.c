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
#include <strsafe.h>
#include <string.h>
#include <stdlib.h>
#include <Shlwapi.h>

#include <log.h>
#include <utf8-conv.h>
#include <qubes-io.h>

#include "filecopy-error.h"
#include "dvm2.h"

void SendFile(IN const WCHAR *filePath)
{
    LogDebug("processing local file: %s", filePath);
    HANDLE stdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE file = CreateFile(filePath, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if (file == INVALID_HANDLE_VALUE)
        FcReportError(GetLastError(), L"open '%s'", filePath);

    const WCHAR* fileName = PathFindFileNameW(filePath);

    char* fileNameUtf8;
    size_t lenUtf8;
    if (ERROR_SUCCESS != ConvertUTF16ToUTF8Static(fileName, &fileNameUtf8, &lenUtf8))
        FcReportError(GetLastError(), L"Failed to convert filename '%s' to UTF8", fileName);

    if (lenUtf8 >= DVM_FILENAME_SIZE)
        FcReportError(ERROR_FILENAME_EXCED_RANGE, L"converted file name is too long (%zu)", lenUtf8);

    char zero[DVM_FILENAME_SIZE] = { 0 };

    if (!QioWriteBuffer(stdOut, fileNameUtf8, (DWORD)lenUtf8))
        FcReportError(GetLastError(), L"send filename");

    if (!QioWriteBuffer(stdOut, zero, (DWORD)(DVM_FILENAME_SIZE - lenUtf8)))
        FcReportError(GetLastError(), L"send filename padding");

    if (!QioCopyUntilEof(stdOut, file))
        FcReportError(GetLastError(), L"send file");

    CloseHandle(file);
    LogDebug("File sent");
    CloseHandle(stdOut);
}

void ReceiveFile(IN const WCHAR *fileName)
{
    HANDLE stdIn = GetStdHandle(STD_INPUT_HANDLE);

    LogDebug("file: %s", fileName);
    WCHAR* tempDirPath = malloc(MAX_PATH_LONG_WSIZE);
    WCHAR* tempFilePath = malloc(MAX_PATH_LONG_WSIZE);
    if (!tempDirPath || !tempFilePath)
        FcReportError(ERROR_OUTOFMEMORY, L"allocate memory");

    // prepare temporary path
    if (!GetTempPathW(MAX_PATH_LONG, tempDirPath))
        FcReportError(GetLastError(), L"Failed to get temp dir");

    if (!GetTempFileNameW(tempDirPath, L"qvm", 0, tempFilePath))
        FcReportError(GetLastError(), L"Failed to get temp file");

    LogDebug("temp path: %s\\%s", tempDirPath, tempFilePath);
    // create temp file
    HANDLE tempFile = CreateFile(tempFilePath, GENERIC_WRITE, 0, NULL, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (tempFile == INVALID_HANDLE_VALUE)
        FcReportError(GetLastError(), L"Failed to open temp file");

    LogDebug("receiving file");
    if (!QioCopyUntilEof(tempFile, stdIn))
        FcReportError(GetLastError(), L"receiving file from dispVM");

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(tempFile, &fileSize))
        FcReportError(GetLastError(), L"GetFileSizeEx");

    CloseHandle(tempFile);

    if (fileSize.QuadPart == 0)
    {
        DeleteFile(tempFilePath);
        goto cleanup;
    }

    LogDebug("replacing local file");
    if (!MoveFileEx(tempFilePath, fileName, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
        FcReportError(GetLastError(), L"rename");

cleanup:
    free(tempDirPath);
    free(tempFilePath);
    LogDebug("end");
}

int __cdecl wmain(int argc, WCHAR *argv[])
{
    if (argc != 2)
        FcReportError(ERROR_BAD_ARGUMENTS, L"OpenInVM - no file given?");

    LogDebug("start");

    SendFile(argv[1]);
    ReceiveFile(argv[1]);

    LogDebug("end");
    return 0;
}
