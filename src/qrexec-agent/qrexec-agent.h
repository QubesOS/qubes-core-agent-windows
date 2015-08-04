#pragma once
#include <windows.h>

#include <qrexec.h>

#define SERVICE_NAME                    L"QrexecAgent"
#define DEFAULT_USER_PASSWORD_UNICODE   L"userpass"

#define	TRIGGER_PIPE_NAME               L"\\\\.\\pipe\\qrexec_trigger"

#define VCHAN_BUFFER_SIZE 65536

// received from qrexec-client-vm
typedef struct _SERVICE_REQUEST
{
    LIST_ENTRY ListEntry;
    struct trigger_service_params ServiceParams;
    PWSTR CommandLine; // executable that will be the local service endpoint
} SERVICE_REQUEST, *PSERVICE_REQUEST;
