#include "io.h"

#define LOG_FILE_NAME L"c:\\move-profiles.log"

HANDLE g_Heap;

void Sleep(ULONG milliseconds)
{
    LARGE_INTEGER sleepInterval;

    sleepInterval.QuadPart = -1LL * NANOTICKS * milliseconds;
    NtDelayExecution(FALSE, &sleepInterval);
}

void DisplayString(IN const PWCHAR msg)
{
    UNICODE_STRING us;

    us.Buffer = msg;
    us.Length = wcslen(msg) * sizeof(WCHAR);
    us.MaximumLength = us.Length + sizeof(WCHAR);

    NtDisplayString(&us);
}

void NtPrintf(IN const PWCHAR format, ...)
{
    va_list args;
    WCHAR buffer[1024];

    va_start(args, format);
    _vsnwprintf(buffer, RTL_NUMBER_OF(buffer), format, args);
    va_end(args);

    DisplayString(buffer);
}

void NtLog(IN BOOLEAN print, IN const PWCHAR format, ...)
{
    va_list args;
    WCHAR buffer[1024];
    static HANDLE logFile = NULL;
    NTSTATUS status;

    if (!logFile)
    {
        status = FileOpen(&logFile, LOG_FILE_NAME, TRUE, TRUE, FALSE);
        if (!NT_SUCCESS(status))
            goto print;
    }

    va_start(args, format);
    _vsnwprintf(buffer, RTL_NUMBER_OF(buffer), format, args);
    va_end(args);

print:
    if (print)
        DisplayString(buffer);
    if (logFile)
        FileWrite(logFile, buffer, sizeof(WCHAR)*wcslen(buffer), NULL);
}

HANDLE InitHeap(void)
{
    RTL_HEAP_PARAMETERS heapParams;

    RtlZeroMemory(&heapParams, sizeof(heapParams));
    heapParams.Length = sizeof(heapParams);
    return RtlCreateHeap(HEAP_GROWABLE, NULL, 0x100000, 0x1000, NULL, &heapParams);
}

BOOLEAN FreeHeap(HANDLE heap)
{
    return NULL == RtlDestroyHeap(heap);
}

NTSTATUS EnablePrivileges(void)
{
    HANDLE processToken = NULL;
    PTOKEN_PRIVILEGES tp = NULL;
    ULONG size;
    NTSTATUS status;

    // This is a variable-size struct, but definition contains 1 element by default.
    size = sizeof(TOKEN_PRIVILEGES) + sizeof(LUID_AND_ATTRIBUTES);
    tp = RtlAllocateHeap(g_Heap, 0, size);
    
    status = NtOpenProcessToken(NtCurrentProcess(), TOKEN_ALL_ACCESS, &processToken);
    if (!NT_SUCCESS(status))
        goto cleanup;

    tp->PrivilegeCount = 2;
    tp->Privileges[0].Luid.HighPart = 0;
    tp->Privileges[0].Luid.LowPart = SE_SECURITY_PRIVILEGE; // needed for file security manipulation
    tp->Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    tp->Privileges[1].Luid.HighPart = 0;
    tp->Privileges[1].Luid.LowPart = SE_RESTORE_PRIVILEGE; // needed for setting file ownership
    tp->Privileges[1].Attributes = SE_PRIVILEGE_ENABLED;

    status = NtAdjustPrivilegesToken(processToken, FALSE, tp, size, NULL, NULL);

cleanup:
    if (processToken)
        NtClose(processToken);
    if (tp)
        RtlFreeHeap(g_Heap, 0, tp);
    return status;
}

NTSTATUS main(INT argc, PWCHAR argv[], PWCHAR envp[], ULONG DebugFlag OPTIONAL)
{
    NTSTATUS status;

    g_Heap = InitHeap();

    status = EnablePrivileges();
    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] EnablePrivileges failed: %x", status);
        goto cleanup;
    }
    
    DisplayString(L"move-profiles (" TEXT(__DATE__) L" " TEXT(__TIME__) L")\n");
    Sleep(1000);

    // TODO: params, removing old, create symlink
    status = FileCopyDirectory(L"c:\\users", L"c:\\welp");

cleanup:
    if (g_Heap)
        FreeHeap(g_Heap);
    NtTerminateProcess(NtCurrentProcess(), status);

    return status;
}
