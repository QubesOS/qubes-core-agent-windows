#include <tchar.h>
#include <windows.h>
#include <commctrl.h>
#include <taskdialog.h> // definitions absent in mingw
#include "gui-fatal.h"
#include "gui-progress.h"
#include "log.h"

extern INT64 total_size;
extern BOOL cancel_operation;

HWND hDialog = NULL;
HANDLE hProgressWindowThread;

// workaround for lack of TaskDialog related stuff in mingw
TaskDialogIndirectProc *TaskDialogIndirectDynamic = NULL;

void ResolveTaskDialogProc()
{
    HMODULE comctl32 = LoadLibrary(TEXT("comctl32.dll"));
    // todo: proper error handling
    if (!comctl32)
    {
        perror("ResolveTaskDialogProc: LoadLibrary(comctl32)");
    }
    TaskDialogIndirectDynamic = (TaskDialogIndirectProc*) GetProcAddress(comctl32, "TaskDialogIndirect");
    if (!TaskDialogIndirectDynamic)
    {
        perror("ResolveTaskDialogProc: GetProcAddress(TaskDialogIndirect)");
    }
    debugf("ResolveTaskDialogProc: TaskDialogIndirect=0x%x\n", TaskDialogIndirectDynamic);
}
// end workaround

HRESULT CALLBACK TaskDialogCallbackProc(HWND hwnd, UINT uNotification,
        WPARAM wParam, LPARAM lParam, LONG_PTR dwRefData)
{
    debugf("TaskDialogCallbackProc: hwnd 0x%x, code 0x%x, wparam 0x%x, lparam 0x%x, data 0x%p\n",
        hwnd, uNotification, wParam, lParam, dwRefData);

    switch (uNotification) {
        case TDN_CREATED:
            hDialog = hwnd;
            break;
        case TDN_DESTROYED:
            hDialog = NULL;
            break;
        case TDN_BUTTON_CLICKED:
            if (wParam == IDCANCEL) {
                cancel_operation = TRUE;
                return S_FALSE;
            }
            // IDOK -> close dialog
    }

    // TODO: for cancel button, only send cancel request, but do not close dialog
    return S_OK;
}

DWORD TaskDialogThreadProc(void *lpThreadParameter)
{
    int nButtonPressed                  = 0;
    TASKDIALOGCONFIG config             = {0};

    debugf("TaskDialogThreadProc: start\n");
    config.cbSize                       = sizeof(config);
    config.hInstance                    = NULL;
    config.dwCommonButtons              = TDCBF_CANCEL_BUTTON;
    config.dwFlags                      = TDF_SHOW_PROGRESS_BAR;
    config.pszMainIcon                  = NULL;
    config.pszMainInstruction           = TEXT("Sending files");
    config.pszContent                   = TEXT("Sending files, please wait");
    config.pButtons                     = NULL;
    config.cButtons                     = 0;
    config.pfCallback                   = TaskDialogCallbackProc;

    (*TaskDialogIndirectDynamic) (&config, &nButtonPressed, NULL, NULL);
    switch (nButtonPressed)
    {
        case IDOK:
            break; // the user pressed button 0.
        case IDCANCEL:
            break; // user canceled the dialog
        default:
            break; // should never happen
    }

    debugf("TaskDialogThreadProc: exiting, result: %d\n", nButtonPressed);
    return 0;
}

void switch_progressbar_to_red(int red) {
    if (red)
        do_notify_progress(0, PROGRESS_FLAG_ERROR);
    else
        do_notify_progress(0, PROGRESS_FLAG_NORMAL);
}

void createProgressWindow()
{
    hProgressWindowThread = CreateThread(NULL, 0, TaskDialogThreadProc, NULL, 0, NULL);
    if (!hProgressWindowThread) {
        perror("createProgressWindow: CreateThread");
    }
    set_error_gui_callbacks(hDialog, switch_progressbar_to_red);
}

void do_notify_progress(long long written, int flag)
{
    if (!TaskDialogIndirectDynamic)
        ResolveTaskDialogProc();

    switch (flag) {
        case PROGRESS_FLAG_INIT:
            debugf("do_notify_progress: PROGRESS_FLAG_INIT\n");
            createProgressWindow();
            break;
        case PROGRESS_FLAG_NORMAL:
            debugf("do_notify_progress: PROGRESS_FLAG_NORMAL\n");
            if (hDialog) {
                if (written)
                    SendNotifyMessage(hDialog, TDM_SET_PROGRESS_BAR_POS, (WPARAM)(100LL*written/total_size), 0);
                else
                    SendNotifyMessage(hDialog, TDM_SET_PROGRESS_BAR_STATE, (WPARAM)PBST_NORMAL, 0);
            }
            break;
        case PROGRESS_FLAG_DONE:
            debugf("do_notify_progress: PROGRESS_FLAG_DONE\n");
            if (hDialog)
                SendNotifyMessage(hDialog, TDM_CLICK_BUTTON, IDOK, 0);
            if (hProgressWindowThread) {
                WaitForSingleObject(hProgressWindowThread, 1000); // don't wait forever
                CloseHandle(hProgressWindowThread);
            }
            break;
        case PROGRESS_FLAG_ERROR:
            debugf("do_notify_progress: PROGRESS_FLAG_ERROR\n");
            if (hDialog)
                SendNotifyMessage(hDialog, TDM_SET_PROGRESS_BAR_STATE, (WPARAM)PBST_ERROR, 0);
            break;
    }

    debugf("do_notify_progress: exiting\n");
}
