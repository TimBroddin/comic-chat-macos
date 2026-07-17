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

// --- scalar typedefs -------------------------------------------------------
typedef int            BOOL;
typedef uint8_t        BYTE;
typedef uint8_t        UCHAR;
typedef uint16_t       WORD;
typedef uint16_t       USHORT;
typedef uint32_t       DWORD;
typedef uint32_t       ULONG;
typedef uint32_t       UINT;
typedef int32_t        LONG;   // Win32 LONG is 32-bit; engine structs rely on it
typedef int16_t        SHORT;  // R9: avatar loading chain (bbox.h SRECT, avatar.h m_iconIndex)
typedef char           CHAR;   // R9: avatar.h GetIndices/SetIndices signed-byte indices
typedef char           TCHAR;
typedef const char*    LPCSTR;
typedef char*          LPSTR;
typedef const char*    LPCTSTR;
typedef char*          LPTSTR;
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
#define min(a,b) (((a) < (b)) ? (a) : (b))
#define max(a,b) (((a) > (b)) ? (a) : (b))

// --- GetTickCount (rule R9; avatar.cpp CAvatarComplex::SetSequential, only
//     reachable inside its CC_NO_UI-stubbed internals, but must still parse) -
inline DWORD GetTickCount() { return 0; }

// --- Win32 geometry & color -----------------------------------------------
struct POINT { LONG x; LONG y; };
struct SIZE  { LONG cx; LONG cy; };
struct RECT  { LONG left; LONG top; LONG right; LONG bottom; };

typedef DWORD COLORREF;
#define RGB(r,g,b) ((COLORREF)(((BYTE)(r)) | (((DWORD)(BYTE)(g)) << 8) | (((DWORD)(BYTE)(b)) << 16)))
#define GetRValue(c) ((BYTE)(c))
#define GetGValue(c) ((BYTE)((c) >> 8))
#define GetBValue(c) ((BYTE)((c) >> 16))

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

// --- MFC exception-handling macros (rule R9; used by lifted avbfile.cpp /
//     avatario.cpp around `new` calls that could throw). MFC's TRY/CATCH_ALL
//     wrap try/catch(CException*); we have no CException hierarchy, so these
//     become plain try/catch(...) blocks - any thrown exception (e.g.
//     std::bad_alloc from `new`) is treated the same as MFC's CATCH_ALL. ---
#define TRY try
#define CATCH_ALL(e) catch (...)
#define END_CATCH_ALL

// --- rendering types are opaque in this plan (rule R4) ----------------------
class CDC;
class CPalette;

// --- minimal CObject ---------------------------------------------------------
class CObject {
public:
    virtual ~CObject() {}
};

// --- CString (byte-oriented, MFC-flavored subset) ----------------------------
class CString {
public:
    CString() {}
    CString(const char* s) : m_s(s ? s : "") {}
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
    void FreeExtra() { m_v.shrink_to_fit(); }  // R9: releases unused capacity, like MFC's FreeExtra()
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

#endif // MFC_COMPAT_H
