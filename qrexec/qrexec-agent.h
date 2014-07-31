#pragma once
#include <windows.h>
#include <stdlib.h>
#include <tchar.h>
#include <lmcons.h>
#include "common.h"
#include "qrexec.h"
#include "libvchan.h"
#include "glue.h"
#include "service.h"
#include "getopt.h"
#include "exec.h"
#include "pipe-server.h"
#include <strsafe.h>


typedef enum {
	HTYPE_INVALID = 0,
	HTYPE_PROCESS,
	HTYPE_STDOUT,
	HTYPE_STDERR,
	HTYPE_VCHAN
} HANDLE_TYPE;


typedef struct _HANDLE_INFO {
	ULONG	uClientNumber;	// number of the repsective CLIENT_INFO in the g_Clients array
	HANDLE_TYPE	bType;
} HANDLE_INFO, *PHANDLE_INFO;



#define	FREE_CLIENT_SPOT_ID	-1

// Besides the processes, for each of which we have 3 events to watch (process termination, stdout, stderr)
// we have a vchan link event, a stop service event and an "existing client" connect event.
// So it's maximum - 3, and the rest available are divided by the number of events used for each process.
#define	MAX_CLIENTS	((MAXIMUM_WAIT_OBJECTS - 3) / 3)

