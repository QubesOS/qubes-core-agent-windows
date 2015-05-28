#pragma once
#include <windows.h>
#include <aclapi.h>
#include "common.h"
#include "qrexec.h"
#include "log.h"
#include <strsafe.h>

typedef enum _CONNECTION_STATE
{
    STATE_WAITING_FOR_CLIENT = 0,
    STATE_RECEIVING_PARAMETERS,
    STATE_WAITING_FOR_DAEMON_DECISION,
    STATE_SENDING_IO_HANDLES,
    STATE_RECEIVING_PROCESS_HANDLE
} CONNECTION_STATE;

#define TRIGGER_PIPE_INSTANCES	4
#define PIPE_TIMEOUT	5000
#define IO_HANDLES_SIZE	sizeof(IO_HANDLES)

typedef struct _PIPE_INSTANCE
{
    OVERLAPPED AsyncState;
    HANDLE Pipe;
    CONNECTION_STATE ConnectionState;
    BOOL PendingIo;
    libvchan_t *Vchan;
    struct trigger_service_params ConnectParams;

    CLIENT_INFO ClientInfo;
    IO_HANDLES RemoteHandles;

    HANDLE ClientProcess;
    CREATE_PROCESS_RESPONSE CreateProcessResponse;
} PIPE_INSTANCE;

ULONG WINAPI WatchForTriggerEvents(
    IN void *param
    );

ULONG ProceedWithExecution(
    IN libvchan_t *vchan,
    IN const char *ident,
    BOOL isVchanServer
    );

extern CRITICAL_SECTION	g_PipesCriticalSection;
