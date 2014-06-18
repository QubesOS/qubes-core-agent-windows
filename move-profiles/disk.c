#include "disk.h"
#include "format.h"
#include "wait-for-volume.h"

// Definitions from winioctl.h/ntdddisk.h, with these includes fails to build with DDK
#define IOCTL_DISK_SET_DISK_ATTRIBUTES      CTL_CODE(IOCTL_DISK_BASE, 0x003d, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

#define DISK_ATTRIBUTE_OFFLINE              0x0000000000000001
#define DISK_ATTRIBUTE_READ_ONLY            0x0000000000000002

typedef struct _SET_DISK_ATTRIBUTES {
    ULONG Version;
    BOOLEAN Persist;
    BOOLEAN RelinquishOwnership;
    BOOLEAN Reserved1[2];
    ULONGLONG Attributes;
    ULONGLONG AttributesMask;
    GUID Owner;
} SET_DISK_ATTRIBUTES, *PSET_DISK_ATTRIBUTES;

// without braces
PWCHAR DISK_CLASS_GUID = L"4d36e967-e325-11ce-bfc1-08002be10318";

// Returns drive number for a given device name that represents a physical drive.
BOOL GetPhysicalDriveNumber(IN WCHAR *deviceName, OUT PULONG driveNumber)
{
	STORAGE_DEVICE_NUMBER deviceNumber;
	DWORD returnedSize;
	BOOL retval = FALSE;
	HANDLE device = CreateFile(deviceName, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);

	if (device == INVALID_HANDLE_VALUE)
	{
		perror("CreateFile");
		goto cleanup;
	}

	// Get device number.
	if (!DeviceIoControl(device, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &deviceNumber, sizeof(deviceNumber), &returnedSize, NULL))
	{
		perror("IOCTL_STORAGE_GET_DEVICE_NUMBER");
		goto cleanup;
	}

	logf("device: %d, partition: %d", deviceNumber.DeviceNumber, deviceNumber.PartitionNumber);
	retval = TRUE;
	*driveNumber = deviceNumber.DeviceNumber;

cleanup:
	if (device != INVALID_HANDLE_VALUE)
		CloseHandle(device);
	return retval;
}

// Returns disk letter if a given volume is mounted.
static BOOL VolumeNameToDiskLetter(IN WCHAR *volumeName, OUT PWCHAR diskLetter)
{
	WCHAR *mountPoints = NULL;
	WCHAR *mountPoint = NULL;
	DWORD returned = 1024;
	BOOL retval = FALSE;

	mountPoints = (WCHAR*)malloc(returned * sizeof(WCHAR));
	ZeroMemory(mountPoints, returned*sizeof(WCHAR));
	if (mountPoints && !GetVolumePathNamesForVolumeName(volumeName, mountPoints, returned, &returned))
	{
		if (GetLastError() == ERROR_MORE_DATA)
		{
			free(mountPoints);
			mountPoints = (WCHAR*)malloc(returned * sizeof(WCHAR));
		}
	}

	if (!mountPoints)
	{
		errorf("No memory");
		return FALSE;
	}

	ZeroMemory(mountPoints, returned*sizeof(WCHAR));
	if (!GetVolumePathNamesForVolumeName(volumeName, mountPoints, returned, &returned))
	{
		perror("GetVolumePathNamesForVolumeName");
		free(mountPoints);
		return FALSE;
	}

	for (mountPoint = mountPoints; *mountPoint; mountPoint += wcslen(mountPoint) + 1)
	{
		logf("Mount point: %s", mountPoint);
		// Check if it's a disk letter.
		if (mountPoint[1] == L':' && mountPoint[2] == L'\\')
		{
			*diskLetter = mountPoint[0];
			retval = TRUE;
			break;
		}
	}

	free(mountPoints);
	return retval;
}

// Returns the first volume residing on a given physical drive.
BOOL DriveNumberToVolumeName(IN DWORD driveNumber, OUT WCHAR *volumeName, IN DWORD volumeNameLength)
{
	HANDLE findHandle;
	HANDLE volume;
	DWORD returnedSize;
	VOLUME_DISK_EXTENTS extents; // Variable size, but we need only one disk.
	BOOL retval = FALSE;

	// Enumerate all volumes and check which disk they belong to.
	findHandle = FindFirstVolume(volumeName, volumeNameLength);
	if (findHandle == INVALID_HANDLE_VALUE)
	{
		perror("FindFirstVolume");
		return FALSE;
	}

	do
	{
		volumeName[wcslen(volumeName) - 1] = 0; // Remove trailing backslash.
		debugf("Volume: %s", volumeName);

		// Open device handle.
		volume = CreateFile(volumeName, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
		if (volume == INVALID_HANDLE_VALUE)
		{
			perror("open volume");
			goto skip;
		}

		// Get volume disk extents.
		if (!DeviceIoControl(volume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, &extents, sizeof(extents), &returnedSize, NULL))
		{
			perror("IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS");
			goto skip;
		}

		if (extents.NumberOfDiskExtents != 1) // >1 for various RAID configurations
			goto skip;

		if (extents.Extents[0].DiskNumber == driveNumber) // match
		{
			CloseHandle(volume);
			retval = TRUE;
			volumeName[wcslen(volumeName)] = L'\\'; // Re-add trailing backslash.
			logf("Found volume '%s' for disk %d", volumeName, driveNumber);
			break;
		}

	skip:
		if (volume != INVALID_HANDLE_VALUE)
			CloseHandle(volume);
		ZeroMemory(volumeName, volumeNameLength);
	} while (FindNextVolume(findHandle, volumeName, volumeNameLength));

	FindVolumeClose(findHandle);

	return retval;
}

// Make sure the disk is initialized and partitioned.
static BOOL InitializeDisk(IN HANDLE device, IN LARGE_INTEGER diskSize, OUT PWCHAR diskLetter)
{
	DWORD requiredSize;
	BYTE partitionBuffer[sizeof(DRIVE_LAYOUT_INFORMATION_EX)+4 * sizeof(PARTITION_INFORMATION_EX)] = { 0 };
	PDRIVE_LAYOUT_INFORMATION_EX driveLayout = (PDRIVE_LAYOUT_INFORMATION_EX)partitionBuffer;
	SET_DISK_ATTRIBUTES diskAttrs = { 0 };
	CREATE_DISK createDisk;

	// Set disk attributes.
	diskAttrs.Version = sizeof(diskAttrs);
	diskAttrs.AttributesMask = DISK_ATTRIBUTE_OFFLINE | DISK_ATTRIBUTE_READ_ONLY;
	diskAttrs.Attributes = 0; // clear
	diskAttrs.Persist = TRUE;

	if (!DeviceIoControl(device, IOCTL_DISK_SET_DISK_ATTRIBUTES, &diskAttrs, sizeof(diskAttrs), NULL, 0, &requiredSize, NULL))
	{
		perror("IOCTL_DISK_SET_DISK_ATTRIBUTES");
		return FALSE;
	}

	// Tell the system that the disk was changed.
	if (!DeviceIoControl(device, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &requiredSize, NULL))
	{
		perror("IOCTL_DISK_UPDATE_PROPERTIES");
		return FALSE;
	}

	// Initialize the disk.
	createDisk.PartitionStyle = PARTITION_STYLE_MBR;
	createDisk.Mbr.Signature = PRIVATE_IMG_SIGNATURE;

	if (!DeviceIoControl(device, IOCTL_DISK_CREATE_DISK, &createDisk, sizeof(createDisk), NULL, 0, &requiredSize, NULL))
	{
		perror("IOCTL_DISK_CREATE_DISK");
		return FALSE;
	}

	logf("Disk initialized OK, signature: 0x%08x", PRIVATE_IMG_SIGNATURE);

	if (!DeviceIoControl(device, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &requiredSize, NULL))
	{
		perror("IOCTL_DISK_UPDATE_PROPERTIES");
		return FALSE;
	}

	// Create partition table.
	driveLayout->PartitionStyle = PARTITION_STYLE_MBR;
	driveLayout->PartitionCount = 4;
	driveLayout->Mbr.Signature = PRIVATE_IMG_SIGNATURE;

	driveLayout->PartitionEntry[0].PartitionStyle = PARTITION_STYLE_MBR;
	driveLayout->PartitionEntry[0].StartingOffset.QuadPart = 63 * 512; // FIXME: use disk geometry numbers
	driveLayout->PartitionEntry[0].PartitionLength.QuadPart = diskSize.QuadPart - 63 * 512; // make sure to subtract MBR size
	driveLayout->PartitionEntry[0].PartitionNumber = 1; // 1-based
	driveLayout->PartitionEntry[0].RewritePartition = TRUE;

	driveLayout->PartitionEntry[0].Mbr.PartitionType = PARTITION_IFS;
	driveLayout->PartitionEntry[0].Mbr.RecognizedPartition = TRUE;
	driveLayout->PartitionEntry[0].Mbr.BootIndicator = FALSE;
	driveLayout->PartitionEntry[0].Mbr.HiddenSectors = 63; // MBR size

	driveLayout->PartitionEntry[1].RewritePartition = TRUE;
	driveLayout->PartitionEntry[2].RewritePartition = TRUE;
	driveLayout->PartitionEntry[3].RewritePartition = TRUE;

	if (!DeviceIoControl(device, IOCTL_DISK_SET_DRIVE_LAYOUT_EX, partitionBuffer, sizeof(partitionBuffer), NULL, 0, &requiredSize, NULL))
	{
		perror("IOCTL_DISK_SET_DRIVE_LAYOUT_EX");
		return FALSE;
	}

	if (!DeviceIoControl(device, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &requiredSize, NULL))
	{
		perror("IOCTL_DISK_UPDATE_PROPERTIES");
		return FALSE;
	}

	// Wait for the new volume to be recognized by the system.
	*diskLetter = WaitForVolumeArrival();
	if (*diskLetter == L'\0')
	{
		errorf("WaitForVolumeArrival failed");
		return FALSE;
	}

	logf("New volume letter: %c", *diskLetter);

	return TRUE;
}

// Returns disk letter for private.img, initializes it if needed.
BOOL PreparePrivateVolume(IN ULONG driveNumber, OUT PWCHAR diskLetter)
{
	HANDLE device;
	DWORD returnedSize;
	BYTE buf[1024] = { 0 };
	PDRIVE_LAYOUT_INFORMATION_EX layout = (PDRIVE_LAYOUT_INFORMATION_EX)buf;
	GET_LENGTH_INFORMATION lengthInfo;
	WCHAR driveName[32] = { 0 };
	WCHAR volumeName[MAX_PATH] = { 0 };
	WCHAR filesystemName[MAX_PATH + 1];
	BOOL reinitialize = FALSE;
	BOOL retval = FALSE;
	WCHAR newDiskLetter;

	// Open the device.
	StringCchPrintf(driveName, RTL_NUMBER_OF(driveName), L"\\\\.\\PhysicalDrive%d", driveNumber);
	logf("Opening %s", driveName);

	device = CreateFile(driveName, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (device == INVALID_HANDLE_VALUE)
	{
		perror("CreateFile");
		return FALSE;
	}

	// Get partition information.
	if (!DeviceIoControl(device, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0, buf, sizeof(buf), &returnedSize, NULL))
	{
		perror("IOCTL_DISK_GET_DRIVE_LAYOUT_EX");
		goto cleanup;
	}

	logf("Partition type: %d, count: %d", layout->PartitionStyle, layout->PartitionCount);
	if (layout->PartitionStyle != PARTITION_STYLE_MBR)
	{
		errorf("Not a MBR disk");
		goto cleanup;
	}

	logf("MBR signature: 0x%08x", layout->Mbr.Signature);

	if (layout->Mbr.Signature != 0)
	{
		// Drive is already initialized. Check if everything is OK.
		logf("Drive is already initialized with signature 0x%08x", layout->Mbr.Signature);

        // Don't change the signature if MBR is already there.
        // This can cause Windows to change drive number.

		// Check if the disk contains formatted volume.
		if (DriveNumberToVolumeName(driveNumber, volumeName, RTL_NUMBER_OF(volumeName)))
		{
			logf("Drive contains volume: %s", volumeName);
			// Check if the volume is mounted.
			if (VolumeNameToDiskLetter(volumeName, diskLetter))
			{
				logf("Volume is mounted as disk '%c:'", *diskLetter);

				// Check the filesystem.
				if (!GetVolumeInformation(volumeName, NULL, 0, NULL, NULL, NULL, filesystemName, RTL_NUMBER_OF(filesystemName)))
				{
					perror("GetVolumeInformation");
					reinitialize = TRUE;
				}
				else
				{
					logf("Volume '%s' filesystem: %s", volumeName, filesystemName);

					if (0 != wcsncmp(L"NTFS", filesystemName, 4))
					{
						errorf("Volume is formatted with a wrong filesystem, reformatting");
						reinitialize = TRUE;
					}
					else
					{
						// Volume is ready.
						CloseHandle(device);
						logf("Volume is ready");
						return TRUE;
					}
				}
			}
			else
			{
				logf("Volume doesn't seem to be mounted, reinitializing");
				reinitialize = TRUE;
			}
		}
		else
		{
			logf("Drive doesn't contain a proper volume, proceeding with initialization");
			reinitialize = TRUE;
		}
	}

	if (layout->Mbr.Signature == 0 || layout->PartitionCount == 0 || reinitialize)
	{
		// Get disk size.
		if (!DeviceIoControl(device, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &lengthInfo, sizeof(lengthInfo), &returnedSize, NULL))
		{
			perror("IOCTL_DISK_GET_LENGTH_INFO");
			goto cleanup;
		}

		logf("Disk size: %I64d", lengthInfo.Length.QuadPart);

		logf("Initializing disk %d", driveNumber);
		if (!InitializeDisk(device, lengthInfo.Length, &newDiskLetter))
		{
			errorf("Failed to initialize disk");
			goto cleanup;
		}
		else
		{
			logf("New volume letter reported by device notification: %c", newDiskLetter);
			// Format disk.
			CloseHandle(device);
			if (!FormatVolume(driveNumber))
			{
				errorf("Failed to format volume");
				return FALSE;
			}

			// Get the new volume name.
			if (DriveNumberToVolumeName(driveNumber, volumeName, RTL_NUMBER_OF(volumeName)))
			{
				logf("New volume: %s", volumeName);
				// Get the disk letter to double-check.
				if (VolumeNameToDiskLetter(volumeName, diskLetter))
				{
					if (*diskLetter != newDiskLetter)
					{
						errorf("Volume disk letter mismatch: device notification returned '%c' but actual letter is '%c'", newDiskLetter, diskLetter);
						return FALSE;
					}

					logf("New volume is mounted as disk '%c:'", *diskLetter);
					return TRUE;
				}
				else
				{
					errorf("Volume doesn't seem to be mounted");
					return FALSE;
				}
			}
			else
			{
				errorf("Failed to format volume");
				return FALSE;
			}
		}
	}

cleanup:
	CloseHandle(device);

	return FALSE;
}
