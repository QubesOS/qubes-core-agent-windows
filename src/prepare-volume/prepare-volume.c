// Initializes/formats private.img and queues moving user profiles there on the next boot.

#include "prepare-volume.h"
#include <shellapi.h>
#include <Shlwapi.h>
#include <ShlObj.h>
#include <Knownfolders.h>
#include <aclapi.h>
#include <stdlib.h>

#include "device.h"
#include "disk.h"

#include <log.h>
#include <config.h>
#include <qubes-string.h>

#define MAX_PATH_LONG 32768

DWORD EnablePrivilege(HANDLE token, const WCHAR *privilegeName)
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
        (PTOKEN_PRIVILEGES) NULL,
        (PDWORD) NULL))
        return perror("AdjustTokenPrivileges");

    if (GetLastError() == ERROR_NOT_ALL_ASSIGNED)
    {
        LogError("The token does not have the specified privilege '%s'", privilegeName);
        return ERROR_PRIVILEGE_NOT_HELD;
    }

    LogDebug("Privilege %s enabled", privilegeName);

    return ERROR_SUCCESS;
}

// Argument: backend device id that represents private.img
int wmain(int argc, WCHAR *argv[])
{
    ULONG backendId;
    ULONG driveNumber;
    DWORD status;
    WCHAR *usersPath;
    WCHAR targetUsersPath[] = L"d:\\Users"; // template
    WCHAR targetLogPath[] = L"d:\\QubesLogs";
    HANDLE token;
    HKEY key;
    DWORD valueType;
    DWORD size;
    WCHAR *valueData;
    WCHAR *command;
    WCHAR *logPath, *logShortPath;
    WCHAR msg[1024];

    if (argc < 2)
    {
        LogError("Usage: %s <backend device ID that represents private.img>", argv[0]);
        return 1;
    }

    backendId = wcstoul(argv[1], NULL, 10);
    if (backendId == 0 || backendId == ULONG_MAX)
    {
        LogError("Invalid backend device ID: %s", argv[1]);
        return 1;
    }

    LogInfo("backend device ID: %lu", backendId);

    // Enable privileges needed for bypassing file security & for ACL manipulation.
    OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &token);
    EnablePrivilege(token, SE_SECURITY_NAME);
    EnablePrivilege(token, SE_RESTORE_NAME);
    EnablePrivilege(token, SE_TAKE_OWNERSHIP_NAME);
    CloseHandle(token);

    // MBR signatures assigned are random to prevent collisions
    srand(GetTickCount());

    if (!GetPrivateImgDriveNumber(backendId, &driveNumber))
    {
        LogError("Failed to get drive number for private.img");
        return 1;
    }

    // This will replace drive letter in targetUsersPath.
    if (!PreparePrivateVolume(driveNumber, targetUsersPath))
    {
        LogError("Failed to initialize private.img");
        return 1;
    }

    // We should have a properly formatted volume by now.

    // Check if profiles directory is already a junction point to the private volume.
    if (S_OK != SHGetKnownFolderPath(&FOLDERID_UserProfiles, 0, NULL, &usersPath))
    {
        perror("SHGetKnownFolderPath(FOLDERID_UserProfiles)");
        return 1;
    }

    if (GetFileAttributes(usersPath) & FILE_ATTRIBUTE_REPARSE_POINT)
    {
        LogInfo("Users directory (%s) is already a reparse point, exiting", usersPath);
        // TODO: make sure it points to private.img?
        return 0;
    }

    size = MAX_PATH_LONG*sizeof(WCHAR);
    valueData = malloc(size);
    command = malloc(size);
    logPath = malloc(size);
    targetLogPath[0] = targetUsersPath[0]; // copy the private.img drive letter

    // Read log directory from the registry. We'll relocate it to the private disk.
    status = CfgReadString(NULL, LOG_CONFIG_PATH_VALUE, logPath, MAX_PATH_LONG, NULL);
    if (ERROR_SUCCESS != status)
    {
        perror2(status, "Reading log path from registry");
        return 1;
    }

    LogDebug("Log directory: '%s'", logPath);

    // Register the native relocate-dir executable as BootExecute in the registry.
    // It's a multi-string value (normal strings terminated with an empty string).
    status = RegOpenKeyEx(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager", 0, KEY_READ | KEY_WRITE, &key);
    if (ERROR_SUCCESS != status)
    {
        perror2(status, "Opening Session Manager registry key");
        return 1;
    }

    // Get the current value (usually filesystem autocheck).
    status = RegQueryValueEx(key, L"BootExecute", NULL, &valueType, (PBYTE)valueData, &size);
    if (ERROR_SUCCESS != status)
    {
        perror2(status, "Reading BootExecute entry");
        return 1;
    }

    // Format the move profiles command.
    if (FAILED(StringCchPrintfW(command, MAX_PATH_LONG, L"relocate-dir %s %s", usersPath, targetUsersPath)))
    {
        LogError("Formatting move profiles command failed");
        return 1;
    }
    LogDebug("profiles command: '%s'", command);

    CoTaskMemFree(usersPath);

    // Append the command to the BootExecute value.
    if (!MultiWStrAdd(valueData, MAX_PATH_LONG*sizeof(WCHAR), command))
    {
        LogError("Buffer too small for BootExecute entry");
        return 1;
    }

    // relocate-dir can't handle arguments with spaces
    size = GetShortPathName(logPath, NULL, 0);
    logShortPath = malloc(size * sizeof(WCHAR));
    GetShortPathName(logPath, logShortPath, size);
    LogDebug("log short path: '%s'", logShortPath);

    // Format the move logs command.
    if (FAILED(StringCchPrintfW(command, MAX_PATH_LONG, L"relocate-dir %s %s", logShortPath, targetLogPath)))
    {
        LogError("Formatting move logs command failed");
        return 1;
    }
    LogDebug("log command: '%s'", command);

    // Append the command to the BootExecute value.
    if (!MultiWStrAdd(valueData, MAX_PATH_LONG*sizeof(WCHAR), command))
    {
        LogError("Buffer too small for BootExecute entry");
        return 1;
    }

    status = RegSetValueEx(key, L"BootExecute", 0, REG_MULTI_SZ, (PBYTE)valueData, MultiWStrSize(valueData, NULL));
    if (ERROR_SUCCESS != status)
    {
        perror2(status, "RegSetValueEx");
        return 1;
    }

    RegCloseKey(key);

    StringCchPrintf(msg, RTL_NUMBER_OF(msg),
        L"Qubes private disk image initialized as disk %c:.\r\n"
        L"User profiles directory will be moved there during the next system boot.",
        targetUsersPath[0]);
    MessageBox(0, msg, L"Qubes Tools for Windows", MB_OK | MB_ICONINFORMATION);

    return 0;
}
