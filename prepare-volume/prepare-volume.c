// Prepares private.img and registers move-profiles as a BootExceute executable.

#include "prepare-volume.h"
#include <shellapi.h>
#include <Shlwapi.h>
#include <ShlObj.h>
#include <Knownfolders.h>
#include <aclapi.h>

#include "device.h"
#include "disk.h"

#include "log.h"

#define MAX_PATH_LONG 32768

DWORD EnablePrivilege(HANDLE token, const PWCHAR privilegeName)
{
    TOKEN_PRIVILEGES tp;
    LUID luid;

    if (!LookupPrivilegeValue(NULL, privilegeName, &luid))
        return perror("LookupPrivilegeValue");

    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (!AdjustTokenPrivileges(
        token,
        FALSE,
        &tp,
        sizeof(TOKEN_PRIVILEGES),
        (PTOKEN_PRIVILEGES)NULL,
        (PDWORD)NULL))
        return perror("AdjustTokenPrivileges");

    if (GetLastError() == ERROR_NOT_ALL_ASSIGNED)
    {
        LogError("The token does not have the specified privilege '%s'", privilegeName);
        return ERROR_PRIVILEGE_NOT_HELD;
    }

    LogDebug("Privilege %s enabled", privilegeName);

    return ERROR_SUCCESS;
}

// Argument: xen/vbd device id that represents private.img
int wmain(int argc, PWCHAR argv[])
{
    ULONG xenVbdId;
    ULONG driveNumber;
    DWORD status;
    WCHAR *usersPath;
    WCHAR toPath[] = L"d:\\Users"; // template
    HANDLE token;
    HKEY key;
    DWORD valueType;
    DWORD size;
    WCHAR valueData[MAX_PATH_LONG];
    WCHAR command[MAX_PATH_LONG];

    if (argc < 2)
    {
        LogError("Usage: %s <xen/vbd device ID that represents private.img>", argv[0]);
        return 1;
    }

    xenVbdId = wcstoul(argv[1], NULL, 10);
    if (xenVbdId == 0 || xenVbdId == ULONG_MAX)
    {
        LogError("Invalid xen/vbd device ID: %s", argv[1]);
        return 2;
    }

    LogInfo("xen/vbd device ID: %lu", xenVbdId);

    // Enable privileges needed for bypassing file security & for ACL manipulation.
    OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &token);
    EnablePrivilege(token, SE_SECURITY_NAME);
    EnablePrivilege(token, SE_RESTORE_NAME);
    EnablePrivilege(token, SE_TAKE_OWNERSHIP_NAME);
    CloseHandle(token);

    if (!GetPrivateImgDriveNumber(xenVbdId, &driveNumber))
    {
        LogError("Failed to get drive number for private.img");
        return 3;
    }

    // This will replace drive letter in toPath.
    if (!PreparePrivateVolume(driveNumber, toPath))
    {
        LogError("Failed to initialize private.img");
        return 4;
    }

    // We should have a properly formatted volume by now.

    // Check if profiles directory is already a junction point to the private volume.
    if (S_OK != SHGetKnownFolderPath(&FOLDERID_UserProfiles, 0, NULL, &usersPath))
    {
        perror("SHGetKnownFolderPath(FOLDERID_UserProfiles)");
        return 5;
    }

    if (GetFileAttributes(usersPath) & FILE_ATTRIBUTE_REPARSE_POINT)
    {
        LogInfo("Users directory (%s) is already a reparse point, exiting", usersPath);
        // TODO: make sure it points to private.img?
        return 0;
    }

    // Register the native move-profiles executable as BootExecute in the registry.
    // It's a multi-string value (null-separated strings terminated with a double null).
    status = RegOpenKeyEx(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager", 0, KEY_READ | KEY_WRITE, &key);
    if (ERROR_SUCCESS != status)
    {
        perror2(status, "RegOpenKeyEx");
        return 6;
    }

    // Get current value (usually filesystem autocheck).
    size = sizeof(valueData);
    status = RegQueryValueEx(key, L"BootExecute", NULL, &valueType, (PBYTE)valueData, &size);
    if (ERROR_SUCCESS != status)
    {
        perror2(status, "RegQueryValueEx");
        return 7;
    }

    // Format the command.
    if (FAILED(StringCchPrintf(command, RTL_NUMBER_OF(command), L"move-profiles %s %s\0", usersPath, toPath)))
    {
        LogError("StringCchPrintf(command) failed");
        return 8;
    }

    CoTaskMemFree(usersPath);

    // Append the command to the current value.
    if ((wcslen(command) + 1) * sizeof(WCHAR) + size > sizeof(valueData))
    {
        LogError("Buffer too small for BootExecute entry");
        return 9;
    }

    memcpy(valueData + size / sizeof(WCHAR) - 1, command, (wcslen(command) + 2) * sizeof(WCHAR));

    status = RegSetValueEx(key, L"BootExecute", 0, REG_MULTI_SZ, (PBYTE)valueData, (wcslen(command) + 1) * sizeof(WCHAR) + size);
    if (ERROR_SUCCESS != status)
    {
        perror2(status, "RegSetValueEx");
        return 10;
    }

    RegCloseKey(key);

    return 0;
}
