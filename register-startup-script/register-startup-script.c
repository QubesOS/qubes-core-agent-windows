// Adds specified command as a local group policy startup script.

#include <Windows.h>
#include <ShlObj.h>
#include <Shlwapi.h>
#include <initguid.h>
#include <gpedit.h>
#include <strsafe.h>

#include "log.h"

#define LOG_NAME L"register-startup-script"

// TODO: does order matter?
WCHAR scriptGuids[] = L"{42B5FAAE-6536-11D2-AE5A-0000F87571E3}{40B6664F-4972-11D1-A7CA-0000F87571E3}";
WCHAR scriptGuidsFull[] = L"[{42B5FAAE-6536-11D2-AE5A-0000F87571E3}{40B6664F-4972-11D1-A7CA-0000F87571E3}]";

/*
 * Set GP startup script processing to synchronous.
 * This can be switched on with a single registry value,
 * but we use group policy COM interface to do that.
 * That's because on a freshly-installed system simply
 * setting the registry value doesn't work, probably
 * some GP stuff isn't yet initialized.
 */
DWORD InitGPO(void)
{
    DWORD val;
    DWORD status;
    IGroupPolicyObject *gpo = NULL;
    HKEY machineKey = NULL, valKey = NULL;
    DWORD retval = ERROR_INVALID_FUNCTION;

    GUID extGuid = REGISTRY_EXTENSION_GUID;
    // Unique guid for our value.
    // {A29AD8B4-9D44-42BB-9319-B4DE42C0A3BE}
    GUID snapGuid = { 0xa29ad8b4, 0x9d44, 0x42bb, { 0x93, 0x19, 0xb4, 0xde, 0x42, 0xc0, 0xa3, 0xbe } };

    status = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (status != S_OK)
    {
        errorf("CoInitializeEx failed: 0x%08x", status);
        goto cleanup;
    }

    // Create the GPO instance.
    status = CoCreateInstance(&CLSID_GroupPolicyObject, NULL, CLSCTX_INPROC_SERVER, &IID_IGroupPolicyObject, &gpo);
    if (status != S_OK)
    {
        errorf("CoCreateInstance(CLSID_GroupPolicyObject) failed: 0x%08x", status);
        goto cleanup;
    }

    status = gpo->lpVtbl->OpenLocalMachineGPO(gpo, GPO_OPEN_LOAD_REGISTRY);
    if (status != S_OK)
    {
        errorf("OpenLocalMachineGPO failed: 0x%08x", status);
        goto cleanup;
    }

    status = gpo->lpVtbl->GetRegistryKey(gpo, GPO_SECTION_MACHINE, &machineKey);
    if (status != S_OK)
    {
        errorf("GetRegistryKey failed: 0x%08x", status);
        goto cleanup;
    }

    // Create/open the key and set the value.
    SetLastError(status = RegCreateKeyEx(machineKey, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
        0, NULL, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, NULL, &valKey, NULL));
    if (status != ERROR_SUCCESS)
    {
        perror("RegCreateKeyEx");
        goto cleanup;
    }

    val = 1;
    SetLastError(status = RegSetKeyValue(valKey, NULL, L"RunStartupScriptSync", REG_DWORD, &val, sizeof(val)));
    if (status != ERROR_SUCCESS)
    {
        perror("RegSetKeyValue");
        goto cleanup;
    }

    // Apply policy and free resources.
    status = gpo->lpVtbl->Save(gpo, TRUE, TRUE, &extGuid, &snapGuid);
    if (status != S_OK)
    {
        errorf("Save failed: 0x%08x", status);
        goto cleanup;
    }

    status = ERROR_SUCCESS;

cleanup:
    if (gpo)
        gpo->lpVtbl->Release(gpo);
    if (valKey)
        RegCloseKey(valKey);
    if (machineKey)
        RegCloseKey(machineKey);
    return status;
}

int wmain(int argc, WCHAR* argv[])
{
    PWCHAR systemPath;
    WCHAR gptPath[MAX_PATH];
    WCHAR scriptsPath[MAX_PATH];
    WCHAR buf[4096] = { 0 };
    ULONG gptVersion = 0;
    PWCHAR policyBuf;
    DWORD policySize;
    PWCHAR str;
    DWORD status;

    log_init_default(LOG_NAME);

    if (S_OK != SHGetKnownFolderPath(&FOLDERID_System, 0, NULL, &systemPath))
    {
        perror("SHGetKnownFolderPath(FOLDERID_System)");
        return 1;
    }

    if (argc < 3)
    {
        errorf("usage: %s <startup script path> <script arguments>", argv[0]);
        return 2;
    }

    if (S_OK != StringCchPrintf(gptPath, RTL_NUMBER_OF(gptPath), L"%s\\GroupPolicy\\gpt.ini", systemPath))
    {
        perror("StringCchPrintf(gptPath)");
        return 3;
    }

    // Ensure that the path exists, it may not on a freshly installed system.
    if (S_OK != StringCchPrintf(scriptsPath, RTL_NUMBER_OF(scriptsPath), L"%s\\GroupPolicy\\Machine\\Scripts", systemPath))
    {
        perror("StringCchPrintf(scriptsPath)");
        return 3;
    }

    if (!PathIsDirectory(scriptsPath))
    {
        status = SHCreateDirectoryEx(NULL, scriptsPath, NULL);
        if (ERROR_SUCCESS != status)
        {
            SetLastError(status);
            perror("SHCreateDirectoryEx");
            return 4;
        }
    }

    if (S_OK != StringCchPrintf(scriptsPath, RTL_NUMBER_OF(scriptsPath), L"%s\\GroupPolicy\\Machine\\Scripts\\scripts.ini", systemPath))
    {
        perror("StringCchPrintf(scriptsPath)");
        return 3;
    }

    logf("gpt.ini: %s", gptPath);
    logf("scripts.ini: %s", scriptsPath);

    // Write startup script information.
    // TODO: check if there are any existing entries
    if (!WritePrivateProfileString(L"Startup", L"0CmdLine", argv[1], scriptsPath))
    {
        perror("WritePrivateProfileString(0CmdLine)");
        return 5;
    }
    if (!WritePrivateProfileString(L"Startup", L"0Parameters", argv[2], scriptsPath))
    {
        perror("WritePrivateProfileString(0Parameters)");
        return 5;
    }

    // Get active policies.
    GetPrivateProfileString(L"General", L"gPCMachineExtensionNames", NULL, buf, RTL_NUMBER_OF(buf), gptPath);
    logf("policies: '%s'", buf);
    // Append script guids if not there.
    if (0 == wcsstr(buf, scriptGuids))
    {
        // The value format is [{guid1}{guid2}...{guidN}]
        str = wcsrchr(buf, L']');
        if (NULL == str) // no [], create the whole thing
        {
            if (!WritePrivateProfileString(L"General", L"gPCMachineExtensionNames", scriptGuidsFull, gptPath))
            {
                perror("WritePrivateProfileString(gPCMachineExtensionNames)");
                return 5;
            }
        }
        else
        {
            // Append script guids.
            policySize = (DWORD)(sizeof(WCHAR)* (wcslen(buf) + wcslen(scriptGuids) + 1));
            policyBuf = malloc(policySize);
            if (!policyBuf)
            {
                errorf("No memory");
                return 6;
            }
            ZeroMemory(policyBuf, policySize);
            StringCbCopy(policyBuf, policySize, buf);
            str = wcsrchr(policyBuf, L']'); // rescan in the new buffer
            memcpy(str, scriptGuids, sizeof(scriptGuids)); // add script guids
            policyBuf[wcslen(policyBuf)] = L']'; // add trailing bracket
            logf("gPCMachineExtensionNames=%s", policyBuf);
            if (!WritePrivateProfileString(L"General", L"gPCMachineExtensionNames", policyBuf, gptPath))
            {
                perror("WritePrivateProfileString(gPCMachineExtensionNames)");
                return 5;
            }
        }
    }

    // Increment the version to force policy refresh.
    gptVersion = GetPrivateProfileInt(L"General", L"Version", 0, gptPath);
    // if the key is not present this will be 0
    gptVersion++;
    StringCchPrintf(buf, RTL_NUMBER_OF(buf), L"%lu", gptVersion);
    logf("Version: %lu", gptVersion);
    if (!WritePrivateProfileString(L"General", L"Version", buf, gptPath))
    {
        perror("WritePrivateProfileString(Version)");
        return 5;
    }

    return InitGPO();
}
