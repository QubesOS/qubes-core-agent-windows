/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (c) 2025 Rafa³ Wojdy³a <omeg@invisiblethingslab.com>
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
 */

#include <assert.h>
#include <stdio.h>

// needed to avoid NTSTATUS related redefinitions
#define WIN32_NO_STATUS
#include <Windows.h>
#undef WIN32_NO_STATUS

#include <LMaccess.h>
#include <ntstatus.h>
#include <winternl.h>
#include <WtsApi32.h>

// this needs to be last, otherwise we get some more redefinitions...
#define _NTDEF_
#include <NTSecAPI.h>

#include "log.h"

// Set current user's password to a random one and enable automatic logon.
// The password is stored as a LSA secret (not plaintext in the registry).
// NOTE: Changing the password invalidates all credentials stored by the system on behalf of the user.
//       All user files with NTFS encryption will become inaccessible!
// TODO: support Active Directory

// generate random password
// length is in characters, including NULL terminator
static DWORD GenerateRandomPassword(_Out_writes_z_(length) wchar_t* password, _In_ size_t length)
{
    if (length < 1)
        return ERROR_INVALID_PARAMETER;

    const wchar_t* chars =
        L"abcdefghijklmnopqrstuvwxyz"
        L"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        L"0123456789"
        L"`~!@#$%^&*()-_=+[]{};':\",.<>/?\\|";
    size_t chars_count = wcslen(chars);
    assert(chars_count < 255);

    for (size_t i = 0; i < length - 1; i++)
    {
        BYTE idx;
        NTSTATUS status = BCryptGenRandom(
            NULL, // algorithm provider
            &idx, // buffer
            sizeof(idx), // buffer size
            BCRYPT_USE_SYSTEM_PREFERRED_RNG); // flags

        if (!NT_SUCCESS(status))
            return win_perror2(RtlNtStatusToDosError(status), "BCryptGenRandom");

        // this doesn't generate a perfect uniform distribution, but is good enough for our purpose
        password[i] = chars[idx % chars_count];
    }
    password[length - 1] = L'\0';

    return ERROR_SUCCESS;
}

// set password for a user account
DWORD SetUserPassword(_In_z_ const wchar_t* username, _In_z_ wchar_t* password)
{
    USER_INFO_1003 pwd_info;
    pwd_info.usri1003_password = password;

    LogInfo("Setting password for user '%s'", username);
    DWORD status = NetUserSetInfo(
        NULL, // server name
        username, // user name
        1003, // info id (password)
        (BYTE*)&pwd_info, // data
        NULL); // parameter error index

    if (status != ERROR_SUCCESS)
        return win_perror2(status, "NetUserSetInfo(password)");
    return ERROR_SUCCESS;
}

// enable autologon and set DefaultUserName
DWORD SetAutologonRegistry(_In_z_ const wchar_t* username)
{
    HKEY key;
    DWORD status = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE, // parent
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", // path
        0, // options
        KEY_READ | KEY_WRITE, // access
        &key);

    if (status != ERROR_SUCCESS)
        return win_perror2(status, "RegOpenKeyExW(Winlogon)");

    status = RegSetValueExW(
        key, // parent
        L"AutoAdminLogon", // name
        0, // reserved
        REG_SZ, // type
        (BYTE*)L"1", // value
        4); // size

    if (status != ERROR_SUCCESS)
        return win_perror2(status, "RegSetValueEx(AutoAdminLogon)");

    status = RegSetValueExW(
        key, // parent
        L"DefaultUserName", // name
        0, // reserved
        REG_SZ, // type
        (BYTE*)username, // value
        2 * (DWORD)(wcslen(username) + 1)); // size

    if (status != ERROR_SUCCESS)
        return win_perror2(status, "RegSetValueEx(DefaultUserName)");

    return ERROR_SUCCESS;
}

// set DefaultPassword for autologon as a LSA secret
DWORD SetAutologonPassword(_In_z_ wchar_t* password)
{
    LSA_OBJECT_ATTRIBUTES attrs;
    ZeroMemory(&attrs, sizeof(attrs));

    LSA_HANDLE lsa;
    NTSTATUS status = LsaOpenPolicy(
        NULL, // server
        &attrs, // attributes
        POLICY_CREATE_SECRET, // access
        &lsa); // output

    if (!NT_SUCCESS(status))
        return win_perror2(LsaNtStatusToWinError(status), "LsaOpenPolicy");

    LSA_UNICODE_STRING secret_name;
    RtlInitUnicodeString(&secret_name, L"DefaultPassword");

    LSA_UNICODE_STRING secret_data;
    RtlInitUnicodeString(&secret_data, password);

    status = LsaStorePrivateData(lsa, &secret_name, &secret_data);
    status = LsaNtStatusToWinError(status);

    if (status != ERROR_SUCCESS)
        return win_perror2(status, "LsaStorePrivateData");

    return ERROR_SUCCESS;
}

int main(void)
{
    wchar_t* username;
    DWORD un_size;
    if (!WTSQuerySessionInformationW(
        WTS_CURRENT_SERVER_HANDLE, // server
        WTS_CURRENT_SESSION, // session id
        WTSUserName, // info type
        &username, // buffer ptr
        &un_size)) // returned size

        return win_perror("WTSQuerySessionInformationW");

    LogDebug("active user name: %s", username);

    wchar_t password[LM20_PWLEN];
    DWORD status = GenerateRandomPassword(password, ARRAYSIZE(password));
    if (status != ERROR_SUCCESS)
        return status;

    status = SetUserPassword(username, password);
    if (status != ERROR_SUCCESS)
        return status;

    status = SetAutologonRegistry(username);
    if (status != ERROR_SUCCESS)
        return status;

    status = SetAutologonPassword(password);
    if (status != ERROR_SUCCESS)
        return status;

    SecureZeroMemory(password, sizeof(password));

    WTSFreeMemory(username);
    return status;
}
