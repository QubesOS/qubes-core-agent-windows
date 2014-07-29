#include "device.h"
#include "disk.h"

#include <SetupAPI.h>
#include <cfgmgr32.h>
#include <rpc.h>
#include <devpkey.h>

static BOOL IsPrivateDisk(IN WCHAR *parentString, IN ULONG privateId)
{
	WCHAR *vbdPrefix = L"xen\\vbd\\";
	WCHAR *idStr = NULL;
	WCHAR *stopStr = NULL;
	ULONG id;
	// Value for xen-provided PV disks is in this format:
	// xen\vbd\4&32fe5319&0&51728
	// Last part is the ID.
	if (0 == wcsncmp(parentString, vbdPrefix, wcslen(vbdPrefix)))
	{
		idStr = wcsrchr(parentString, L'&');
		if (idStr && *idStr && *(idStr + 1))
		{
			idStr++;
			id = wcstoul(idStr, &stopStr, 10);
			if (id == privateId)
				return TRUE;
		}
	}
	return FALSE;
}

// Returns physical drive number that represents private.img.
BOOL GetPrivateImgDriveNumber(IN ULONG xenVbdId, OUT PULONG driveNumber)
{
	HDEVINFO deviceInfoSet = INVALID_HANDLE_VALUE;
	SP_DEVINFO_DATA deviceInfoData;
	DEVPROPTYPE devPropType;
	GUID diskClassGuid = { 0 };
	DWORD returnedSize;
	WCHAR deviceId[MAX_DEVICE_ID_LEN];
	WCHAR deviceName[1024];
	WCHAR parentString[1024];
	WCHAR *s;
	DWORD index;

	// Enumerate disk class devices.
	UuidFromString((RPC_WSTR)DISK_CLASS_GUID, &diskClassGuid);
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

		// Get device's parent.
		ZeroMemory(parentString, sizeof(parentString));
		if (!SetupDiGetDeviceProperty(deviceInfoSet, &deviceInfoData, &DEVPKEY_Device_Parent, &devPropType, (PBYTE)parentString, sizeof(parentString), &returnedSize, 0))
		{
			perror("SetupDiGetDeviceProperty(parent)");
			continue;
		}

		// Check if it's the disk we want.
		if (!IsPrivateDisk(parentString, xenVbdId))
			continue;

        LogDebug("Xen VBD match");

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
