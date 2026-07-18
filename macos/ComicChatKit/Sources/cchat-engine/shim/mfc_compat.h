// mfc_compat.h — minimal MFC/Win32 compatibility layer for lifted engine code.
// macOS port only. Add members ONLY when a lifted file requires them (rule R9),
// and add a selftest for each addition.
#ifndef MFC_COMPAT_H
#define MFC_COMPAT_H

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cstdarg>
#include <string>
#include <vector>
#include <strings.h>
#include <unordered_map>

// comicchat.h (cc_canvas / cc_font_spec / cc_path_pt / …) has zero dependency
// on this header, so it's safe to pull in unconditionally here (Plan 2 Task
// 3's CDC adapter needs the full cc_canvas_ops vtable shape, not just a
// forward declaration). cc_canvas.h (CCanvas, the header-only forwarding
// wrapper) is included further down, after ASSERT is defined -- it needs
// only that macro from this file, so pulling it in early would be a real
// circular include (cc_canvas.h -> mfc_compat.h -> cc_canvas.h) whereas
// deferring it is not (mfc_compat.h -> cc_canvas.h -> mfc_compat.h, but the
// inner mfc_compat.h hits its own include guard immediately and returns).
#include "comicchat.h"

// --- scalar typedefs -------------------------------------------------------
typedef int            BOOL;
typedef uint8_t        BYTE;
typedef uint8_t        UCHAR;
typedef uint16_t       WORD;
typedef uint16_t       USHORT;
typedef uint32_t       DWORD;
typedef uint32_t       ULONG;
typedef uint32_t       UINT;
typedef int32_t        INT;    // R9: format.cpp's PushFormattingOffsetsDW(DWORD*, INT, SHORT)
typedef int32_t        LONG;   // Win32 LONG is 32-bit; engine structs rely on it
typedef int16_t        SHORT;  // R9: avatar loading chain (bbox.h SRECT, avatar.h m_iconIndex)
typedef char           CHAR;   // R9: avatar.h GetIndices/SetIndices signed-byte indices
typedef char           TCHAR;
typedef const char*    LPCSTR;
typedef char*          LPSTR;
typedef const char*    LPCTSTR;
typedef char*          LPTSTR;
// R9 (Plan 3 Task 2): WCHAR is Win32's UTF-16 code-unit width (16-bit), used
// by ccommon_str.cpp's bConvertWideStringToUTF8/bConvertUTF8StringToWide (the
// only lifted callers). Deliberately NOT wchar_t -- wchar_t is 32-bit on this
// platform (Darwin), which would silently double every wide-string buffer's
// byte layout relative to the original Win32 UCS-2 assumption these functions
// were written against (their `new WCHAR[]` allocation math, and the BMP-only
// 1-to-3-byte UTF-8 encode/decode, both assume a 16-bit code unit). uint16_t
// matches Win32's actual WCHAR exactly.
typedef uint16_t       WCHAR;
typedef const WCHAR*   LPCWSTR;
typedef WCHAR*         LPWSTR;
typedef void*          LPVOID;
typedef BYTE*          LPBYTE;   // R9: avatar.h CPose constructor params
typedef DWORD*         LPDWORD;  // R9: avatar.h CPose constructor params
typedef WORD*          LPWORD;   // R9: avbfile.cpp ConvertMasksCommon pixel packing
typedef BYTE*          PBYTE;    // R9: avbfile.cpp AllocAndReadCompressedBuffer / ConvertMasksCommon
typedef void*          PVOID;    // R9: avbfile.cpp AllocAndReadCompressedBuffer
#define TRUE  1
#define FALSE 0

// --- misc Win32 macros/constants (rule R9) ---------------------------------
#define _MAX_PATH 260           // avbfile.h CAvatarFileStream::m_szFileName
#define _MAX_FNAME 256          // avatar.cpp CAvatarX copy ctor / GetAllAvatarNames
#define _MAX_EXT 256            // avatar.cpp GetAllAvatarNames (dir-scan stub path)
#define LOBYTE(w)  ((BYTE)(w))
#define HIBYTE(w)  ((BYTE)(((WORD)(w)) >> 8))
#define LOWORD(l)  ((WORD)(l))
#define HIWORD(l)  ((WORD)(((DWORD)(l)) >> 16))
inline void ZeroMemory(void* p, size_t n) { memset(p, 0, n); }
struct RGBTRIPLE { BYTE rgbtBlue; BYTE rgbtGreen; BYTE rgbtRed; };

// --- CRT case-insensitive compare + min/max (rule R9; avatar.cpp) ----------
// Win32's <windef.h> min/max are macros (no type-checking), not templates —
// matching that exactly avoids deduction failures on the original code's
// mixed int/float/double call sites (e.g. avatar.cpp's max(float, double)).
inline int stricmp(const char* a, const char* b) { return strcasecmp(a ? a : "", b ? b : ""); }
// strnicmp (rule R9; Plan 3 Task 6, protsupp.cpp's CTCP-verb dispatch table
// uses the length-bounded case-insensitive compare -- same Win32-CRT-ism as
// stricmp above, same POSIX equivalent).
inline int strnicmp(const char* a, const char* b, size_t n) { return strncasecmp(a ? a : "", b ? b : "", n); }
#define min(a,b) (((a) < (b)) ? (a) : (b))
#define max(a,b) (((a) > (b)) ? (a) : (b))

// --- GetTickCount (rule R9; avatar.cpp CAvatarComplex::SetSequential, only
//     reachable inside its CC_NO_UI-stubbed internals, but must still parse) -
inline DWORD GetTickCount() { return 0; }

// --- Win32 geometry & color -----------------------------------------------
struct POINT { LONG x; LONG y; };
struct SIZE  { LONG cx; LONG cy; };
struct RECT  { LONG left; LONG top; LONG right; LONG bottom; };
// R9 (Plan 2 Task 6): balloon.cpp forward-declares FindSubStringForINTLThatFits
// (intl.c) with an LPSIZE out-param -- Win32's SIZE*. Never dereferenced by
// lifted code (that function is unreachable on this CP-1252 port; see the
// stub in cc_link_stubs.cpp), but the declaration must name a real type.
typedef SIZE* LPSIZE;

typedef DWORD COLORREF;
#define RGB(r,g,b) ((COLORREF)(((BYTE)(r)) | (((DWORD)(BYTE)(g)) << 8) | (((DWORD)(BYTE)(b)) << 16)))
#define GetRValue(c) ((BYTE)(c))
#define GetGValue(c) ((BYTE)((c) >> 8))
#define GetBValue(c) ((BYTE)((c) >> 16))

// R9 (Plan 2 Task 6): Win32 SetRect fills a RECT. balloon.cpp's bURLHit uses
// it to build the hit-test rect. Matches <winuser.h> SetRect exactly.
inline BOOL SetRect(RECT* r, int l, int t, int rt, int b) {
    if (!r) return FALSE;
    r->left = l; r->top = t; r->right = rt; r->bottom = b;
    return TRUE;
}

// R9 (Plan 2 Task 7): Win32 ::SetRectEmpty(RECT*) zeroes a RECT. bodycam.cpp's
// CBody*::DrawBody uses the FREE-function form (not the CRect member) on the
// early-out path when a pose fails to load. Matches <winuser.h> SetRectEmpty
// exactly.
inline BOOL SetRectEmpty(RECT* r) {
    if (!r) return FALSE;
    r->left = r->top = r->right = r->bottom = 0;
    return TRUE;
}

// R9 (Plan 2 Task 6): GetSysColor(COLOR_WINDOW) -- balloon.cpp's
// iDrawFormattedTextLine compares a run's foreground color against the window
// background to detect a sender-requested-transparency run. This headless
// RGBA port has no system theme; the window background is plain white
// (the 1998 Win32 default and the only value this comparison meaningfully
// needs). Only COLOR_WINDOW is ever queried here.
#define COLOR_WINDOW 5
inline COLORREF GetSysColor(int /*index*/) {
    // Only COLOR_WINDOW is ever queried by lifted code; white is the 1998
    // Win32 default and the only value the transparency comparison needs.
    return RGB(255, 255, 255);
}

// --- DIB structures (packed, wire-compatible with the .avb format) ---------
#pragma pack(push, 2)
struct BITMAPFILEHEADER {
    WORD  bfType;
    DWORD bfSize;
    WORD  bfReserved1;
    WORD  bfReserved2;
    DWORD bfOffBits;
};
#pragma pack(pop)

#pragma pack(push, 4)
struct BITMAPINFOHEADER {
    DWORD biSize;
    LONG  biWidth;
    LONG  biHeight;
    WORD  biPlanes;
    WORD  biBitCount;
    DWORD biCompression;
    DWORD biSizeImage;
    LONG  biXPelsPerMeter;
    LONG  biYPelsPerMeter;
    DWORD biClrUsed;
    DWORD biClrImportant;
};
struct RGBQUAD { BYTE rgbBlue; BYTE rgbGreen; BYTE rgbRed; BYTE rgbReserved; };
struct BITMAPINFO { BITMAPINFOHEADER bmiHeader; RGBQUAD bmiColors[1]; };
#pragma pack(pop)
typedef RGBQUAD* LPRGBQUAD;
typedef BITMAPINFO* LPBITMAPINFO;
typedef BITMAPINFOHEADER* LPBITMAPINFOHEADER;  // R9: avbfile.cpp ConvertMasksCommon
#define BI_RGB  0u
#define BI_RLE8 1u
#define BI_RLE4 2u

// --- C-runtime string/file helpers (rule R9; avbfile.cpp CAvatarFileStream) --
inline int lstrlen(LPCSTR s) { return (int)strlen(s ? s : ""); }
inline char* lstrcpy(char* dst, LPCSTR src) { return strcpy(dst, src ? src : ""); }
#define __T(x) x
inline FILE* _tfopen(LPCTSTR path, LPCTSTR mode) { return fopen(path, mode); }

// --- lstrcmpi (rule R9; backdrop.cpp NotifyDownloadedBackdrop uses it as a
//     case-insensitive string compare, same semantics as stricmp) -----------
inline int lstrcmpi(LPCSTR a, LPCSTR b) { return strcasecmp(a ? a : "", b ? b : ""); }

// --- _T / _tcsXXX (rule R9; format.h's FIXEDPITCHFACENAMES/SYMBOLFACENAMES
//     array initializers use _T("Courier") etc.; format.cpp's FFixedPitchFont/
//     FSymbolFont/GetFormattedTextExtent use _tcscpy/_tcsicmp/_tcslen). This is
//     an ANSI build (TCHAR == char, matching the rest of this header), so
//     these collapse to their plain-char CRT equivalents exactly like Win32's
//     own tchar.h does when _UNICODE is not defined. ---------------------------
#define _T(x) __T(x)
inline char* _tcscpy(char* dst, LPCTSTR src) { return strcpy(dst, src ? src : ""); }
inline size_t _tcslen(LPCTSTR s) { return strlen(s ? s : ""); }
inline int _tcsicmp(LPCTSTR a, LPCTSTR b) { return strcasecmp(a ? a : "", b ? b : ""); }

// --- MAKELONG / IsDBCSLeadByte / CharNext / FillMemory (rule R9; format.cpp's
//     formatting-array packing (OOOOEEFB DWORDs) and control-byte scanning) --
// MAKELONG matches Win32's <windef.h> exactly: low WORD in bits 0-15, high
// WORD in bits 16-31.
#define MAKELONG(low, high) ((LONG)(((WORD)(low)) | (((DWORD)((WORD)(high))) << 16)))
// This port has no East-Asian DBCS codepage active (ANSI/Latin build), so
// IsDBCSLeadByte always reports FALSE -- matches Win32's own behavior under
// a single-byte codepage. format.cpp's DBCS lead-byte checks become no-ops,
// which is correct for this port (no multi-byte input is ever fed through
// the selftest/measurement path).
inline BOOL IsDBCSLeadByte(BYTE) { return FALSE; }
// CharNext advances one character, skipping an extra byte for a DBCS lead
// byte (never true here, see above) and never advancing past the terminating
// NUL -- matches Win32's own CharNext semantics at the end of a string.
inline LPTSTR CharNext(LPCTSTR p) { return (LPTSTR)(p + (*p ? 1 : 0)); }
inline void FillMemory(void* dst, size_t len, BYTE val) { memset(dst, val, len); }

// --- _mbschr / _mbsrchr / _mbspbrk + OurMbs* macros (rule R9; Plan 3 Task 3:
//     protsupp.cpp's key-string codec, utils.h:104-106) -----------------------
// The original's OurMbsChr/OurMbsRChr/OurMbsPbrk macros (utils.h) wrap the CRT
// MBCS-aware scan functions _mbschr/_mbsrchr/_mbspbrk so the key-string codec
// (FindInKeyString/GetValueFromKeyString) scans correctly under a DBCS
// codepage. This port's permanent posture is single-byte CP-1252 (no DBCS
// codepage is ever active -- the same decision IsDBCSLeadByte()->FALSE/
// CharNext() above already codify, and Plan 3 Task 2 made permanent for the
// intl.c seam). Under that posture the MBCS scan degenerates exactly to the
// plain single-byte CRT scan, so these collapse to strchr/strrchr/strpbrk.
// Signatures match the CRT's own _mbschr family (const unsigned char* needle
// haystack, taking/returning `const UCHAR*`), matching how utils.h's macros
// cast to/from `(const UCHAR*)(LPCSTR)`.
inline const UCHAR* _mbschr(const UCHAR* s, UINT c) {
    return (const UCHAR*)strchr((const char*)s, (int)c);
}
inline const UCHAR* _mbsrchr(const UCHAR* s, UINT c) {
    return (const UCHAR*)strrchr((const char*)s, (int)c);
}
inline const UCHAR* _mbspbrk(const UCHAR* s, const UCHAR* set) {
    return (const UCHAR*)strpbrk((const char*)s, (const char*)set);
}
#define OurMbsChr(s,c)   ((LPCSTR)_mbschr((const UCHAR*)(LPCSTR)(s), c))
#define OurMbsRChr(s,c)  ((LPCSTR)_mbsrchr((const UCHAR*)(LPCSTR)(s), c))
#define OurMbsPbrk(s,s2) ((LPCSTR)_mbspbrk((const UCHAR*)(LPCSTR)(s), (const UCHAR*)(LPCSTR)(s2)))

// --- legacy OS/2-style DIB header (wire-compatible; used only to detect the
//     older BITMAPCOREHEADER format, rule R9 for dib.cpp) --------------------
#pragma pack(push, 2)
struct BITMAPCOREHEADER {
    DWORD bcSize;
    WORD  bcWidth;
    WORD  bcHeight;
    WORD  bcPlanes;
    WORD  bcBitCount;
};
#pragma pack(pop)

// --- raster-op / DIB-color constants (declarations only stay live per R4;
//     values match Win32 exactly since dib.h uses SRCCOPY as a default
//     parameter value that must compile even with drawing stubbed out) ------
#define SRCCOPY        0x00CC0020
#define DIB_RGB_COLORS 0

// --- file existence check (rule R9; avatario.cpp uses GetFileAttributes()
//     purely to test for file existence before opening) ---------------------
DWORD GetFileAttributes(LPCSTR pszPath);
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)

// --- MFC exception raisers (rule R9; avbfile.cpp ConvertMasksCommon and
//     CChatBackdrop::LoadBackdrop call these on allocation/format failure).
//     MFC's AfxThrowMemoryException()/AfxThrowUserException() throw
//     CMemoryException*/CUserException*; matching our TRY/CATCH_ALL(e) shim
//     (plain try/catch(...)), these just throw a generic marker so the
//     surrounding catch(...) block still triggers. ---------------------------
struct CMemoryException {};
struct CUserException {};
[[noreturn]] inline void AfxThrowMemoryException() { throw CMemoryException{}; }
[[noreturn]] inline void AfxThrowUserException() { throw CUserException{}; }

// --- diagnostics ------------------------------------------------------------
// Plan 2 Task 1: level-gated logging. 0=silent, 1=errors (ASSERT/VERIFY
// failures), 2=trace (default). ccLogWouldEmit lets selftests assert the gate
// itself rather than scraping stdout/stderr.
void ccLog(const char* fmt, ...);       // TRACE-level (gate: level >= 2)
void ccLogError(const char* fmt, ...);  // error-level (gate: level >= 1); ASSERT/VERIFY
int  ccLogWouldEmit(int level);
#ifdef NDEBUG
#define ASSERT(e) ((void)0)
#else
#define ASSERT(e) do { if (!(e)) { ccLogError("ASSERT failed: %s (%s:%d)", #e, __FILE__, __LINE__); abort(); } } while (0)
#endif
#define VERIFY(e) do { if (!(e)) ccLogError("VERIFY failed: %s (%s:%d)", #e, __FILE__, __LINE__); } while (0)
#define TRACE(...) ccLog(__VA_ARGS__)

// --- Win32 NLS surface for balloon.cpp's Capitalize (rule R9; Plan 3 Task 2,
//     restoring Capitalize verbatim -- see balloon.cpp for the full history).
//     Capitalize's reachable branch on this port is exclusively the `default:`
//     one (ccContext().session.charSet has no setter anywhere in the engine;
//     it is permanently 0 == ANSI_CHARSET, the CP-1252 posture Task 2 Step 1
//     makes permanent) -- CharUpperBuff below is the only one of these that
//     ever actually executes. MAKELCID/MAKELANGID/LANG_*/SUBLANG_NEUTRAL/
//     SORT_DEFAULT/LCMapString/LCMAP_UPPERCASE exist ONLY so the GREEK_CHARSET/
//     RUSSIAN_CHARSET/TURKISH_CHARSET branches (dead code under this port's
//     fixed CP-1252 posture, but part of Capitalize's ORIGINAL body, which the
//     brief requires restoring verbatim rather than re-forking) still compile;
//     they are unreachable and intentionally unimplemented placeholders (each
//     ASSERTs if ever actually called -- a real Far-East/Cyrillic build would
//     need to implement them for real, at which point this comment is stale).
#define LANG_GREEK       0x08
#define LANG_RUSSIAN     0x19
#define LANG_TURKISH     0x1f
#define SUBLANG_NEUTRAL  0x00
#define SORT_DEFAULT     0x00
inline DWORD MAKELANGID(int primary, int sub) { return (DWORD)((sub << 10) | primary); }
inline DWORD MAKELCID(DWORD langid, int /*sort*/) { return langid; }
#define LCMAP_UPPERCASE  0x00000200
inline int LCMapString(DWORD /*lcid*/, DWORD /*mapFlags*/, const char* /*src*/, int /*cchSrc*/, char* /*dst*/, int /*cchDst*/) {
    ASSERT(0);  // unreachable: charSet is permanently ANSI_CHARSET on this port (see above)
    return 0;
}
// R9 (Plan 3 Task 2, extended Plan 4a Task 3): CharUpperBuff -- Capitalize's
// actually-reachable default-branch call (theApp.m_charSet == ANSI_CHARSET,
// i.e. CP-1252). Real Win32 CharUpperBuffA under codepage 1252 uppercases the
// ASCII range exactly like the "C" locale's toupper AND additionally
// case-folds the accented Latin-1-supplement bytes (0x80-0xFF) per the
// CP-1252 codepage's own case table -- this is a deterministic published
// Windows-1252 uppercase mapping (corrected from a prior comment here that
// overstated it as having "no authoritative source"; the table below encodes
// it directly, one row per byte, self-documenting and independently checkable
// against any CP-1252 case-fold reference). Plan 4a Task 3 Step 2 makes this
// total: the 256-entry table folds every byte exactly like real
// CharUpperBuffA, including CP-1252's three out-of-band Latin Extended-A
// pairs (0x9A/0x9C/0x9E) and the one pair that crosses the C0/C1-vs-Latin-1-
// Supplement block boundary (0xFF->0x9F), and preserves CP-1252's three
// genuine no-uppercase-exists bytes (0xB5 micro sign, 0xDF sharp s, 0xF7
// division sign) unchanged.
static const unsigned char g_cp1252UpperFold[256] = {
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
    0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F,
    0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28,0x29,0x2A,0x2B,0x2C,0x2D,0x2E,0x2F,
    0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F,
    0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4A,0x4B,0x4C,0x4D,0x4E,0x4F,
    0x50,0x51,0x52,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5A,0x5B,0x5C,0x5D,0x5E,0x5F,
    0x60,
    /* 'a'..'z' (0x61-0x7A) -> -0x20 */
    0x41,0x42,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4A,0x4B,0x4C,0x4D,
    0x4E,0x4F,0x50,0x51,0x52,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5A,
    0x7B,0x7C,0x7D,0x7E,0x7F,
    0x80,0x81,0x82,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
    0x8A,  // 0x9A folds here; identity for 0x8A itself
    0x8B,
    0x8C,  // 0x9C folds here; identity for 0x8C itself
    0x8D,
    0x8E,  // 0x9E folds here; identity for 0x8E itself
    0x8F,
    0x90,0x91,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,
    0x8A,  // 0x9A: s-caron -> S-caron
    0x9B,
    0x8C,  // 0x9C: oe -> OE
    0x9D,
    0x8E,  // 0x9E: z-caron -> Z-caron
    0x9F,  // 0x9F: Y-diaeresis, already uppercase (identity)
    0xA0,0xA1,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7,0xA8,0xA9,0xAA,0xAB,0xAC,0xAD,0xAE,0xAF,
    0xB0,0xB1,0xB2,0xB3,0xB4,
    0xB5,  // micro sign: unchanged, not a cased letter
    0xB6,0xB7,0xB8,0xB9,0xBA,0xBB,0xBC,0xBD,0xBE,0xBF,
    0xC0,0xC1,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,0xCA,0xCB,0xCC,0xCD,0xCE,0xCF,
    0xD0,0xD1,0xD2,0xD3,0xD4,0xD5,0xD6,0xD7,0xD8,0xD9,0xDA,0xDB,0xDC,0xDD,0xDE,
    0xDF,  // sharp s: unchanged, CP-1252 has no uppercase eszett
    /* 0xE0..0xFE -> -0x20 (Latin-1 Supplement lowercase accented run),
       EXCEPT 0xF7 (division sign, not a letter -- unchanged) */
    0xC0,0xC1,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,0xCA,0xCB,0xCC,0xCD,0xCE,0xCF,
    0xD0,0xD1,0xD2,0xD3,0xD4,0xD5,0xD6,
    0xF7,  // division sign: unchanged
    0xD8,0xD9,0xDA,0xDB,0xDC,0xDD,0xDE,
    0x9F,  // 0xFF: y-diaeresis -> Y-diaeresis (crosses to the C1 block)
};

inline void CharUpperBuff(char* s, int len) {
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        s[i] = (char)g_cp1252UpperFold[c];
    }
}

// R9 (Plan 3 Task 4): CharUpper -- ccomp.cpp's bIsMaskCompare (the WHO-filter
// mask/nickname comparator lifted for query.cpp's PRUSERMATCH support) calls
// Win32's single-character CharUpper(LPTSTR) idiom: the character is packed
// into the pointer's low byte via MAKELONG(ch, 0) (a real, if odd, Win32
// convention for the "one-character-in, one-character-out" overload), and
// CharUpper returns the same packing with the byte case-folded. UNLIKE
// CharUpperBuff above (which Plan 4a Task 3 gave the full CP-1252 fold table),
// this overload is still ASCII-only (0x00-0x7F only; bytes >= 0x80 pass
// through unchanged) -- out of Task 3's stated scope (that task's brief names
// CharUpperBuff specifically; bIsMaskCompare's WHO-filter input domain is
// plain-ASCII nick masks in every existing selftest, so this is not yet a
// known live bug, just an intentionally-deferred asymmetry). Flagged here: if
// a future caller needs accented-byte CharUpper fidelity, reuse
// g_cp1252UpperFold (defined above CharUpperBuff) instead of re-deriving it.
inline LPTSTR CharUpper(LPTSTR lpsz) {
    uintptr_t packed = (uintptr_t)lpsz;
    if (packed <= 0xFFFFu) {
        unsigned char c = (unsigned char)(packed & 0xFF);
        if (c <= 0x7F) c = (unsigned char)toupper(c);
        return (LPTSTR)(uintptr_t)c;
    }
    // Real multi-char-string overload (Win32 also allows CharUpper(LPTSTR) to
    // uppercase a NUL-terminated buffer in place when the pointer is a real
    // string, not a packed character) -- ccomp.cpp only ever uses the packed-
    // character form, but keeping this arm makes the shim total rather than
    // silently wrong if a future caller passes a real string pointer.
    for (char* p = lpsz; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c <= 0x7F) *p = (char)toupper(c);
    }
    return lpsz;
}

// --- Win32 codepage-conversion NLS surface (rule R9; Plan 3 Task 4;
//     ircproto.cpp's EncodeNick/DecodeNick/EncodeChan/DecodeChan/DecodeString/
//     EncodeString -- the FIRST lifted functions in this port to call the raw
//     Win32 MultiByteToWideChar/WideCharToMultiByte pair directly; Task 2's
//     ccommon_str.cpp lift works entirely on wide-char buffers the CALLER
//     already produced via these APIs, so this shim didn't exist yet). This
//     port's permanent posture (Task 2) is single-byte CP-1252 (GetACP()'s
//     one and only return value here); the wide encoding side is WCHAR ==
//     uint16_t == UTF-16 code units (never surrogate pairs, since CP-1252 has
//     no codepoint above U+FFFF -- see WCHAR's own typedef comment above).
//     MB_PRECOMPOSED is a real Win32 flag (composed rather than decomposed
//     Unicode forms) with no distinct effect on a single-byte->UTF-16
//     expansion of a codepage with no combining sequences, so it's a no-op
//     flag value here, kept only so call sites compile unchanged.
#define CP_ACP 0
#define MB_PRECOMPOSED 0x00000001
inline UINT GetACP() { return CP_ACP; }

// CP-1252's C1-control-block remap (0x80-0x9F): the only rows where CP-1252
// diverges from plain Latin-1/ISO-8859-1. Values from the Unicode
// Consortium's published CP-1252 mapping table (the same authoritative
// source real Win32's kernel32 codepage tables encode). 0x81/0x8D/0x8F/0x90/
// 0x9D have no assigned character in real CP-1252 -- Win32's own
// MultiByteToWideChar maps these five to U+0081/U+008D/U+008F/U+0090/U+009D
// (i.e. passes them through as C1 control codes, matching ISO-8859-1) rather
// than failing the call; reproduced exactly (verified against the published
// table, not guessed -- unlike mfc_compat.h's CharUpperBuff gap above, this
// table IS complete and authoritative for the full byte range).
inline WCHAR cc_cp1252_to_unicode(unsigned char b) {
    static const WCHAR tbl[32] = {
        0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
        0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
    };
    if (b >= 0x80 && b <= 0x9F) return tbl[b - 0x80];
    return (WCHAR)b;  // 0x00-0x7F and 0xA0-0xFF: identical to Unicode code point
}
// Inverse of the table above (Unicode code point -> CP-1252 byte). Returns
// '?' (0x3F, real Win32's WideCharToMultiByte default "unmappable" fallback
// char with no lpUsedDefaultChar wired) for a code point this codepage
// cannot represent at all (never reached by this port's own CP-1252 round
// trips, since every wide string passed in came FROM cc_cp1252_to_unicode
// above -- only relevant if fed non-CP-1252-origin UTF-16, e.g. UTF-8-decoded
// wide text, which the codec functions this shim serves already handle on
// their own path independent of these two conversion functions).
inline unsigned char cc_unicode_to_cp1252(WCHAR w) {
    if (w <= 0x7F) return (unsigned char)w;
    if (w >= 0xA0 && w <= 0xFF) return (unsigned char)w;
    static const struct { WCHAR u; unsigned char b; } rev[] = {
        {0x20AC,0x80},{0x0081,0x81},{0x201A,0x82},{0x0192,0x83},{0x201E,0x84},
        {0x2026,0x85},{0x2020,0x86},{0x2021,0x87},{0x02C6,0x88},{0x2030,0x89},
        {0x0160,0x8A},{0x2039,0x8B},{0x0152,0x8C},{0x008D,0x8D},{0x017D,0x8E},
        {0x008F,0x8F},{0x0090,0x90},{0x2018,0x91},{0x2019,0x92},{0x201C,0x93},
        {0x201D,0x94},{0x2022,0x95},{0x2013,0x96},{0x2014,0x97},{0x02DC,0x98},
        {0x2122,0x99},{0x0161,0x9A},{0x203A,0x9B},{0x0153,0x9C},{0x009D,0x9D},
        {0x017E,0x9E},{0x0178,0x9F},
    };
    for (auto &e : rev) if (e.u == w) return e.b;
    return (unsigned char)'?';
}

// MultiByteToWideChar(CP-1252 -> UTF-16): cchWideChar == -1 means szInStr is
// NUL-terminated and the returned count includes the terminating NUL (Win32's
// documented convention); a non-negative cchWideChar instead means "convert
// exactly this many source bytes" and the return value is the number of wide
// chars written (no implied NUL). dwFlags (MB_PRECOMPOSED) is accepted but
// ignored (see header note above).
inline int MultiByteToWideChar(UINT /*codepage*/, DWORD /*dwFlags*/,
                                LPCSTR szInStr, int cchIn,
                                LPWSTR wszOut, int cchWideCharBufSize) {
    if (!szInStr) return 0;
    int srcLen = (cchIn == -1) ? (int)strlen(szInStr) + 1 : cchIn;
    if (cchWideCharBufSize == 0) return srcLen;  // size-query mode
    int n = (srcLen < cchWideCharBufSize) ? srcLen : cchWideCharBufSize;
    for (int i = 0; i < n; i++)
        wszOut[i] = cc_cp1252_to_unicode((unsigned char)szInStr[i]);
    return n;
}

// WideCharToMultiByte(UTF-16 -> CP-1252 or US_CODEPAGE, both single-byte on
// this port -- see DecodeChan's `codepage` local, ircproto.cpp): cchWideChar
// == -1 means wszInStr is NUL-terminated (returned count includes the NUL).
// lpDefaultChar/lpUsedDefaultChar are never used by any lifted caller (all
// pass NULL) so they're accepted but ignored, matching Win32's own optional-
// parameter contract.
inline int WideCharToMultiByte(UINT /*codepage*/, DWORD /*dwFlags*/,
                                LPCWSTR wszInStr, int cchIn,
                                LPSTR szOut, int cbMultiByteBufSize,
                                LPCSTR /*lpDefaultChar*/, BOOL* /*lpUsedDefaultChar*/) {
    if (!wszInStr) return 0;
    int srcLen = 0;
    if (cchIn == -1) { while (wszInStr[srcLen]) srcLen++; srcLen++; }
    else srcLen = cchIn;
    if (cbMultiByteBufSize == 0) return srcLen;  // size-query mode
    int n = (srcLen < cbMultiByteBufSize) ? srcLen : cbMultiByteBufSize;
    for (int i = 0; i < n; i++)
        szOut[i] = (char)cc_unicode_to_cp1252(wszInStr[i]);
    return n;
}

// --- GetStringTypeEx / character classification (rule R9; Plan 3 Task 4;
//     ircproto.cpp's DecodeNickForScreen -- scans a decoded nickname for any
//     control/blank character to decide whether to wrap it in quotes for
//     display). CT_CTYPE1 is the only ctype real Win32 callers here ever
//     request; C1_CNTRL/C1_BLANK are the only two bits DecodeNickForScreen
//     tests. GetUserDefaultLCID's actual locale value is never inspected by
//     that call site (only threaded through opaquely), so a fixed sentinel
//     is exact.
#define CT_CTYPE1 1
#define C1_CNTRL  0x0002
#define C1_BLANK  0x0004
typedef DWORD LCID;
inline LCID GetUserDefaultLCID() { return 0x0409; /* en-US, arbitrary sentinel */ }
inline BOOL GetStringTypeEx(LCID /*lcid*/, DWORD dwInfoType, LPCSTR sz, int cch, WORD* out) {
    if (dwInfoType != CT_CTYPE1) { ASSERT(0); return FALSE; }
    for (int i = 0; i < cch; i++) {
        unsigned char c = (unsigned char)sz[i];
        WORD w = 0;
        if (c < 0x20 || c == 0x7F) w |= C1_CNTRL;
        if (c == ' ' || c == '\t') w |= C1_BLANK;
        out[i] = w;
    }
    return TRUE;
}

// --- wsprintf (rule R9; ircproto.cpp's DecodeNickForScreen quoting) --------
// Win32's wsprintfA is a constrained sprintf (1024-byte output cap, no
// floating-point specifiers) -- DecodeNickForScreen's only use
// (`wsprintf(szBufOut, "\"%s\"", pszDecodedNick)`) is a plain %s substitution
// well within any real implementation's limits, so a direct vsnprintf-backed
// forward is behavior-identical for every reachable call site.
inline int wsprintf(char* out, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(out, 1024, fmt, ap);  // 1024: real wsprintfA's own output cap
    va_end(ap);
    return n;
}

// --- MFC exception-handling macros (rule R9; used by lifted avbfile.cpp /
//     avatario.cpp around `new` calls that could throw). MFC's TRY/CATCH_ALL
//     wrap try/catch(CException*); we have no CException hierarchy, so these
//     become plain try/catch(...) blocks - any thrown exception (e.g.
//     std::bad_alloc from `new`) is treated the same as MFC's CATCH_ALL. ---
#define TRY try
#define CATCH_ALL(e) catch (...)
#define END_CATCH_ALL

// cc_canvas.h (CCanvas) only needs the ASSERT macro just defined above from
// this file -- see the top-of-file comment on why this is deferred here
// rather than included alongside comicchat.h.
#include "cc_canvas.h"
// bridge_decode_dib_to_rgba (bridge_art.cpp) -- CDC::StretchDIBits reuses
// Plan 1's palettized-DIB-to-RGBA converter (R14 header; task brief).
#include "bridge_art.h"

// ccContext()/CCEngineContext live in engine_context.h, which itself
// #includes this file for CString -- an #include here would be a genuine
// circular include (unlike cc_canvas.h above, engine_context.h needs types
// defined throughout this file, not just one early macro). Forward-declare
// instead, matching engine_context.h's own forward declaration of cc_canvas.
struct CCEngineContext;
CCEngineContext& ccContext();


// --- minimal CObject ---------------------------------------------------------
class CObject {
public:
    virtual ~CObject() {}
};

// --- DECLARE_DYNAMIC (rule R9; Plan 2 Task 6, panel.h's CDamage). MFC's
//     DECLARE_DYNAMIC(cls) declares RTTI machinery (a static CRuntimeClass +
//     GetRuntimeClass()/IsKindOf()). panel.h's CDamage (an UpdateAllViews
//     hint object) is the only lifted class using it, and no lifted code
//     ever calls RUNTIME_CLASS/IsKindOf on it -- so a no-op expansion (which
//     leaves CDamage a plain class) is exact for this port. Selftested in
//     cc_selftest_balloon(). ---------------------------------------------------
#define DECLARE_DYNAMIC(class_name)
// R9 (Plan 2 Task 8): MFC's IMPLEMENT_DYNAMIC(class, base) defines the
// CRuntimeClass machinery that DECLARE_DYNAMIC declares. panel.cpp:29 has
// IMPLEMENT_DYNAMIC(CDamage, CObject) at file scope. Since DECLARE_DYNAMIC is a
// no-op here (CDamage's RTTI is never queried -- it's only used as an
// UpdateAllViews hint inside the R11-wrapped RefreshPanelN), the matching
// IMPLEMENT_DYNAMIC is a no-op too. Selftested in cc_selftest_panel (CDamage
// constructs/destructs cleanly). The trailing ';' in the original source
// terminates the (now empty) expansion.
#define IMPLEMENT_DYNAMIC(class_name, base_class)

// --- Win32 string-resource table (rule R9; Plan 2 Task 9, textpose.cpp's
//     InitializeEmotionRules -> CString::LoadString(ruleIDs[i])). Real MFC/
//     Win32 loads STRINGTABLE entries out of the .exe's compiled resource
//     segment by numeric ID; this port has no resource compiler (R6: resource
//     loading is out of scope), so the table is a plain registry a caller
//     populates explicitly instead of the linker populating it implicitly.
//     RegisterStringResource is idempotent-by-overwrite (last write for a
//     given id wins) so re-registration (e.g. between selftest runs) is safe.
//     The only current populator is engine_context.cpp's one-time seeding of
//     the ID_RULE_* rule-table strings (R17 — see engine_context.h) -- LoadString
//     itself is fully generic and knows nothing about that; a lookup miss
//     matches real LoadString's contract (returns FALSE, leaves the CString
//     untouched by convention -- callers here follow suit).
void RegisterStringResource(UINT id, const char* value);
const char* LookupStringResource(UINT id); // nullptr if unregistered

// --- CString (byte-oriented, MFC-flavored subset) ----------------------------
class CString {
public:
    CString() {}
    CString(const char* s) : m_s(s ? s : "") {}
    // R9 (Plan 3 Task 3): MFC's CString(LPCTSTR lpch, int nLength) builds a
    // string from an explicit byte count rather than a NUL terminator --
    // protsupp.cpp's key-string codec uses it to slice a substring out of a
    // larger buffer (GetValueFromKeyString: `CString(pszKeyString + nValPos,
    // nNextValPos - nValPos)`). A negative/zero length yields an empty string
    // (matches MFC: the real CString ASSERTs nLength>=0, but every lifted call
    // site here always passes a non-negative difference of two positions
    // within the same string, so no other behavior is reachable).
    CString(const char* s, int len) : m_s(s && len > 0 ? std::string(s, (size_t)len) : std::string()) {}
    CString(const CString&) = default;
    CString& operator=(const CString&) = default;
    CString& operator=(const char* s) { m_s = s ? s : ""; return *this; }

    operator LPCSTR() const { return m_s.c_str(); }
    int GetLength() const { return (int)m_s.size(); }
    BOOL IsEmpty() const { return m_s.empty(); }
    void Empty() { m_s.clear(); }
    char GetAt(int i) const { return m_s[(size_t)i]; }

    CString& operator+=(const char* s) { m_s += (s ? s : ""); return *this; }
    CString& operator+=(const CString& s) { m_s += s.m_s; return *this; }
    CString& operator+=(char c) { m_s += c; return *this; }
    friend CString operator+(const CString& a, const CString& b) { CString r(a); r += b; return r; }
    friend bool operator==(const CString& a, const char* b) { return a.m_s == (b ? b : ""); }
    friend bool operator==(const CString& a, const CString& b) { return a.m_s == b.m_s; }
    friend bool operator!=(const CString& a, const char* b) { return !(a == b); }

    void Format(const char* fmt, ...) {
        va_list ap; va_start(ap, fmt);
        char buf[4096];
        vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        m_s = buf;
    }
    CString Left(int n) const { return CString(m_s.substr(0, (size_t)n).c_str()); }
    CString Mid(int i) const {
        int len = (int)m_s.size();
        if (i < 0) i = 0;
        if (i >= len) return CString("");
        return CString(m_s.substr((size_t)i).c_str());
    }
    CString Mid(int i, int n) const {
        int len = (int)m_s.size();
        if (i < 0) i = 0;
        if (i >= len) return CString("");
        if (n < 0) n = 0;
        int remaining = len - i;
        if (n > remaining) n = remaining;
        return CString(m_s.substr((size_t)i, (size_t)n).c_str());
    }
    int Find(char c) const { auto p = m_s.find(c); return p == std::string::npos ? -1 : (int)p; }
    int Find(const char* s) const { auto p = m_s.find(s); return p == std::string::npos ? -1 : (int)p; }
    void MakeUpper() { for (auto& c : m_s) c = (char)toupper((unsigned char)c); }
    void MakeLower() { for (auto& c : m_s) c = (char)tolower((unsigned char)c); }
    int CompareNoCase(const char* s) const { return strcasecmp(m_s.c_str(), s ? s : ""); }

    // R9 (Plan 2 Task 9): MFC's CString::LoadString(UINT nID) loads a
    // STRINGTABLE resource by ID, returning TRUE and replacing the string's
    // content on success, FALSE (content left UNCHANGED, matching real MFC)
    // on a missing resource ID. Backed by the registry above.
    BOOL LoadString(UINT id) {
        const char* v = LookupStringResource(id);
        if (v == nullptr) return FALSE;
        m_s = v;
        return TRUE;
    }

private:
    std::string m_s;
};

// --- geometry classes ---------------------------------------------------------
class CPoint : public POINT {
public:
    CPoint() { x = y = 0; }
    CPoint(LONG px, LONG py) { x = px; y = py; }
};
class CSize : public SIZE {
public:
    CSize() { cx = cy = 0; }
    CSize(LONG w, LONG h) { cx = w; cy = h; }
};
class CRect : public RECT {
public:
    CRect() { left = top = right = bottom = 0; }
    CRect(LONG l, LONG t, LONG r, LONG b) { left = l; top = t; right = r; bottom = b; }
    LONG Width() const { return right - left; }
    LONG Height() const { return bottom - top; }
    void SetRectEmpty() { left = top = right = bottom = 0; }
    BOOL IsRectEmpty() const { return Width() <= 0 || Height() <= 0; }
};

// --- rendering types (Plan 2 Task 3) -----------------------------------------
// Plan 1 (rule R4) left CDC as a pointer-only forward declaration while
// drawing bodies stayed disabled behind CC_NO_RENDER. Plan 2 replaces that
// forward declaration with a concrete CDC: a thin adapter translating GDI
// calls 1:1 onto the cc_canvas vtable (Task 2), plus the small family of GDI
// object types (CFont/CPen/CBrush) and structs (LOGFONT/TEXTMETRIC) lifted
// code selects into it. See the task brief's "Adapter semantics" block for
// the exact window-origin / clip-stack / path-accumulation contract this
// class implements; every method here is exercised by cc_selftest_dc().
//
// CPalette itself stays a pointer-only opaque type (rule R4, unchanged from
// Plan 1) -- every call site that would touch it goes through the palette
// no-op free functions further below (R14(ii): RGBA end-to-end, no real
// palette).
class CPalette;

// R8 (Plan 2 Task 5): CRichEditCtrl and CHARFORMAT stay pointer-only opaque
// types -- format.h declares PRGDWGetFormatting(CRichEditCtrl*, ...) and
// bLOGFONTToCHARFORMAT(..., CHARFORMAT*) so their signatures must still
// name real types, but every function body that dereferences them
// (PRGDWGetFormatting, bLOGFONTToCHARFORMAT, MatchFont) is whole-function
// R11-wrapped under CC_NO_UI in format.cpp -- no lifted code outside those
// wrapped bodies ever needs a real member of either type.
class CRichEditCtrl;
struct CHARFORMAT;

// --- GDI constants (rule R9; CDC adapter + selftest) -------------------------
#define OPAQUE      2
#define TRANSPARENT 1
#define MM_TWIPS    9
#define PS_SOLID    0
#define RGN_COPY    5
#define LOGPIXELSX  88
#define LOGPIXELSY  90
// DrawTextEx flags (rule R9; Plan 4a Task 7): real Win32 bit values, needed
// only so CStarLabel::Draw's un-wrapped call site (balloon.cpp) still compiles
// with its original flags expression -- CDC::DrawTextEllipsis (below) takes
// the combined UINT opaquely and never decodes individual bits, so exact
// values matter only for documentation fidelity with the original constants.
#define DT_LEFT          0x00000000
#define DT_SINGLELINE    0x00000020
#define DT_NOPREFIX      0x00000800
#define DT_END_ELLIPSIS  0x00008000
// SRCCOPY already defined above (Plan 1, dib.h default-arg compatibility).
// StretchDIBits only supports SRCCOPY in this adapter (R14(i)); any other
// rop is a forced-explicit-transformation site (ASSERT(0) — ports that need
// a different rop must add one deliberately, not silently inherit it here).

// --- ccommon.h constants (rule R9; format.cpp's #include "ccommon.h" is
//     deleted per R8 -- ccommon.h itself is not scheduled for any lift, it
//     only ever supplied string-processing constants + externs for files
//     outside this plan's scope. format.cpp uses exactly three of its
//     constants (g_chEOS as the string terminator sentinel in the control-
//     byte scanners, g_chComma in nFillFormatting's ^cWX,YZ construction,
//     g_chTransparent as the fill byte for "invisible" runs in
//     GetFormattedTextExtent's transparency case) -- reproduced verbatim
//     from artifacts/inc/ccommon.h's const TCHAR definitions. ---------------
const TCHAR g_chEOS         = _T('\0');
const TCHAR g_chComma       = _T(',');
const TCHAR g_chTransparent = 0x01;

// --- LOGFONT / TEXTMETRIC (twips; rule R9, CFont/CDC::GetTextMetrics) -------
#define LF_FACESIZE 32
struct LOGFONT {
    LONG lfHeight;
    LONG lfWidth;
    LONG lfEscapement;
    LONG lfOrientation;
    LONG lfWeight;
    BYTE lfItalic;
    BYTE lfUnderline;
    BYTE lfStrikeOut;
    BYTE lfCharSet;
    // R9 (Plan 2 Task 5): format.cpp's GetFormattedTextExtent toggles
    // FIXED_PITCH/VARIABLE_PITCH bits on this field when a run's formatting
    // switches to/from a fixed-pitch or symbol face.
    BYTE lfPitchAndFamily;
    char lfFaceName[LF_FACESIZE];
};
// R9 (Plan 2 Task 5): lfPitchAndFamily bit values, matching Win32's
// <wingdi.h> exactly (low nibble = pitch, FIXED_PITCH/VARIABLE_PITCH are
// mutually exclusive bits within it).
#define FIXED_PITCH    0x01
#define VARIABLE_PITCH 0x02
struct TEXTMETRIC {
    LONG tmHeight;
    LONG tmAscent;
    LONG tmDescent;
    LONG tmInternalLeading;
    LONG tmExternalLeading;
    LONG tmAveCharWidth;
    LONG tmMaxCharWidth;
    // R9 (Plan 2 Task 6): fonts.cpp's SetFonts reads tm.tmCharSet (line 55/77)
    // to decide font-substitution and Far-East italic suppression. CDC::
    // GetTextMetrics fills it from the selected font's lfCharSet (the metrics
    // canvas has no real font engine to report an actual physical charset).
    BYTE tmCharSet;
};

// R9 (Plan 2 Task 6): Win32 <wingdi.h> charset constants used by fonts.cpp's
// SetFonts (Far-East-italic test) and balloon.cpp's Capitalize (live since
// Plan 3 Task 2 -- see that function). Values are Win32's exact <wingdi.h> definitions.
// DEFAULT_CHARSET (1) is already defined further down for EnumFontFamiliesEx.
#define ANSI_CHARSET        0
#define GREEK_CHARSET       161
#define TURKISH_CHARSET     162
#define BALTIC_CHARSET      186
#define RUSSIAN_CHARSET     204
// R9 (Plan 2 Task 6): DEFAULT_PITCH (low-nibble pitch value 0) -- balloon.cpp's
// iDrawFormattedTextLine sets lfPitchAndFamily's pitch to DEFAULT_PITCH for a
// symbol-font run. FIXED_PITCH(1)/VARIABLE_PITCH(2) are defined below.
#define DEFAULT_PITCH       0

// --- HDC / GetDC / ReleaseDC (rule R9; format.cpp's nGetSpecialFontIndex --
//     the mandatory GetFormattedTextExtent-reachable function that probes
//     for fixed-pitch/symbol system fonts via EnumFontFamiliesEx). This port
//     has no real Win32 GDI device context; HDC is an opaque non-null
//     sentinel token, exactly like CDC::GetSafeHdc()'s existing convention.
//     GetDC(NULL)/ReleaseDC(NULL, hdc) query/release the (nonexistent)
//     screen DC in the original -- here they just hand back/accept the
//     sentinel so the caller's !hdc check and later ReleaseDC call both
//     behave exactly like the real API would on a live display. -------------
typedef void* HWND;
typedef void* HDC;
inline HDC GetDC(HWND) { static int sentinel; return &sentinel; }
inline void ReleaseDC(HWND, HDC) {}

// --- Font-enumeration types + EnumFontFamiliesEx (rule R9; format.cpp's
//     nGetSpecialFontIndex/EnumFontFamExProc). This headless port has no
//     real font catalog to enumerate, so EnumFontFamiliesEx always reports
//     "no font family matched" -- it never invokes the callback and returns
//     a value the callback itself would never return (matches Win32's own
//     documented behavior when a face name has zero installed fonts: the
//     original's `iRet = EnumFontFamiliesEx(...)` compares `iRet == 0` to
//     detect "callback found and stopped enumeration", so any nonzero
//     sentinel here reproduces the honest "not found" outcome without
//     fabricating font presence). LPARAM matches Win32's pointer-sized
//     signed integer; CALLBACK is __stdcall on Win32 and empty here (this
//     port has one calling convention). --------------------------------------
typedef intptr_t LPARAM;
#define CALLBACK
#define FW_NORMAL 400
#define DEFAULT_CHARSET 1
struct ENUMLOGFONTEX { LOGFONT elfLogFont; char elfFullName[LF_FACESIZE]; char elfStyle[LF_FACESIZE]; };
struct NEWTEXTMETRICEX { TEXTMETRIC ntmTm; };
typedef int (CALLBACK *FONTENUMPROC)(ENUMLOGFONTEX*, NEWTEXTMETRICEX*, int, LPARAM);
inline int EnumFontFamiliesEx(HDC, LOGFONT*, FONTENUMPROC, LPARAM, DWORD) {
    return 1;  // no matching font family found; callback never invoked
}

// --- CFont (rule R9; CDC::SelectObject/GetCurrentFont, format.cpp-style
//     per-run CreateFontIndirect callers in a later task) -------------------
class CFont {
public:
    LOGFONT m_lf{};
    BOOL CreateFontIndirect(const LOGFONT* lf) {
        m_lf = *lf;
        return TRUE;
    }
    // R9 (Plan 2 Task 5): format.cpp's GetFormattedTextExtent reads the
    // currently-selected font back out via pOldFont->GetLogFont(&logFontOld)
    // before iterating per-run overrides -- exact inverse of
    // CreateFontIndirect above (which itself stores m_lf = *lf).
    void GetLogFont(LOGFONT* lf) const { *lf = m_lf; }
    // R9 (Plan 2 Task 5): format.cpp calls fontTmp.DeleteObject() after each
    // per-run SelectObject/GetTextExtent round-trip. MFC's CGdiObject::
    // DeleteObject() frees the underlying GDI handle; this CFont never holds
    // a real GDI handle (see CreateFontIndirect above -- it's a LOGFONT
    // value holder), so this is a no-op, matching this shim's established
    // posture for CPen/CBrush (also handle-less value holders).
    BOOL DeleteObject() { return TRUE; }
    // Translates this LOGFONT into the cc_canvas boundary's font_spec (Plan 2
    // spec Sec4.3, comicchat.h). Twips pass through unchanged (R14 header).
    cc_font_spec spec() const {
        cc_font_spec s;
        memset(&s, 0, sizeof(s));
        strncpy(s.face, m_lf.lfFaceName, sizeof(s.face) - 1);
        s.height = (int32_t)m_lf.lfHeight;
        s.weight = (int32_t)m_lf.lfWeight;
        s.italic = m_lf.lfItalic ? 1 : 0;
        s.underline = m_lf.lfUnderline ? 1 : 0;
        s.strikeout = m_lf.lfStrikeOut ? 1 : 0;
        s.charset = m_lf.lfCharSet;
        return s;
    }
};

// --- CPen (rule R9; CDC::SelectObject, StrokePath/StrokeAndFillPath) --------
class CPen {
public:
    int32_t width = 1;
    COLORREF color = 0;
    CPen() {}
    CPen(int /*style*/, int w, COLORREF c) { CreatePen(0, w, c); }
    BOOL CreatePen(int /*style*/, int w, COLORREF c) {
        width = w;
        color = c;
        return TRUE;
    }
};

// --- CBrush (rule R9; CDC::SelectObject, StrokeAndFillPath/Ellipse fill) ----
class CBrush {
public:
    COLORREF color = 0;
    CBrush() {}
    BOOL CreateSolidBrush(COLORREF c) {
        color = c;
        return TRUE;
    }
};

// --- CBitmap (rule R9; CDC::SelectObject(CBitmap*) inventory completeness --
//     no lifted call site exercises bitmap selection yet; kept as an opaque
//     placeholder so the SelectObject overload set compiles.) ---------------
class CBitmap {};

// R9 (Plan 2 Task 7): CDC::DrawPoseImage takes CDIB*/mask-CDIB* pointers (the
// CBody draw path, bodycam.cpp). Pointer-only here; the method body (defined
// out-of-line in mfc_compat.cpp) needs the full CDIB definition and #includes
// dib.h there. Every other CDIB use in lifted code already #includes dib.h
// directly.
class CDIB;

// --- CDC (Plan 2 Task 3) -----------------------------------------------------
// Adapter over cc_canvas (Task 2). One CDC wraps exactly one cc_canvas* for
// its lifetime; every drawing/measurement call forwards to that canvas via
// CCanvas, translating GDI's window-origin / clip-region / path-accumulation
// state (which cc_canvas's vtable itself knows nothing about) into the
// vtable's flat coordinate + immediate-path calls. See the task brief's
// "Adapter semantics" block for the exact contract; cc_selftest_dc() locks
// every behavior enumerated below.
class CDC {
public:
    BOOL m_bPrinting = FALSE;
    // R9 (Plan 2 Task 6): MFC's CDC exposes a public m_hDC handle. balloon.cpp
    // passes pdc->m_hDC to FindSubStringForINTLThatFits (dead INTL path, stub)
    // and DrawTextEx (CStarLabel::Draw, R11-wrapped render). HDC is an opaque
    // non-null token in this port (same convention as GetSafeHdc); it's only
    // ever handed to a stub or a wrapped-out call, never dereferenced.
    HDC m_hDC = (HDC)this;

    explicit CDC(cc_canvas* canvas) : canvas_(canvas) {}

    // --- object selection (SelectObject returns the previously-selected
    //     object of the same kind, MFC style) --------------------------------
    CFont* SelectObject(CFont* f) { CFont* old = curFont_; curFont_ = f; return old; }
    CPen*  SelectObject(CPen* p)  { CPen* old = curPen_; curPen_ = p; return old; }
    CBrush* SelectObject(CBrush* b) { CBrush* old = curBrush_; curBrush_ = b; return old; }
    CBitmap* SelectObject(CBitmap* bm) { CBitmap* old = curBitmap_; curBitmap_ = bm; return old; }

    CFont* GetCurrentFont() const { return curFont_; }

    // --- text ----------------------------------------------------------------
    CSize GetTextExtent(const char* s, int len) const {
        cc_font_spec f = currentFontSpec();
        int32_t w = 0, h = 0;
        canvasWrap().measure_text(&f, s, len, &w, &h);
        return CSize(w, h);
    }
    // R9 (Plan 2 Task 6): returns BOOL (was void) so fonts.cpp's
    // VERIFY(pDc->GetTextMetrics(&tm)) / balloon.cpp's VERIFY(...) compile --
    // MFC's CDC::GetTextMetrics returns BOOL (success). Always succeeds here.
    // Also fills tmCharSet from the selected font's lfCharSet (the metrics
    // canvas reports no physical charset; the requested charset is the honest
    // stand-in, and matches fonts.cpp's "lfCharSet == tm.tmCharSet" no-op
    // expectation when the requested font is available).
    BOOL GetTextMetrics(TEXTMETRIC* tm) const {
        cc_font_spec f = currentFontSpec();
        cc_text_metrics m;
        memset(&m, 0, sizeof(m));
        canvasWrap().font_metrics(&f, &m);
        tm->tmHeight = m.height;
        tm->tmAscent = m.ascent;
        tm->tmDescent = m.descent;
        tm->tmInternalLeading = m.internal_leading;
        tm->tmExternalLeading = m.external_leading;
        tm->tmAveCharWidth = m.ave_char_width;
        tm->tmMaxCharWidth = m.max_char_width;
        tm->tmCharSet = f.charset;
        return TRUE;
    }
    // R9 (Plan 2 Task 6): fonts.cpp reads the *physical* face name of the
    // currently-selected font (GetTextFace) to gate the "Comic Sans MS"
    // vertical-kerning tweak. The metrics canvas has no font substitution, so
    // the physical face equals the requested face. Copies it into buf (up to
    // count chars incl. NUL) and returns its length, matching Win32's
    // GetTextFace return value.
    int GetTextFace(int count, char* buf) const {
        cc_font_spec f = currentFontSpec();
        if (count <= 0 || !buf) return 0;
        strncpy(buf, f.face, (size_t)count - 1);
        buf[count - 1] = '\0';
        return (int)strlen(buf);
    }
    COLORREF SetTextColor(COLORREF c) { COLORREF old = textColor_; textColor_ = c; return old; }
    int SetBkMode(int mode) { int old = bkMode_; bkMode_ = mode; return old; }
    COLORREF SetBkColor(COLORREF c) { COLORREF old = bkColor_; bkColor_ = c; return old; }
    void TextOut(int x, int y, const char* s, int len) {
        cc_font_spec f = currentFontSpec();
        int32_t lx, ly;
        toLogical(x, y, lx, ly);
        canvasWrap().draw_text(&f, lx, ly, textColor_, bkMode_ == OPAQUE ? 1 : 0,
                                bkColor_, s, len);
    }

    // R9 (Plan 4a Task 7): adapter for CStarLabel::Draw's
    // DrawTextEx(pdc->m_hDC, sz, -1, &rect, DT_LEFT|DT_NOPREFIX|DT_SINGLELINE|
    // DT_END_ELLIPSIS, NULL) -- a Win32 single-line ellipsized-draw idiom with
    // no 1:1 cc_canvas op (R14: unmappable GDI idiom; measure_text/draw_text
    // only draw a caller-supplied byte run, they don't fit-and-truncate).
    // Built over the two ops that DO exist: measure_text (binary-search the
    // longest prefix that, with "..." appended, fits the box width) then
    // draw_text (the actual paint, left-justified, single line, transparent
    // background -- flags is accepted but not decoded: every caller in this
    // port passes exactly DT_LEFT|DT_NOPREFIX|DT_SINGLELINE|DT_END_ELLIPSIS,
    // and there is no other combination to honor).
    //
    // Box-top alignment: draw_text's `y` is the GDI TA_TOP box-top (see
    // comicchat.h's doc comment on cc_canvas_ops::draw_text) -- the same
    // convention TextOut(rect->left, rect->top, ...) above already uses, so
    // this draws at the box's top-left corner, matching DrawTextEx's default
    // (no DT_VCENTER/DT_BOTTOM) top alignment.
    //
    // sz is NOT required to be NUL-terminated at exactly its fitting point --
    // it must be a NUL-terminated C string (mirrors DrawTextEx's nCount==-1
    // convention, the only form CStarLabel::Draw ever uses).
    void DrawTextEllipsis(const char* sz, RECT* rc, UINT flags) {
        (void)flags;  // see comment above: accepted, not decoded
        int boxWidth = rc->right - rc->left;
        int fullLen = (int)strlen(sz);

        CSize fullExtent = GetTextExtent(sz, fullLen);
        if (fullExtent.cx <= boxWidth) {
            // Untruncated: the whole string already fits.
            TextOut(rc->left, rc->top, sz, fullLen);
            return;
        }

        // Truncated: binary-search the longest prefix length (0..fullLen)
        // whose measured width, WITH "..." appended, is <= boxWidth. The
        // search predicate is monotone (longer prefix -> wider), so a
        // standard binary search finds the max fitting length in O(log n)
        // measure_text calls rather than a linear scan.
        static const char kEllipsis[] = "...";
        const int ellipsisLen = 3;
        int lo = 0, hi = fullLen;  // invariant: lo fits, hi may not
        while (lo < hi) {
            int mid = lo + (hi - lo + 1) / 2;  // round up -- probe toward hi
            CSize prefixExtent = GetTextExtent(sz, mid);
            CSize ellipsisExtent = GetTextExtent(kEllipsis, ellipsisLen);
            if (prefixExtent.cx + ellipsisExtent.cx <= boxWidth) {
                lo = mid;
            } else {
                hi = mid - 1;
            }
        }

        std::string truncated(sz, sz + lo);
        truncated += kEllipsis;
        TextOut(rc->left, rc->top, truncated.c_str(), (int)truncated.size());
    }

    // --- paths (see brief's "Adapter semantics": between BeginPath/EndPath,
    //     MoveTo/LineTo/PolyBezier/CloseFigure accumulate points; outside a
    //     path, MoveTo just moves the current position and LineTo emits an
    //     immediate 2-point stroke path) ----------------------------------
    void BeginPath() { inPath_ = true; pathPts_.clear(); }
    void EndPath() { inPath_ = false; }
    void MoveTo(int x, int y) {
        int32_t lx, ly;
        toLogical(x, y, lx, ly);
        if (inPath_) {
            pathPts_.push_back({CC_PATH_MOVE, lx, ly});
        }
        curX_ = lx; curY_ = ly;
    }
    // R9 (Plan 2 Task 4): POINT overload -- traj.cpp calls dc->MoveTo(POINT)
    // (e.g. CTraj::Draw's `dc->MoveTo(seg->SegLo())`); GDI itself overloads
    // MoveTo(int,int)/MoveTo(POINT) identically. Forwards to the (x,y) form.
    void MoveTo(POINT pt) { MoveTo(pt.x, pt.y); }
    void LineTo(int x, int y) {
        int32_t lx, ly;
        toLogical(x, y, lx, ly);
        if (inPath_) {
            pathPts_.push_back({CC_PATH_LINE, lx, ly});
            curX_ = lx; curY_ = ly;
        } else {
            // Outside BeginPath/EndPath: emit an immediate 2-point stroke
            // path with the current pen and advance position (traj.cpp's
            // dash-segment drawing style; brief's Adapter semantics).
            cc_path_pt pts[2] = { {CC_PATH_MOVE, curX_, curY_}, {CC_PATH_LINE, lx, ly} };
            emitPath(pts, 2, /*doFill=*/0, /*doStroke=*/1);
            curX_ = lx; curY_ = ly;
        }
    }
    // R9 (Plan 2 Task 4): POINT overload -- traj.cpp's DashSeg calls
    // dc->LineTo(interpedPoint) / dc->LineTo(thisPoint) with a POINT.
    void LineTo(POINT pt) { LineTo(pt.x, pt.y); }
    void CloseFigure() {
        if (inPath_) pathPts_.push_back({CC_PATH_CLOSE, 0, 0});
    }
    void PolyBezier(const POINT* pts, int count) {
        if (!inPath_) return;
        for (int i = 0; i < count; i++) {
            int32_t lx, ly;
            toLogical(pts[i].x, pts[i].y, lx, ly);
            pathPts_.push_back({CC_PATH_CUBIC, lx, ly});
        }
        if (count > 0) { curX_ = pathPts_.back().x; curY_ = pathPts_.back().y; }
    }
    // R9 (Plan 2 Task 4): spline.cpp's CSpline::Draw calls
    // dc->PolyBezierTo(bezpts+1, BezierCount()-1). Real GDI distinguishes
    // PolyBezier (pts[0] is an explicit start point) from PolyBezierTo (all
    // points are curve/control points continuing from the current position)
    // -- but this adapter's PolyBezier body above already treats every point
    // as a CC_PATH_CUBIC entry with no special-casing of pts[0], which is
    // exactly PolyBezierTo's semantics. Same body, distinct name for fidelity
    // with the original call sites (panel.cpp, a later plan, calls the other
    // overload: `dc->PolyBezier(card.bezpts, card.BezierCount())`).
    void PolyBezierTo(const POINT* pts, int count) { PolyBezier(pts, count); }
    void StrokePath() {
        emitPath(pathPts_.data(), (int32_t)pathPts_.size(), /*doFill=*/0, /*doStroke=*/1);
        pathPts_.clear();
    }
    void StrokeAndFillPath() {
        emitPath(pathPts_.data(), (int32_t)pathPts_.size(), /*doFill=*/1, /*doStroke=*/1);
        pathPts_.clear();
    }
    void Ellipse(int l, int t, int r, int b) {
        // 4-cubic closed-path approximation of an axis-aligned ellipse
        // (kappa 0.5522847498), filled with the current brush and stroked
        // with the current pen (GDI Ellipse semantics; R14(iv)). Emitted
        // immediately -- Ellipse is not itself a path-accumulation call in
        // GDI (it draws regardless of BeginPath/EndPath bracketing). The
        // rect is logical coordinates like every other GDI call (window
        // origin applies here too).
        int32_t ll, tt, rr, bb;
        toLogical(l, t, ll, tt);
        toLogical(r, b, rr, bb);
        const double kappa = 0.5522847498;
        double cx = (ll + rr) / 2.0, cy = (tt + bb) / 2.0;
        double rx = (rr - ll) / 2.0, ry = (bb - tt) / 2.0;
        double ox = rx * kappa, oy = ry * kappa;
        // Four on-curve points, starting at the rightmost point, sweeping
        // through top/left/bottom/back-to-right (standard 4-arc layout).
        struct Pt { double x, y; };
        Pt p0{cx + rx, cy}, p1{cx, cy + ry}, p2{cx - rx, cy}, p3{cx, cy - ry};
        cc_path_pt pts[12];
        int i = 0;
        auto arc = [&](Pt from, Pt c1, Pt c2, Pt to) {
            pts[i++] = { CC_PATH_CUBIC, (int32_t)c1.x, (int32_t)c1.y };
            pts[i++] = { CC_PATH_CUBIC, (int32_t)c2.x, (int32_t)c2.y };
            pts[i++] = { CC_PATH_CUBIC, (int32_t)to.x, (int32_t)to.y };
            (void)from;
        };
        arc(p0, {p0.x, p0.y + oy}, {p1.x + ox, p1.y}, p1);
        arc(p1, {p1.x - ox, p1.y}, {p2.x, p2.y + oy}, p2);
        arc(p2, {p2.x, p2.y - oy}, {p3.x - ox, p3.y}, p3);
        arc(p3, {p3.x + ox, p3.y}, {p0.x, p0.y - oy}, p0);
        emitPath(pts, 12, /*doFill=*/1, /*doStroke=*/1);
    }
    // R9 (Plan 2 Task 6): MFC's CDC::Ellipse is overloaded on LPCRECT.
    // balloon.cpp's CBWoodringThink::Draw calls pdc->Ellipse(&circRect) (the
    // think-bubble circles). Forwards to the (l,t,r,b) form above.
    void Ellipse(const RECT* r) { Ellipse(r->left, r->top, r->right, r->bottom); }

    // --- solid fills -----------------------------------------------------
    void FillSolidRect(const RECT* r, COLORREF color) {
        FillSolidRect(r->left, r->top, r->right, r->bottom, color);
    }
    void FillSolidRect(int l, int t, int r, int b, COLORREF color) {
        int32_t ll, tt, rr, bb;
        toLogical(l, t, ll, tt);
        toLogical(r, b, rr, bb);
        canvasWrap().fill_rect(ll, tt, rr, bb, color);
    }

    // --- image blit (rule R14(i): SRCCOPY only) --------------------------
    void StretchDIBits(int destX, int destY, int destW, int destH,
                        int srcX, int srcY, int srcW, int srcH,
                        const void* bits, const BITMAPINFO* bmi,
                        UINT /*usage*/, DWORD rop) {
        ASSERT(rop == (DWORD)SRCCOPY);
        int32_t w = 0, h = 0;
        uint8_t* rgba = nullptr;
        if (!bridge_decode_dib_to_rgba(const_cast<BITMAPINFO*>(bmi),
                                        const_cast<void*>(bits), &w, &h, &rgba)) {
            return;
        }
        cc_image img; img.width = w; img.height = h; img.rgba = rgba;
        int32_t dl, dt, dr, db;
        toLogical(destX, destY, dl, dt);
        toLogical(destX + destW, destY + destH, dr, db);
        canvasWrap().draw_image(&img, dl, dt, dr, db, srcX, srcY, srcX + srcW, srcY + srcH);
        cc_image_free(&img);
    }

    // --- pose-plane blit with alpha (rule R14(i); Plan 2 Task 7, refined in
    //     the Task 13 follow-on) -------------------------------------------
    // Draws one avatar pose plane (image DIB `image`, optional separate mask
    // DIB `mask`) as a SINGLE alpha-composited draw_image. With a non-NULL
    // `mask`, it is the RGBA collapse of the original CBody draw's MERGEPAINT-
    // mask + SRCAND-drawing ROP pair (bodycam.cpp DrawBody); the mask supplies
    // the alpha exactly as the pose-image golden path does. When `mask` is
    // NULL, the original blitted the drawing plane SRCAND-ALONE -- WHITE source
    // pixels transparent, non-white opaque (NOT self-opaque; see
    // bridge_decode_dib_pair_to_rgba's derivation). NOT used for the aura plane
    // -- see DrawAuraImage below, which has different (MERGEPAINT-alone)
    // polarity. The dest rect is logical coordinates (window origin applies),
    // like every other GDI call; the src rect is the full decoded image.
    // Defined out of line in mfc_compat.cpp (needs the full CDIB definition --
    // dib.h).
    void DrawPoseImage(CDIB* image, CDIB* mask,
                       int destX, int destY, int destW, int destH);

    // --- aura-plane blit with white-alpha decode (R14(v); Task 7 review fix)
    // Draws one avatar pose's aura (whisper-nimbus) plane as a SINGLE alpha-
    // composited draw_image, reproducing the original's MERGEPAINT-ALONE draw
    // (no paired SRCAND, unlike the image/mask draw DrawPoseImage serves)
    // exactly: dest forced WHITE where the aura bit is 1 (black/silhouette),
    // dest UNTOUCHED (background shows through) where the aura bit is 0
    // (white/background) -- see bridge_decode_aura_to_white_alpha's full ROP-
    // algebra derivation in bridge_art.cpp. Collapsing that MERGEPAINT-alone
    // draw to DrawPoseImage(aura, NULL, ...) (the prior, incorrect shape of
    // this call site) would decode the aura plane through the image/mask
    // polarity instead: fully opaque (an opaque box hiding the background)
    // with the silhouette rendered black (an inverted halo) -- backwards on
    // both color and coverage. This method exists specifically to avoid that.
    // The dest rect is logical coordinates, like DrawPoseImage; the src rect
    // is the full decoded image. Defined out of line in mfc_compat.cpp (needs
    // the full CDIB definition -- dib.h).
    void DrawAuraImage(CDIB* aura, int destX, int destY, int destW, int destH);

    // --- clip region (stack of rects, stored in canvas/device space -- i.e.
    //     with the window origin already applied, matching real GDI: a clip
    //     region set under one origin does not retroactively move if the
    //     origin later changes. Base = +/-2^28 sentinel.) ------------------
    void IntersectClipRect(const RECT* r) {
        // MM_TWIPS is y-up (a normal rect has top >= bottom numerically), so
        // intersecting on x takes the tighter (larger) left / (smaller)
        // right exactly as a y-down RECT would; on y it takes the tighter
        // (smaller) top / (larger) bottom -- the min/max pairing flips
        // relative to x because the sign convention flips.
        int32_t rl, rt, rr, rb;
        toLogical(r->left, r->top, rl, rt);
        toLogical(r->right, r->bottom, rr, rb);
        CRect cur = clipStack_.back();
        CRect next(
            max(cur.left, rl), min(cur.top, rt),
            min(cur.right, rr), max(cur.bottom, rb));
        clipStack_.push_back(next);
        canvasWrap().clip_push(next.left, next.top, next.right, next.bottom);
    }
    void GetClipBox(RECT* r) const {
        // Convert the canvas-space clip rect back into the CURRENT logical
        // coordinate system (adds back the current origin) -- matches real
        // GDI's GetClipBox, which reports the clip region in whatever
        // logical space is active right now, not the space it was set in.
        const CRect& cur = clipStack_.back();
        r->left = cur.left + orgX_; r->top = cur.top + orgY_;
        r->right = cur.right + orgX_; r->bottom = cur.bottom + orgY_;
    }
    void SelectClipRgn(void* rgn, int mode) {
        (void)rgn; (void)mode;  // only NULL/RGN_COPY ("reset") is supported
        clipStack_.resize(1);   // back to the base sentinel
        canvasWrap().clip_pop();
    }

    // --- window origin (accumulates; every emitted coordinate is c - org) --
    void SetWindowOrg(int x, int y) { orgX_ = x; orgY_ = y; }
    void OffsetWindowOrg(int dx, int dy) { orgX_ += dx; orgY_ += dy; }
    int GetMapMode() const { return MM_TWIPS; }
    void SetMapMode(int mode) { ASSERT(mode == MM_TWIPS); }

    // --- DC state queries --------------------------------------------------
    BOOL IsPrinting() const { return m_bPrinting; }
    int GetDeviceCaps(int index) const {
        // Only LOGPIXELSX/Y are meaningful here (R14 header: 1440 makes the
        // original's MulDiv(pt, LOGPIXELSY, 72) arithmetic yield twips).
        ASSERT(index == LOGPIXELSX || index == LOGPIXELSY);
        return 1440;
    }
    void* GetSafeHdc() const { return (void*)this; }  // opaque non-null token; only ever null-checked

protected:
    cc_canvas* canvas_;
    CFont* curFont_ = nullptr;
    CPen* curPen_ = nullptr;
    CBrush* curBrush_ = nullptr;
    CBitmap* curBitmap_ = nullptr;
    COLORREF textColor_ = 0;
    int bkMode_ = OPAQUE;
    COLORREF bkColor_ = 0;
    int32_t orgX_ = 0, orgY_ = 0;
    int32_t curX_ = 0, curY_ = 0;
    bool inPath_ = false;
    std::vector<cc_path_pt> pathPts_;
    std::vector<CRect> clipStack_{ CRect(-(1 << 28), (1 << 28), (1 << 28), -(1 << 28)) };

    CCanvas canvasWrap() const { return CCanvas(canvas_); }
    void toLogical(int x, int y, int32_t& outX, int32_t& outY) const {
        outX = x - orgX_;
        outY = y - orgY_;
    }
    cc_font_spec currentFontSpec() const {
        static const CFont kDefault{};
        return (curFont_ ? curFont_ : &kDefault)->spec();
    }
    void emitPath(const cc_path_pt* pts, int32_t n, int32_t doFill, int32_t doStroke) {
        uint32_t fillColor = curBrush_ ? curBrush_->color : 0;
        uint32_t strokeColor = curPen_ ? curPen_->color : 0;
        int32_t width = curPen_ ? curPen_->width : 1;
        canvasWrap().path(pts, n, doFill, fillColor, doStroke, strokeColor, width, /*dashed=*/0);
    }
};

// --- CClientDC (Plan 2 Task 3) -----------------------------------------------
// A CDC auto-bound to the registered metrics canvas (replaces the original's
// shared MM_TWIPS desktop CClientDC used for layout-time text measurement --
// R17). ASSERTs the metrics canvas is registered (cc_set_metrics_canvas) --
// constructing one before that call is a programming error, not a runtime
// condition to handle gracefully.
class CClientDC : public CDC {
public:
    CClientDC();  // defined in mfc_compat.cpp (needs engine_context.h's full
                   // CCEngineContext -- this header only forward-declares it,
                   // see the ccContext() forward declaration above).
};

// --- palette / stretch-mode no-ops (rule R9; free functions, matching the
//     original's Win32-global call shape -- GetBrushOrgEx/SetBrushOrgEx/
//     SetStretchBltMode/GetCurrentPalette/SelectPalette/RealizePalette all
//     become no-ops end-to-end since the port is RGBA-only, R14(ii)) --------
inline CPalette* GetCurrentPalette(CDC*) { return nullptr; }
inline CPalette* SelectPalette(CDC*, CPalette*, BOOL) { return nullptr; }
inline UINT RealizePalette(CDC*) { return 0; }
inline int SetStretchBltMode(CDC*, int) { return 0; }
inline BOOL GetBrushOrgEx(CDC*, POINT* pt) { pt->x = 0; pt->y = 0; return TRUE; }
inline BOOL SetBrushOrgEx(CDC*, int, int, POINT* pt) { if (pt) { pt->x = 0; pt->y = 0; } return TRUE; }

// --- free ::StretchDIBits(HDC, ...) (rule R9; Plan 2 Task 7) -----------------
// dib.cpp's CDIB::Draw overloads (re-enabled when CC_NO_RENDER retired) call
// the Win32 FREE function ::StretchDIBits(hdc, ...), not the CDC member. In
// this port HDC is the CDC* itself (CDC::GetSafeHdc() returns (void*)this), so
// this free form recovers the CDC and forwards to CDC::StretchDIBits (the
// SRCCOPY-only adapter that decodes the palettized DIB to RGBA and blits it).
// Signature matches Win32's ::StretchDIBits exactly (return value is the number
// of scan lines; the sole callers, CDIB::Draw, ignore it).
inline int StretchDIBits(HDC hdc, int destX, int destY, int destW, int destH,
                         int srcX, int srcY, int srcW, int srcH,
                         const void* bits, const BITMAPINFO* bmi,
                         UINT usage, DWORD rop) {
    if (hdc == nullptr) return 0;
    ((CDC*)hdc)->StretchDIBits(destX, destY, destW, destH,
                               srcX, srcY, srcW, srcH, bits, bmi, usage, rop);
    return destH;
}

// --- collections (MFC-flavored, std::vector-backed) ---------------------------
template <typename T>
class CCArrayBase {
public:
    int GetSize() const { return (int)m_v.size(); }
    int GetUpperBound() const { return (int)m_v.size() - 1; }
    void SetSize(int n, int nGrowBy = -1) { (void)nGrowBy; m_v.resize((size_t)n); }
    void RemoveAll() { m_v.clear(); }
    T& operator[](int i) { return m_v[(size_t)i]; }
    const T& operator[](int i) const { return m_v[(size_t)i]; }
    T GetAt(int i) const { return m_v[(size_t)i]; }
    void SetAt(int i, T v) { m_v[(size_t)i] = v; }
    int Add(T v) { m_v.push_back(v); return (int)m_v.size() - 1; }
    void RemoveAt(int i) { m_v.erase(m_v.begin() + i); }
    // R9 (Plan 2 Task 5): format.cpp's InsertFormat inserts a new formatting
    // DWORD at a computed index i (the first entry whose offset is >= the
    // new offset), shifting everything from i onward up by one -- MFC's
    // CDWordArray::InsertAt(index, newElement) semantics exactly.
    void InsertAt(int i, T v) { m_v.insert(m_v.begin() + i, v); }
    void FreeExtra() { m_v.shrink_to_fit(); }  // R9: releases unused capacity, like MFC's FreeExtra()
    // R9 (Plan 2 Task 6): MFC's CArray::GetData returns a pointer to the
    // contiguous element storage. balloon.cpp's FindFurthestLineBreakIntl
    // (dead INTL path) passes prgdwFormatting->GetData() to
    // FindSubStringForINTLThatFits. Empty-array case returns NULL (MFC returns
    // a valid-but-unusable pointer for size 0; NULL is safer and the one
    // caller already guards with `prgdwFormatting ? ...->GetData() : NULL`).
    T* GetData() { return m_v.empty() ? nullptr : m_v.data(); }
    const T* GetData() const { return m_v.empty() ? nullptr : m_v.data(); }
protected:
    std::vector<T> m_v;
};
class CDWordArray  : public CCArrayBase<DWORD> {};
class CPtrArray    : public CCArrayBase<void*> {};
class CObArray     : public CCArrayBase<CObject*> {};
class CStringArray : public CCArrayBase<CString> {};

// --- CTypedPtrArray (rule R9; avatar.h: CTypedPtrArray<CPtrArray, CPose*>) ---
// MFC's CTypedPtrArray<BASE_CLASS, TYPE> is BASE_CLASS with typed accessors.
// CPose* is stored as void* underneath (matches CPtrArray's element type).
template <typename BaseArray, typename T>
class CTypedPtrArray : public BaseArray {
public:
    T operator[](int i) const { return (T)(BaseArray::operator[](i)); }
    T GetAt(int i) const { return (T)BaseArray::GetAt(i); }
    void SetAt(int i, T v) { BaseArray::SetAt(i, (void*)v); }
    int Add(T v) { return BaseArray::Add((void*)v); }
};

// --- POSITION (rule R9; backdrop.cpp FlushBackDropCache iterates a
//     CMapWordToPtr with GetStartPosition()/GetNextAssoc(), MFC's generic
//     "iteration cursor" idiom). MFC declares it as `void*` (NULL = end of
//     collection); we do the same. --------------------------------------------
typedef void* POSITION;

// --- CMapWordToPtr (rule R9; backdrop.cpp backMapS/backMapP cache backdrop
//     art by WORD backID). MFC's CMapWordToPtr is a WORD->void* hash map with
//     Lookup/SetAt/RemoveKey/RemoveAll and POSITION-based iteration via
//     GetStartPosition()/GetNextAssoc(). Backed by std::unordered_map; POSITION
//     here is that map's node in disguise, encoded as an index into a stable
//     key list captured at GetStartPosition() time (simplest correct mapping
//     for backdrop.cpp's single linear iterate-and-optionally-delete use). ---
//
// ITERATION CONTRACT (review fix, matches real MFC CMapWordToPtr behavior):
//   - Iteration order is unspecified (hash order, not insertion order).
//   - GetStartPosition() snapshots the key set at that moment: a SetAt() of a
//     NEW key during an in-flight iteration is NOT reflected in that
//     iteration (you won't see it via GetNextAssoc()).
//   - RemoveKey() of ANY key (the one just visited, one not yet visited, or
//     even one already visited) during an in-flight iteration is TOLERATED:
//     GetNextAssoc() skips snapshotted keys no longer present in the map. The
//     loop completes over the surviving entries and never throws, matching
//     MFC's documented "removing the current element during iteration is
//     legal" idiom.
class CMapWordToPtr {
public:
    explicit CMapWordToPtr(int /*nBlockSize*/ = 0) {}

    BOOL Lookup(WORD key, void*& value) const {
        auto it = m_map.find(key);
        if (it == m_map.end()) return FALSE;
        value = it->second;
        return TRUE;
    }
    void SetAt(WORD key, void* value) { m_map[key] = value; }
    BOOL RemoveKey(WORD key) { return (BOOL)m_map.erase(key); }
    void RemoveAll() { m_map.clear(); }

    POSITION GetStartPosition() const {
        m_iterKeys.clear();
        m_iterKeys.reserve(m_map.size());
        for (auto& kv : m_map) m_iterKeys.push_back(kv.first);
        return m_iterKeys.empty() ? nullptr : (POSITION)(uintptr_t)1;
    }
    void GetNextAssoc(POSITION& pos, WORD& key, void*& value) const {
        size_t idx = (size_t)(uintptr_t)pos - 1;
        // Skip any snapshotted key that's since been removed (RemoveKey
        // mid-iteration), so this never throws and the loop still lands on
        // an existing entry (or runs off the end, setting pos to nullptr).
        auto it = m_map.end();
        while (idx < m_iterKeys.size()) {
            it = m_map.find(m_iterKeys[idx]);
            if (it != m_map.end()) break;
            idx++;
        }
        if (idx >= m_iterKeys.size() || it == m_map.end()) {
            pos = nullptr;
            return;
        }
        key = it->first;
        value = it->second;
        idx++;
        pos = idx < m_iterKeys.size() ? (POSITION)(uintptr_t)(idx + 1) : nullptr;
    }

private:
    std::unordered_map<WORD, void*> m_map;
    mutable std::vector<WORD> m_iterKeys;
};

// --- CMapStringToPtr (rule R9; spline.cpp's betaMatrixMap caches CBeta
//     blending matrices keyed by a "%f*%f" tension/bias string). MFC's
//     CMapStringToPtr is a CString->void* hash map with the same
//     Lookup/SetAt/RemoveKey/RemoveAll + POSITION-based GetStartPosition()/
//     GetNextAssoc() shape as CMapWordToPtr above (spline.cpp's
//     DestroySplineMatrixCaches iterates-and-removes exactly like
//     backdrop.cpp's FlushBackDropCache does over CMapWordToPtr), just keyed
//     by string instead of WORD. Same iteration contract as CMapWordToPtr
//     (see its comment above): unspecified order, snapshot-at-GetStartPosition,
//     tolerates RemoveKey of any key -- visited, unvisited, or already gone --
//     mid-iteration. MFC's SetAt dup's the key string internally; std::string
//     already owns its storage, so no extra copy step is needed here. --------
class CMapStringToPtr {
public:
    explicit CMapStringToPtr(int /*nBlockSize*/ = 0) {}

    BOOL Lookup(const char* key, void*& value) const {
        auto it = m_map.find(key ? key : "");
        if (it == m_map.end()) return FALSE;
        value = it->second;
        return TRUE;
    }
    void SetAt(const char* key, void* value) { m_map[key ? key : ""] = value; }
    BOOL RemoveKey(const char* key) { return (BOOL)m_map.erase(key ? key : ""); }
    void RemoveAll() { m_map.clear(); }

    POSITION GetStartPosition() const {
        m_iterKeys.clear();
        m_iterKeys.reserve(m_map.size());
        for (auto& kv : m_map) m_iterKeys.push_back(kv.first);
        return m_iterKeys.empty() ? nullptr : (POSITION)(uintptr_t)1;
    }
    void GetNextAssoc(POSITION& pos, CString& key, void*& value) const {
        size_t idx = (size_t)(uintptr_t)pos - 1;
        auto it = m_map.end();
        while (idx < m_iterKeys.size()) {
            it = m_map.find(m_iterKeys[idx]);
            if (it != m_map.end()) break;
            idx++;
        }
        if (idx >= m_iterKeys.size() || it == m_map.end()) {
            pos = nullptr;
            return;
        }
        key = it->first.c_str();
        value = it->second;
        idx++;
        pos = idx < m_iterKeys.size() ? (POSITION)(uintptr_t)(idx + 1) : nullptr;
    }

private:
    std::unordered_map<std::string, void*> m_map;
    mutable std::vector<std::string> m_iterKeys;
};

// --- CPtrList (rule R9; traj.h's CTraj::m_segs holds the CSeg* chain drawn/
//     dashed in order). MFC's CPtrList is a doubly-linked list of void*;
//     traj.cpp's usage is the minimal "append, then walk head-to-tail" idiom
//     (AddTail + GetHeadPosition/GetNext), so that's all that's implemented
//     here -- backed by std::vector for simplicity, POSITION reused as a
//     1-based index into it (index 0 doubles as "end of list", matching
//     POSITION's NULL-means-end convention used throughout this header). ---
class CPtrList {
public:
    void AddTail(void* p) { m_v.push_back(p); }
    // R9 (Plan 2 Task 6): fonts.cpp builds m_fonts/m_fontInfos with AddHead
    // (prepend), then DestroyFonts walks head-to-tail deleting each. MFC's
    // AddHead inserts at the front; order only matters for the walk, and
    // DestroyFonts deletes every element regardless, so prepend-vs-append is
    // behaviorally identical there -- but AddHead's contract is "prepend", so
    // that is what it does (GetHeadPosition then yields the most recent add).
    void AddHead(void* p) { m_v.insert(m_v.begin(), p); }
    // R9 (Plan 2 Task 6): fonts.cpp's DestroyFonts calls m_fonts.RemoveAll()/
    // m_fontInfos.RemoveAll() after deleting every element. Clears the list.
    void RemoveAll() { m_v.clear(); }
    POSITION GetHeadPosition() const {
        return m_v.empty() ? nullptr : (POSITION)(uintptr_t)1;
    }
    // const: GetNext/GetPrev read m_v[idx] and mutate only the caller's POSITION
    // (never the list), so they are const-safe -- panel.cpp calls them on const
    // CPtrList& (GetIndex(const CPtrList&), CPanel copy-ctor over `const CPanel&
    // p`), exactly as the original relies on MFC allowing. Matches MFC's node-
    // walking semantics: the list is not mutated by a cursor advance.
    void* GetNext(POSITION& pos) const {
        size_t idx = (size_t)(uintptr_t)pos - 1;
        void* value = m_v[idx];
        idx++;
        pos = idx < m_v.size() ? (POSITION)(uintptr_t)(idx + 1) : nullptr;
        return value;
    }

    // --- R9 (Plan 2 Task 8): panel.cpp's CPanel/CUnitPanel/CUnitPanelPage own
    //     three CPtrLists (m_elements/m_bodies/m_panels) and exercise the full
    //     doubly-linked-list idiom, not just head-to-tail append+walk. The
    //     additions below complete that idiom over the same vector backing
    //     (POSITION stays a 1-based index; NULL/0 == "end"). Every member is
    //     exercised by cc_selftest_panel_ptrlist(). MFC's CPtrList is a real
    //     linked list, but panel.cpp never relies on node-pointer stability
    //     across a structural mutation held ACROSS a suspended iteration (it
    //     re-fetches head/tail after each RemoveTail/SetAt), so the vector
    //     model is behavior-faithful for these call sites (verified per site
    //     in the Task 8 report).

    // GetCount: panel.cpp uses it constantly for the panel-break decision
    // (m_panels.GetCount() < 2), element caps (m_elements.GetCount() >= 5),
    // body caps, and title trimming.
    int GetCount() const { return (int)m_v.size(); }
    // IsEmpty: CUnitPanelPage::UpdateTitle (m_panels.IsEmpty()).
    BOOL IsEmpty() const { return m_v.empty(); }

    // GetHead/GetTail: peek the ends without a cursor (AddLine reads
    // m_panels.GetTail(); UpdateTitle reads m_panels.GetHead()/GetTail()).
    // Empty-list case returns nullptr rather than dereferencing an empty vector
    // (UB): panel.cpp's AddLine evaluates `pOldP = m_panels.GetTail()` on the
    // very first (empty) call but the || short-circuit (m_newPanel==TRUE) never
    // dereferences pOldP then, so nullptr is the safe stand-in for MFC's
    // "returns a never-used header node on empty" behavior. (Real MFC ASSERTs on
    // empty in debug; nullptr is the faithful non-crashing equivalent for the
    // guarded call sites here.)
    void* GetHead() const { return m_v.empty() ? nullptr : m_v.front(); }
    void* GetTail() const { return m_v.empty() ? nullptr : m_v.back(); }

    // GetTailPosition + GetPrev: reverse walk (DrawRoutes, CUnitPanel::Draw
    // iterate m_elements tail-to-head so later balloons draw under earlier
    // ones). GetPrev returns the element AT pos and steps pos toward the head
    // (mirrors GetNext's "return current, advance cursor" contract).
    POSITION GetTailPosition() const {
        return m_v.empty() ? nullptr : (POSITION)(uintptr_t)m_v.size();
    }
    void* GetPrev(POSITION& pos) const {
        size_t idx = (size_t)(uintptr_t)pos - 1;
        void* value = m_v[idx];
        pos = idx == 0 ? nullptr : (POSITION)(uintptr_t)idx;  // idx-1, 1-based
        return value;
    }

    // RemoveTail: CPage::RemoveLastPanel returns m_panels.RemoveTail();
    // UpdateTitle trims trailing title elements. MFC returns the removed
    // element pointer.
    void* RemoveTail() {
        void* value = m_v.back();
        m_v.pop_back();
        return value;
    }

    // R9 (Plan 3 Task 2): RemoveHead -- the parser's query list (a CPtrList-
    // derived CCQuery, Task 4) is an AddTail-append / RemoveHead-dequeue FIFO
    // (the original's "queue outstanding queries, service the oldest first"
    // idiom -- ircproto-map.md Sec8). MFC's RemoveHead returns the removed
    // element pointer, same contract as RemoveTail above.
    void* RemoveHead() {
        void* value = m_v.front();
        m_v.erase(m_v.begin());
        return value;
    }

    // GetAt/SetAt by POSITION: CPanel::ReplaceBody walks m_bodies with a cursor,
    // reads GetAt(pos), substitutes via SetAt(pos, newBody). MFC's GetAt/SetAt
    // take a POSITION (not an int index) -- distinct from the CPtrArray int
    // overloads. The cursor here is a 1-based index (see GetNext).
    void* GetAt(POSITION pos) const { return m_v[(size_t)(uintptr_t)pos - 1]; }
    void SetAt(POSITION pos, void* p) { m_v[(size_t)(uintptr_t)pos - 1] = p; }

    // FindIndex(nIndex): CPanel copy-ctor maps an element's speaker to the
    // matching cloned body by ordinal index (GetIndex gives the int; FindIndex
    // turns it back into a POSITION for GetAt). MFC returns NULL for an
    // out-of-range index.
    POSITION FindIndex(int nIndex) const {
        if (nIndex < 0 || (size_t)nIndex >= m_v.size()) return nullptr;
        return (POSITION)(uintptr_t)(nIndex + 1);
    }

    // R9 (Plan 3 Task 4): RemoveAt(POSITION) -- query.cpp's
    // CQueryPtrList::FreeRemoveAt deletes then removes one query cell found
    // earlier by FindQuery's returned POSITION (the reply-handler correlation
    // path, Task 5b). Distinct from CPtrArray's int-indexed RemoveAt (already
    // present on that class below) -- MFC overloads the name per-container;
    // this is the CPtrList (POSITION-cursor) member.
    void RemoveAt(POSITION pos) {
        size_t idx = (size_t)(uintptr_t)pos - 1;
        m_v.erase(m_v.begin() + (ptrdiff_t)idx);
    }

private:
    std::vector<void*> m_v;
};

#endif // MFC_COMPAT_H
