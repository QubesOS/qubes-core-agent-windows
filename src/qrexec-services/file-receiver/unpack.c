#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <Strsafe.h>
#include <Shlwapi.h>

#include <utf8-conv.h>
#include <qubes-io.h>
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

void SetSizeLimit(IN INT64 bytesLimit, IN INT64 filesLimit)
{
    g_bytesLimit = bytesLimit;
    g_filesLimit = filesLimit;
}

BOOL ReadWithCrc(IN HANDLE input, OUT void *buffer, IN DWORD bufferSize)
{
    BOOL ret;

    ret = QioReadBuffer(input, buffer, bufferSize);
    if (ret)
        g_crc32 = Crc32_ComputeBuf(g_crc32, buffer, bufferSize);

    return ret;
}

void SendStatusAndCrc(IN UINT32 statusCode, IN const char *lastFileName OPTIONAL)
{
    struct result_header header;
    struct result_header_ext headerExt;

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

    errorCode = ConvertUTF8ToUTF16(untrustedNameUtf8, &untrustedFileName, NULL);
    if (ERROR_SUCCESS != errorCode)
        SendStatusAndExit(EINVAL, NULL);

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

    errorCode = ConvertUTF8ToUTF16(untrustedNameUtf8, &untrustedDirectoryName, NULL);
    if (ERROR_SUCCESS != errorCode)
        SendStatusAndExit(EINVAL, NULL);

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
    ULONG errorCode;
    BOOL targetIsFile = FALSE; /* default to directory links */

    errorCode = ConvertUTF8ToUTF16(untrustedNameUtf8, &untrustedName, NULL);
    if (ERROR_SUCCESS != errorCode)
        SendStatusAndExit(EINVAL, NULL);

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

    errorCode = ConvertUTF8ToUTF16(untrustedLinkTargetPathUtf8, &untrustedLinkTargetPath, NULL);
    if (ERROR_SUCCESS != errorCode)
        SendStatusAndExit(EINVAL, untrustedNameUtf8);

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
