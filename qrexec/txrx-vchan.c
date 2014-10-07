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
#include "libvchan.h"
#include <stdlib.h>

struct libvchan *ctrl;

int write_all_vchan_ext(void *buf, int size)
{
    int written = 0;
    int ret;

    if (!ctrl)
        return -1;

    while (written < size)
    {
        ret =
            libvchan_write(ctrl, (char *) buf + written,
            size - written);
        if (ret <= 0)
            return ret;

        written += ret;
    }

    //      fprintf(stderr, "sent %d bytes\n", size);
    return size;
}

int read_all_vchan_ext(void *buf, int size)
{
    int written = 0;
    int ret;

    while (written < size)
    {
        ret =
            libvchan_read(ctrl, (char *) buf + written,
            size - written);
        if (ret <= 0)
            return ret;

        written += ret;
    }
    //      fprintf(stderr, "read %d bytes\n", size);
    return size;
}

int read_ready_vchan_ext()
{
    return libvchan_data_ready(ctrl);
}

int buffer_space_vchan_ext()
{
    return libvchan_buffer_space(ctrl);
}

int peer_server_init(int port)
{
    ctrl = libvchan_server_init(port);
    if (!ctrl)
        return 1;

    return 0;
}
