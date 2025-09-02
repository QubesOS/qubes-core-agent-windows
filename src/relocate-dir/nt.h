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

// This include is meant for kernel file system drivers, but we can use it for user mode defines as well.
#ifndef _AMD64_
#define _AMD64_
#endif
#include <ntifs.h>

#pragma intrinsic(memcpy, memset)

// Rest of this file contains declarations/definitions that are either not documented or missing from kernel mode includes.

typedef UCHAR BYTE;

NTSTATUS
NTAPI
NtQueryAttributesFile(
    IN  POBJECT_ATTRIBUTES ObjectAttributes,
    OUT FILE_BASIC_INFORMATION *FileInformation
    );

#define HEAP_ZERO_MEMORY                0x00000008

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

BOOLEAN
NTAPI
RtlDosPathNameToNtPathName_U(
    IN  PCWSTR DosPathName OPTIONAL,
    OUT UNICODE_STRING *NtPathName,
    OUT PCWSTR *NtFileNamePart OPTIONAL,
    OUT RTL_RELATIVE_NAME_U *DirectoryInfo OPTIONAL
    );

NTSTATUS
NTAPI
RtlSystemTimeToLocalTime(
    IN  PLARGE_INTEGER SystemTime,
    OUT LARGE_INTEGER *LocalTime
    );

NTSTATUS
NTAPI
NtDelayExecution(
    IN  BOOLEAN Alertable,
    IN  PLARGE_INTEGER Interval
    );

NTSTATUS
NTAPI
NtDisplayString(
    IN  PUNICODE_STRING DisplayString
    );

NTSTATUS
NTAPI
NtOpenKey(
    OUT HANDLE *KeyHandle,
    IN  ACCESS_MASK DesiredAccess,
    IN  POBJECT_ATTRIBUTES ObjectAttributes
    );

NTSTATUS
NTAPI
NtSetValueKey(
    IN  HANDLE KeyHandle,
    IN  PUNICODE_STRING ValueName,
    IN  ULONG TitleIndex OPTIONAL,
    IN  ULONG Type,
    IN  PVOID Data,
    IN  ULONG DataSize
    );

NTSTATUS
NTAPI
NtTerminateProcess(
    IN  HANDLE ProcessHandle,
    IN  NTSTATUS ExitStatus
    );

NTSTATUS
NTAPI
NtQuerySystemTime(
    OUT LARGE_INTEGER *CurrentTime
    );

// process startup parameters

typedef struct _PEB_LDR_DATA
{
    BYTE       Reserved1[8];
    PVOID      Reserved2[3];
    LIST_ENTRY InMemoryOrderModuleList;
} PEB_LDR_DATA, *PPEB_LDR_DATA;

typedef struct _RTL_USER_PROCESS_PARAMETERS
{
    BYTE           Reserved1[16];
    PVOID          Reserved2[10];
    UNICODE_STRING ImagePathName;
    UNICODE_STRING CommandLine;
} RTL_USER_PROCESS_PARAMETERS, *PRTL_USER_PROCESS_PARAMETERS;

struct _PEB32
{
    BYTE                          Reserved1[2];
    BYTE                          BeingDebugged;
    BYTE                          Reserved2[1];
    PVOID                         Reserved3[2];
    PPEB_LDR_DATA                 LoaderData;
    PRTL_USER_PROCESS_PARAMETERS  ProcessParameters;
};

PRTL_USER_PROCESS_PARAMETERS
NTAPI
RtlNormalizeProcessParams(
    IN PRTL_USER_PROCESS_PARAMETERS Params
    );

struct _PEB64
{
    BYTE Reserved1[2];
    BYTE BeingDebugged;
    BYTE Reserved2[21];
    PPEB_LDR_DATA LoaderData;
    PRTL_USER_PROCESS_PARAMETERS ProcessParameters;
};

#ifdef AMD64
typedef struct _PEB64 PEB, *PPEB2;
#else
typedef struct _PEB32 PEB, *PPEB2;
#endif
