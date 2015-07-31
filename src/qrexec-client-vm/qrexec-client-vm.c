// This program is used to trigger qrexec services in remote domains.
// It connects to local qrexec agent and sends it:
// trigger_service_params (service params), size_t (local handler path size), local handler path (WCHARs).
// Local handler will be launched as the local endpoint for the triggered service.

#include <windows.h>
#include <strsafe.h>

#include <qrexec.h>
#include <log.h>
#include <qubes-io.h>
#include <utf8-conv.h>
#include <pipe-server.h>

int __cdecl wmain(int argc, WCHAR *argv[])
{
    HANDLE readPipe, writePipe;
    BOOL success = FALSE;
    LPTSTR pipeName = L"\\\\.\\pipe\\qrexec_trigger";
    struct trigger_service_params triggerParams = { 0 };
    ULONG status;
    UCHAR *argumentUtf8;
    HRESULT hresult;
    size_t size;

    if (argc < 4)
    {
        wprintf(L"usage: %s <vm name> <qrexec service name> <local program> [local program arguments]\n", argv[0]);
        return ERROR_INVALID_PARAMETER;
    }

    LogDebug("domain '%s', service '%s', local command '%s'", argv[1], argv[2], argv[3]);

    // Prepare the parameter structure containing the first two arguments.
    argumentUtf8 = NULL;
    status = ConvertUTF16ToUTF8(argv[2], &argumentUtf8, NULL); // service name
    if (ERROR_SUCCESS != status)
        return perror2(status, "ConvertUTF16ToUTF8");

    hresult = StringCchCopyA(triggerParams.service_name, sizeof(triggerParams.service_name), argumentUtf8);
    if (FAILED(hresult))
        return perror2(hresult, "StringCchCopyA");

    ConvertFree(argumentUtf8);
    argumentUtf8 = NULL;

    status = ConvertUTF16ToUTF8(argv[1], &argumentUtf8, NULL); // vm name
    if (ERROR_SUCCESS != status)
        return perror2(status, "ConvertUTF16ToUTF8");

    hresult = StringCchCopyA(triggerParams.target_domain, sizeof(triggerParams.target_domain), argumentUtf8);
    if (FAILED(hresult))
        return perror2(hresult, "StringCchCopyA");

    ConvertFree(argumentUtf8);
    argumentUtf8 = NULL;

    LogDebug("Connecting to qrexec-agent");

    if (ERROR_SUCCESS != QpsConnect(pipeName, &readPipe, &writePipe))
        return perror("open agent pipe");

    CloseHandle(readPipe);
    LogDebug("Sending the parameters to qrexec-agent");

    if (!QioWriteBuffer(writePipe, &triggerParams, sizeof(triggerParams)))
        return perror("write trigger params to agent");

    size = (wcslen(argv[3]) + 1) * sizeof(WCHAR);
    if (!QioWriteBuffer(writePipe, &size, sizeof(size)))
        return perror("write command size to agent");

    if (!QioWriteBuffer(writePipe, argv[3], (DWORD)size))
        return perror("write command to agent");

    CloseHandle(writePipe);

    LogVerbose("exiting");

    return ERROR_SUCCESS;
}
