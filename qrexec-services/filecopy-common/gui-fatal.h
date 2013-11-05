#pragma once
#include <tchar.h>
#include <windows.h>

typedef void (*show_error_t)(int);
void set_error_gui_callbacks(HWND hD, show_error_t cb);

void gui_fatal(TCHAR *fmt, ...);
void gui_nonfatal(TCHAR *fmt, ...);
