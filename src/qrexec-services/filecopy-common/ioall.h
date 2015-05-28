#pragma once
#include <windows.h>

BOOL FcWriteBuffer(IN HANDLE file, IN const void *buffer, IN DWORD bufferSize);
BOOL FcReadBuffer(IN HANDLE file, OUT void *buffer, IN DWORD bufferSize);
BOOL FcCopyUntilEof(IN HANDLE output, IN HANDLE input);

// Returns number of bytes read.
DWORD FcReadUntilEof(IN HANDLE file, OUT void *buffer, IN DWORD bufferSize);
