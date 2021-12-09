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
#include <commctrl.h>

#include <stdio.h>
#include <io.h>
#include <fcntl.h>

#include <qubes-io.h>
#include <utf8-conv.h>
#include <log.h>

#define APP_MAP_KEY L"Software\\Invisible Things Lab\\Qubes Tools\\AppMap"
#define MAX_PATH_LONG 32768
#define INPUT_PREFIX "xdgicon:"

DWORD GetShortcutPath(OUT WCHAR *linkPath, IN DWORD linkPathLength)
{
    char param[64] = { 0 };
    DWORD status;
    HKEY key = NULL;
    WCHAR *valueName = NULL;
    DWORD valueType;
    DWORD size, i;

    // Input is in the form of: xdgicon:name
    // Name is a sha1 hash of the file in this case, we'll look it up in the registry.
    // It's set by GetAppMenus Qubes service.
    size = QioReadUntilEof(GetStdHandle(STD_INPUT_HANDLE), param, sizeof(param) - 1);
    if (size == 0)
    {
        status = win_perror("QioReadUntilEof(stdin)");
        goto cleanup;
    }

    LogDebug("input: '%S'", param);

    // Strip whitespaces at the end.
    for (i = RTL_NUMBER_OF(param) - 1; i > 0; i--)
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

    // convert ascii to wchar
    if (ERROR_SUCCESS != ConvertUTF8ToUTF16(param, &valueName, NULL))
    {
        status = win_perror("ConvertUTF8ToUTF16");
        goto cleanup;
    }

    LogDebug("input converted: '%s'", valueName);

    SetLastError(status = RegOpenKeyEx(HKEY_LOCAL_MACHINE, APP_MAP_KEY, 0, KEY_READ, &key));
    if (status != ERROR_SUCCESS)
    {
        status = win_perror("RegOpenKeyEx(AppMap key)");
        goto cleanup;
    }

    size = linkPathLength * sizeof(WCHAR); // buffer size
    SetLastError(status = RegQueryValueEx(key, valueName + strlen(INPUT_PREFIX), NULL, &valueType, (BYTE *) linkPath, &size));
    if (status != ERROR_SUCCESS)
    {
        status = win_perror("RegQueryValueEx");
        goto cleanup;
    }

    if (valueType != REG_SZ)
    {
        SetLastError(status = ERROR_DATATYPE_MISMATCH);
        win_perror("RegQueryValueEx");
        goto cleanup;
    }

cleanup:
    if (key)
        RegCloseKey(key);
    if (valueName)
        free(valueName);
    return status;
}

int wmain(int argc, WCHAR *argv[])
{
    WCHAR *linkPath = NULL;
    WORD pi = -1;
    ICONINFO ii;
    SHFILEINFO shfi = { 0 };
    HICON ico = NULL;
    HDC dc;
    BITMAP bm;
    DWORD size;
    BYTE *buffer;
    BITMAPINFO bmi;
    int x, y;
    HIMAGELIST imgList;
    DWORD status;
#ifdef WRITE_PPM
    FILE *f1;
#endif

    status = ERROR_NOT_ENOUGH_MEMORY;
    linkPath = malloc(MAX_PATH_LONG*sizeof(WCHAR));
    if (!linkPath)
        goto cleanup;

    // Set stdout to binary mode to prevent newline conversions.
    _setmode(_fileno(stdout), _O_BINARY);

    // Read input and convert it to the shortcut path.
    if (ERROR_SUCCESS != GetShortcutPath(linkPath, MAX_PATH_LONG))
    {
        status = win_perror("GetShortcutPath");
        goto cleanup;
    }

    LogDebug("LinkPath: %s", linkPath);

    CoInitialize(NULL);
    // We use SHGFI_SYSICONINDEX and load the icon manually later, because icons retrieved by
    // SHGFI_ICON always have the shortcut arrow overlay even if the overlay is not visible
    // normally (eg. for start menu shortcuts).
    // https://devblogs.microsoft.com/oldnewthing/?p=11653
    imgList = (HIMAGELIST)SHGetFileInfo(linkPath, 0, &shfi, sizeof(shfi), SHGFI_SYSICONINDEX);
    LogDebug("SHGetFileInfo(%s) returned 0x%x, shfi.iIcon=%d", linkPath, imgList, shfi.iIcon);
    if (!imgList)
    {
        status = win_perror("SHGetFileInfo(SHGFI_SYSICONINDEX)");
        goto cleanup;
    }

    // Retrieve the icon directly from the system image list
    ico = ImageList_GetIcon(imgList, shfi.iIcon, ILD_TRANSPARENT);
    if (!ico)
    {
        status = win_perror("ImageList_GetIcon(ILD_TRANSPARENT)");
        goto cleanup;
    }

    // Create bitmap for the icon.
    if (!GetIconInfo(ico, &ii))
    {
        status = win_perror("GetIconInfo");
        goto cleanup;
    }

    dc = CreateCompatibleDC(NULL);
    SelectObject(dc, ii.hbmColor);

    // Retrieve the color bitmap of the icon.
    GetObject(ii.hbmColor, sizeof(bm), &bm);

    size = bm.bmBitsPixel / 8 * bm.bmHeight * bm.bmWidth; // pixel buffer size
    buffer = (BYTE *) malloc(size); // pixel buffer
    ZeroMemory(&bmi, sizeof(BITMAPINFO));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = bm.bmWidth;
    bmi.bmiHeader.biHeight = bm.bmHeight;
    bmi.bmiHeader.biPlanes = bm.bmPlanes;
    bmi.bmiHeader.biBitCount = bm.bmBitsPixel;

    // Copy pixel buffer.
    GetDIBits(dc, ii.hbmColor, 0, bm.bmHeight, buffer, &bmi, DIB_RGB_COLORS);

    LogDebug("Size: %dx%d", bm.bmWidth, bm.bmHeight);
    printf("%d %d\n", bm.bmWidth, bm.bmHeight);

#ifdef WRITE_PPM
    // Create a PPM bitmap file.
    f1 = fopen("icon.ppm", "w");
    fprintf(f1, "P3\n");
    fprintf(f1, "%d %d\n", bm.bmWidth, bm.bmHeight);
    fprintf(f1, "255\n");
#endif

    // Output bitmap.
    for (y = 0; y < bm.bmHeight; y++)
    {
        for (x = 0; x < bm.bmWidth; x++)
        {
            DWORD offset = (bm.bmHeight - y - 1) * 4 * bm.bmWidth + x * 4;
            BYTE b = *(buffer + offset + 0);
            BYTE g = *(buffer + offset + 1);
            BYTE r = *(buffer + offset + 2);
            BYTE a = *(buffer + offset + 3);

            fwrite(buffer + offset, 4, 1, stdout);
#ifdef WRITE_PPM
            fprintf(f1, "%d %d %d ", r, g, b);
#endif
        }
#ifdef WRITE_PPM
        fprintf(f1, "\n");
#endif
    }

    printf("\n");

#ifdef WRITE_PPM
    fclose(f1);
#endif

    status = ERROR_SUCCESS;

cleanup:
    if (status != ERROR_SUCCESS && linkPath)
        LogError("LinkPath: %s", linkPath);
    // Everything will be cleaned up upon process exit.
    LogDebug("returning %lu", status);
    return status;
}
