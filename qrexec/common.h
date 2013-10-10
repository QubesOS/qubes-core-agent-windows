#pragma once


//#ifndef _PREFAST_
//#pragma warning(disable:4068)
//#endif

//#define BUILD_AS_SERVICE

#define SERVICE_NAME	TEXT("qrexec-agent")
#define DEFAULT_USER_PASSWORD_UNICODE	L"userpass"

// todo: this should be taken from some system-wide configuration (or passed as argument)
#define LOG_DIR TEXT("c:\\qubes\\logs")

//#define DISPLAY_CONSOLE_OUTPUT

//#define START_SERVICE_AFTER_INSTALLATION

// wr_ring_size[=1024] - sizeof(hdr)[=12]
#define READ_BUFFER_SIZE	1012

typedef enum {
	PTYPE_INVALID = 0,
	PTYPE_STDOUT,
	PTYPE_STDERR
} PIPE_TYPE;


typedef struct _PIPE_DATA {	
	HANDLE	hReadPipe;
	PIPE_TYPE	bPipeType;
	BOOLEAN	bReadInProgress;
	BOOLEAN	bDataIsReady;
	BOOLEAN bPipeClosed;
	BOOLEAN bVchanWritePending;
	DWORD	dwSentBytes;
	OVERLAPPED	olRead;
	CHAR	ReadBuffer[READ_BUFFER_SIZE + 1];
} PIPE_DATA, *PPIPE_DATA;

typedef struct _CLIENT_INFO {
	int	client_id;
	BOOLEAN	bClientIsReady;

	HANDLE	hProcess;
	HANDLE	hWriteStdinPipe;
	BOOLEAN	bStdinPipeClosed;
	BOOLEAN	bChildExited;
	DWORD	dwExitCode;

	BOOLEAN	bReadingIsDisabled;

	PIPE_DATA	Stdout;
	PIPE_DATA	Stderr;
	
} CLIENT_INFO, *PCLIENT_INFO;

ULONG AddExistingClient(
	int client_id, 
	PCLIENT_INFO pClientInfo
);

ULONG CreateClientPipes(
	CLIENT_INFO *pClientInfo, 
	HANDLE *phPipeStdin, 
	HANDLE *phPipeStdout, 
	HANDLE *phPipeStderr
);

ULONG CloseReadPipeHandles(
	int client_id, 
	PIPE_DATA *pPipeData
);

ULONG ReturnData(
	int client_id,
	int type,
	PVOID pData,
	ULONG uDataSize,
	PULONG puDataWritten
);

ULONG send_exit_code(
	int client_id,
	int status
);
