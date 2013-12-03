#include <Windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <tchar.h>
#include <Wtsapi32.h>
#include <utf8-conv.h>
#include <Shlwapi.h>

PTCHAR	pszExpectedUser;
BOOL	bFound = FALSE;

BOOL CheckSession(DWORD	dSessionId) {
	PTCHAR  pszUserName;
	DWORD  cbUserName;

	if (FAILED(WTSQuerySessionInformation(WTS_CURRENT_SERVER_HANDLE, dSessionId, WTSUserName, &pszUserName, &cbUserName))) {
		fprintf(stderr, "WTSQuerySessionInformation failed: 0x%x\n", GetLastError());
		return FALSE;
	}
#ifdef DBG
# ifdef UNICODE
	fprintf(stderr, "Found session: %S\n", pszUserName);
# else
	fprintf(stderr, "Found session: %s\n", pszUserName);
# endif
#endif
	if (_tcscmp(pszUserName, pszExpectedUser)==0) {
		bFound = TRUE;
	}
	WTSFreeMemory(pszUserName);

	return bFound;
}

BOOL CheckIfUserLoggedIn() {
	PWTS_SESSION_INFO	pSessionInfo;
	DWORD				dSessionCount;
	DWORD i;

	if (FAILED(WTSEnumerateSessions(WTS_CURRENT_SERVER_HANDLE, 0, 1, &pSessionInfo, &dSessionCount))) {
		fprintf(stderr, "Failed to enumerate sessions: 0x%x\n", GetLastError());
		return FALSE;
	}
	for (i = 0; i < dSessionCount; i++) {
		if (pSessionInfo[i].State == WTSActive) {
			if (CheckSession(pSessionInfo[i].SessionId)) {
				bFound = TRUE;
			}
		}
	}

	WTSFreeMemory(pSessionInfo);

	return bFound;
}


LRESULT CALLBACK WindowProc(
		HWND hWnd,       // handle to window
		UINT Msg,        // WM_WTSSESSION_CHANGE
		WPARAM wParam,   // session state change event
		LPARAM lParam    // session ID
		) {
	PTCHAR	pszUserName;
	size_t	cbUserName;
	switch (Msg) {
		case WM_WTSSESSION_CHANGE:
			if (wParam == WTS_SESSION_LOGON) {
				if (CheckSession((DWORD)lParam))
					bFound = TRUE;
			}
			return TRUE;

		default:
			return DefWindowProc(hWnd, Msg, wParam, lParam);
	}

	return FALSE;
}

HWND createMainWindow(HINSTANCE hInst)
{
	WNDCLASSEX wc;
	ATOM windowClass;

	wc.cbSize = sizeof(wc);
	wc.style = 0;
	wc.lpfnWndProc = (WNDPROC)WindowProc;
	wc.cbClsExtra = 0;
	wc.cbWndExtra = 0;
	wc.hInstance = hInst;
	wc.hIcon = NULL;
	wc.hCursor = NULL;
	wc.hbrBackground = (HBRUSH)COLOR_WINDOW;
	wc.lpszMenuName = NULL;
	wc.lpszClassName = TEXT("MainWindowClass");
	wc.hIconSm = NULL;

	windowClass = RegisterClassEx(&wc);
	if (!windowClass) {
		return NULL;
	}

	return CreateWindow(
			wc.lpszClassName, /* class */
			TEXT("Qubes session wait service"), /* name */
			WS_OVERLAPPEDWINDOW, /* style */
			CW_USEDEFAULT, CW_USEDEFAULT, /* x,y */
			10, 10, /* w, h */
			HWND_MESSAGE, /* parent */
			NULL, /* menu */
			hInst, /* instance */
			NULL);
}

int ReadUntilEOF(HANDLE fd, void *buf, int size)
{
	int got_read = 0;
	int ret;
	while (got_read < size) {
		if (!ReadFile(fd, (char *) buf + got_read, size - got_read, &ret, NULL)) {
			if (GetLastError() == ERROR_BROKEN_PIPE)
				return got_read;
			return -1;
		}
		if (ret == 0) {
			return got_read;
		}
		got_read += ret;
	}
	//      fprintf(stderr, "read %d bytes\n", size);
	return got_read;
}

int APIENTRY _tWinMain(HINSTANCE hInst, HINSTANCE hPrevInst, PTCHAR pszCommandLine, int nCmdShow) {
	HWND	hMainWindow;
	size_t	cbExpectedUserUtf8;
	size_t	cchExpectedUser = 0;
	UCHAR	pszExpectedUserUtf8[USERNAME_LENGTH + 1];
	HANDLE	hStdIn;

	// first try command parameter
	pszExpectedUser = PathGetArgs(pszCommandLine);

	// if none was given, read from stdin
	if (!pszExpectedUser || pszExpectedUser[0] == TEXT('\0')) {
		hStdIn = GetStdHandle(STD_INPUT_HANDLE);
		if (hStdIn == INVALID_HANDLE_VALUE) {
			// some error handler?
			return 1;
		}

		cbExpectedUserUtf8 = ReadUntilEOF(hStdIn, pszExpectedUserUtf8, sizeof(pszExpectedUserUtf8));
		if (cbExpectedUserUtf8 <= 0 || cbExpectedUserUtf8 >= sizeof(pszExpectedUserUtf8)) {
			fprintf(stderr, "Failed to read the user name\n");
			return 1;
		}

		// strip end of line and spaces
		// FIXME: what about multibyte char with one byte '\r'/'\n'/' '?
		while (cbExpectedUserUtf8 > 0 &&
				(pszExpectedUserUtf8[cbExpectedUserUtf8-1] == '\n' ||
				 pszExpectedUserUtf8[cbExpectedUserUtf8-1] == '\r' ||
				 pszExpectedUserUtf8[cbExpectedUserUtf8-1] == ' '))
			cbExpectedUserUtf8--;
		pszExpectedUserUtf8[cbExpectedUserUtf8] = '\0';


#ifdef UNICODE
		if (FAILED(ConvertUTF8ToUTF16(pszExpectedUserUtf8, &pszExpectedUser, &cchExpectedUser))) {
			fprintf(stderr, "Failed to convert the user name to UTF16\n");
			return 1;
		}
#else
		pszExpectedUser = pszExpectedUserUtf8;
		cchExpectedUser = cbExpectedUserUtf8;
#endif
	}

	hMainWindow = createMainWindow(hInst);

	if (hMainWindow == INVALID_HANDLE_VALUE) {
		fprintf(stderr, "Failed to create main window: 0x%x\n", GetLastError());
		return 1;
	}

	if (FAILED(WTSRegisterSessionNotification(hMainWindow, NOTIFY_FOR_ALL_SESSIONS))) {
		fprintf(stderr, "Failed to register session notification: 0x%x\n", GetLastError());
		DestroyWindow(hMainWindow);
		return 1;
	}

	bFound = FALSE;
	if (!CheckIfUserLoggedIn()) {
		BOOL	bRet;
		MSG		msg;

#ifdef DBG
# ifdef UNICODE
		fprintf(stderr, "Waiting for user %S\n", pszExpectedUser);
# else
		fprintf(stderr, "Waiting for user %s\n", pszExpectedUser);
# endif
#endif
		while( (bRet = GetMessage( &msg, hMainWindow, 0, 0 )) != 0)
		{
			if (bRet == -1)
			{
				// handle the error and possibly exit
				break;
			}
			else
			{
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
			if (bFound)
				break;
		}
	}
	WTSUnRegisterSessionNotification(hMainWindow);
	DestroyWindow(hMainWindow);
#ifdef UNICODE
	if (cchExpectedUser)
		free(pszExpectedUser);
#endif
	return 0;
}
