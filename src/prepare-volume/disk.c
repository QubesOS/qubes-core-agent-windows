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

#include "disk.h"
#include "format.h"
#include "wait-for-volume.h"
#ifdef __MINGW32__
#include "customddkinc.h"
#endif

#include <stdlib.h>
#include <strsafe.h>

// without braces
PWCHAR DISK_CLASS_GUID = L"4d36e967-e325-11ce-bfc1-08002be10318";

// Returns drive number for a given device name that represents a physical drive.
BOOL GetPhysicalDriveNumber(IN WCHAR *deviceName, OUT ULONG *driveNumber)
{
    STORAGE_DEVICE_NUMBER deviceNumber;
    DWORD returnedSize;
    BOOL retval = FALSE;
    HANDLE device = CreateFile(deviceName, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);

    if (device == INVALID_HANDLE_VALUE)
    {
        win_perror("CreateFile");
        goto cleanup;
    }

    // Get device number.
    if (!DeviceIoControl(device, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &deviceNumber, sizeof(deviceNumber), &returnedSize, NULL))
    {
        win_perror("IOCTL_STORAGE_GET_DEVICE_NUMBER");
        goto cleanup;
    }

    LogDebug("device: %d, partition: %d", deviceNumber.DeviceNumber, deviceNumber.PartitionNumber);
    retval = TRUE;
    *driveNumber = deviceNumber.DeviceNumber;

cleanup:
    if (device != INVALID_HANDLE_VALUE)
        CloseHandle(device);
    return retval;
}

// Returns disk letter if a given volume is mounted.
static BOOL VolumeNameToDiskLetter(IN WCHAR *volumeName, OUT WCHAR *diskLetter)
{
    WCHAR *mountPoints = NULL;
    WCHAR *mountPoint = NULL;
    DWORD returned = 1024;
    BOOL retval = FALSE;

    mountPoints = (WCHAR *) malloc(returned * sizeof(WCHAR));
    ZeroMemory(mountPoints, returned*sizeof(WCHAR));
    if (mountPoints && !GetVolumePathNamesForVolumeName(volumeName, mountPoints, returned, &returned))
    {
        if (GetLastError() == ERROR_MORE_DATA)
        {
            free(mountPoints);
            mountPoints = (WCHAR*) malloc(returned * sizeof(WCHAR));
        }
    }

    if (!mountPoints)
    {
        LogError("No memory");
        return FALSE;
    }

    ZeroMemory(mountPoints, returned*sizeof(WCHAR));
    if (!GetVolumePathNamesForVolumeName(volumeName, mountPoints, returned, &returned))
    {
        win_perror("GetVolumePathNamesForVolumeName");
        free(mountPoints);
        return FALSE;
    }

    for (mountPoint = mountPoints; *mountPoint; mountPoint += wcslen(mountPoint) + 1)
    {
        LogDebug("Mount point: %s", mountPoint);
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
        win_perror("FindFirstVolume");
        return FALSE;
    }

    do
    {
        volumeName[wcslen(volumeName) - 1] = 0; // Remove trailing backslash.
        LogDebug("Volume: %s", volumeName);

        // Open device handle.
        volume = CreateFile(volumeName, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (volume == INVALID_HANDLE_VALUE)
        {
            win_perror("open volume");
            goto skip;
        }

        // Get volume disk extents.
        if (!DeviceIoControl(volume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, &extents, sizeof(extents), &returnedSize, NULL))
        {
            win_perror("IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS");
            goto skip;
        }

        if (extents.NumberOfDiskExtents != 1) // >1 for various RAID configurations
            goto skip;

        if (extents.Extents[0].DiskNumber == driveNumber) // match
        {
            CloseHandle(volume);
            retval = TRUE;
            volumeName[wcslen(volumeName)] = L'\\'; // Re-add trailing backslash.
            LogInfo("Found volume '%s' for disk %d", volumeName, driveNumber);
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

static ULONG Rand32(void)
{
    ULONG x;

    x =   rand() & 0xff;
    x |= (rand() & 0xff) << 0x08;
    x |= (rand() & 0xff) << 0x10;
    x |= (rand() & 0xff) << 0x18;

    return x;
}

// Make sure the disk is initialized and partitioned.
static BOOL InitializeDisk(IN HANDLE device, IN LARGE_INTEGER diskSize, OUT WCHAR *diskLetter)
{
    DWORD requiredSize;
    DWORD returnedSize;
    BYTE partitionBuffer[sizeof(DRIVE_LAYOUT_INFORMATION_EX) + 4 * sizeof(PARTITION_INFORMATION_EX)] = { 0 };
    DRIVE_LAYOUT_INFORMATION_EX *driveLayout = (DRIVE_LAYOUT_INFORMATION_EX *) partitionBuffer;
    SET_DISK_ATTRIBUTES diskAttrs = { 0 };
    CREATE_DISK createDisk;
    ULONG signature = Rand32();
    DISK_GEOMETRY_EX diskGeometry = { 0 };
    // "track0" is the first 64+ sectors reserved for MBR, partition table, etc.
    // NOTE: using CHS-style 63 sectors causes an issue with Windows 10 format utility
    // (BIOS reports a maximum of 63 sectors/track but this isn't the true value)
    DWORD firstTrackSectors = 64;
    DWORD firstTrackOffset;
    
    // Set disk attributes.
    diskAttrs.Version = sizeof(diskAttrs);
    diskAttrs.AttributesMask = DISK_ATTRIBUTE_OFFLINE | DISK_ATTRIBUTE_READ_ONLY;
    diskAttrs.Attributes = 0; // clear
    diskAttrs.Persist = TRUE;

    if (!DeviceIoControl(device, IOCTL_DISK_SET_DISK_ATTRIBUTES, &diskAttrs, sizeof(diskAttrs), NULL, 0, &requiredSize, NULL))
    {
        win_perror("IOCTL_DISK_SET_DISK_ATTRIBUTES");
        return FALSE;
    }

    // Tell the system that the disk was changed.
    if (!DeviceIoControl(device, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &requiredSize, NULL))
    {
        win_perror("IOCTL_DISK_UPDATE_PROPERTIES");
        return FALSE;
    }

    // Initialize the disk.
    createDisk.PartitionStyle = PARTITION_STYLE_MBR;
    // We assign a random MBR signature because they need to be unique for a given Windows installation.
    // Although private disk's signature collision is not a problem on a single system,
    // it prevents offline mounting of a HVM private image in another HVM with Windows Tools.
    // More info: http://blogs.technet.com/b/markrussinovich/archive/2011/11/08/3463572.aspx
    createDisk.Mbr.Signature = signature;

    if (!DeviceIoControl(device, IOCTL_DISK_CREATE_DISK, &createDisk, sizeof(createDisk), NULL, 0, &requiredSize, NULL))
    {
        win_perror("IOCTL_DISK_CREATE_DISK");
        return FALSE;
    }

    LogInfo("Disk initialized OK, signature: 0x%08x", signature);

    if (!DeviceIoControl(device, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &requiredSize, NULL))
    {
        win_perror("IOCTL_DISK_UPDATE_PROPERTIES");
        return FALSE;
    }

    if (!DeviceIoControl(device, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, &diskGeometry, sizeof(diskGeometry), &returnedSize, NULL))
    {
        perror("IOCTL_DISK_GET_DRIVE_GEOMETRY_EX");
        return FALSE;
    }

    LogInfo("Disk geometry: cylinders=%d tracksPerCylinder=%d sectorsPerTrack=%d bytesPerSector=%d", diskGeometry.Geometry.Cylinders, diskGeometry.Geometry.TracksPerCylinder, diskGeometry.Geometry.SectorsPerTrack, diskGeometry.Geometry.BytesPerSector);

    firstTrackOffset = firstTrackSectors * diskGeometry.Geometry.BytesPerSector;

    // Create partition table.
    driveLayout->PartitionStyle = PARTITION_STYLE_MBR;
    driveLayout->PartitionCount = 4;
    driveLayout->Mbr.Signature = signature;

    driveLayout->PartitionEntry[0].PartitionStyle = PARTITION_STYLE_MBR;
    driveLayout->PartitionEntry[0].StartingOffset.QuadPart = firstTrackOffset;
    driveLayout->PartitionEntry[0].PartitionLength.QuadPart = diskGeometry.DiskSize.QuadPart - firstTrackOffset;
    driveLayout->PartitionEntry[0].PartitionNumber = 1; // 1-based
    driveLayout->PartitionEntry[0].RewritePartition = TRUE;

    driveLayout->PartitionEntry[0].Mbr.PartitionType = PARTITION_IFS;
    driveLayout->PartitionEntry[0].Mbr.RecognizedPartition = TRUE;
    driveLayout->PartitionEntry[0].Mbr.BootIndicator = FALSE;
    driveLayout->PartitionEntry[0].Mbr.HiddenSectors = firstTrackSectors;

    driveLayout->PartitionEntry[1].RewritePartition = TRUE;
    driveLayout->PartitionEntry[2].RewritePartition = TRUE;
    driveLayout->PartitionEntry[3].RewritePartition = TRUE;

    if (!DeviceIoControl(device, IOCTL_DISK_SET_DRIVE_LAYOUT_EX, partitionBuffer, sizeof(partitionBuffer), NULL, 0, &requiredSize, NULL))
    {
        win_perror("IOCTL_DISK_SET_DRIVE_LAYOUT_EX");
        return FALSE;
    }

    if (!DeviceIoControl(device, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &requiredSize, NULL))
    {
        win_perror("IOCTL_DISK_UPDATE_PROPERTIES");
        return FALSE;
    }

    // Wait for the new volume to be recognized by the system.
    *diskLetter = WaitForVolumeArrival();
    if (*diskLetter == L'\0')
    {
        LogError("WaitForVolumeArrival failed");
        return FALSE;
    }

    LogInfo("New volume letter: %c", *diskLetter);

    return TRUE;
}

// Returns disk letter for private.img, initializes it if needed.
BOOL PreparePrivateVolume(IN ULONG driveNumber, OUT WCHAR *diskLetter)
{
    HANDLE device;
    DWORD returnedSize;
    BYTE buf[1024] = { 0 };
    DRIVE_LAYOUT_INFORMATION_EX *layout = (DRIVE_LAYOUT_INFORMATION_EX *) buf;
    GET_LENGTH_INFORMATION lengthInfo;
    WCHAR driveName[32] = { 0 };
    WCHAR volumeName[MAX_PATH] = { 0 };
    WCHAR filesystemName[MAX_PATH + 1];
    BOOL reinitialize = FALSE;
    BOOL retval = FALSE;
    WCHAR newDiskLetter;

    // Open the device.
    StringCchPrintf(driveName, RTL_NUMBER_OF(driveName), L"\\\\.\\PhysicalDrive%d", driveNumber);
    LogDebug("Opening %s", driveName);

    device = CreateFile(driveName, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (device == INVALID_HANDLE_VALUE)
    {
        win_perror("CreateFile");
        return FALSE;
    }

    // Get partition information.
    if (!DeviceIoControl(device, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0, buf, sizeof(buf), &returnedSize, NULL))
    {
        win_perror("IOCTL_DISK_GET_DRIVE_LAYOUT_EX");
        goto cleanup;
    }

    LogDebug("Partition type: %d, count: %d", layout->PartitionStyle, layout->PartitionCount);
    if (layout->PartitionStyle != PARTITION_STYLE_MBR)
    {
        LogError("Not a MBR disk");
        goto cleanup;
    }

    LogDebug("MBR signature: 0x%08x", layout->Mbr.Signature);

    if (layout->Mbr.Signature != 0)
    {
        // Drive is already initialized. Check if everything is OK.
        LogInfo("Drive is already initialized with signature 0x%08x", layout->Mbr.Signature);

        // Don't change the signature if MBR is already there.
        // This can cause Windows to change drive number.

        // Check if the disk contains formatted volume.
        if (DriveNumberToVolumeName(driveNumber, volumeName, RTL_NUMBER_OF(volumeName)))
        {
            LogInfo("Drive contains volume: %s", volumeName);
            // Check if the volume is mounted.
            if (VolumeNameToDiskLetter(volumeName, diskLetter))
            {
                LogInfo("Volume is mounted as disk '%c:'", *diskLetter);

                // Check the filesystem.
                if (!GetVolumeInformation(volumeName, NULL, 0, NULL, NULL, NULL, filesystemName, RTL_NUMBER_OF(filesystemName)))
                {
                    win_perror("GetVolumeInformation");
                    reinitialize = TRUE;
                }
                else
                {
                    LogInfo("Volume '%s' filesystem: %s", volumeName, filesystemName);

                    if (0 != wcsncmp(L"NTFS", filesystemName, 4))
                    {
                        LogWarning("Volume is formatted with a wrong filesystem, reformatting");
                        reinitialize = TRUE;
                    }
                    else
                    {
                        // Volume is ready.
                        CloseHandle(device);
                        LogInfo("Volume is ready");
                        return TRUE;
                    }
                }
            }
            else
            {
                LogInfo("Volume doesn't seem to be mounted, reinitializing");
                reinitialize = TRUE;
            }
        }
        else
        {
            LogInfo("Drive doesn't contain a proper volume, proceeding with initialization");
            reinitialize = TRUE;
        }
    }

    if (layout->Mbr.Signature == 0 || layout->PartitionCount == 0 || reinitialize)
    {
        // Get disk size.
        if (!DeviceIoControl(device, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &lengthInfo, sizeof(lengthInfo), &returnedSize, NULL))
        {
            win_perror("IOCTL_DISK_GET_LENGTH_INFO");
            goto cleanup;
        }

        LogInfo("Disk size: %I64d", lengthInfo.Length.QuadPart);

        LogDebug("Initializing disk %d", driveNumber);
        if (!InitializeDisk(device, lengthInfo.Length, &newDiskLetter))
        {
            LogError("Failed to initialize disk");
            goto cleanup;
        }
        else
        {
            LogDebug("New volume letter reported by device notification: %c", newDiskLetter);
            // Format disk.
            CloseHandle(device);
            if (!FormatVolume(driveNumber))
            {
                LogError("Failed to format volume");
                return FALSE;
            }

            // Get the new volume name.
            if (DriveNumberToVolumeName(driveNumber, volumeName, RTL_NUMBER_OF(volumeName)))
            {
                LogInfo("New volume: %s", volumeName);
                // Get the disk letter to double-check.
                if (VolumeNameToDiskLetter(volumeName, diskLetter))
                {
                    if (*diskLetter != newDiskLetter)
                    {
                        LogError("Volume disk letter mismatch: device notification returned '%c' but actual letter is '%c'", newDiskLetter, diskLetter);
                        return FALSE;
                    }

                    LogInfo("New volume is mounted as disk '%c:'", *diskLetter);
                    return TRUE;
                }
                else
                {
                    LogError("Volume doesn't seem to be mounted");
                    return FALSE;
                }
            }
            else
            {
                LogError("Failed to format volume");
                return FALSE;
            }
        }
    }

cleanup:
    CloseHandle(device);

    return FALSE;
}
