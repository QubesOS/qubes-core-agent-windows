// Moves user profiles directory to private.img

#include "move-profiles.h"
#include "device.h"
#include "disk.h"

#include <shellapi.h>
#include <Shlwapi.h>
#include <ShlObj.h>
#include <Knownfolders.h>

#define LOG_NAME L"move-profiles"
#define REG_CONFIG_KEY L"Software\\Invisible Things Lab\\Qubes Tools"
#define REG_CONFIG_LOG_VALUE L"LogDir"

// TODO: move registry reading to windows-utils
ULONG ReadRegistryConfig(void)
{
	HKEY key = NULL;
	DWORD status = ERROR_SUCCESS;
	DWORD type;
	DWORD size;
	WCHAR logPath[MAX_PATH];

	// Read the log directory.
	SetLastError(status = RegOpenKey(HKEY_LOCAL_MACHINE, REG_CONFIG_KEY, &key));
	if (status != ERROR_SUCCESS)
	{
		// failed, use some safe default
		// todo: use event log
		log_init(L"c:\\", LOG_NAME);
		logf("registry config: '%s'", REG_CONFIG_KEY);
		return perror("RegOpenKey");
	}

	size = sizeof(logPath)-sizeof(TCHAR);
	RtlZeroMemory(logPath, sizeof(logPath));
	SetLastError(status = RegQueryValueEx(key, REG_CONFIG_LOG_VALUE, NULL, &type, (PBYTE)logPath, &size));
	if (status != ERROR_SUCCESS)
	{
		log_init(L"c:\\", LOG_NAME);
		errorf("Failed to read log path from '%s\\%s'", REG_CONFIG_KEY, REG_CONFIG_LOG_VALUE);
		perror("RegQueryValueEx");
		status = ERROR_SUCCESS; // don't fail
		goto cleanup;
	}

	if (type != REG_SZ)
	{
		log_init(L"c:\\", LOG_NAME);
		errorf("Invalid type of config value '%s', 0x%x instead of REG_SZ", REG_CONFIG_LOG_VALUE, type);
		status = ERROR_SUCCESS; // don't fail
		goto cleanup;
	}

	log_init(logPath, LOG_NAME);

cleanup:
	if (key)
		RegCloseKey(key);

	return status;
}

// Argument: xen/vbd device id that represents private.img
int wmain(int argc, PWCHAR argv[])
{
	ULONG xenVbdId;
	ULONG driveNumber;
	SHFILEOPSTRUCT shfop;
	int ret;
	WCHAR *usersPath;
	WCHAR *fromPath;
	WCHAR toPath[] = L"d:\\Users\0"; // template

	if (ReadRegistryConfig() != ERROR_SUCCESS)
		return -1;

	if (argc < 2)
	{
		errorf("Usage: %s <xen/vbd device ID that represents private.img>", argv[0]);
		return 1;
	}

	xenVbdId = wcstoul(argv[1], NULL, 10);
	if (xenVbdId == 0 || xenVbdId == ULONG_MAX)
	{
		errorf("Invalid xen/vbd device ID: %s", argv[1]);
		return 2;
	}

	logf("xen/vbd device ID: %lu", xenVbdId);

	if (!GetPrivateImgDriveNumber(xenVbdId, &driveNumber))
	{
		errorf("Failed to get drive number for private.img");
		return 3;
	}

	if (!PreparePrivateVolume(driveNumber, toPath))
	{
		errorf("Failed to initialize private.img");
		return 4;
	}

	// We should have a properly formatted volume by now.
	
	// Check if USERS directory is already a junction point to the private volume.
	if (S_OK != SHGetKnownFolderPath(&FOLDERID_UserProfiles, 0, NULL, &usersPath))
	{
		perror("SHGetKnownFolderPath(FOLDERID_UserProfiles)");
		return 7;
	}

	if (GetFileAttributes(usersPath) & FILE_ATTRIBUTE_REPARSE_POINT)
	{
		logf("Users directory (%s) is already a reparse point, exiting", usersPath);
		// TODO: make sure it points to private.img?
		return 0;
	}

	// Paths for SHFileOperation need to be double null-terminated.
	fromPath = malloc((wcslen(usersPath) + 2)*sizeof(WCHAR)); // 1 character more
	ZeroMemory(fromPath, (wcslen(usersPath) + 2)*sizeof(WCHAR));
	StringCchCopy(fromPath, wcslen(usersPath) + 2, usersPath);

	CoTaskMemFree(usersPath);

	// Copy users directory to the private volume.
	logf("Copying '%s' to '%s'", fromPath, toPath);
	ZeroMemory(&shfop, sizeof(shfop));
	shfop.wFunc = FO_COPY;
	shfop.pFrom = fromPath;
	shfop.pTo = toPath;
	shfop.fFlags = FOF_NO_UI;
	ret = SHFileOperation(&shfop);

	if (ret != 0)
	{
		errorf("Copy failed: %d", ret);
		return 8;
	}

	logf("Copy OK, deleting old Users directory");
	ZeroMemory(&shfop, sizeof(shfop));
	shfop.wFunc = FO_DELETE;
	shfop.pFrom = fromPath;
	shfop.fFlags = FOF_NO_UI;
	ret = SHFileOperation(&shfop);

	if (ret != 0 || PathFileExists(fromPath))
	{
		errorf("Delete failed: %d", ret);
		return 9;
	}

	logf("Delete OK, creating junction point");
	if (!CreateSymbolicLink(fromPath, toPath, SYMBOLIC_LINK_FLAG_DIRECTORY))
	{
		return perror("CreateSymbolicLink");
	}

	logf("Junction created, '%s'->'%s'", fromPath, toPath);

	return ERROR_SUCCESS;
}
