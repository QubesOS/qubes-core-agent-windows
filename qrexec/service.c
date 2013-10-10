#include "service.h"

HANDLE	g_hStopServiceEvent;

static HANDLE	g_hServiceThread;
static SERVICE_STATUS	g_ServiceStatus;
static SERVICE_STATUS_HANDLE	g_hServiceStatus; 

extern ULONG WINAPI ServiceExecutionThread(PVOID pParam);
static VOID StopService();

ULONG UpdateServiceStatus(DWORD dwCurrentState,
						 DWORD dwWin32ExitCode,
						 DWORD dwServiceSpecificExitCode,
						 DWORD dwWaitHint)
{
	ULONG	uResult;
	static DWORD	dwCheckPoint = 1;

	g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
	g_ServiceStatus.dwCurrentState = dwCurrentState;
	g_ServiceStatus.dwServiceSpecificExitCode = dwServiceSpecificExitCode;
	g_ServiceStatus.dwWaitHint = dwWaitHint;

	if (SERVICE_START_PENDING == dwCurrentState) {
		g_ServiceStatus.dwControlsAccepted = 0;
	} else {
		g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
	} 

	if ((SERVICE_RUNNING == dwCurrentState) || (SERVICE_STOPPED == dwCurrentState))
		g_ServiceStatus.dwCheckPoint = 0;
	else
		g_ServiceStatus.dwCheckPoint = dwCheckPoint++;

	if (0 == dwServiceSpecificExitCode) {
		g_ServiceStatus.dwWin32ExitCode = dwWin32ExitCode;
	} else {
		g_ServiceStatus.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
	}

	if (!SetServiceStatus(g_hServiceStatus, &g_ServiceStatus)) {
		uResult = GetLastError();
		perror("UpdateServiceStatus(): SetServiceStatus()");
		StopService();
		return uResult;
	}

	return ERROR_SUCCESS;
}

static VOID StopService()
{
	debugf("StopService(): Signaling the stop event\n");

	SetEvent(g_hStopServiceEvent);
	WaitForSingleObject(g_hServiceThread, INFINITE);

	if (SERVICE_STOPPED != g_ServiceStatus.dwCurrentState)
		UpdateServiceStatus(SERVICE_STOPPED, NO_ERROR, 0, 0);
}

static VOID WINAPI ServiceCtrlHandler(ULONG uControlCode)
{
	switch(uControlCode) {	
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

VOID WINAPI ServiceMain(DWORD dwArgc, LPTSTR *lpszArgv)
{
	ULONG	uResult;

	// Manual reset, initial state is not signaled
	g_hStopServiceEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!g_hStopServiceEvent) {
		perror("ServiceMain(): CreateEvent()");
		return;
	}

	g_hServiceStatus = RegisterServiceCtrlHandler(SERVICE_NAME, (LPHANDLER_FUNCTION)ServiceCtrlHandler);
	if (!g_hServiceStatus) {
		perror("ServiceMain(): RegisterServiceCtrlHandler()");
		CloseHandle(g_hStopServiceEvent);
		return;
	}

	uResult = UpdateServiceStatus(SERVICE_START_PENDING, NO_ERROR, 0, 500);
	if (ERROR_SUCCESS != uResult) {
		perror("ServiceMain(): UpdateServiceStatus()");
		CloseHandle(g_hStopServiceEvent);
		return;
	}

	uResult = Init(&g_hServiceThread);
	if (ERROR_SUCCESS != uResult) {
		perror("ServiceMain(): Init()");
		CloseHandle(g_hStopServiceEvent);
		UpdateServiceStatus(SERVICE_STOPPED, uResult, 0, 0);
		return;
	}

	UpdateServiceStatus(SERVICE_RUNNING, NO_ERROR, 0, 0);

	WaitForSingleObject(g_hServiceThread, INFINITE);
	UpdateServiceStatus(SERVICE_STOPPED, NO_ERROR, 0, 0);

	CloseHandle(g_hServiceThread);
	CloseHandle(g_hStopServiceEvent);
}

ULONG WaitForService(DWORD dwPendingState, DWORD dwWantedState, HANDLE hService, PDWORD pdwCurrentState, PDWORD pdwExitCode)
{
	SERVICE_STATUS_PROCESS ServiceStatus;
	DWORD	dwBytesNeeded;
	ULONG	uResult;
	DWORD	dwStartTickCount;
	DWORD	dwOldCheckPoint;
	DWORD	dwWaitTime;

	if (!pdwCurrentState || !pdwExitCode)
		return ERROR_INVALID_PARAMETER;

	// Check the status until the service is no longer start pending. 
 
	if (!QueryServiceStatusEx( 
		hService, // handle to service 
		SC_STATUS_PROCESS_INFO, // info level
		(LPBYTE) &ServiceStatus, // address of structure
		sizeof(SERVICE_STATUS_PROCESS), // size of structure
		&dwBytesNeeded)) {

		uResult = GetLastError();
		perror("WaitForService(): QueryServiceStatusEx()");
		return uResult;
	}

	if (dwWantedState == ServiceStatus.dwCurrentState) {
		*pdwCurrentState = ServiceStatus.dwCurrentState;
		*pdwExitCode = ServiceStatus.dwWin32ExitCode;
		return ERROR_SUCCESS;
	}
 
	// Save the tick count and initial checkpoint.
//#pragma prefast(suppress:28159, "This routine will not run for longer than 10 seconds")
	dwStartTickCount = GetTickCount();
	dwOldCheckPoint = ServiceStatus.dwCheckPoint;

	while (dwPendingState == ServiceStatus.dwCurrentState) { 
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
			(LPBYTE)&ServiceStatus,
			sizeof(SERVICE_STATUS_PROCESS),
			&dwBytesNeeded)) {

			perror("WaitForService(): QueryServiceStatusEx()");
			break;
		}
 
		if (ServiceStatus.dwCheckPoint > dwOldCheckPoint) {
			// Continue to wait and check.
//#pragma prefast(suppress:28159, "This routine will not run for longer than 10 seconds")
			dwStartTickCount = GetTickCount();
			dwOldCheckPoint = ServiceStatus.dwCheckPoint;
		} else {
//#pragma prefast(suppress:28159, "This routine will not run for longer than 10 seconds")
			if (GetTickCount() - dwStartTickCount > ServiceStatus.dwWaitHint) {
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
	ULONG	uResult;
	SERVICE_STATUS	Status;
	SERVICE_STATUS_PROCESS ServiceStatus;
	DWORD	dwBytesNeeded;
	DWORD	dwPendingState;

	if (!pdwCurrentState || !pdwExitCode)
		return ERROR_INVALID_PARAMETER;

	if (SERVICE_RUNNING != dwWantedState && SERVICE_STOPPED != dwWantedState)
		return ERROR_INVALID_PARAMETER;

	if (!QueryServiceStatusEx( 
		hService, // handle to service 
		SC_STATUS_PROCESS_INFO, // info level
		(LPBYTE) &ServiceStatus, // address of structure
		sizeof(SERVICE_STATUS_PROCESS), // size of structure
		&dwBytesNeeded)) {

		uResult = GetLastError();
		perror("ChangeServiceState(): QueryServiceStatusEx()");
		return uResult;
	}

	if (dwWantedState == ServiceStatus.dwCurrentState) {
		if (pbNothingToDo)
			*pbNothingToDo = TRUE;
		*pdwCurrentState = ServiceStatus.dwCurrentState;
		*pdwExitCode = ServiceStatus.dwWin32ExitCode;
		return ERROR_SUCCESS;
	}

	if (pbNothingToDo)
		*pbNothingToDo = TRUE;

	switch (dwWantedState) {
	case SERVICE_RUNNING:
		dwPendingState = SERVICE_START_PENDING;

		if (dwPendingState == ServiceStatus.dwCurrentState)
			debugf("ChangeServiceState(): Service is being started already...\n");
		else {
			debugf("ChangeServiceState(): Starting the service...\n");
			if (!StartService(hService, 0, NULL)) {
				uResult = GetLastError();
				perror("ChangeServiceState(): StartService()");
				return uResult;
			}
		}
		break;

	case SERVICE_STOPPED:
		dwPendingState = SERVICE_STOP_PENDING;

		if (dwPendingState == ServiceStatus.dwCurrentState)
			debugf("ChangeServiceState(): Service is being stopped already...\n");
		else {
			debugf("ChangeServiceState(): Stopping the service...\n");
			if (!ControlService(hService, SERVICE_CONTROL_STOP, &Status)) {
				uResult = GetLastError();
				perror("ChangeServiceState(): ControlService()");
				return uResult;
			}
		}
		break;
	}

	return WaitForService(dwPendingState, dwWantedState, hService, pdwCurrentState, pdwExitCode);
}

ULONG CreateApplicationEventSource(PTCHAR pszEventSourceName, PTCHAR pszPathToModule)
{
	TCHAR	szEventSourceKey[255];
	HRESULT	hResult;
	ULONG	uResult;
	HKEY	hKey;
	size_t	cbSize;
	DWORD	dwTypesSupported;

	if (!pszEventSourceName || !pszPathToModule)
		return ERROR_INVALID_PARAMETER;

	hResult = StringCbLength(pszPathToModule, STRSAFE_MAX_CCH * sizeof(TCHAR), &cbSize);
	if (FAILED(hResult)) {
		perror("CreateApplicationEventSource(): StringCbLength()");
		return hResult;
	}

	// RegSetValueEx must receive cbSize (data size in bytes) which includes the size of a terminating null character.
	cbSize += sizeof(TCHAR);

	hResult = StringCchPrintf(
			szEventSourceKey, 
			RTL_NUMBER_OF(szEventSourceKey), 
			TEXT("SYSTEM\\CurrentControlSet\\Services\\Eventlog\\Application\\%s"),
			pszEventSourceName);

	if (FAILED(hResult)) {
		perror("CreateApplicationEventSource(): StringCchPrintf()");
		return hResult;
	}

	uResult = RegCreateKeyEx(
			HKEY_LOCAL_MACHINE,
			szEventSourceKey,
			0,
			NULL,
			REG_OPTION_NON_VOLATILE,
			KEY_ALL_ACCESS,
			NULL,
			&hKey,
			NULL);

	if (ERROR_SUCCESS != uResult) {
		perror("CreateApplicationEventSource(): RegCreateKeyEx()");
		return uResult;
	}

	uResult = RegSetValueEx(
			hKey,
			TEXT("EventMessageFile"),
			0,
			REG_SZ,
			(LPBYTE)pszPathToModule,
			cbSize);
	if (ERROR_SUCCESS != uResult) {
		perror("CreateApplicationEventSource(): RegSetValueEx(\"EventMessageFile\")");
		RegDeleteKey(hKey, NULL);
		return uResult;
	}

	dwTypesSupported = EVENTLOG_ERROR_TYPE;

	uResult = RegSetValueEx(
			hKey,
			TEXT("TypesSupported"),
			0,
			REG_DWORD,
			(LPBYTE)&dwTypesSupported,
			sizeof(dwTypesSupported));
	if (ERROR_SUCCESS != uResult) {
		perror("CreateApplicationEventSource(): RegSetValueEx(\"TypesSupported\")");
		RegDeleteKey(hKey, NULL);
		return uResult;
	}

	RegCloseKey(hKey);
	return ERROR_SUCCESS;
}

ULONG DeleteApplicationEventSource(PTCHAR pszEventSourceName)
{
	TCHAR	szEventSourceKey[255];
	HRESULT	hResult;
	ULONG	uResult;

	if (!pszEventSourceName)
		return ERROR_INVALID_PARAMETER;

	hResult = StringCchPrintf(
			szEventSourceKey, 
			RTL_NUMBER_OF(szEventSourceKey), 
			TEXT("SYSTEM\\CurrentControlSet\\Services\\Eventlog\\Application\\%s"),
			pszEventSourceName);

	if (FAILED(hResult)) {
		perror("DeleteApplicationEventSource(): StringCchPrintf()");
		return hResult;
	}

	uResult = RegDeleteKey(HKEY_LOCAL_MACHINE, szEventSourceKey);
	if (ERROR_SUCCESS != uResult) {
		perror("DeleteApplicationEventSource(): RegDeleteKey()");
		return uResult;
	}

	return ERROR_SUCCESS;
}

ULONG InstallService(PTCHAR pszServiceFileName, PTCHAR pszServiceName)
{
	SC_HANDLE	hService;
	SC_HANDLE	hScm;
	ULONG	uResult;
#ifdef START_SERVICE_AFTER_INSTALLATION
	DWORD	dwCurrentState;
	DWORD	dwExitCode;
#endif

	if (!pszServiceFileName || !pszServiceName)
		return ERROR_INVALID_PARAMETER;

	hScm = OpenSCManager(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
	if (!hScm) {
		uResult = GetLastError();
		perror("InstallService(): OpenSCManager()");
		return uResult;
	}

	hService = CreateService(hScm,
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
	if (!hService) {
		uResult = GetLastError();
		perror("InstallService(): CreateService()");
		CloseServiceHandle(hScm);
		return uResult;
	}

	debugf("InstallService(): Service installed\n");

	uResult = CreateApplicationEventSource(pszServiceName, pszServiceFileName);
	if (ERROR_SUCCESS != uResult)
		perror("InstallService(): CreateApplicationEventSource()");


#ifdef START_SERVICE_AFTER_INSTALLATION
	uResult = ChangeServiceState(SERVICE_RUNNING, hService, &dwCurrentState, &dwExitCode, NULL);
	if (ERROR_SUCCESS != uResult) {
		perror("InstallService(): ChangeServiceState()");
		CloseServiceHandle(hService);
		CloseServiceHandle(hScm);
		return uResult;
	}

	if (SERVICE_RUNNING != dwCurrentState) {
		if (SERVICE_STOPPED == dwCurrentState)
			perror("InstallService(): Service start failed");
		else
			debugf("InstallService(): Service is not running, current state is %d\n", dwCurrentState);
	} else
		debugf("InstallService(): Service is running\n");
#endif

	CloseServiceHandle(hService);
	CloseServiceHandle(hScm);

	return ERROR_SUCCESS;
}

ULONG UninstallService(PTCHAR pszServiceName)
{
	SC_HANDLE	hService;
	SC_HANDLE	hScm;
	ULONG	uResult;
	DWORD	dwCurrentState;
	DWORD	dwExitCode;
	BOOLEAN	bNothingToDo;

	if (!pszServiceName)
		return ERROR_INVALID_PARAMETER;

	hScm = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (!hScm) {
		uResult = GetLastError();
		perror("UninstallService(): OpenSCManager()");
		return uResult;
	}

	hService = OpenService(hScm, pszServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
	if (!hService) {
		uResult = GetLastError();
		perror("UninstallService(): OpenService()");
		CloseServiceHandle(hScm);
		return uResult;
	}

	uResult = ChangeServiceState(SERVICE_STOPPED, hService, &dwCurrentState, &dwExitCode, &bNothingToDo);
	if (ERROR_SUCCESS != uResult)
		perror("UninstallService(): ChangeServiceState()");
	else {
		if (!bNothingToDo) {
			if (SERVICE_STOPPED == dwCurrentState) {
				if (ERROR_SUCCESS != dwExitCode)
					perror("UninstallService(): Service");
				else
					debugf("UninstallService(): Service stopped\n");
			} else
				debugf("UninstallService(): Failed to stop the service, current state is %d\n", dwCurrentState);
		}
	}

	if (!DeleteService(hService)) {
		uResult = GetLastError();
		perror("UninstallService(): DeleteService()");

		CloseServiceHandle(hService);
		CloseServiceHandle(hScm);
		return uResult;
	}

	uResult = DeleteApplicationEventSource(pszServiceName);
	if (ERROR_SUCCESS != uResult)
		perror("UninstallService(): DeleteApplicationEventSource()");

	CloseServiceHandle(hService);
	CloseServiceHandle(hScm);

	debugf("UninstallService(): Service uninstalled\n");
	return ERROR_SUCCESS;
}

ULONG ReportErrorToEventLog(ULONG uErrorMessageId) 
{ 
	ULONG	uResult;
	HANDLE	hEventSource;
	LPCTSTR	lpszStrings[1];

//#pragma prefast(suppress:28735, "This way we support XP+")
	hEventSource = RegisterEventSource(NULL, SERVICE_NAME);
	if (NULL == hEventSource) {
		uResult = GetLastError();
		perror("ReportErrorToEventLog(): RegisterEventSource()");
		return uResult;
	}

	lpszStrings[0] = SERVICE_NAME;

	if (!ReportEvent(hEventSource, // event log handle
			EVENTLOG_ERROR_TYPE, // event type
			0, // event category
			uErrorMessageId, // event identifier
			NULL, // no security identifier
			1, // size of lpszStrings array
			0, // no binary data
			lpszStrings, // array of strings
			NULL)) { // no binary data

		uResult = GetLastError();
		perror("ReportErrorToEventLog(): ReportEvent()");
		DeregisterEventSource(hEventSource);
		return uResult;
	}

	DeregisterEventSource(hEventSource);
	return ERROR_SUCCESS;
}
