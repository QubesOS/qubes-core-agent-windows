/* 
    Common routines between Xen store user library and daemon.
    Copyright (C) 2005 Rusty Russell IBM Corporation

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "xs_lib.h"

/* Common routines for the Xen store daemon and client library. */

/* Simple routines for writing to sockets, etc. */
BOOL xs_write_all(HANDLE fd, const void *data, unsigned int len)
{

	char* data_chars = (char*)data;
	OVERLAPPED ol;
	ol.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

	while (len) {
		DWORD done;

		memset(&ol, 0, sizeof(ol));

		// Synchronous write against an overlapped FD:

		if(!WriteFile(fd, data_chars, len, NULL, &ol)) {
			if(GetLastError() != ERROR_IO_PENDING) {
				CloseHandle(ol.hEvent);
				return FALSE;
			}
		}
		if(!GetOverlappedResult(fd, &ol, &done, TRUE)) {
			CloseHandle(ol.hEvent);
			return FALSE;
		}

		data_chars += done;
		len -= done;
	}

	CloseHandle(ol.hEvent);
	return TRUE;
}

/* Convert strings to permissions.  False if a problem. */
BOOL xs_strings_to_perms(struct xs_permissions *perms, unsigned int num,
			 const char *strings)
{
	const char *p;
	char *end;
	unsigned int i;

	for (p = strings, i = 0; i < num; i++) {
		/* "r", "w", or "b" for both. */
		switch (*p) {
		case 'r':
			perms[i].perms = XS_PERM_READ;
			break;
		case 'w':
			perms[i].perms = XS_PERM_WRITE;
			break;
		case 'b':
			perms[i].perms = XS_PERM_READ|XS_PERM_WRITE;
			break;
		case 'n':
			perms[i].perms = XS_PERM_NONE;
			break;
		default:
			return FALSE;
		} 
		p++;
		perms[i].id = strtol(p, &end, 0);
		if (*end || !*p) {
			return FALSE;
		}
		p = end + 1;
	}
	return TRUE;
}

/* Convert permissions to a string (up to len MAX_STRLEN(unsigned int)+1). */
BOOL xs_perm_to_string(const struct xs_permissions *perm,
                       char *buffer, size_t buf_len)
{
	switch (perm->perms) {
	case XS_PERM_WRITE:
		*buffer = 'w';
		break;
	case XS_PERM_READ:
		*buffer = 'r';
		break;
	case XS_PERM_READ|XS_PERM_WRITE:
		*buffer = 'b';
		break;
	case XS_PERM_NONE:
		*buffer = 'n';
		break;
	default:
		return FALSE;
	}
	_snprintf(buffer+1, buf_len-1, "%i", (int)perm->id);
	return TRUE;
}

/* Given a string and a length, count how many strings (nul terms). */
unsigned int xs_count_strings(const char *strings, unsigned int len)
{
	unsigned int num;
	const char *p;

	for (p = strings, num = 0; p < strings + len; p++)
		if (*p == '\0')
			num++;

	return num;
}
