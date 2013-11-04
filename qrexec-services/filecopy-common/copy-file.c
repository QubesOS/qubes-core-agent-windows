#include <Windows.h>
#include "ioall.h"
#include "filecopy.h"
#include "crc32.h"
#include "log.h"

extern int notify_progress(int, int);

int copy_file(HANDLE outfd, HANDLE infd, long long size, unsigned long *crc32)
{
    char buf[4096];
    long long written = 0;
    DWORD ret;
    long long count;

    logf("copy_file: size %lu\n", size);
    while (written < size) {
        if (size - written > sizeof(buf))
            count = sizeof buf;
        else
            count = size - written;
        debugf("copy_file: reading %d...", count);
        if (!ReadFile(infd, buf, (DWORD)count, &ret, NULL))
        {
            perror("copy_file: ReadFile");
            return COPY_FILE_READ_ERROR;
        }
        debugf("done\n");
        if (ret == 0)
            return COPY_FILE_READ_EOF;
        /* accumulate crc32 if requested */
        if (crc32)
            *crc32 = Crc32_ComputeBuf(*crc32, buf, ret);
        if (!write_all(outfd, buf, ret))
            return COPY_FILE_WRITE_ERROR;
        if (notify_progress(ret, 0) != 0) // operation aborted
            return COPY_FILE_READ_EOF;
        written += ret;
    }
    return COPY_FILE_OK;
}

char * copy_file_status_to_str(int status)
{
    switch (status) {
        case COPY_FILE_OK: return "OK";
        case COPY_FILE_READ_EOF: return "Unexpected end of data while reading";
        case COPY_FILE_READ_ERROR: return "Error reading";
        case COPY_FILE_WRITE_ERROR: return "Error writing";
        default: return "????????";
    }
} 
