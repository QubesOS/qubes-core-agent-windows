#include <windows.h>
#include <tchar.h>
#include <strsafe.h>
#include <string.h>

#define FULLSCREEN_ON_EVENT_NAME TEXT("WGA_FULLSCREEN_ON")
#define FULLSCREEN_ON_COMMAND "FULLSCREEN"
#define FULLSCREEN_OFF_EVENT_NAME TEXT("WGA_FULLSCREEN_OFF")
#define FULLSCREEN_OFF_COMMAND "SEAMLESS"

int APIENTRY _tWinMain(HINSTANCE hInst, HINSTANCE hPrevInst, TCHAR *lpCommandLine, int nCmdShow)
{
    char param[64] = { 0 };
    DWORD size;

    if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), param, RTL_NUMBER_OF(param) - 1, &size, NULL))
        return GetLastError();

    if (_strnicmp(param, FULLSCREEN_ON_COMMAND, strlen(FULLSCREEN_ON_COMMAND)) == 0)
    {
        HANDLE event = OpenEvent(EVENT_MODIFY_STATE, FALSE, FULLSCREEN_ON_EVENT_NAME);
        if (!event)
            return GetLastError();
        SetEvent(event);
        return GetLastError();
    }

    if (_strnicmp(param, FULLSCREEN_OFF_COMMAND, strlen(FULLSCREEN_OFF_COMMAND)) == 0)
    {
        HANDLE event = OpenEvent(EVENT_MODIFY_STATE, FALSE, FULLSCREEN_OFF_EVENT_NAME);
        if (!event)
            return GetLastError();
        SetEvent(event);
        return GetLastError();
    }

    return ERROR_INVALID_PARAMETER;
}
