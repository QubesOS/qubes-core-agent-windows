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

#pragma once
#include "prepare-volume.h"
#include <winioctl.h>

typedef enum
{
    FCC_PROGRESS,
    FCC_DONE_WITH_STRUCTURE,
    FCC_UNKNOWN2,
    FCC_INCOMPATIBLE_FILE_SYSTEM,
    FCC_UNKNOWN4,
    FCC_UNKNOWN5,
    FCC_ACCESS_DENIED,
    FCC_MEDIA_WRITE_PROTECTED,
    FCC_VOLUME_IN_USE,
    FCC_CANT_QUICK_FORMAT,
    FCC_UNKNOWNA,
    FCC_DONE,
    FCC_BAD_LABEL,
    FCC_UNKNOWND,
    FCC_OUTPUT,
    FCC_STRUCTURE_PROGRESS,
    FCC_CLUSTER_SIZE_TOO_SMALL,
    FCC_CLUSTER_SIZE_TOO_BIG,
    FCC_VOLUME_TOO_SMALL,
    FCC_VOLUME_TOO_BIG,
    FCC_NO_MEDIA_IN_DRIVE,
    FCC_UNKNOWN15,
    FCC_UNKNOWN16,
    FCC_UNKNOWN17,
    FCC_DEVICE_NOT_READY,
    FCC_CHECKDISK_PROGRESS,
    FCC_UNKNOWN1A,
    FCC_UNKNOWN1B,
    FCC_UNKNOWN1C,
    FCC_UNKNOWN1D,
    FCC_UNKNOWN1E,
    FCC_UNKNOWN1F,
    FCC_READ_ONLY_MODE,
} FILE_SYSTEM_CALLBACK_COMMAND;

typedef BOOLEAN(WINAPI *FILE_SYSTEM_CALLBACK)(
    FILE_SYSTEM_CALLBACK_COMMAND Command,
    ULONG Action,
    void * pData
    );

typedef void(WINAPI *FormatEx_t)(
    WCHAR *DriveRoot,
    MEDIA_TYPE MediaType,		// See WinIoCtl.h
    WCHAR *FileSystemTypeName,
    WCHAR *Label,
    BOOL QuickFormat,
    ULONG DesiredUnitAllocationSize,
    FILE_SYSTEM_CALLBACK Callback
    );

// Format the volume created by InitializeDisk.
BOOL FormatVolume(IN DWORD diskNumber);
