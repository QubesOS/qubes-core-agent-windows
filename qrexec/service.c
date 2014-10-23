#include "service.h"

HANDLE g_StopServiceEvent;

static HANDLE g_ServiceThread;
static SERVICE_STATUS g_ServiceStatus;
static SERVICE_STATUS_HANDLE g_ServiceStatusHandle;

extern ULONG WINAPI ServiceExecutionThread(void *param);

void StopService(void);

ULONG UpdateServiceStatus(
    IN DWORD currentState,
    IN DWORD win32ExitCode,
    IN DWORD serviceSpecificExitCode,
    IN DWORD waitHint
    )
{
    ULONG status;
    static DWORD checkPoint = 1;

    LogVerbose("start");

    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwCurrentState = currentState;
    g_ServiceStatus.dwServiceSpecificExitCode = serviceSpecificExitCode;
    g_ServiceStatus.dwWaitHint = waitHint;

    if (SERVICE_START_PENDING == currentState)
    {
        g_ServiceStatus.dwControlsAccepted = 0;
    }
    else
    {
        g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    }

    if ((SERVICE_RUNNING == currentState) || (SERVICE_STOPPED == currentState))
        g_ServiceStatus.dwCheckPoint = 0;
    else
        g_ServiceStatus.dwCheckPoint = checkPoint++;

    if (0 == serviceSpecificExitCode)
    {
        g_ServiceStatus.dwWin32ExitCode = win32ExitCode;
    }
    else
    {
        g_ServiceStatus.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
    }

    if (!SetServiceStatus(g_ServiceStatusHandle, &g_ServiceStatus))
    {
        status = perror("SetServiceStatus");
        StopService();
        return status;
    }

    LogVerbose("success");

    return ERROR_SUCCESS;
}

static void StopService(void)
{
    LogDebug("Signaling the stop event\n");

    SetEvent(g_StopServiceEvent);
    WaitForSingleObject(g_ServiceThread, INFINITE);

    if (SERVICE_STOPPED != g_ServiceStatus.dwCurrentState)
        UpdateServiceStatus(SERVICE_STOPPED, NO_ERROR, 0, 0);

    LogVerbose("success");
}

static void WINAPI ServiceCtrlHandler(IN ULONG controlCode)
{
    LogVerbose("code %d", controlCode);

    switch (controlCode)
    {
    case SERVICE_CONTROL_SHUTDOWN:
    case SERVICE_CONTROL_STOP:
        UpdateServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 0, 3000);
        StopService();
        return;
    default:
        break;
    }

    UpdateServiceStatus(g_ServiceStatus.dwCurrentState, NO_ERROR, 0, 0);

    LogVerbose("success");
}

void WINAPI ServiceMain(DWORD argc, WCHAR *argv[])
{
    ULONG status;

    LogVerbose("start");

    // Manual reset, initial state is not signaled
    g_StopServiceEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!g_StopServiceEvent)
    {
        perror("CreateEvent");
        return;
    }

    g_ServiceStatusHandle = RegisterServiceCtrlHandler(SERVICE_NAME, (LPHANDLER_FUNCTION) ServiceCtrlHandler);
    if (!g_ServiceStatusHandle)
    {
        perror("RegisterServiceCtrlHandler");
        CloseHandle(g_StopServiceEvent);
        return;
    }

    status = UpdateServiceStatus(SERVICE_START_PENDING, NO_ERROR, 0, 500);
    if (ERROR_SUCCESS != status)
    {
        perror2(status, "UpdateServiceStatus");
        CloseHandle(g_StopServiceEvent);
        return;
    }

    status = Init(&g_ServiceThread);
    if (ERROR_SUCCESS != status)
    {
        CloseHandle(g_StopServiceEvent);
        UpdateServiceStatus(SERVICE_STOPPED, status, 0, 0);
        perror2(status, "Init");
        return;
    }

    UpdateServiceStatus(SERVICE_RUNNING, NO_ERROR, 0, 0);

    WaitForSingleObject(g_ServiceThread, INFINITE);
    UpdateServiceStatus(SERVICE_STOPPED, NO_ERROR, 0, 0);

    CloseHandle(g_ServiceThread);
    CloseHandle(g_StopServiceEvent);

    LogVerbose("success");
}

static ULONG WaitForService(IN DWORD pendingState, IN DWORD wantedState, IN HANDLE service, OUT DWORD *currentState, OUT DWORD *exitCode)
{
    SERVICE_STATUS_PROCESS serviceStatus;
    DWORD bytesNeeded;
    DWORD startTickCount;
    DWORD oldCheckPoint;
    DWORD waitTime;

    LogVerbose("start");

    if (!currentState || !exitCode)
        return ERROR_INVALID_PARAMETER;

    // Check the status until the service is no longer start pending.

    if (!QueryServiceStatusEx(
        service, // handle to service
        SC_STATUS_PROCESS_INFO, // info level
        (BYTE *) &serviceStatus, // address of structure
        sizeof(serviceStatus), // size of structure
        &bytesNeeded))
    {
        return perror("QueryServiceStatusEx");
    }

    if (wantedState == serviceStatus.dwCurrentState)
    {
        *currentState = serviceStatus.dwCurrentState;
        *exitCode = serviceStatus.dwWin32ExitCode;
        return ERROR_SUCCESS;
    }

    // Save the tick count and initial checkpoint.
#pragma prefast(suppress:28159, "This routine will not run for longer than 10 seconds")
    // omeg: ^ what if the machine is suspended mid-execution? :P
    startTickCount = GetTickCount();
    oldCheckPoint = serviceStatus.dwCheckPoint;

    while (pendingState == serviceStatus.dwCurrentState)
    {
        // Do not wait longer than the wait hint. A good interval is
        // one-tenth the wait hint, but no less than 1 second and no
        // more than 10 seconds.

        waitTime = serviceStatus.dwWaitHint / 10;

        if (waitTime < 1000)
            waitTime = 1000;
        else
            if (waitTime > 10000)
                waitTime = 10000;

        Sleep(waitTime);

        // Check the status again.

        if (!QueryServiceStatusEx(
            service,
            SC_STATUS_PROCESS_INFO,
            (BYTE *) &serviceStatus,
            sizeof(SERVICE_STATUS_PROCESS),
            &bytesNeeded))
        {
            perror("QueryServiceStatusEx");
            break;
        }

        if (serviceStatus.dwCheckPoint > oldCheckPoint)
        {
            // Continue to wait and check.
#pragma prefast(suppress:28159, "This routine will not run for longer than 10 seconds")
            startTickCount = GetTickCount();
            oldCheckPoint = serviceStatus.dwCheckPoint;
        }
        else
        {
#pragma prefast(suppress:28159, "This routine will not run for longer than 10 seconds")
            if (GetTickCount() - startTickCount > serviceStatus.dwWaitHint)
            {
                // No progress made within the wait hint.
                break;
            }
        }
    }

    *currentState = serviceStatus.dwCurrentState;
    *exitCode = serviceStatus.dwWin32ExitCode;

    LogVerbose("success");

    return ERROR_SUCCESS;
}

static ULONG ChangeServiceState(IN DWORD wantedState, IN HANDLE service, OUT DWORD *currentState, OUT DWORD *exitCode, OUT BOOL *nothingToDo OPTIONAL)
{
    SERVICE_STATUS status;
    SERVICE_STATUS_PROCESS statusProcess;
    DWORD bytesNeeded;
    DWORD pendingState;

    LogVerbose("start");

    if (!currentState || !exitCode)
        return ERROR_INVALID_PARAMETER;

    if (SERVICE_RUNNING != wantedState && SERVICE_STOPPED != wantedState)
        return ERROR_INVALID_PARAMETER;

    if (!QueryServiceStatusEx(
        service, // handle to service
        SC_STATUS_PROCESS_INFO, // info level
        (LPBYTE) &statusProcess, // address of structure
        sizeof(SERVICE_STATUS_PROCESS), // size of structure
        &bytesNeeded))
    {
        return perror("QueryServiceStatusEx");
    }

    if (wantedState == statusProcess.dwCurrentState)
    {
        if (nothingToDo)
            *nothingToDo = TRUE;
        *currentState = statusProcess.dwCurrentState;
        *exitCode = statusProcess.dwWin32ExitCode;
        return ERROR_SUCCESS;
    }

    if (nothingToDo)
        *nothingToDo = TRUE;

    switch (wantedState)
    {
    case SERVICE_RUNNING:
        pendingState = SERVICE_START_PENDING;

        if (pendingState == statusProcess.dwCurrentState)
            LogWarning("Service is being started already...\n");
        else
        {
            LogInfo("Starting the service...\n");
            if (!StartService(service, 0, NULL))
            {
                return perror("StartService");
            }
        }
        break;

    case SERVICE_STOPPED:
        pendingState = SERVICE_STOP_PENDING;

        if (pendingState == statusProcess.dwCurrentState)
            LogWarning("ChangeServiceState(): Service is being stopped already...\n");
        else
        {
            LogInfo("Stopping the service...\n");
            if (!ControlService(service, SERVICE_CONTROL_STOP, &status))
            {
                return perror("ControlService");
            }
        }
        break;
    }

    LogVerbose("success");

    return WaitForService(pendingState, wantedState, service, currentState, exitCode);
}

ULONG InstallService(IN const WCHAR *executablePath, IN const WCHAR *serviceName)
{
    SC_HANDLE service;
    SC_HANDLE scm;
    ULONG status;

    LogVerbose("service '%s', path '%s'", serviceName, executablePath);

    if (!executablePath || !serviceName)
        return ERROR_INVALID_PARAMETER;

    scm = OpenSCManager(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!scm)
    {
        return perror("OpenSCManager");
    }

    service = CreateService(
        scm,
        serviceName,
        serviceName,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        executablePath,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL);

    if (!service)
    {
        status = GetLastError();
        perror2(status, "CreateService");
        CloseServiceHandle(scm);
        return status;
    }

    LogInfo("Service installed\n");

    CloseServiceHandle(service);
    CloseServiceHandle(scm);

    LogVerbose("success");
    
    return ERROR_SUCCESS;
}

ULONG UninstallService(IN const WCHAR *serviceName)
{
    SC_HANDLE service;
    SC_HANDLE scm;
    ULONG status;
    DWORD currentState;
    DWORD exitCode;
    BOOL nothingToDo;

    LogVerbose("service '%s'", serviceName);
    
    if (!serviceName)
        return ERROR_INVALID_PARAMETER;

    scm = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm)
    {
        return perror("OpenSCManager");
    }

    service = OpenService(scm, serviceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (!service)
    {
        status = perror("OpenService");
        CloseServiceHandle(scm);
        return status;
    }

    status = ChangeServiceState(SERVICE_STOPPED, service, &currentState, &exitCode, &nothingToDo);
    if (ERROR_SUCCESS != status)
        perror2(status, "ChangeServiceState");
    else
    {
        if (!nothingToDo)
        {
            if (SERVICE_STOPPED == currentState)
            {
                if (ERROR_SUCCESS != exitCode)
                    perror2(exitCode, "Service");
                else
                    LogInfo("Service stopped\n");
            }
            else
                LogWarning("Failed to stop the service, current state is %d\n", currentState);
        }
    }

    if (!DeleteService(service))
    {
        status = perror("DeleteService");

        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return status;
    }

    CloseServiceHandle(service);
    CloseServiceHandle(scm);

    LogInfo("Service uninstalled\n");
    return ERROR_SUCCESS;
}
