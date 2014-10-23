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
#include <stdlib.h>

#include "libvchan.h"

struct libvchan *g_Vchan;

int VchanSendBuffer(IN const void *buffer, IN int size)
{
    int written = 0;
    int status;

    if (!g_Vchan)
        return -1;

    while (written < size)
    {
        status = libvchan_write(g_Vchan, (char *) buffer + written, size - written);
        if (status <= 0)
            return status;

        written += status;
    }

    return size;
}

int VchanReceiveBuffer(OUT void *buffer, IN int size)
{
    int written = 0;
    int status;

    while (written < size)
    {
        status = libvchan_read(g_Vchan, (char *) buffer + written, size - written);
        if (status <= 0)
            return status;

        written += status;
    }
    return size;
}

int VchanGetReadBufferSize(void)
{
    return libvchan_data_ready(g_Vchan);
}

int VchanGetWriteBufferSize(void)
{
    return libvchan_buffer_space(g_Vchan);
}

BOOL VchanInitServer(IN int port)
{
    g_Vchan = libvchan_server_init(port);
    if (!g_Vchan)
        return FALSE;

    return TRUE;
}
