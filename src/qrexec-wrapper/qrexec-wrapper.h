#pragma once
#include <windows.h>
#include <libvchan.h>
#include <qrexec.h>

#define DEFAULT_USER_PASSWORD_UNICODE   L"userpass"

#define VCHAN_BUFFER_SIZE 65536
#define PIPE_BUFFER_SIZE 65536
#define PIPE_DEFAULT_TIMEOUT 100

typedef enum _PIPE_TYPE
{
    PTYPE_INVALID = 0,
    PTYPE_STDOUT,
    PTYPE_STDERR,
    PTYPE_STDIN
} PIPE_TYPE;

// child i/o pipe
typedef struct _PIPE_DATA
{
    HANDLE      ReadEndpoint;
    HANDLE      WriteEndpoint;
} PIPE_DATA, *PPIPE_DATA;

// state of the child process
typedef struct _CHILD_STATE
{
    HANDLE       Process;
    HANDLE       StdoutThread;
    HANDLE       StderrThread;

    PIPE_DATA    Stdout;
    PIPE_DATA    Stderr;
    PIPE_DATA    Stdin;

    PSECURITY_DESCRIPTOR PipeSd;
    PACL         PipeAcl;

    libvchan_t   *Vchan;

    BOOL         IsVchanServer;
} CHILD_STATE, *PCHILD_STATE;
