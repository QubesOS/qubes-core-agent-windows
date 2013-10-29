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
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include "log.h"

int write_all(HANDLE fd, void *buf, int size)
{
    int written = 0;
    DWORD ret;
    debugf("write_all: start, %d bytes\n", size);
    while (written < size) {
        if (!WriteFile(fd, (char *) buf + written, size - written, &ret, NULL)) {
            perror("write_all: WriteFile");
            return 0;
        }
        written += ret;
        debugf("write_all: partial write, total %d bytes\n", written);
    }
    debugf("write_all: sent %d bytes\n", size);
    return 1;
}

int read_all(HANDLE fd, void *buf, int size)
{
    int got_read = 0;
    DWORD ret;
    debugf("read_all: start, %d bytes\n", size);
    while (got_read < size) {
        if (!ReadFile(fd, (char *) buf + got_read, size - got_read, &ret, NULL)) {
            perror("read_all: ReadFile");
            return 0;
        }
        if (ret == 0) {
            errno = 0;
            //fprintf(stderr, "EOF\n");
            debugf("read_all: EOF\n");
            return 0;
        }
        got_read += ret;
        debugf("read_all: partial read, total %d bytes\n", got_read);
    }
    debugf("read_all: read %d bytes\n", size);
    return 1;
}

int copy_fd_all(HANDLE fdout, HANDLE fdin)
{
    DWORD ret;
    char buf[4096];
    for (;;) {
        if (!ReadFile(fdin, buf, sizeof(buf), &ret, NULL)) {
            // PIPE returns ERROR_BROKEN_PIPE instead of 0-bytes read on EOF
            if (GetLastError() == ERROR_BROKEN_PIPE)
                break;
            perror("copy_fd_all: ReadFile");
            return 0;
        }
        if (!ret)
            break;
        if (!write_all(fdout, buf, ret)) {
            errorf("copy_fd_all: write_all failed\n");
            return 0;
        }
    }
    return 1;
}
