#pragma once
#include <windows.h>
#include <stdlib.h>
#include <lmcons.h>
#include <strsafe.h>

#include "common.h"
#include "service.h"
#include "pipe-server.h"

#include <qrexec.h>
#include <libvchan.h>
#include <vchan-common.h>
#include <getopt.h>
#include <exec.h>

typedef enum _HANDLE_TYPE
{
    HTYPE_INVALID = 0,
    HTYPE_PROCESS,
    HTYPE_STDOUT,
    HTYPE_STDERR,
    HTYPE_CONTROL_VCHAN,
    HTYPE_DATA_VCHAN
} HANDLE_TYPE;

typedef struct _HANDLE_INFO
{
    ULONG ClientIndex;	// number of the repsective CLIENT_INFO in the g_Clients array
    HANDLE_TYPE	Type;
} HANDLE_INFO;

// Besides the processes, for each of which we have 3 events to watch (process termination, stdout, stderr)
// we have a vchan link event, a stop service event and an "existing client" connect event.
// So it's maximum - 3, and the rest available are divided by the number of events used for each process.
#define	MAX_CLIENTS	((MAXIMUM_WAIT_OBJECTS - 3) / 3)
