#include <windows.h>
#include <tchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <Shlwapi.h>
#include <Strsafe.h>
#include <Shellapi.h>
#include <ioall.h>
#include <gui-fatal.h>
#include "linux.h"
#include "filecopy.h"
#include "crc32.h"
#include "utf8_conv.h"

char untrusted_namebuf[MAX_PATH_LENGTH];
long long bytes_limit = 0;
long long files_limit = 0;
long long total_bytes = 0;
long long total_files = 0;


extern	HANDLE	STDIN;
extern	HANDLE	STDOUT;
extern	WCHAR	g_wcMappedDriveLetter;

#ifdef DBG
#define internal_fatal gui_fatal
#else
static __inline void internal_fatal(const PWCHAR fmt, ...) {
	gui_fatal(L"Internal error");
}
#endif

void notify_progress(int p1, int p2)
{
}

void set_size_limit(long long new_bytes_limit, long long new_files_limit)
{
	bytes_limit = new_bytes_limit;
	files_limit = new_files_limit;
}

unsigned long crc32_sum = 0;
int read_all_with_crc(HANDLE fd, void *buf, int size) {
	int ret;
	ret = read_all(fd, buf, size);
	if (ret)
		crc32_sum = Crc32_ComputeBuf(crc32_sum, buf, size);
	return ret;
}

void send_status_and_crc(int status) {
	struct result_header hdr;

	hdr.error_code = status;
	hdr.crc32 = crc32_sum;
	write_all(STDOUT, &hdr, sizeof(hdr));
}

void do_exit(int code)
{
	if (code == LEGAL_EOF)
		code = 0;
	send_status_and_crc(code);
	CloseHandle(STDOUT);
	exit(code == 0);
}

/*
void fix_times_and_perms(struct file_header *untrusted_hdr,
			 char *untrusted_name)
{
	struct timeval times[2] =
	    { {untrusted_hdr->atime, untrusted_hdr->atime_nsec / 1000},
	    {untrusted_hdr->mtime,
	     untrusted_hdr->mtime_nsec / 1000}
	};
	if (chmod(untrusted_name, untrusted_hdr->mode & 07777))
		do_exit(errno);
	if (utimes(untrusted_name, times))
		do_exit(errno);
}
*/


void process_one_file_reg(struct file_header *untrusted_hdr,
			  char *untrusted_name)
{
	int	ret;
	ULONG	uResult;
	HANDLE	fdout;
	PWCHAR	pszUtf16UntrustedName = NULL;
	WCHAR	wszTrustedFilePath[MAX_PATH + 1];
	HRESULT	hResult;

	uResult = ConvertUTF8ToUTF16(untrusted_name, &pszUtf16UntrustedName, NULL);
	if (ERROR_SUCCESS != uResult)
		do_exit(EINVAL);

	hResult = StringCchPrintfW(
			wszTrustedFilePath, 
			RTL_NUMBER_OF(wszTrustedFilePath), 
			L"%c:\\%s", 
			g_wcMappedDriveLetter,
			pszUtf16UntrustedName);
	free(pszUtf16UntrustedName);

	if (FAILED(hResult)) 
		do_exit(EINVAL);


	fdout = CreateFileW(wszTrustedFilePath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, 0, NULL);	/* safe because of chroot */
	if (INVALID_HANDLE_VALUE == fdout) {
		// maybe some more complete error code translation needed here, but
		// anyway qfile-agent will handle only those listed below
		if (GetLastError() == ERROR_FILE_EXISTS)
			do_exit(EEXIST);
		else if (GetLastError() == ERROR_ACCESS_DENIED)
			do_exit(EACCES);
		else
			do_exit(EIO);
	}

	total_bytes += untrusted_hdr->filelen;
	if (bytes_limit && total_bytes > bytes_limit)
		do_exit(EDQUOT);
	ret = copy_file(fdout, STDIN, untrusted_hdr->filelen, &crc32_sum);
	if (ret != COPY_FILE_OK) {
		do_exit(EIO);
	}

	CloseHandle(fdout);
//	fix_times_and_perms(untrusted_hdr, untrusted_name);
}


void process_one_file_dir(struct file_header *untrusted_hdr,
			  char *untrusted_name)
{
// fix perms only when the directory is sent for the second time
// it allows to transfer r.x directory contents, as we create it rwx initially
	ULONG	uResult;
	PWCHAR	pszUtf16UntrustedName = NULL;
	WCHAR	wszTrustedDirectoryPath[MAX_PATH + 1];
	HRESULT	hResult;

	uResult = ConvertUTF8ToUTF16(untrusted_name, &pszUtf16UntrustedName, NULL);
	if (ERROR_SUCCESS != uResult)
		do_exit(EINVAL);

	hResult = StringCchPrintfW(
			wszTrustedDirectoryPath, 
			RTL_NUMBER_OF(wszTrustedDirectoryPath), 
			L"%c:\\%s", 
			g_wcMappedDriveLetter,
			pszUtf16UntrustedName);
	free(pszUtf16UntrustedName);

	if (FAILED(hResult)) 
		do_exit(EINVAL);

	if (!CreateDirectory(wszTrustedDirectoryPath, NULL)) {	/* safe because of chroot */
		uResult = GetLastError();
		if (ERROR_ALREADY_EXISTS != uResult)
			do_exit(ENOTDIR);
	}

//	fix_times_and_perms(untrusted_hdr, untrusted_name);
}

void process_one_file_link(struct file_header *untrusted_hdr,
			   char *untrusted_name)
{
/*
	char untrusted_content[MAX_PATH_LENGTH];
	unsigned int filelen;
	if (untrusted_hdr->filelen > MAX_PATH_LENGTH - 1)
		do_exit(ENAMETOOLONG);
	filelen = untrusted_hdr->filelen;	// sanitized above
	if (!read_all_with_crc(0, untrusted_content, filelen))
		do_exit(LEGAL_EOF);	// hopefully remote has produced error message
	untrusted_content[filelen] = 0;
	if (symlink(untrusted_content, untrusted_name))	// safe because of chroot
		do_exit(errno);
*/
}

void process_one_file(struct file_header *untrusted_hdr)
{
	unsigned int namelen;
	if (untrusted_hdr->namelen > MAX_PATH_LENGTH - 1)
		do_exit(ENAMETOOLONG);
	namelen = untrusted_hdr->namelen;	/* sanitized above */
	if (!read_all_with_crc(STDIN, untrusted_namebuf, namelen))
		do_exit(LEGAL_EOF);	// hopefully remote has produced error message
	untrusted_namebuf[namelen] = 0;
	if (S_ISREG(untrusted_hdr->mode))
		process_one_file_reg(untrusted_hdr, untrusted_namebuf);
	else if (S_ISLNK(untrusted_hdr->mode))
		process_one_file_link(untrusted_hdr, untrusted_namebuf);
	else if (S_ISDIR(untrusted_hdr->mode))
		process_one_file_dir(untrusted_hdr, untrusted_namebuf);
	else
		do_exit(EINVAL);
}

int do_unpack()
{
	struct file_header untrusted_hdr;

	/* initialize checksum */
	crc32_sum = 0;
	while (read_all_with_crc(STDIN, &untrusted_hdr, sizeof untrusted_hdr)) {
		/* check for end of transfer marker */
		if (untrusted_hdr.namelen == 0) {
			errno = 0;
			break;
		}
		process_one_file(&untrusted_hdr);
		total_files++;
		if (files_limit && total_files > files_limit)
			do_exit(EDQUOT);
	}
	send_status_and_crc(errno);
	return errno;
}
