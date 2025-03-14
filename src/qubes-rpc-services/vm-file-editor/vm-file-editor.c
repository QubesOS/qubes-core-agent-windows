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
#include <string.h>
#include <stdlib.h>
#include <shellapi.h>
#include <rpc.h>
#include <strsafe.h>

#include <log.h>
#include <utf8-conv.h>
#include <qubes-io.h>

#include "dvm2.h"

HANDLE g_stdIn = INVALID_HANDLE_VALUE;
HANDLE g_stdOut = INVALID_HANDLE_VALUE;

BOOL GetTempDirectory(OUT WCHAR **dirPath, OUT size_t *cchDirPath)
{
    BOOL retval = FALSE;
    size_t cchPath = 0, cchSubdirPath = 0;
    WCHAR *subdirPath = NULL;
    UUID uuid;
    WCHAR *subdirName = NULL;

    // subdir will be a uuid
    if (UuidCreate(&uuid) != RPC_S_OK)
        goto cleanup;

    if (RPC_S_OK != UuidToString(&uuid, &subdirName))
    {
        LogError("UuidToString failed");
        goto cleanup;
    }

    cchPath = GetTempPath(0, NULL);
    if (!cchPath)
        goto cleanup;

    cchSubdirPath = cchPath + wcslen(subdirName);
    subdirPath = (WCHAR*) malloc(cchSubdirPath * sizeof(WCHAR));
    if (!subdirPath)
        goto cleanup;

    cchPath = GetTempPath((DWORD)cchPath, subdirPath);
    if (!cchPath)
        goto cleanup;

    if (FAILED(StringCchCat(subdirPath, cchSubdirPath, subdirName)))
        goto cleanup;

    if (!CreateDirectory(subdirPath, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
    {
        fprintf(stderr, "Failed to create tmp subdir: 0x%x\n", GetLastError());
        goto cleanup;
    }

    // todo? check if directory already exists
    // This is extremely unlikely, GUIDs v4 are totally PRNG-based.
    retval = TRUE;

cleanup:
    if (!retval)
    {
        free(subdirPath);
        subdirPath = NULL;
        RpcStringFree(&subdirName);
    }

    *dirPath = subdirPath;
    *cchDirPath = cchSubdirPath;
    return retval;
}

// returns base file name
WCHAR *GetTempFilePath(OUT WCHAR **tempDirPath)
{
    char fileNameUtf8[DVM_FILENAME_SIZE + 1];
    DWORD read = 0;

    // read file name from stdin
    if (!ReadFile(g_stdIn, fileNameUtf8, DVM_FILENAME_SIZE, &read, NULL) || read != DVM_FILENAME_SIZE)
    {
        fprintf(stderr, "Failed get filename: 0x%x\n", GetLastError());
        return NULL;
    }

    fileNameUtf8[DVM_FILENAME_SIZE] = 0;
    if (strchr(fileNameUtf8, '/'))
    {
        fprintf(stderr, "filename contains /");
        return NULL;
    }

    if (strchr(fileNameUtf8, '\\'))
    {
        fprintf(stderr, "filename contains \\");
        return NULL;
    }

    for (int i = 0; i < DVM_FILENAME_SIZE && fileNameUtf8[i] != 0; i++)
    {
        // replace some characters with _ (eg mimeopen have problems with some of them)
        if (strchr(" !?\"#$%^&*()[]<>;`~", fileNameUtf8[i]))
            fileNameUtf8[i] = '_';
    }

    WCHAR* fileName;
    size_t cchFileName;
    // convert to utf16
    if (ERROR_SUCCESS != ConvertUTF8ToUTF16Static(fileNameUtf8, &fileName, &cchFileName))
    {
        fprintf(stderr, "Invalid file name\n");
        return NULL;
    }

    size_t cchTempDirPath;
    if (!GetTempDirectory(tempDirPath, &cchTempDirPath))
    {
        fprintf(stderr, "Failed to get tmpdir\n");
        return NULL;
    }

    size_t cchFullPath = cchTempDirPath + cchFileName + 1;
    WCHAR* fullPath = malloc(sizeof(WCHAR) * cchFullPath);
    if (!fullPath)
        exit(ERROR_OUTOFMEMORY);

    StringCchPrintf(fullPath, cchFullPath, L"%s%s", *tempDirPath, fileName);
    return fullPath;
}

BOOL ReceiveFile(IN const WCHAR *localFilePath)
{
    BOOL retval = FALSE;
    HANDLE localFile = CreateFile(localFilePath, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);

    if (localFile == INVALID_HANDLE_VALUE)
    {
        if (GetLastError() == ERROR_FILE_EXISTS)
            fprintf(stderr, "File already exists, cleanup temp directory\n");
        else
            fprintf(stderr, "Failed to create file '%S': 0x%x\n", localFilePath, GetLastError());

        goto cleanup;
    }

    if (!QioCopyUntilEof(localFile, g_stdIn))
    {
        fprintf(stderr, "Failed to read/write file: 0x%x\n", GetLastError());
        goto cleanup;
    }

    retval = TRUE;

cleanup:
    CloseHandle(localFile);
    return retval;
}

BOOL SendFile(IN const WCHAR *localFilePath)
{
    BOOL retval = FALSE;
    HANDLE localFile = CreateFile(localFilePath, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if (localFile == INVALID_HANDLE_VALUE)
    {
        fprintf(stderr, "Failed to open file '%S': 0x%x\n", localFilePath, GetLastError());
        goto cleanup;
    }

    if (g_stdOut == INVALID_HANDLE_VALUE)
    {
        fprintf(stderr, "Failed to open STDOUT: 0x%x\n", GetLastError());
        goto cleanup;
    }

    if (!QioCopyUntilEof(g_stdOut, localFile))
    {
        fprintf(stderr, "Failed read/write file: 0x%x\n", GetLastError());
        goto cleanup;
    }

    retval = TRUE;

cleanup:
    CloseHandle(localFile);
    if (g_stdOut != INVALID_HANDLE_VALUE)
        CloseHandle(g_stdOut);
    return retval;
}

int wmain(int argc, WCHAR *argv[])
{
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    WCHAR *filePath = NULL;
    int	exitCode = 1;
    WCHAR *tempDir = NULL;

    g_stdIn = GetStdHandle(STD_INPUT_HANDLE);
    if (g_stdIn == INVALID_HANDLE_VALUE)
    {
        exitCode = win_perror("GetStdHandle(STD_INPUT_HANDLE)");
        fprintf(stderr, "Failed to open STDIN: 0x%x\n", exitCode);
        goto cleanup;
    }

    g_stdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (g_stdOut == INVALID_HANDLE_VALUE)
    {
        exitCode = win_perror("GetStdHandle(STD_OUTPUT_HANDLE)");
        fprintf(stderr, "Failed to open STDOUT: 0x%x\n", exitCode);
        goto cleanup;
    }

    filePath = GetTempFilePath(&tempDir);
    if (!filePath)
        goto cleanup;
    if (!ReceiveFile(filePath))
        goto cleanup;

    WIN32_FILE_ATTRIBUTE_DATA attributesPre;
    if (!GetFileAttributesEx(filePath, GetFileExInfoStandard, &attributesPre))
    {
        exitCode = win_perror("GetFileAttributesEx pre");
        fprintf(stderr, "Failed to get file attributes pre: 0x%x\n", exitCode);
        goto cleanup;
    }

    SHELLEXECUTEINFO sei;
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_UNICODE;
    sei.hwnd = NULL;
    sei.lpVerb = L"open";
    sei.lpFile = filePath;
    sei.lpParameters = NULL;
    sei.lpDirectory = NULL;
    sei.nShow = SW_SHOW;
    sei.hProcess = NULL;

    LogDebug("Opening '%s'", filePath);

    if (!ShellExecuteEx(&sei))
    {
        exitCode = win_perror("ShellExecuteEx");
        fprintf(stderr, "Editor startup failed: 0x%x\n", exitCode);
        goto cleanup;
    }

    if (sei.hProcess == NULL)
    {
        fprintf(stderr, "Failed to start editor process\n");
        exitCode = ERROR_UNIDENTIFIED_ERROR;
        goto cleanup;
    }

    // Wait until child process exits.
    WaitForSingleObject(sei.hProcess, INFINITE);

    DWORD childExitCode;
    if (!GetExitCodeProcess(sei.hProcess, &childExitCode))
    {
        exitCode = win_perror("GetExitCodeProcess");
        fprintf(stderr, "Cannot get editor exit code: 0x%x\n", exitCode);
        goto cleanup;
    }

    // Close child process handle.
    CloseHandle(sei.hProcess);

    if (childExitCode != ERROR_SUCCESS)
    {
        LogError("Process exit code: 0x%x", childExitCode);
        fprintf(stderr, "Editor failed: 0x%x\n", childExitCode);
        exitCode = childExitCode;
        goto cleanup;
    }

    WIN32_FILE_ATTRIBUTE_DATA attributesPost;
    if (!GetFileAttributesEx(filePath, GetFileExInfoStandard, &attributesPost))
    {
        exitCode = win_perror("GetFileAttributesEx post");
        fprintf(stderr, "Failed to get file attributes post: 0x%x\n", exitCode);
        goto cleanup;
    }

    // send file if it changed
    if (attributesPre.ftLastWriteTime.dwLowDateTime != attributesPost.ftLastWriteTime.dwLowDateTime ||
        attributesPre.ftLastWriteTime.dwHighDateTime != attributesPost.ftLastWriteTime.dwHighDateTime)
    {
        SendFile(filePath);
    }

    exitCode = ERROR_SUCCESS;

cleanup:
    if (filePath)
        DeleteFile(filePath);
    if (tempDir)
        RemoveDirectory(tempDir);
    return exitCode;
}
