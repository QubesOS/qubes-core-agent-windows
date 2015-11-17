/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (c) Invisible Things Lab
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#pragma once

#include "nt.h"
#include <stdarg.h>

// Number of 100ns ticks in 1 second.
#define NANOTICKS 10000LL

extern HANDLE g_Heap;

// Maximum path length for NTFS.
#define MAX_PATH_LONG 32768

void NtLog(IN BOOLEAN print, IN const PWCHAR format, ...);
NTSTATUS FileOpen(OUT HANDLE *file, IN const PWCHAR fileName, IN BOOLEAN write, IN BOOLEAN overwrite, IN BOOLEAN isReparse);
NTSTATUS FileGetAttributes(IN const PWCHAR fileName, OUT ULONG *attrs);
NTSTATUS FileSetAttributes(IN const PWCHAR fileName, IN ULONG attrs);
NTSTATUS FileGetSize(IN HANDLE file, OUT INT64 *fileSize);
NTSTATUS FileGetPosition(IN HANDLE file, OUT INT64 *position);
NTSTATUS FileSetPosition(IN HANDLE file, IN INT64 position);
NTSTATUS FileRead(IN HANDLE file, OUT void *buffer, IN ULONG bufferSize, OUT ULONG *readSize);
NTSTATUS FileWrite(IN HANDLE file, IN const PVOID buffer, IN ULONG bufferSize, OUT ULONG *writtenSize);
NTSTATUS FileRename(IN const PWCHAR existingFileName, IN const PWCHAR newFileName, IN BOOLEAN replaceIfExists);
NTSTATUS FileCopy(IN const PWCHAR sourceName, IN const PWCHAR targetName);
NTSTATUS FileCopySecurity(IN HANDLE source, IN HANDLE target);
NTSTATUS FileDelete(IN HANDLE file);
NTSTATUS FileCreateDirectory(const IN PWCHAR path);
NTSTATUS FileCopyReparsePoint(IN const PWCHAR sourcePath, IN const PWCHAR targetPath);
NTSTATUS FileSetSymlink(IN const PWCHAR sourcePath, IN const PWCHAR targetPath);
NTSTATUS FileCopyDirectory(IN const PWCHAR sourcePath, IN const PWCHAR targetPath, IN BOOLEAN ignoreErrors);
NTSTATUS FileDeleteDirectory(IN const PWCHAR path, IN BOOLEAN deleteSelf);
