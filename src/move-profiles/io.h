#pragma once
#define _CRT_SECURE_NO_WARNINGS
#include "nt.h"
#include <stdio.h>
#include <stdarg.h>

// Number of 100ns ticks in 1 second.
#define NANOTICKS 10000LL

extern HANDLE g_Heap;

// Maximum path length for NTFS.
#define MAX_PATH_LONG 32768

void NtLog(IN BOOLEAN print, IN const WCHAR *format, ...);
NTSTATUS FileOpen(OUT PHANDLE file, IN const WCHAR *fileName, IN BOOLEAN write, IN BOOLEAN overwrite, IN BOOLEAN isReparse);
NTSTATUS FileGetAttributes(IN const WCHAR *fileName, OUT ULONG *attrs);
NTSTATUS FileSetAttributes(const IN WCHAR *fileName, IN ULONG attrs);
NTSTATUS FileGetSize(IN HANDLE file, OUT INT64 *fileSize);
NTSTATUS FileGetPosition(IN HANDLE file, OUT INT64 *position);
NTSTATUS FileSetPosition(IN HANDLE file, IN INT64 position);
NTSTATUS FileRead(IN HANDLE file, OUT void *buffer, IN ULONG bufferSize, OUT PULONG readSize);
NTSTATUS FileWrite(IN HANDLE file, IN const void *buffer, IN ULONG bufferSize, OUT ULONG *writtenSize);
NTSTATUS FileRename(IN const WCHAR *existingFileName, IN const WCHAR *newFileName, IN BOOLEAN replaceIfExists);
NTSTATUS FileCopy(IN const WCHAR *sourceName, IN const WCHAR *targetName);
NTSTATUS FileCopySecurity(IN HANDLE source, IN HANDLE target);
NTSTATUS FileDelete(IN HANDLE file);
NTSTATUS FileCreateDirectory(const IN WCHAR *dirName);
NTSTATUS FileCopyReparsePoint(IN const WCHAR *sourcePath, IN const WCHAR *targetPath);
NTSTATUS FileSetSymlink(IN const WCHAR *sourcePath, IN const WCHAR *targetPath);
NTSTATUS FileCopyDirectory(IN const WCHAR *sourcePath, IN const WCHAR *targetPath, IN BOOLEAN ignoreErrors);
NTSTATUS FileDeleteDirectory(IN const WCHAR *path, IN BOOLEAN deleteSelf);
