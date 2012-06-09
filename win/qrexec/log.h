#pragma once
#include <windows.h>
#include <strsafe.h>
#include <stdio.h>
#include "common.h"

#ifdef DBG

VOID lprintf(PUCHAR szFormat, ...);
VOID lprintf_err(ULONG uErrorCode, PUCHAR szFormat, ...);

#else

#define lprintf(...)
#define lprintf_err(...)

#endif