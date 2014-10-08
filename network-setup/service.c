#include <windows.h>
#include <stdio.h>

#define SERVICE_NAME TEXT("QubesNetworkSetup")

SERVICE_STATUS ServiceStatus;
SERVICE_STATUS_HANDLE hStatus;

void  ServiceMain(int argc, WCHAR* argv[]);
void  ControlHandler(DWORD request);

// from qubes-network-setup.c
int qubes_setup_network(void);

void service_main()
{
    SERVICE_TABLE_ENTRY ServiceTable[2];
    ServiceTable[0].lpServiceName = SERVICE_NAME;
    ServiceTable[0].lpServiceProc = (LPSERVICE_MAIN_FUNCTION) ServiceMain;
    ServiceTable[1].lpServiceName = NULL;
    ServiceTable[1].lpServiceProc = NULL;
    // Start the control dispatcher thread for our service
    StartServiceCtrlDispatcher(ServiceTable);
}

void ServiceMain(int argc, WCHAR* argv[])
{
    int error;
    ServiceStatus.dwServiceType = SERVICE_WIN32;
    ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
    ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    ServiceStatus.dwWin32ExitCode = 0;
    ServiceStatus.dwServiceSpecificExitCode = 0;
    ServiceStatus.dwCheckPoint = 0;
    ServiceStatus.dwWaitHint = 0;
    hStatus = RegisterServiceCtrlHandler(
        SERVICE_NAME,
        (LPHANDLER_FUNCTION) ControlHandler);

    if (hStatus == (SERVICE_STATUS_HANDLE) 0)
    {
        // Registering Control Handler failed
        return;
    }
    // We report the running status to SCM.
    ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(hStatus, &ServiceStatus);

    error = qubes_setup_network();

    // Done
    ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    ServiceStatus.dwWin32ExitCode = error;
    SetServiceStatus(hStatus, &ServiceStatus);
    return;
}

// Control handler function
void ControlHandler(DWORD request)
{
    switch (request)
    {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        ServiceStatus.dwWin32ExitCode = 0;
        ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(hStatus, &ServiceStatus);
        return;
    default:
        break;
    }
    // Report current status
    SetServiceStatus(hStatus, &ServiceStatus);
    return;
}
