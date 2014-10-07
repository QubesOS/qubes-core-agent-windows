#include <windows.h>
#include <Shlwapi.h>
#include <shellapi.h>

#include <stdio.h>
#include <io.h>
#include <fcntl.h>

#include "utf8-conv.h"
#include "log.h"

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
    if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), param, RTL_NUMBER_OF(param) - 1, &size, NULL))
    {
        status = perror("ReadFile(stdin)");
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
        status = perror("ConvertUTF8ToUTF16");
        goto cleanup;
    }

    LogDebug("input converted: '%s'", valueName);

    SetLastError(status = RegOpenKeyEx(HKEY_LOCAL_MACHINE, APP_MAP_KEY, 0, KEY_READ, &key));
    if (status != ERROR_SUCCESS)
    {
        status = perror("RegOpenKeyEx(AppMap key)");
        goto cleanup;
    }

    size = linkPathLength * sizeof(WCHAR); // buffer size
    SetLastError(status = RegQueryValueEx(key, valueName + strlen(INPUT_PREFIX), NULL, &valueType, (PBYTE) linkPath, &size));
    if (status != ERROR_SUCCESS)
    {
        status = perror("RegQueryValueEx");
        goto cleanup;
    }

    if (valueType != REG_SZ)
    {
        SetLastError(status = ERROR_DATATYPE_MISMATCH);
        perror("RegQueryValueEx");
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
    WCHAR linkPath[MAX_PATH_LONG];
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
    DWORD_PTR ret;
    DWORD status;
#ifdef WRITE_PPM
    FILE *f1;
#endif

    // Set stdout to binary mode to prevent newline conversions.
    _setmode(_fileno(stdout), _O_BINARY);

    // Read input and convert it to the shortcut path.
    if (ERROR_SUCCESS != GetShortcutPath(linkPath, RTL_NUMBER_OF(linkPath)))
    {
        perror("GetShortcutPath");
        goto cleanup;
    }

    LogDebug("LinkPath: %s", linkPath);

    CoInitialize(NULL);
    // We use SHGFI_ICONLOCATION and load the icon manually later, because icons retrieved by
    // SHGFI_ICON always have the shortcut arrow overlay even if the overlay is not visible
    // normally (eg. for start menu shortcuts).
    ret = SHGetFileInfo(linkPath, 0, &shfi, sizeof(shfi), SHGFI_ICONLOCATION);
    LogDebug("SHGetFileInfo(%s) returned 0x%x, shfi.szDisplayName='%s', shfi.iIcon=%d", linkPath, ret, shfi.szDisplayName, shfi.iIcon);
    if (!ret)
    {
        status = perror("SHGetFileInfo(SHGFI_ICONLOCATION)");
        goto cleanup;
    }

    if (shfi.szDisplayName[0] == 0)
    {
        // This can happen for some reason: SHGetFileInfo returns nonzero (success according to MSDN),
        // but the icon info is missing, even if the file clearly has an icon. We fall back to
        // SHGFI_ICON which will retrieve an icon with the shortcut arrow overlay.
        ret = SHGetFileInfo(linkPath, 0, &shfi, sizeof(shfi), SHGFI_ICON);
        if (!ret)
        {
            status = perror("SHGetFileInfo(SHGFI_ICON)");
            goto cleanup;
        }
        ico = shfi.hIcon;
    }
    else
    {
        ico = ExtractIcon(0, shfi.szDisplayName, shfi.iIcon);
    }

    // Create bitmap for the icon.
    if (!GetIconInfo(ico, &ii))
    {
        status = perror("GetIconInfo");
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
            //fwrite(&r, 1, 1, stdout);
            //fwrite(&g, 1, 1, stdout);
            //fwrite(&b, 1, 1, stdout);
            //fwrite(&a, 1, 1, stdout);
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

cleanup:
    if (status != ERROR_SUCCESS)
        LogError("LinkPath: %s", linkPath);
    // Everything will be cleaned up upon process exit.
    return status;
}
