#pragma once
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <tchar.h>
#include <strsafe.h>
#include <lmcons.h>
#include "common.h"
#include "qrexec.h"
#include "libvchan.h"
#include "glue.h"
#include "service.h"
#include "getopt.h"
#include "errors.h"
#include "exec.h"



typedef enum {
	HTYPE_INVALID = 0,
	HTYPE_PROCESS,
	HTYPE_STDOUT,
	HTYPE_STDERR,
	HTYPE_VCHAN
} HANDLE_TYPE;

typedef enum {
	PTYPE_INVALID = 0,
	PTYPE_STDOUT,
	PTYPE_STDERR
} PIPE_TYPE;

typedef struct _HANDLE_INFO {
	ULONG	uClientNumber;	// number of the repsective CLIENT_INFO in the g_Clients array
	HANDLE_TYPE	bType;
} HANDLE_INFO, *PHANDLE_INFO;

#define READ_BUFFER_SIZE	512

typedef struct _PIPE_DATA {	
	HANDLE	hReadPipe;
	PIPE_TYPE	bPipeType;
	BOOLEAN	bReadInProgress;
	BOOLEAN	bDataIsReady;
	OVERLAPPED	olRead;
	CHAR	ReadBuffer[READ_BUFFER_SIZE + 1];
} PIPE_DATA, *PPIPE_DATA;

typedef struct _CLIENT_INFO {
	int	client_id;

	HANDLE	hProcess;
	HANDLE	hWriteStdinPipe;

	BOOLEAN	bReadingIsDisabled;

	PIPE_DATA	Stdout;
	PIPE_DATA	Stderr;
	
} CLIENT_INFO, *PCLIENT_INFO;


#define FREE_CLIENT_SPOT_ID	-1

#ifdef BUILD_AS_SERVICE
#define	MAX_CLIENTS	((MAXIMUM_WAIT_OBJECTS - 2) / 3)
#else
#define	MAX_CLIENTS	((MAXIMUM_WAIT_OBJECTS - 1) / 3)
#endif