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
#ifdef WRITE_PPM
#include <strsafe.h>
#endif

#include <stdio.h>
#include <io.h>
#include <fcntl.h>

#include <qubes-io.h>
#include <utf8-conv.h>
#include <log.h>

HICON GetIcon(HWND window)
{
    HICON iconHandle;

    iconHandle = (HICON)SendMessage(window, WM_GETICON, ICON_BIG, 0);
    if (iconHandle != NULL)
        return iconHandle;

    iconHandle = (HICON)SendMessage(window, WM_GETICON, ICON_SMALL, 0);
    if (iconHandle != NULL)
        return iconHandle;

    iconHandle = (HICON)GetClassLongPtr(window, GCLP_HICON);
    if (iconHandle != NULL)
        return iconHandle;

    iconHandle = (HICON)GetClassLongPtr(window, GCLP_HICONSM);
    if (iconHandle != NULL)
        return iconHandle;

    return NULL;
}

BOOL CALLBACK EnumWindowsProc(
    _In_ HWND   hwnd,
    _In_ LPARAM lParam
    )
{
    ICONINFO ii;
    HICON ico;
    HDC dc;
    BITMAP bm;
    DWORD size;
    BYTE *buffer;
    BITMAPINFO bmi;
    int x, y;
    DWORD status;
#ifdef WRITE_PPM
    FILE *f1;
    CHAR path[MAX_PATH];
#endif

    if (!IsWindowVisible(hwnd))
        return TRUE;
    
    ico = GetIcon(hwnd);
    if (ico == NULL)
        return TRUE;

    LogDebug("Icon for window 0x%x (%I64d)", hwnd, hwnd);
    printf("%I64d\n", hwnd);

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
    buffer = (BYTE *)malloc(size); // pixel buffer
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
    StringCbPrintfA(path, sizeof(path), "icon-%x.ppm", hwnd);
    f1 = fopen(path, "w");
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
    return TRUE;
}

int wmain(int argc, WCHAR *argv[])
{
    WORD pi = -1;

    // Set stdout to binary mode to prevent newline conversions.
    _setmode(_fileno(stdout), _O_BINARY);

    if (!EnumWindows(EnumWindowsProc, 0))
        return 1;

    return 0;
}
