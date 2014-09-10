#pragma once
#include "nt.h"
#include <stdio.h>
#include <stdarg.h>

// Number of 100ns ticks in 1 second.
#define NANOTICKS 10000LL

extern HANDLE g_Heap;

// Maximum path length for NTFS.
#define MAX_PATH_LONG 32768

void NtLog(IN BOOLEAN print, IN const PWCHAR format, ...);
NTSTATUS FileOpen(OUT PHANDLE file, IN const PWCHAR fileName, IN BOOLEAN write, IN BOOLEAN overwrite, IN BOOLEAN isReparse);
NTSTATUS FileGetAttributes(IN const PWCHAR fileName, OUT PULONG attrs);
NTSTATUS FileSetAttributes(const IN PWCHAR fileName, IN ULONG attrs);
NTSTATUS FileGetSize(IN HANDLE file, OUT PLONGLONG fileSize);
NTSTATUS FileGetPosition(IN HANDLE file, OUT PLONGLONG position);
NTSTATUS FileSetPosition(IN HANDLE file, IN LONGLONG position);
NTSTATUS FileRead(IN HANDLE file, OUT PVOID buffer, IN ULONG bufferSize, OUT PULONG readSize);
NTSTATUS FileWrite(IN HANDLE file, IN const PVOID buffer, IN ULONG bufferSize, OUT PULONG writtenSize);
NTSTATUS FileRename(IN const PWCHAR existingFileName, IN const PWCHAR newFileName, IN BOOLEAN replaceIfExists);
NTSTATUS FileCopy(IN const PWCHAR sourceName, IN const PWCHAR targetName);
NTSTATUS FileCopySecurity(IN HANDLE source, IN HANDLE target);
NTSTATUS FileDelete(IN HANDLE file);
NTSTATUS FileCreateDirectory(const IN PWCHAR dirName);
NTSTATUS FileCopyReparsePoint(IN const PWCHAR sourcePath, IN const PWCHAR targetPath);
NTSTATUS FileSetSymlink(IN const PWCHAR sourcePath, IN const PWCHAR targetPath);
NTSTATUS FileCopyDirectory(IN const PWCHAR sourcePath, IN const PWCHAR targetPath, IN BOOLEAN ignoreErrors);
NTSTATUS FileDeleteDirectory(IN const PWCHAR path, IN BOOLEAN deleteSelf);
