#include <tchar.h>
#include <windows.h>
#include <commctrl.h>
#include <taskdialog.h> // definitions absent in mingw
#include "gui-fatal.h"
#include "gui-progress.h"

extern INT64 total_size;
extern BOOL cancel_operation;

HWND hDialog = NULL;
HANDLE hProgressWindowThread;

// workaround for lack of TaskDialog related stuff in mingw
TaskDialogIndirectProc *TaskDialogIndirectDynamic = NULL;

void ResolveFunc()
{
	HMODULE comctl32 = LoadLibrary(TEXT("comctl32.dll"));
	// todo: proper error handling
	if (!comctl32)
	{
		MessageBox(NULL, TEXT("Failed to load comctl32.dll"), TEXT("gui-progress"), MB_ICONERROR);
	}
	TaskDialogIndirectDynamic = (TaskDialogIndirectProc*) GetProcAddress(comctl32, "TaskDialogIndirect");
	if (!TaskDialogIndirectDynamic)
		MessageBox(NULL, TEXT("Failed to GetProcAddress(TaskDialogIndirect)"), TEXT("gui-progress"), MB_ICONERROR);
}
// end workaround

HRESULT CALLBACK TaskDialogCallbackProc(HWND hwnd, UINT uNotification,
		WPARAM wParam, LPARAM lParam, LONG_PTR dwRefData)
{
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


DWORD doTaskDialogThread(LPVOID lpThreadParameter)
{
	int nButtonPressed                  = 0;
	TASKDIALOGCONFIG config             = {0};

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

	// TODO: call it in separate thread
	(*TaskDialogIndirectDynamic) (&config, &nButtonPressed, NULL, NULL);
	switch (nButtonPressed)
	{
		case IDOK:
			break; // the user pressed button 0 (change password).
		case IDCANCEL:
			break; // user canceled the dialog
		default:
			break; // should never happen
	}
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
	hProgressWindowThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)doTaskDialogThread, NULL, 0, NULL);
	if (!hProgressWindowThread) {
		// TODO: error handler
	}
	set_error_gui_callbacks(hDialog, switch_progressbar_to_red);
}

void do_notify_progress(long long written, int flag)
{
	if (!TaskDialogIndirectDynamic)
		ResolveFunc();
	switch (flag) {
		case PROGRESS_FLAG_INIT:
			createProgressWindow();
			break;
		case PROGRESS_FLAG_NORMAL:
			if (hDialog) {
				if (written)
					SendNotifyMessage(hDialog, TDM_SET_PROGRESS_BAR_POS, (WPARAM)(100LL*written/total_size), 0);
				else
					SendNotifyMessage(hDialog, TDM_SET_PROGRESS_BAR_STATE, (WPARAM)PBST_NORMAL, 0);
			}
			break;
		case PROGRESS_FLAG_DONE:
			if (hDialog)
				SendNotifyMessage(hDialog, TDM_CLICK_BUTTON, IDOK, 0);
			if (hProgressWindowThread) {
				WaitForSingleObject(hProgressWindowThread, INFINITE);
				CloseHandle(hProgressWindowThread);
			}
			break;
		case PROGRESS_FLAG_ERROR:
			if (hDialog)
				SendNotifyMessage(hDialog, TDM_SET_PROGRESS_BAR_STATE, (WPARAM)PBST_ERROR, 0);
			break;
	}
}
