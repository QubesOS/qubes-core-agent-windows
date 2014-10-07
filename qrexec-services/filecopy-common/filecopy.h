#define FILECOPY_SPOOL "/home/user/.filecopyspool"
#define FILECOPY_VMNAME_SIZE 32
#define PROGRESS_NOTIFY_DELTA (1*1000*1000)
#define MAX_PATH_LENGTH 16384

#define LEGAL_EOF 31415926

#include <windows.h>

struct file_header
{
    UINT32 namelen;
    UINT32 mode;
    UINT64 filelen;
    UINT32 atime;
    UINT32 atime_nsec;
    UINT32 mtime;
    UINT32 mtime_nsec;
    /*
    char filename[0];
    char data[0];
    */
};

#pragma pack(1)
struct result_header
{
    UINT32 error_code;
    UINT32 _pad;
    UINT64 crc32;
};

/* optional info about last processed file */
#pragma pack(1)
struct result_header_ext
{
    UINT32 last_namelen;
    char last_name[0];
};

enum
{
    COPY_FILE_OK,
    COPY_FILE_READ_EOF,
    COPY_FILE_READ_ERROR,
    COPY_FILE_WRITE_ERROR
};

int copy_file(HANDLE outfd, HANDLE infd, long long size, unsigned long *crc32);
char *copy_file_status_to_str(int status);
void set_size_limit(long long new_bytes_limit, long long new_files_limit);
