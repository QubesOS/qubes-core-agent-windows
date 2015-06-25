#pragma once
#include <windows.h>

#include <libvchan.h>
#include <qrexec.h>

#define BUILD_AS_SERVICE

#define SERVICE_NAME                    L"QrexecAgent"
#define DEFAULT_USER_PASSWORD_UNICODE   L"userpass"

#define	TRIGGER_PIPE_NAME               L"\\\\.\\pipe\\qrexec_trigger"

#define VCHAN_BUFFER_SIZE 65536
#define PIPE_BUFFER_SIZE 65536
#define PIPE_DEFAULT_TIMEOUT 50

typedef enum _PIPE_TYPE
{
    PTYPE_INVALID = 0,
    PTYPE_STDOUT,
    PTYPE_STDERR
} PIPE_TYPE;

// child i/o state for a single pipe
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
    BYTE        ReadBuffer[MAX_DATA_CHUNK];
} PIPE_DATA;

// state of a child process
typedef struct _CLIENT_INFO
{
    BOOL ClientIsReady;

    HANDLE ChildProcess;
    HANDLE WriteStdinPipe;
    BOOL StdinPipeClosed;
    BOOL ChildExited;
    DWORD ExitCode;

    BOOL ReadingDisabled;

    PIPE_DATA StdoutData;
    PIPE_DATA StderrData;

    libvchan_t *Vchan; // associated client's vchan for i/o data exchange

    // Usually qrexec-client is the vchan server, but in vm/vm connections
    // two agents are connected. This field is TRUE if we're the server.
    BOOL IsVchanServer;
} CLIENT_INFO;

ULONG AddExistingClient(
    CLIENT_INFO *clientInfo
    );

ULONG CreateClientPipes(
    IN OUT CLIENT_INFO *clientInfo,
    OUT HANDLE *pipeStdin,
    OUT HANDLE *pipeStdout,
    OUT HANDLE *pipeStderr
    );

ULONG CloseReadPipeHandles(
    IN CLIENT_INFO *clientInfo OPTIONAL,
    IN OUT PIPE_DATA *data
    );

ULONG SendMessageToVchan(
    IN libvchan_t *vchan,
    IN UINT messageType,
    IN const void *data,
    IN ULONG cbData,
    OUT ULONG *cbWritten,
    IN const WCHAR *what
    );

ULONG SendExitCodeVchan(
    IN libvchan_t *vchan,
    IN int exitCode
    );

ULONG SendExitCode(
    IN const CLIENT_INFO *clientInfo,
    IN int exitCode
    );
