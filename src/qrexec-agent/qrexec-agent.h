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

#include <qrexec.h>

#define SERVICE_NAME                    L"QrexecAgent"
#define DEFAULT_USER_PASSWORD_UNICODE   L"userpass"

#define	TRIGGER_PIPE_NAME               L"\\\\.\\pipe\\qrexec_trigger"

#define VCHAN_BUFFER_SIZE 65536

// received from qrexec-client-vm
typedef struct _SERVICE_REQUEST
{
    LIST_ENTRY ListEntry;
    struct trigger_service_params ServiceParams;
    PWSTR CommandLine; // executable that will be the local service endpoint
} SERVICE_REQUEST, *PSERVICE_REQUEST;
