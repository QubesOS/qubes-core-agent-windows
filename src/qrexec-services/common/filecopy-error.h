#pragma once
#include <windows.h>

typedef void(*fErrorCallback)(IN BOOL errorOccured);

void FcSetErrorCallback(IN HWND window, IN fErrorCallback callback);
void FcReportError(IN DWORD errorCode, IN BOOL fatal, IN const WCHAR *format, ...);
