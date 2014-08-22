#include "io.h"

NTSTATUS FileOpen(OUT PHANDLE file, const IN PWCHAR fileName, IN BOOLEAN write, IN BOOLEAN overwrite, IN BOOLEAN isReparse)
{
    UNICODE_STRING fileNameU;
    IO_STATUS_BLOCK iosb;
    ULONG createDisposition = 0;
    WCHAR fileNameNt[1024] = L"\\??\\"; // FIXME: long paths
    OBJECT_ATTRIBUTES oa;
    ACCESS_MASK desiredAccess;
    NTSTATUS status;

    wcscat(fileNameNt, fileName);
    RtlInitUnicodeString(&fileNameU, fileNameNt);

    InitializeObjectAttributes(
        &oa,
        &fileNameU,
        OBJ_CASE_INSENSITIVE,
        NULL,
        NULL);

    desiredAccess = SYNCHRONIZE | FILE_READ_ATTRIBUTES | FILE_READ_EA | READ_CONTROL | ACCESS_SYSTEM_SECURITY;

    if (write)
    {
        if (!isReparse)
            desiredAccess |= FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA | WRITE_DAC | WRITE_OWNER;

        if (overwrite)
        {
            createDisposition = FILE_OVERWRITE_IF;
        }
        else
        {
            createDisposition = FILE_OPEN_IF;
        }
    }
    else
    {
        if (!isReparse)
            desiredAccess |= FILE_READ_DATA;
        createDisposition = FILE_OPEN;
    }

    status = NtCreateFile(
        file,
        desiredAccess,
        &oa,
        &iosb,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        0, // no sharing
        createDisposition,
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT | FILE_OPEN_FOR_BACKUP_INTENT,
        NULL,
        0);

    return status;
}

NTSTATUS FileGetAttributes(const IN PWCHAR fileName, OUT PULONG attrs)
{
    NTSTATUS status;
    WCHAR fileNameNt[1024] = L"\\??\\"; // FIXME: long paths
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING fileNameU;
    FILE_BASIC_INFORMATION fbi;

    wcscat(fileNameNt, fileName);
    RtlInitUnicodeString(&fileNameU, fileNameNt);

    InitializeObjectAttributes(&oa, &fileNameU, OBJ_CASE_INSENSITIVE, NULL, NULL);
    status = NtQueryAttributesFile(&oa, &fbi);

    if (NT_SUCCESS(status))
        *attrs = fbi.FileAttributes;

    return status;
}

NTSTATUS FileGetSize(IN HANDLE file, OUT PLONGLONG fileSize)
{
    IO_STATUS_BLOCK iosb;
    FILE_STANDARD_INFORMATION fsi;
    NTSTATUS status;

    status = NtQueryInformationFile(file, &iosb, &fsi, sizeof(fsi), FileStandardInformation);
    if (NT_SUCCESS(status))
    {
        *fileSize = fsi.EndOfFile.QuadPart;
    }

    return status;
}

NTSTATUS FileGetPosition(IN HANDLE file, OUT PLONGLONG position)
{
    IO_STATUS_BLOCK iosb;
    FILE_POSITION_INFORMATION fpi;
    NTSTATUS status;

    status = NtQueryInformationFile(file, &iosb, &fpi, sizeof(fpi), FilePositionInformation);
    if (NT_SUCCESS(status))
    {
        *position = fpi.CurrentByteOffset.QuadPart;
    }

    return status;
}

NTSTATUS FileSetPosition(IN HANDLE file, IN LONGLONG position)
{
    IO_STATUS_BLOCK iosb;
    FILE_POSITION_INFORMATION fpi;
    NTSTATUS status;

    fpi.CurrentByteOffset.QuadPart = position;
    status = NtSetInformationFile(file, &iosb, &fpi, sizeof(fpi), FilePositionInformation);

    return status;
}

NTSTATUS FileRead(IN HANDLE file, OUT PVOID buffer, IN ULONG bufferSize, OUT PULONG readSize)
{
    IO_STATUS_BLOCK iosb;
    NTSTATUS status;

    status = NtReadFile(file, NULL, NULL, NULL, &iosb, buffer, bufferSize, NULL, NULL);
    if (NT_SUCCESS(status))
    {
        if (readSize)
            *readSize = (ULONG)iosb.Information; // single io block size is limited to 32bits
    }

    return status;
}

NTSTATUS FileWrite(IN HANDLE file, IN const PVOID buffer, IN ULONG bufferSize, OUT PULONG writtenSize)
{
    IO_STATUS_BLOCK iosb;
    NTSTATUS status;

    status = NtWriteFile(file, NULL, NULL, NULL, &iosb, buffer, bufferSize, NULL, NULL);

    if (NT_SUCCESS(status))
    {
        if (writtenSize)
            *writtenSize = (ULONG)iosb.Information; // single io block size is limited to 32bits
    }

    return status;
}

// Only works within file's volume.
NTSTATUS FileRename(IN const PWCHAR existingFileName, IN const PWCHAR newFileName, IN BOOLEAN replaceIfExists)
{
    PFILE_RENAME_INFORMATION fri;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    UNICODE_STRING existingFileNameU;
    WCHAR newFileNameNt[1024] = L"\\??\\";
    HANDLE file = NULL;
    ULONG fileNameSize;
    NTSTATUS status;

    existingFileNameU.Buffer = NULL;
    if (!RtlDosPathNameToNtPathName_U(existingFileName, &existingFileNameU, NULL, NULL))
    {
        status = STATUS_UNSUCCESSFUL;
        goto cleanup;
    }

    if ((wcslen(newFileName) > 2) && L':' == newFileName[1])
    {
        wcscat(newFileNameNt, newFileName);
    }
    else
    {
        wcsncpy(newFileNameNt, newFileName, RTL_NUMBER_OF(newFileNameNt) - 5);
    }

    InitializeObjectAttributes(
        &oa,
        &existingFileNameU,
        OBJ_CASE_INSENSITIVE,
        NULL,
        NULL);

    status = NtCreateFile(
        &file,
        FILE_ALL_ACCESS,
        &oa,
        &iosb,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        0,
        FILE_OPEN,
        FILE_SYNCHRONOUS_IO_NONALERT,
        NULL,
        0);

    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] NtCreateFile failed: %lx\n", status);
        goto cleanup;
    }

    fileNameSize = wcslen(newFileNameNt) * sizeof(*newFileNameNt);

    fri = (PFILE_RENAME_INFORMATION) RtlAllocateHeap(g_Heap,
        HEAP_ZERO_MEMORY, sizeof(FILE_RENAME_INFORMATION) + fileNameSize);

    if (!fri)
    {
        NtLog(TRUE, L"[!] RtlAllocateHeap failed\n");
        status = STATUS_NO_MEMORY;
        goto cleanup;
    }

    fri->RootDirectory = NULL;
    fri->ReplaceIfExists = replaceIfExists;
    fri->FileNameLength = fileNameSize;
    RtlCopyMemory(fri->FileName, newFileNameNt, fileNameSize);

    status = NtSetInformationFile(
        file,
        &iosb,
        fri,
        sizeof(FILE_RENAME_INFORMATION) + fileNameSize,
        FileRenameInformation);

    RtlFreeHeap(g_Heap, 0, fri);

cleanup:
    if (file)
        NtClose(file);
    if (existingFileNameU.Buffer)
        RtlFreeUnicodeString(&existingFileNameU);

    return status;
}

NTSTATUS FileCopySecurity(IN HANDLE source, IN HANDLE target)
{
    BYTE buffer[65536]; // maximum security descriptor size for NTFS (http://msdn.microsoft.com/en-us/library/windows/hardware/ff567066(v=vs.85).aspx)
    PSECURITY_DESCRIPTOR sd = (PSECURITY_DESCRIPTOR)buffer;
    ULONG requiredSize = 0;
    NTSTATUS status;

    status = NtQuerySecurityObject(
        source,
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | SACL_SECURITY_INFORMATION,
        sd,
        sizeof(buffer),
        &requiredSize);

    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] FileCopySecurity: NtQuerySecurityObject failed: %x\n", status);
        goto cleanup;
    }

    status = NtSetSecurityObject(
        target,
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | SACL_SECURITY_INFORMATION,
        sd);

    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] FileCopySecurity: NtSetSecurityObject failed: %x\n", status);
    }

cleanup:
    return status;
}

NTSTATUS FileCopy(IN const PWCHAR sourceName, IN const PWCHAR targetName)
{
    HANDLE fileSource = NULL;
    HANDLE fileTarget = NULL;
    BYTE buffer[16384];
    LONGLONG fileSize = 0;
    LONGLONG writtenTotal = 0;
    ULONG readSize = 0;
    ULONG writtenSize = 0;
    NTSTATUS status;

    status = FileOpen(&fileSource, sourceName, FALSE, FALSE, FALSE);
    if (!NT_SUCCESS(status))
        goto cleanup;

    status = FileOpen(&fileTarget, targetName, TRUE, TRUE, FALSE);
    if (!NT_SUCCESS(status))
        goto cleanup;

    status = FileCopySecurity(fileSource, fileTarget);
    if (!NT_SUCCESS(status))
        goto cleanup;

    status = FileGetSize(fileSource, &fileSize);
    if (!NT_SUCCESS(status))
        goto cleanup;

    // TODO: attributes, ADS?
    // Copy data.
    writtenTotal = 0;
    while (writtenTotal < fileSize)
    {
        readSize = 0;

        status = FileRead(fileSource, buffer, sizeof(buffer), &readSize);
        if (!NT_SUCCESS(status))
            goto cleanup;

        status = FileWrite(fileTarget, buffer, readSize, &writtenSize);
        if (!NT_SUCCESS(status))
            goto cleanup;

        if (readSize != writtenSize)
        {
            status = STATUS_UNSUCCESSFUL;
            goto cleanup;
        }

        writtenTotal += writtenSize;
    }

    if (writtenSize != fileSize)
        status = STATUS_UNSUCCESSFUL;

cleanup:
    if (fileSource)
        NtClose(fileSource);
    if (fileTarget)
        NtClose(fileTarget);

    return status;
}

NTSTATUS FileDelete(IN const PWCHAR path)
{
    UNICODE_STRING pathU;
    NTSTATUS status;
    OBJECT_ATTRIBUTES oa;

    pathU.Buffer = NULL;
    if (!RtlDosPathNameToNtPathName_U(path, &pathU, NULL, NULL))
    {
        status = STATUS_UNSUCCESSFUL;
        goto cleanup;
    }

    InitializeObjectAttributes(&oa, &pathU, OBJ_CASE_INSENSITIVE, NULL, NULL);
    
    status = NtDeleteFile(&oa);

cleanup:
    if (pathU.Buffer)
        RtlFreeUnicodeString(&pathU);
    return status;
}

NTSTATUS FileCreateDirectory(IN const PWCHAR path)
{
    UNICODE_STRING pathU;
    NTSTATUS status;
    HANDLE file = NULL;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;

    pathU.Buffer = NULL;
    if (!RtlDosPathNameToNtPathName_U(path, &pathU, NULL, NULL))
    {
        status = STATUS_UNSUCCESSFUL;
        goto cleanup;
    }

    InitializeObjectAttributes(&oa, &pathU, OBJ_CASE_INSENSITIVE, NULL, NULL);

    status = NtCreateFile(
        &file,
        FILE_LIST_DIRECTORY | SYNCHRONIZE | FILE_OPEN_FOR_BACKUP_INTENT,
        &oa,
        &iosb,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        0,
        FILE_CREATE,
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_DIRECTORY_FILE,
        NULL,
        0);

cleanup:
    if (pathU.Buffer)
        RtlFreeUnicodeString(&pathU);
    if (file)
        NtClose(file);
    return status;
}

NTSTATUS FileCopyReparsePoint(IN const PWCHAR sourcePath, IN const PWCHAR targetPath)
{
    BYTE buffer[MAX_PATH_LONG]; // MSDN doesn't specify maximum structure's length, but it should be close to MAX_PATH_LONG
    PREPARSE_DATA_BUFFER rdb = (PREPARSE_DATA_BUFFER)buffer;
    ULONG size;
    WCHAR dest[MAX_PATH_LONG];
    NTSTATUS status;
    HANDLE source = NULL;
    HANDLE target = NULL;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    
    status = FileOpen(&source, sourcePath, FALSE, FALSE, TRUE);
    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] FileOpen(%s) failed: %x\n", sourcePath, status);
        goto cleanup;
    }

    // Get reparse data.
    status = NtFsControlFile(
        source,
        NULL,
        NULL, NULL,
        &iosb,
        FSCTL_GET_REPARSE_POINT,
        NULL, 0,
        buffer, sizeof(buffer));

    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] FSCTL_GET_REPARSE_POINT failed: %x\n", status);
        goto cleanup;
    }

    if (rdb->ReparseTag == IO_REPARSE_TAG_SYMLINK)
    {
        memcpy(dest, rdb->SymbolicLinkReparseBuffer.PathBuffer +
            rdb->SymbolicLinkReparseBuffer.PrintNameOffset / sizeof(WCHAR),
            rdb->SymbolicLinkReparseBuffer.PrintNameLength);
        dest[rdb->SymbolicLinkReparseBuffer.PrintNameLength / sizeof(WCHAR)] = 0;

        NtLog(TRUE, L"[*] Symlink: '%s' -> '%s'\n", sourcePath, dest);
        /*
        if (!CreateSymbolicLink(targetPath, dest, (attrs & FILE_ATTRIBUTE_DIRECTORY) ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0))
        {
            status = perror("CreateSymbolicLink");
            goto cleanup;
        }
        */
    }
    else if (rdb->ReparseTag == IO_REPARSE_TAG_MOUNT_POINT)
    {
        memcpy(dest, rdb->MountPointReparseBuffer.PathBuffer +
            rdb->MountPointReparseBuffer.PrintNameOffset / sizeof(WCHAR),
            rdb->MountPointReparseBuffer.PrintNameLength);
        dest[rdb->MountPointReparseBuffer.PrintNameLength / sizeof(WCHAR)] = 0;
        NtLog(TRUE, L"[*] Mount point: '%s' -> '%s'\n", sourcePath, dest);

        // TODO: copy security
        FileCreateDirectory(targetPath);

        status = FileOpen(&target, targetPath, TRUE, FALSE, FALSE);
        if (!NT_SUCCESS(status))
        {
            NtLog(TRUE, L"[!] FileOpen(%s) failed: %x\n", targetPath, status);
            goto cleanup;
        }

        // Copy reparse data.
        status = NtFsControlFile(
            target,
            NULL,
            NULL, NULL,
            &iosb,
            FSCTL_SET_REPARSE_POINT,
            buffer, (ULONG)iosb.Information,
            NULL, 0);

        if (!NT_SUCCESS(status))
        {
            NtLog(TRUE, L"[!] FSCTL_SET_REPARSE_POINT failed: %x\n", status);
            goto cleanup;
        }
    }
    else
    {
        NtLog(TRUE, L"[!] Unknown reparse tag 0x%x in '%s'\n", rdb->ReparseTag, sourcePath);
    }

cleanup:
    if (source)
        NtClose(source);
    if (target)
        NtClose(target);
    return status;
}

NTSTATUS FileCopyDirectory(IN const PWCHAR sourcePath, IN const PWCHAR targetPath)
{
    UNICODE_STRING dirNameU;
    OBJECT_ATTRIBUTES oa;
    HANDLE dir = NULL, target = NULL;
    NTSTATUS status;
    IO_STATUS_BLOCK iosb;
    BOOLEAN firstQuery = TRUE;
    PFILE_FULL_DIR_INFORMATION dirInfo = NULL, entry;
    HANDLE event = NULL;
    WCHAR fullPath[MAX_PATH]; // FIXME: long paths
    WCHAR fullTargetPath[MAX_PATH];

    dirNameU.Buffer = NULL;
    if (!RtlDosPathNameToNtPathName_U(sourcePath, &dirNameU, NULL, NULL))
    {
        NtLog(TRUE, L"[!] RtlDosPathNameToNtPathName_U(%s) failed\n", sourcePath);
        status = STATUS_UNSUCCESSFUL;
        goto cleanup;
    }
    
    NtLog(TRUE, L"[D] %s\n", sourcePath);

    status = FileCreateDirectory(targetPath);
    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] FileCreateDirectory(%s) failed: %x\n", targetPath, status);
        goto cleanup;
    }

    status = FileOpen(&dir, sourcePath, FALSE, FALSE, FALSE);
    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] FileOpen(%s) failed: %x\n", sourcePath, status);
        goto cleanup;
    }

    // Copy security.
    status = FileOpen(&target, targetPath, TRUE, FALSE, FALSE);
    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] FileOpen(%s) failed: %x\n", targetPath, status);
        goto cleanup;
    }

    status = FileCopySecurity(dir, target);
    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] FileCopySecurity(%s, %s) failed: %x\n", sourcePath, targetPath, status);
        goto cleanup;
    }

    dirInfo = RtlAllocateHeap(g_Heap, 0, 16384);
    if (!dirInfo)
    {
        NtLog(TRUE, L"[!] RtlAllocateHeap(dirInfo) failed\n");
        status = STATUS_NO_MEMORY;
        goto cleanup;
    }

    InitializeObjectAttributes(&oa, NULL, 0, NULL, NULL);
    status = NtCreateEvent(
        &event,
        EVENT_ALL_ACCESS,
        &oa,
        SynchronizationEvent,
        FALSE);

    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] NtCreateEvent failed: %x\n", status);
        goto cleanup;
    }

    while (TRUE)
    {
        status = NtQueryDirectoryFile(
            dir,
            event,
            NULL,
            0,
            &iosb,
            dirInfo,
            16384,
            FileFullDirectoryInformation,
            FALSE,
            NULL,
            firstQuery);

        if (status == STATUS_PENDING)
        {
            NtWaitForSingleObject(event, FALSE, NULL);
            status = iosb.Status;
        }

        if (status == STATUS_NO_MORE_FILES)
            break;

        if (!NT_SUCCESS(status))
        {
            NtLog(FALSE, L"[*] NtQueryDirectoryFile: %x\n", status);
            goto cleanup;
        }

        entry = dirInfo;

        while (entry)
        {
            if (0 != wcsncmp(L".", entry->FileName, entry->FileNameLength / 2) &&
                0 != wcsncmp(L"..", entry->FileName, entry->FileNameLength / 2))
            {
                NtLog(FALSE, L"> %.*s [A: %x, EA: %x, Size: %ld]\n",
                    entry->FileNameLength / 2, entry->FileName,
                    entry->FileAttributes, entry->EaSize, entry->AllocationSize.QuadPart);

                wcscpy(fullPath, sourcePath);
                wcscat(fullPath, L"\\");
                wcsncat(fullPath, entry->FileName, entry->FileNameLength / 2);
                wcscpy(fullTargetPath, targetPath);
                wcscat(fullTargetPath, L"\\");
                wcsncat(fullTargetPath, entry->FileName, entry->FileNameLength / 2);

                if (entry->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                {
                    FileCopyReparsePoint(fullPath, fullTargetPath);
                }
                else if (entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                {
                    FileCopyDirectory(fullPath, fullTargetPath);
                }
                else
                {
                    FileCopy(fullPath, fullTargetPath);
                }
            }

            if (!entry->NextEntryOffset)
                break;

            // Move to next entry.
            entry = (PFILE_FULL_DIR_INFORMATION)((ULONG_PTR)entry + entry->NextEntryOffset);
        }

        firstQuery = FALSE;
    }

    status = STATUS_SUCCESS;

cleanup:
    if (dirNameU.Buffer)
        RtlFreeUnicodeString(&dirNameU);
    if (dirInfo)
        RtlFreeHeap(g_Heap, 0, dirInfo);
    if (event)
        NtClose(event);
    if (dir)
        NtClose(dir);
    if (target)
        NtClose(target);
    return status;
}
