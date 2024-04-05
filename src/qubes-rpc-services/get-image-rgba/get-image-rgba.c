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

#include <windows.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <strsafe.h>
#include <commctrl.h>

#include <stdio.h>
#include <io.h>
#include <fcntl.h>

#include <qubes-io.h>
#include <utf8-conv.h>
#include <log.h>

//#define WRITE_PPM

// FIXME hardcoded reg path, use config library
#define APP_MAP_KEY L"Software\\Invisible Things Lab\\Qubes Tools\\AppMap"
#define INPUT_PREFIX "xdgicon:"

// returns appmap hash
WCHAR* GetShortcutPath(OUT WCHAR *linkPath, IN DWORD linkPathLength)
{
    DWORD status = ERROR_SUCCESS;
    HKEY key = NULL;
    WCHAR* valueName = NULL;

    // Input is in the form of: xdgicon:name
    // Name is a sha1 hash of the file in this case, we'll look it up in the registry.
    // It's set by GetAppMenus Qubes service.
    char param[64] = { 0 };
    DWORD size = QioReadUntilEof(GetStdHandle(STD_INPUT_HANDLE), param, sizeof(param) - 1);
    if (size == 0)
    {
        status = win_perror("QioReadUntilEof(stdin)");
        goto cleanup;
    }

    LogDebug("input: '%S'", param);

    // Strip whitespaces at the end.
    for (size_t i = ARRAYSIZE(param) - 1; i > 0; i--)
    {
        if (param[i] == 0)
            continue;

        if (isspace(param[i]))
        {
            param[i] = 0;
        }
        else
        {
            break; // stop on first non-space character
        }
    }

    status = ConvertUTF8ToUTF16Static(param, &valueName, NULL);
    if (status != ERROR_SUCCESS)
    {
        win_perror2(status, "ConvertUTF8ToUTF16Static");
        goto cleanup;
    }

    LogDebug("input converted: '%s'", valueName);

    status = RegOpenKeyEx(HKEY_CURRENT_USER, APP_MAP_KEY, 0, KEY_READ, &key);
    if (status != ERROR_SUCCESS)
    {
        win_perror2(status, "RegOpenKeyEx(AppMap key)");
        goto cleanup;
    }

    size = linkPathLength * sizeof(WCHAR); // buffer size
    DWORD valueType;
    status = RegQueryValueEx(key, valueName + strlen(INPUT_PREFIX), NULL, &valueType, (BYTE *) linkPath, &size);
    if (status != ERROR_SUCCESS)
    {
        win_perror2(status, "RegQueryValueEx");
        goto cleanup;
    }

    if (valueType != REG_SZ)
    {
        status = ERROR_DATATYPE_MISMATCH;
        LogError("AppMap(%s) registry value has incorrect format (0x%x)", valueName + strlen(INPUT_PREFIX), valueType);
        goto cleanup;
    }

    status = ERROR_SUCCESS;

cleanup:
    if (key)
        RegCloseKey(key);
    SetLastError(status);
    if (status == ERROR_SUCCESS)
        valueName += strlen(INPUT_PREFIX);
    else
        valueName = NULL;
    return valueName; // this value is a static buffer in ConvertUTF* library functions
}

int wmain(int argc, WCHAR *argv[])
{
    DWORD status = ERROR_OUTOFMEMORY;
    WCHAR* linkPath = malloc(MAX_PATH_LONG_WSIZE);
    if (!linkPath)
        goto cleanup;

    // Set stdout to binary mode to prevent newline conversions.
    (void)_setmode(_fileno(stdout), _O_BINARY);

    // Read input and convert it to the shortcut path.
    WCHAR* hash = GetShortcutPath(linkPath, MAX_PATH_LONG);
    if (!hash)
    {
        win_perror("GetShortcutPath");
        goto cleanup;
    }

    LogDebug("LinkPath: %s", linkPath);

    (void)CoInitialize(NULL);
    // We use SHGFI_SYSICONINDEX and load the icon manually later, because icons retrieved by
    // SHGFI_ICON always have the shortcut arrow overlay even if the overlay is not visible
    // normally (eg. for start menu shortcuts).
    // https://devblogs.microsoft.com/oldnewthing/?p=11653
    SHFILEINFO shfi = { 0 };
    HIMAGELIST imgList = (HIMAGELIST)SHGetFileInfo(linkPath, 0, &shfi, sizeof(shfi), SHGFI_SYSICONINDEX);
    LogDebug("shfi.iIcon=%d", shfi.iIcon);
    if (!imgList)
    {
        status = win_perror("SHGetFileInfo(SHGFI_SYSICONINDEX)");
        goto cleanup;
    }

    // Retrieve the icon directly from the system image list
    HICON ico = ImageList_GetIcon(imgList, shfi.iIcon, ILD_TRANSPARENT);
    if (!ico)
    {
        status = win_perror("ImageList_GetIcon(ILD_TRANSPARENT)");
        goto cleanup;
    }

    // Create bitmap for the icon.
    ICONINFO ii;
    if (!GetIconInfo(ico, &ii))
    {
        status = win_perror("GetIconInfo");
        goto cleanup;
    }

    HDC dc = CreateCompatibleDC(NULL);
    SelectObject(dc, ii.hbmColor);

    // Retrieve the color bitmap of the icon.
    BITMAP bm;
    GetObject(ii.hbmColor, sizeof(bm), &bm);

    size_t size = bm.bmBitsPixel / 8 * bm.bmHeight * bm.bmWidth; // pixel buffer size
    BYTE* buffer = (BYTE *) malloc(size); // pixel buffer
    if (!buffer)
        exit(ERROR_OUTOFMEMORY);

    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = bm.bmWidth;
    bmi.bmiHeader.biHeight = bm.bmHeight;
    bmi.bmiHeader.biPlanes = bm.bmPlanes;
    bmi.bmiHeader.biBitCount = bm.bmBitsPixel;

    // Copy pixel buffer.
    GetDIBits(dc, ii.hbmColor, 0, bm.bmHeight, buffer, &bmi, DIB_RGB_COLORS);

    LogDebug("Size: %dx%d, %d bpp", bm.bmWidth, bm.bmHeight, bm.bmBitsPixel);
    printf("%d %d\n", bm.bmWidth, bm.bmHeight);

#ifdef WRITE_PPM
    // Create a PPM bitmap file.
    char path[256];
    StringCchPrintfA(path, ARRAYSIZE(path), "icon-%S.ppm", hash);
    FILE* ppm = NULL;
    if (fopen_s(&ppm, path, "w") != 0)
    {
        LogWarning("Failed to open PPM file %S", path);
    }

    if (ppm)
    {
        fprintf(ppm, "P3\n");
        fprintf(ppm, "%d %d\n", bm.bmWidth, bm.bmHeight);
        fprintf(ppm, "255\n");
    }
#endif

    // Output bitmap.
    for (int y = 0; y < bm.bmHeight; y++)
    {
        for (int x = 0; x < bm.bmWidth; x++)
        {
            DWORD offset = (bm.bmHeight - y - 1) * 4 * bm.bmWidth + x * 4;
            BYTE b = *(buffer + offset + 0);
            BYTE g = *(buffer + offset + 1);
            BYTE r = *(buffer + offset + 2);
            BYTE a = *(buffer + offset + 3);

            fwrite(buffer + offset, 4, 1, stdout);
#ifdef WRITE_PPM
            if (ppm)
                fprintf(ppm, "%d %d %d ", r, g, b);
#endif
        }
#ifdef WRITE_PPM
        if (ppm)
            fprintf(ppm, "\n");
#endif
    }

#ifdef WRITE_PPM
    if (ppm)
        fclose(ppm);
#endif

    // apparently stdout is not flushed automatically on process exit if in binary mode...
    fflush(stdout);
    status = ERROR_SUCCESS;

cleanup:
    if (status == ERROR_SUCCESS && linkPath)
        LogInfo("LinkPath: %s", linkPath);
    // Everything will be cleaned up upon process exit.
    LogDebug("returning %lu", status);
    return status;
}
