#include <Windows.h>

#include "filecopy.h"
#include "crc32.h"

#include <qubes-io.h>
#include <log.h>

FC_COPY_STATUS FcCopyFile(IN HANDLE output, IN HANDLE input, IN UINT64 size, OUT UINT32 *crc32 OPTIONAL, IN fNotifyProgressCallback progressCallback OPTIONAL)
{
    BYTE buffer[4096];
    UINT64 cbTransferred = 0;
    DWORD cbRead;
    DWORD cbToRead;

    while (cbTransferred < size)
    {
        if (size - cbTransferred > sizeof(buffer))
            cbToRead = sizeof(buffer);
        else
            cbToRead = (DWORD)(size - cbTransferred); // safe cast: difference is always <= sizeof(buffer)

        if (!ReadFile(input, buffer, cbToRead, &cbRead, NULL))
        {
            perror("ReadFile");
            return COPY_FILE_READ_ERROR;
        }

        if (cbRead == 0)
            return COPY_FILE_READ_EOF;

        /* accumulate crc32 if requested */
        if (crc32)
            *crc32 = Crc32_ComputeBuf(*crc32, buffer, cbRead);

        if (!QioWriteBuffer(output, buffer, cbRead))
            return COPY_FILE_WRITE_ERROR;

        if (progressCallback)
            progressCallback(cbRead, PROGRESS_TYPE_NORMAL);

        cbTransferred += cbRead;
    }

    return COPY_FILE_OK;
}

char *FcStatusToString(IN FC_COPY_STATUS status)
{
    switch (status)
    {
    case COPY_FILE_OK: return "OK";
    case COPY_FILE_READ_EOF: return "Unexpected end of data while reading";
    case COPY_FILE_READ_ERROR: return "Error reading";
    case COPY_FILE_WRITE_ERROR: return "Error writing";
    default: 
        LogWarning("Unknown status: %d", status);
        return "Unknown error";
    }
}
