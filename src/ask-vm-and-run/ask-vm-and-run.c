#include <windows.h>
#include <Shlwapi.h>
#include <strsafe.h>
#include <stdlib.h>
#include "resource.h"

#include "log.h"

#define QREXEC_CLIENT_VM L"qrexec-client-vm.exe"

INT_PTR CALLBACK InputBoxProc(IN HWND hwnd, IN UINT message, IN WPARAM wParam, IN LPARAM lParam)
{
    WCHAR input[128];

    if (message == WM_COMMAND)
    {
        switch (LOWORD(wParam))
        {
        case IDOK:
            GetWindowText(GetDlgItem(hwnd, IDC_INPUT), input, RTL_NUMBER_OF(input));
            EndDialog(hwnd, (INT_PTR) _wcsdup(input));
            return TRUE;
        case IDCANCEL:
            EndDialog(hwnd, 0);
            return TRUE;
        }
    }

    return FALSE;
}

void ReportError(IN const WCHAR *message)
{
    MessageBox(NULL, message, L"Qubes", MB_OK | MB_ICONERROR);
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE previousInstance, LPTSTR commandLine, int showState)
{
    INT_PTR result;
    WCHAR qrexecClientPath[MAX_PATH] = { 0 };
    WCHAR *qrexecClientCmdLine;
    size_t cchQrexecClientCmdLine;
    WCHAR *pathSeparator;
    PROCESS_INFORMATION pi;
    STARTUPINFO si = { 0 };
    DWORD status;

    result = DialogBox(
        instance, // application instance
        MAKEINTRESOURCE(IDD_INPUTBOX), // dialog box resource
        NULL, // owner window
        InputBoxProc // dialog box window procedure
        );

    switch (result)
    {
    case 0:
        // cancel
        return 0;
    case -1:
        // error
        return 1;
    }

    // build RPC service config file path
    if (!GetModuleFileName(NULL, qrexecClientPath, RTL_NUMBER_OF(qrexecClientPath)))
    {
        status = perror("GetModuleFileName");
        ReportError(L"Failed to get " QREXEC_CLIENT_VM L" path");
        return status;
    }

    // cut off file name (qrexec_agent.exe)
    pathSeparator = wcsrchr(qrexecClientPath, L'\\');
    if (!pathSeparator)
    {
        LogError("Bad executable path");
        ReportError(L"Cannot find dir containing " QREXEC_CLIENT_VM);
        return ERROR_BAD_PATHNAME;
    }

    // Leave trailing backslash
    pathSeparator++;
    *pathSeparator = L'\0';
    PathAppend(qrexecClientPath, QREXEC_CLIENT_VM);

    cchQrexecClientCmdLine = wcslen(QREXEC_CLIENT_VM) + wcslen((WCHAR *) result) + wcslen(commandLine) + 3;
    qrexecClientCmdLine = malloc(cchQrexecClientCmdLine * sizeof(WCHAR));

    if (!qrexecClientCmdLine)
    {
        LogError("malloc failed");
        ReportError(L"Out of memory");
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    if (FAILED(StringCchPrintf(qrexecClientCmdLine, cchQrexecClientCmdLine, QREXEC_CLIENT_VM L" %s %s", (WCHAR *) result, commandLine)))
    {
        LogError("Failed to construct command line");
        ReportError(L"Failed to construct command line");
        return ERROR_BAD_PATHNAME;
    }

    si.cb = sizeof(si);

    if (!CreateProcess(qrexecClientPath, qrexecClientCmdLine, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
    {
        status = perror("CreateProcess");
        ReportError(L"Failed to execute qrexec-client-vm.exe");
        return status;
    }

    return ERROR_SUCCESS;
}
