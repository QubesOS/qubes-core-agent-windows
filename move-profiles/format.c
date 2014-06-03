#include "format.h"
#include "disk.h"

static DWORD g_FormatStatus = 0;
static FormatEx_t g_FormatEx = NULL;
static HMODULE g_fmifsDll = NULL;

static BOOLEAN WINAPI FormatExCallback(FILE_SYSTEM_CALLBACK_COMMAND Command, DWORD Action, PVOID pData)
{
	switch (Command)
	{
	case FCC_DONE:
		if (*(BOOLEAN*)pData == FALSE)
		{
			errorf("Error while formatting");
			g_FormatStatus = 1;
		}
		else
		{
			logf("format done");
			g_FormatStatus = 0;
		}
		break;

	case FCC_STRUCTURE_PROGRESS:
		logf("FCC_STRUCTURE_PROGRESS");
		break;

	case FCC_DONE_WITH_STRUCTURE:
		logf("FCC_DONE_WITH_STRUCTURE");
		break;

	case FCC_INCOMPATIBLE_FILE_SYSTEM:
		errorf("Incompatible File System");
		g_FormatStatus = 1;
		break;
	case FCC_ACCESS_DENIED:
		errorf("Access denied");
		g_FormatStatus = ERROR_ACCESS_DENIED;
		break;
	case FCC_MEDIA_WRITE_PROTECTED:
		errorf("Media is write protected");
		g_FormatStatus = ERROR_WRITE_PROTECT;
		break;
	case FCC_VOLUME_IN_USE:
		errorf("Volume is in use");
		g_FormatStatus = ERROR_DEVICE_IN_USE;
		break;
	case FCC_DEVICE_NOT_READY:
		errorf("The device is not ready");
		g_FormatStatus = ERROR_NOT_READY;
		break;
	case FCC_CANT_QUICK_FORMAT:
		errorf("Cannot quick format this volume");
		g_FormatStatus = 1;
		break;
	case FCC_BAD_LABEL:
		errorf("Bad label");
		g_FormatStatus = ERROR_LABEL_TOO_LONG;
		break;
		break;
	case FCC_CLUSTER_SIZE_TOO_BIG:
	case FCC_CLUSTER_SIZE_TOO_SMALL:
		errorf("Unsupported cluster size");
		g_FormatStatus = 1;
		break;
	case FCC_VOLUME_TOO_BIG:
	case FCC_VOLUME_TOO_SMALL:
		errorf("Volume is too %s", (Command == FCC_VOLUME_TOO_BIG) ? "big" : "small");
		g_FormatStatus = 1;
		break;
	case FCC_NO_MEDIA_IN_DRIVE:
		errorf("No media in drive");
		g_FormatStatus = ERROR_NO_MEDIA_IN_DRIVE;
		break;
	default:
		errorf("Received unhandled command 0x02%X", Command);
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
		logf("Formatting volume: %s", volumeName);

		// Proceed with format.

		if (!g_fmifsDll)
		{
			g_fmifsDll = LoadLibrary(L"fmifs");
			if (!g_fmifsDll)
			{
				perror("LoadLibrary(fmifs)");
				return FALSE;
			}
		}

		if (!g_FormatEx)
		{
			g_FormatEx = (FormatEx_t)GetProcAddress(g_fmifsDll, "FormatEx");
			if (!g_FormatEx)
			{
				perror("GetProcAddress(fmifs, FormatEx)");
				return FALSE;
			}
		}
		
		// This call is synchronous.
		g_FormatEx(volumeName, FixedMedia, L"NTFS", L"Qubes Private Image", TRUE, 0, FormatExCallback);
	}

	return g_FormatStatus == 0;
}
