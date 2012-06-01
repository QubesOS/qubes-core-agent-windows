#pragma once
#include <windows.h>
#include <strsafe.h>
#include <stdio.h>

#ifdef DBG

#define SERVICE_LOG		TEXT("c:\\qrexec_agent.log")

VOID lprintf(PUCHAR szFormat, ...);
VOID lprintf_err(ULONG uErrorCode, PUCHAR szFormat, ...);

#else

#define lprintf(...)
#define lprintf_err(...)

#endif