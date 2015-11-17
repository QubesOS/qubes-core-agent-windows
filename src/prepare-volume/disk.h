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

// string form needed for creating device names
#define DISK_INTERFACE_GUID	L"{53f56307-b6bf-11d0-94f2-00a0c91efb8b}"

extern WCHAR *DISK_CLASS_GUID;

// Returns drive number for a given device name that represents a physical drive.
BOOL GetPhysicalDriveNumber(IN WCHAR *deviceName, OUT ULONG *driveNumber);

// Returns the first volume residing on a given physical drive.
BOOL DriveNumberToVolumeName(IN DWORD driveNumber, OUT WCHAR *volumeName, IN DWORD volumeNameLength);

// Returns disk letter for private.img, initializes it if needed.
BOOL PreparePrivateVolume(IN ULONG driveNumber, OUT WCHAR *diskLetter);
