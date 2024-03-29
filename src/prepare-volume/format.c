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

#include "format.h"
#include "disk.h"

static DWORD g_FormatStatus = 0;
static FormatEx_t g_FormatEx = NULL;
static HMODULE g_fmifsDll = NULL;

static BOOLEAN WINAPI FormatExCallback(FILE_SYSTEM_CALLBACK_COMMAND Command, DWORD Action, void *pData)
{
    switch (Command)
    {
    case FCC_DONE:
        if (*(BOOLEAN*) pData == FALSE)
        {
            LogError("Error while formatting");
            g_FormatStatus = 1;
        }
        else
        {
            LogInfo("format done");
            g_FormatStatus = 0;
        }
        break;

    case FCC_STRUCTURE_PROGRESS:
        LogDebug("FCC_STRUCTURE_PROGRESS");
        break;

    case FCC_DONE_WITH_STRUCTURE:
        LogDebug("FCC_DONE_WITH_STRUCTURE");
        break;

    case FCC_INCOMPATIBLE_FILE_SYSTEM:
        LogError("Incompatible File System");
        g_FormatStatus = 1;
        break;
    case FCC_ACCESS_DENIED:
        LogError("Access denied");
        g_FormatStatus = ERROR_ACCESS_DENIED;
        break;
    case FCC_MEDIA_WRITE_PROTECTED:
        LogError("Media is write protected");
        g_FormatStatus = ERROR_WRITE_PROTECT;
        break;
    case FCC_VOLUME_IN_USE:
        LogError("Volume is in use");
        g_FormatStatus = ERROR_DEVICE_IN_USE;
        break;
    case FCC_DEVICE_NOT_READY:
        LogError("The device is not ready");
        g_FormatStatus = ERROR_NOT_READY;
        break;
    case FCC_CANT_QUICK_FORMAT:
        LogError("Cannot quick format this volume");
        g_FormatStatus = 1;
        break;
    case FCC_BAD_LABEL:
        LogError("Bad label");
        g_FormatStatus = ERROR_LABEL_TOO_LONG;
        break;
        break;
    case FCC_CLUSTER_SIZE_TOO_BIG:
    case FCC_CLUSTER_SIZE_TOO_SMALL:
        LogError("Unsupported cluster size");
        g_FormatStatus = 1;
        break;
    case FCC_VOLUME_TOO_BIG:
    case FCC_VOLUME_TOO_SMALL:
        LogError("Volume is too %s", (Command == FCC_VOLUME_TOO_BIG) ? "big" : "small");
        g_FormatStatus = 1;
        break;
    case FCC_NO_MEDIA_IN_DRIVE:
        LogError("No media in drive");
        g_FormatStatus = ERROR_NO_MEDIA_IN_DRIVE;
        break;
    default:
        LogWarning("Received unhandled command 0x02%X", Command);
        break;
    }
    return (!IS_ERROR(g_FormatStatus));
}

// Format the volume created by InitializeDisk.
BOOL FormatVolume(IN DWORD diskNumber)
{
    WCHAR volumeName[MAX_PATH];

    // First, we need to obtain the volume handle.
    if (DriveNumberToVolumeName(diskNumber, volumeName, RTL_NUMBER_OF(volumeName)))
    {
        // Remove trailing backslash, FormatEx fails with it. Hurrah for consistency...
        volumeName[wcslen(volumeName) - 1] = 0;
        LogInfo("Formatting volume: %s", volumeName);

        // Proceed with format.

        if (!g_fmifsDll)
        {
            g_fmifsDll = LoadLibrary(L"fmifs");
            if (!g_fmifsDll)
            {
                win_perror("LoadLibrary(fmifs)");
                return FALSE;
            }
        }

        if (!g_FormatEx)
        {
            g_FormatEx = (FormatEx_t) GetProcAddress(g_fmifsDll, "FormatEx");
            if (!g_FormatEx)
            {
                win_perror("GetProcAddress(fmifs, FormatEx)");
                return FALSE;
            }
        }

        // This call is synchronous.
        g_FormatEx(volumeName, FixedMedia, L"NTFS", L"Qubes Private Image", TRUE, 0, FormatExCallback);
    }

    return g_FormatStatus == 0;
}
