#include "comicchat.h"
#include "mfc_compat.h"
#include "engine_context.h"
#include "dib.h"
#include "cc_canvas.h"
#include "cc_recording_canvas.h"
#include "vector2d.h"
#include "traj.h"
#include "spline.h"
#include "format.h"
#include "bbox.h"
#include "pe.h"
#include "avatar.h"
#include "balloon.h"    // Task 6: CFontInfo/CBalloon/CBWoodring* + ::BreakIntoLines
#include "backdrop.h"
#include "panel.h"      // Task 6: CUnitPanelPage (SetFonts + font statics)
#include <unistd.h>   // mkstemp, close (testShimFileApis)

// ::BreakIntoLines free function (balloon.cpp) — declared here for the
// characterization test (balloon.h declares only the CLabel:: method wrapper).
int BreakIntoLines(CDC *pdc, int iMaxWidth, char *szString, CDWordArray *prgdwFormatting,
                   char *rgszStarts[], int rgiLengths[], int rgiWidths[]);

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

// --- Plan 2 Task 1: ccLog level gate selftest --------------------------------
// Entry debt fix: ccLog had no level gate at all. Verifies the gate itself
// (ccLogWouldEmit), not stdout/stderr content, per the brief.

static void cc_selftest_loglevel() {
    cc_set_log_level(2);  // start from a known state: default level
    CC_CHECK(ccLogWouldEmit(2) == 1);
    cc_set_log_level(1);
    CC_CHECK(ccLogWouldEmit(2) == 0);
    cc_set_log_level(2);
    CC_CHECK(ccLogWouldEmit(2) == 1);

    // Test clamping on both paths:
    // At level 0, neither level 1 nor 2 should emit
    cc_set_log_level(0);
    CC_CHECK(ccLogWouldEmit(1) == 0);
    CC_CHECK(ccLogWouldEmit(2) == 0);

    // At level 1, level 1 should emit but level 2 should not
    cc_set_log_level(1);
    CC_CHECK(ccLogWouldEmit(1) == 1);
    CC_CHECK(ccLogWouldEmit(2) == 0);

    // cc_set_log_level(99) should clamp to 2 (acts as level 2)
    cc_set_log_level(99);
    CC_CHECK(ccLogWouldEmit(2) == 1);

    // cc_set_log_level(-5) should clamp to 0 (acts as level 0)
    cc_set_log_level(-5);
    CC_CHECK(ccLogWouldEmit(1) == 0);
    CC_CHECK(ccLogWouldEmit(2) == 0);

    // Restore to level 2 (default) so later tests see TRACE behavior
    cc_set_log_level(2);
}

// --- Plan 2 Task 2: canvas C vtable + C++ wrapper + recording canvas --------
// Drives every cc_canvas_ops entry once through CCanvas, over a
// CCRecordingCanvas, and asserts the exact log line each op produces. The
// log format is a stable contract documented in cc_recording_canvas.h; every
// later task's selftests assert against these exact shapes, so this test
// must not drift from that format.

static void cc_selftest_canvas() {
    CCRecordingCanvas rec;
    CCanvas canvas(rec.handle());

    cc_font_spec font;
    memset(&font, 0, sizeof(font));
    strcpy(font.face, "Comic Sans MS");
    font.height = -240;
    font.weight = 400;

    int32_t w = 0, h = 0;
    canvas.measure_text(&font, "hello", 5, &w, &h);
    CC_CHECK(w == 600 && h == 240);

    cc_text_metrics tm;
    memset(&tm, 0, sizeof(tm));
    canvas.font_metrics(&font, &tm);
    CC_CHECK(tm.height == 240);
    CC_CHECK(tm.ascent == 190);
    CC_CHECK(tm.descent == 50);
    CC_CHECK(tm.internal_leading == 40);
    CC_CHECK(tm.external_leading == 20);
    CC_CHECK(tm.ave_char_width == 120);
    CC_CHECK(tm.max_char_width == 240);

    canvas.draw_text(&font, 100, -200, 0x00000000, 0, 0x00FFFFFF, "hi", 2);
    canvas.fill_rect(0, 0, 2400, -2400, 0x00FFFFFF);

    cc_image img;
    memset(&img, 0, sizeof(img));
    canvas.draw_image(&img, 0, 0, 1200, -1600, 0, 0, 60, 80);

    cc_path_pt pts[5];
    pts[0] = { CC_PATH_MOVE, 0, 0 };
    pts[1] = { CC_PATH_LINE, 10, 0 };
    pts[2] = { CC_PATH_CUBIC, 20, 0 };
    pts[3] = { CC_PATH_CUBIC, 30, 10 };
    pts[4] = { CC_PATH_CUBIC, 40, 10 };
    canvas.path(pts, 5, 1, 0x000000FF, 1, 0x00FF0000, 20, 0);

    canvas.clip_push(0, 0, 2400, -2400);
    canvas.clip_pop();

    int32_t printing = canvas.is_printing();
    CC_CHECK(printing == 0);

    const std::vector<std::string>& log = rec.log();
    CC_CHECK(log.size() == 6);
    size_t i = 0;
    CC_CHECK(i < log.size() && log[i++] == "text 100,-200 color=000000 \"hi\"");
    CC_CHECK(i < log.size() && log[i++] == "rect 0,0,2400,-2400 fill=FFFFFF");
    CC_CHECK(i < log.size() && log[i++] == "image 0,0,1200,-1600 src=0,0,60,80");
    CC_CHECK(i < log.size() && log[i++] ==
        "path n=5 fill=1 fillc=FF0000 stroke=1 strokec=0000FF w=20 dashed=0 [M 0,0 L 10,0 C 20,0 30,10 40,10]");
    CC_CHECK(i < log.size() && log[i++] == "clip+ 0,0,2400,-2400");
    CC_CHECK(i < log.size() && log[i++] == "clip-");
}

// --- Plan 2 Task 3: CDC adapter over cc_canvas -------------------------------
// Drives a concrete CDC bound to a CCRecordingCanvas through every Produces
// member, asserting the exact log lines the recording canvas's documented
// grammar (cc_recording_canvas.h) says each op must produce. Cases (a)-(i)
// per the task brief; kept as one function per that brief's Step 1.

static void cc_selftest_dc() {
    CCRecordingCanvas rec;
    CDC dc(rec.handle());

    // (a) font select + GetTextExtent("hello",5) == 600x240;
    //     GetTextMetrics height 240/ascent 190.
    LOGFONT lf;
    memset(&lf, 0, sizeof(lf));
    strcpy(lf.lfFaceName, "Comic Sans MS");
    lf.lfHeight = -240;
    lf.lfWeight = 400;
    CFont font;
    font.CreateFontIndirect(&lf);
    CFont* pOldFont = dc.SelectObject(&font);
    CC_CHECK(pOldFont == nullptr);  // nothing selected before

    CSize extent = dc.GetTextExtent("hello", 5);
    CC_CHECK(extent.cx == 600 && extent.cy == 240);

    TEXTMETRIC tm;
    memset(&tm, 0, sizeof(tm));
    dc.GetTextMetrics(&tm);
    CC_CHECK(tm.tmHeight == 240);
    CC_CHECK(tm.tmAscent == 190);

    // (b) TextOut log line carries color/bk state (observable part: color).
    dc.SetTextColor(RGB(1, 2, 3));
    dc.SetBkMode(OPAQUE);
    dc.SetBkColor(RGB(4, 5, 6));
    dc.TextOut(0, 0, "hi", 2);
    {
        const std::vector<std::string>& log = rec.log();
        CC_CHECK(!log.empty());
        CC_CHECK(log.back() == "text 0,0 color=010203 \"hi\"");
    }

    // (c) origin: after SetWindowOrg(100,50), TextOut(100,50,...) logs
    //     "text 0,0" (emitted coord = input - org).
    dc.SetWindowOrg(100, 50);
    dc.TextOut(100, 50, "x", 1);
    CC_CHECK(rec.log().back() == "text 0,0 color=010203 \"x\"");
    // OffsetWindowOrg accumulates.
    dc.OffsetWindowOrg(10, 10);  // org now (110, 60)
    dc.TextOut(110, 60, "y", 1);
    CC_CHECK(rec.log().back() == "text 0,0 color=010203 \"y\"");
    dc.OffsetWindowOrg(-10, -10);
    dc.SetWindowOrg(0, 0);  // reset for subsequent cases

    // (d) clip push/intersect/reset sequence logs clip+/clip- correctly.
    {
        size_t before = rec.log().size();
        CRect oldClip;
        dc.GetClipBox(&oldClip);
        // Base clip is the +/-2^28 sentinel.
        CC_CHECK(oldClip.left == -(1 << 28) && oldClip.top == (1 << 28));
        CC_CHECK(oldClip.right == (1 << 28) && oldClip.bottom == -(1 << 28));

        CRect r1(0, 0, 2400, -2400);
        dc.IntersectClipRect(&r1);
        CRect afterFirst;
        dc.GetClipBox(&afterFirst);
        CC_CHECK(afterFirst.left == 0 && afterFirst.top == 0);
        CC_CHECK(afterFirst.right == 2400 && afterFirst.bottom == -2400);

        CRect r2(100, -100, 2000, -2000);
        dc.IntersectClipRect(&r2);
        CRect afterSecond;
        dc.GetClipBox(&afterSecond);
        // intersection of (0,0,2400,-2400) and (100,-100,2000,-2000)
        CC_CHECK(afterSecond.left == 100 && afterSecond.top == -100);
        CC_CHECK(afterSecond.right == 2000 && afterSecond.bottom == -2000);

        dc.SelectClipRgn(NULL, RGN_COPY);
        CRect afterReset;
        dc.GetClipBox(&afterReset);
        CC_CHECK(afterReset.left == -(1 << 28) && afterReset.top == (1 << 28));
        CC_CHECK(afterReset.right == (1 << 28) && afterReset.bottom == -(1 << 28));

        const std::vector<std::string>& log = rec.log();
        CC_CHECK(log.size() == before + 3);
        CC_CHECK(log[before] == "clip+ 0,0,2400,-2400");
        CC_CHECK(log[before + 1] == "clip+ 100,-100,2000,-2000");
        CC_CHECK(log[before + 2] == "clip-");
    }

    // (d, continued) clip rects are logical coordinates: window origin
    // applies to IntersectClipRect's input and GetClipBox's output, exactly
    // like every other coordinate this adapter emits. A clip set under one
    // origin does not retroactively move when the origin later changes
    // (real GDI semantics -- the region is fixed in device/canvas space).
    {
        size_t before = rec.log().size();
        dc.SetWindowOrg(100, 50);
        CRect r(100, 50, 500, -50);  // logical (100,50)-(500,-50) under this origin
        dc.IntersectClipRect(&r);    // canvas space: (0,0)-(400,-100)
        CC_CHECK(rec.log().back() == "clip+ 0,0,400,-100");

        CRect box;
        dc.GetClipBox(&box);  // read back under the SAME origin -> same rect
        CC_CHECK(box.left == 100 && box.top == 50 && box.right == 500 && box.bottom == -50);

        dc.SetWindowOrg(0, 0);  // origin changes; clip stays fixed in canvas space
        CRect boxAfterOriginChange;
        dc.GetClipBox(&boxAfterOriginChange);
        CC_CHECK(boxAfterOriginChange.left == 0 && boxAfterOriginChange.top == 0);
        CC_CHECK(boxAfterOriginChange.right == 400 && boxAfterOriginChange.bottom == -100);

        dc.SelectClipRgn(NULL, RGN_COPY);  // reset for subsequent cases
        const std::vector<std::string>& log = rec.log();
        CC_CHECK(log.size() == before + 2);
        CC_CHECK(log[before + 1] == "clip-");
    }

    // Set up pen/brush for path cases.
    CPen pen;
    pen.CreatePen(PS_SOLID, 20, RGB(0xFF, 0, 0));
    CPen* pOldPen = dc.SelectObject(&pen);
    (void)pOldPen;
    CBrush brush;
    brush.CreateSolidBrush(RGB(0, 0, 0xFF));
    CBrush* pOldBrush = dc.SelectObject(&brush);
    (void)pOldBrush;

    // (e) BeginPath..MoveTo(0,0),LineTo(10,0),CloseFigure,EndPath,StrokePath
    //     logs one path with M/L/Z.
    {
        size_t before = rec.log().size();
        dc.BeginPath();
        dc.MoveTo(0, 0);
        dc.LineTo(10, 0);
        dc.CloseFigure();
        dc.EndPath();
        dc.StrokePath();
        const std::vector<std::string>& log = rec.log();
        CC_CHECK(log.size() == before + 1);
        CC_CHECK(log[before] ==
            "path n=3 fill=0 fillc=0000FF stroke=1 strokec=FF0000 w=20 dashed=0 [M 0,0 L 10,0 Z]");
    }

    // (f) bare MoveTo/LineTo (outside BeginPath/EndPath) logs an immediate
    //     2-pt path.
    {
        size_t before = rec.log().size();
        dc.MoveTo(5, 5);
        dc.LineTo(15, 5);
        const std::vector<std::string>& log = rec.log();
        CC_CHECK(log.size() == before + 1);
        CC_CHECK(log[before] ==
            "path n=2 fill=0 fillc=0000FF stroke=1 strokec=FF0000 w=20 dashed=0 [M 5,5 L 15,5]");
    }

    // (g) Ellipse(0,0,100,-100) logs a 12-entry cubic path (4x3).
    {
        size_t before = rec.log().size();
        dc.Ellipse(0, 0, 100, -100);
        const std::vector<std::string>& log = rec.log();
        CC_CHECK(log.size() == before + 1);
        const std::string& line = log[before];
        CC_CHECK(line.substr(0, 6) == "path n");
        CC_CHECK(line.find("n=12 ") != std::string::npos);
        CC_CHECK(line.find("fill=1") != std::string::npos);
        CC_CHECK(line.find("stroke=1") != std::string::npos);
        // 4 cubic triples, no M/L/Z entries.
        CC_CHECK(line.find("M ") == std::string::npos);
        CC_CHECK(line.find("L ") == std::string::npos);
        CC_CHECK(line.find("Z") == std::string::npos);
        size_t cCount = 0;
        size_t pos = 0;
        while ((pos = line.find("C ", pos)) != std::string::npos) { cCount++; pos += 2; }
        CC_CHECK(cCount == 4);
    }

    // (g, continued) Ellipse's rect is logical coordinates too -- window
    // origin shifts every emitted point by the same (dx,dy), same as every
    // other coordinate this adapter emits.
    {
        dc.SetWindowOrg(10, 20);
        size_t before = rec.log().size();
        dc.Ellipse(10, 20, 110, -80);  // same shape as the (g) case, offset by the org
        dc.SetWindowOrg(0, 0);
        const std::vector<std::string>& log = rec.log();
        CC_CHECK(log.size() == before + 1);
        CC_CHECK(log[before] == log[before - 1]);  // identical to the un-offset (g) line
    }

    // (h) FillSolidRect logs rect.
    {
        size_t before = rec.log().size();
        CRect r(0, 0, 500, -500);
        dc.FillSolidRect(&r, RGB(9, 9, 9));
        const std::vector<std::string>& log = rec.log();
        CC_CHECK(log.size() == before + 1);
        CC_CHECK(log[before] == "rect 0,0,500,-500 fill=090909");
    }
    {
        size_t before = rec.log().size();
        dc.FillSolidRect(0, 0, 500, -500, RGB(9, 9, 9));
        const std::vector<std::string>& log = rec.log();
        CC_CHECK(log.size() == before + 1);
        CC_CHECK(log[before] == "rect 0,0,500,-500 fill=090909");
    }

    // (i) GetDeviceCaps(LOGPIXELSY) == 1440.
    CC_CHECK(dc.GetDeviceCaps(LOGPIXELSY) == 1440);
    CC_CHECK(dc.GetDeviceCaps(LOGPIXELSX) == 1440);

    // Misc adapter members: m_bPrinting, IsPrinting, GetSafeHdc,
    // GetCurrentFont, palette/stretch-mode no-ops.
    CC_CHECK(dc.m_bPrinting == FALSE);
    CC_CHECK(dc.IsPrinting() == FALSE);
    CC_CHECK(dc.GetSafeHdc() != nullptr);
    CC_CHECK(dc.GetCurrentFont() == &font);

    CPalette* pal = GetCurrentPalette(&dc);
    CC_CHECK(pal == nullptr);
    SelectPalette(&dc, nullptr, TRUE);
    RealizePalette(&dc);
    SetStretchBltMode(&dc, 0);
    POINT brushOrg;
    GetBrushOrgEx(&dc, &brushOrg);
    SetBrushOrgEx(&dc, 0, 0, &brushOrg);

    // CClientDC binds to ccContext().metricsCanvas by default.
    cc_set_metrics_canvas(rec.handle());
    CClientDC clientDc;
    CC_CHECK(clientDc.GetDeviceCaps(LOGPIXELSY) == 1440);

    // ccContext().metricsDC(): lazily-constructed CDC bound to
    // metricsCanvas, same instance on every call (R17).
    CDC* mdc1 = ccContext().metricsDC();
    CC_CHECK(mdc1 != nullptr);
    CDC* mdc2 = ccContext().metricsDC();
    CC_CHECK(mdc1 == mdc2);
    CC_CHECK(mdc1->GetDeviceCaps(LOGPIXELSY) == 1440);

    // restore selections
    dc.SelectObject(pOldPen);
    dc.SelectObject(pOldBrush);
    dc.SelectObject(pOldFont);
}

static void cc_selftest_canvas_truncated_cubic() {
    // Test that a path with a truncated cubic run (incomplete triple) logs
    // the verbs before the truncation and stops safely without reading past
    // the pts array bounds.
    CCRecordingCanvas rec;
    CCanvas canvas(rec.handle());

    // Create a path: MOVE, LINE, then single CUBIC (incomplete triple).
    cc_path_pt pts[3];
    pts[0] = { CC_PATH_MOVE, 0, 0 };
    pts[1] = { CC_PATH_LINE, 10, 0 };
    pts[2] = { CC_PATH_CUBIC, 20, 0 };  // Incomplete triple: need pts[3] and pts[4]
    canvas.path(pts, 3, 1, 0x00FFFFFF, 0, 0x00000000, 1, 0);

    const std::vector<std::string>& log = rec.log();
    CC_CHECK(log.size() == 1);
    // The path should contain the MOVE and LINE, but stop before the
    // truncated cubic. Truncation logs an error via ccLog, which doesn't
    // fail the path — it just stops parsing. The log line should end at
    // the last complete verb (L 10,0), with no C triple following.
    CC_CHECK(log[0].find("M 0,0") != std::string::npos);
    CC_CHECK(log[0].find("L 10,0") != std::string::npos);
    // Verify that the cubic triple did NOT make it into the output.
    CC_CHECK(log[0].find("C ") == std::string::npos);
}

// --- Task 4 review fix round 1: CDC::PolyBezierTo + CMapStringToPtr --------
// Task 4 added five R9 shim members; two (CDC::PolyBezierTo, CMapStringToPtr)
// had no selftest coverage at all. Review flagged this against R9's own
// mandate ("every shim addition gets a selftest exercising it"). Grouped
// here, same pattern as the Task 4/5 shim-selftest blocks above.

static void testMapStringToPtr() {
    // CMapStringToPtr (spline.cpp's betaMatrixMap, keyed by a "%f*%f"
    // tension/bias string -- see CBeta::SetMatrix). Same Lookup/SetAt/
    // RemoveKey/RemoveAll/POSITION-iteration shape as CMapWordToPtr, just
    // keyed by string. Exercise every member the shim implements: ctor,
    // Lookup (hit + miss), SetAt (incl. overwrite), RemoveKey, RemoveAll,
    // GetStartPosition/GetNextAssoc.
    int a = 1, b = 2;
    CMapStringToPtr map(10);

    void* found = nullptr;
    CC_CHECK(map.Lookup("0.400000*1.000000", found) == FALSE);  // miss: empty map

    map.SetAt("0.400000*1.000000", &a);
    map.SetAt("5.000000*1.000000", &b);
    CC_CHECK(map.Lookup("0.400000*1.000000", found) == TRUE && found == &a);
    CC_CHECK(map.Lookup("5.000000*1.000000", found) == TRUE && found == &b);
    CC_CHECK(map.Lookup("9.000000*9.000000", found) == FALSE);  // miss: never set

    // Overwrite an existing key.
    int c = 3;
    map.SetAt("0.400000*1.000000", &c);
    CC_CHECK(map.Lookup("0.400000*1.000000", found) == TRUE && found == &c);

    // POSITION iteration sees both surviving keys with their exact values.
    int seen = 0;
    bool sawFirst = false, sawSecond = false;
    POSITION pos = map.GetStartPosition();
    while (pos) {
        CString key;
        void* value;
        map.GetNextAssoc(pos, key, value);
        if (key == "0.400000*1.000000") { CC_CHECK(value == &c); sawFirst = true; }
        else if (key == "5.000000*1.000000") { CC_CHECK(value == &b); sawSecond = true; }
        else CC_CHECK(false);  // unexpected key
        seen++;
    }
    CC_CHECK(seen == 2);
    CC_CHECK(sawFirst && sawSecond);

    CC_CHECK(map.RemoveKey("5.000000*1.000000") == TRUE);
    CC_CHECK(map.Lookup("5.000000*1.000000", found) == FALSE);
    CC_CHECK(map.RemoveKey("5.000000*1.000000") == FALSE);  // already removed

    map.RemoveAll();
    CC_CHECK(map.Lookup("0.400000*1.000000", found) == FALSE);
    CC_CHECK(map.GetStartPosition() == nullptr);  // empty map has no iteration
}

// --- Plan 2 Task 4: pure-geometry lift (defines.h, spline, splinutl, traj) ---
// (a) Builds a CTraj from 3 known points via two CLine segments, draws it
//     through a recording-canvas-bound CDC, and asserts the exact logged
//     path string per cc_recording_canvas.h's grammar.
// (b) A hand-computed numeric check of splinutl.cpp's split_bezier() (De
//     Casteljau bisection) -- picked because its arithmetic is simple enough
//     to verify by hand and unambiguous (no MFC/rounding involved, it
//     operates on DPOINT doubles throughout).
// (c) Review fix round 1: CDC::PolyBezierTo, exercised through its one real
//     call site, CSpline::Draw (spline.cpp:299): `dc->PolyBezierTo(bezpts+1,
//     BezierCount()-1)`. Built the smallest possible concrete spline (a
//     CCardinal with exactly 2 control points, unclosed) so bezpts has only
//     4 entries and Draw emits exactly one PolyBezierTo call with 3 points
//     (one C triple) -- and hand-computed those bezpts from the real
//     CCardinal matrix math below. This exercises the actual production
//     code path rather than driving PolyBezierTo standalone, which is a
//     strictly stronger test for the same effort.

static void cc_selftest_geometry() {
    // (a) CTraj of 2 CLine segs over 3 known points: P0=(0,0), P1=(100,0),
    // P2=(100,50). CTraj::Draw does: BeginPath(); MoveTo(firstSeg->SegLo());
    // for each seg, seg->Draw(dc); [CloseFigure() if m_closed]; EndPath().
    // CLine::Draw(dc) is just dc->LineTo(m_hi). So the accumulated path is:
    //   MoveTo(P0) -> M 0,0
    //   seg1 (P0->P1).Draw -> LineTo(P1) -> L 100,0
    //   seg2 (P1->P2).Draw -> LineTo(P2) -> L 100,50
    // m_closed defaults to FALSE (CTraj ctor), so no CloseFigure/Z entry.
    {
        CCRecordingCanvas rec;
        CDC dc(rec.handle());

        POINT p0{0, 0}, p1{100, 0}, p2{100, 50};
        CTraj traj;
        traj.AddSeg(new CLine(p0, p1));
        traj.AddSeg(new CLine(p1, p2));
        CC_CHECK(traj.m_closed == FALSE);

        CPen pen;
        pen.CreatePen(PS_SOLID, 10, RGB(0xFF, 0, 0));   // red stroke
        dc.SelectObject(&pen);
        CBrush brush;
        brush.CreateSolidBrush(RGB(0, 0, 0xFF));        // blue fill (unused: stroke only)
        dc.SelectObject(&brush);

        traj.Draw(&dc);      // accumulates BeginPath..EndPath
        dc.StrokePath();     // emits the log line (balloon.cpp:1789/1794's
                              // real-code pattern: m_traj->Draw(pdc); pdc->
                              // StrokePath()/StrokeAndFillPath())

        const std::vector<std::string>& log = rec.log();
        CC_CHECK(log.size() == 1);
        CC_CHECK(log[0] ==
            "path n=3 fill=0 fillc=0000FF stroke=1 strokec=FF0000 w=10 dashed=0 [M 0,0 L 100,0 L 100,50]");
    }

    // (b) split_bezier() hand-computed check. Control polygon (an S-curve,
    // not collinear, so both axes and the midpoint arithmetic are exercised):
    //   p0=(0,0) p1=(0,10) p2=(10,10) p3=(10,0)
    // De Casteljau bisection (algorithm per the code's own header comment,
    // traced with the exact variable names/order used in split_bezier()):
    //   left.p0  = p0                            = (0,0)
    //   left.p1  = 0.5*(p0+p1)                    = 0.5*(0,10)          = (0,5)
    //   t        = 0.5*(p1+p2)                    = 0.5*(10,20)         = (5,10)
    //   left.p2  = 0.5*(left.p1+t)                = 0.5*((0,5)+(5,10))  = (2.5,7.5)
    //   right.p3 = p3                              = (10,0)
    //   right.p2 = 0.5*(p2+p3)                    = 0.5*(20,10)         = (10,5)
    //   right.p1 = 0.5*(t+right.p2)               = 0.5*((5,10)+(10,5))= (7.5,7.5)
    //   left.p3 = right.p0 = 0.5*(left.p2+right.p1) = 0.5*((2.5,7.5)+(7.5,7.5)) = (5,7.5)
    {
        void split_bezier(BEZIER *b, BEZIER *left, BEZIER *right);  // splinutl.cpp

        BEZIER b, left, right;
        b.p0 = {0.0, 0.0};
        b.p1 = {0.0, 10.0};
        b.p2 = {10.0, 10.0};
        b.p3 = {10.0, 0.0};
        split_bezier(&b, &left, &right);

        CC_CHECK(left.p0.x == 0.0 && left.p0.y == 0.0);
        CC_CHECK(left.p1.x == 0.0 && left.p1.y == 5.0);
        CC_CHECK(left.p2.x == 2.5 && left.p2.y == 7.5);
        CC_CHECK(left.p3.x == 5.0 && left.p3.y == 7.5);
        CC_CHECK(right.p0.x == 5.0 && right.p0.y == 7.5);
        CC_CHECK(right.p1.x == 7.5 && right.p1.y == 7.5);
        CC_CHECK(right.p2.x == 10.0 && right.p2.y == 5.0);
        CC_CHECK(right.p3.x == 10.0 && right.p3.y == 0.0);
        // left/right share the split point (De Casteljau: T0 == S3).
        CC_CHECK(left.p3.x == right.p0.x && left.p3.y == right.p0.y);
    }

    // (c) CDC::PolyBezierTo, exercised via CSpline::Draw. CCardinal ctor
    // requires n >= 2 control points (CSpline ctor's ASSERT); with exactly
    // 2 -- P0=(0,0), P1=(100,50) -- and unclosed:
    //   GetDups() = 2 (CCardinal), so KnotCount() = nCps + 2 = 4,
    //   BezierCount() = 3*KnotCount() - 8 = 4 -- the minimum possible, i.e.
    //   exactly one bezier segment (bezpts[0..3]).
    // GetKnot(index) for the unclosed case (dups=2, nCps=2):
    //   index 0: 0 < dups(2)              -> cps[0] = (0,0)
    //   index 1: 1 < dups(2)              -> cps[0] = (0,0)
    //   index 2: nCps+dups-2 = 2, 2 >= 2  -> cps[nCps-1] = cps[1] = (100,50)
    //   index 3: 3 >= 2                   -> cps[1] = (100,50)
    // So the 4 knots fed to ComputeBezpts are k0=k1=(0,0), k2=k3=(100,50).
    //
    // CCardinal::SetMatrix(tension=defaultTension=0.4) builds (per
    // spline.cpp's own assignments, (*matrix)[row][col]):
    //   row0 = [-0.4,  1.6, -1.6,  0.4]
    //   row1 = [ 0.8, -2.6,  2.2, -0.4]
    //   row2 = [-0.4,  0.0,  0.4,  0.0]
    //   row3 = [ 0.0,  1.0,  0.0,  0.0]
    // CvertsToCubic computes c3=row0.k, c2=row1.k, c1=row2.k, c0=row3.k
    // (each component ROUND()ed independently), against k0=(0,0), k1=(0,0),
    // k2=(100,50), k3=(100,50):
    //   c3.x = ROUND(-1.6*100 + 0.4*100) = ROUND(-120)      = -120
    //   c3.y = ROUND(-1.6*50  + 0.4*50)  = ROUND(-60)       = -60
    //   c2.x = ROUND( 2.2*100 - 0.4*100) = ROUND(180)       = 180
    //   c2.y = ROUND( 2.2*50  - 0.4*50)  = ROUND(90)        = 90
    //   c1.x = ROUND( 0.4*100 + 0.0*100) = ROUND(40)        = 40
    //   c1.y = ROUND( 0.4*50  + 0.0*50)  = ROUND(20)        = 20
    //   c0.x = ROUND( 1.0*0)             = 0
    //   c0.y = ROUND( 1.0*0)             = 0
    // so c0=(0,0), c1=(40,20), c2=(180,90), c3=(-120,-60).
    // CubicToBezier:
    //   b0 = c0                                              = (0,0)
    //   b1.x = c0.x + ROUND(c1.x/3) = 0 + ROUND(13.33)       = 13
    //   b1.y = c0.y + ROUND(c1.y/3) = 0 + ROUND(6.67)        = 7
    //   b2.x = b1.x + ROUND((c1.x+c2.x)/3) = 13+ROUND(73.33) = 86
    //   b2.y = b1.y + ROUND((c1.y+c2.y)/3) = 7+ROUND(36.67)  = 44
    //   b3 = c0+c1+c2+c3 (componentwise sum)                 = (100,50)
    // so bezpts = [(0,0), (13,7), (86,44), (100,50)].
    // CSpline::Draw does: dc->PolyBezierTo(bezpts+1, BezierCount()-1), i.e.
    // PolyBezierTo([(13,7),(86,44),(100,50)], 3) -- one C triple, no
    // BeginPath/EndPath/MoveTo of its own (the caller is expected to bracket
    // it, exactly like balloon.cpp's real usage bracket CSpline::Draw calls).
    {
        CCRecordingCanvas rec;
        CDC dc(rec.handle());

        POINT p0{0, 0}, p1{100, 50};
        POINT cpArray[2] = {p0, p1};
        CCardinal cardinal(cpArray, 2, FALSE);
        CC_CHECK(cardinal.KnotCount() == 4);
        CC_CHECK(cardinal.BezierCount() == 4);
        CC_CHECK(cardinal.bezpts[0].x == 0 && cardinal.bezpts[0].y == 0);
        CC_CHECK(cardinal.bezpts[1].x == 13 && cardinal.bezpts[1].y == 7);
        CC_CHECK(cardinal.bezpts[2].x == 86 && cardinal.bezpts[2].y == 44);
        CC_CHECK(cardinal.bezpts[3].x == 100 && cardinal.bezpts[3].y == 50);

        CPen pen;
        pen.CreatePen(PS_SOLID, 5, RGB(0, 0xFF, 0));    // green stroke
        dc.SelectObject(&pen);
        CBrush brush;
        brush.CreateSolidBrush(RGB(0xFF, 0xFF, 0));     // yellow fill (unused: stroke only)
        dc.SelectObject(&brush);

        dc.BeginPath();
        dc.MoveTo(cardinal.SegLo());  // bezpts[0] = (0,0)
        cardinal.Draw(&dc);           // dc->PolyBezierTo(bezpts+1, 3)
        dc.EndPath();
        dc.StrokePath();

        const std::vector<std::string>& log = rec.log();
        CC_CHECK(log.size() == 1);
        CC_CHECK(log[0] ==
            "path n=4 fill=0 fillc=FFFF00 stroke=1 strokec=00FF00 w=5 dashed=0 "
            "[M 0,0 C 13,7 86,44 100,50]");
    }
}

// --- Plan 2 Task 5: format.h/.cpp formatting/measurement half ---------------
// (a) GetFormattedTextExtent(dc, "hello", 5, NULL): the lifted code's actual
//     NULL-formatting handling is bSizorPresent(NULL) -- which returns FALSE
//     on its very first line ("if (!prgdwFormatting) return FALSE;") -- so
//     GetFormattedTextExtent takes its early-return branch
//     (`if (!bSizorPresent(...)) return pdc->GetTextExtent(...)`) and never
//     touches the per-run font-swap loop at all. Under the recording
//     canvas's fake metrics (120 twips/byte, height 240 regardless of
//     style), GetTextExtent("hello", 5) = 5*120=600 wide, 240 tall.
// (b) Two-run case built via InsertFormat(NULL, TRUE, wBold, 2): since the
//     array starts NULL, InsertFormat takes its "empty array" branch
//     (ASSERT(bAddFormat) passes, ignores the find-insertion-point search)
//     and produces a single entry MAKELONG(wBold, 2) -- format=wBold from
//     offset 2 onward. Feeding "aabb" (len 4) through GetFormattedTextExtent
//     with that 1-entry array: bSizorPresent sees wFormat & wBold set on
//     that entry -> TRUE, so the per-run loop runs. Run 1 measures
//     szInput[0..2) = "aa" (2 bytes * 120 = 240). After the loop's single
//     iteration (iUpper=0), the trailing tail (wMaxLen=4 > wCurLen=2) is
//     measured too: szInputTmp = szInput+2 = "bb", length wMaxLen-wCurLen=2
//     -> 240. Fake metrics ignore the lfWeight=700 style swap, so total
//     width = 240+240 = 480, height = max(240,240) = 240.
// (c) SzControlLess("a\x02b", &arr): iterates byte-by-byte. 'a' opens the
//     output buffer (szOutput=szWrite=szInput). chCtlBold (0x02) is a
//     control byte: SzSkipOneFormat sets wFormat |= wBold (0x0100) via the
//     output param and returns szRead+1 (the control byte's *own* position
//     + 1, since the chCtlBold case doesn't itself advance szRead before
//     the switch's break) -- i.e. skips exactly the one control byte,
//     landing back on 'b'. bNewFormatInPlace is now TRUE. 'b' is copied to
//     szWrite (now szInput+1, i.e. output offset 1), and because
//     bNewFormatInPlace is TRUE, exactly one formatting entry is appended:
//     MAKELONG(wBold, szWrite-szOutput) = MAKELONG(0x0100, 1) -- "from
//     output offset 1 onward, bold is active" (the 'b' that followed
//     chCtlBold in the original text). The loop then hits the NUL and
//     terminates the output at szWrite+1 (output offset 2). Net effect:
//     output string "ab" (control byte stripped), formatting array has
//     exactly 1 entry packing format=wBold(0x0100) at offset=1.

static void cc_selftest_format() {
    CCRecordingCanvas rec;
    CDC dc(rec.handle());
    LOGFONT lf;
    memset(&lf, 0, sizeof(lf));
    strcpy(lf.lfFaceName, "Comic Sans MS");
    lf.lfHeight = -240;
    lf.lfWeight = 400;
    CFont font;
    font.CreateFontIndirect(&lf);
    dc.SelectObject(&font);

    // (a) NULL formatting -> bSizorPresent(NULL) is FALSE -> early-return
    // branch -> plain GetTextExtent("hello", 5) = 600x240.
    {
        char text[] = "hello";
        CSize extent = GetFormattedTextExtent(&dc, text, 5, NULL);
        CC_CHECK(extent.cx == 600 && extent.cy == 240);
    }

    // (b) Two-run case: "aabb" with a bold switch at offset 2, built via
    // InsertFormat on a NULL array. Expect width 480 (run-splitting
    // arithmetic; style itself is ignored by the fake metrics).
    {
        CDWordArray* arr = InsertFormat(NULL, TRUE, wBold, 2);
        CC_CHECK(arr != NULL);
        CC_CHECK(arr->GetSize() == 1);
        CC_CHECK(LOWORD(arr->GetAt(0)) == wBold);
        CC_CHECK(HIWORD(arr->GetAt(0)) == 2);

        char text[] = "aabb";
        CSize extent = GetFormattedTextExtent(&dc, text, 4, arr);
        CC_CHECK(extent.cx == 480 && extent.cy == 240);

        FreeAndNullFormatting(&arr);
        CC_CHECK(arr == NULL);
    }

    // (c) SzControlLess strips chCtlBold (0x02) from "a\x02b" -> "ab",
    // producing exactly 1 formatting entry: format=wBold at offset=1.
    {
        char text[] = { 'a', chCtlBold, 'b', '\0' };
        CDWordArray arr;
        char* result = SzControlLess(text, &arr);
        CC_CHECK(strcmp(result, "ab") == 0);
        CC_CHECK(arr.GetSize() == 1);
        CC_CHECK(LOWORD(arr.GetAt(0)) == wBold);
        CC_CHECK(HIWORD(arr.GetAt(0)) == 1);
    }

    dc.SelectObject((CFont*)NULL);
}

// --- Plan 2 Task 6: balloon layout engine ------------------------------------
// Step 1 (R17): session settings struct + metricsDC() verification. The
// original layout code reached theApp.m_comicsColor/m_charSet/m_comicsFont/
// m_szGuiFaceName/m_iFontHeightBalloon/m_flags1 for its font + formatting
// defaults; those become ccContext().session fields (R17). This first test
// pins the documented default values every field is initialized to.

static void cc_selftest_balloon() {
    // (Step 1) session defaults, per the task brief's Step 1 struct plus the
    // extra fields fonts.cpp/balloon.h actually reach (R17: doc/settings via
    // ccContext().session). Defaults chosen to match the original's shipping
    // registry/resource defaults: black comics text, "Comic Sans MS" 12pt.
    CCEngineContext& ctx = ccContext();
    CC_CHECK(ctx.session.comicsColor == RGB(0, 0, 0));
    CC_CHECK(strcmp(ctx.session.comicsFontFace, "Comic Sans MS") == 0);
    CC_CHECK(ctx.session.comicsFontPts == 12);
    CC_CHECK(ctx.session.charSet == 0);
    CC_CHECK(ctx.session.iFontHeightBalloon == 240);  // 12pt * 20 twips/pt
    CC_CHECK(strcmp(ctx.session.guiFaceName, "Comic Sans MS") == 0);
    CC_CHECK(ctx.session.flags1 == 0);

    // (R9 shim additions for balloon.cpp/fonts.cpp) --------------------------
    cc_set_metrics_canvas(nullptr);  // reset (each metricsDC() call below is
                                     // separate from the CClientDC lifetime)
    CCRecordingCanvas shimRec;
    CDC shimDc(shimRec.handle());
    LOGFONT slf;
    memset(&slf, 0, sizeof(slf));
    strcpy(slf.lfFaceName, "Comic Sans MS");
    slf.lfHeight = -240;
    slf.lfWeight = 400;
    CFont sfont;
    sfont.CreateFontIndirect(&slf);
    shimDc.SelectObject(&sfont);

    // GetTextMetrics now returns BOOL (fonts.cpp: VERIFY(pDc->GetTextMetrics))
    // and fills tmCharSet from the selected font's charset.
    TEXTMETRIC stm;
    memset(&stm, 0, sizeof(stm));
    BOOL gotMetrics = shimDc.GetTextMetrics(&stm);
    CC_CHECK(gotMetrics == TRUE);
    CC_CHECK(stm.tmHeight == 240);
    CC_CHECK(stm.tmCharSet == 0);  // slf.lfCharSet was 0 (DEFAULT via memset)

    // GetTextFace returns the selected font's face name (fonts.cpp doVKern
    // "Comic Sans MS" comparison), returning its length.
    char face[LF_FACESIZE];
    int faceLen = shimDc.GetTextFace(LF_FACESIZE, face);
    CC_CHECK(faceLen == (int)strlen("Comic Sans MS"));
    CC_CHECK(strcmp(face, "Comic Sans MS") == 0);

    // CPtrList::AddHead prepends; GetHeadPosition/GetNext then walk head-first.
    int va = 1, vb = 2;
    CPtrList plist;
    plist.AddHead(&va);
    plist.AddHead(&vb);  // vb now at head
    POSITION ppos = plist.GetHeadPosition();
    CC_CHECK(plist.GetNext(ppos) == &vb);
    CC_CHECK(plist.GetNext(ppos) == &va);
    CC_CHECK(ppos == nullptr);
    plist.RemoveAll();  // fonts.cpp DestroyFonts
    CC_CHECK(plist.GetHeadPosition() == nullptr);

    // Charset constants (fonts.cpp SetFonts Far-East italic test).
    CC_CHECK(ANSI_CHARSET == 0);
    CC_CHECK(GREEK_CHARSET == 161);
    CC_CHECK(TURKISH_CHARSET == 162);
    CC_CHECK(BALTIC_CHARSET == 186);
    CC_CHECK(RUSSIAN_CHARSET == 204);

    // GetSysColor(COLOR_WINDOW) (balloon.cpp iDrawFormattedTextLine): the
    // window background default is white on this port (RGBA end-to-end, no
    // system theme) -- the original compared a run's fg color against it to
    // detect "same as window bg -> transparent".
    CC_CHECK(GetSysColor(COLOR_WINDOW) == RGB(255, 255, 255));

    // SetRect (balloon.cpp bURLHit) fills a RECT.
    RECT sr;
    SetRect(&sr, 1, 2, 3, 4);
    CC_CHECK(sr.left == 1 && sr.top == 2 && sr.right == 3 && sr.bottom == 4);

    // DEFAULT_PITCH (balloon.cpp iDrawFormattedTextLine symbol-font branch).
    CC_CHECK(DEFAULT_PITCH == 0);

    // === Step 3 characterization tests ==================================
    // All measurement flows through ccContext().metricsDC(), which binds to
    // the registered metrics canvas. Register a persistent recording canvas
    // (its fake metrics: 120 twips/byte wide, 240 tall; font_metrics height
    // 240, ascent 190, descent 50, internal_leading 40, external_leading 20).
    // cc_set_metrics_canvas resets the cached metricsDC (Task 6), so this
    // rebinds cleanly even though cc_selftest_dc left one cached earlier.
    static CCRecordingCanvas balloonMetrics;  // static: outlives this call, so
                                              // the cached metricsDC never
                                              // dangles for later selftests.
    cc_set_metrics_canvas(balloonMetrics.handle());
    // Task 6: cc_set_metrics_canvas resets the cached metricsDC so a
    // re-registration rebinds cleanly (cc_selftest_dc left one cached against
    // a now-destroyed canvas). Verify the rebound DC is live.
    CDC* mdcRebound = ccContext().metricsDC();
    CC_CHECK(mdcRebound != nullptr);
    CC_CHECK(mdcRebound->GetDeviceCaps(LOGPIXELSY) == 1440);

    CFont balloonFont;
    {
        LOGFONT lf;
        memset(&lf, 0, sizeof(lf));
        strcpy(lf.lfFaceName, "Comic Sans MS");
        lf.lfHeight = -240;
        lf.lfWeight = 400;
        balloonFont.CreateFontIndirect(&lf);
    }

    // --- (a) CFontInfo ctor field arithmetic (balloon.cpp:606) ----------
    // CFontInfo(pFont, color, nLeading=-40, nBaseAdd=30) under the fake
    // metrics (tm.tmHeight=240, tm.tmExternalLeading=20):
    //   m_leading  = nLeading + tmExternalLeading = -40 + 20         = -20
    //   m_baseAdd  = nBaseAdd - tmExternalLeading =  30 - 20         =  10
    //   topOffset  = nLeading ? 0 : FAREAST_TOPOFFSET; nLeading!=0   =>  0
    //   m_lineHeight = tmHeight + m_leading = 240 + (-20)            = 220
    //   m_continuationWidth = GetTextExtent("...",3).cx = 3*120      = 360
    {
        CFontInfo fi(&balloonFont, RGB(0, 0, 0), -40, 30);
        CC_CHECK(fi.m_leading == -20);
        CC_CHECK(fi.m_baseAdd == 10);
        CC_CHECK(fi.m_topOffset == 0);
        CC_CHECK(fi.m_lineHeight == 220);
        CC_CHECK(fi.m_continuationWidth == 360);
        CC_CHECK(fi.m_font == &balloonFont);
        CC_CHECK(fi.m_crDefaultForeColor == RGB(0, 0, 0));
    }
    // A second CFontInfo with nLeading==0 exercises the topOffset else-branch:
    //   m_leading = 0 + 20 = 20; m_baseAdd = 0 - 20 = -20;
    //   topOffset = FAREAST_TOPOFFSET = 50; m_lineHeight = 240 + 20 = 260.
    {
        CFontInfo fi0(&balloonFont, RGB(0, 0, 0), 0, 0);
        CC_CHECK(fi0.m_leading == 20);
        CC_CHECK(fi0.m_baseAdd == -20);
        CC_CHECK(fi0.m_topOffset == 50);   // FAREAST_TOPOFFSET
        CC_CHECK(fi0.m_lineHeight == 260);
    }

    // --- (b) ::BreakIntoLines characterization (balloon.cpp:347) --------
    // Input "hello world foo bar" (19 bytes), iMaxWidth 1200 (=10 bytes @
    // 120 twips/byte), NULL formatting (so each byte measures 120 wide).
    // Greedy wrap, traced by hand against the algorithm + fake metrics:
    //   line 0 "hello"      : "hello"(600) fits, "hello world"(1320) doesn't
    //                         -> break after "hello"          w=600  len=5
    //   line 1 "world foo"  : "world"(600), "world foo"(1080) fit,
    //                         "world foo bar"(1560) doesn't    w=1080 len=9
    //   line 2 "bar"        : "bar"(360) runs to end-of-string w=360  len=3
    // => 3 lines; widths {600,1080,360}; lengths {5,9,3}; max width 1080.
    {
        char text[] = "hello world foo bar";
        char* rgszStarts[MAXLINES];
        int rgiLengths[MAXLINES], rgiWidths[MAXLINES];
        CDC* pdc = ccContext().metricsDC();
        CFont* pOld = pdc->SelectObject(&balloonFont);
        int nLines = BreakIntoLines(pdc, 1200, text, NULL, rgszStarts, rgiLengths, rgiWidths);
        pdc->SelectObject(pOld);
        CC_CHECK(nLines == 3);
        CC_CHECK(rgiLengths[0] == 5 && rgiWidths[0] == 600);
        CC_CHECK(rgiLengths[1] == 9 && rgiWidths[1] == 1080);
        CC_CHECK(rgiLengths[2] == 3 && rgiWidths[2] == 360);
        // rgszStarts point into `text` at the start of each wrapped line.
        CC_CHECK(strncmp(rgszStarts[0], "hello", 5) == 0);
        CC_CHECK(strncmp(rgszStarts[1], "world foo", 9) == 0);
        CC_CHECK(strncmp(rgszStarts[2], "bar", 3) == 0);
    }

    // --- (d) SetFonts -> four CFontInfo statics (fonts.cpp) -------------
    // Done before (c) because CBWoodringNormal's ctor pulls its CFontInfo
    // from CUnitPanelPage::m_fiWNormal, which SetFonts populates.
    // SetFonts(logFont{lfHeight=-240,face="Comic Sans MS"}, RGB(0,0,0)):
    //   reduction = abs(-240)/180 = 1.333..; szPhysFaceName="Comic Sans MS"
    //   so doVKern=1.
    //   m_fiWNormal  = CFontInfo(fontBalloon, color, (int)(-40*1.333)= -53,
    //                            (int)(30*1.333)= 40)
    //     -> leading = -53+20 = -33; lineHeight = 240 + (-33) = 207
    //   m_fiWWhisper = same params -> lineHeight = 207
    //   UpdateTitleFonts (reduction' = m_unitWidth/4860; m_unitWidth =
    //     MINUNITPANELWIDTH-1 = 2299):
    //       reduction' = 2299/4860 = 0.473...
    //       m_fiTitle = CFontInfo(fontTitle, color, (int)(-220*0.473*1)= -104,
    //                             (int)(120*0.473)= 56)
    //         -> leading = -104+20 = -84; lineHeight = 240 + (-84) = 156
    //       m_fiShout = CFontInfo(fontShout, color, 0, 0)
    //         -> leading = 0+20 = 20; lineHeight = 240 + 20 = 260
    //   (doVKern for title/shout is computed the same way: physical face is
    //    "Comic Sans MS" -> 1; title uses it, shout passes 0,0 so unaffected.)
    {
        LOGFONT lf;
        memset(&lf, 0, sizeof(lf));
        strcpy(lf.lfFaceName, "Comic Sans MS");
        lf.lfHeight = -240;
        lf.lfWeight = 400;
        lf.lfCharSet = 0;  // DEFAULT-ish; == tm.tmCharSet so no substitution
        BOOL ok = CUnitPanelPage::SetFonts(lf, RGB(0, 0, 0));
        CC_CHECK(ok == TRUE);
        CC_CHECK(CUnitPanelPage::m_fiWNormal != NULL);
        CC_CHECK(CUnitPanelPage::m_fiWWhisper != NULL);
        CC_CHECK(CUnitPanelPage::m_fiTitle != NULL);
        CC_CHECK(CUnitPanelPage::m_fiShout != NULL);
        CC_CHECK(CUnitPanelPage::m_fiWNormal->m_lineHeight == 207);
        CC_CHECK(CUnitPanelPage::m_fiWWhisper->m_lineHeight == 207);
        CC_CHECK(CUnitPanelPage::m_fiTitle->m_lineHeight == 156);
        CC_CHECK(CUnitPanelPage::m_fiShout->m_lineHeight == 260);
    }

    // --- (c) CBalloon::SetBBox stable-bbox characterization -------------
    // With m_fiWNormal now set, build a CBWoodringNormal for "hello world
    // foo bar" and SetBBox it into a fixed rect. SetBBox(left,bottom,right,
    // top) computes internals (BreakIntoLines + spline). The resulting
    // m_trueBox (cloud bbox, balloon-local) must be stable and plausible:
    //   width  = Right-Left  >= the longest wrapped line (1080 twips), since
    //            the cloud must enclose the widest text line plus borders;
    //   height = Top-Bottom  ~ nLines(3) * lineHeight(207) + vertical margins.
    // We freeze the exact m_trueBox once observed, having hand-verified the
    // plausibility bounds below; ShiftLines/CreateBalloonSpline use randfloat
    // but MAXLEFTSHIFT/MAXCENTERSHIFT are both 0, so the wrap + shape are
    // deterministic (no RNG effect on geometry).
    {
        CBWoodringNormal balloon("hello world foo bar", NULL, NULL);
        // Give it a speaker anchor is not needed for SetBBox/ComputeInternals
        // (AddArrow/tail is separate). SetBBox width small enough to force the
        // 3-line wrap: interior width right-left-2*XBORDER must be ~1200.
        //   XBORDER=100, so choose right-left = 1200 + 2*100 = 1400.
        BOOL ok = balloon.SetBBox(0, 0, 1400, 0);
        CC_CHECK(ok == TRUE);
        // Wrapped into 3 lines (== the (b) trace): m_fInfo->m_nLines == 3.
        CC_CHECK(balloon.m_fInfo != NULL);
        CC_CHECK(balloon.m_fInfo->m_nLines == 3);
        RECT tb;
        balloon.GetCloudBBox(&tb);
        int width  = tb.right - tb.left;
        int height = tb.top - tb.bottom;
        // Observed cloud bbox (frozen after hand-verifying plausibility):
        //   L=0 T=0 R=1280 B=-751  (width 1280, height 751)
        // Plausibility check by hand:
        //   width  1280 == longest wrapped line (1080) + 2*XBORDER (2*100):
        //     CreateBalloonSpline pushes each boundary filter out by XBORDER
        //     (lFilters[i].x -= XBORDER; rFilters[i].x += XBORDER), so the
        //     cloud encloses the widest line plus one border on each side.
        //   height 751 ~ nLines(3)*lineHeight(207)=621 + vertical borders
        //     (TOPBORDER/-20, YBORDER/40, baseAdd, wave height) ~ 130 twips.
        //   Both >= the required lower bounds (width >= longest line; height
        //   >= nLines*lineHeight), and deterministic (MAXLEFTSHIFT ==
        //   MAXCENTERSHIFT == 0, so ShiftLines/CreateBalloonSpline use no RNG
        //   effect on geometry).
        CC_CHECK(tb.left == 0 && tb.top == 0);
        CC_CHECK(tb.right == 1280 && tb.bottom == -751);
        CC_CHECK(width == 1280);   // == 1080 (longest line) + 2*XBORDER(100)
        CC_CHECK(width  >= 1080);  // >= longest wrapped line
        CC_CHECK(height == 751);
        CC_CHECK(height >= 3 * 207);  // >= nLines * lineHeight
        // Stability: a second identical balloon yields the identical bbox
        // (deterministic wrap + zero shift).
        CBWoodringNormal balloon2("hello world foo bar", NULL, NULL);
        CC_CHECK(balloon2.SetBBox(0, 0, 1400, 0) == TRUE);
        RECT tb2;
        balloon2.GetCloudBBox(&tb2);
        CC_CHECK(tb2.left == tb.left && tb2.right == tb.right);
        CC_CHECK(tb2.top == tb.top && tb2.bottom == tb.bottom);
    }
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
    testMapStringToPtr();
    cc_selftest_loglevel();
    cc_selftest_canvas();
    cc_selftest_canvas_truncated_cubic();
    cc_selftest_dc();
    cc_selftest_geometry();
    cc_selftest_format();
    cc_selftest_balloon();
    return g_failures;
}
