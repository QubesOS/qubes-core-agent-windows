#include <Windows.h>
#include <stdio.h>
#include <tchar.h>
#include <strsafe.h>
#include <string.h>
#include <malloc.h>
#include <stdlib.h>
#include <ioall.h>
#include <gui-fatal.h>
#include "dvm2.h"
#include "utf8-conv.h"

void send_file(PTCHAR fname)
{
	PTCHAR	base, base1, base2;
	PUCHAR	baseUTF8;
	UCHAR	basePadded[DVM_FILENAME_SIZE];
	HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
	HANDLE fd = CreateFile(fname, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (fd == INVALID_HANDLE_VALUE)
		gui_fatal(TEXT("open %s"), fname);

	base1 = _tcsrchr(fname, TEXT('\\'));
	base2 = _tcsrchr(fname, TEXT('/'));
	if (base2 > base1) {
		base = base2;
		base++;
	} else if (base1) {
		base = base1;
		base++;
	} else
		base = fname;
#ifdef UNICODE
	if (FAILED(ConvertUTF16ToUTF8(base, &baseUTF8, NULL)))
		gui_fatal(TEXT("Failed convert filename to UTF8"));
#else
	baseUTF8 = base;
#endif

	if (strlen(baseUTF8) >= DVM_FILENAME_SIZE)
		baseUTF8 += strlen(baseUTF8) - DVM_FILENAME_SIZE + 1;
	memset(basePadded, 0, sizeof(basePadded));
	StringCbCopyA(basePadded, sizeof(basePadded), baseUTF8);
#ifdef UNICODE
	free(baseUTF8);
#endif
	if (!write_all(hStdOut, basePadded, DVM_FILENAME_SIZE))
		gui_fatal(TEXT("send filename to dispVM"));
	if (!copy_fd_all(hStdOut, fd))
		gui_fatal(TEXT("send file to dispVM"));
	CloseHandle(fd);
	fprintf(stderr, "File sent\n");
	CloseHandle(hStdOut);
}

int copy_and_return_nonemptiness(HANDLE tmpfd)
{
	LARGE_INTEGER size;
	HANDLE hStdIn = GetStdHandle(STD_INPUT_HANDLE);

	if (!copy_fd_all(tmpfd, hStdIn))
		gui_fatal(TEXT("receiving file from dispVM"));
	if (!GetFileSizeEx(tmpfd, &size))
		gui_fatal(TEXT("GetFileSizeEx"));
	CloseHandle(tmpfd);

	return size.QuadPart > 0;
}

void actually_recv_file(PTCHAR fname, PTCHAR tempfile, HANDLE tmpfd)
{
	if (!copy_and_return_nonemptiness(tmpfd)) {
		DeleteFile(tempfile);
		return;
	}
	if (!MoveFileEx(tempfile, fname, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
		gui_fatal(TEXT("rename"));
}

void recv_file(PTCHAR fname)
{
	HANDLE tmpfd;
	int ret;
	TCHAR tempdir[32768];
	TCHAR tempfile[32768];

	if (!GetTempPath(RTL_NUMBER_OF(tempdir), tempdir)) {
		gui_fatal(TEXT("Failed to get temp dir"));
	}
	if (!GetTempFileName(tempdir, TEXT("qvm"), 0, tempfile)) {
		gui_fatal(TEXT("Failed to get temp file"));
	}
	tmpfd = CreateFile(tempfile, GENERIC_WRITE, 0, NULL, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

	if (tmpfd == INVALID_HANDLE_VALUE) {
		gui_fatal(TEXT("Failed to open temp file"));
	}
	actually_recv_file(fname, tempfile, tmpfd);
}

void talk_to_daemon(PTCHAR fname)
{
	send_file(fname);
	recv_file(fname);
}

int __cdecl _tmain(ULONG argc, PTCHAR argv[])
{
	if (argc!=2) 
		gui_fatal(TEXT("OpenInVM - no file given?"));
	fprintf(stderr, "OpenInVM starting\n");
	talk_to_daemon(argv[1]);
	return 0;
}	
