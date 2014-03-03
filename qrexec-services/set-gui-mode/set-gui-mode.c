#include <windows.h>
#include <tchar.h>

#define FULLSCREEN_EVENT_NAME TEXT("WGA_FULLSCREEN_SWITCH")

int APIENTRY _tWinMain(HINSTANCE hInst, HINSTANCE hPrevInst, LPTSTR lpCommandLine, int nCmdShow)
{
    HANDLE event = OpenEvent(EVENT_MODIFY_STATE, FALSE, FULLSCREEN_EVENT_NAME);
    if (!event)
        return GetLastError();
    SetEvent(event);
    return GetLastError(); 	
}
