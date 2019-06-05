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

#include "device.h"
#include "disk.h"

#include <setupapi.h>
#include <cfgmgr32.h>
#include <rpc.h>
#include <devpkey.h>

#ifdef __MINGW32__
#include "customddkinc.h"
#include "setupapifn.h"
#endif

#include <strsafe.h>

// Convert backend device ID to Windows' disk target id.
// See __ParseVbd() in xenvbd/fdo.c in the new pvdrivers.
static ULONG BackendIdToTargetId(ULONG backendId)
{
    if ((backendId & ~((1 << 29) - 1)) != 0)
    {
        LogWarning("Invalid backend ID 0x%x", backendId);
        return 0xffffffff;
    }

    if (backendId & (1 << 28))
        return (backendId & ((1 << 20) - 1)) >> 8;           /* xvd    */

    switch (backendId >> 8)
    {
        case 202:   return (backendId & 0xF0) >> 4;          /* xvd    */
        case 8:     return (backendId & 0xF0) >> 4;          /* sd     */
        case 3:     return (backendId & 0xC0) >> 6;          /* hda..b */
        case 22:    return ((backendId & 0xC0) >> 6) + 2;    /* hdc..d */
        case 33:    return ((backendId & 0xC0) >> 6) + 4;    /* hde..f */
        case 34:    return ((backendId & 0xC0) >> 6) + 6;    /* hdg..h */
        case 56:    return ((backendId & 0xC0) >> 6) + 8;    /* hdi..j */
        case 57:    return ((backendId & 0xC0) >> 6) + 10;   /* hdk..l */
        case 88:    return ((backendId & 0xC0) >> 6) + 12;   /* hdm..n */
        case 89:    return ((backendId & 0xC0) >> 6) + 14;   /* hdo..p */
    }

    LogError("Invalid backend ID 0x%x", backendId);
    return 0xFFFFFFFF; // OBVIOUS ERROR VALUE
}

static BOOL IsPrivateDisk(IN WCHAR *locationString, IN ULONG privateId)
{
    const WCHAR *targetStr1 = L"Target Id "; // for PV disks
    const WCHAR *targetStr2 = L"Target "; // for emulated disks
    WCHAR *idStr = NULL;
    WCHAR *stopStr = NULL;
    ULONG targetId, id;
    size_t len;

    targetId = BackendIdToTargetId(privateId);
    if (targetId == 0xffffffff)
        return FALSE;

    // Location string is in this format: "Bus Number 0, Target Id 1, LUN 0"
    // FIXME: avoid string parsing. Although it's not localized it would be better to use something else.
    len = wcslen(targetStr1);
    idStr = wcsstr(locationString, targetStr1);
    if (!idStr)
    {
        len = wcslen(targetStr2);
        idStr = wcsstr(locationString, targetStr2);
    }

    if (!idStr)
        return FALSE;

    // skip to the Target ID itself
    idStr += len;

    id = wcstoul(idStr, &stopStr, 10);
    if (id != targetId)
        return FALSE;

    LogDebug("Target ID %lu in '%s' matches backend ID %lu", id, locationString, privateId);
    return TRUE;
}

// Returns physical drive number that represents private.img.
BOOL GetPrivateImgDriveNumber(IN ULONG xenVbdId, OUT ULONG *driveNumber)
{
    HDEVINFO deviceInfoSet = INVALID_HANDLE_VALUE;
    SP_DEVINFO_DATA deviceInfoData;
    DEVPROPTYPE devPropType;
    GUID diskClassGuid = { 0 };
    DWORD returnedSize;
    WCHAR deviceId[MAX_DEVICE_ID_LEN];
    WCHAR deviceName[1024];
    WCHAR locationString[1024];
    WCHAR *s;
    DWORD index;

    // Enumerate disk class devices.
    UuidFromString((RPC_WSTR) DISK_CLASS_GUID, &diskClassGuid);
    deviceInfoSet = SetupDiGetClassDevs(&diskClassGuid, NULL, NULL, DIGCF_PRESENT); // only connected devices

    if (deviceInfoSet == INVALID_HANDLE_VALUE)
    {
        perror("SetupDiGetClassDevs");
        return FALSE;
    }

    deviceInfoData.cbSize = sizeof(deviceInfoData);
    for (index = 0;
        SetupDiEnumDeviceInfo(deviceInfoSet, index, &deviceInfoData);
        index++)
    {
        if (!SetupDiGetDeviceInstanceId(deviceInfoSet, &deviceInfoData, deviceId, RTL_NUMBER_OF(deviceId), &returnedSize))
        {
            perror("SetupDiGetDeviceInstanceId");
            continue;
        }

        LogDebug("DEVID: %s", deviceId);

        // Get device's location info.
        ZeroMemory(locationString, sizeof(locationString));
        if (!SetupDiGetDeviceProperty(deviceInfoSet, &deviceInfoData, &DEVPKEY_Device_LocationInfo, &devPropType,
            (BYTE *)locationString, sizeof(locationString), &returnedSize, 0))
        {
            perror("SetupDiGetDeviceProperty(location)");
            continue;
        }

        // Check if it's the disk we want.
        if (!IsPrivateDisk(locationString, xenVbdId))
            continue;

        LogDebug("backend ID match");

        // Get the user mode device name.
        StringCchPrintf(deviceName, RTL_NUMBER_OF(deviceName), L"\\\\?\\%s\\" DISK_INTERFACE_GUID, deviceId);
        // Replace all backslashes with #.
        for (s = deviceName + 4; *s; s++)
            if (*s == L'\\')
                *s = '#';
        LogDebug("Device name: %s", deviceName);

        // Get physical drive number.
        return GetPhysicalDriveNumber(deviceName, driveNumber);
    }

    // No matching drive found.
    return FALSE;
}
