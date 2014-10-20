#include <windows.h>
#include <stdio.h>

#define SERVICE_NAME L"QubesNetworkSetup"

SERVICE_STATUS g_serviceStatus;
SERVICE_STATUS_HANDLE g_statusHandle;

void ServiceMain(IN int argc, IN WCHAR* argv[]);
void ControlHandler(IN DWORD request);

// from qubes-network-setup.c
DWORD SetupNetwork(void);

DWORD ServiceStartup(void)
{
    SERVICE_TABLE_ENTRY serviceTable[2];
    serviceTable[0].lpServiceName = SERVICE_NAME;
    serviceTable[0].lpServiceProc = (LPSERVICE_MAIN_FUNCTION) ServiceMain;
    serviceTable[1].lpServiceName = NULL;
    serviceTable[1].lpServiceProc = NULL;
    // Start the control dispatcher thread for our service
    StartServiceCtrlDispatcher(serviceTable);
    return GetLastError();
}

void ServiceMain(IN int argc, IN WCHAR* argv[])
{
    DWORD status;

    g_serviceStatus.dwServiceType = SERVICE_WIN32;
    g_serviceStatus.dwCurrentState = SERVICE_START_PENDING;
    g_serviceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    g_serviceStatus.dwWin32ExitCode = 0;
    g_serviceStatus.dwServiceSpecificExitCode = 0;
    g_serviceStatus.dwCheckPoint = 0;
    g_serviceStatus.dwWaitHint = 0;
    g_statusHandle = RegisterServiceCtrlHandler(
        SERVICE_NAME,
        (LPHANDLER_FUNCTION) ControlHandler);

    if (g_statusHandle == (SERVICE_STATUS_HANDLE) 0)
    {
        // Registering Control Handler failed
        return;
    }
    // We report the running status to SCM.
    g_serviceStatus.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_statusHandle, &g_serviceStatus);

    status = SetupNetwork();

    // Done
    g_serviceStatus.dwCurrentState = SERVICE_STOPPED;
    g_serviceStatus.dwWin32ExitCode = status;
    SetServiceStatus(g_statusHandle, &g_serviceStatus);
    return;
}

// Control handler function
void ControlHandler(IN DWORD request)
{
    switch (request)
    {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        g_serviceStatus.dwWin32ExitCode = 0;
        g_serviceStatus.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(g_statusHandle, &g_serviceStatus);
        return;
    default:
        break;
    }
    // Report current status
    SetServiceStatus(g_statusHandle, &g_serviceStatus);
    return;
}
