// Moves user profiles directory to private.img

#include "move-profiles.h"
#include <shellapi.h>
#include <Shlwapi.h>
#include <ShlObj.h>
#include <Knownfolders.h>
#include <aclapi.h>
#include <winioctl.h>

#include "device.h"
#include "disk.h"

#include "log.h"


#define LOG_NAME L"move-profiles"

// from ntifs.h
typedef struct _REPARSE_DATA_BUFFER
{
    ULONG  ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;

    union
    {
        struct
        {
            USHORT SubstituteNameOffset;
            USHORT SubstituteNameLength;
            USHORT PrintNameOffset;
            USHORT PrintNameLength;
            ULONG  Flags;
            WCHAR  PathBuffer[1];
        }
        SymbolicLinkReparseBuffer;

        struct
        {
            USHORT SubstituteNameOffset;
            USHORT SubstituteNameLength;
            USHORT PrintNameOffset;
            USHORT PrintNameLength;
            WCHAR  PathBuffer[1];
        }
        MountPointReparseBuffer;

        struct
        {
            UCHAR DataBuffer[1];
        }
        GenericReparseBuffer;
    };
} REPARSE_DATA_BUFFER, *PREPARSE_DATA_BUFFER;

#define MAX_PATH_LONG 32768
#define LONG_PATH_PREFIX L"\\\\?\\"
#define LONG_PATH_PREFIX_LENGTH 4

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
        errorf("The token does not have the specified privilege");
        return ERROR_PRIVILEGE_NOT_HELD;
    }

    logf("Privilege %s enabled", privilegeName);

    return ERROR_SUCCESS;
}

// sourcePath must be an existing and empty directory.
DWORD SetReparsePoint(const PWCHAR sourcePath, const PWCHAR targetPath)
{
    BYTE buffer[MAX_PATH_LONG]; // MSDN doesn't specify maximum structure's length, but it should be close to MAX_PATH_LONG
    PREPARSE_DATA_BUFFER rdb = (PREPARSE_DATA_BUFFER)buffer;
    DWORD size, targetSize;
    WCHAR dest[MAX_PATH_LONG];
    DWORD status = ERROR_SUCCESS;
    HANDLE file = CreateFile(sourcePath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);

    logf("Creating reparse point: '%s' -> '%s'", sourcePath, targetPath);

    if (file == INVALID_HANDLE_VALUE)
    {
        status = perror("CreateFile");
        goto cleanup;
    }

    targetSize = wcslen(targetPath) * sizeof(WCHAR);

    rdb->ReparseTag = IO_REPARSE_TAG_SYMLINK;
    // 12 = SymbolicLinkReparseBuffer fields without PathBuffer
    // sizeof(PathBuffer) = 2*targetSize + sizeof(L"\\??\\")
    rdb->ReparseDataLength = 12 + targetSize * 2 + 8;
    rdb->SymbolicLinkReparseBuffer.Flags = 0; // absolute link
    StringCchPrintf(rdb->SymbolicLinkReparseBuffer.PathBuffer, sizeof(buffer) - sizeof(REPARSE_DATA_BUFFER),
        L"%s\\??\\%s", targetPath, targetPath);
    rdb->SymbolicLinkReparseBuffer.PrintNameOffset = 0;
    rdb->SymbolicLinkReparseBuffer.PrintNameLength = targetSize;
    rdb->SymbolicLinkReparseBuffer.SubstituteNameOffset = rdb->SymbolicLinkReparseBuffer.PrintNameLength;
    rdb->SymbolicLinkReparseBuffer.SubstituteNameLength = targetSize + 8;

    debugf("PrintName: %.*s", rdb->SymbolicLinkReparseBuffer.PrintNameLength / 2, rdb->SymbolicLinkReparseBuffer.PathBuffer + rdb->SymbolicLinkReparseBuffer.PrintNameOffset / 2);
    debugf("SubstituteName: %.*s", rdb->SymbolicLinkReparseBuffer.SubstituteNameLength / 2, rdb->SymbolicLinkReparseBuffer.PathBuffer + rdb->SymbolicLinkReparseBuffer.SubstituteNameOffset / 2);

    // 8 = fields before the union
    if (!DeviceIoControl(file, FSCTL_SET_REPARSE_POINT, buffer, rdb->ReparseDataLength + 8, NULL, 0, &size, NULL))
    {
        status = perror("FSCTL_SET_REPARSE_POINT");
        goto cleanup;
    }

cleanup:
    CloseHandle(file);
    return status;
}

DWORD CopyReparsePoint(const PWCHAR sourcePath, const PWCHAR targetPath, BOOL isDirectory)
{
    BYTE buffer[MAX_PATH_LONG]; // MSDN doesn't specify maximum structure's length, but it should be close to MAX_PATH_LONG
    PREPARSE_DATA_BUFFER rdb = (PREPARSE_DATA_BUFFER)buffer;
    DWORD size;
    WCHAR dest[MAX_PATH_LONG];
    DWORD status = ERROR_SUCCESS;
    HANDLE file = CreateFile(sourcePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);

    if (file == INVALID_HANDLE_VALUE)
    {
        status = perror("CreateFile");
        goto cleanup;
    }

    if (!DeviceIoControl(file, FSCTL_GET_REPARSE_POINT, NULL, 0, buffer, sizeof(buffer), &size, NULL))
    {
        status = perror("FSCTL_GET_REPARSE_POINT");
        goto cleanup;
    }

    if (rdb->ReparseTag == IO_REPARSE_TAG_SYMLINK)
    {
        memcpy(dest, rdb->SymbolicLinkReparseBuffer.PathBuffer +
            rdb->SymbolicLinkReparseBuffer.PrintNameOffset / sizeof(WCHAR),
            rdb->SymbolicLinkReparseBuffer.PrintNameLength);
        dest[rdb->SymbolicLinkReparseBuffer.PrintNameLength / sizeof(WCHAR)] = 0;
        debugf("Symlink: '%s' -> '%s'", sourcePath, dest);

        if (!CreateSymbolicLink(targetPath, dest, isDirectory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0))
        {
            status = perror("CreateSymbolicLink");
            goto cleanup;
        }
    }
    else if (rdb->ReparseTag == IO_REPARSE_TAG_MOUNT_POINT)
    {
        memcpy(dest, rdb->MountPointReparseBuffer.PathBuffer +
            rdb->MountPointReparseBuffer.PrintNameOffset / sizeof(WCHAR),
            rdb->MountPointReparseBuffer.PrintNameLength);
        dest[rdb->MountPointReparseBuffer.PrintNameLength / sizeof(WCHAR)] = 0;
        debugf("Mount point: '%s' -> '%s'", sourcePath, dest);

        if (!CreateSymbolicLink(targetPath, dest, isDirectory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0))
        {
            status = perror("CreateSymbolicLink");
            goto cleanup;
        }
    }
    else
        logf("Unknown reparse tag 0x%x in '%s'", rdb->ReparseTag, sourcePath);

cleanup:
    CloseHandle(file);
    return status;
}

DWORD CopyDirectory(const PWCHAR sourcePath, const PWCHAR targetPath, BOOL continueOnError)
{
    PWCHAR sourceMask = (PWCHAR)LocalAlloc(0, MAX_PATH_LONG);
    PWCHAR current = (PWCHAR)LocalAlloc(0, MAX_PATH_LONG);
    PWCHAR target = (PWCHAR)LocalAlloc(0, MAX_PATH_LONG);
    WIN32_FIND_DATA fd;
    HANDLE findObject;
    BOOL loop;
    DWORD status = ERROR_SUCCESS;
    PSECURITY_DESCRIPTOR sd = NULL;
    PSID owner, group;
    PACL dacl, sacl;

    debugf("'%s' -> '%s'", sourcePath, targetPath);

    current[0] = 0; // for error reporting at the end

    // Create target directory if not present.
    if (INVALID_FILE_ATTRIBUTES == GetFileAttributes(targetPath))
    {
        if (!CreateDirectory(targetPath, NULL))
        {
            status = perror("CreateDirectory");
            goto cleanup;
        }

        // Copy source's security descriptor.
        SetLastError(status = GetNamedSecurityInfo(sourcePath, SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | SACL_SECURITY_INFORMATION | OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION,
            &owner, &group, &dacl, &sacl, &sd));
        if (ERROR_SUCCESS != status)
        {
            perror("GetNamedSecurityInfo");
            goto cleanup;
        }

        SetLastError(status = SetNamedSecurityInfo(targetPath, SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | SACL_SECURITY_INFORMATION | OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION,
            owner, group, dacl, sacl));
        if (ERROR_SUCCESS != status)
        {
            perror("SetNamedSecurityInfo");
            goto cleanup;
        }

        LocalFree(sd);
        sd = NULL;
    }

    StringCchPrintf(sourceMask, MAX_PATH_LONG, L"%s\\*", sourcePath);

    // Enumerate directory contents.
    findObject = FindFirstFile(sourceMask, &fd);
    loop = findObject != INVALID_HANDLE_VALUE;
    while (loop)
    {
        if (0 == wcscmp(L".", fd.cFileName) || 0 == wcscmp(L"..", fd.cFileName))
            goto skip;

        StringCchPrintf(current, MAX_PATH_LONG, L"%s\\%s", sourcePath, fd.cFileName);
        StringCchPrintf(target, MAX_PATH_LONG, L"%s\\%s", targetPath, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            debugf("DIR: %s", current);
            // Recursively copy unless it's a reparse point.
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            {
                status = CopyReparsePoint(current, target, TRUE);
                if (ERROR_SUCCESS != status && !continueOnError)
                    goto cleanup;
            }
            else
            {
                status = CopyDirectory(current, target, continueOnError);
                if (ERROR_SUCCESS != status && !continueOnError)
                    goto cleanup;
            }
        }
        else
        {
            debugf("FILE: %s", current);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            {
                status = CopyReparsePoint(current, target, FALSE);
                if (ERROR_SUCCESS != status && !continueOnError)
                    goto cleanup;
            }
            else
            {
                if (!CopyFile(current, target, FALSE) && !continueOnError)
                {
                    status = perror("CopyFile");
                    goto cleanup;
                }
            }
        }

    skip:
        if (!FindNextFile(findObject, &fd) && GetLastError() == ERROR_NO_MORE_FILES)
            loop = FALSE;
    }

    FindClose(findObject);

cleanup:
    if (ERROR_SUCCESS != status)
    {
        errorf("Failed to copy '%s' to '%s'", sourcePath, targetPath);
        if (*current)
            errorf("Current file: '%s', target: '%s'", current, target);
    }
    if (sd)
        LocalFree(sd);
    LocalFree(sourceMask);
    LocalFree(current);
    LocalFree(target);
    return status;
}

DWORD DeleteDirectory(const PWCHAR sourcePath)
{
    PWCHAR sourceMask = (PWCHAR)LocalAlloc(0, MAX_PATH_LONG);
    PWCHAR current = (PWCHAR)LocalAlloc(0, MAX_PATH_LONG);
    WIN32_FIND_DATA fd;
    HANDLE findObject;
    BOOL loop;
    DWORD status = ERROR_SUCCESS, i;

    debugf("+'%s'", sourcePath);

    current[0] = 0; // for error reporting at the end

    StringCchPrintf(sourceMask, MAX_PATH_LONG, L"%s\\*", sourcePath);

    // Enumerate directory contents.
    findObject = FindFirstFile(sourceMask, &fd);
    loop = findObject != INVALID_HANDLE_VALUE;
    while (loop)
    {
        if (0 == wcscmp(L".", fd.cFileName) || 0 == wcscmp(L"..", fd.cFileName))
            goto skip;

        StringCchPrintf(current, MAX_PATH_LONG, L"%s\\%s", sourcePath, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
        {
            if (!SetFileAttributes(current, fd.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY))
            {
                status = perror("SetFileAttributes");
                goto cleanup;
            }
        }

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            debugf("DIR: %s", current);
            // Recursively delete unless it's a reparse point.
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            {
                if (!RemoveDirectory(current)) // this deletes the junction without affecting the target
                {
                    status = perror("RemoveDirectory");
                    goto cleanup;
                }
            }
            else
            {
                status = DeleteDirectory(current);
                if (ERROR_SUCCESS != status)
                    goto cleanup;
            }
        }
        else
        {
            debugf("FILE: %s", current);
            // Reparse points don't matter for file deletion, it only deletes the source.
            if (!DeleteFile(current))
            {
                status = perror("DeleteFile");
                goto cleanup;
            }
        }

    skip:
        if (!FindNextFile(findObject, &fd) && GetLastError() == ERROR_NO_MORE_FILES)
            loop = FALSE;
    }

    FindClose(findObject);

    debugf("-'%s'", sourcePath);
    if (!RemoveDirectory(sourcePath))
        status = perror("RemoveDirectory");

cleanup:
    if (ERROR_SUCCESS != status)
    {
        errorf("Failed to delete '%s'", sourcePath);
        if (*current)
            errorf("Current file: '%s'", current);
    }
    LocalFree(sourceMask);
    LocalFree(current);
    return status;
}

// Argument: xen/vbd device id that represents private.img
int wmain(int argc, PWCHAR argv[])
{
    ULONG xenVbdId;
    ULONG driveNumber;
    SHFILEOPSTRUCT shfop;
    DWORD status;
    WCHAR *usersPath;
    WCHAR fromPath[MAX_PATH_LONG];
    WCHAR toPath[] = L"\\\\?\\d:\\Users"; // template
    HANDLE token;

    log_init_default(LOG_NAME);

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

    // Enable privileges needed for bypassing file security & for ACL manipulation.
    OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &token);
    EnablePrivilege(token, SE_SECURITY_NAME);
    EnablePrivilege(token, SE_RESTORE_NAME);
    EnablePrivilege(token, SE_TAKE_OWNERSHIP_NAME);
    CloseHandle(token);

    if (!GetPrivateImgDriveNumber(xenVbdId, &driveNumber))
    {
        errorf("Failed to get drive number for private.img");
        return 3;
    }

    // This will replace drive letter in toPath.
    if (!PreparePrivateVolume(driveNumber, &toPath[LONG_PATH_PREFIX_LENGTH]))
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

    // Add long path prefix to profiles directory if not present.
    // This allows us to process paths longer than MAX_PATH up to NTFS' limit (32k).
    if (0 != wcsncmp(LONG_PATH_PREFIX, usersPath, LONG_PATH_PREFIX_LENGTH))
        StringCchPrintf(fromPath, MAX_PATH_LONG, LONG_PATH_PREFIX L"%s", usersPath);
    else
        StringCchCopy(fromPath, MAX_PATH_LONG, usersPath);

    CoTaskMemFree(usersPath);

    // Copy users directory to the private volume.
    logf("Copying '%s' to '%s'", fromPath, toPath);
    status = CopyDirectory(fromPath, toPath, FALSE);

    if (ERROR_SUCCESS != status)
    {
        errorf("Copy failed: %d", status);
        return 8;
    }

    logf("Copy OK, deleting old Users directory");

    status = DeleteDirectory(fromPath);

    if (status != ERROR_SUCCESS || PathFileExists(fromPath))
    {
        errorf("Delete failed: %d", status);

        // This can happen for some reason even if deletion of everything inside c:\users succeeds.
        // RemoveDirectory fails with ACCESS_DENIED, (NTSTATUS) 0xc0000121 - An attempt has been made to remove a file or directory that cannot be deleted.
        // The dir has no special ACLs or anything... I've tried to debug the reason but ultimately stopped
        // since we can set a reparse point on an empty directory anyway.

        // If old Users dir is empty, create a reparse point to the copied directory.
        // TODO: check if old Users is empty.
        // Reparse paths can't contain long path prefix.
        if (ERROR_SUCCESS != SetReparsePoint(fromPath + LONG_PATH_PREFIX_LENGTH, toPath + LONG_PATH_PREFIX_LENGTH))
        {
            // Everything failed, try to copy old Users back.
            CopyDirectory(toPath, fromPath, TRUE);
            return 9;
        }

        // We should have a working junction here.
        logf("Junction created, '%s'->'%s'", fromPath, toPath);
        return ERROR_SUCCESS;
    }

    logf("Delete OK, creating junction point");
    // This call requires that fromPath directory doesn't exist.
    if (!CreateSymbolicLink(fromPath, toPath, SYMBOLIC_LINK_FLAG_DIRECTORY))
    {
        return perror("CreateSymbolicLink");
    }

    logf("Junction created, '%s'->'%s'", fromPath, toPath);

    return ERROR_SUCCESS;
}
