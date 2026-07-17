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
typedef char           TCHAR;
typedef const char*    LPCSTR;
typedef char*          LPSTR;
typedef const char*    LPCTSTR;
typedef char*          LPTSTR;
typedef void*          LPVOID;
#define TRUE  1
#define FALSE 0

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
#define BI_RGB  0u
#define BI_RLE8 1u
#define BI_RLE4 2u

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

// --- diagnostics ------------------------------------------------------------
void ccLog(const char* fmt, ...);
#ifdef NDEBUG
#define ASSERT(e) ((void)0)
#else
#define ASSERT(e) do { if (!(e)) { ccLog("ASSERT failed: %s (%s:%d)", #e, __FILE__, __LINE__); abort(); } } while (0)
#endif
#define VERIFY(e) do { if (!(e)) ccLog("VERIFY failed: %s (%s:%d)", #e, __FILE__, __LINE__); } while (0)
#define TRACE(...) ccLog(__VA_ARGS__)

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
    void SetSize(int n) { m_v.resize((size_t)n); }
    void RemoveAll() { m_v.clear(); }
    T& operator[](int i) { return m_v[(size_t)i]; }
    const T& operator[](int i) const { return m_v[(size_t)i]; }
    T GetAt(int i) const { return m_v[(size_t)i]; }
    void SetAt(int i, T v) { m_v[(size_t)i] = v; }
    int Add(T v) { m_v.push_back(v); return (int)m_v.size() - 1; }
    void RemoveAt(int i) { m_v.erase(m_v.begin() + i); }
protected:
    std::vector<T> m_v;
};
class CDWordArray  : public CCArrayBase<DWORD> {};
class CPtrArray    : public CCArrayBase<void*> {};
class CObArray     : public CCArrayBase<CObject*> {};
class CStringArray : public CCArrayBase<CString> {};

#endif // MFC_COMPAT_H
