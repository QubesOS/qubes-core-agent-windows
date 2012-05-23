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

#define REXEC_PORT 512


enum {
	MSG_CLIENT_TO_SERVER_EXEC_CMDLINE = 0x100,
	MSG_CLIENT_TO_SERVER_JUST_EXEC,
	MSG_CLIENT_TO_SERVER_CONNECT_EXISTING,

	MSG_SERVER_TO_AGENT_CONNECT_EXISTING,
	MSG_SERVER_TO_AGENT_EXEC_CMDLINE,
	MSG_SERVER_TO_AGENT_JUST_EXEC,
	MSG_SERVER_TO_AGENT_INPUT,
	MSG_SERVER_TO_AGENT_CLIENT_END,

	MSG_XOFF,
	MSG_XON,

	MSG_AGENT_TO_SERVER_STDOUT,
	MSG_AGENT_TO_SERVER_STDERR,
	MSG_AGENT_TO_SERVER_EXIT_CODE,
	MSG_AGENT_TO_SERVER_TRIGGER_CONNECT_EXISTING,

	MSG_SERVER_TO_CLIENT_STDOUT,
	MSG_SERVER_TO_CLIENT_STDERR,
	MSG_SERVER_TO_CLIENT_EXIT_CODE
};

struct server_header {
	unsigned int type;
	unsigned int client_id;
	unsigned int len;
};

struct client_header {
	unsigned int type;
	unsigned int len;
};

typedef enum {
	HTYPE_INVALID = 0,
	HTYPE_PROCESS,
	HTYPE_STDOUT,
	HTYPE_STDERR
} HANDLE_TYPE;


typedef struct _HANDLE_INFO {
	ULONG	uClientNumber;	// number of the repsective CLIENT_INFO in the g_Clients array
	HANDLE_TYPE	bType;
} HANDLE_INFO, *PHANDLE_INFO;

#define READ_BUFFER_SIZE	1024

typedef struct _PIPE_DATA {
	HANDLE	hReadPipe;
	BOOLEAN	bReadInProgress;
	OVERLAPPED	olRead;
	CHAR	ReadBuffer[READ_BUFFER_SIZE];
} PIPE_DATA, *PPIPE_DATA;

typedef struct _CLIENT_INFO {
	int	client_id;

	HANDLE	hProcess;
	HANDLE	hWriteStdinPipe;

	PIPE_DATA	Stdout;
	PIPE_DATA	Stderr;
	
} CLIENT_INFO, *PCLIENT_INFO;


#define FREE_CLIENT_SPOT_ID	-1
#define	MAX_CLIENTS	((MAXIMUM_WAIT_OBJECTS - 1) / 3)
