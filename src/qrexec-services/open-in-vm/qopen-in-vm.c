#include <Windows.h>
#include <stdio.h>
#include <strsafe.h>
#include <string.h>
#include <stdlib.h>

#include <utf8-conv.h>
#include <qubes-io.h>

#include "filecopy-error.h"
#include "dvm2.h"

void SendFile(IN const WCHAR *filePath)
{
    const WCHAR *base, *base1, *base2;
    char *baseUtf8;
    char basePadded[DVM_FILENAME_SIZE];
    HANDLE stdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE file = CreateFile(filePath, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if (file == INVALID_HANDLE_VALUE)
        FcReportError(GetLastError(), TRUE, L"open '%s'", filePath);

    base1 = wcsrchr(filePath, L'\\');
    base2 = wcsrchr(filePath, L'/');

    if (base2 > base1)
    {
        base = base2;
        base++;
    }
    else if (base1)
    {
        base = base1;
        base++;
    }
    else
        base = filePath;

    if (ERROR_SUCCESS != ConvertUTF16ToUTF8(base, &baseUtf8, NULL))
        FcReportError(GetLastError(), TRUE, L"Failed to convert filename '%s' to UTF8", base);

    if (strlen(baseUtf8) >= DVM_FILENAME_SIZE)
        baseUtf8 += strlen(baseUtf8) - DVM_FILENAME_SIZE + 1;

    ZeroMemory(basePadded, sizeof(basePadded));
    StringCbCopyA(basePadded, sizeof(basePadded), baseUtf8);

    free(baseUtf8);

    if (!QioWriteBuffer(stdOut, basePadded, DVM_FILENAME_SIZE))
        FcReportError(GetLastError(), TRUE, L"send filename to dispVM");

    if (!QioCopyUntilEof(stdOut, file))
        FcReportError(GetLastError(), TRUE, L"send file to dispVM");

    CloseHandle(file);
    fprintf(stderr, "File sent\n");
    CloseHandle(stdOut);
}

void ReceiveFile(IN const WCHAR *fileName)
{
    HANDLE tempFile;
    WCHAR tempDirPath[32768];
    WCHAR tempFilePath[32768];
    LARGE_INTEGER fileSize;
    HANDLE stdIn = GetStdHandle(STD_INPUT_HANDLE);

    // prepare temporary path
    if (!GetTempPath(RTL_NUMBER_OF(tempDirPath), tempDirPath))
    {
        FcReportError(GetLastError(), TRUE, L"Failed to get temp dir");
    }

    if (!GetTempFileName(tempDirPath, L"qvm", 0, tempFilePath))
    {
        FcReportError(GetLastError(), TRUE, L"Failed to get temp file");
    }

    // create temp file
    tempFile = CreateFile(tempFilePath, GENERIC_WRITE, 0, NULL, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (tempFile == INVALID_HANDLE_VALUE)
        FcReportError(GetLastError(), TRUE, L"Failed to open temp file");

    if (!QioCopyUntilEof(tempFile, stdIn))
        FcReportError(GetLastError(), TRUE, L"receiving file from dispVM");

    if (!GetFileSizeEx(tempFile, &fileSize))
        FcReportError(GetLastError(), TRUE, L"GetFileSizeEx");

    CloseHandle(tempFile);

    if (fileSize.QuadPart == 0)
    {
        DeleteFile(tempFilePath);
        return;
    }

    if (!MoveFileEx(tempFilePath, fileName, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
        FcReportError(GetLastError(), TRUE, L"rename");
}

int __cdecl wmain(int argc, WCHAR *argv[])
{
    if (argc != 2)
        FcReportError(ERROR_BAD_ARGUMENTS, TRUE, L"OpenInVM - no file given?");

    fprintf(stderr, "OpenInVM starting\n");

    SendFile(argv[1]);
    ReceiveFile(argv[1]);

    return 0;
}
