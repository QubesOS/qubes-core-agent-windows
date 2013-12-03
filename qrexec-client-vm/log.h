#pragma once
#include <windows.h>
#include <strsafe.h>
#include <stdio.h>


VOID lprintf(PUCHAR szFormat, ...);
VOID lprintf_err(ULONG uErrorCode, PUCHAR szFormat, ...);

