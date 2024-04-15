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
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <PathCch.h>

#include <utf8-conv.h>
#include <qubes-io.h>
#include <log.h>
#include <crc32.h>

#include "linux.h"
#include "filecopy.h"

static_assert(FC_MAX_PATH < MAX_PATH_LONG, "FC_MAX_PATH must be lesser than MAX_PATH_LONG");

char g_untrustedName[FC_MAX_PATH];
INT64 g_bytesLimit = 0;
INT64 g_filesLimit = 0;
INT64 g_totalBytesReceived = 0;
INT64 g_totalFilesReceived = 0;
ULONG g_crc32 = 0;

extern HANDLE g_stdin;
extern HANDLE g_stdout;

// FIXME: see how this differs from ConvertUTF8ToUTF16 and possibly update the latter
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
            if (c == ':') {
                c = '_';
            } else if (c == '/') {
                // CanonicalizePath... APIs don't change slashes to backslashes
                // this is needed to properly compare normalized path prefixes
                c = '\\';
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

// wcs must have space for MAX_PATH_LONG WCHARs
static inline int xutftowcs_path(wchar_t *wcs, const char *utf)
{
    return xutftowcs_path_ex(wcs, utf, MAX_PATH_LONG, -1);
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
        exit((int)win_perror("sending status"));

    if (lastFileName)
    {
        headerExt.last_namelen = (UINT32)strlen(lastFileName);
        if (!QioWriteBuffer(g_stdout, &headerExt, sizeof(headerExt)))
            exit((int)win_perror("sending last filename len"));
        if (!QioWriteBuffer(g_stdout, lastFileName, headerExt.last_namelen))
            exit((int)win_perror("sending last filename"));
    }
}

DECLSPEC_NORETURN
void SendStatusAndExit(IN UINT32 statusCode, IN const char *lastFileName)
{
    if (statusCode == LEGAL_EOF)
        statusCode = 0;

    LogDebug("status %lu, last file %S", statusCode, lastFileName);
    SendStatusAndCrc(statusCode, lastFileName);
    CloseHandle(g_stdout);
    exit(statusCode);
}

// if untrustedPathUtf8 is a link target, linkPath needs to be a sanitized path of the link file
// returns buffer that needs to be freed by the caller
// sends status and exits on any failure
WCHAR* SanitizePath(IN const WCHAR* incomingDir, IN const char* untrustedPathUtf8, IN const WCHAR* linkPath OPTIONAL)
{
    LogVerbose("start");
    WCHAR* untrustedPath = (WCHAR*)malloc(MAX_PATH_LONG_WSIZE);
    if (!untrustedPath)
        SendStatusAndExit(ENOMEM, untrustedPathUtf8);

    int result = xutftowcs_path(untrustedPath, untrustedPathUtf8);
    if (result <= 0)
    {
        LogError("Failed to convert untrusted path to UTF16");
        SendStatusAndExit(EINVAL, untrustedPathUtf8);
    }

    LogDebug("untrusted path: '%s'", untrustedPath);

    if (linkPath && !PathIsRelative(untrustedPath))
    {
        LogError("link target is not relative, link path: %s", linkPath);
        SendStatusAndExit(EPERM, untrustedPathUtf8);
    }

    WCHAR* localPath = (WCHAR*)malloc(MAX_PATH_LONG_WSIZE);
    if (!localPath)
        SendStatusAndExit(ENOMEM, untrustedPathUtf8);

    HRESULT hresult;

    if (linkPath) // link targets are relative to the link itself
    {
        WCHAR* linkBase = _wcsdup(linkPath);
        hresult = PathCchRemoveFileSpec(linkBase, MAX_PATH_LONG);
        if (FAILED(hresult))
        {
            win_perror2(hresult, "removing file name from link path");
            SendStatusAndExit(EINVAL, untrustedPathUtf8);
        }

        LogDebug("link base: %s", linkBase);
        hresult = PathCchCombineEx(localPath, MAX_PATH_LONG, linkBase, untrustedPath, PATHCCH_ALLOW_LONG_PATHS);
        if (FAILED(hresult))
        {
            win_perror2(hresult, "combining link target path");
            SendStatusAndExit(EINVAL, untrustedPathUtf8);
        }
        free(linkBase);
    }
    else
    {
        hresult = PathCchCombineEx(localPath, MAX_PATH_LONG, incomingDir, untrustedPath, PATHCCH_ALLOW_LONG_PATHS);
        if (FAILED(hresult))
        {
            win_perror2(hresult, "combining full path");
            SendStatusAndExit(EINVAL, untrustedPathUtf8);
        }
    }

    free(untrustedPath);

    LogDebug("combined path: '%s'", localPath);

    WCHAR* trustedPath = (WCHAR*)malloc(MAX_PATH_LONG_WSIZE);
    if (!trustedPath)
        SendStatusAndExit(ENOMEM, untrustedPathUtf8);

    // slashes must be converted to backslashes already
    hresult = PathCchCanonicalizeEx(trustedPath, MAX_PATH_LONG, localPath, PATHCCH_ALLOW_LONG_PATHS);
    if (FAILED(hresult))
    {
        win_perror2(hresult, "canonicalizing trusted path");
        SendStatusAndExit(EINVAL, untrustedPathUtf8);
    }

    free(localPath);
    // we know canonicalFilePath is absolute, check if it really starts with incomingDir
    if (!PathIsPrefix(incomingDir, trustedPath))
    {
        LogError("canonical path '%s' is not within incoming dir '%s'", trustedPath, incomingDir);
        SendStatusAndExit(EINVAL, untrustedPathUtf8);
    }

    LogDebug("trusted path: '%s'", trustedPath);
    LogVerbose("end");
    return trustedPath;
}

void ProcessRegularFile(IN const WCHAR* incomingDir, IN const struct file_header *untrustedHeader,
    IN const char *untrustedNameUtf8)
{
    LogVerbose("start");
    WCHAR* trustedPath = SanitizePath(incomingDir, untrustedNameUtf8, NULL);

    HANDLE outputFile = CreateFile(trustedPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, 0, NULL);
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
    LogInfo("receiving file: '%s'", trustedPath);

    FC_COPY_STATUS copyStatus = FcCopyFile(outputFile, g_stdin, untrustedHeader->filelen, &g_crc32, NULL);
    if (copyStatus != COPY_FILE_OK)
        SendStatusAndExit(EIO, untrustedNameUtf8);

    free(trustedPath);
    CloseHandle(outputFile);
    LogVerbose("end");
}

void ProcessDirectory(IN const WCHAR* incomingDir, IN const struct file_header *untrustedHeader,
    IN const char *untrustedNameUtf8)
{
    LogVerbose("start");
    WCHAR* trustedPath = SanitizePath(incomingDir, untrustedNameUtf8, NULL);

    LogInfo("creating directory: '%s'", trustedPath);
    if (!CreateDirectory(trustedPath, NULL))
    {
        DWORD errorCode = GetLastError();
        if (ERROR_ALREADY_EXISTS != errorCode)
            SendStatusAndExit(ENOTDIR, untrustedNameUtf8);
    }

    free(trustedPath);
    LogVerbose("end");
}

void ProcessLink(IN const WCHAR* incomingDir, IN const struct file_header *untrustedHeader,
    IN const char *untrustedNameUtf8)
{
    LogVerbose("start");
    WCHAR* trustedLinkPath = SanitizePath(incomingDir, untrustedNameUtf8, NULL);

    if (untrustedHeader->filelen > FC_MAX_PATH - 1)
        SendStatusAndExit(ENAMETOOLONG, untrustedNameUtf8);

    DWORD linkTargetSize = (DWORD) untrustedHeader->filelen; // sanitized above

    char* untrustedLinkTargetPathUtf8 = (char*)malloc(FC_MAX_PATH);
    if (!untrustedLinkTargetPathUtf8)
        SendStatusAndExit(ENOMEM, untrustedNameUtf8);

    if (!ReadWithCrc(g_stdin, untrustedLinkTargetPathUtf8, linkTargetSize))
        SendStatusAndExit(EIO, untrustedNameUtf8);

    untrustedLinkTargetPathUtf8[linkTargetSize] = 0;

    WCHAR* trustedLinkTargetPath = SanitizePath(incomingDir, untrustedLinkTargetPathUtf8, trustedLinkPath);

    free(untrustedLinkTargetPathUtf8);
    LogInfo("target: '%s'", trustedLinkTargetPath);

    DWORD linkFlags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
    if (PathIsDirectory(trustedLinkTargetPath))
        linkFlags |= SYMBOLIC_LINK_FLAG_DIRECTORY;

    LogInfo("creating link: '%s' -> '%s'", trustedLinkPath, trustedLinkTargetPath);
    BOOL success = CreateSymbolicLink(trustedLinkPath, trustedLinkTargetPath, linkFlags);

    if (!success)
    {
        win_perror("CreateSymbolicLink");
        if (GetLastError() == ERROR_FILE_EXISTS)
            SendStatusAndExit(EEXIST, untrustedNameUtf8);
        else if (GetLastError() == ERROR_ACCESS_DENIED)
            SendStatusAndExit(EACCES, untrustedNameUtf8);
        else if (GetLastError() == ERROR_PRIVILEGE_NOT_HELD)
            SendStatusAndExit(EACCES, untrustedNameUtf8);
        else
            SendStatusAndExit(EIO, untrustedNameUtf8);
    }

    free(trustedLinkPath);
    free(trustedLinkTargetPath);
    LogVerbose("end");
}

void ProcessEntry(IN const WCHAR* incomingDir, IN const struct file_header *untrustedHeader)
{
    UINT32 nameSize;

    LogVerbose("start");
    if (untrustedHeader->namelen > FC_MAX_PATH - 1)
        SendStatusAndExit(ENAMETOOLONG, NULL);

    nameSize = untrustedHeader->namelen;	/* sanitized above */
    if (!ReadWithCrc(g_stdin, g_untrustedName, nameSize))
        SendStatusAndExit(LEGAL_EOF, NULL);	// hopefully remote has produced error message

    LogDebug("mode 0x%x", untrustedHeader->mode);

    g_untrustedName[nameSize] = 0;
    if (S_ISREG(untrustedHeader->mode))
        ProcessRegularFile(incomingDir, untrustedHeader, g_untrustedName);
    else if (S_ISLNK(untrustedHeader->mode))
        ProcessLink(incomingDir, untrustedHeader, g_untrustedName);
    else if (S_ISDIR(untrustedHeader->mode))
        ProcessDirectory(incomingDir, untrustedHeader, g_untrustedName);
    else
        SendStatusAndExit(EINVAL, g_untrustedName);
    LogVerbose("end");
}

int ReceiveFiles(IN const WCHAR* incomingDir)
{
    struct file_header untrustedHeader;
    LogDebug("incoming dir: %s", incomingDir);

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

        ProcessEntry(incomingDir, &untrustedHeader);
        g_totalFilesReceived++;

        if (g_filesLimit && g_totalFilesReceived > g_filesLimit)
            SendStatusAndExit(EDQUOT, g_untrustedName);
    }
    SendStatusAndCrc(errno, NULL);
    return errno;
}
