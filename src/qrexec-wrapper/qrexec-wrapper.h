/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (c) Invisible Things Lab
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

#pragma once
#include <windows.h>
#include <libvchan.h>
#include <qrexec.h>

// TODO: make configurable
#define DEFAULT_USER_PASSWORD_UNICODE   L"userpass"

#define VCHAN_BUFFER_SIZE 65536
#define PIPE_BUFFER_SIZE 65536
#define PIPE_DEFAULT_TIMEOUT 100

typedef enum _PIPE_TYPE
{
    PTYPE_INVALID = 0,
    PTYPE_STDOUT,
    PTYPE_STDERR,
    PTYPE_STDIN
} PIPE_TYPE;

// child i/o pipe
typedef struct _PIPE_DATA
{
    HANDLE      ReadEndpoint;
    HANDLE      WriteEndpoint;
} PIPE_DATA, *PPIPE_DATA;

// state of the child process
typedef struct _CHILD_STATE
{
    HANDLE       Process;
    HANDLE       StdoutThread;
    HANDLE       StderrThread;

    PIPE_DATA    Stdout;
    PIPE_DATA    Stderr;
    PIPE_DATA    Stdin;

    PSECURITY_DESCRIPTOR PipeSd;
    PACL         PipeAcl;

    libvchan_t   *Vchan;

    BOOL         IsVchanServer;
} CHILD_STATE, *PCHILD_STATE;
