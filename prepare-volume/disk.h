#pragma once
#include "prepare-volume.h"

// MBR signature for private disk, basically unique disk ID for Windows
// http://technet.microsoft.com/en-us/library/cc976786.aspx
// DWORD, short for 'Qubes private disk image'
#define PRIVATE_IMG_SIGNATURE 'idpQ'

// string form needed for creating device names
#define DISK_INTERFACE_GUID	L"{53f56307-b6bf-11d0-94f2-00a0c91efb8b}"

extern WCHAR *DISK_CLASS_GUID;

// Returns drive number for a given device name that represents a physical drive.
BOOL GetPhysicalDriveNumber(IN WCHAR *deviceName, OUT ULONG *driveNumber);

// Returns the first volume residing on a given physical drive.
BOOL DriveNumberToVolumeName(IN DWORD driveNumber, OUT WCHAR *volumeName, IN DWORD volumeNameLength);

// Returns disk letter for private.img, initializes it if needed.
BOOL PreparePrivateVolume(IN ULONG driveNumber, OUT WCHAR *diskLetter);
