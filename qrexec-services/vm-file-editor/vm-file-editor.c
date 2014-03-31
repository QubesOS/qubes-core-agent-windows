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

int get_tempdir(PTCHAR *pBuf, size_t *pcchBuf)
{
    int		size, size_all;
    TCHAR	*pTmpBuf;
    TCHAR	*pDstBuf;
    UUID uuid;
    TCHAR subdir[64] = {0};

    if (UuidCreate(&uuid) != RPC_S_OK)
        return 0;
    // there is no UuidToString version that operates on TCHARs
    if (FAILED(StringCchPrintf(subdir, RTL_NUMBER_OF(subdir), TEXT("%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x\\"),
        uuid.Data1, uuid.Data2, uuid.Data3,
        uuid.Data4[0], uuid.Data4[1],
        uuid.Data4[2], uuid.Data4[3], uuid.Data4[4], uuid.Data4[5], uuid.Data4[6], uuid.Data4[7])))
        return 0;

    size = GetTempPath(0, NULL);
    if (!size)
        return 0;

    size_all = size + _tcslen(subdir);
    pTmpBuf = (TCHAR*) malloc(size_all * sizeof(TCHAR));
    if (!pTmpBuf)
        return 0;

    size = GetTempPath(size, pTmpBuf);
    if (!size) {
        free(pTmpBuf);
        return 0;
    }

    if (FAILED(StringCchCat(pTmpBuf, size_all, subdir))) {
        free(pTmpBuf);
        return 0;
    }

    if (!CreateDirectory(pTmpBuf, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        fprintf(stderr, "Failed to create tmp subdir: 0x%x\n", GetLastError());
        free(pTmpBuf);
        return 0;
    }

    // todo? check if directory already exists
    // This is extremely unlikely, GUIDs v4 are totally PRNG-based.

    *pBuf = pTmpBuf;
    *pcchBuf = size_all;
    return size_all * sizeof(TCHAR);
}

TCHAR *get_filename()
{
    char buf[DVM_FILENAME_SIZE+1];
    PTCHAR basename;
    PTCHAR retname;
    PTCHAR tmpname;
    size_t basename_len, retname_len, tmpname_len;
    int i;

    if (!read_all(hStdIn, buf, DVM_FILENAME_SIZE)) {
        fprintf(stderr, "Failed get filename: 0x%x\n", GetLastError());
        exit(1);
    }
    buf[DVM_FILENAME_SIZE] = 0;
    if (strchr(buf, '/')) {
        fprintf(stderr, "filename contains /");
        exit(1);
    }
    if (strchr(buf, '\\')) {
        fprintf(stderr, "filename contains \\");
        exit(1);
    }
    for (i=0; i < DVM_FILENAME_SIZE && buf[i]!=0; i++) {
        // replace some characters with _ (eg mimeopen have problems with some of them)
        if (strchr(" !?\"#$%^&*()[]<>;`~", buf[i]))
            buf[i]='_';
    }
    if (FAILED(ConvertUTF8ToUTF16(buf, &basename, &basename_len))) {
        fprintf(stderr, "Invalid filename\n");
        exit(1);
    }
    if (!get_tempdir(&tmpname, &tmpname_len)) {
        free(basename);
        fprintf(stderr, "Failed to get tmpdir\n");
        exit(1);
    }
    retname_len = tmpname_len + basename_len + 1;
    retname = malloc(sizeof(TCHAR) * retname_len);
    // TODO: better tmp filename set up - at least unique...
    StringCchPrintf(retname, retname_len, TEXT("%s%s"), tmpname, basename);
    free(tmpname);
    free(basename);
    return retname;
}

void copy_file(PTCHAR filename)
{
    HANDLE fd = CreateFile(filename, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);

 
    if (fd == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_EXISTS)
            fprintf(stderr, "File already exists, cleanup temp directory\n");
        else
            fprintf(stderr, "Failed to create file %s: 0x%x\n", filename, GetLastError());
        exit(1);
    }
    if (!copy_fd_all(fd, hStdIn)) {
        fprintf(stderr, "Failed read/write file: 0x%x\n", GetLastError());
        exit(1);
    }
    CloseHandle(fd);
}

void send_file_back(PTCHAR filename)
{
    HANDLE fd = CreateFile(filename, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fd == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Failed to open file %s: 0x%x\n", filename, GetLastError());
        exit(1);
    }
    if (hStdOut == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Failed to open STDOUT: 0x%x\n", GetLastError());
        goto cleanup;
    }

    if (!copy_fd_all(hStdOut, fd)) {
        fprintf(stderr, "Failed read/write file: 0x%x\n", GetLastError());
        goto cleanup;
    }

cleanup:
    CloseHandle(fd);
    CloseHandle(hStdOut);
}

int __cdecl _tmain(ULONG argc, PTCHAR argv[])
{
    WIN32_FILE_ATTRIBUTE_DATA stat_pre, stat_post, session_stat;
    PTCHAR	filename;
    HANDLE	child;
    TCHAR	cmdline[32768];
    int		dwExitCode;
    SHELLEXECUTEINFO sei;
    STARTUPINFO si;
    PROCESS_INFORMATION pi;

    hStdIn = GetStdHandle(STD_INPUT_HANDLE);
    hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hStdIn == INVALID_HANDLE_VALUE || hStdOut == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Failed to open STDIN/STDOUT: 0x%x\n", GetLastError());
        exit(1);
    }

    filename = get_filename();
    copy_file(filename);
    if (!GetFileAttributesEx(filename, GetFileExInfoStandard, &stat_pre)) {
        fprintf(stderr, "ERROR stat pre: 0x%x\n", GetLastError());
        exit(1);
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

    if (FAILED(ShellExecuteEx(&sei))) {
        fprintf(stderr, "Editor startup failed: 0x%x\n", GetLastError());
        exit(1);
    }

    if (sei.hProcess == NULL) {
        fprintf(stderr, "Don't know how to wait for editor finish, exiting\n");
        exit(0);
    }

    // Wait until child process exits.
    WaitForSingleObject(sei.hProcess, INFINITE );

    if (!GetExitCodeProcess(sei.hProcess, &dwExitCode)) {
        fprintf(stderr, "Cannot get editor exit code: 0x%x\n", GetLastError());
        exit(1);
    }

    // Close process handle. 
    CloseHandle(sei.hProcess);

    if (dwExitCode != 0) {
        fprintf(stderr, "Editor failed: %d\n", dwExitCode);
        exit(1);
    }

    if (!GetFileAttributesEx(filename, GetFileExInfoStandard, &stat_post)) {
        fprintf(stderr, "ERROR stat post: 0x%x\n", GetLastError());
        exit(1);
    }

    if (stat_pre.ftLastWriteTime.dwLowDateTime != stat_post.ftLastWriteTime.dwLowDateTime ||
        stat_pre.ftLastWriteTime.dwHighDateTime != stat_post.ftLastWriteTime.dwHighDateTime)
        send_file_back(filename);
    DeleteFile(filename);
    return 0;
}
