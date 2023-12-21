/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (c) Invisible Things Lab
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include <windows.h>
#include <strsafe.h>
#include <string.h>

#define FULLSCREEN_ON_EVENT_NAME L"QUBES_GUI_AGENT_FULLSCREEN_ON"
#define FULLSCREEN_ON_COMMAND "FULLSCREEN"
#define FULLSCREEN_OFF_EVENT_NAME L"QUBES_GUI_AGENT_FULLSCREEN_OFF"
#define FULLSCREEN_OFF_COMMAND "SEAMLESS"

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE previousInstance, WCHAR *commandLine, int showState)
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
