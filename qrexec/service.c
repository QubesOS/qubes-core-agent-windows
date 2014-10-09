#include "service.h"

HANDLE g_hStopServiceEvent;

static HANDLE g_hServiceThread;
static SERVICE_STATUS g_ServiceStatus;
static SERVICE_STATUS_HANDLE g_hServiceStatus;

extern ULONG WINAPI ServiceExecutionThread(void *pParam);
static void StopService();

ULONG UpdateServiceStatus(
    DWORD dwCurrentState,
    DWORD dwWin32ExitCode,
    DWORD dwServiceSpecificExitCode,
    DWORD dwWaitHint)
{
    ULONG uResult;
    static DWORD dwCheckPoint = 1;

    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwCurrentState = dwCurrentState;
    g_ServiceStatus.dwServiceSpecificExitCode = dwServiceSpecificExitCode;
    g_ServiceStatus.dwWaitHint = dwWaitHint;

    if (SERVICE_START_PENDING == dwCurrentState)
    {
        g_ServiceStatus.dwControlsAccepted = 0;
    }
    else
    {
        g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    }

    if ((SERVICE_RUNNING == dwCurrentState) || (SERVICE_STOPPED == dwCurrentState))
        g_ServiceStatus.dwCheckPoint = 0;
    else
        g_ServiceStatus.dwCheckPoint = dwCheckPoint++;

    if (0 == dwServiceSpecificExitCode)
    {
        g_ServiceStatus.dwWin32ExitCode = dwWin32ExitCode;
    }
    else
    {
        g_ServiceStatus.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
    }

    if (!SetServiceStatus(g_hServiceStatus, &g_ServiceStatus))
    {
        uResult = GetLastError();
        perror("SetServiceStatus");
        StopService();
        return uResult;
    }

    return ERROR_SUCCESS;
}

static void StopService()
{
    LogDebug("Signaling the stop event\n");

    SetEvent(g_hStopServiceEvent);
    WaitForSingleObject(g_hServiceThread, INFINITE);

    if (SERVICE_STOPPED != g_ServiceStatus.dwCurrentState)
        UpdateServiceStatus(SERVICE_STOPPED, NO_ERROR, 0, 0);
}

static void WINAPI ServiceCtrlHandler(ULONG uControlCode)
{
    switch (uControlCode)
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
}

void WINAPI ServiceMain(DWORD dwArgc, LPTSTR *lpszArgv)
{
    ULONG uResult;

    // Manual reset, initial state is not signaled
    g_hStopServiceEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!g_hStopServiceEvent)
    {
        perror("CreateEvent");
        return;
    }

    g_hServiceStatus = RegisterServiceCtrlHandler(SERVICE_NAME, (LPHANDLER_FUNCTION) ServiceCtrlHandler);
    if (!g_hServiceStatus)
    {
        perror("RegisterServiceCtrlHandler");
        CloseHandle(g_hStopServiceEvent);
        return;
    }

    uResult = UpdateServiceStatus(SERVICE_START_PENDING, NO_ERROR, 0, 500);
    if (ERROR_SUCCESS != uResult)
    {
        perror2(uResult, "UpdateServiceStatus");
        CloseHandle(g_hStopServiceEvent);
        return;
    }

    uResult = Init(&g_hServiceThread);
    if (ERROR_SUCCESS != uResult)
    {
        CloseHandle(g_hStopServiceEvent);
        UpdateServiceStatus(SERVICE_STOPPED, uResult, 0, 0);
        perror2(uResult, "Init");
        return;
    }

    UpdateServiceStatus(SERVICE_RUNNING, NO_ERROR, 0, 0);

    WaitForSingleObject(g_hServiceThread, INFINITE);
    UpdateServiceStatus(SERVICE_STOPPED, NO_ERROR, 0, 0);

    CloseHandle(g_hServiceThread);
    CloseHandle(g_hStopServiceEvent);
}

ULONG WaitForService(DWORD dwPendingState, DWORD dwWantedState, HANDLE hService, DWORD *pdwCurrentState, DWORD *pdwExitCode)
{
    SERVICE_STATUS_PROCESS ServiceStatus;
    DWORD dwBytesNeeded;
    DWORD dwStartTickCount;
    DWORD dwOldCheckPoint;
    DWORD dwWaitTime;

    if (!pdwCurrentState || !pdwExitCode)
        return ERROR_INVALID_PARAMETER;

    // Check the status until the service is no longer start pending.

    if (!QueryServiceStatusEx(
        hService, // handle to service
        SC_STATUS_PROCESS_INFO, // info level
        (BYTE *) &ServiceStatus, // address of structure
        sizeof(SERVICE_STATUS_PROCESS), // size of structure
        &dwBytesNeeded))
    {
        return perror("QueryServiceStatusEx");
    }

    if (dwWantedState == ServiceStatus.dwCurrentState)
    {
        *pdwCurrentState = ServiceStatus.dwCurrentState;
        *pdwExitCode = ServiceStatus.dwWin32ExitCode;
        return ERROR_SUCCESS;
    }

    // Save the tick count and initial checkpoint.
#pragma prefast(suppress:28159, "This routine will not run for longer than 10 seconds")
    // omeg: ^ what if the machine is suspended mid-execution? :P
    dwStartTickCount = GetTickCount();
    dwOldCheckPoint = ServiceStatus.dwCheckPoint;

    while (dwPendingState == ServiceStatus.dwCurrentState)
    {
        // Do not wait longer than the wait hint. A good interval is
        // one-tenth the wait hint, but no less than 1 second and no
        // more than 10 seconds.

        dwWaitTime = ServiceStatus.dwWaitHint / 10;

        if (dwWaitTime < 1000)
            dwWaitTime = 1000;
        else
            if (dwWaitTime > 10000)
                dwWaitTime = 10000;

        Sleep(dwWaitTime);

        // Check the status again.

        if (!QueryServiceStatusEx(
            hService,
            SC_STATUS_PROCESS_INFO,
            (BYTE *) &ServiceStatus,
            sizeof(SERVICE_STATUS_PROCESS),
            &dwBytesNeeded))
        {
            perror("QueryServiceStatusEx");
            break;
        }

        if (ServiceStatus.dwCheckPoint > dwOldCheckPoint)
        {
            // Continue to wait and check.
#pragma prefast(suppress:28159, "This routine will not run for longer than 10 seconds")
            dwStartTickCount = GetTickCount();
            dwOldCheckPoint = ServiceStatus.dwCheckPoint;
        }
        else
        {
#pragma prefast(suppress:28159, "This routine will not run for longer than 10 seconds")
            if (GetTickCount() - dwStartTickCount > ServiceStatus.dwWaitHint)
            {
                // No progress made within the wait hint.
                break;
            }
        }
    }

    *pdwCurrentState = ServiceStatus.dwCurrentState;
    *pdwExitCode = ServiceStatus.dwWin32ExitCode;

    return ERROR_SUCCESS;
}

ULONG ChangeServiceState(DWORD dwWantedState, HANDLE hService, DWORD *pdwCurrentState, DWORD *pdwExitCode, BOOLEAN *pbNothingToDo)
{
    SERVICE_STATUS Status;
    SERVICE_STATUS_PROCESS ServiceStatus;
    DWORD dwBytesNeeded;
    DWORD dwPendingState;

    if (!pdwCurrentState || !pdwExitCode)
        return ERROR_INVALID_PARAMETER;

    if (SERVICE_RUNNING != dwWantedState && SERVICE_STOPPED != dwWantedState)
        return ERROR_INVALID_PARAMETER;

    if (!QueryServiceStatusEx(
        hService, // handle to service
        SC_STATUS_PROCESS_INFO, // info level
        (LPBYTE) &ServiceStatus, // address of structure
        sizeof(SERVICE_STATUS_PROCESS), // size of structure
        &dwBytesNeeded))
    {
        return perror("QueryServiceStatusEx");
    }

    if (dwWantedState == ServiceStatus.dwCurrentState)
    {
        if (pbNothingToDo)
            *pbNothingToDo = TRUE;
        *pdwCurrentState = ServiceStatus.dwCurrentState;
        *pdwExitCode = ServiceStatus.dwWin32ExitCode;
        return ERROR_SUCCESS;
    }

    if (pbNothingToDo)
        *pbNothingToDo = TRUE;

    switch (dwWantedState)
    {
    case SERVICE_RUNNING:
        dwPendingState = SERVICE_START_PENDING;

        if (dwPendingState == ServiceStatus.dwCurrentState)
            LogWarning("Service is being started already...\n");
        else
        {
            LogInfo("Starting the service...\n");
            if (!StartService(hService, 0, NULL))
            {
                return perror("StartService");
            }
        }
        break;
    case SERVICE_STOPPED:
        dwPendingState = SERVICE_STOP_PENDING;

        if (dwPendingState == ServiceStatus.dwCurrentState)
            LogWarning("ChangeServiceState(): Service is being stopped already...\n");
        else
        {
            LogInfo("Stopping the service...\n");
            if (!ControlService(hService, SERVICE_CONTROL_STOP, &Status))
            {
                return perror("ControlService");
            }
        }
        break;
    }

    return WaitForService(dwPendingState, dwWantedState, hService, pdwCurrentState, pdwExitCode);
}

ULONG InstallService(TCHAR *pszServiceFileName, TCHAR *pszServiceName)
{
    SC_HANDLE hService;
    SC_HANDLE hScm;
    ULONG uResult;

    if (!pszServiceFileName || !pszServiceName)
        return ERROR_INVALID_PARAMETER;

    hScm = OpenSCManager(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!hScm)
    {
        return perror("OpenSCManager");
    }

    hService = CreateService(
        hScm,
        pszServiceName,
        pszServiceName,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        pszServiceFileName,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL);

    if (!hService)
    {
        uResult = GetLastError();
        perror2(uResult, "CreateService");
        CloseServiceHandle(hScm);
        return uResult;
    }

    LogInfo("Service installed\n");

#ifdef START_SERVICE_AFTER_INSTALLATION
    uResult = ChangeServiceState(SERVICE_RUNNING, hService, &dwCurrentState, &dwExitCode, NULL);
    if (ERROR_SUCCESS != uResult) {
        perror2(uResult, "ChangeServiceState");
        CloseServiceHandle(hService);
        CloseServiceHandle(hScm);
        return uResult;
    }

    if (SERVICE_RUNNING != dwCurrentState) {
        if (SERVICE_STOPPED == dwCurrentState)
            perror2(dwExitCode, "Service start failed");
        else
            LogWarning("Service is not running, current state is %d\n", dwCurrentState);
}
    else
        LogInfo("Service is running\n");
#endif

    CloseServiceHandle(hService);
    CloseServiceHandle(hScm);

    return ERROR_SUCCESS;
}

ULONG UninstallService(TCHAR *pszServiceName)
{
    SC_HANDLE hService;
    SC_HANDLE hScm;
    ULONG uResult;
    DWORD dwCurrentState;
    DWORD dwExitCode;
    BOOLEAN bNothingToDo;

    if (!pszServiceName)
        return ERROR_INVALID_PARAMETER;

    hScm = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!hScm)
    {
        return perror("OpenSCManager");
    }

    hService = OpenService(hScm, pszServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (!hService)
    {
        uResult = GetLastError();
        perror2(uResult, "OpenService");
        CloseServiceHandle(hScm);
        return uResult;
    }

    uResult = ChangeServiceState(SERVICE_STOPPED, hService, &dwCurrentState, &dwExitCode, &bNothingToDo);
    if (ERROR_SUCCESS != uResult)
        perror2(uResult, "ChangeServiceState");
    else
    {
        if (!bNothingToDo)
        {
            if (SERVICE_STOPPED == dwCurrentState)
            {
                if (ERROR_SUCCESS != dwExitCode)
                    perror2(dwExitCode, "Service");
                else
                    LogInfo("Service stopped\n");
            }
            else
                LogWarning("Failed to stop the service, current state is %d\n", dwCurrentState);
        }
    }

    if (!DeleteService(hService))
    {
        uResult = GetLastError();
        perror2(uResult, "DeleteService");

        CloseServiceHandle(hService);
        CloseServiceHandle(hScm);
        return uResult;
    }

    CloseServiceHandle(hService);
    CloseServiceHandle(hScm);

    LogInfo("Service uninstalled\n");
    return ERROR_SUCCESS;
}
