#include <windows.h>
#include <tchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <Strsafe.h>
#include <Shlwapi.h>
#include <Shellapi.h>
#include <ioall.h>
#include <linux.h>
#include <gui-fatal.h>
#include "gui-progress.h"
#include "filecopy.h"
#include "crc32.h"
#include "utf8_conv.h"

HANDLE STDIN = INVALID_HANDLE_VALUE;
HANDLE STDOUT = INVALID_HANDLE_VALUE;
HANDLE STDERR = INVALID_HANDLE_VALUE;

INT64 total_size = 0;
BOOL cancel_operation = FALSE;

#ifdef DBG
#define internal_fatal gui_fatal
#else
static __inline void internal_fatal(const PTCHAR fmt, ...) {
	gui_fatal(TEXT("Internal error"));
}
#endif


unsigned long crc32_sum;
int write_all_with_crc(HANDLE hOutput, void *pBuf, int sSize)
{
	crc32_sum = Crc32_ComputeBuf(crc32_sum, pBuf, sSize);
	return write_all(hOutput, pBuf, sSize);
}

void notify_progress(int size, int flag)
{
	static long long total_written = 0;
	static long long prev_total = 0;
	total_written += size;
	if (total_written > prev_total + PROGRESS_NOTIFY_DELTA
	    || (flag != PROGRESS_FLAG_NORMAL)) {
		do_notify_progress(total_written, flag);
		prev_total = total_written;
	}
}

void wait_for_result()
{
	struct result_header hdr;

	if (!read_all(STDIN, &hdr, sizeof(hdr))) {
		exit(1);	// hopefully remote has produced error message
	}
	if (hdr.error_code != 0) {
		switch (hdr.error_code) {
			case EEXIST:
				gui_fatal(TEXT("File copy: not overwriting existing file. Clean incoming dir, and retry copy"));
				break;
			case EINVAL:
				gui_fatal(TEXT("File copy: Corrupted data from packer"));
				break;
			default:
				gui_fatal(TEXT("File copy: %s"),
						strerror(hdr.error_code));
		}
	}
	if (hdr.crc32 != crc32_sum) {
		gui_fatal(TEXT("File transfer failed: checksum mismatch"));
	}
}

#define UNIX_EPOCH_OFFSET 11644478640LL

void convertWindowTimeToUnix(PFILETIME srctime, unsigned int *puDstTime, unsigned int *puDstTimeNsec) {
	ULARGE_INTEGER tmp;

	tmp.LowPart = srctime->dwLowDateTime;
	tmp.HighPart = srctime->dwHighDateTime;

	*puDstTimeNsec = (unsigned int)((tmp.QuadPart % 10000000LL) * 100LL);
	*puDstTime = (unsigned int)((tmp.QuadPart / 10000000LL) - UNIX_EPOCH_OFFSET);
}


void write_headers(struct file_header *hdr, PTCHAR pszFilename)
{
#ifdef UNICODE
	PUCHAR pszFilenameUtf8 = NULL;
	size_t cbFilenameUtf8;

	if (FAILED(ConvertUTF16ToUTF8(pszFilename, &pszFilenameUtf8, &cbFilenameUtf8)))
		gui_fatal(TEXT("Cannot convert path '%s' to UTF-8"), pszFilename);
	hdr->namelen = cbFilenameUtf8;
	if (!write_all_with_crc(STDOUT, hdr, sizeof(*hdr))
	    || !write_all_with_crc(STDOUT, pszFilenameUtf8, hdr->namelen)) {
		wait_for_result();
		exit(1);
	}
	free(pszFilenameUtf8);
#else
	hdr->namelen = _tcslen(pszFilename);
	if (!write_all_with_crc(STDOUT, hdr, sizeof(*hdr))
	    || !write_all_with_crc(STDOUT, pszFilename, hdr->namelen)) {
		wait_for_result();
		exit(1);
	}
#endif
}

int single_file_processor(PTCHAR pszFilename, DWORD dwAttrs)
{
	struct file_header hdr;
	HANDLE hInput;
	FILETIME atime, mtime;

	if (dwAttrs & FILE_ATTRIBUTE_DIRECTORY) 
		hdr.mode = 0755 | 0040000;
	else
		hdr.mode = 0644 | 0100000;

	// FILE_FLAG_BACKUP_SEMANTICS required to access directories
	hInput = CreateFile(pszFilename, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (hInput == INVALID_HANDLE_VALUE)
		gui_fatal(TEXT("Cannot open file %s"), pszFilename);

	/* FIXME: times are already retrieved by FindFirst/NextFile */
	if (!GetFileTime(hInput, NULL, &atime, &mtime)) {
		CloseHandle(hInput);
		gui_fatal(TEXT("Cannot get file %s time"), pszFilename);
	}
	convertWindowTimeToUnix(&atime, &hdr.atime, &hdr.atime_nsec);
	convertWindowTimeToUnix(&mtime, &hdr.mtime, &hdr.mtime_nsec);

	if ((dwAttrs & FILE_ATTRIBUTE_DIRECTORY) == 0) { /* FIXME: symlink */
		int ret;
		LARGE_INTEGER size;

		if (!GetFileSizeEx(hInput, &size)) {
			CloseHandle(hInput);
			gui_fatal(TEXT("Cannot get file %s size"), pszFilename);
		}
		hdr.filelen = size.QuadPart;
		write_headers(&hdr, pszFilename);
		ret = copy_file(STDOUT, hInput, hdr.filelen, &crc32_sum);
		// if COPY_FILE_WRITE_ERROR, hopefully remote will produce a message
		if (ret != COPY_FILE_OK) {
			if (ret != COPY_FILE_WRITE_ERROR) {
				CloseHandle(hInput);
				gui_fatal(TEXT("Copying file %s: %hs"), pszFilename,
					  copy_file_status_to_str(ret));
			} else {
				wait_for_result();
				exit(1);
			}
		}
	}
	if (dwAttrs & FILE_ATTRIBUTE_DIRECTORY) {
		hdr.filelen = 0;
		write_headers(&hdr, pszFilename);
	}
	/* TODO */
#if 0
	if (S_ISLNK(mode)) {
		char name[st->st_size + 1];
		if (readlink(pwszFilename, name, sizeof(name)) != st->st_size)
			gui_fatal(L"readlink %s", pwszFilename);
		hdr.filelen = st->st_size + 1;
		write_headers(&hdr, pwszFilename);
		if (!write_all_with_crc(1, name, st->st_size + 1))
			exit(1);
	}
#endif
	CloseHandle(hInput);
	return 0;
}

INT64 getFileSizeByPath(PTCHAR pszFilename)
{
	HANDLE hFile;
	LARGE_INTEGER dwFileSize;

	hFile = CreateFile(pszFilename, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
		gui_fatal(TEXT("Cannot open file %s"), pszFilename);

	if (!GetFileSizeEx(hFile, &dwFileSize)) {
		CloseHandle(hFile);
		gui_fatal(TEXT("Cannot get file size of %s"), pszFilename);
	}

	CloseHandle(hFile);
	return dwFileSize.QuadPart;
}

INT64 do_fs_walk(PTCHAR pszPath, BOOL bCalcSize)
{
	PTCHAR pszCurrentPath;
	size_t cchCurrentPath, cchSearchPath;
	DWORD dwAttrs;
	WIN32_FIND_DATA ent;
	PTCHAR pszSearchPath;
	HANDLE hSearch;
	INT64 size = 0;

	if ((dwAttrs = GetFileAttributes(pszPath)) == INVALID_FILE_ATTRIBUTES)
		gui_fatal(TEXT("Cannot get attributes of %s"), pszPath);
	if (!bCalcSize)
		single_file_processor(pszPath, dwAttrs);
	if (!(dwAttrs & FILE_ATTRIBUTE_DIRECTORY)) {
		if (bCalcSize)
			return getFileSizeByPath(pszPath);
		else
			return 0;
	}
	cchSearchPath = _tcslen(pszPath)+3;
	pszSearchPath = malloc(sizeof(TCHAR)*cchSearchPath);
	if (!pszSearchPath)
		internal_fatal(TEXT("malloc at %d"), __LINE__);
	if (FAILED(StringCchPrintf(pszSearchPath, cchSearchPath, TEXT("%s\\*"), pszPath)))
		internal_fatal(TEXT("StringCchPrintf at %d"), __LINE__);
	hSearch = FindFirstFile(pszSearchPath, &ent);
	if (hSearch == INVALID_HANDLE_VALUE) {
		LONG ret = GetLastError();
		if (ret == ERROR_FILE_NOT_FOUND)
			// empty directory
			return 0;
		gui_fatal(TEXT("Cannot list directory %s"), pszPath);
	}
	do {
		if (!_tcscmp(ent.cFileName, TEXT(".")) || !_tcscmp(ent.cFileName, TEXT("..")))
			continue;
		cchCurrentPath = _tcslen(pszPath)+_tcslen(ent.cFileName)+2;
		pszCurrentPath = malloc(sizeof(TCHAR)*cchCurrentPath);
		if (!pszCurrentPath)
			internal_fatal(TEXT("malloc at %d"), __LINE__);
		// use forward slash here to send it also to the other end
		if (FAILED(StringCchPrintf(pszCurrentPath, cchCurrentPath, TEXT("%s/%s"), pszPath, ent.cFileName)))
			internal_fatal(TEXT("StringCchPrintf at %d"), __LINE__);
		size += do_fs_walk(pszCurrentPath, bCalcSize);
		free(pszCurrentPath);
		if (cancel_operation)
			break;
	} while (FindNextFile(hSearch, &ent));
	FindClose(hSearch);
	// directory metadata is resent; this makes the code simple,
	// and the atime/mtime is set correctly at the second time
	if (!bCalcSize)
		single_file_processor(pszPath, dwAttrs);
	return size;
}

void notify_end_and_wait_for_result()
{
	struct file_header end_hdr;

	/* nofity end of transfer */
	memset(&end_hdr, 0, sizeof(end_hdr));
	end_hdr.namelen = 0;
	end_hdr.filelen = 0;
	write_all_with_crc(STDOUT, &end_hdr, sizeof(end_hdr));

	wait_for_result();
}

PTCHAR GetAbsolutePath(PTCHAR pszCwd, PTCHAR pszPath)
{
	PTCHAR pszAbsolutePath;
	size_t cchAbsolutePath;
	if (!PathIsRelative(pszPath))
		return _tcsdup(pszPath);
	cchAbsolutePath = _tcslen(pszCwd)+_tcslen(pszPath)+2;
	pszAbsolutePath = malloc(sizeof(TCHAR)*cchAbsolutePath);
	if (!pszAbsolutePath) {
		return NULL;
	}
	if (FAILED(StringCchPrintf(pszAbsolutePath, cchAbsolutePath, TEXT("%s\\%s"), pszCwd, pszPath))) {
		free(pszAbsolutePath);
		return NULL;
	}
	return pszAbsolutePath;
}

int __cdecl _tmain(int argc, PTCHAR argv[])
{
	int i;
	PTCHAR pszArgumentDirectory, pszArgumentBasename;
	TCHAR szCwd[MAX_PATH_LENGTH];

	STDERR = GetStdHandle(STD_ERROR_HANDLE);
	if (STDERR == NULL || STDERR == INVALID_HANDLE_VALUE) {
		internal_fatal(TEXT("Failed to get STDERR handle"));
		exit(1);
	}
	STDIN = GetStdHandle(STD_INPUT_HANDLE);
	if (STDIN == NULL || STDIN == INVALID_HANDLE_VALUE) {
		internal_fatal(TEXT("Failed to get STDIN handle"));
		exit(1);
	}
	STDOUT = GetStdHandle(STD_OUTPUT_HANDLE);
	if (STDOUT == NULL || STDOUT == INVALID_HANDLE_VALUE) {
		internal_fatal(TEXT("Failed to get STDOUT handle"));
		exit(1);
	}
	notify_progress(0, PROGRESS_FLAG_INIT);
	crc32_sum = 0;
	if (!GetCurrentDirectory(RTL_NUMBER_OF(szCwd), szCwd)) {
		internal_fatal(TEXT("Failed to get current directory"));
		exit(1);
	}
	// calculate total size for progressbar purpose
	total_size = 0;
	for (i = 1; i < argc; i++) {
		if (cancel_operation)
			break;
		// do not change dir, as don't care about form of the path here
		total_size += do_fs_walk(argv[i], TRUE);
	}
	for (i = 1; i < argc; i++) {
		if (cancel_operation)
			break;
		pszArgumentDirectory = GetAbsolutePath(szCwd, argv[i]);
		if (!pszArgumentDirectory) {
			gui_fatal(TEXT("GetAbsolutePath %s"), argv[i]);
		}
		// absolute path has at least one character
		if (PathGetCharType(pszArgumentDirectory[_tcslen(pszArgumentDirectory)-1]) & GCT_SEPARATOR)
			pszArgumentDirectory[_tcslen(pszArgumentDirectory)-1] = L'\0';
		pszArgumentBasename = _tcsdup(pszArgumentDirectory);
		PathStripPath(pszArgumentBasename);
		PathRemoveFileSpec(pszArgumentDirectory);

		if (!SetCurrentDirectory(pszArgumentDirectory))
			gui_fatal(TEXT("chdir to %s"), pszArgumentDirectory);
		do_fs_walk(pszArgumentBasename, FALSE);
		free(pszArgumentDirectory);
		free(pszArgumentBasename);
	}
	notify_end_and_wait_for_result();
	notify_progress(0, PROGRESS_FLAG_DONE);
	return 0;
}
