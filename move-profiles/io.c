#include "io.h"

NTSTATUS FileOpen(OUT HANDLE *file, const IN WCHAR *fileName, IN BOOLEAN write, IN BOOLEAN overwrite, IN BOOLEAN isReparse)
{
    UNICODE_STRING fileNameU = { 0 };
    IO_STATUS_BLOCK iosb;
    ULONG createDisposition = 0;
    OBJECT_ATTRIBUTES oa;
    ACCESS_MASK desiredAccess;
    NTSTATUS status;

    if (!RtlDosPathNameToNtPathName_U(fileName, &fileNameU, NULL, NULL))
    {
        status = STATUS_INVALID_PARAMETER_2;
        goto cleanup;
    }

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
            desiredAccess |= FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA | WRITE_DAC | WRITE_OWNER | DELETE;

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

cleanup:

    if (fileNameU.Buffer)
        RtlFreeUnicodeString(&fileNameU);
    return status;
}

NTSTATUS FileGetAttributes(const IN WCHAR *fileName, OUT ULONG *attrs)
{
    NTSTATUS status;
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING fileNameU = { 0 };
    FILE_BASIC_INFORMATION fbi;

    if (!RtlDosPathNameToNtPathName_U(fileName, &fileNameU, NULL, NULL))
    {
        status = STATUS_INVALID_PARAMETER_1;
        goto cleanup;
    }

    InitializeObjectAttributes(&oa, &fileNameU, OBJ_CASE_INSENSITIVE, NULL, NULL);
    status = NtQueryAttributesFile(&oa, &fbi);

    if (NT_SUCCESS(status))
        *attrs = fbi.FileAttributes;

cleanup:

    if (fileNameU.Buffer)
        RtlFreeUnicodeString(&fileNameU);

    return status;
}

NTSTATUS FileSetAttributes(const IN WCHAR *fileName, IN ULONG attrs)
{
    NTSTATUS status;
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING fileNameU = { 0 };
    FILE_BASIC_INFORMATION fbi;
    IO_STATUS_BLOCK iosb;
    HANDLE file = NULL;

    if (!RtlDosPathNameToNtPathName_U(fileName, &fileNameU, NULL, NULL))
    {
        status = STATUS_INVALID_PARAMETER_1;
        goto cleanup;
    }

    InitializeObjectAttributes(&oa, &fileNameU, OBJ_CASE_INSENSITIVE, NULL, NULL);

    // Not using FileOpen because it requests DELETE access by default, which would fail for read-only files.
    status = NtCreateFile(
        &file,
        SYNCHRONIZE | FILE_WRITE_ATTRIBUTES,
        &oa,
        &iosb,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        0, // no sharing
        FILE_OPEN,
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT | FILE_OPEN_FOR_BACKUP_INTENT,
        NULL,
        0);

    if (!NT_SUCCESS(status))
        goto cleanup;

    // Read current attributes (to not overwrite creation time etc).
    status = NtQueryAttributesFile(&oa, &fbi);
    if (!NT_SUCCESS(status))
        goto cleanup;

    fbi.FileAttributes = attrs;

    // Set new attributes.
    status = NtSetInformationFile(file, &iosb, &fbi, sizeof(fbi), FileBasicInformation);

cleanup:

    if (fileNameU.Buffer)
        RtlFreeUnicodeString(&fileNameU);
    if (file)
        NtClose(file);

    return status;
}

NTSTATUS FileGetSize(IN HANDLE file, OUT INT64 *fileSize)
{
    IO_STATUS_BLOCK iosb;
    FILE_STANDARD_INFORMATION fsi;
    NTSTATUS status;

    status = NtQueryInformationFile(file, &iosb, &fsi, sizeof(fsi), FileStandardInformation);
    if (NT_SUCCESS(status))
        *fileSize = fsi.EndOfFile.QuadPart;

    return status;
}

NTSTATUS FileGetPosition(IN HANDLE file, OUT INT64 *position)
{
    IO_STATUS_BLOCK iosb;
    FILE_POSITION_INFORMATION fpi;
    NTSTATUS status;

    status = NtQueryInformationFile(file, &iosb, &fpi, sizeof(fpi), FilePositionInformation);
    if (NT_SUCCESS(status))
        *position = fpi.CurrentByteOffset.QuadPart;

    return status;
}

NTSTATUS FileSetPosition(IN HANDLE file, IN INT64 position)
{
    IO_STATUS_BLOCK iosb;
    FILE_POSITION_INFORMATION fpi;
    NTSTATUS status;

    fpi.CurrentByteOffset.QuadPart = position;
    status = NtSetInformationFile(file, &iosb, &fpi, sizeof(fpi), FilePositionInformation);

    return status;
}

NTSTATUS FileRead(IN HANDLE file, OUT void *buffer, IN ULONG bufferSize, OUT ULONG *readSize)
{
    IO_STATUS_BLOCK iosb;
    NTSTATUS status;

    status = NtReadFile(file, NULL, NULL, NULL, &iosb, buffer, bufferSize, NULL, NULL);
    if (NT_SUCCESS(status))
    {
        if (readSize)
            *readSize = (ULONG) iosb.Information; // single io block size is limited to 32bits
    }

    return status;
}

NTSTATUS FileWrite(IN HANDLE file, IN const void *buffer, IN ULONG bufferSize, OUT ULONG *writtenSize)
{
    IO_STATUS_BLOCK iosb;
    NTSTATUS status;

    status = NtWriteFile(file, NULL, NULL, NULL, &iosb, buffer, bufferSize, NULL, NULL);

    if (NT_SUCCESS(status))
    {
        if (writtenSize)
            *writtenSize = (ULONG) iosb.Information; // single io block size is limited to 32bits
    }

    return status;
}

// Only works within file's volume.
NTSTATUS FileRename(IN const WCHAR *existingFileName, IN const WCHAR *newFileName, IN BOOLEAN replaceIfExists)
{
    FILE_RENAME_INFORMATION *fri;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    UNICODE_STRING existingFileNameU = { 0 };
    UNICODE_STRING newFileNameU = { 0 };
    HANDLE file = NULL;
    ULONG fileNameSize;
    NTSTATUS status;

    if (!RtlDosPathNameToNtPathName_U(existingFileName, &existingFileNameU, NULL, NULL))
    {
        status = STATUS_INVALID_PARAMETER_1;
        goto cleanup;
    }

    if (!RtlDosPathNameToNtPathName_U(newFileName, &newFileNameU, NULL, NULL))
    {
        status = STATUS_INVALID_PARAMETER_2;
        goto cleanup;
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
        NtLog(TRUE, L"[!] FileRename: NtCreateFile(%s) failed: %lx\n", existingFileName, status);
        goto cleanup;
    }

    fileNameSize = newFileNameU.Length * sizeof(WCHAR);

    fri = (FILE_RENAME_INFORMATION *) RtlAllocateHeap(g_Heap, HEAP_ZERO_MEMORY, sizeof(FILE_RENAME_INFORMATION) + fileNameSize);

    if (!fri)
    {
        NtLog(TRUE, L"[!] FileRename: RtlAllocateHeap failed\n");
        status = STATUS_NO_MEMORY;
        goto cleanup;
    }

    fri->RootDirectory = NULL;
    fri->ReplaceIfExists = replaceIfExists;
    fri->FileNameLength = fileNameSize;
    RtlCopyMemory(fri->FileName, newFileNameU.Buffer, fileNameSize);

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
    if (newFileNameU.Buffer)
        RtlFreeUnicodeString(&newFileNameU);

    return status;
}

NTSTATUS FileCopySecurity(IN HANDLE source, IN HANDLE target)
{
    SECURITY_DESCRIPTOR *sd = NULL;
    ULONG requiredSize = 0;
    NTSTATUS status;

    sd = RtlAllocateHeap(g_Heap, 0, 65536); // don't allocate on stack, deep recursion can be fatal
    // maximum security descriptor size for NTFS (http://msdn.microsoft.com/en-us/library/windows/hardware/ff567066(v=vs.85).aspx)
    if (!sd)
    {
        NtLog(TRUE, L"[!] FileCopySecurity: RtlAllocateHeap failed\n");
        status = STATUS_NO_MEMORY;
        goto cleanup;
    }

    status = NtQuerySecurityObject(
        source,
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | SACL_SECURITY_INFORMATION,
        sd,
        65536,
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

    if (sd)
        RtlFreeHeap(g_Heap, 0, sd);
    return status;
}

NTSTATUS FileCopyBasicInformation(IN HANDLE source, IN HANDLE target)
{
    NTSTATUS status;
    IO_STATUS_BLOCK iosb;
    FILE_BASIC_INFORMATION fbi;

    status = NtQueryInformationFile(source, &iosb, &fbi, sizeof(fbi), FileBasicInformation);
    if (!NT_SUCCESS(status))
        goto cleanup;

    status = NtSetInformationFile(target, &iosb, &fbi, sizeof(fbi), FileBasicInformation);

cleanup:

    return status;
}

NTSTATUS FileCopy(IN const WCHAR *sourceName, IN const WCHAR *targetName)
{
    HANDLE fileSource = NULL;
    HANDLE fileTarget = NULL;
    BYTE *buffer = NULL;
    INT64 fileSize = 0;
    INT64 writtenTotal = 0;
    ULONG readSize = 0;
    ULONG writtenSize = 0;
    NTSTATUS status;

    status = FileOpen(&fileSource, sourceName, FALSE, FALSE, FALSE);
    if (!NT_SUCCESS(status))
        goto cleanup;

    status = FileOpen(&fileTarget, targetName, TRUE, TRUE, FALSE);
    if (!NT_SUCCESS(status))
        goto cleanup;

    status = FileCopyBasicInformation(fileSource, fileTarget);
    if (!NT_SUCCESS(status))
        goto cleanup;

    status = FileCopySecurity(fileSource, fileTarget);
    if (!NT_SUCCESS(status))
        goto cleanup;

    status = FileGetSize(fileSource, &fileSize);
    if (!NT_SUCCESS(status))
        goto cleanup;

    buffer = RtlAllocateHeap(g_Heap, 0, 65536); // don't allocate on stack, deep recursion can be fatal
    if (!buffer)
    {
        status = STATUS_NO_MEMORY;
        goto cleanup;
    }

    // TODO: ADS, quota, ...?
    // Copy data.
    writtenTotal = 0;
    while (writtenTotal < fileSize)
    {
        readSize = 0;

        status = FileRead(fileSource, buffer, 65536, &readSize);
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

    if (buffer)
        RtlFreeHeap(g_Heap, 0, buffer);
    if (fileSource)
        NtClose(fileSource);
    if (fileTarget)
        NtClose(fileTarget);

    return status;
}

// Read-only attribute must be cleared, otherwise the file can't be opened for DELETE access.
NTSTATUS FileDelete(IN HANDLE file)
{
    NTSTATUS status;
    FILE_BASIC_INFORMATION fbi;
    FILE_DISPOSITION_INFORMATION fdi;
    REPARSE_DATA_BUFFER *rdb = NULL;
    REPARSE_GUID_DATA_BUFFER rgdb = { 0 };
    IO_STATUS_BLOCK iosb;

    // Get attributes.
    status = NtQueryInformationFile(file, &iosb, &fbi, sizeof(fbi), FileBasicInformation);
    if (!NT_SUCCESS(status))
        goto cleanup;

    if (fbi.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
    {
        // Get reparse tag.

        // MSDN doesn't specify maximum structure's length, but it should be close to MAX_PATH_LONG
        rdb = (REPARSE_DATA_BUFFER *) RtlAllocateHeap(g_Heap, 0, MAX_PATH_LONG); // don't allocate on stack, deep recursion can be fatal
        if (!rdb)
        {
            status = STATUS_NO_MEMORY;
            goto cleanup;
        }

        // Get reparse data.
        status = NtFsControlFile(
            file,
            NULL,
            NULL, NULL,
            &iosb,
            FSCTL_GET_REPARSE_POINT,
            NULL, 0,
            rdb, MAX_PATH_LONG);

        if (!NT_SUCCESS(status))
        {
            NtLog(TRUE, L"[!] FileDelete: FSCTL_GET_REPARSE_POINT failed: %x\n", status);
            goto cleanup;
        }

        rgdb.ReparseTag = rdb->ReparseTag;

        // Delete reparse point.
        status = NtFsControlFile(
            file,
            NULL,
            NULL, NULL,
            &iosb,
            FSCTL_DELETE_REPARSE_POINT,
            &rgdb, REPARSE_GUID_DATA_BUFFER_HEADER_SIZE,
            NULL, 0);

        if (!NT_SUCCESS(status))
        {
            NtLog(TRUE, L"[!] FileDelete: FSCTL_DELETE_REPARSE_POINT failed: %x\n", status);
            goto cleanup;
        }
    }

    // Set delete disposition.
    fdi.DeleteFile = TRUE;

    status = NtSetInformationFile(file, &iosb, &fdi, sizeof(fdi), FileDispositionInformation);

cleanup:
    if (rdb)
        RtlFreeHeap(g_Heap, 0, rdb);
    return status;
}

NTSTATUS FileCreateDirectory(IN const WCHAR *path)
{
    UNICODE_STRING pathU = { 0 };
    NTSTATUS status;
    HANDLE file = NULL;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;

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

// sourcePath must be an existing and empty directory.
NTSTATUS FileSetSymlink(IN const WCHAR *sourcePath, IN const WCHAR *targetPath)
{
    BYTE buffer[MAX_PATH_LONG]; // MSDN doesn't specify maximum structure's length, but it should be close to MAX_PATH_LONG
    REPARSE_DATA_BUFFER *rdb = (REPARSE_DATA_BUFFER *) buffer;
    DWORD size, targetSize;
    WCHAR dest[MAX_PATH_LONG];
    NTSTATUS status;
    HANDLE file = NULL;
    IO_STATUS_BLOCK iosb;

    status = FileOpen(&file, sourcePath, TRUE, FALSE, FALSE);
    if (!NT_SUCCESS(status))
        goto cleanup;

    targetSize = wcslen(targetPath) * sizeof(WCHAR);

    rdb->ReparseTag = IO_REPARSE_TAG_SYMLINK;
    // 12 = SymbolicLinkReparseBuffer fields without PathBuffer
    // sizeof(PathBuffer) = 2*targetSize + sizeof(L"\\??\\")
    rdb->ReparseDataLength = (USHORT) (12 + targetSize * 2 + 8);
    rdb->SymbolicLinkReparseBuffer.Flags = 0; // absolute link

    swprintf_s(rdb->SymbolicLinkReparseBuffer.PathBuffer, (sizeof(buffer) - sizeof(REPARSE_DATA_BUFFER)) / sizeof(WCHAR),
        L"%s\\??\\%s", targetPath, targetPath);

    rdb->SymbolicLinkReparseBuffer.PrintNameOffset = 0;
    rdb->SymbolicLinkReparseBuffer.PrintNameLength = (USHORT) targetSize;
    rdb->SymbolicLinkReparseBuffer.SubstituteNameOffset = rdb->SymbolicLinkReparseBuffer.PrintNameLength;
    rdb->SymbolicLinkReparseBuffer.SubstituteNameLength = (USHORT) (targetSize + 8);

    NtLog(FALSE, L"PrintName: %.*s\n", rdb->SymbolicLinkReparseBuffer.PrintNameLength / 2, rdb->SymbolicLinkReparseBuffer.PathBuffer + rdb->SymbolicLinkReparseBuffer.PrintNameOffset / 2);
    NtLog(FALSE, L"SubstituteName: %.*s\n", rdb->SymbolicLinkReparseBuffer.SubstituteNameLength / 2, rdb->SymbolicLinkReparseBuffer.PathBuffer + rdb->SymbolicLinkReparseBuffer.SubstituteNameOffset / 2);

    status = NtFsControlFile(
        file,
        NULL,
        NULL, NULL,
        &iosb,
        FSCTL_SET_REPARSE_POINT,
        // 8 = fields before the union
        buffer, rdb->ReparseDataLength + 8,
        NULL, 0);

    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] FileSetReparsePoint: FSCTL_SET_REPARSE_POINT(%s) failed: %x\n", sourcePath, status);
        goto cleanup;
    }

    status = STATUS_SUCCESS;

cleanup:
    if (file)
        NtClose(file);
    return status;
}

NTSTATUS FileCopyReparsePoint(IN const WCHAR *sourcePath, IN const WCHAR *targetPath)
{
    REPARSE_DATA_BUFFER *rdb = NULL;
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
        NtLog(TRUE, L"[!] FileCopyReparsePoint: FileOpen(%s) failed: %x\n", sourcePath, status);
        goto cleanup;
    }

    // MSDN doesn't specify maximum structure's length, but it should be close to MAX_PATH_LONG
    rdb = RtlAllocateHeap(g_Heap, 0, MAX_PATH_LONG); // don't allocate on stack, deep recursion can be fatal
    if (!rdb)
    {
        status = STATUS_NO_MEMORY;
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
        rdb, MAX_PATH_LONG);

    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] FileCopyReparsePoint: FSCTL_GET_REPARSE_POINT(%s) failed: %x\n", sourcePath, status);
        goto cleanup;
    }

    switch (rdb->ReparseTag)
    {
    case IO_REPARSE_TAG_SYMLINK:

        memcpy(dest, rdb->SymbolicLinkReparseBuffer.PathBuffer +
            rdb->SymbolicLinkReparseBuffer.PrintNameOffset / sizeof(WCHAR),
            rdb->SymbolicLinkReparseBuffer.PrintNameLength);
        dest[rdb->SymbolicLinkReparseBuffer.PrintNameLength / sizeof(WCHAR)] = 0;

        NtLog(FALSE, L"[*] Symlink: '%s' -> '%s'\n", sourcePath, dest);
        break;

    case IO_REPARSE_TAG_MOUNT_POINT:

        memcpy(dest, rdb->MountPointReparseBuffer.PathBuffer +
            rdb->MountPointReparseBuffer.PrintNameOffset / sizeof(WCHAR),
            rdb->MountPointReparseBuffer.PrintNameLength);
        dest[rdb->MountPointReparseBuffer.PrintNameLength / sizeof(WCHAR)] = 0;

        NtLog(FALSE, L"[*] Mount point: '%s' -> '%s'\n", sourcePath, dest);
        break;

    default:

        NtLog(TRUE, L"[!] FileCopyReparsePoint: Unknown reparse tag 0x%x in '%s'\n", rdb->ReparseTag, sourcePath);
        goto cleanup;
    }

    status = FileCreateDirectory(targetPath);
    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] FileCopyReparsePoint: FileCreateDirectory(%s) failed: %x\n", targetPath, status);
        goto cleanup;
    }

    status = FileOpen(&target, targetPath, TRUE, FALSE, FALSE);
    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] FileCopyReparsePoint: FileOpen(%s) failed: %x\n", targetPath, status);
        goto cleanup;
    }

    status = FileCopyBasicInformation(source, target);
    if (!NT_SUCCESS(status))
        goto cleanup;

    status = FileCopySecurity(source, target);
    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] FileCopyReparsePoint: FileCopySecurity(%s, %s) failed: %x\n", sourcePath, targetPath, status);
        goto cleanup;
    }

    // Copy reparse data.
    status = NtFsControlFile(
        target,
        NULL,
        NULL, NULL,
        &iosb,
        FSCTL_SET_REPARSE_POINT,
        rdb, (ULONG) iosb.Information,
        NULL, 0);

    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] FileCopyReparsePoint: FSCTL_SET_REPARSE_POINT(%s) failed: %x\n", targetPath, status);
        goto cleanup;
    }

cleanup:
    if (rdb)
        RtlFreeHeap(g_Heap, 0, rdb);
    if (source)
        NtClose(source);
    if (target)
        NtClose(target);
    return status;
}

NTSTATUS FileCopyDirectory(IN const WCHAR *sourcePath, IN const WCHAR *targetPath, IN BOOLEAN ignoreErrors)
{
    UNICODE_STRING dirNameU = { 0 };
    OBJECT_ATTRIBUTES oa;
    HANDLE dir = NULL, target = NULL;
    NTSTATUS status;
    IO_STATUS_BLOCK iosb;
    BOOLEAN firstQuery = TRUE;
    FILE_FULL_DIR_INFORMATION *dirInfo = NULL, *entry;
    HANDLE event = NULL;
    WCHAR *fullPath = NULL;
    WCHAR *fullTargetPath = NULL;

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
        if (!ignoreErrors)
            goto cleanup;
    }

    status = FileOpen(&dir, sourcePath, FALSE, FALSE, FALSE);
    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] FileOpen(%s) failed: %x\n", sourcePath, status);
        goto cleanup;
    }

    status = FileOpen(&target, targetPath, TRUE, FALSE, FALSE);
    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] FileOpen(%s) failed: %x\n", targetPath, status);
        goto cleanup;
    }

    status = FileCopyBasicInformation(dir, target);
    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] FileCopyBasicInformation(%s, %s) failed: %x\n", sourcePath, targetPath, status);
        if (!ignoreErrors)
            goto cleanup;
    }

    status = FileCopySecurity(dir, target);
    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] FileCopySecurity(%s, %s) failed: %x\n", sourcePath, targetPath, status);
        if (!ignoreErrors)
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
            NtLog(TRUE, L"[!] NtQueryDirectoryFile(%s) failed: %x\n", sourcePath, status);
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

                if (!fullPath)
                    fullPath = RtlAllocateHeap(g_Heap, HEAP_ZERO_MEMORY, MAX_PATH_LONG*sizeof(WCHAR));
                if (!fullTargetPath)
                    fullTargetPath = RtlAllocateHeap(g_Heap, HEAP_ZERO_MEMORY, MAX_PATH_LONG*sizeof(WCHAR));

                wcscpy_s(fullPath, MAX_PATH_LONG - 1, sourcePath); // 1 for backslash
                wcscat_s(fullPath, MAX_PATH_LONG, L"\\");
                wcsncat_s(fullPath, MAX_PATH_LONG, entry->FileName, entry->FileNameLength / 2);
                wcscpy_s(fullTargetPath, MAX_PATH_LONG - 1, targetPath); // 1 for backslash
                wcscat_s(fullTargetPath, MAX_PATH_LONG, L"\\");
                wcsncat_s(fullTargetPath, MAX_PATH_LONG, entry->FileName, entry->FileNameLength / 2);

                if (entry->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                {
                    FileCopyReparsePoint(fullPath, fullTargetPath);
                }
                else if (entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                {
                    FileCopyDirectory(fullPath, fullTargetPath, ignoreErrors);
                }
                else
                {
                    FileCopy(fullPath, fullTargetPath);
                }
            }

            if (!entry->NextEntryOffset)
                break;

            // Move to next entry.
            entry = (FILE_FULL_DIR_INFORMATION *) ((ULONG_PTR) entry + entry->NextEntryOffset);
        }

        firstQuery = FALSE;
    }

    status = STATUS_SUCCESS;

cleanup:
    if (dirNameU.Buffer)
        RtlFreeUnicodeString(&dirNameU);
    if (dirInfo)
        RtlFreeHeap(g_Heap, 0, dirInfo);
    if (fullPath)
        RtlFreeHeap(g_Heap, 0, fullPath);
    if (fullTargetPath)
        RtlFreeHeap(g_Heap, 0, fullTargetPath);
    if (event)
        NtClose(event);
    if (dir)
        NtClose(dir);
    if (target)
        NtClose(target);
    return status;
}

NTSTATUS FileDeleteDirectory(IN const WCHAR *path, IN BOOLEAN deleteSelf)
{
    UNICODE_STRING dirNameU = { 0 };
    OBJECT_ATTRIBUTES oa;
    HANDLE dir = NULL, file;
    NTSTATUS status;
    IO_STATUS_BLOCK iosb;
    BOOLEAN firstQuery = TRUE;
    FILE_FULL_DIR_INFORMATION *dirInfo = NULL, *entry;
    HANDLE event = NULL;
    WCHAR *fullPath = NULL;
    ULONG attrs;

    if (!RtlDosPathNameToNtPathName_U(path, &dirNameU, NULL, NULL))
    {
        NtLog(TRUE, L"[!] RtlDosPathNameToNtPathName_U(%s) failed\n", path);
        status = STATUS_UNSUCCESSFUL;
        goto cleanup;
    }

    NtLog(TRUE, L"[~] %s\n", path);

    status = FileOpen(&dir, path, FALSE, FALSE, FALSE);
    if (!NT_SUCCESS(status))
    {
        NtLog(TRUE, L"[!] FileOpen(%s) failed: %x\n", path, status);
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
            NtLog(TRUE, L"[!] NtQueryDirectoryFile(%s) failed: %x\n", path, status);
            goto cleanup;
        }

        entry = dirInfo;

        while (entry)
        {
            if (0 != wcsncmp(L".", entry->FileName, entry->FileNameLength / 2) &&
                0 != wcsncmp(L"..", entry->FileName, entry->FileNameLength / 2))
            {
                NtLog(FALSE, L"- %.*s [A: %x, EA: %x, Size: %ld]\n",
                    entry->FileNameLength / 2, entry->FileName,
                    entry->FileAttributes, entry->EaSize, entry->AllocationSize.QuadPart);

                if (!fullPath)
                    fullPath = RtlAllocateHeap(g_Heap, HEAP_ZERO_MEMORY, MAX_PATH_LONG*sizeof(WCHAR));

                wcscpy_s(fullPath, MAX_PATH_LONG - 1, path); // 1 for backslash
                wcscat_s(fullPath, MAX_PATH_LONG, L"\\");
                wcsncat_s(fullPath, MAX_PATH_LONG, entry->FileName, entry->FileNameLength / 2);

                // Clear read-only attribute.
                if (entry->FileAttributes & FILE_ATTRIBUTE_READONLY)
                {
                    status = FileSetAttributes(fullPath, entry->FileAttributes & ~FILE_ATTRIBUTE_READONLY);
                    if (!NT_SUCCESS(status))
                    {
                        NtLog(TRUE, L"[!] FileSetAttributes(%s) failed: %x\n", fullPath, status);
                        goto cleanup;
                    }
                }

                if ((entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(entry->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                {
                    // directory that is not a reparse point: recursively delete
                    status = FileDeleteDirectory(fullPath, TRUE);
                    if (!NT_SUCCESS(status))
                    {
                        NtLog(TRUE, L"[!] FileDeleteDirectory(%s) failed: %x\n", fullPath, status);
                        goto cleanup;
                    }
                }
                else
                {
                    // just delete
                    status = FileOpen(&file, fullPath, TRUE, FALSE, FALSE);
                    if (!NT_SUCCESS(status))
                    {
                        NtLog(TRUE, L"[!] FileOpen(%s) failed: %x\n", fullPath, status);
                        goto cleanup;
                    }

                    status = FileDelete(file);
                    if (!NT_SUCCESS(status))
                    {
                        NtLog(TRUE, L"[!] FileDelete(%s) failed: %x\n", fullPath, status);
                        NtClose(file);
                        goto cleanup;
                    }
                    NtClose(file);
                }
            }

            if (!entry->NextEntryOffset)
                break;

            // Move to next entry.
            entry = (FILE_FULL_DIR_INFORMATION *) ((ULONG_PTR) entry + entry->NextEntryOffset);
        }

        firstQuery = FALSE;
    }

    if (deleteSelf)
    {
        NtClose(dir);

        // Clear read-only attribute.
        status = FileGetAttributes(path, &attrs);
        if (!NT_SUCCESS(status))
        {
            NtLog(TRUE, L"[!] FileGetAttributes(%s) failed: %x\n", path, status);
            goto cleanup;
        }

        if (attrs & FILE_ATTRIBUTE_READONLY)
        {
            status = FileSetAttributes(path, attrs & ~FILE_ATTRIBUTE_READONLY);
            if (!NT_SUCCESS(status))
            {
                NtLog(TRUE, L"[!] FileSetAttributes(%s) failed: %x\n", path, status);
                goto cleanup;
            }
        }
        // Reopen for write.
        status = FileOpen(&dir, path, TRUE, FALSE, FALSE);
        if (!NT_SUCCESS(status))
        {
            NtLog(TRUE, L"[!] FileOpen(%s) failed: %x\n", path, status);
            goto cleanup;
        }

        // Delete the parent itself.
        status = FileDelete(dir);
        if (!NT_SUCCESS(status))
        {
            NtLog(TRUE, L"[!] FileDelete(%s) failed: %x\n", path, status);
            goto cleanup;
        }
    }

    status = STATUS_SUCCESS;

cleanup:
    if (dirNameU.Buffer)
        RtlFreeUnicodeString(&dirNameU);
    if (dirInfo)
        RtlFreeHeap(g_Heap, 0, dirInfo);
    if (fullPath)
        RtlFreeHeap(g_Heap, 0, fullPath);
    if (event)
        NtClose(event);
    if (dir)
        NtClose(dir);
    return status;
}
