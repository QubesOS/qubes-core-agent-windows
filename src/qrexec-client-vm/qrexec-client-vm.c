#include <windows.h>
#include <strsafe.h>

#include <qrexec.h>
#include <log.h>
#include <qubes-io.h>
#include <utf8-conv.h>

int __cdecl wmain(int argc, WCHAR *argv[])
{
    HANDLE agentPipe;
    BOOL success = FALSE;
    LPTSTR pipeName = L"\\\\.\\pipe\\qrexec_trigger";
    struct trigger_service_params triggerParams = { 0 };
    ULONG status;
    UCHAR *argumentUtf8;
    HRESULT hresult;

    if (argc < 4)
    {
        wprintf(L"usage: %s <vm name> <qrexec service name> <local program> [local program arguments]\n", argv[0]);
        return ERROR_INVALID_PARAMETER;
    }

    // Prepare the parameter structure containing the first two arguments.
    argumentUtf8 = NULL;
    status = ConvertUTF16ToUTF8(argv[2], &argumentUtf8, NULL); // local program
    if (ERROR_SUCCESS != status)
        return perror2(status, "ConvertUTF16ToUTF8");

    hresult = StringCchCopyA(triggerParams.service_name, sizeof(triggerParams.service_name), argumentUtf8);
    if (FAILED(hresult))
        return perror2(hresult, "StringCchCopyA");

    free(argumentUtf8);
    argumentUtf8 = NULL;

    status = ConvertUTF16ToUTF8(argv[1], &argumentUtf8, NULL); // vm name
    if (ERROR_SUCCESS != status)
        return perror2(status, "ConvertUTF16ToUTF8");

    hresult = StringCchCopyA(triggerParams.target_domain, sizeof(triggerParams.target_domain), argumentUtf8);
    if (FAILED(hresult))
        return perror2(hresult, "StringCchCopyA");

    free(argumentUtf8);
    argumentUtf8 = NULL;

    LogDebug("Connecting to qrexec-agent");

    // Try to open a named pipe; wait for it, if necessary.

    while (TRUE)
    {
        agentPipe = CreateFile(
            pipeName,
            GENERIC_READ | GENERIC_WRITE,
            0,
            NULL,
            OPEN_EXISTING,
            0,
            NULL);

        if (agentPipe != INVALID_HANDLE_VALUE)
            break;

        // Exit if an error other than ERROR_PIPE_BUSY occurs.
        status = GetLastError();
        if (ERROR_PIPE_BUSY != status)
            return perror2(status, "CreateFile(agent pipe)");

        // All pipe instances are busy, so wait for 10 seconds.
        if (!WaitNamedPipe(pipeName, 10000))
            return perror("WaitNamedPipe");
    }

    LogDebug("Sending the parameters to qrexec-agent");

    if (!QioWriteBuffer(agentPipe, &triggerParams, sizeof(triggerParams)))
        return perror("write to agent");

    CloseHandle(agentPipe);

    LogVerbose("exiting");

    return ERROR_SUCCESS;
}
