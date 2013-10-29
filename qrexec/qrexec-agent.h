#pragma once
#include <tchar.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <lmcons.h>
#include <shlwapi.h>
#include <strsafe.h> // include after shlwapi or it breaks some shlwapi string functions

#include "common.h"
#include "qrexec.h"
#include "vchan-common.h"
#include "glue.h"
//#include "service.h"
#include "getopt.h"
#include "errors.h"
#include "exec.h"
#include "pipe-server.h"

typedef enum {
    HTYPE_INVALID = 0,
    HTYPE_PROCESS,
    HTYPE_STDOUT,
    HTYPE_STDERR,
    HTYPE_DAEMON_VCHAN,
    HTYPE_CLIENT_VCHAN
} HANDLE_TYPE;

typedef struct _HANDLE_INFO {
    ULONG	uChildIndex;	// index of the respective CHILD_INFO in the g_Children array
    HANDLE_TYPE	bType;
} HANDLE_INFO;

// Besides the processes, for each of which we have 4 events to watch
// (process termination, stdout, stderr, associated qrexec-client vchan),
// we also have a daemon vchan event, a stop service event and an "existing client" connect event.
// So it's maximum - 3, and the rest available are divided by the number of events used for each process.
#define	MAX_CHILDREN	((MAXIMUM_WAIT_OBJECTS - 3) / 4)
