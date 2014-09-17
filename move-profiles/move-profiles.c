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

NTSTATUS wmain(INT argc, PWCHAR argv[], PWCHAR envp[], ULONG DebugFlag OPTIONAL)
{
    NTSTATUS status;
    ULONG attrs;
    TIME_FIELDS tf;
    LARGE_INTEGER systemTime, localTime;

    g_Heap = InitHeap();
    if (!g_Heap)
    {
        status = STATUS_NO_MEMORY;
        goto cleanup;
    }

    NtLog(FALSE, L"move-profiles (" TEXT(__DATE__) L" " TEXT(__TIME__) L")\n");

    status = EnablePrivileges();
    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] EnablePrivileges failed: %x\n", status);
        goto cleanup;
    }

    NtQuerySystemTime(&systemTime);
    RtlSystemTimeToLocalTime(&systemTime, &localTime);
    RtlTimeToTimeFields(&localTime, &tf);

    NtLog(FALSE, L"[*] Start time: %04d-%02d-%02d %02d:%02d:%02d.%03d\n",
        tf.Year, tf.Month, tf.Day, tf.Hour, tf.Minute, tf.Second, tf.Milliseconds);

    if (argc < 3)
    {
        NtLog(TRUE, L"[!] Usage: move-profiles <source dir> <target dir>\n");
        status = STATUS_INVALID_PARAMETER;
        goto cleanup;
    }

    // Check if source directory is already a symlink.
    status = FileGetAttributes(argv[1], &attrs);
    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] FileGetAttributes(%s) failed: %x\n", argv[1], status);
        goto cleanup;
    }

    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT)
    {
        NtLog(TRUE, L"[*] Source directory (%s) is already a reparse point, aborting\n", argv[1]);
        goto cleanup;
    }

    // Check if destination directory exists.
    status = FileGetAttributes(argv[2], &attrs);
    if (NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[?] Destination directory (%s) already exists, aborting\n", argv[2]);
        goto cleanup;
    }

    // TODO: parsing quotes so directories can have embedded spaces
    // Might happen in some non-english languages?
    NtLog(TRUE, L"[*] Copying: '%s' -> '%s'\n", argv[1], argv[2]);
    status = FileCopyDirectory(argv[1], argv[2], FALSE);
    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] FileCopyDirectory(%s, %s) failed: %x\n", argv[1], argv[2], status);
        goto cleanup;
    }

    NtLog(TRUE, L"[*] Deleting: '%s'\n", argv[1]);
    status = FileDeleteDirectory(argv[1], FALSE);
    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] FileDeleteDirectory failed: %x\n", status);

        // Attempt to restore previous state.
        FileCopyDirectory(argv[2], argv[1], TRUE);
        goto cleanup;
    }

    NtLog(TRUE, L"[*] Creating symlink: '%s' -> '%s'\n", argv[1], argv[2]);
    status = FileSetSymlink(argv[1], argv[2]);
    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] FileSetReparsePoint failed: %x\n", status);
        goto cleanup;
    }

    status = STATUS_SUCCESS;

cleanup:
    NtQuerySystemTime(&systemTime);
    RtlSystemTimeToLocalTime(&systemTime, &localTime);
    RtlTimeToTimeFields(&localTime, &tf);

    NtLog(FALSE, L"[*] End time: %04d-%02d-%02d %02d:%02d:%02d.%03d\n",
        tf.Year, tf.Month, tf.Day, tf.Hour, tf.Minute, tf.Second, tf.Milliseconds);

    if (g_Heap)
        FreeHeap(g_Heap);
    NtTerminateProcess(NtCurrentProcess(), status);

    return status;
}
