/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (c) Invisible Things Lab
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include <windows.h>
#include <commctrl.h>

#include <log.h>

#include "filecopy-error.h"
#include "gui-progress.h"

extern INT64 g_totalSize;
extern BOOL g_cancelOperation;

HWND g_progressDialog = NULL;
HANDLE g_progressWindowThread = NULL;

static HRESULT CALLBACK TaskDialogCallbackProc(IN HWND window, IN UINT notification, IN WPARAM wParam, IN LPARAM lParam, IN LONG_PTR context)
{
    LogVerbose("hwnd 0x%x, code %lu", window, notification);
    switch (notification)
    {
    case TDN_CREATED:
        g_progressDialog = window;
        break;
    case TDN_DESTROYED:
        g_progressDialog = NULL;
        break;
    case TDN_BUTTON_CLICKED:
        if (wParam == IDCANCEL)
        {
            g_cancelOperation = TRUE;
            return S_FALSE;
        }
        // IDOK -> close dialog
    }

    // TODO: for cancel button, only send cancel request, but do not close dialog
    return S_OK;
}

static DWORD TaskDialogThread(IN void *param)
{
    int buttonPressed = 0;
    TASKDIALOGCONFIG config = { 0 };
    HRESULT status;

    LogVerbose("start");
    config.cbSize = sizeof(config);
    config.hInstance = NULL;
    config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    config.dwFlags = TDF_SHOW_PROGRESS_BAR;
    config.pszMainIcon = NULL;
    config.pszMainInstruction = L"Sending files";
    config.pszContent = L"Sending files, please wait";
    config.pButtons = NULL;
    config.cButtons = 0;
    config.pfCallback = TaskDialogCallbackProc;

    status = TaskDialogIndirect(&config, &buttonPressed, NULL, NULL);
    if (status != S_OK)
    {
        LogError("TaskDialogIndirect failed: %d 0x%x", status, status);
    }
    LogDebug("button: %d 0x%x", buttonPressed, buttonPressed);

    return 0;
}

static void SetProgressbarColor(IN BOOL errorOccured)
{
    if (errorOccured)
        UpdateProgress(0, PROGRESS_TYPE_ERROR);
    else
        UpdateProgress(0, PROGRESS_TYPE_NORMAL);
}

static void CreateProgressWindow(void)
{
    LogVerbose("start");
    g_progressWindowThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE) TaskDialogThread, NULL, 0, NULL);
    if (!g_progressWindowThread)
    {
        win_perror("CreateThread");
        return;
    }

    FcSetErrorCallback(g_progressDialog, SetProgressbarColor);
}

void UpdateProgress(IN UINT64 written, IN FC_PROGRESS_TYPE progressType)
{
    LogVerbose("written %I64u, type %d", written, progressType);
    switch (progressType)
    {
    case PROGRESS_TYPE_INIT:
        CreateProgressWindow();
        break;

    case PROGRESS_TYPE_NORMAL:
        if (g_progressDialog)
        {
            if (written)
                SendNotifyMessage(g_progressDialog, TDM_SET_PROGRESS_BAR_POS, (WPARAM) (100ULL * written / g_totalSize), 0);
            else
                SendNotifyMessage(g_progressDialog, TDM_SET_PROGRESS_BAR_STATE, (WPARAM) PBST_NORMAL, 0);
        }
        break;

    case PROGRESS_TYPE_DONE:
        if (g_progressDialog)
            SendNotifyMessage(g_progressDialog, TDM_CLICK_BUTTON, IDOK, 0);

        if (g_progressWindowThread)
        {
            WaitForSingleObject(g_progressWindowThread, 1000);
            CloseHandle(g_progressWindowThread);
        }
        break;

    case PROGRESS_TYPE_ERROR:
        if (g_progressDialog)
            SendNotifyMessage(g_progressDialog, TDM_SET_PROGRESS_BAR_STATE, (WPARAM) PBST_ERROR, 0);
        break;
    }
}
