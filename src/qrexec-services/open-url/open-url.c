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
#include <qubes-io.h>
#include <log.h>

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE previousInstance, WCHAR *commandLine, int showState)
{
    char *Url;
    const DWORD BufferSize = 4096;
    DWORD ReadSize;

    Url = malloc(BufferSize);
    if (Url == NULL)
        return ERROR_NOT_ENOUGH_MEMORY;

    ReadSize = QioReadUntilEof(GetStdHandle(STD_INPUT_HANDLE), Url, BufferSize);
    if (ReadSize == 0)
    {
        LogError("empty request");
        return ERROR_INVALID_PARAMETER;
    }

    LogDebug("request: %S", Url);

    if ((int)ShellExecuteA(NULL, "open", Url, NULL, NULL, SW_SHOWNORMAL) <= 32)
    {
        perror("ShellExecute failed");
        return GetLastError();
    }

    return ERROR_SUCCESS;
}
