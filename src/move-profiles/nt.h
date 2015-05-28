#pragma once

#ifndef WIN32_NO_STATUS
#define WIN32_NO_STATUS
#endif

#include <windef.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <winioctl.h>

#define FASTCALL
#define NT_SUCCESS(Status)              (((NTSTATUS)(Status)) >= 0)

typedef long NTSTATUS;

#define NtCurrentProcess()                      ((HANDLE)(LONG_PTR)-1)

typedef struct _UNICODE_STRING
{
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
} UNICODE_STRING;

typedef struct _OBJECT_ATTRIBUTES
{
    ULONG Length;
    HANDLE RootDirectory;
    UNICODE_STRING *ObjectName;
    ULONG Attributes;
    void *SecurityDescriptor;
    void *SecurityQualityOfService;
} OBJECT_ATTRIBUTES;

/////////////////// Io ///////////////////
//
// NtCreateFile Flags
//
#define FILE_DIRECTORY_FILE                     0x00000001
#define FILE_WRITE_THROUGH                      0x00000002
#define FILE_SEQUENTIAL_ONLY                    0x00000004
#define FILE_NO_INTERMEDIATE_BUFFERING          0x00000008
#define FILE_SYNCHRONOUS_IO_ALERT               0x00000010
#define FILE_SYNCHRONOUS_IO_NONALERT            0x00000020
#define FILE_NON_DIRECTORY_FILE                 0x00000040
#define FILE_CREATE_TREE_CONNECTION             0x00000080
#define FILE_COMPLETE_IF_OPLOCKED               0x00000100
#define FILE_NO_EA_KNOWLEDGE                    0x00000200
#define FILE_OPEN_REMOTE_INSTANCE               0x00000400
#define FILE_RANDOM_ACCESS                      0x00000800
#define FILE_DELETE_ON_CLOSE                    0x00001000
#define FILE_OPEN_BY_FILE_ID                    0x00002000
#define FILE_OPEN_FOR_BACKUP_INTENT             0x00004000
#define FILE_NO_COMPRESSION                     0x00008000
#define FILE_RESERVE_OPFILTER                   0x00100000
#define FILE_OPEN_REPARSE_POINT                 0x00200000
#define FILE_OPEN_NO_RECALL                     0x00400000
#define FILE_OPEN_FOR_FREE_SPACE_QUERY          0x00800000

//
// NtCreateFile OpenType Flags
//
#define FILE_SUPERSEDE                          0x00000000
#define FILE_OPEN                               0x00000001
#define FILE_CREATE                             0x00000002
#define FILE_OPEN_IF                            0x00000003
#define FILE_OVERWRITE                          0x00000004
#define FILE_OVERWRITE_IF                       0x00000005
#define FILE_MAXIMUM_DISPOSITION                0x00000005

//
// File Information Classes for NtQueryInformationFile
//
typedef enum _FILE_INFORMATION_CLASS
{
    FileDirectoryInformation = 1,
    FileFullDirectoryInformation,
    FileBothDirectoryInformation,
    FileBasicInformation,
    FileStandardInformation,
    FileInternalInformation,
    FileEaInformation,
    FileAccessInformation,
    FileNameInformation,
    FileRenameInformation,
    FileLinkInformation,
    FileNamesInformation,
    FileDispositionInformation,
    FilePositionInformation,
    FileFullEaInformation,
    FileModeInformation,
    FileAlignmentInformation,
    FileAllInformation,
    FileAllocationInformation,
    FileEndOfFileInformation,
    FileAlternateNameInformation,
    FileStreamInformation,
    FilePipeInformation,
    FilePipeLocalInformation,
    FilePipeRemoteInformation,
    FileMailslotQueryInformation,
    FileMailslotSetInformation,
    FileCompressionInformation,
    FileObjectIdInformation,
    FileCompletionInformation,
    FileMoveClusterInformation,
    FileQuotaInformation,
    FileReparsePointInformation,
    FileNetworkOpenInformation,
    FileAttributeTagInformation,
    FileTrackingInformation,
    FileIdBothDirectoryInformation,
    FileIdFullDirectoryInformation,
    FileValidDataLengthInformation,
    FileShortNameInformation,
    FileMaximumInformation
} FILE_INFORMATION_CLASS;

//
// I/O Status Block
//
typedef struct _IO_STATUS_BLOCK
{
    union
    {
        NTSTATUS Status;
        void *Pointer;
    };
    ULONG_PTR Information;
} IO_STATUS_BLOCK;

//
// File Information structures for NtQueryInformationFile
//
typedef struct _FILE_BASIC_INFORMATION
{
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    ULONG FileAttributes;
} FILE_BASIC_INFORMATION;

typedef struct _FILE_STANDARD_INFORMATION
{
    LARGE_INTEGER AllocationSize;
    LARGE_INTEGER EndOfFile;
    ULONG NumberOfLinks;
    BOOLEAN DeletePending;
    BOOLEAN Directory;
} FILE_STANDARD_INFORMATION;

typedef struct _FILE_STREAM_INFORMATION
{
    ULONG NextEntryOffset;
    ULONG StreamNameLength;
    LARGE_INTEGER StreamSize;
    LARGE_INTEGER StreamAllocationSize;
    WCHAR StreamName[1];
} FILE_STREAM_INFORMATION;

typedef struct _FILE_POSITION_INFORMATION
{
    LARGE_INTEGER CurrentByteOffset;
} FILE_POSITION_INFORMATION;

typedef struct _FILE_RENAME_INFORMATION
{
    BOOLEAN ReplaceIfExists;
    HANDLE  RootDirectory;
    ULONG FileNameLength;
    WCHAR FileName[1];
} FILE_RENAME_INFORMATION;

typedef struct _FILE_FULL_DIR_INFORMATION
{
    ULONG NextEntryOffset;
    ULONG FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG FileAttributes;
    ULONG FileNameLength;
    ULONG EaSize;
    WCHAR FileName[1];
} FILE_FULL_DIR_INFORMATION;

typedef struct _FILE_DISPOSITION_INFORMATION
{
    BOOLEAN DeleteFile;
} FILE_DISPOSITION_INFORMATION;

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
} REPARSE_DATA_BUFFER;

typedef void
(NTAPI *PIO_APC_ROUTINE)(
    IN void *ApcContext,
    IN IO_STATUS_BLOCK *IoStatusBlock,
    IN ULONG Reserved
);

NTSYSCALLAPI
NTSTATUS
NTAPI
NtCreateFile(
    OUT HANDLE *FileHandle,
    IN ACCESS_MASK DesiredAccess,
    IN OBJECT_ATTRIBUTES *ObjectAttributes,
    OUT IO_STATUS_BLOCK *IoStatusBlock,
    IN OPTIONAL LARGE_INTEGER *AllocationSize,
    IN ULONG FileAttributes,
    IN ULONG ShareAccess,
    IN ULONG CreateDisposition,
    IN ULONG CreateOptions,
    IN OPTIONAL void *EaBuffer,
    IN ULONG EaLength
);

NTSYSCALLAPI
NTSTATUS
NTAPI
NtDeleteFile(
    IN OBJECT_ATTRIBUTES *ObjectAttributes
);

NTSYSCALLAPI
NTSTATUS
NTAPI
NtFsControlFile(
    IN HANDLE FileHandle,
    IN OPTIONAL HANDLE Event,
    IN OPTIONAL PIO_APC_ROUTINE ApcRoutine,
    IN OPTIONAL void *ApcContext,
    OUT IO_STATUS_BLOCK *IoStatusBlock,
    IN ULONG FsControlCode,
    IN OPTIONAL void *InputBuffer,
    IN ULONG InputBufferLength,
    OUT OPTIONAL void *OutputBuffer,
    IN ULONG OutputBufferLength
);

NTSYSCALLAPI
NTSTATUS
NTAPI
NtQueryAttributesFile(
    IN OBJECT_ATTRIBUTES *ObjectAttributes,
    OUT FILE_BASIC_INFORMATION *FileInformation
);

NTSYSCALLAPI
NTSTATUS
NTAPI
NtQueryInformationFile(
    IN HANDLE FileHandle,
    OUT IO_STATUS_BLOCK *IoStatusBlock,
    OUT void *FileInformation,
    IN ULONG Length,
    IN FILE_INFORMATION_CLASS FileInformationClass
);

NTSYSCALLAPI
NTSTATUS
NTAPI
NtSetInformationFile(
    IN HANDLE FileHandle,
    OUT IO_STATUS_BLOCK *IoStatusBlock,
    IN void *FileInformation,
    IN ULONG Length,
    IN FILE_INFORMATION_CLASS FileInformationClass
);

NTSYSCALLAPI
NTSTATUS
NTAPI
NtQueryDirectoryFile(
    IN HANDLE FileHandle,
    IN OPTIONAL HANDLE Event,
    IN OPTIONAL PIO_APC_ROUTINE ApcRoutine,
    IN OPTIONAL void *ApcContext,
    OUT IO_STATUS_BLOCK *IoStatusBlock,
    OUT void *FileInformation,
    IN ULONG Length,
    IN FILE_INFORMATION_CLASS FileInformationClass,
    IN BOOLEAN ReturnSingleEntry,
    IN OPTIONAL UNICODE_STRING *FileName,
    IN BOOLEAN RestartScan
);

NTSYSCALLAPI
NTSTATUS
NTAPI
NtReadFile(
    IN HANDLE FileHandle,
    IN OPTIONAL HANDLE Event,
    IN OPTIONAL PIO_APC_ROUTINE ApcRoutine,
    IN OPTIONAL void *ApcContext,
    OUT IO_STATUS_BLOCK *IoStatusBlock,
    OUT void *Buffer,
    IN ULONG Length,
    IN OPTIONAL LARGE_INTEGER *ByteOffset,
    IN OPTIONAL ULONG *Key
);

NTSYSCALLAPI
NTSTATUS
NTAPI
NtWriteFile(
    IN HANDLE FileHandle,
    IN OPTIONAL HANDLE Event,
    IN OPTIONAL PIO_APC_ROUTINE ApcRoutine,
    IN OPTIONAL void *ApcContext,
    OUT IO_STATUS_BLOCK *IoStatusBlock,
    IN const void *Buffer,
    IN ULONG Length,
    IN OPTIONAL LARGE_INTEGER *ByteOffset,
    IN OPTIONAL ULONG *Key
);

/////////////////// Ob ///////////////////

//
// Definitions for Object Creation
//
#define OBJ_INHERIT                             0x00000002L
#define OBJ_PERMANENT                           0x00000010L
#define OBJ_EXCLUSIVE                           0x00000020L
#define OBJ_CASE_INSENSITIVE                    0x00000040L
#define OBJ_OPENIF                              0x00000080L
#define OBJ_OPENLINK                            0x00000100L
#define OBJ_KERNEL_HANDLE                       0x00000200L
#define OBJ_FORCE_ACCESS_CHECK                  0x00000400L
#define OBJ_VALID_ATTRIBUTES                    0x000007F2L

#define InitializeObjectAttributes(p,n,a,r,s) { \
    (p)->Length = sizeof(OBJECT_ATTRIBUTES);    \
    (p)->RootDirectory = (r);                   \
    (p)->Attributes = (a);                      \
    (p)->ObjectName = (n);                      \
    (p)->SecurityDescriptor = (s);              \
    (p)->SecurityQualityOfService = NULL;       \
}

NTSYSCALLAPI
NTSTATUS
NTAPI
NtClose(
    IN HANDLE Handle
);

NTSYSCALLAPI
NTSTATUS
NTAPI
NtQuerySecurityObject(
    IN HANDLE Handle,
    IN SECURITY_INFORMATION SecurityInformation,
    OUT OPTIONAL SECURITY_DESCRIPTOR *SecurityDescriptor,
    IN ULONG Length,
    OUT ULONG *LengthNeeded
);

NTSYSCALLAPI
NTSTATUS
NTAPI
NtSetSecurityObject(
    IN HANDLE Handle,
    IN SECURITY_INFORMATION SecurityInformation,
    IN SECURITY_DESCRIPTOR *SecurityDescriptor
);

NTSYSCALLAPI
NTSTATUS
NTAPI
NtWaitForSingleObject(
    IN HANDLE Object,
    IN BOOLEAN Alertable,
    IN OPTIONAL LARGE_INTEGER *Time
);

/////////////////// Rtl ///////////////////

typedef struct _RTLP_CURDIR_REF
{
    LONG RefCount;
    HANDLE Handle;
} RTLP_CURDIR_REF;

typedef struct _RTL_RELATIVE_NAME_U
{
    UNICODE_STRING RelativeName;
    HANDLE ContainingDirectory;
    RTLP_CURDIR_REF *CurDirRef;
} RTL_RELATIVE_NAME_U;

typedef NTSTATUS
(NTAPI * PRTL_HEAP_COMMIT_ROUTINE)(
IN void *Base,
    IN OUT void **CommitAddress,
    IN OUT SIZE_T *CommitSize
);

typedef struct _RTL_HEAP_PARAMETERS
{
    ULONG Length;
    SIZE_T SegmentReserve;
    SIZE_T SegmentCommit;
    SIZE_T DeCommitFreeBlockThreshold;
    SIZE_T DeCommitTotalFreeThreshold;
    SIZE_T MaximumAllocationSize;
    SIZE_T VirtualMemoryThreshold;
    SIZE_T InitialCommit;
    SIZE_T InitialReserve;
    PRTL_HEAP_COMMIT_ROUTINE CommitRoutine;
    SIZE_T Reserved[2];
} RTL_HEAP_PARAMETERS;

typedef struct _TIME_FIELDS
{
    SHORT Year;
    SHORT Month;
    SHORT Day;
    SHORT Hour;
    SHORT Minute;
    SHORT Second;
    SHORT Milliseconds;
    SHORT Weekday;
} TIME_FIELDS;

NTSYSAPI
void *
NTAPI
RtlCreateHeap(
    IN ULONG Flags,
    IN OPTIONAL void *BaseAddress,
    IN OPTIONAL SIZE_T SizeToReserve,
    IN OPTIONAL SIZE_T SizeToCommit,
    IN OPTIONAL void *Lock,
    IN OPTIONAL RTL_HEAP_PARAMETERS *Parameters
);

NTSYSAPI
void *
NTAPI
RtlAllocateHeap(
    IN void *HeapHandle,
    IN OPTIONAL ULONG Flags,
    IN SIZE_T Size
);

NTSYSAPI
HANDLE
NTAPI
RtlDestroyHeap(
    IN HANDLE Heap
);

NTSYSAPI
BOOLEAN
NTAPI
RtlFreeHeap(
    IN HANDLE HeapHandle,
    IN OPTIONAL ULONG Flags,
    IN void *P
);

#define RtlGetProcessHeap() (NtCurrentPeb()->ProcessHeap)

NTSYSAPI
void
NTAPI
RtlInitUnicodeString(
    OUT UNICODE_STRING *DestinationString,
    IN OPTIONAL PCWSTR SourceString
);

NTSYSAPI
void
NTAPI
RtlFreeUnicodeString(
    IN OUT UNICODE_STRING *UnicodeString
);

NTSYSAPI
BOOLEAN
NTAPI
RtlDosPathNameToNtPathName_U(
    IN OPTIONAL PCWSTR DosPathName,
    OUT UNICODE_STRING *NtPathName,
    OUT OPTIONAL PCWSTR *NtFileNamePart,
    OUT OPTIONAL RTL_RELATIVE_NAME_U *DirectoryInfo
);

NTSYSAPI
NTSTATUS
NTAPI
RtlSystemTimeToLocalTime(
    IN LARGE_INTEGER *SystemTime,
    OUT LARGE_INTEGER *LocalTime
);

NTSYSAPI
void
NTAPI
RtlTimeToTimeFields(
    IN LARGE_INTEGER *Time,
    OUT TIME_FIELDS *TimeFields
);

/////////////////// Ex ///////////////////

typedef enum _EVENT_TYPE
{
    NotificationEvent,
    SynchronizationEvent
} EVENT_TYPE;

NTSYSCALLAPI
NTSTATUS
NTAPI
NtDelayExecution(
    IN BOOLEAN Alertable,
    IN LARGE_INTEGER *Interval
);

NTSYSCALLAPI
NTSTATUS
NTAPI
NtCreateEvent(
    OUT HANDLE *EventHandle,
    IN ACCESS_MASK DesiredAccess,
    IN OPTIONAL OBJECT_ATTRIBUTES *ObjectAttributes,
    IN EVENT_TYPE EventType,
    IN BOOLEAN InitialState
);

NTSYSCALLAPI
NTSTATUS
NTAPI
NtDisplayString(
    IN UNICODE_STRING *DisplayString
);

/////////////////// Cm ///////////////////

NTSYSCALLAPI
NTSTATUS
NTAPI
NtOpenKey(
    OUT HANDLE *KeyHandle,
    IN ACCESS_MASK DesiredAccess,
    IN OBJECT_ATTRIBUTES *ObjectAttributes
);

NTSYSCALLAPI
NTSTATUS
NTAPI
NtSetValueKey(
    IN HANDLE KeyHandle,
    IN UNICODE_STRING *ValueName,
    IN OPTIONAL ULONG TitleIndex,
    IN ULONG Type,
    IN void *Data,
    IN ULONG DataSize
);

/////////////////// Se ///////////////////

//
// Privilege constants
//
#define SE_MIN_WELL_KNOWN_PRIVILEGE       (2L)
#define SE_CREATE_TOKEN_PRIVILEGE         (2L)
#define SE_ASSIGNPRIMARYTOKEN_PRIVILEGE   (3L)
#define SE_LOCK_MEMORY_PRIVILEGE          (4L)
#define SE_INCREASE_QUOTA_PRIVILEGE       (5L)
#define SE_UNSOLICITED_INPUT_PRIVILEGE    (6L)
#define SE_MACHINE_ACCOUNT_PRIVILEGE      (6L)
#define SE_TCB_PRIVILEGE                  (7L)
#define SE_SECURITY_PRIVILEGE             (8L)
#define SE_TAKE_OWNERSHIP_PRIVILEGE       (9L)
#define SE_LOAD_DRIVER_PRIVILEGE          (10L)
#define SE_SYSTEM_PROFILE_PRIVILEGE       (11L)
#define SE_SYSTEMTIME_PRIVILEGE           (12L)
#define SE_PROF_SINGLE_PROCESS_PRIVILEGE  (13L)
#define SE_INC_BASE_PRIORITY_PRIVILEGE    (14L)
#define SE_CREATE_PAGEFILE_PRIVILEGE      (15L)
#define SE_CREATE_PERMANENT_PRIVILEGE     (16L)
#define SE_BACKUP_PRIVILEGE               (17L)
#define SE_RESTORE_PRIVILEGE              (18L)
#define SE_SHUTDOWN_PRIVILEGE             (19L)
#define SE_DEBUG_PRIVILEGE                (20L)
#define SE_AUDIT_PRIVILEGE                (21L)
#define SE_SYSTEM_ENVIRONMENT_PRIVILEGE   (22L)
#define SE_CHANGE_NOTIFY_PRIVILEGE        (23L)
#define SE_REMOTE_SHUTDOWN_PRIVILEGE      (24L)
#define SE_UNDOCK_PRIVILEGE               (25L)
#define SE_SYNC_AGENT_PRIVILEGE           (26L)
#define SE_ENABLE_DELEGATION_PRIVILEGE    (27L)
#define SE_MANAGE_VOLUME_PRIVILEGE        (28L)
#define SE_IMPERSONATE_PRIVILEGE          (29L)
#define SE_CREATE_GLOBAL_PRIVILEGE        (30L)
#define SE_MAX_WELL_KNOWN_PRIVILEGE       (SE_CREATE_GLOBAL_PRIVILEGE)

NTSYSCALLAPI
NTSTATUS
NTAPI
NtOpenProcessToken(
    IN HANDLE ProcessHandle,
    IN ACCESS_MASK DesiredAccess,
    OUT HANDLE *TokenHandle
);

NTSYSCALLAPI
NTSTATUS
NTAPI
NtAdjustPrivilegesToken(
    IN HANDLE TokenHandle,
    IN BOOLEAN DisableAllPrivileges,
    IN OPTIONAL TOKEN_PRIVILEGES *NewState,
    IN ULONG BufferLength,
    OUT OPTIONAL TOKEN_PRIVILEGES *PreviousState,
    OUT OPTIONAL ULONG *ReturnLength
);

NTSYSCALLAPI
NTSTATUS
NTAPI
NtTerminateProcess(
    IN HANDLE ProcessHandle,
    IN NTSTATUS ExitStatus
);

NTSYSCALLAPI
NTSTATUS
NTAPI
NtQuerySystemTime(
    OUT LARGE_INTEGER *CurrentTime
);
