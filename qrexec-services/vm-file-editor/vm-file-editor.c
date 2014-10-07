#include <Windows.h>
#include <tchar.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <Strsafe.h>
#include <Shellapi.h>
#include <rpc.h>
#include <ioall.h>
#include "dvm2.h"
#include "utf8-conv.h"
#include "ioall.h"

HANDLE hStdIn = INVALID_HANDLE_VALUE;
HANDLE hStdOut = INVALID_HANDLE_VALUE;

BOOL get_tempdir(PTCHAR *pBuf, size_t *pcchBuf)
{
    BOOL retval = FALSE;
    int size = 0, size_all = 0;
    TCHAR *pTmpBuf = NULL;
    TCHAR *pDstBuf = NULL;
    UUID uuid;
    TCHAR subdir[64] = { 0 };

    if (UuidCreate(&uuid) != RPC_S_OK)
        goto cleanup;

    // there is no UuidToString version that operates on TCHARs
    if (FAILED(StringCchPrintf(subdir, RTL_NUMBER_OF(subdir), TEXT("%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x\\"),
        uuid.Data1, uuid.Data2, uuid.Data3,
        uuid.Data4[0], uuid.Data4[1],
        uuid.Data4[2], uuid.Data4[3], uuid.Data4[4], uuid.Data4[5], uuid.Data4[6], uuid.Data4[7])))
    {
        goto cleanup;
    }

    size = GetTempPath(0, NULL);
    if (!size)
        goto cleanup;

    size_all = size + _tcslen(subdir);
    pTmpBuf = (TCHAR*) malloc(size_all * sizeof(TCHAR));
    if (!pTmpBuf)
        goto cleanup;

    size = GetTempPath(size, pTmpBuf);
    if (!size)
        goto cleanup;

    if (FAILED(StringCchCat(pTmpBuf, size_all, subdir)))
        goto cleanup;

    if (!CreateDirectory(pTmpBuf, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
    {
        fprintf(stderr, "Failed to create tmp subdir: 0x%x\n", GetLastError());
        goto cleanup;
    }

    // todo? check if directory already exists
    // This is extremely unlikely, GUIDs v4 are totally PRNG-based.
    retval = TRUE;

cleanup:
    if (!retval)
        free(pTmpBuf);

    *pBuf = pTmpBuf;
    *pcchBuf = size_all;
    return retval;
}

TCHAR *get_filename(PTCHAR *tmpname)
{
    char buf[DVM_FILENAME_SIZE + 1];
    PTCHAR basename;
    PTCHAR retname;
    size_t basename_len, retname_len, tmpname_len;
    int i;

    if (!read_all(hStdIn, buf, DVM_FILENAME_SIZE))
    {
        fprintf(stderr, "Failed get filename: 0x%x\n", GetLastError());
        return NULL;
    }

    buf[DVM_FILENAME_SIZE] = 0;
    if (strchr(buf, '/'))
    {
        fprintf(stderr, "filename contains /");
        return NULL;
    }

    if (strchr(buf, '\\'))
    {
        fprintf(stderr, "filename contains \\");
        return NULL;
    }

    for (i = 0; i < DVM_FILENAME_SIZE && buf[i] != 0; i++)
    {
        // replace some characters with _ (eg mimeopen have problems with some of them)
        if (strchr(" !?\"#$%^&*()[]<>;`~", buf[i]))
            buf[i] = '_';
    }

    if (FAILED(ConvertUTF8ToUTF16(buf, &basename, &basename_len)))
    {
        fprintf(stderr, "Invalid filename\n");
        return NULL;
    }

    if (!get_tempdir(tmpname, &tmpname_len))
    {
        free(basename);
        fprintf(stderr, "Failed to get tmpdir\n");
        return NULL;
    }

    retname_len = tmpname_len + basename_len + 1;
    retname = malloc(sizeof(TCHAR) * retname_len);

    StringCchPrintf(retname, retname_len, TEXT("%s%s"), *tmpname, basename);
    free(basename);
    return retname;
}

BOOL copy_file(PTCHAR filename)
{
    BOOL retval = FALSE;
    HANDLE fd = CreateFile(filename, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);

    if (fd == INVALID_HANDLE_VALUE)
    {
        if (GetLastError() == ERROR_FILE_EXISTS)
            fprintf(stderr, "File already exists, cleanup temp directory\n");
        else
            fprintf(stderr, "Failed to create file %s: 0x%x\n", filename, GetLastError());
        goto cleanup;
    }

    if (!copy_fd_all(fd, hStdIn))
    {
        fprintf(stderr, "Failed to read/write file: 0x%x\n", GetLastError());
        goto cleanup;
    }

    retval = TRUE;

cleanup:
    CloseHandle(fd);
    return retval;
}

BOOL send_file_back(PTCHAR filename)
{
    BOOL retval = FALSE;
    HANDLE fd = CreateFile(filename, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if (fd == INVALID_HANDLE_VALUE)
    {
        fprintf(stderr, "Failed to open file %s: 0x%x\n", filename, GetLastError());
        goto cleanup;
    }

    if (hStdOut == INVALID_HANDLE_VALUE)
    {
        fprintf(stderr, "Failed to open STDOUT: 0x%x\n", GetLastError());
        goto cleanup;
    }

    if (!copy_fd_all(hStdOut, fd))
    {
        fprintf(stderr, "Failed read/write file: 0x%x\n", GetLastError());
        goto cleanup;
    }

    retval = TRUE;

cleanup:
    CloseHandle(fd);
    CloseHandle(hStdOut);
    return retval;
}

int __cdecl _tmain(ULONG argc, PTCHAR argv[])
{
    WIN32_FILE_ATTRIBUTE_DATA stat_pre, stat_post, session_stat;
    PTCHAR filename;
    HANDLE child;
    TCHAR cmdline[32768];
    int	exitCode = 1, childExitCode;
    SHELLEXECUTEINFO sei;
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    PTCHAR tempDir = NULL;

    hStdIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hStdIn == INVALID_HANDLE_VALUE)
    {
        exitCode = GetLastError();
        fprintf(stderr, "Failed to open STDIN: 0x%x\n", exitCode);
        goto cleanup;
    }

    hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hStdOut == INVALID_HANDLE_VALUE)
    {
        exitCode = GetLastError();
        fprintf(stderr, "Failed to open STDOUT: 0x%x\n", exitCode);
        goto cleanup;
    }

    filename = get_filename(&tempDir);
    if (!copy_file(filename))
        goto cleanup;

    if (!GetFileAttributesEx(filename, GetFileExInfoStandard, &stat_pre))
    {
        exitCode = GetLastError();
        fprintf(stderr, "ERROR stat pre: 0x%x\n", exitCode);
        goto cleanup;
    }

    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
#ifdef UNICODE
    sei.fMask |= SEE_MASK_UNICODE;
#endif
    sei.hwnd = NULL;
    sei.lpVerb = TEXT("open");
    sei.lpFile = filename;
    sei.lpParameters = NULL;
    sei.lpDirectory = NULL;
    sei.nShow = SW_SHOW;
    sei.hProcess = NULL;

    if (FAILED(ShellExecuteEx(&sei)))
    {
        exitCode = GetLastError();
        fprintf(stderr, "Editor startup failed: 0x%x\n", exitCode);
        goto cleanup;
    }

    if (sei.hProcess == NULL)
    {
        fprintf(stderr, "Don't know how to wait for editor finish, exiting\n");
        exitCode = ERROR_SUCCESS;
        goto cleanup;
    }

    // Wait until child process exits.
    WaitForSingleObject(sei.hProcess, INFINITE);

    if (!GetExitCodeProcess(sei.hProcess, &childExitCode))
    {
        exitCode = GetLastError();
        fprintf(stderr, "Cannot get editor exit code: 0x%x\n", exitCode);
        goto cleanup;
    }

    // Close child process handle.
    CloseHandle(sei.hProcess);

    if (childExitCode != 0)
    {
        fprintf(stderr, "Editor failed: %d\n", childExitCode);
        exitCode = childExitCode;
        goto cleanup;
    }

    if (!GetFileAttributesEx(filename, GetFileExInfoStandard, &stat_post))
    {
        exitCode = GetLastError();
        fprintf(stderr, "ERROR stat post: 0x%x\n", exitCode);
        goto cleanup;
    }

    if (stat_pre.ftLastWriteTime.dwLowDateTime != stat_post.ftLastWriteTime.dwLowDateTime ||
        stat_pre.ftLastWriteTime.dwHighDateTime != stat_post.ftLastWriteTime.dwHighDateTime)
        send_file_back(filename);

    exitCode = ERROR_SUCCESS;

cleanup:
    DeleteFile(filename);
    RemoveDirectory(tempDir);
    return exitCode;
}
