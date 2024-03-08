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

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <strsafe.h>

#include <log.h>
#include <utf8-conv.h>
#include <qubes-io.h>
#include <crc32.h>

#include "filecopy.h"
#include "linux.h"
#include "filecopy-error.h"
#include "gui-progress.h"

HANDLE g_stdin = INVALID_HANDLE_VALUE;
HANDLE g_stdout = INVALID_HANDLE_VALUE;
HANDLE g_stderr = INVALID_HANDLE_VALUE;

INT64 g_totalSize = 0;
BOOL g_cancelOperation = FALSE;
UINT32 g_crc32 = 0;

static BOOL WriteWithCrc(IN HANDLE output, IN const void *buffer, IN DWORD size)
{
    LogVerbose("size %lu", size);
    g_crc32 = Crc32_ComputeBuf(g_crc32, buffer, size);
    return QioWriteBuffer(output, buffer, size);
}

static void NotifyProgress(IN DWORD size, IN FC_PROGRESS_TYPE progressType)
{
    static UINT64 cbWrittenTotal = 0;
    static UINT64 cbPreviousTotal = 0;

    cbWrittenTotal += size;
    if (cbWrittenTotal > cbPreviousTotal + PROGRESS_NOTIFY_DELTA || (progressType != PROGRESS_TYPE_NORMAL))
    {
        UpdateProgress(cbWrittenTotal, progressType);
        cbPreviousTotal = cbWrittenTotal;
    }
}

static void WaitForResult(void)
{
    struct result_header hdr;
    struct result_header_ext hdr_ext;
    char lastFilename[MAX_PATH + 1];
    char lastFilenamePrefix[] = "; Last file: ";

    LogVerbose("start");
    if (!QioReadBuffer(g_stdin, &hdr, sizeof(hdr)))
    {
        LogError("QioReadBuffer failed");
        exit(1);	// hopefully remote has produced error message
    }

    if (!QioReadBuffer(g_stdin, &hdr_ext, sizeof(hdr_ext)))
    {
        // remote used old result_header struct
        hdr_ext.last_namelen = 0;
    }

    if (hdr_ext.last_namelen > MAX_PATH)
    {
        // read only at most MAX_PATH chars
        hdr_ext.last_namelen = MAX_PATH;
    }

    if (!QioReadBuffer(g_stdin, lastFilename, hdr_ext.last_namelen))
    {
        fprintf(stderr, "Failed to get last filename\n");
        LogError("Failed to get last filename");
        hdr_ext.last_namelen = 0;
    }

    lastFilename[hdr_ext.last_namelen] = '\0';

    if (!hdr_ext.last_namelen)
    {
        /* set prefix to empty string */
        lastFilenamePrefix[0] = '\0';
    }

    if (hdr.error_code != 0)
    {
        switch (hdr.error_code)
        {
        case EEXIST:
            FcReportError(ERROR_ALREADY_EXISTS, TRUE, L"File copy: not overwriting existing file. Clean incoming dir, and retry copy%hs%hs", lastFilenamePrefix, lastFilename);
            break;
        case EINVAL:
            FcReportError(ERROR_INVALID_DATA, TRUE, L"File copy: Corrupted data from packer%hs%hs", lastFilenamePrefix, lastFilename);
            break;
        default:
            FcReportError(ERROR_UNIDENTIFIED_ERROR, TRUE, L"File copy: %hs%hs%hs", strerror(hdr.error_code), lastFilenamePrefix, lastFilename);
        }
    }

    if (hdr.crc32 != g_crc32)
    {
        FcReportError(ERROR_INVALID_DATA, TRUE, L"File transfer failed: checksum mismatch");
    }
}

static void WindowTimeToUnix(IN FILETIME *windowsTime, OUT unsigned int *unixTime, OUT unsigned int *unixTimeNsec)
{
    ULARGE_INTEGER tmp;

    tmp.LowPart = windowsTime->dwLowDateTime;
    tmp.HighPart = windowsTime->dwHighDateTime;

    *unixTimeNsec = (unsigned int) ((tmp.QuadPart % 10000000LL) * 100LL);
    *unixTime = (unsigned int) ((tmp.QuadPart / 10000000LL) - UNIX_EPOCH_OFFSET);
}

static void WriteHeaders(IN struct file_header *hdr, IN const WCHAR *fileName)
{
    char *fileNameUtf8 = NULL;
    size_t cbFileNameUtf8;

    LogVerbose("start");
    if (ERROR_SUCCESS != ConvertUTF16ToUTF8(fileName, &fileNameUtf8, &cbFileNameUtf8))
        FcReportError(GetLastError(), TRUE, L"Cannot convert path '%s' to UTF-8", fileName);

    hdr->namelen = (UINT32)cbFileNameUtf8;

    if (!WriteWithCrc(g_stdout, hdr, sizeof(*hdr)) || !WriteWithCrc(g_stdout, fileNameUtf8, hdr->namelen))
    {
        WaitForResult();
        exit(1);
    }
    free(fileNameUtf8);
}

static void ProcessSingleFile(IN const WCHAR *fileName, IN DWORD fileAttributes)
{
    struct file_header hdr;
    HANDLE input;
    FILETIME accessTime, modificationTime;

    LogDebug("%s", fileName);
    if (fileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        hdr.mode = 0755 | 0040000;
    else
        hdr.mode = 0644 | 0100000;

    // FILE_FLAG_BACKUP_SEMANTICS required to access directories
    input = CreateFile(fileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (input == INVALID_HANDLE_VALUE)
        FcReportError(GetLastError(), TRUE, L"Cannot open file '%s'", fileName);

    /* FIXME: times are already retrieved by FindFirst/NextFile */
    if (!GetFileTime(input, NULL, &accessTime, &modificationTime))
    {
        FcReportError(GetLastError(), TRUE, L"Cannot get time of file '%s'", fileName);
        CloseHandle(input);
    }

    WindowTimeToUnix(&accessTime, &hdr.atime, &hdr.atime_nsec);
    WindowTimeToUnix(&modificationTime, &hdr.mtime, &hdr.mtime_nsec);

    if ((fileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
    { /* FIXME: symlink */
        FC_COPY_STATUS copyResult;
        LARGE_INTEGER size;

        if (!GetFileSizeEx(input, &size))
        {
            FcReportError(GetLastError(), TRUE, L"Cannot get size of file '%s'", fileName);
            CloseHandle(input);
        }

        hdr.filelen = size.QuadPart;
        WriteHeaders(&hdr, fileName);
        copyResult = FcCopyFile(g_stdout, input, hdr.filelen, &g_crc32, NotifyProgress);

        // if COPY_FILE_WRITE_ERROR, hopefully remote will produce a message
        if (copyResult != COPY_FILE_OK)
        {
            if (copyResult != COPY_FILE_WRITE_ERROR)
            {
                FcReportError(GetLastError(), TRUE, L"Error copying file '%s': %hs", fileName, FcStatusToString(copyResult));
                CloseHandle(input);
            }
            else
            {
                WaitForResult();
                exit(1);
            }
        }
    }

    if (fileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    {
        hdr.filelen = 0;
        WriteHeaders(&hdr, fileName);
    }

    /* TODO */
#if 0
    if (S_ISLNK(mode))
    {
        char name[st->st_size + 1];
        if (readlink(pwszFilename, name, sizeof(name)) != st->st_size)
            GuiFatal(L"readlink %s", pwszFilename);
        hdr.filelen = st->st_size + 1;
        write_headers(&hdr, pwszFilename);
        if (!WriteWithCrc(1, name, st->st_size + 1))
            exit(1);
    }
#endif
    CloseHandle(input);
}

static INT64 GetFileSizeByPath(IN const WCHAR *filePath)
{
    HANDLE file;
    LARGE_INTEGER fileSize;

    file = CreateFile(filePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (file == INVALID_HANDLE_VALUE)
        FcReportError(GetLastError(), TRUE, L"Cannot open file '%s'L 0x%x", filePath, GetLastError());

    if (!GetFileSizeEx(file, &fileSize))
    {
        CloseHandle(file);
        FcReportError(GetLastError(), TRUE, L"Cannot get file size of '%s'", filePath);
    }

    CloseHandle(file);
    LogDebug("%s: %I64d", filePath, fileSize.QuadPart);
    return fileSize.QuadPart;
}

static INT64 ProcessDirectory(IN const WCHAR *directoryPath, IN BOOL calculateSize)
{
    WCHAR *currentPath;
    size_t cchCurrentPath, cchSearchPath;
    DWORD attributes;
    WIN32_FIND_DATA findData;
    WCHAR *searchPath;
    HANDLE searchHandle;
    INT64 size = 0;

    LogDebug("%s", directoryPath);
    if ((attributes = GetFileAttributes(directoryPath)) == INVALID_FILE_ATTRIBUTES)
        FcReportError(GetLastError(), TRUE, L"Cannot get attributes of '%s'", directoryPath);

    if (!calculateSize)
        ProcessSingleFile(directoryPath, attributes);

    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY))
    {
        if (calculateSize)
            return GetFileSizeByPath(directoryPath);
        else
            return 0;
    }

    cchSearchPath = wcslen(directoryPath) + 3;
    searchPath = malloc(sizeof(WCHAR)*cchSearchPath);

    if (!searchPath)
    {
        FcReportError(ERROR_OUTOFMEMORY, TRUE, L"ProcessDirectory(%s) failed", directoryPath);
    }

    if (FAILED(StringCchPrintf(searchPath, cchSearchPath, L"%s\\*", directoryPath)))
        FcReportError(ERROR_BAD_PATHNAME, TRUE, L"ProcessDirectory(%s) failed", directoryPath);

    searchHandle = FindFirstFile(searchPath, &findData);

    if (searchHandle == INVALID_HANDLE_VALUE)
    {
        DWORD status = GetLastError();
        if (status == ERROR_FILE_NOT_FOUND) // empty directory
            return 0;
        FcReportError(status, TRUE, L"Cannot list directory '%s'", directoryPath);
    }

    do
    {
        if (!wcscmp(findData.cFileName, L".") || !wcscmp(findData.cFileName, L".."))
            continue;

        cchCurrentPath = wcslen(directoryPath) + wcslen(findData.cFileName) + 2;
        currentPath = malloc(sizeof(WCHAR)*cchCurrentPath);

        if (!currentPath)
            FcReportError(ERROR_OUTOFMEMORY, TRUE, L"ProcessDirectory(%s) failed", directoryPath);

        // use forward slash here to send it also to the other end
        if (FAILED(StringCchPrintf(currentPath, cchCurrentPath, L"%s/%s", directoryPath, findData.cFileName)))
            FcReportError(ERROR_BAD_PATHNAME, TRUE, L"ProcessDirectory(%s) failed", directoryPath);

        size += ProcessDirectory(currentPath, calculateSize);
        free(currentPath);

        if (g_cancelOperation)
            break;
    } while (FindNextFile(searchHandle, &findData));

    FindClose(searchHandle);
    // directory metadata is resent; this makes the code simple,
    // and the atime/mtime is set correctly at the second time
    if (!calculateSize)
        ProcessSingleFile(directoryPath, attributes);
    return size;
}

static void NotifyEndAndWaitForResult(void)
{
    struct file_header endHeader;

    LogVerbose("start");
    /* nofity end of transfer */
    ZeroMemory(&endHeader, sizeof(endHeader));
    endHeader.namelen = 0;
    endHeader.filelen = 0;
    WriteWithCrc(g_stdout, &endHeader, sizeof(endHeader));

    WaitForResult();
}

static WCHAR *GetAbsolutePath(IN const WCHAR *currentDirectory, IN const WCHAR *path)
{
    WCHAR *absolutePath;
    size_t cchAbsolutePath;

    if (!PathIsRelative(path))
        return _wcsdup(path);

    cchAbsolutePath = wcslen(currentDirectory) + wcslen(path) + 2;
    absolutePath = malloc(sizeof(WCHAR)*cchAbsolutePath);

    if (!absolutePath)
    {
        return NULL;
    }

    if (FAILED(StringCchPrintf(absolutePath, cchAbsolutePath, L"%s\\%s", currentDirectory, path)))
    {
        free(absolutePath);
        LogError("StringCchPrintf failed, currentDirectory='%s', path='%s'", currentDirectory, path);
        return NULL;
    }

    return absolutePath;
}

int __cdecl wmain(int argc, WCHAR *argv[])
{
    int i;
    WCHAR *directory, *baseName;
    WCHAR currentDirectory[MAX_PATH_LENGTH];

    LogVerbose("start");
    g_stderr = GetStdHandle(STD_ERROR_HANDLE);

    if (g_stderr == NULL || g_stderr == INVALID_HANDLE_VALUE)
    {
        FcReportError(GetLastError(), TRUE, L"Failed to get STDERR handle");
        exit(1);
    }

    g_stdin = GetStdHandle(STD_INPUT_HANDLE);

    if (g_stdin == NULL || g_stdin == INVALID_HANDLE_VALUE)
    {
        FcReportError(GetLastError(), TRUE, L"Failed to get STDIN handle");
        exit(1);
    }

    g_stdout = GetStdHandle(STD_OUTPUT_HANDLE);

    if (g_stdout == NULL || g_stdout == INVALID_HANDLE_VALUE)
    {
        FcReportError(GetLastError(), TRUE, L"Failed to get STDOUT handle");
        exit(1);
    }

    NotifyProgress(0, PROGRESS_TYPE_INIT);
    g_crc32 = 0;

    if (!GetCurrentDirectory(RTL_NUMBER_OF(currentDirectory), currentDirectory))
    {
        FcReportError(GetLastError(), TRUE, L"Failed to get current directory");
        exit(1);
    }

    LogDebug("Current directory: %s", currentDirectory);
    // calculate total size for progressbar purpose
    g_totalSize = 0;

    for (i = 1; i < argc; i++)
    {
        if (g_cancelOperation)
            break;
        // do not change dir, as don't care about form of the path here
        g_totalSize += ProcessDirectory(argv[i], TRUE);
    }

    for (i = 1; i < argc; i++)
    {
        if (g_cancelOperation)
            break;

        directory = GetAbsolutePath(currentDirectory, argv[i]);

        if (!directory)
        {
            FcReportError(ERROR_BAD_PATHNAME, TRUE, L"GetAbsolutePath '%s'", argv[i]); // exits
        }

        // absolute path has at least one character
        if (PathGetCharType(directory[wcslen(directory) - 1]) & GCT_SEPARATOR)
            directory[wcslen(directory) - 1] = L'\0';

        baseName = _wcsdup(directory);
        PathStripPath(baseName);
        PathRemoveFileSpec(directory);

        if (!SetCurrentDirectory(directory))
            FcReportError(GetLastError(), TRUE, L"SetCurrentDirectory(%s)", directory);

        ProcessDirectory(baseName, FALSE);
        free(directory);
        free(baseName);
    }

    NotifyEndAndWaitForResult();
    NotifyProgress(0, PROGRESS_TYPE_DONE);
    LogVerbose("end");
    return 0;
}
