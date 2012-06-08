#include "exec.h"


ULONG GetAccountSid(LPCTSTR pszAccountName, LPCTSTR pszSystemName, PSID *ppSid)
{
	SID_NAME_USE	sidUsage;
	DWORD	cbSid;
	DWORD	cchReferencedDomainName = MAX_PATH;
	TCHAR	ReferencedDomainName[MAX_PATH];
	ULONG	uResult;
	PSID	pSid;


	if (!pszAccountName || !ppSid)
		return ERROR_INVALID_PARAMETER;

	cbSid = 0;
	*ppSid = NULL;

	if (!LookupAccountName(
		pszSystemName,
		pszAccountName,
		NULL,
		&cbSid,
		ReferencedDomainName,
		&cchReferencedDomainName,
		&sidUsage)) {

		uResult = GetLastError();
		if (ERROR_INSUFFICIENT_BUFFER != uResult) {
			lprintf_err(uResult, "GetAccountSid(): LookupAccountName()");
			return uResult;
		}
	}

	pSid = LocalAlloc(LPTR, cbSid);
	if (!pSid) {
		uResult = GetLastError();
		lprintf_err(uResult, "GetAccountSid(): LocalAlloc()");
		return uResult;
	}


	if (!LookupAccountName(
		pszSystemName,
		pszAccountName,
		pSid,
		&cbSid,
		ReferencedDomainName, 
		&cchReferencedDomainName,
		&sidUsage)) {

		uResult = GetLastError();
		LocalFree(pSid);
		lprintf_err(uResult, "GetAccountSid(): LookupAccountName()");
		return uResult;
	}

	*ppSid = pSid;

	return ERROR_SUCCESS;
}


ULONG GetObjectSecurityDescriptorDacl(HANDLE hObject, PSECURITY_DESCRIPTOR *ppSD, PBOOL pbDaclPresent, PACL *ppDacl)
{
	ULONG	uResult;
	SECURITY_INFORMATION	SIRequested;
	PSECURITY_DESCRIPTOR	pSD = NULL;
	DWORD	nLength;
	DWORD	nLengthNeeded;
	BOOL	bDaclPresent = FALSE;
	PACL	pDacl = NULL;
	BOOL	bDaclDefaulted;


	if (!ppSD || !pbDaclPresent || !ppDacl)
		return ERROR_INVALID_PARAMETER;


	*ppSD = NULL;
	*ppDacl = NULL;
	*pbDaclPresent = FALSE;

	SIRequested = DACL_SECURITY_INFORMATION;
	nLength = 0;

	if (!GetUserObjectSecurity(
		hObject,
		&SIRequested,
		NULL,
		0,
		&nLengthNeeded)) {

		uResult = GetLastError();
		if (ERROR_INSUFFICIENT_BUFFER != uResult) {
			lprintf_err(uResult, "GetObjectSecurityDescriptorDacl(): GetUserObjectSecurity()");
			return uResult;
		}
	}

	pSD = LocalAlloc(LPTR, nLengthNeeded);
	if (!pSD) {
		uResult = GetLastError();
		lprintf_err(uResult, "GetObjectSecurityDescriptorDacl(): LocalAlloc()");
		return uResult;
	}

	if (!GetUserObjectSecurity(
		hObject,
		&SIRequested,
		pSD,
		nLengthNeeded,
		&nLengthNeeded)) {

		uResult = GetLastError();
		lprintf_err(uResult, "GetObjectSecurityDescriptorDacl(): GetUserObjectSecurity()");
		return uResult;
	}


	if (!GetSecurityDescriptorDacl(pSD, &bDaclPresent, &pDacl, &bDaclDefaulted)) {
		uResult = GetLastError();
		LocalFree(pSD);
		lprintf_err(uResult, "GetObjectSecurityDescriptorDacl(): GetSecurityDescriptorDacl()");
		return uResult;
	}


	*ppSD = pSD;
	*pbDaclPresent = bDaclPresent;
	*ppDacl = pDacl;

	return ERROR_SUCCESS;
}


ULONG MergeWithExistingDacl(HANDLE hObject, ULONG cCountOfExplicitEntries, PEXPLICIT_ACCESS pListOfExplicitEntries)
{
	ULONG	uResult;
	PSECURITY_DESCRIPTOR	pSD = NULL;
	BOOL	bDaclPresent = FALSE;
	PACL	pDacl = NULL;
	PACL	pNewAcl = NULL;
	SECURITY_INFORMATION	SIRequested = DACL_SECURITY_INFORMATION;


	if (!cCountOfExplicitEntries || !pListOfExplicitEntries)
		return ERROR_INVALID_PARAMETER;


	uResult = GetObjectSecurityDescriptorDacl(hObject, &pSD, &bDaclPresent, &pDacl);
	if (ERROR_SUCCESS != uResult) {
		lprintf_err(uResult, "MergeWithExistingDacl(): GetObjectSecurityDescriptorDacl()");
		return uResult;
	}

	uResult = SetEntriesInAcl(cCountOfExplicitEntries, pListOfExplicitEntries, pDacl, &pNewAcl);
	LocalFree(pSD);

	if (ERROR_SUCCESS != uResult) {
		lprintf_err(uResult, "MergeWithExistingDacl(): SetEntriesInAcl()");
		return uResult;
	}

	pSD = LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
	if (!pSD) {
		uResult = GetLastError();
		LocalFree(pNewAcl);
		lprintf_err(uResult, "MergeWithExistingDacl(): LocalAlloc()");
		return uResult;
	}

	if (!InitializeSecurityDescriptor(pSD, SECURITY_DESCRIPTOR_REVISION)) {
		uResult = GetLastError();
		LocalFree(pNewAcl);
		LocalFree(pSD);
		lprintf_err(uResult, "MergeWithExistingDacl(): InitializeSecurityDescriptor()");
		return uResult;
	}

	if (!SetSecurityDescriptorDacl(pSD, TRUE, pNewAcl, FALSE)) {
		uResult = GetLastError();
		LocalFree(pNewAcl);
		LocalFree(pSD);
		lprintf_err(uResult, "MergeWithExistingDacl(): SetSecurityDescriptorDacl()");
		return uResult;
	}


	if (!SetUserObjectSecurity(hObject, &SIRequested, pSD)) {
		uResult = GetLastError();
		LocalFree(pNewAcl);
		LocalFree(pSD);
		lprintf_err(uResult, "MergeWithExistingDacl(): SetUserObjectSecurity()");
		return uResult;
	}

	LocalFree(pNewAcl);
	LocalFree(pSD);

	return ERROR_SUCCESS;
}


ULONG GrantDesktopAccess(LPCTSTR pszAccountName, LPCTSTR pszSystemName)
{
	HANDLE	hWindowStation;
	HANDLE	hOriginalWindowStation;
	HANDLE	hDesktop;
	ULONG	uResult;
	PSID	pSid = NULL;
	EXPLICIT_ACCESS NewAccessAllowedAces[2];


	if (!pszAccountName)
		return ERROR_INVALID_PARAMETER;

	hOriginalWindowStation = GetProcessWindowStation();
	if (!hOriginalWindowStation) {
		uResult = GetLastError();
		lprintf_err(uResult, "GrantDesktopAccess(): GetProcessWindowStation()");
		return uResult;
	}

	hWindowStation = OpenWindowStation(
				TEXT("Winsta0"),
				FALSE,
				READ_CONTROL | WRITE_DAC);
	if (!hWindowStation) {
		uResult = GetLastError();
		lprintf_err(uResult, "GrantDesktopAccess(): OpenWindowStation()");
		return uResult;
	}


	if (!SetProcessWindowStation(hWindowStation)) {
		uResult = GetLastError();
		CloseWindowStation(hWindowStation);
		lprintf_err(uResult, "GrantDesktopAccess(): SetProcessWindowStation()");
		return uResult;
	}

	hDesktop = OpenDesktop(
			TEXT("Default"),
			0,
			FALSE,
			READ_CONTROL | WRITE_DAC | DESKTOP_WRITEOBJECTS | DESKTOP_READOBJECTS);
	if (!hDesktop) {
		uResult = GetLastError();
		CloseWindowStation(hWindowStation);
		lprintf_err(uResult, "GrantDesktopAccess(): OpenDesktop()");
		return uResult;
	}

	if (!SetProcessWindowStation(hOriginalWindowStation)) {
		uResult = GetLastError();
		CloseDesktop(hDesktop);
		CloseWindowStation(hWindowStation);
		lprintf_err(uResult, "GrantDesktopAccess(): SetProcessWindowStation(Original)");
		return uResult;
	}


	uResult = GetAccountSid(pszAccountName, pszSystemName, &pSid);
	if (ERROR_SUCCESS != uResult) {
		CloseDesktop(hDesktop);
		CloseWindowStation(hWindowStation);
		lprintf_err(uResult, "GrantDesktopAccess(): GetAccountSid()");
		return uResult;
	}


	NewAccessAllowedAces[0].grfAccessPermissions = GENERIC_ACCESS;
	NewAccessAllowedAces[0].grfAccessMode = GRANT_ACCESS;
	NewAccessAllowedAces[0].grfInheritance = CONTAINER_INHERIT_ACE | INHERIT_ONLY_ACE | OBJECT_INHERIT_ACE;
	NewAccessAllowedAces[0].Trustee.MultipleTrusteeOperation = NO_MULTIPLE_TRUSTEE;
	NewAccessAllowedAces[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
	NewAccessAllowedAces[0].Trustee.TrusteeType = TRUSTEE_IS_USER;
	NewAccessAllowedAces[0].Trustee.ptstrName = pSid;

	NewAccessAllowedAces[1] = NewAccessAllowedAces[0];

	NewAccessAllowedAces[1].grfAccessPermissions = WINSTA_ALL;
	NewAccessAllowedAces[1].grfInheritance = NO_PROPAGATE_INHERIT_ACE;


	uResult = MergeWithExistingDacl(hWindowStation, 2, NewAccessAllowedAces);
	CloseWindowStation(hWindowStation);

	if (ERROR_SUCCESS != uResult) {
		CloseDesktop(hDesktop);		
		LocalFree(pSid);
		lprintf_err(uResult, "GrantDesktopAccess(): MergeWithExistingDacl(WindowStation)");
		return uResult;
	}

	NewAccessAllowedAces[0].grfAccessPermissions = DESKTOP_ALL;
	NewAccessAllowedAces[0].grfAccessMode = GRANT_ACCESS;
	NewAccessAllowedAces[0].grfInheritance = 0;

	uResult = MergeWithExistingDacl(hDesktop, 1, NewAccessAllowedAces);
	CloseDesktop(hDesktop);

	if (ERROR_SUCCESS != uResult) {
		LocalFree(pSid);
		lprintf_err(uResult, "GrantDesktopAccess(): MergeWithExistingDacl(Desktop)");
		return uResult;
	}


	LocalFree(pSid);
	return ERROR_SUCCESS;
}


// Open a window station and a desktop in another session, grant access to those handles
ULONG GrantRemoteSessionDesktopAccess(DWORD dwSessionId, LPCTSTR pszAccountName, LPCTSTR pszSystemName)
{
	ULONG	uResult;
	HRESULT	hResult;
	HANDLE	hToken;
	HANDLE	hTokenDuplicate;
	TCHAR	szFullPath[MAX_PATH + 1];
	TCHAR	szArguments[UNLEN + 1];
	PROCESS_INFORMATION	pi;
	STARTUPINFO	si;
	DWORD	dwCurrentSessionId;



	if (!pszAccountName)
		return ERROR_INVALID_PARAMETER;


	if (!ProcessIdToSessionId(GetCurrentProcessId(), &dwCurrentSessionId)) {
		uResult = GetLastError();
		lprintf_err(uResult, "GrantRemoteSessionDesktopAccess(): ProcessIdToSessionId()");
		return uResult;
	}

	if (dwCurrentSessionId == dwSessionId) {
		// We're in the same session, no need to run an additional process.
		lprintf("GrantRemoteSessionDesktopAccess(): Already running in the specified session\n");
		uResult = GrantDesktopAccess(pszAccountName, pszSystemName);
		if (ERROR_SUCCESS != uResult)
			lprintf_err(uResult, "GrantRemoteSessionDesktopAccess(): GrantDesktopAccess()");

		return uResult;
	}


	if (!OpenThreadToken(GetCurrentThread(), TOKEN_ALL_ACCESS, TRUE, &hToken)) {
//		uResult = GetLastError();
//		lprintf_err(uResult, "GrantRemoteSessionDesktopAccess(): OpenThreadToken()");

		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &hToken)) {
			uResult = GetLastError();
			lprintf_err(uResult, "GrantRemoteSessionDesktopAccess(): OpenProcessToken()");
			return uResult;
		}
	}

	if (!DuplicateTokenEx(
		hToken,
		MAXIMUM_ALLOWED,
		NULL,
		SecurityIdentification,
		TokenPrimary,
		&hTokenDuplicate)) {

		uResult = GetLastError();
		CloseHandle(hToken);
		lprintf_err(uResult, "GrantRemoteSessionDesktopAccess(): DuplicateTokenEx()");
		return uResult;
	}
	CloseHandle(hToken);

	hToken = hTokenDuplicate;


	if (!SetTokenInformation(hToken, TokenSessionId, &dwSessionId, sizeof(dwSessionId))) {
		uResult = GetLastError();

		CloseHandle(hToken);
		lprintf_err(uResult, "GrantRemoteSessionDesktopAccess(): SetTokenInformation()");
		return uResult;
	}


	memset(szFullPath, 0, sizeof(szFullPath));
	if (!GetModuleFileName(NULL, szFullPath, RTL_NUMBER_OF(szFullPath) - 1)) {
		uResult = GetLastError();
		CloseHandle(hToken);
		lprintf_err(uResult, "GrantRemoteSessionDesktopAccess(): GetModuleFileName()");
		return uResult;
	}

	hResult = StringCchPrintf(szArguments, RTL_NUMBER_OF(szArguments), TEXT("\"%s\" -a %s"), szFullPath, pszAccountName);
	if (FAILED(hResult)) {
		CloseHandle(hToken);
		lprintf_err(hResult, "GrantRemoteSessionDesktopAccess(): StringCchPrintf()");
		return hResult;
	}

	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);

	lprintf("GrantRemoteSessionDesktopAccess(): CreateProcessAsUser(\"%S\")\n", szArguments);

	if (!CreateProcessAsUser(
			hToken,
			szFullPath, 
			szArguments, 
			NULL, 
			NULL, 
			TRUE, // handles are inherited
			0, 
			NULL, 
			NULL, 
			&si, 
			&pi)) {

		uResult = GetLastError();
		CloseHandle(hToken);
		lprintf_err(uResult, "GrantRemoteSessionDesktopAccess(): CreateProcessAsUser(\"%S %S\")", szFullPath, szArguments);
		return uResult;
	}

	CloseHandle(pi.hThread);
	uResult = WaitForSingleObject(pi.hProcess, 1000);

	if (WAIT_OBJECT_0 != uResult) {
		if (WAIT_TIMEOUT == uResult) {
			uResult = ERROR_ACCESS_DENIED;
			lprintf("GrantRemoteSessionDesktopAccess(): WaitForSingleObject() timed out\n");
		} else {
			uResult = GetLastError();
			lprintf_err(uResult, "GrantRemoteSessionDesktopAccess(): WaitForSingleObject()");
		}

		CloseHandle(pi.hProcess);
		CloseHandle(hToken);
		return uResult;
	}

	CloseHandle(pi.hProcess);
	CloseHandle(hToken);
	return ERROR_SUCCESS;
}


ULONG CreatePipedProcessAsCurrentUserW(
		PWCHAR pwszCommand,
		BOOLEAN bRunInteractively,
		HANDLE hPipeStdin,
		HANDLE hPipeStdout,
		HANDLE hPipeStderr,
		HANDLE *phProcess)
{
	PROCESS_INFORMATION	pi;
	STARTUPINFOW	si;
	ULONG	uResult;
	BOOLEAN	bInheritHandles;


	if (!pwszCommand || !phProcess)
		return ERROR_INVALID_PARAMETER;

	*phProcess = INVALID_HANDLE_VALUE;

	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);

	bInheritHandles = FALSE;

	if (INVALID_HANDLE_VALUE != hPipeStdin &&
		INVALID_HANDLE_VALUE != hPipeStdout &&
		INVALID_HANDLE_VALUE != hPipeStderr) {

		si.dwFlags = STARTF_USESTDHANDLES;	
		si.hStdInput = hPipeStdin;
		si.hStdOutput = hPipeStdout;
		si.hStdError = hPipeStderr;

		bInheritHandles = TRUE;
	}

	if (!CreateProcessW(
			NULL, 
			pwszCommand, 
			NULL, 
			NULL, 
			bInheritHandles, // inherit handles if IO is piped
			0, 
			NULL, 
			NULL, 
			&si, 
			&pi)) {

		uResult = GetLastError();
		lprintf_err(uResult, "CreatePipedProcessAsCurrentUserW(): CreateProcessW(\"%S\")", pwszCommand);
		return uResult;
	}

	lprintf("CreatePipedProcessAsCurrentUserW(): pid %d\n", pi.dwProcessId);

	*phProcess = pi.hProcess;
	CloseHandle(pi.hThread);

	return ERROR_SUCCESS;
}



ULONG CreatePipedProcessAsUserW(
		PWCHAR pwszUserName,
		PWCHAR pwszUserPassword,
		PWCHAR pwszCommand,
		BOOLEAN bRunInteractively,
		HANDLE hPipeStdin,
		HANDLE hPipeStdout,
		HANDLE hPipeStderr,
		HANDLE *phProcess)
{
	DWORD	dwActiveSessionId;
	DWORD	dwCurrentSessionId;
	ULONG	uResult;
	HANDLE	hUserToken;
	HANDLE	hUserTokenDuplicate;
	PROCESS_INFORMATION	pi;
	STARTUPINFOW	si;
	PVOID	pEnvironment;
	WCHAR	wszLoggedUserName[UNLEN + 1];
	DWORD	nSize;
	PROFILEINFO	ProfileInfo;
	BOOLEAN	bInheritHandles;
	BOOLEAN	bUserIsLoggedOn;



	if (!pwszUserName || !pwszCommand || !phProcess)
		return ERROR_INVALID_PARAMETER;

	*phProcess = INVALID_HANDLE_VALUE;

	if (!ProcessIdToSessionId(GetCurrentProcessId(), &dwCurrentSessionId)) {
		uResult = GetLastError();
		lprintf_err(uResult, "CreatePipedProcessAsUserW(): ProcessIdToSessionId()");
		return uResult;
	}


	dwActiveSessionId = WTSGetActiveConsoleSessionId();
	if (0xFFFFFFFF == dwActiveSessionId) {
		uResult = GetLastError();

		// There is no clear indication in the manual that WTSGetActiveConsoleSessionId() sets the last error properly.
		if (ERROR_SUCCESS == uResult)
			uResult = ERROR_NOT_SUPPORTED;

		lprintf_err(uResult, "CreatePipedProcessAsUserW(): WTSGetActiveConsoleSessionId()");		
		return uResult;
	}

	if (!WTSQueryUserToken(dwActiveSessionId, &hUserToken)) {
		uResult = GetLastError();
		lprintf_err(uResult, "CreatePipedProcessAsUserW(): WTSQueryUserToken()");
		return uResult;
	}


	if (!DuplicateTokenEx(
		hUserToken,
		MAXIMUM_ALLOWED,
		NULL,
		SecurityIdentification,
		TokenPrimary,
		&hUserTokenDuplicate)) {

		uResult = GetLastError();
		CloseHandle(hUserToken);
		lprintf_err(uResult, "CreatePipedProcessAsUserW(): DuplicateTokenEx()");
		return uResult;
	}

	CloseHandle(hUserToken);

	hUserToken = hUserTokenDuplicate;

	// Check if the logged on user is the same as the user specified by pwszUserName -
	// in that case we won't need to do LogonUser()
	if (!ImpersonateLoggedOnUser(hUserToken)) {
		uResult = GetLastError();
		CloseHandle(hUserToken);
		lprintf_err(uResult, "CreatePipedProcessAsUserW(): ImpersonateLoggedOnUser()");
		return uResult;
	}

	nSize = RTL_NUMBER_OF(wszLoggedUserName);
	if (!GetUserNameW(wszLoggedUserName, &nSize)) {
		uResult = GetLastError();

		RevertToSelf();
		CloseHandle(hUserToken);
		lprintf_err(uResult, "CreatePipedProcessAsUserW(): GetUserName()");
		return uResult;
	}

	RevertToSelf();

	bUserIsLoggedOn = FALSE;
	if (wcscmp(wszLoggedUserName, pwszUserName)) {

		// Current user is not the one specified by pwszUserName. Log on the required user.

		CloseHandle(hUserToken);

		if (!LogonUserW(
			pwszUserName,
			L".",
			pwszUserPassword,
			LOGON32_LOGON_INTERACTIVE,
			LOGON32_PROVIDER_DEFAULT,
			&hUserToken)) {

			uResult = GetLastError();
			lprintf_err(uResult, "CreatePipedProcessAsUserW(): LogonUserW()");
			return uResult;
		}

/*
		memset(&ProfileInfo, 0, sizeof(ProfileInfo));
		ProfileInfo.dwSize = sizeof(ProfileInfo);
		ProfileInfo.lpUserName = pwszUserName;

		if (!LoadUserProfile(hUserToken, &ProfileInfo)) {
			uResult = GetLastError();
			lprintf_err(uResult, "CreatePipedProcessAsUserW(): LoadUserProfile()");
		}
*/
	} else
		bUserIsLoggedOn = TRUE;

	if (!bRunInteractively)
		dwActiveSessionId = dwCurrentSessionId;

	if (!(bUserIsLoggedOn && bRunInteractively)) {

		// Do not do this if the specified user is currently logged on and the process is run interactively
		// because the user already has all the access to the window station and desktop, and
		// we don't have to change the session.
		if (!SetTokenInformation(hUserToken, TokenSessionId, &dwActiveSessionId, sizeof(dwActiveSessionId))) {
			uResult = GetLastError();
			CloseHandle(hUserToken);
			lprintf_err(uResult, "CreatePipedProcessAsUserW(): SetTokenInformation()");
			return uResult;
		}

		uResult = GrantRemoteSessionDesktopAccess(dwActiveSessionId, pwszUserName, NULL);
		if (ERROR_SUCCESS != uResult)
			lprintf_err(uResult, "CreatePipedProcessAsUserW(): GrantRemoteSessionDesktopAccess()");
	}


	if (!CreateEnvironmentBlock(&pEnvironment, hUserToken, FALSE)) {
		uResult = GetLastError();		
		CloseHandle(hUserToken);
		lprintf_err(uResult, "CreatePipedProcessAsUserW(): CreateEnvironmentBlock()");
		return uResult;
	}


	if (!ImpersonateLoggedOnUser(hUserToken)) {
		uResult = GetLastError();
		CloseHandle(hUserToken);
		DestroyEnvironmentBlock(pEnvironment);
		lprintf_err(uResult, "CreatePipedProcessAsUserW(): ImpersonateLoggedOnUser()");
		return uResult;
	}

	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);

	si.lpDesktop = TEXT("Winsta0\\Default");

	bInheritHandles = FALSE;

	if (INVALID_HANDLE_VALUE != hPipeStdin &&
		INVALID_HANDLE_VALUE != hPipeStdout &&
		INVALID_HANDLE_VALUE != hPipeStderr) {

		si.dwFlags = STARTF_USESTDHANDLES;	
		si.hStdInput = hPipeStdin;
		si.hStdOutput = hPipeStdout;
		si.hStdError = hPipeStderr;

		bInheritHandles = TRUE;
	}

	if (!CreateProcessAsUserW(
			hUserToken,
			NULL,
			pwszCommand,
			NULL,
			NULL,
			bInheritHandles, // inherit handles if IO is piped
			NORMAL_PRIORITY_CLASS | CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT,
			pEnvironment,
			NULL,
			&si,
			&pi)) {

		uResult = GetLastError();
		RevertToSelf();
		CloseHandle(hUserToken);
		DestroyEnvironmentBlock(pEnvironment);
		lprintf_err(uResult, "CreatePipedProcessAsUserW(): CreateProcessAsUserW(\"%S\")", pwszCommand);
		return uResult;
	}

	RevertToSelf();

	DestroyEnvironmentBlock(pEnvironment);

	lprintf("CreatePipedProcessAsUserW(): pid %d\n", pi.dwProcessId);

	*phProcess = pi.hProcess;
	CloseHandle(pi.hThread);
	CloseHandle(hUserToken);

	return ERROR_SUCCESS;

}

ULONG CreateNormalProcessAsUserW(
		PWCHAR pwszUserName,
		PWCHAR pwszUserPassword,
		PWCHAR pwszCommand,
		BOOLEAN bRunInteractively,
		HANDLE *phProcess)
{
	ULONG	uResult;


	uResult = CreatePipedProcessAsUserW(
			pwszUserName,
			pwszUserPassword,
			pwszCommand,
			bRunInteractively,
			INVALID_HANDLE_VALUE,
			INVALID_HANDLE_VALUE,
			INVALID_HANDLE_VALUE,
			phProcess);

	if (ERROR_SUCCESS != uResult)
		lprintf_err(uResult, "CreateNormalProcessAsUserW(): CreatePipedProcessAsUserW()");

	return uResult;
}

ULONG CreateNormalProcessAsCurrentUserW(
		PWCHAR pwszCommand,
		BOOLEAN bRunInteractively,
		HANDLE *phProcess)
{
	ULONG	uResult;


	uResult = CreatePipedProcessAsCurrentUserW(
			pwszCommand,
			bRunInteractively,
			INVALID_HANDLE_VALUE,
			INVALID_HANDLE_VALUE,
			INVALID_HANDLE_VALUE,
			phProcess);

	if (ERROR_SUCCESS != uResult)
		lprintf_err(uResult, "CreateNormalProcessAsCurrentUserW(): CreatePipedProcessAsCurrentUserW()");

	return uResult;
}
