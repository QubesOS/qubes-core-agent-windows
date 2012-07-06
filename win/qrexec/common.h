#pragma once


#ifndef _PREFAST_
#pragma warning(disable:4068)
#endif

#define BUILD_AS_SERVICE

#define SERVICE_NAME	TEXT("qrexec_agent")
#define DEFAULT_USER_PASSWORD_UNICODE	L"userpass"

#ifdef DBG
#define LOG_FILE		TEXT("c:\\qrexec_agent.log")
#endif

#define	TRIGGER_PIPE_NAME	TEXT("\\\\.\\pipe\\qrexec_trigger")

//#define DISPLAY_CONSOLE_OUTPUT

//#define START_SERVICE_AFTER_INSTALLATION


#define READ_BUFFER_SIZE	512

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
	OVERLAPPED	olRead;
	CHAR	ReadBuffer[READ_BUFFER_SIZE + 1];
} PIPE_DATA, *PPIPE_DATA;

typedef struct _CLIENT_INFO {
	int	client_id;
	BOOLEAN	bClientIsReady;

	HANDLE	hProcess;
	HANDLE	hWriteStdinPipe;

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