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
#include <errno.h>
#include <shlwapi.h>
#include <strsafe.h>

#include <utf8-conv.h>
#include <qubes-io.h>
#include <log.h>
#include <crc32.h>

#include "linux.h"
#include "filecopy.h"

char g_untrustedName[MAX_PATH_LENGTH];
INT64 g_bytesLimit = 0;
INT64 g_filesLimit = 0;
INT64 g_totalBytesReceived = 0;
INT64 g_totalFilesReceived = 0;
ULONG g_crc32 = 0;

extern HANDLE g_stdin;
extern HANDLE g_stdout;
extern WCHAR g_mappedDriveLetter;

static int xutftowcsn(wchar_t *wcs, const char *utfs, size_t wcslen, int utflen)
{
    int upos = 0, wpos = 0;
    const unsigned char *utf = (const unsigned char*) utfs;
    if (!utf || !wcs || wcslen < 1) {
        errno = EINVAL;
        return -1;
    }
    /* reserve space for \0 */
    wcslen--;
    if (utflen < 0)
        utflen = INT_MAX;

    while (upos < utflen) {
        int c = utf[upos++] & 0xff;
        if (utflen == INT_MAX && c == 0)
            break;

        if (wpos >= (int)wcslen) {
            wcs[wpos] = 0;
            errno = ERANGE;
            return -1;
        }

        if (c < 0x80) {
            /* ASCII */
            // check colon symbol ':'
            if (c == 0x3a) {
                c = '_';
            }
            wcs[wpos++] = c;
        } else if (c >= 0xc2 && c < 0xe0 && upos < utflen &&
                   (utf[upos] & 0xc0) == 0x80) {
            /* 2-byte utf-8 */
            c = ((c & 0x1f) << 6);
            c |= (utf[upos++] & 0x3f);
            wcs[wpos++] = c;
        } else if (c >= 0xe0 && c < 0xf0 && upos + 1 < utflen &&
                   !(c == 0xe0 && utf[upos] < 0xa0) && /* over-long encoding */
                   (utf[upos] & 0xc0) == 0x80 &&
                   (utf[upos + 1] & 0xc0) == 0x80) {
            /* 3-byte utf-8 */
            c = ((c & 0x0f) << 12);
            c |= ((utf[upos++] & 0x3f) << 6);
            c |= (utf[upos++] & 0x3f);
            wcs[wpos++] = c;
        } else if (c >= 0xf0 && c < 0xf5 && upos + 2 < utflen &&
                   wpos + 1 < wcslen &&
                   !(c == 0xf0 && utf[upos] < 0x90) && /* over-long encoding */
                   !(c == 0xf4 && utf[upos] >= 0x90) && /* > \u10ffff */
                   (utf[upos] & 0xc0) == 0x80 &&
                   (utf[upos + 1] & 0xc0) == 0x80 &&
                   (utf[upos + 2] & 0xc0) == 0x80) {
            /* 4-byte utf-8: convert to \ud8xx \udcxx surrogate pair */
            c = ((c & 0x07) << 18);
            c |= ((utf[upos++] & 0x3f) << 12);
            c |= ((utf[upos++] & 0x3f) << 6);
            c |= (utf[upos++] & 0x3f);
            c -= 0x10000;
            wcs[wpos++] = 0xd800 | (c >> 10);
            wcs[wpos++] = 0xdc00 | (c & 0x3ff);
        } else if (c >= 0xa0) {
            /* invalid utf-8 byte, printable unicode char: convert 1:1 */
            wcs[wpos++] = c;
        } else {
            /* invalid utf-8 byte, non-printable unicode: convert to hex */
            static const char *hex = "0123456789abcdef";
            wcs[wpos++] = hex[c >> 4];
            if (wpos < wcslen)
                wcs[wpos++] = hex[c & 0x0f];
        }
    }
    wcs[wpos] = 0;
    return wpos;
}

static inline int xutftowcs_path_ex(wchar_t *wcs, const char *utf,
                                    size_t wcslen, int utflen)
{
    int result = xutftowcsn(wcs, utf, wcslen, utflen);
    if (result < 0 && errno == ERANGE)
        errno = ENAMETOOLONG;
    return result;
}

static inline int xutftowcs_path(wchar_t *wcs, const char *utf)
{
    return xutftowcs_path_ex(wcs, utf, MAX_PATH, -1);
}

void SetSizeLimit(IN INT64 bytesLimit, IN INT64 filesLimit)
{
    g_bytesLimit = bytesLimit;
    g_filesLimit = filesLimit;
}

BOOL ReadWithCrc(IN HANDLE input, OUT void *buffer, IN DWORD bufferSize)
{
    BOOL ret;

    LogVerbose("%lu", bufferSize);
    ret = QioReadBuffer(input, buffer, bufferSize);
    if (ret)
        g_crc32 = Crc32_ComputeBuf(g_crc32, buffer, bufferSize);

    return ret;
}

void SendStatusAndCrc(IN UINT32 statusCode, IN const char *lastFileName OPTIONAL)
{
    struct result_header header;
    struct result_header_ext headerExt;

    LogVerbose("status %lu", statusCode);
    header.error_code = statusCode;
    header.crc32 = g_crc32;
    if (!QioWriteBuffer(g_stdout, &header, sizeof(header)))
    {

    }

    if (lastFileName)
    {
        headerExt.last_namelen = (UINT32)strlen(lastFileName);
        QioWriteBuffer(g_stdout, &headerExt, sizeof(headerExt));
        QioWriteBuffer(g_stdout, lastFileName, headerExt.last_namelen);
    }
}

void SendStatusAndExit(IN UINT32 statusCode, IN const char *lastFileName)
{
    if (statusCode == LEGAL_EOF)
        statusCode = 0;

    LogDebug("status %lu, last file %S", statusCode, lastFileName);
    SendStatusAndCrc(statusCode, lastFileName);
    CloseHandle(g_stdout);
    exit(statusCode);
}

void ProcessRegularFile(IN const struct file_header *untrustedHeader, IN const char *untrustedNameUtf8)
{
    FC_COPY_STATUS copyStatus;
    ULONG errorCode;
    HANDLE outputFile;
    WCHAR *untrustedFileName = NULL;
    WCHAR trustedFilePath[MAX_PATH + 1];
    HRESULT hresult;

    untrustedFileName = (WCHAR*)calloc((MAX_PATH + 1) * sizeof(WCHAR), 1);
    if (!untrustedFileName)
        SendStatusAndExit(EINVAL, NULL);
    int result = xutftowcs_path(untrustedFileName, untrustedNameUtf8);
    if (result <= 0)
        SendStatusAndExit(EINVAL, NULL);

    LogDebug("file '%s'", untrustedFileName);
    hresult = StringCchPrintf(
        trustedFilePath,
        RTL_NUMBER_OF(trustedFilePath),
        L"%c:\\%s",
        g_mappedDriveLetter,
        untrustedFileName);

    free(untrustedFileName);

    if (FAILED(hresult))
        SendStatusAndExit(EINVAL, untrustedNameUtf8);

    outputFile = CreateFile(trustedFilePath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, 0, NULL);	/* safe because of chroot */
    if (INVALID_HANDLE_VALUE == outputFile)
    {
        // maybe some more complete error code translation needed here, but
        // anyway qfile-agent will handle only those listed below
        if (GetLastError() == ERROR_FILE_EXISTS)
            SendStatusAndExit(EEXIST, untrustedNameUtf8);
        else if (GetLastError() == ERROR_ACCESS_DENIED)
            SendStatusAndExit(EACCES, untrustedNameUtf8);
        else
            SendStatusAndExit(EIO, untrustedNameUtf8);
    }

    g_totalBytesReceived += untrustedHeader->filelen;
    if (g_bytesLimit && g_totalBytesReceived > g_bytesLimit)
        SendStatusAndExit(EDQUOT, untrustedNameUtf8);

    // receive file data from stdin
    copyStatus = FcCopyFile(outputFile, g_stdin, untrustedHeader->filelen, &g_crc32, NULL);
    if (copyStatus != COPY_FILE_OK)
    {
        SendStatusAndExit(EIO, untrustedNameUtf8);
    }

    CloseHandle(outputFile);
}

void ProcessDirectory(IN const struct file_header *untrustedHeader, IN const char *untrustedNameUtf8)
{
    ULONG errorCode;
    WCHAR *untrustedDirectoryName = NULL;
    WCHAR trustedDirectoryPath[MAX_PATH + 1];
    HRESULT hresult;

    untrustedDirectoryName = (WCHAR*)calloc((MAX_PATH + 1) * sizeof(WCHAR), 1);
    if (!untrustedDirectoryName)
        SendStatusAndExit(EINVAL, NULL);
    int result = xutftowcs_path(untrustedDirectoryName, untrustedNameUtf8);
    if (result <= 0)
        SendStatusAndExit(EINVAL, NULL);

    LogDebug("dir '%s'", untrustedDirectoryName);
    hresult = StringCchPrintf(
        trustedDirectoryPath,
        RTL_NUMBER_OF(trustedDirectoryPath),
        L"%c:\\%s",
        g_mappedDriveLetter,
        untrustedDirectoryName);
    free(untrustedDirectoryName);

    if (FAILED(hresult))
        SendStatusAndExit(EINVAL, untrustedNameUtf8);

    if (!CreateDirectory(trustedDirectoryPath, NULL))
    {	/* safe because of chroot */
        errorCode = GetLastError();
        if (ERROR_ALREADY_EXISTS != errorCode)
            SendStatusAndExit(ENOTDIR, untrustedNameUtf8);
    }
}

void ProcessLink(IN const struct file_header *untrustedHeader, IN const char *untrustedNameUtf8)
{
    char untrustedLinkTargetPathUtf8[MAX_PATH_LENGTH];
    DWORD linkTargetSize;
    WCHAR *untrustedName = NULL;
    WCHAR trustedFilePath[MAX_PATH + 1];
    WCHAR *untrustedLinkTargetPath = NULL;
    WCHAR untrustedLinkTargetAbsolutePath[MAX_PATH + 1];
    BOOL success;
    HRESULT hresult;
    BOOL targetIsFile = FALSE; /* default to directory links */

    untrustedName = (WCHAR*)calloc((MAX_PATH + 1) * sizeof(WCHAR), 1);
    if (!untrustedName)
        SendStatusAndExit(EINVAL, NULL);
    int result = xutftowcs_path(untrustedName, untrustedNameUtf8);
    if (result <= 0)
        SendStatusAndExit(EINVAL, NULL);

    LogDebug("link '%s'", untrustedName);
    hresult = StringCchPrintf(
        trustedFilePath,
        RTL_NUMBER_OF(trustedFilePath),
        L"%c:\\%s",
        g_mappedDriveLetter,
        untrustedName);

    free(untrustedName);

    if (FAILED(hresult))
        SendStatusAndExit(EINVAL, untrustedNameUtf8);

    if (untrustedHeader->filelen > MAX_PATH - 1)
        SendStatusAndExit(ENAMETOOLONG, untrustedNameUtf8);

    linkTargetSize = (DWORD) untrustedHeader->filelen; // sanitized above
    if (!ReadWithCrc(g_stdin, untrustedLinkTargetPathUtf8, linkTargetSize))
        SendStatusAndExit(EIO, untrustedNameUtf8);

    untrustedLinkTargetPathUtf8[linkTargetSize] = 0;

    untrustedLinkTargetPath = (WCHAR*)calloc((MAX_PATH + 1) * sizeof(WCHAR), 1);
    if (!untrustedLinkTargetPath)
        SendStatusAndExit(EINVAL, NULL);
    result = xutftowcs_path(untrustedLinkTargetPath, untrustedLinkTargetPathUtf8);
    if (result <= 0)
        SendStatusAndExit(EINVAL, untrustedNameUtf8);

    LogDebug("target '%s'", untrustedLinkTargetPath);
    /* TODO? sanitize link target path in any way? we don't allow to override
     * existing files, so this shouldn't be a problem to leave it alone */

    /* try to determine if link target is a file or directory */
    if (PathIsRelative(untrustedLinkTargetPath))
    {
        WCHAR tempPath[MAX_PATH + 1];

        hresult = StringCchPrintfW(
            tempPath,
            RTL_NUMBER_OF(tempPath),
            L"%c:\\%s",
            g_mappedDriveLetter,
            untrustedLinkTargetPath);

        *(PathFindFileName(tempPath)) = L'\0';
        if (!PathCombine(untrustedLinkTargetAbsolutePath, tempPath, untrustedLinkTargetPath))
        {
            free(untrustedLinkTargetPath);
            SendStatusAndExit(EINVAL, untrustedNameUtf8);
        }

        if (PathFileExists(untrustedLinkTargetAbsolutePath) && !PathIsDirectory(untrustedLinkTargetAbsolutePath))
        {
            targetIsFile = TRUE;
        }
    }
    else
    {
        free(untrustedLinkTargetPath);
        /* deny absolute links */
        SendStatusAndExit(EPERM, untrustedNameUtf8);
    }

    success = CreateSymbolicLink(trustedFilePath, untrustedLinkTargetPath,
        targetIsFile ? 0 : SYMBOLIC_LINK_FLAG_DIRECTORY);

    free(untrustedLinkTargetPath);

    if (!success)
    {
        if (GetLastError() == ERROR_FILE_EXISTS)
            SendStatusAndExit(EEXIST, untrustedNameUtf8);
        else if (GetLastError() == ERROR_ACCESS_DENIED)
            SendStatusAndExit(EACCES, untrustedNameUtf8);
        else if (GetLastError() == ERROR_PRIVILEGE_NOT_HELD)
            SendStatusAndExit(EACCES, untrustedNameUtf8);
        else
            SendStatusAndExit(EIO, untrustedNameUtf8);
    }
}

void ProcessEntry(IN const struct file_header *untrustedHeader)
{
    UINT32 nameSize;

    if (untrustedHeader->namelen > MAX_PATH_LENGTH - 1)
        SendStatusAndExit(ENAMETOOLONG, NULL);

    nameSize = untrustedHeader->namelen;	/* sanitized above */
    if (!ReadWithCrc(g_stdin, g_untrustedName, nameSize))
        SendStatusAndExit(LEGAL_EOF, NULL);	// hopefully remote has produced error message

    g_untrustedName[nameSize] = 0;
    if (S_ISREG(untrustedHeader->mode))
        ProcessRegularFile(untrustedHeader, g_untrustedName);
    else if (S_ISLNK(untrustedHeader->mode))
        ProcessLink(untrustedHeader, g_untrustedName);
    else if (S_ISDIR(untrustedHeader->mode))
        ProcessDirectory(untrustedHeader, g_untrustedName);
    else
        SendStatusAndExit(EINVAL, g_untrustedName);
}

int ReceiveFiles(void)
{
    struct file_header untrustedHeader;

    /* initialize checksum */
    g_crc32 = 0;
    while (ReadWithCrc(g_stdin, &untrustedHeader, sizeof untrustedHeader))
    {
        /* check for end of transfer marker */
        if (untrustedHeader.namelen == 0)
        {
            errno = 0;
            break;
        }

        ProcessEntry(&untrustedHeader);
        g_totalFilesReceived++;

        if (g_filesLimit && g_totalFilesReceived > g_filesLimit)
            SendStatusAndExit(EDQUOT, g_untrustedName);
    }
    SendStatusAndCrc(errno, NULL);
    return errno;
}
