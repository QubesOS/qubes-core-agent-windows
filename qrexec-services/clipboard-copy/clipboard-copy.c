#include <windows.h>
#include <Shlwapi.h>
#include <strsafe.h>
#include <stdlib.h>

#include "ioall.h"
#include "utf8-conv.h"
#include "log.h"

#define CLIPBOARD_FORMAT CF_UNICODETEXT

BOOL WriteClipboardText(IN HWND window, OUT HANDLE outputFile)
{
    HANDLE clipData;
    WCHAR *clipText;
    UCHAR *clipTextUtf8;
    size_t cbTextUtf8;

    if (!IsClipboardFormatAvailable(CLIPBOARD_FORMAT))
        return FALSE;

    if (!OpenClipboard(window))
    {
        perror("OpenClipboard");
        return FALSE;
    }

    clipData = GetClipboardData(CLIPBOARD_FORMAT);
    if (!clipData)
    {
        perror("GetClipboardData");
        CloseClipboard();
        return FALSE;
    }

    clipText = GlobalLock(clipData);
    if (!clipText)
    {
        perror("GlobalLock");
        CloseClipboard();
        return FALSE;
    }

    if (FAILED(ConvertUTF16ToUTF8(clipText, &clipTextUtf8, &cbTextUtf8)))
    {
        perror("ConvertUTF16ToUTF8");
        GlobalUnlock(clipData);
        CloseClipboard();
        return FALSE;
    }

    if (!FcWriteBuffer(outputFile, clipTextUtf8, cbTextUtf8))
    {
        LogError("write failed");
        GlobalUnlock(clipData);
        CloseClipboard();
        return FALSE;
    }

    GlobalUnlock(clipData);
    CloseClipboard();
    return TRUE;
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE previousInstance, WCHAR *commandLine, int showFlags)
{
    HANDLE stdOut;

    stdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (stdOut == INVALID_HANDLE_VALUE)
    {
        perror("GetStdHandle");
        return 1;
    }
    if (!WriteClipboardText(NULL, stdOut))
    {
        return 1;
    }

    LogDebug("all ok");
    return 0;
}
