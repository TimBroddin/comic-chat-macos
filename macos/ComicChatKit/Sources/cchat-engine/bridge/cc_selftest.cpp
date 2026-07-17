#include "comicchat.h"
#include "mfc_compat.h"
#include "engine_context.h"
#include "dib.h"
#include <unistd.h>   // mkstemp, close (testShimFileApis)

static int g_failures;
#define CC_CHECK(e) do { if (!(e)) { g_failures++; ccLog("SELFTEST FAIL: %s (%s:%d)", #e, __FILE__, __LINE__); } } while (0)

static void testCString() {
    CString s;
    CC_CHECK(s.IsEmpty());
    s.Format("%s/%s.avb", "/art", "anna");
    CC_CHECK(s == "/art/anna.avb");
    CC_CHECK(s.GetLength() == 13);
    CC_CHECK(s.Find('.') == 9);
    CC_CHECK(s.Mid(5, 4) == "anna");
    CString t = s.Left(4);
    CC_CHECK(t == "/art");
    t += "x";
    CC_CHECK(t == "/artx");
    CC_CHECK(t.CompareNoCase("/ARTX") == 0);
}

static void testGeometry() {
    CRect r(10, 20, 110, 220);
    CC_CHECK(r.Width() == 100 && r.Height() == 200);
    CC_CHECK(!r.IsRectEmpty());
    r.SetRectEmpty();
    CC_CHECK(r.IsRectEmpty());
}

static void testColor() {
    COLORREF c = RGB(1, 2, 3);
    CC_CHECK(GetRValue(c) == 1 && GetGValue(c) == 2 && GetBValue(c) == 3);
}

static void testRasterOpConstants() {
    // R9 addition: dib.h uses SRCCOPY as a Draw() default argument, which
    // must compile even with CC_NO_RENDER defined (declaration stays live).
    CC_CHECK(SRCCOPY == 0x00CC0020);
    CC_CHECK(DIB_RGB_COLORS == 0);
}

static void testStructSizes() {
    // Wire-format compatibility: these sizes must match Win32 exactly.
    CC_CHECK(sizeof(BITMAPFILEHEADER) == 14);
    CC_CHECK(sizeof(BITMAPINFOHEADER) == 40);
    CC_CHECK(sizeof(RGBQUAD) == 4);
    // BITMAPCOREHEADER (legacy OS/2 DIB header, R9 addition for dib.cpp):
    // DWORD bcSize + 4 WORDs = 4 + 8 = 12 bytes.
    CC_CHECK(sizeof(BITMAPCOREHEADER) == 12);
    BITMAPCOREHEADER bch;
    bch.bcSize = 12;
    bch.bcBitCount = 8;
    CC_CHECK(bch.bcSize == 12 && bch.bcBitCount == 8);
}

static void testCollections() {
    CDWordArray a;
    CC_CHECK(a.GetSize() == 0);
    a.Add(7); a.Add(9);
    CC_CHECK(a.GetSize() == 2 && a[1] == 9);
    a.RemoveAt(0);
    CC_CHECK(a.GetSize() == 1 && a[0] == 9);
}

static void testContext() {
    cc_set_art_dirs("/tmp/av", "/tmp/bg");
    CC_CHECK(ccContext().avatarDir == "/tmp/av");
    CC_CHECK(ccContext().backdropDir == "/tmp/bg");
}

static void testCStringMidClamping() {
    CC_CHECK(CString("ab").Mid(10) == "");
    CC_CHECK(CString("ab").Mid(10, 5) == "");
    CC_CHECK(CString("abcdef").Mid(2) == "cdef");
    CC_CHECK(CString("abcdef").Mid(2, 100) == "cdef");
    CC_CHECK(CString("abcdef").Mid(-1) == "abcdef");
    CC_CHECK(CString("abcdef").Mid(2, -1) == "");
}

static void testDibHelpers() {
    // 8-bit rows pad to 4 bytes: 3px @ 8bpp -> 4 bytes.
    CC_CHECK(DIBStorageWidth(3, 8) == 4);
    CC_CHECK(DIBStorageWidth(4, 8) == 4);
    CC_CHECK(DIBStorageWidth(5, 8) == 8);
    // 4-bit: 5px -> 3 bytes of pixels -> pads to 4.
    CC_CHECK(DIBStorageWidth(5, 4) == 4);
}

static void testDibCreate() {
    // Build a 2x2 8-bit DIB in memory and hand it to CDIB::Create.
    size_t infoSize = sizeof(BITMAPINFOHEADER) + 256 * sizeof(RGBQUAD);
    BITMAPINFO* bmi = (BITMAPINFO*)calloc(1, infoSize);
    bmi->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi->bmiHeader.biWidth = 2;
    bmi->bmiHeader.biHeight = 2;
    bmi->bmiHeader.biPlanes = 1;
    bmi->bmiHeader.biBitCount = 8;
    bmi->bmiHeader.biCompression = BI_RGB;
    bmi->bmiHeader.biClrUsed = 256;
    BYTE* bits = (BYTE*)calloc(1, DIBStorageWidth(2, 8) * 2);
    CDIB dib;
    CC_CHECK(dib.Create(bmi, bits));
    CC_CHECK(dib.GetWidth() == 2 && dib.GetHeight() == 2);
    CC_CHECK(dib.GetNumClrEntries() == 256);
}

// --- Task 4 R9 shim additions: selftests (review fix) -----------------------
// mfc_compat.h added these while lifting avbfile/avatario/avatar (Task 4);
// review flagged that none had selftests per R9's own mandate. Grouped below.

static void testShimFileApis() {
    // GetFileAttributes: existence check only (avatario.cpp's LoadAvatarInfo).
    char tmpTemplate[] = "/tmp/cc_selftest_fileapis_XXXXXX";
    int fd = mkstemp(tmpTemplate);
    CC_CHECK(fd != -1);
    if (fd != -1) {
        close(fd);
        CC_CHECK(GetFileAttributes(tmpTemplate) != INVALID_FILE_ATTRIBUTES);
        remove(tmpTemplate);
        CC_CHECK(GetFileAttributes(tmpTemplate) == INVALID_FILE_ATTRIBUTES);
    }

    // lstrlen / lstrcpy / _tfopen (avbfile.cpp CAvatarFileStream ctor/Open()).
    char buf[64];
    CC_CHECK(lstrlen("hello") == 5);
    CC_CHECK(lstrcpy(buf, "hello") == buf);
    CC_CHECK(strcmp(buf, "hello") == 0);

    char tmpTemplate2[] = "/tmp/cc_selftest_tfopen_XXXXXX";
    int fd2 = mkstemp(tmpTemplate2);
    CC_CHECK(fd2 != -1);
    if (fd2 != -1) {
        close(fd2);
        FILE* f = _tfopen(tmpTemplate2, __T("rb"));
        CC_CHECK(f != NULL);
        if (f != NULL) {
            fclose(f);
        }
        remove(tmpTemplate2);
    }
}

static void testShimStringApis() {
    // stricmp (avatar.cpp: GetAvatar/GetAvatar2/GetAvatar3/GetNextAvatarName).
    CC_CHECK(stricmp("Anna", "anna") == 0);
    CC_CHECK(stricmp("ABC", "abc") == 0);
    int cmpDiff = stricmp("abc", "abd");
    int cmpRef = strcasecmp("abc", "abd");
    CC_CHECK(cmpDiff != 0);
    // sign should agree with the real strcasecmp we delegate to.
    CC_CHECK((cmpDiff < 0) == (cmpRef < 0));
    CC_CHECK((cmpDiff > 0) == (cmpRef > 0));
}

static void testShimCollections2() {
    // CTypedPtrArray<CPtrArray, T*> (avatar.h: m_arrPoses).
    int a = 1, b = 2, c = 3;
    CTypedPtrArray<CPtrArray, int*> arr;
    CC_CHECK(arr.GetSize() == 0);
    arr.Add(&a);
    arr.Add(&b);
    arr.Add(&c);
    CC_CHECK(arr.GetSize() == 3);
    CC_CHECK(arr.GetAt(0) == &a);
    CC_CHECK(arr.GetAt(1) == &b);
    CC_CHECK(arr[2] == &c);
    *arr.GetAt(1) = 42;
    CC_CHECK(b == 42);

    // CCArrayBase::SetSize(n, growBy) + FreeExtra (avatar.cpp: m_arrPoses.SetSize(0,16),
    // avbfile.cpp: pAvatar->m_arrPoses.FreeExtra()).
    CDWordArray arr2;
    arr2.SetSize(0, 16);  // growBy is a capacity hint only; size must still be 0.
    CC_CHECK(arr2.GetSize() == 0);
    arr2.SetSize(5, 100);
    CC_CHECK(arr2.GetSize() == 5);
    for (int i = 0; i < 5; i++) arr2.SetAt(i, (DWORD)i);
    arr2.FreeExtra();
    CC_CHECK(arr2.GetSize() == 5);  // FreeExtra trims capacity, not size.
    for (int i = 0; i < 5; i++) CC_CHECK(arr2.GetAt(i) == (DWORD)i);
}

static void testShimMacros() {
    // ZeroMemory (avatar.cpp CPose ctor, avbfile.cpp ConvertMasksCommon).
    BYTE buf[16];
    memset(buf, 0xAA, sizeof(buf));
    ZeroMemory(buf, sizeof(buf));
    bool allZero = true;
    for (size_t i = 0; i < sizeof(buf); i++) if (buf[i] != 0) allZero = false;
    CC_CHECK(allZero);

    // RGBTRIPLE (avbfile.cpp CAvatarDIB::Load PM-DIB color table conversion).
    CC_CHECK(sizeof(RGBTRIPLE) == 3);

    // LOBYTE/HIBYTE/LOWORD/HIWORD (avbfile.cpp ConvertMasksCommon, CAvatarX::LoadAvatar).
    CC_CHECK(LOWORD(0x12345678u) == 0x5678);
    CC_CHECK(HIWORD(0x12345678u) == 0x1234);
    CC_CHECK(LOBYTE((WORD)0x1234) == 0x34);
    CC_CHECK(HIBYTE((WORD)0x1234) == 0x12);

    // GetTickCount (avatar.cpp CAvatarComplex::SetSequential, CC_NO_UI-stubbed
    // internals; stub always returns 0, so consecutive calls stay monotonic).
    DWORD t1 = GetTickCount();
    DWORD t2 = GetTickCount();
    CC_CHECK(t2 >= t1);

    // min/max macros (avatar.cpp CBodyDouble::GetDimInfo, CEmotionOpts::Add).
    CC_CHECK(min(3, 5) == 3);
    CC_CHECK(max(3, 5) == 5);
    CC_CHECK(min(-1, 1) == -1);
    CC_CHECK(max(-1, 1) == 1);

    // AfxThrowMemoryException / AfxThrowUserException (avbfile.cpp
    // ConvertMasksCommon / CChatBackdrop::LoadBackdrop): both are
    // [[noreturn]] and unconditionally throw, matching MFC's
    // AfxThrowXxxException() semantics. They don't abort — they throw a
    // C++ exception the caller's TRY/CATCH_ALL(e) (== try/catch(...)) is
    // expected to catch, so they CAN be exercised safely here.
    bool caughtMemory = false;
    try {
        AfxThrowMemoryException();
    } catch (const CMemoryException&) {
        caughtMemory = true;
    }
    CC_CHECK(caughtMemory);

    bool caughtUser = false;
    try {
        AfxThrowUserException();
    } catch (const CUserException&) {
        caughtUser = true;
    }
    CC_CHECK(caughtUser);
}

// --- Task 5 R9 shim additions: selftests -------------------------------------
// mfc_compat.h added these while lifting backdrop.cpp (Task 5): lstrcmpi
// (NotifyDownloadedBackdrop) and CMapWordToPtr/POSITION (backMapS/backMapP
// caches + FlushBackDropCache's iteration).

static void testShimStringApis2() {
    // lstrcmpi (backdrop.cpp NotifyDownloadedBackdrop).
    CC_CHECK(lstrcmpi("Anna", "anna") == 0);
    CC_CHECK(lstrcmpi("ABC", "abc") == 0);
    CC_CHECK(lstrcmpi("abc", "abd") != 0);
}

static void testMapWordToPtr() {
    // CMapWordToPtr + POSITION (backdrop.cpp backMapS/backMapP,
    // GetBackDropArtFromID/FlushBackDropFromID/FlushBackDropCache).
    int a = 1, b = 2, c = 3;
    CMapWordToPtr map(10);

    void* found = nullptr;
    CC_CHECK(map.Lookup(1, found) == FALSE);

    map.SetAt(1, &a);
    map.SetAt(2, &b);
    map.SetAt(3, &c);
    CC_CHECK(map.Lookup(2, found) == TRUE && found == &b);

    // Iterate via POSITION/GetNextAssoc and verify every key yields its exact
    // paired value (review fix: not just "a plausible key", the right value).
    int seen = 0;
    bool sawKey1 = false, sawKey2 = false, sawKey3 = false;
    POSITION pos = map.GetStartPosition();
    while (pos) {
        WORD key;
        void* value;
        map.GetNextAssoc(pos, key, value);
        CC_CHECK(key >= 1 && key <= 3);
        if (key == 1) { CC_CHECK(value == &a); sawKey1 = true; }
        else if (key == 2) { CC_CHECK(value == &b); sawKey2 = true; }
        else if (key == 3) { CC_CHECK(value == &c); sawKey3 = true; }
        seen++;
    }
    CC_CHECK(seen == 3);
    CC_CHECK(sawKey1 && sawKey2 && sawKey3);

    CC_CHECK(map.RemoveKey(2) == TRUE);
    CC_CHECK(map.Lookup(2, found) == FALSE);
    CC_CHECK(map.RemoveKey(2) == FALSE);  // already removed

    map.RemoveAll();
    CC_CHECK(map.Lookup(1, found) == FALSE);
    CC_CHECK(map.GetStartPosition() == nullptr);  // empty map has no iteration
}

static void testMapWordToPtrRemoveDuringIteration() {
    // Review fix (Important): MFC's documented CMapWordToPtr contract allows
    // RemoveKey() of any entry — including ones not yet visited — while a
    // GetStartPosition()/GetNextAssoc() iteration is in flight. The loop must
    // complete over the surviving entries, never yield the removed key, and
    // never throw (mid-iteration mutation is a legal, if easy-to-misuse,
    // real-MFC idiom — see backdrop.cpp's FlushBackDropCache callers).
    int a = 1, b = 2, c = 3;
    CMapWordToPtr map;
    map.SetAt(1, &a);
    map.SetAt(2, &b);
    map.SetAt(3, &c);

    // Snapshot the key set, then remove key 2 before the loop visits
    // anything — unambiguously "not yet visited" regardless of the (MFC-
    // unspecified) hash iteration order.
    POSITION pos = map.GetStartPosition();
    CC_CHECK(map.RemoveKey(2) == TRUE);

    int seen = 0;
    bool sawRemovedKey = false;
    while (pos) {
        WORD key;
        void* value;
        map.GetNextAssoc(pos, key, value);

        if (key == 1) CC_CHECK(value == &a);
        else if (key == 2) sawRemovedKey = true;  // must never happen
        else if (key == 3) CC_CHECK(value == &c);
        else CC_CHECK(false);  // unexpected key
        seen++;
    }
    // Loop completed without throwing (we got here), never yielded the
    // removed key, and still saw exactly the two surviving entries.
    CC_CHECK(!sawRemovedKey);
    CC_CHECK(seen == 2);

    void* found = nullptr;
    CC_CHECK(map.Lookup(1, found) == TRUE && found == &a);
    CC_CHECK(map.Lookup(2, found) == FALSE);
    CC_CHECK(map.Lookup(3, found) == TRUE && found == &c);
}

extern "C" int32_t cc_run_selftests(void) {
    g_failures = 0;
    testCString();
    testGeometry();
    testColor();
    testRasterOpConstants();
    testStructSizes();
    testCollections();
    testContext();
    testCStringMidClamping();
    testDibHelpers();
    testDibCreate();
    testShimFileApis();
    testShimStringApis();
    testShimCollections2();
    testShimMacros();
    testShimStringApis2();
    testMapWordToPtr();
    testMapWordToPtrRemoveDuringIteration();
    return g_failures;
}
