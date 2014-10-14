/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (C) 2010  Rafal Wojtczuk  <rafal@invisiblethingslab.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include <windows.h>
#include <stdio.h>

#include "log.h"

BOOL FcWriteBuffer(IN HANDLE file, IN const void *buffer, IN DWORD bufferSize)
{
    DWORD cbWrittenTotal = 0;
    DWORD cbWritten;

    while (cbWrittenTotal < bufferSize)
    {
        if (!WriteFile(file, (BYTE *) buffer + cbWrittenTotal, bufferSize - cbWrittenTotal, &cbWritten, NULL))
        {
            perror("WriteFile");
            return FALSE;
        }
        cbWrittenTotal += cbWritten;
    }
    return TRUE;
}

BOOL FcReadBuffer(IN HANDLE file, OUT void *buffer, IN DWORD bufferSize)
{
    DWORD cbReadTotal = 0;
    DWORD cbRead;

    while (cbReadTotal < bufferSize)
    {
        if (!ReadFile(file, (BYTE *) buffer + cbReadTotal, bufferSize - cbReadTotal, &cbRead, NULL))
        {
            perror("ReadFile");
            return FALSE;
        }

        if (cbRead == 0)
        {
            fprintf(stderr, "EOF\n");
            return FALSE;
        }

        cbReadTotal += cbRead;
    }
    return TRUE;
}

// Returns number of bytes read.
DWORD FcReadUntilEof(IN HANDLE input, OUT void *buffer, IN DWORD bufferSize)
{
    DWORD cbReadTotal = 0;
    DWORD cbRead;

    while (cbReadTotal < bufferSize)
    {
        if (!ReadFile(input, (BYTE *) buffer + cbReadTotal, bufferSize - cbReadTotal, &cbRead, NULL))
        {
            return cbReadTotal;
        }
        if (cbRead == 0)
        {
            return cbReadTotal;
        }
        cbReadTotal += cbRead;
    }
    return cbReadTotal;
}

BOOL FcCopyUntilEof(IN HANDLE output, IN HANDLE input)
{
    DWORD cb;
    BYTE buffer[4096];

    while (TRUE)
    {
        if (!ReadFile(input, buffer, sizeof(buffer), &cb, NULL))
        {
            // PIPE returns ERROR_BROKEN_PIPE instead of 0-bytes read on EOF
            if (GetLastError() == ERROR_BROKEN_PIPE)
                break;

            perror("ReadFile");
            return FALSE;
        }

        if (cb == 0)
            break;

        if (!FcWriteBuffer(output, buffer, cb))
        {
            return FALSE;
        }
    }
    return TRUE;
}
