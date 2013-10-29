#pragma once
#include <windows.h>
#include "libvchan.h"
#include "qrexec.h"

//#define BUILD_AS_SERVICE

#define SERVICE_NAME	TEXT("qrexec-agent")
#define DEFAULT_USER_PASSWORD_UNICODE	L"userpass"

//#define DISPLAY_CONSOLE_OUTPUT

//#define START_SERVICE_AFTER_INSTALLATION

#define READ_BUFFER_SIZE (65536)

#define VCHAN_BUFFER_SIZE 65536
#define PIPE_BUFFER_SIZE 65536
#define PIPE_DEFAULT_TIMEOUT 50

typedef enum {
    PTYPE_INVALID = 0,
    PTYPE_STDOUT,
    PTYPE_STDERR
} PIPE_TYPE;

// child i/o state for a single pipe
typedef struct _PIPE_DATA {
    HANDLE	hReadPipe;
    PIPE_TYPE	bPipeType;
    BOOL	bReadInProgress;
    BOOL	bDataIsReady;
    BOOL bPipeClosed;
    BOOL bVchanWritePending;
    DWORD	dwSentBytes;
    OVERLAPPED	olRead;
    CHAR	ReadBuffer[READ_BUFFER_SIZE + 1];
} PIPE_DATA;

// state of a child process
typedef struct _CHILD_INFO {
    //int	client_id;
    BOOLEAN	bChildIsReady;

    HANDLE	hProcess;
    HANDLE	hWriteStdinPipe;
    BOOL	bStdinPipeClosed;
    BOOL	bChildExited;
    DWORD	dwExitCode;

    BOOL	bReadingIsDisabled;

    PIPE_DATA	Stdout;
    PIPE_DATA	Stderr;

    libvchan_t *vchan; // associated client's vchan for i/o data exchange
} CHILD_INFO;

ULONG AddExistingClient(
    libvchan_t *vchan,
    CHILD_INFO *pChildInfo
);

ULONG CreateChildPipes(
    CHILD_INFO *pChildInfo,
    HANDLE *phPipeStdin,
    HANDLE *phPipeStdout,
    HANDLE *phPipeStderr
);

ULONG CloseReadPipeHandles(
    libvchan_t *vchan,
    PIPE_DATA *pPipeData
);

ULONG send_msg_to_vchan(
    libvchan_t *vchan,
    int type,
    void *pData,
    ULONG uDataSize,
    ULONG *puDataWritten
);

ULONG send_exit_code(
    libvchan_t *vchan,
    int status
);

ULONG handle_input(struct msg_header *hdr, libvchan_t *vchan);
