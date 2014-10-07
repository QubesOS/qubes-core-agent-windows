#include <windows.h>
#include <tchar.h>
#include <Shlwapi.h>
#include <strsafe.h>
#include <stdlib.h>
#include "resource.h"

INT_PTR CALLBACK inputBoxProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    TCHAR szInput[128];

    if (message == WM_COMMAND)
    {
        switch (LOWORD(wParam))
        {
        case IDOK:
            GetWindowText(GetDlgItem(hwnd, IDC_INPUT), szInput, RTL_NUMBER_OF(szInput));
            EndDialog(hwnd, (INT_PTR) _tcsdup(szInput));
            return TRUE;
        case IDCANCEL:
            EndDialog(hwnd, 0);
            return TRUE;
        }
    }

    return FALSE;
}

void reportError(PTCHAR pszMessage)
{
    MessageBox(NULL, pszMessage, TEXT("Qubes"), MB_OK | MB_ICONERROR);
}

int APIENTRY _tWinMain(HINSTANCE hInst, HINSTANCE hPrevInst, LPTSTR lpCommandLine, int nCmdShow)
{
    MSG msg;
    INT_PTR hResult;
    TCHAR szQrexecClientPath[MAX_PATH];
    PTCHAR pszQrexecClientCmdLine;
    size_t cchQrexecClientCmdLine;
    PTCHAR pSeparator;
    PROCESS_INFORMATION pi;
    STARTUPINFO si;

    hResult = DialogBox(
        hInst, // application instance
        MAKEINTRESOURCE(IDD_INPUTBOX), // dialog box resource
        NULL, // owner window
        inputBoxProc // dialog box window procedure
        );

    switch (hResult)
    {
    case 0:
        // cancel
        return 0;
    case -1:
        // error
        return 1;
    }

    // build RPC service config file path
    memset(szQrexecClientPath, 0, sizeof(szQrexecClientPath));
    if (!GetModuleFileName(NULL, szQrexecClientPath, RTL_NUMBER_OF(szQrexecClientPath)))
    {
        reportError(TEXT("Failed to get qrexec-client-vm.exe path"));
        return 1;
    }

    // cut off file name (qrexec_agent.exe)
    pSeparator = _tcsrchr(szQrexecClientPath, TEXT('\\'));
    if (!pSeparator)
    {
        reportError(TEXT("Cannot find dir containing qrexec-client-vm.exe"));
        return 1;
    }

    // Leave trailing backslash
    pSeparator++;
    *pSeparator = TEXT('\0');
    PathAppend(szQrexecClientPath, TEXT("qrexec-client-vm.exe"));

    cchQrexecClientCmdLine = _tcslen(TEXT("qrexec-client-vm.exe")) + _tcslen((PTCHAR) hResult) + _tcslen(lpCommandLine) + 3;
    pszQrexecClientCmdLine = malloc(cchQrexecClientCmdLine*sizeof(TCHAR));

    if (!pszQrexecClientCmdLine)
    {
        reportError(TEXT("out of memory"));
        return 1;
    }

    if (FAILED(StringCchPrintf(pszQrexecClientCmdLine, cchQrexecClientCmdLine, TEXT("qrexec-client-vm.exe %s %s"), (PTCHAR) hResult, lpCommandLine)))
    {
        reportError(TEXT("Failed to construct command line"));
        return 1;
    }

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);

    if (!CreateProcess(szQrexecClientPath, pszQrexecClientCmdLine, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
    {
        reportError(TEXT("Failed to execute qrexec-client-vm.exe"));
        return 1;
    }

    return 0;
}
