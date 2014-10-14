#pragma once

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

typedef enum _FC_COPY_STATUS
{
    COPY_FILE_OK,
    COPY_FILE_READ_EOF,
    COPY_FILE_READ_ERROR,
    COPY_FILE_WRITE_ERROR
} FC_COPY_STATUS;

typedef enum _FC_PROGRESS_TYPE
{
    PROGRESS_TYPE_NORMAL,
    PROGRESS_TYPE_INIT,
    PROGRESS_TYPE_DONE,
    PROGRESS_TYPE_ERROR
} FC_PROGRESS_TYPE;

typedef void(*fNotifyProgressCallback)(DWORD size, FC_PROGRESS_TYPE progressType);

FC_COPY_STATUS FcCopyFile(IN HANDLE output, IN HANDLE input, IN UINT64 size, OUT UINT32 *crc32 OPTIONAL, IN fNotifyProgressCallback progressCallback OPTIONAL);
char *FcStatusToString(IN FC_COPY_STATUS status);
