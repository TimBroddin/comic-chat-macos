// ccommon_str.h — lifted from artifacts/inc/ccommon.h (string half only).
// Plan 3 Task 2. See ccommon_str.cpp for the provenance/Edit Rules note.
//
// artifacts/inc/ccommon.h declares ~20 externs; this header carries over ONLY
// the constants and prototypes the string/byte codec functions lifted into
// ccommon_str.cpp need (g_chLLQuoteCTCP/g_chLLQuoteIRCX + the five function
// signatures named in the task brief). Every other ccommon.h declaration
// (bExtendedString/bExtendedChannelName/bConvertString/bDataToString/
// bStringToData/bSB2DBKatakana/... ) is NOT copied here -- those functions are
// not lifted this task (out of the brief's named scope) and are not called by
// the five that are, so there is no dangling reference to satisfy.
#ifndef CCOMMON_STR_H
#define CCOMMON_STR_H

#include "mfc_compat.h"  // R1: TCHAR/LPCTSTR/LPTSTR/BOOL/INT + WCHAR/LPCWSTR/LPWSTR (R9 additions)

// --- constants, verbatim from artifacts/inc/ccommon.h:26-41 ------------------
// (ccommon.h also defines g_chEOS/g_chComma/g_chTransparent, already provided
// byte-identically by mfc_compat.h per R8's Plan-2 note -- not re-declared
// here to avoid a duplicate-definition clash.) The brief names g_chLLQuoteCTCP/
// g_chLLQuoteIRCX explicitly as Produces; g_chLF/g_chCR/g_chExtNckPfx/
// g_chExtChnPfx/g_chGblChnPfx/g_chLclChnPfx are additional ccommon.h constants
// the five lifted function bodies reference verbatim (bLowLevelQuoting/
// Unquoting use g_chLF/g_chCR; bConvertWideStringToUTF8/bConvertUTF8StringToWide
// use the nick/channel-name prefix chars) -- carried over from the same
// ccommon.h const block since the bodies would not otherwise compile.
extern const TCHAR g_chLF;            // ccommon.h:33 -- '\n'
extern const TCHAR g_chCR;            // ccommon.h:34 -- '\r'
extern const TCHAR g_chExtNckPfx;     // ccommon.h:35 -- '\'' (nickname prefix)
extern const TCHAR g_chExtChnPfx;     // ccommon.h:36 -- '%' (extended channel prefix)
extern const TCHAR g_chGblChnPfx;     // ccommon.h:37 -- '#' (global channel prefix)
extern const TCHAR g_chLclChnPfx;     // ccommon.h:38 -- '&' (local channel prefix)
extern const TCHAR g_chLLQuoteIRCX;   // ccommon.h:39 -- '\\', histent.cpp save-file quoting only (NOT wire)
extern const TCHAR g_chLLQuoteCTCP;   // ccommon.h:41 -- 0x10 (DLE), used for all wire traffic

// --- function prototypes, verbatim from artifacts/inc/ccommon.h:51-53,59-60 -
extern BOOL    bConvertWideStringToUTF8(LPCWSTR wszInStr, INT cchIn, LPTSTR *pszOutStr, INT *pcchOut, BOOL bNickname = FALSE, BOOL bChannelName = FALSE, BOOL bPostProcess = TRUE, BOOL bEscapeWildcards = FALSE);
extern BOOL    bConvertUTF8StringToWide(LPCTSTR szInStr, INT cchIn, LPWSTR *pwszOutStr, INT *pcchOut, BOOL bNickname = FALSE, BOOL bChannelName = FALSE, BOOL bPostProcess = TRUE);
extern LPCTSTR SzNextUTF8Char(LPCTSTR szInStr);
extern BOOL    bLowLevelQuoting(TCHAR chQuotingChar, BOOL bTreatAsByteArray, LPCTSTR szSrc, LPTSTR *pszDst, BOOL *pbFree, BOOL bRemoveCarriageReturns = FALSE);
extern BOOL    bLowLevelUnquoting(TCHAR chQuotingChar, BOOL bTreatAsByteArray, LPCTSTR szSrc, LPTSTR szDst);

// --- Plan 3 Task 4 addition: bExtendedNickname (ccommon.cpp:109) ------------
// Needed by ircproto.cpp's ChatChangeNick/ChatBanUser/StrEncodeCommandParam
// (IsIRCX() && bExtendedNickname(...) gates whether a nickname needs the
// UTF-8 EncodeNick() treatment before going on the wire). Pure single-byte
// character-class scan, zero dependency beyond g_chEOS (already in
// mfc_compat.h) -- same tier as the five functions above, just not named in
// the Task 2 brief because Task 2 had no outbound-builder caller yet.
extern BOOL    bExtendedNickname(LPCTSTR szNickname);

#endif // CCOMMON_STR_H
