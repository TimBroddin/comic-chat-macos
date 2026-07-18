// lifted from artifacts/core/ccommon.cpp (string half only) — see plan-3
// R-notes (docs/superpowers/plans/2026-07-18-macos-port-plan-3-protocol.md
// "Edit Rules added by this plan (R18-R21)", carried-forward R1-R17).
//
// Only the pure string/byte codec functions the Plan-3 Task-2 brief names are
// lifted here: bLowLevelQuoting (ccommon.cpp:945), bLowLevelUnquoting (:1026),
// bConvertWideStringToUTF8 (:218), bConvertUTF8StringToWide (:364),
// SzNextUTF8Char (:500). Every socket/registry/WinInet function in the
// original ccommon.cpp is left behind (this file has zero dependency on
// windows.h/winsock). Each function's own dependency list was checked before
// lifting (grepped for calls inside its body): the only non-self-referential
// calls are ASSERT, CharNext, lstrlen, and lstrlenW -- all satisfied by
// mfc_compat.h (CharNext/lstrlen already present; lstrlenW/WCHAR/LPWSTR/
// LPCWSTR added below under R9, see mfc_compat.h). No forbidden dependency
// (socket/registry/windows.h) is dragged in by any of the five.
//
// Edit Rules applied, hunk by hunk (fidelity diff in the task report):
//   R1  - #include "stdafx.h" (original: #include "CCommon.H" / "CDebug.H",
//         SZTHISFILE) -> #include "ccommon_str.h" (+ mfc_compat.h transitively).
//         CDebug.H's SZTHISFILE/debug-heap machinery is not lifted (R8-style:
//         it's a debug-build-only diagnostic aid, not string-codec logic; no
//         lifted function's *behavior* depends on it).
//   R13 - the original's ASSERT(cond, "message") is CDebug.H's two-argument
//         debug-assert macro (distinct from MFC's single-arg ASSERT already
//         used elsewhere in this port's lifted files, e.g. format.cpp).
//         mfc_compat.h's ASSERT is single-argument (fires via ccLogError with
//         the condition text + file:line -- a strict superset of the dropped
//         message string's information). Rewritten as ASSERT(cond); every
//         instance is a straight drop of the redundant literal, condition
//         unchanged. ASSERT(FALSE, "...") -> ASSERT(0), matching the existing
//         ASSERT(0) idiom already used in lifted format.cpp/balloon.cpp.
//         Every instance individually listed in the task report.
#include "ccommon_str.h"
#include <cstring>  // memcpy-free byte copies below use pointer arithmetic only;
                    // no CRT string function is called by the lifted bodies
                    // themselves (kept for lstrlen's strlen() use, already
                    // provided by mfc_compat.h -- included here defensively,
                    // matches the original's transitive CRT availability).

// --- constants, verbatim values from artifacts/inc/ccommon.h:33-41 ----------
const TCHAR g_chLF         = '\n';
const TCHAR g_chCR         = '\r';
const TCHAR g_chExtNckPfx  = '\'';
const TCHAR g_chExtChnPfx  = '%';
const TCHAR g_chGblChnPfx  = '#';
const TCHAR g_chLclChnPfx  = '&';
const TCHAR g_chLLQuoteIRCX = '\\';
const TCHAR g_chLLQuoteCTCP = 0x10;

// R9 (Plan 3 Task 2): lstrlenW -- Win32's wide-string length (WCHAR is the
// UTF-16 code-unit width Win32 itself uses; see mfc_compat.h's WCHAR/LPWSTR/
// LPCWSTR typedefs added alongside this). bConvertWideStringToUTF8 is the
// only lifted caller.
static inline INT lstrlenW(LPCWSTR s) {
    INT n = 0;
    if (s) while (s[n]) n++;
    return n;
}

////////////////////////////////////////////////////////////////////////////
// Encoding routine:
// szSrc:	string to encode
// pszDst:	pointer where resulting string should be placed
// pbFree:	set to TRUE if at least one character was quoted and the caller
//          needs to free the resulting string
// Encoding rules:
//				'<Q>'  -> "<Q><Q>"
//				'<Q>n' -> "<Q>n"
//				'<Q>r' -> "<Q>r"
// The memory allocation for the resulting string is done internally.
// The caller should deallocate the string if the function succeeds and
// pbFree is set to TRUE
// Function failure means an OOM condition
////////////////////////////////////////////////////////////////////////////
BOOL bLowLevelQuoting(TCHAR chQuotingChar, BOOL bTreatAsByteArray, LPCTSTR szSrc, LPTSTR *pszDst, BOOL *pbFree, BOOL bRemoveCarriageReturns)
{
	ASSERT(szSrc);   // R13: was ASSERT(szSrc,  "szSrc  is NULL in bLowLevelQuoting")
	ASSERT(pszDst);  // R13: was ASSERT(pszDst, "pszDst is NULL in bLowLevelQuoting")
	ASSERT(pbFree);  // R13: was ASSERT(pbFree, "pbFree is NULL in bLowLevelQuoting")

	LPTSTR	szTmpDst;
	LPCTSTR	szTmpSrcNC, szTmpSrc = szSrc;
	UINT	cToBeQuoted = 0, i;

	*pszDst = NULL;
	*pbFree = FALSE;

	while (g_chEOS != *szTmpSrc)
	{
		if (*szTmpSrc == chQuotingChar || *szTmpSrc == g_chLF || *szTmpSrc == g_chCR)
			cToBeQuoted++;
		szTmpSrc = bTreatAsByteArray ? szTmpSrc+1 : CharNext(szTmpSrc);
	}

	if (0 == cToBeQuoted)
	{
		*pszDst = (LPTSTR) szSrc;
		return TRUE;
	}

	if (!(*pszDst = new TCHAR[(szTmpSrc-szSrc) + cToBeQuoted + 1]))
		return FALSE;

	szTmpDst = *pszDst;
	szTmpSrc = szSrc;

	while (*szTmpSrc != g_chEOS)
	{
		switch (*szTmpSrc)
		{
			case g_chLF:
				*szTmpDst++ = chQuotingChar;
				*szTmpDst++ = 'n';
				szTmpSrc++;
				break;

			case g_chCR:
				if (!bRemoveCarriageReturns)
				{
					*szTmpDst++ = chQuotingChar;
					*szTmpDst++ = 'r';
				}
				szTmpSrc++;
				break;

			default:
				if (*szTmpSrc == chQuotingChar)
					*szTmpDst++ = chQuotingChar;
				szTmpSrcNC = bTreatAsByteArray ? szTmpSrc+1 : CharNext(szTmpSrc);
				for (i = 0; i < (UINT) (szTmpSrcNC - szTmpSrc); i++)
					*szTmpDst++ = *(szTmpSrc+i);
				szTmpSrc = szTmpSrcNC;
		}
	}
	*szTmpDst = g_chEOS;

	*pbFree = TRUE;

	return TRUE;
}


////////////////////////////////////////////////////////////////////////////
// Decoding routine:
// szSrc:	string to decode
// szDst:	resulting string
// Decoding rules:
//				"<Q><Q>" -> '<Q>'
//				"<Q>n" -> '<Q>n'
//				"<Q>r" -> '<Q>r'
// The resulting block is at most as big as the original one, so we can
// overwrite the original string, therefore the caller can set szDst to szSrc
// No mem allocation is done in this function
// Function failure means that couldn't properly decode string
////////////////////////////////////////////////////////////////////////////
BOOL bLowLevelUnquoting(TCHAR chQuotingChar, BOOL bTreatAsByteArray, LPCTSTR szSrc, LPTSTR szDst)
{
	LPCTSTR	szReadNC, szRead  = szSrc;
	LPTSTR	szWrite = szDst;
	BOOL	bQuotedChar = FALSE;
	BOOL	bQuotedString = TRUE;
	UINT	i;

	ASSERT(szSrc);  // R13: was ASSERT(szSrc, "szSrc  is NULL in bLowLevelUnquoting")
	ASSERT(szDst);  // R13: was ASSERT(szDst, "szDst  is NULL in bLowLevelUnquoting")

	// First check if the string seems to be low level quoted or not
	while (*szRead != g_chEOS)
	{
		if (chQuotingChar == *szRead)
		{
			switch (*(szRead+1))
			{
				case 'n':
				case 'r':
					bQuotedChar = TRUE;
					szRead++;
					break;

				default:
					if (chQuotingChar == *(szRead+1))
						szRead++;
					else
					{
						bQuotedString = FALSE;
						goto Unquote;
					}
			}
		}
		szRead = bTreatAsByteArray ? szRead+1 : CharNext(szRead);
	}

Unquote:
	if ((!bQuotedChar || !bQuotedString) && szDst == szSrc)
		return TRUE;

	szRead = szSrc;

	if (!bQuotedString)
	{
		while (*szRead != g_chEOS)
			*szWrite++ = *szRead++;
	}
	else
	{
		while (*szRead != g_chEOS)
		{
			if (chQuotingChar == *szRead)
			{
				switch (*(szRead+1))
				{
					case 'n':
						*szWrite = g_chLF;
						break;
					case 'r':
						*szWrite = g_chCR;
						break;
					default:
						if (chQuotingChar == *(szRead+1))
							*szWrite = chQuotingChar;
						else
						{
							ASSERT(0);  // R13: was ASSERT(FALSE, "Unexpected string format in bLowLevelUnquoting")
							szDst = NULL;
							return FALSE;
						}
				}
				szRead++;
			}
			else
			{
				szReadNC = bTreatAsByteArray ? szRead+1 : CharNext(szRead);
				for (i = 0; i < (UINT) (szReadNC - szRead); i++)
					*(szWrite+i) = *(szRead+i);
			}

			szWrite = bTreatAsByteArray ? szWrite+1 : CharNext(szWrite);
			szRead = bTreatAsByteArray ? szRead+1 : CharNext(szRead);
		}
	}

	*szWrite = g_chEOS;

	return TRUE;
}


BOOL bConvertWideStringToUTF8(LPCWSTR wszInStr, INT cchIn, LPTSTR *pszOutStr, INT *pcchOut, BOOL bNickname, BOOL bChannelName, BOOL bPostProcess, BOOL bEscapeWildcards)
{
	ASSERT(wszInStr);                     // R13: was ASSERT(wszInStr, "wszInStr in NULL in bConvertWideStringToUTF8")
	ASSERT(pszOutStr);                    // R13: was ASSERT(pszOutStr, "pszOutStr is NULL in bConvertWideStringToUTF8")
	ASSERT(!bNickname || !bChannelName);  // R13: was ASSERT(!bNickname || !bChannelName, "Both bNickname and bChannelName set in bConvertWideStringToUTF8")

	LPCWSTR	wszTmpIn = wszInStr;
	LPTSTR	szTmpOut;
	INT		cchInL = cchIn ? cchIn : lstrlenW(wszInStr);
	INT		cchOutL = 0;

	*pszOutStr = NULL;

	if (pcchOut)
		*pcchOut = 0;

	ASSERT(cchInL > 0);  // R13: was ASSERT(cchInL > 0, "szInStr is empty in bConvertWideStringToUTF8")

	// Allocate tripple size buffer
	szTmpOut = *pszOutStr = (LPTSTR) new TCHAR[3*(cchInL+1)];

	if (!*pszOutStr)
		return FALSE;

	if (bNickname)
	{
		// Put a single quote as a prefix
		*szTmpOut++ = g_chExtNckPfx;
		cchOutL = 1;
	}
	else if (bChannelName)
	{
		// Put a '#', '&', '%#' or '%&' as a prefix
		if (*wszTmpIn == L'%')
		{
			*szTmpOut++ = g_chExtChnPfx;
			cchOutL = 1;
			wszTmpIn++;
		}
		else
		{
			*szTmpOut++ = g_chExtChnPfx;
			cchOutL = 1;
		}

		if (*wszTmpIn == L'#' || *wszTmpIn == L'&')
		{
			*szTmpOut++ = (TCHAR) *wszTmpIn;
			cchOutL++;
			wszTmpIn++;
		}
		else
		{
			// Add a '#' if prefix is missing
			*szTmpOut++ = g_chGblChnPfx;
			cchOutL++;
		}
	}

    while (*wszTmpIn)
    {
        if (bPostProcess && *wszTmpIn == L'\0')	// REGISB: Don't know if this can ever happen
        {
            *szTmpOut++ = '\\';
            *szTmpOut++ = '0';
            cchOutL += 2;
        }
        else if (bPostProcess && *wszTmpIn == L'\n')
        {
            *szTmpOut++ = '\\';
            *szTmpOut++ = 'n';
            cchOutL += 2;
        }
        else if (bPostProcess && *wszTmpIn == L'\r')
        {
            *szTmpOut++ = '\\';
            *szTmpOut++ = 'r';
            cchOutL += 2;
        }
        else if (bPostProcess && *wszTmpIn == L'\t')
        {
            *szTmpOut++ = '\\';
            *szTmpOut++ = 't';
            cchOutL += 2;
        }
        else if (bPostProcess && *wszTmpIn == L' ')
        {
            *szTmpOut++ = '\\';
            *szTmpOut++ = 'b';
            cchOutL += 2;
        }
        else if (bPostProcess && *wszTmpIn == L',')
        {
            *szTmpOut++ = '\\';
            *szTmpOut++ = 'c';
            cchOutL += 2;
        }
        else if (bPostProcess && *wszTmpIn == L'\\')
        {
            *szTmpOut++ = '\\';
            *szTmpOut++ = '\\';
            cchOutL += 2;
        }
		else if (bEscapeWildcards && *wszTmpIn == L'*')
		{
            *szTmpOut++ = '\\';
            *szTmpOut++ = '*';
            cchOutL += 2;
		}
		else if (bEscapeWildcards && *wszTmpIn == L'?')
		{
            *szTmpOut++ = '\\';
            *szTmpOut++ = '?';
            cchOutL += 2;
		}
        else if ((unsigned short) *wszTmpIn <= 0x7F)
        {
            *szTmpOut++ = (TCHAR) *wszTmpIn;
            cchOutL += 1;
        }
        else if ((unsigned short) *wszTmpIn <= 0x07FF)
        {
            *szTmpOut++ = 0xC0 | ((*wszTmpIn >> 6) & 0x1F);	// Kent's code says 0x03 instead
            *szTmpOut++ = 0x80 | (*wszTmpIn & 0x3F);
            cchOutL += 2;
        }
        else
        {
            *szTmpOut++ = 0xE0 | ((*wszTmpIn >> 12) & 0x0F);
            *szTmpOut++ = 0x80 | ((*wszTmpIn >> 6) & 0x3F);
            *szTmpOut++ = 0x80 | (*wszTmpIn & 0x3F);
			cchOutL += 3;
        }

        wszTmpIn++;
    }

	*szTmpOut = '\0';

	if (pcchOut)
		*pcchOut = cchOutL;

	return TRUE;
}


BOOL bConvertUTF8StringToWide(LPCTSTR szInStr, INT cchIn, LPWSTR *pwszOutStr, INT *pcchOut, BOOL bNickname, BOOL bChannelName, BOOL bPostProcess)
{
	ASSERT(szInStr);                      // R13: was ASSERT(szInStr, "szInStr in NULL in bConvertUTF8StringToWide")
	ASSERT(pwszOutStr);                   // R13: was ASSERT(pwszOutStr, "pwszOutStr is NULL in bConvertUTF8StringToWide")
	ASSERT(!bNickname || !bChannelName);  // R13: was ASSERT(!bNickname || !bChannelName, "Both bNickname and bChannelName set in bConvertUTF8StringToWide")

	LPCTSTR	szTmpIn = szInStr;
	LPWSTR	wszTmpOut;
	INT		cchInL = cchIn ? cchIn : lstrlen(szInStr);
	INT		cchOutL = 0;

	*pwszOutStr = NULL;

	if (pcchOut)
		*pcchOut = 0;

	ASSERT(cchInL > 0);  // R13: was ASSERT(cchInL > 0, "szInStr is empty in bConvertUTF8StringToWide")

	// Allocate same size buffer
	wszTmpOut = *pwszOutStr = (LPWSTR) new WCHAR[cchInL+1];

	if (!*pwszOutStr)
		return FALSE;

    // For nicknames, skip the initial single quote
	if (bNickname)
	{
		ASSERT(g_chExtNckPfx == *szInStr);  // R13: was ASSERT(g_chExtNckPfx == *szInStr, "Nickname has no single quote prefix in bConvertUTF8StringToWide")
		szTmpIn++;
		cchInL--;
	}
	else if (bChannelName)
	{
		// Put a '%#' or '%&' as a prefix
		ASSERT(*szTmpIn == '%' || *szTmpIn == '#' || *szTmpIn == '&');  // R13: was ASSERT(..., "Unexpected channel prefix in bConvertUTF8StringToWide")

		if (*szTmpIn == g_chExtChnPfx)
		{
			szTmpIn++;
			cchInL--;
			*wszTmpOut++ = L'%';
	        cchOutL++;
		}

		if (*szTmpIn == g_chGblChnPfx || *szTmpIn == g_chLclChnPfx)
		{
			*wszTmpOut++ = (WCHAR) *szTmpIn;
	        cchOutL++;
			szTmpIn++;
			cchInL--;
		}
	}

    while (g_chEOS != *szTmpIn)
    {
        // The backslash is an escape character, convert back to raw.
        if (bPostProcess && *szTmpIn == '\\')
        {
            szTmpIn++;
			cchInL--;

            if (*szTmpIn == '0')		// REGISB: Don't think this can ever happen
            {
                *wszTmpOut++ = L'\0';
            }
            else if (*szTmpIn == 'n')
            {
                *wszTmpOut++ = L'\n';
            }
            else if (*szTmpIn == 'r')
            {
                *wszTmpOut++ = L'\r';
            }
            else if (*szTmpIn == 't')
            {
                *wszTmpOut++ = L'\t';
            }
            else if (*szTmpIn == 'b')
            {
                *wszTmpOut++ = L' ';
            }
            else if (*szTmpIn == 'c')
            {
                *wszTmpOut++ = L',';
            }
            else if (*szTmpIn == '\\')
            {
                *wszTmpOut++ = L'\\';
            }
        }
        else if ((unsigned short) *szTmpIn <= 0x7F)
        {
            *wszTmpOut++ = (WCHAR) *szTmpIn;
        }
        else if ((*szTmpIn & 0xE0) == 0xC0)
        {
            //  Must have at least two remaining characters in the string.
            if (cchInL >= 2)
            {								 // Kent's code uses 0x03 instead !?
                *wszTmpOut++ = ((szTmpIn[0] & 0x1F) << 6) | (szTmpIn[1] & 0x3F);

                szTmpIn++;
                cchInL--;
            }
            else
                *wszTmpOut++ = L'?';
        }
        else
        {
            //  Must have at least three remaining characters in the string.
            if (cchInL >= 3)
            {
                *wszTmpOut++ = ((szTmpIn[0] & 0x0F) << 12) | ((szTmpIn[1] & 0x3F) << 6) | (szTmpIn[2] & 0x3F);

                szTmpIn += 2;
                cchInL -= 2;
            }
            else
                *wszTmpOut++ = L'?';
        }

        // Skip to the next character.
        cchOutL++;
        szTmpIn++;
		cchInL--;
    }

	*wszTmpOut = L'\0';

	if (pcchOut)
		*pcchOut = cchOutL;

	return TRUE;
}


LPCTSTR SzNextUTF8Char(LPCTSTR szInStr)
{
	ASSERT(szInStr);  // R13: was ASSERT(szInStr, "szInStr is NULL in SzNextUTF8Char")

	if (!*szInStr)
		return szInStr;

    if (*szInStr == '\\')
    {
		switch (*(szInStr+1))
		{
			case '0':
				return szInStr+1;

			case 'n':
			case 'r':
			case 't':
			case 'b':
			case 'c':
			case '\\':
				return szInStr+2;

			default:
				return szInStr+1;
        }
	}
    else
		if ((unsigned short) *szInStr <= 0x7F)
				return szInStr+1;
        else
			if ((*szInStr & 0xE0) == 0xC0)
			{
				//  Should have at least two remaining characters in the string.
				if (*(szInStr+1))
					return szInStr+2;
				else
					return szInStr+1;
			}
	        else
		    {
				//  Should have at least three remaining characters in the string.
				if (*(szInStr+1) && *(szInStr+2))
					return szInStr+3;
				else
					return szInStr+1;
			}
}
