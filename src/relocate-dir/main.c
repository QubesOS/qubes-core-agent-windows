#include "io.h"

// TODO: read from registry
#define LOG_FILE_NAME L"c:\\move-profiles.log"

HANDLE g_Heap;

__declspec(dllimport)
int _vsnwprintf(
    wchar_t *buffer,
    size_t count,
    const wchar_t *format,
    va_list argptr
    );

void Sleep(ULONG milliseconds)
{
    LARGE_INTEGER sleepInterval;

    sleepInterval.QuadPart = -1LL * NANOTICKS * milliseconds;
    NtDelayExecution(FALSE, &sleepInterval);
}

void DisplayString(IN const PWCHAR msg)
{
    UNICODE_STRING us;

#pragma warning(push)
#pragma warning(disable: 4090) // different const qualifiers
    // The call below doesn't change the string.
    us.Buffer = msg;
#pragma warning(pop)
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
    TOKEN_PRIVILEGES *tp = NULL;
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

// TODO: preserve non-default values if present.
NTSTATUS RemoveBootExecuteEntry(void)
{
    WCHAR keyName[] = L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Session Manager";
    WCHAR valueName[] = L"BootExecute";
    WCHAR defaultValue[] = L"autocheck autochk *\0"; // multi-string, so double null-terminated
    UNICODE_STRING keyNameU, valueNameU;
    OBJECT_ATTRIBUTES oa;
    HANDLE key = NULL;
    NTSTATUS status;

    keyNameU.Buffer = keyName;
    keyNameU.Length = wcslen(keyName) * sizeof(WCHAR);
    keyNameU.MaximumLength = keyNameU.Length + sizeof(WCHAR);

    InitializeObjectAttributes(
        &oa,
        &keyNameU,
        OBJ_CASE_INSENSITIVE,
        NULL,
        NULL);

    status = NtOpenKey(&key, KEY_READ | KEY_WRITE, &oa);
    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] RemoveBootExecuteEntry: NtOpenKey(%s) failed: %x\n", keyName, status);
        goto cleanup;
    }

    valueNameU.Buffer = valueName;
    valueNameU.Length = wcslen(valueName) * sizeof(WCHAR);
    valueNameU.MaximumLength = valueNameU.Length + sizeof(WCHAR);

    status = NtSetValueKey(key, &valueNameU, 0, REG_MULTI_SZ, defaultValue, sizeof(defaultValue));
    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] RemoveBootExecuteEntry: NtSetValueKey(%s, %s) failed: %x\n", keyName, valueName, status);
        goto cleanup;
    }

    status = STATUS_SUCCESS;

cleanup:
    if (key)
        NtClose(key);
    return status;
}

NTSTATUS wmain(INT argc, WCHAR *argv[], WCHAR *envp[], ULONG DebugFlag OPTIONAL)
{
    NTSTATUS status;
    ULONG attrs;
    TIME_FIELDS tf;
    LARGE_INTEGER systemTime, localTime;

    NtLog(TRUE, L"move-profiles (" TEXT(__DATE__) L" " TEXT(__TIME__) L")\n");

    status = EnablePrivileges();
    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] EnablePrivileges failed: %x\n", status);
        goto cleanup;
    }

    NtQuerySystemTime(&systemTime);
    RtlSystemTimeToLocalTime(&systemTime, &localTime);
    RtlTimeToTimeFields(&localTime, &tf);

    NtLog(TRUE, L"[*] Start time: %04d-%02d-%02d %02d:%02d:%02d.%03d\n",
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
    NtLog(TRUE, L"[*] Copying: '%s' -> '%s', stand by...\n", argv[1], argv[2]);
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

    NtLog(TRUE, L"[*] End time: %04d-%02d-%02d %02d:%02d:%02d.%03d\n",
        tf.Year, tf.Month, tf.Day, tf.Hour, tf.Minute, tf.Second, tf.Milliseconds);

    // Remove itself from BootExecute.
    RemoveBootExecuteEntry();

    return status;
}

void EnvironmentStringToUnicodeString(IN WCHAR *wsIn, OUT UNICODE_STRING *usOut)
{
    if (wsIn)
    {
        WCHAR *currentChar = wsIn;

        while (*currentChar)
        {
            while (*currentChar++);
        }

        currentChar++;

        usOut->Buffer = wsIn;
        usOut->MaximumLength = usOut->Length = (currentChar - wsIn) * sizeof(WCHAR);
    }
    else
    {
        usOut->Buffer = NULL;
        usOut->Length = usOut->MaximumLength = 0;
    }
}

// from ReactOS lib/nt/entry_point.c
void NtProcessStartup(PPEB2 Peb)
{
    NTSTATUS Status;
    PRTL_USER_PROCESS_PARAMETERS ProcessParameters;
    UNICODE_STRING *CmdLineString;
    UNICODE_STRING UnicodeEnvironment;
    PWCHAR NullPointer = NULL;
    INT argc = 0;
    PWCHAR *argv;
    PWCHAR *envp;
    PWCHAR *ArgumentList;
    PWCHAR Source, Destination;
    ULONG Length;

    /* Normalize and get the Process Parameters */
    ProcessParameters = RtlNormalizeProcessParams(Peb->ProcessParameters);

    Status = STATUS_NO_MEMORY;
    g_Heap = InitHeap();
    if (!g_Heap)
        goto fail;

    /* Allocate memory for the argument list, enough for 512 tokens */
    //FIXME: what if 512 is not enough????
    ArgumentList = RtlAllocateHeap(g_Heap, 0, 512 * sizeof(PWCHAR));
    if (!ArgumentList)
        goto fail;

    /* Use a null pointer as default */
    argv = &NullPointer;
    envp = &NullPointer;

    /* Set the first pointer to NULL, and set the argument array to the buffer */
    *ArgumentList = NULL;
    argv = ArgumentList;

    /* Get the pointer to the Command Line */
    CmdLineString = &ProcessParameters->CommandLine;

    /* If we don't have a command line, use the image path instead */
    if (!CmdLineString->Buffer || !CmdLineString->Length)
    {
        CmdLineString = &ProcessParameters->ImagePathName;
    }

    /* Save parameters for parsing */
    Source = CmdLineString->Buffer;
    Length = CmdLineString->Length;

    /* Ensure it's valid */
    if (Source)
    {
        /* Allocate a buffer for the destination */
        Destination = RtlAllocateHeap(g_Heap, 0, (Length + 1) * sizeof(WCHAR));
        if (!Destination)
            goto fail;

        /* Start parsing */
        while (*Source)
        {
            /* Skip the white space. */
            while (*Source && *Source <= L' ') Source++;

            /* Copy until the next white space is reached */
            if (*Source)
            {
                /* Save one token pointer */
                *ArgumentList++ = Destination;

                /* Increase one token count */
                argc++;

                /* Copy token until white space */
                while (*Source > L' ')
                    *Destination++ = *Source++;

                /* Null terminate it */
                *Destination++ = L'\0';
            }
        }
    }

    /* Null terminate the token pointer list */
    *ArgumentList++ = NULL;

    /* Now handle the enviornment, point the envp at our current list location. */
    envp = ArgumentList;

    /* Call the Main Function */
    Status = wmain(argc, argv, envp, 0);

fail:
    /* We're done here */
    NtTerminateProcess(NtCurrentProcess(), Status);
}
