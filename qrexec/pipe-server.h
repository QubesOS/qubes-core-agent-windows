#pragma once
#include <tchar.h>
#include <windows.h>
#include <aclapi.h>
#include <strsafe.h>

#include "qrexec.h"
#include "common.h"
#include "log.h"
#include "agent.h"

typedef enum {
	STATE_WAITING_FOR_CLIENT = 0,
	STATE_RECEIVING_PARAMETERS,
	STATE_WAITING_FOR_DAEMON_DECISION,
	STATE_SENDING_IO_HANDLES,
	STATE_RECEIVING_PROCESS_HANDLE
};

#define INSTANCES	4 
#define PIPE_TIMEOUT	5000
#define IO_HANDLES_ARRAY_SIZE	sizeof(IO_HANDLES_ARRAY)
 
typedef struct {
	OVERLAPPED	oOverlapped;
	HANDLE	hPipeInst;
	ULONG	uState;
	BOOLEAN	fPendingIO;
	int	assigned_client_id;
	struct	trigger_connect_params	params;

	CLIENT_INFO	ClientInfo;
	IO_HANDLES_ARRAY	RemoteHandles;

	HANDLE	hClientProcess;
	CREATE_PROCESS_RESPONSE	CreateProcessResponse;
} PIPEINST, *LPPIPEINST;


ULONG WINAPI WatchForTriggerEvents(
	PVOID pParam
);

ULONG ProceedWithExecution(
	int assigned_client_id,
	PUCHAR pszIdent
);

extern CRITICAL_SECTION	g_PipesCriticalSection;
