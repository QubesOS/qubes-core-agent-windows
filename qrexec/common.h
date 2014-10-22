#pragma once
#include <windows.h>

#ifndef _PREFAST_
#pragma warning(disable:4068)
#endif

#define BUILD_AS_SERVICE

#define SERVICE_NAME                    TEXT("qrexec_agent")
#define DEFAULT_USER_PASSWORD_UNICODE   L"userpass"

#define	TRIGGER_PIPE_NAME               TEXT("\\\\.\\pipe\\qrexec_trigger")

// wr_ring_size[=1024] - sizeof(hdr)[=12]
#define READ_BUFFER_SIZE    1012

typedef enum _PIPE_TYPE
{
    PTYPE_INVALID = 0,
    PTYPE_STDOUT,
    PTYPE_STDERR
} PIPE_TYPE;

typedef struct _PIPE_DATA
{
    HANDLE      ReadPipe;
    PIPE_TYPE   PipeType;
    BOOL        ReadInProgress;
    BOOL        DataIsReady;
    BOOL        PipeClosed;
    BOOL        VchanWritePending;
    DWORD       cbSentBytes;
    OVERLAPPED  ReadState;
    BYTE        ReadBuffer[READ_BUFFER_SIZE + 1];
} PIPE_DATA;

typedef struct _CLIENT_INFO
{
    UINT ClientId;
    BOOL ClientIsReady;

    HANDLE ChildProcess;
    HANDLE WriteStdinPipe;
    BOOL StdinPipeClosed;
    BOOL ChildExited;
    DWORD ExitCode;

    BOOL ReadingDisabled;

    PIPE_DATA StdoutData;
    PIPE_DATA StderrData;
} CLIENT_INFO;

ULONG AddExistingClient(
    ULONG clientId,
    CLIENT_INFO *clientInfo
    );

ULONG CreateClientPipes(
    IN OUT CLIENT_INFO *clientInfo,
    OUT HANDLE *pipeStdin,
    OUT HANDLE *pipeStdout,
    OUT HANDLE *pipeStderr
    );

ULONG CloseReadPipeHandles(
    IN ULONG clientId,
    IN OUT PIPE_DATA *data
    );

ULONG SendMessageToDaemon(
    IN ULONG clientId,
    IN UINT messageType,
    IN const void *data,
    IN ULONG cbData,
    OUT ULONG *cbWritten
    );

ULONG SendExitCode(
    IN ULONG clientId,
    IN int exitCode
    );
