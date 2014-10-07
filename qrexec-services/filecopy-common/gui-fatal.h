#pragma once

typedef void(*show_error_t)(int);
void set_error_gui_callbacks(HWND hD, show_error_t cb);

void gui_fatal(const PTCHAR fmt, ...);
void gui_nonfatal(const PTCHAR fmt, ...);
