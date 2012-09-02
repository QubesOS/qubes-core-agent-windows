#include <Windows.h>
#include <stdlib.h>
#include <strsafe.h>


ULONG ConvertUTF8ToUTF16(PUCHAR pszUtf8, PWCHAR *ppwszUtf16, size_t *pcchUTF16)
{
	HRESULT	hResult;
	ULONG	uResult;
	size_t	cchUTF8;
	int	cchUTF16;
	PWCHAR	pwszUtf16;


	hResult = StringCchLengthA(pszUtf8, STRSAFE_MAX_CCH, &cchUTF8);
	if (FAILED(hResult)) {
		return hResult;
	}

	cchUTF16 = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pszUtf8, cchUTF8 + 1, NULL, 0);
	if (!cchUTF16) {
		uResult = GetLastError();
		return uResult;
	}

	pwszUtf16 = malloc(cchUTF16 * sizeof(WCHAR));
	if (!pwszUtf16)
		return ERROR_NOT_ENOUGH_MEMORY;

	uResult = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pszUtf8, cchUTF8 + 1, pwszUtf16, cchUTF16);
	if (!uResult) {
		uResult = GetLastError();
		return uResult;
	}

	pwszUtf16[cchUTF16 - 1] = L'\0';
	*ppwszUtf16 = pwszUtf16;
	if (pcchUTF16)
		*pcchUTF16 = cchUTF16 - 1; /* without terminating NULL character */

	return ERROR_SUCCESS;
}

ULONG ConvertUTF16ToUTF8(PWCHAR pwszUtf16, PUCHAR *ppszUtf8, size_t *pcbUtf8)
{
	PUCHAR pszUtf8;
	size_t cbUtf8;
    DWORD   dwConversionFlags;

    // WC_ERR_INVALID_CHARS is defined for Vista and later only
#if (WINVER >= 0x0600)
    dwConversionFlags = WC_ERR_INVALID_CHARS;
#else
    dwConversionFlags = 0;
#endif

	/* convert filename from UTF-16 to UTF-8 */
	/* calculate required size */
	cbUtf8 = WideCharToMultiByte(CP_UTF8, dwConversionFlags, pwszUtf16, -1, NULL, 0, NULL, NULL);
	if (!cbUtf8) {
		return GetLastError();
	}
	pszUtf8 = malloc(sizeof(PUCHAR)*cbUtf8);
	if (!pszUtf8) {
		return ERROR_NOT_ENOUGH_MEMORY;
	}
	if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, pwszUtf16, -1, pszUtf8, cbUtf8, NULL, NULL)) {
		free(pszUtf8);
		return GetLastError();
	}
	if (pcbUtf8)
		*pcbUtf8 = cbUtf8 - 1; /* without terminating NULL character */
	*ppszUtf8 = pszUtf8;
	return ERROR_SUCCESS;
}

