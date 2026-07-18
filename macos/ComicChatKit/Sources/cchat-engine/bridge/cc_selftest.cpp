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
#include "avbfile.h"    // Task 7: CAvatarFileStream (cc_selftest_bodydraw)
#include "avatar.h"
#include "avatario.h"   // Task 7: InitializeAvatars/DestroyAvatars
#include "balloon.h"    // Task 6: CFontInfo/CBalloon/CBWoodring* + ::BreakIntoLines
#include "backdrop.h"
#include "panel.h"      // Task 6: CUnitPanelPage (SetFonts + font statics)
#include "bridge_art.h" // Task 7 review fix: bridge_decode_aura_to_white_alpha
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
    // R9 addition: dib.h uses SRCCOPY as a Draw() default argument. Its value
    // must match Win32 exactly -- the (now live, Task 7) CDIB::Draw bodies pass
    // it straight to the CDC adapter's SRCCOPY-only StretchDIBits.
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

// --- Plan 2 Task 7: CBody draw path (bodycam.cpp CBody* methods now LIVE) -----
// Opens a real fixture avatar (path passed from the Swift test, same fixture
// the ArtTests use), registers it so GetAvatar(m_avatarID) resolves, builds a
// body for a known emotion, then draws it through a recording-canvas CDC and
// asserts:
//   - exactly one "image" log line per expected blit (the R14(i) mask-ROP-pair
//     collapse means ONE draw_image per pose plane -- not the two GDI blits the
//     original emitted);
//   - each blit's dest rect matches GetBodyBox's torsoRect/headRect exactly;
//   - the DrawBody-returned fullRect matches GetBodyBox's fullRect;
//   - no ASSERT traps fire (the ten cc_link_stubs CBody* traps are gone; if a
//     stub survived, ASSERT(0) would abort the whole test binary).
// Returns the failure count so the Swift wrapper can assert == 0.

// Formats a RECT as the recording canvas logs an image dest rect
// ("image dl,dt,dr,db ...") -- dest is (left,top)-(right,bottom), no origin
// shift (the selftest draws at window origin 0). Used to compare a GetBodyBox
// rect against a logged blit line.
static std::string bodydrawDestPrefix(const RECT& r) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "image %ld,%ld,%ld,%ld ",
                  (long)r.left, (long)r.top, (long)r.right, (long)r.bottom);
    return std::string(buf);
}

static int cc_selftest_bodydraw(const char* avatarPath) {
    int startFailures = g_failures;
    InitializeAvatars();

    CAvatarFileStream* pStream = new CAvatarFileStream(avatarPath);
    CAvatarX* av = CAvatarX::LoadAvatar(pStream);
    CC_CHECK(av != NULL);
    if (av == NULL) { delete pStream; DestroyAvatars(); return g_failures - startFailures; }
    av->SetStream(pStream);
    av->IndexAvatar();  // registers into the avatars[] array so GetAvatar works
    CC_CHECK(av->m_avatarID != 0);
    CC_CHECK(GetAvatar(av->m_avatarID) == av);

    // anna.avb is a CAvatarComplex (TORSOFIRST|HEADMASK, flags 5): its body is
    // a CBodyDouble. Build a body for happy/full-intensity and drive the draw.
    CAvatarComplex* avc = (CAvatarComplex*)av;
    CEmotion emotion(1.0, 0.0);
    CBody* body = av->GetBodyFromEmotion(emotion);
    CC_CHECK(body != NULL);
    CC_CHECK(body->GetClass() == BC_BODYDOUBLE);
    CBodyDouble* dbl = (CBodyDouble*)body;

    // Reference geometry: resolve the head/torso poses and compute GetBodyBox
    // independently, so the blit dest rects can be checked against it.
    CPose* headPose = NULL;
    CPose* torsoPose = NULL;
    BOOL gotPoses = avc->GetPosesFromIDs(dbl->m_faceRec->poseID, dbl->m_torsoRec->poseID,
                                         &headPose, &torsoPose);
    CC_CHECK(gotPoses == TRUE);
    CC_CHECK(headPose != NULL && torsoPose != NULL);

    RECT clientRect; clientRect.left = 0; clientRect.top = 0;
    clientRect.right = 2400; clientRect.bottom = -2400;  // MM_TWIPS, y-up
    RECT refFull, refHead, refTorso;
    dbl->GetBodyBox(headPose, torsoPose, clientRect, refFull, refHead, refTorso);

    // --- (a) drawNimbus = FALSE: exactly 2 image blits (torso drawing + head
    //     drawing; TORSOFIRST => torso first). No aura, no other log lines.
    {
        CCRecordingCanvas rec;
        CDC dc(rec.handle());
        RECT full = dbl->DrawBody(&dc, clientRect, FALSE);

        // fullRect matches GetBodyBox.
        CC_CHECK(full.left == refFull.left && full.top == refFull.top &&
                 full.right == refFull.right && full.bottom == refFull.bottom);

        const std::vector<std::string>& log = rec.log();
        // Only image lines, one per plane.
        int imageCount = 0;
        for (const std::string& l : log)
            if (l.rfind("image ", 0) == 0) imageCount++;
        CC_CHECK(log.size() == 2);
        CC_CHECK(imageCount == 2);
        if (log.size() == 2) {
            // Blit 0 = torso drawing at torsoRect; blit 1 = head drawing at headRect.
            CC_CHECK(log[0].rfind(bodydrawDestPrefix(refTorso), 0) == 0);
            CC_CHECK(log[1].rfind(bodydrawDestPrefix(refHead), 0) == 0);
        }
    }

    // --- (b) drawNimbus = TRUE: exactly 4 image blits (torso aura + head aura,
    //     then torso drawing + head drawing). Auras blit at the same
    //     torso/head dest rects (nimbus over the body box).
    {
        CCRecordingCanvas rec;
        CDC dc(rec.handle());
        RECT full = dbl->DrawBody(&dc, clientRect, TRUE);

        CC_CHECK(full.left == refFull.left && full.top == refFull.top &&
                 full.right == refFull.right && full.bottom == refFull.bottom);

        const std::vector<std::string>& log = rec.log();
        int imageCount = 0;
        for (const std::string& l : log)
            if (l.rfind("image ", 0) == 0) imageCount++;
        CC_CHECK(log.size() == 4);
        CC_CHECK(imageCount == 4);
        if (log.size() == 4) {
            // Auras first (torso, head), then drawings (torso, head).
            CC_CHECK(log[0].rfind(bodydrawDestPrefix(refTorso), 0) == 0);
            CC_CHECK(log[1].rfind(bodydrawDestPrefix(refHead), 0) == 0);
            CC_CHECK(log[2].rfind(bodydrawDestPrefix(refTorso), 0) == 0);
            CC_CHECK(log[3].rfind(bodydrawDestPrefix(refHead), 0) == 0);
        }
    }

    // --- (c) Task 7 review fix (R14(v)): decode-level guard. The recording
    //     canvas above only sees blit GEOMETRY (dest rects, blit count/order)
    //     -- it cannot see pixel semantics, so it cannot catch the aura's
    //     color/alpha polarity being wrong. Call the new bridge decode
    //     function directly on a tiny synthetic 1bpp aura DIB (2x1: one black
    //     pixel, one white pixel; palette irrelevant -- the decode reads the
    //     raw bit, not a color table) and check the exact output bytes.
    //     Per the MERGEPAINT-alone derivation (bridge_art.cpp): bit=1 (black/
    //     silhouette) -> opaque white {255,255,255,255}; bit=0 (white/
    //     background) -> transparent {*,*,*,0}.
    {
        size_t infoSize = sizeof(BITMAPINFOHEADER) + 2 * sizeof(RGBQUAD);
        BITMAPINFO* bmi = (BITMAPINFO*)calloc(1, infoSize);
        bmi->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi->bmiHeader.biWidth = 2;
        bmi->bmiHeader.biHeight = 1;
        bmi->bmiHeader.biPlanes = 1;
        bmi->bmiHeader.biBitCount = 1;
        bmi->bmiHeader.biCompression = BI_RGB;
        bmi->bmiHeader.biClrUsed = 2;
        // Aura palette convention: index0 = white (background), index1 =
        // black (silhouette) -- not consulted by the decode, but set anyway
        // for a faithful synthetic DIB.
        RGBQUAD* clr = (RGBQUAD*)((BYTE*)bmi + sizeof(BITMAPINFOHEADER));
        clr[0].rgbRed = clr[0].rgbGreen = clr[0].rgbBlue = 255; // white
        clr[1].rgbRed = clr[1].rgbGreen = clr[1].rgbBlue = 0;   // black

        UINT storageWidth = DIBStorageWidth(2, 1);
        BYTE* bits = (BYTE*)calloc(1, storageWidth);
        // MSB-first packing (readIndexedPixel: bitIdx = 7 - (x % 8)): pixel 0
        // (leftmost) = bit 7 = 1 (black); pixel 1 = bit 6 = 0 (white).
        bits[0] = 0x80;

        int32_t w = 0, h = 0;
        uint8_t* rgba = nullptr;
        bool ok = bridge_decode_aura_to_white_alpha(bmi, bits, &w, &h, &rgba);
        CC_CHECK(ok == TRUE);
        CC_CHECK(w == 2 && h == 1);
        if (ok && rgba != nullptr) {
            // Pixel 0: black/silhouette bit -> opaque white.
            CC_CHECK(rgba[0] == 255 && rgba[1] == 255 && rgba[2] == 255 && rgba[3] == 255);
            // Pixel 1: white/background bit -> fully transparent (RGB
            // unconstrained by the contract; this decode still emits white).
            CC_CHECK(rgba[7] == 0);
            free(rgba);
        }
        free(bits);
        free(bmi);
    }

    // --- (d) R14(i) refinement: maskless pose blits emulate SRCAND-alone
    //     white-transparency. The original's CBody DrawBody blits the drawing
    //     plane SRCAND-ALONE whenever the mask guard ((flags & *MASK) &&
    //     GetMask()) is false (bodycam.cpp: the mask MERGEPAINT is guarded, the
    //     drawing SRCAND is unconditional). SRCAND's algebra (dest = src AND
    //     dest) makes WHITE source pixels transparent (0xFF AND dest = dest)
    //     and BLACK source pixels replace dest. bridge_decode_dib_pair_to_rgba
    //     with maskBmi==NULL (the DrawPoseImage maskless path) must reproduce
    //     that: white -> alpha 0, non-white -> alpha 255, RGB unchanged. (The
    //     golden pose-export path decodeDibToRgba(drawing, mask) is a SEPARATE
    //     entry point and stays fully opaque when maskless -- unchanged.)
    //     Synthetic 2x1 1bpp image DIB: one white pixel, one black pixel.
    {
        size_t infoSize = sizeof(BITMAPINFOHEADER) + 2 * sizeof(RGBQUAD);
        BITMAPINFO* bmi = (BITMAPINFO*)calloc(1, infoSize);
        bmi->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi->bmiHeader.biWidth = 2;
        bmi->bmiHeader.biHeight = 1;
        bmi->bmiHeader.biPlanes = 1;
        bmi->bmiHeader.biBitCount = 1;
        bmi->bmiHeader.biCompression = BI_RGB;
        bmi->bmiHeader.biClrUsed = 2;
        // MonochromePalette convention (avbfile.cpp:15): index0 = white,
        // index1 = black. This one IS consulted -- the pair decode reads the
        // color table for RGB, then the SRCAND rule sets alpha from whiteness.
        RGBQUAD* clr = (RGBQUAD*)((BYTE*)bmi + sizeof(BITMAPINFOHEADER));
        clr[0].rgbRed = clr[0].rgbGreen = clr[0].rgbBlue = 255; // white
        clr[1].rgbRed = clr[1].rgbGreen = clr[1].rgbBlue = 0;   // black

        UINT storageWidth = DIBStorageWidth(2, 1);
        BYTE* bits = (BYTE*)calloc(1, storageWidth);
        // MSB-first: pixel 0 (leftmost) = bit 7; pixel 1 = bit 6. Want pixel 0
        // WHITE (index 0 -> bit 0) and pixel 1 BLACK (index 1 -> bit 1): so the
        // byte is 0b01000000 = 0x40.
        bits[0] = 0x40;

        int32_t w = 0, h = 0;
        uint8_t* rgba = nullptr;
        // maskBmi/maskBits NULL -> the maskless DrawPoseImage path.
        bool ok = bridge_decode_dib_pair_to_rgba(bmi, bits, nullptr, nullptr,
                                                 &w, &h, &rgba);
        CC_CHECK(ok == TRUE);
        CC_CHECK(w == 2 && h == 1);
        if (ok && rgba != nullptr) {
            // Pixel 0: white source -> transparent (SRCAND: white passes dest
            // through). RGB stays white.
            CC_CHECK(rgba[0] == 255 && rgba[1] == 255 && rgba[2] == 255);
            CC_CHECK(rgba[3] == 0);
            // Pixel 1: black source -> opaque (SRCAND: src replaces dest).
            CC_CHECK(rgba[4] == 0 && rgba[5] == 0 && rgba[6] == 0);
            CC_CHECK(rgba[7] == 255);
            free(rgba);
        }
        free(bits);
        free(bmi);
    }

    // --- (e) R14(i) refinement, call-shape lock: DrawPoseImage(image, NULL,...)
    //     still emits exactly one image blit at the requested dest rect (the
    //     maskless SRCAND fix is decode-internal; the CDC call shape and blit
    //     geometry are unchanged). Uses the same synthetic maskless DIB, driven
    //     through a real CDC over the recording canvas.
    {
        size_t infoSize = sizeof(BITMAPINFOHEADER) + 2 * sizeof(RGBQUAD);
        BITMAPINFO* bmi = (BITMAPINFO*)calloc(1, infoSize);
        bmi->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi->bmiHeader.biWidth = 2;
        bmi->bmiHeader.biHeight = 1;
        bmi->bmiHeader.biPlanes = 1;
        bmi->bmiHeader.biBitCount = 1;
        bmi->bmiHeader.biCompression = BI_RGB;
        bmi->bmiHeader.biClrUsed = 2;
        RGBQUAD* clr = (RGBQUAD*)((BYTE*)bmi + sizeof(BITMAPINFOHEADER));
        clr[0].rgbRed = clr[0].rgbGreen = clr[0].rgbBlue = 255;
        clr[1].rgbRed = clr[1].rgbGreen = clr[1].rgbBlue = 0;
        UINT storageWidth = DIBStorageWidth(2, 1);
        BYTE* bits = (BYTE*)calloc(1, storageWidth);
        bits[0] = 0x40;

        CDIB image;
        CC_CHECK(image.Create(bmi, bits));

        CCRecordingCanvas rec;
        CDC dc(rec.handle());
        dc.DrawPoseImage(&image, NULL, 0, 0, 200, -100);

        const std::vector<std::string>& log = rec.log();
        CC_CHECK(log.size() == 1);
        if (log.size() == 1) {
            CC_CHECK(log[0].rfind("image 0,0,200,-100 ", 0) == 0);
        }
        free(bits);
        free(bmi);
    }

    delete body;
    DestroyAvatars();  // deletes av (which owns pStream + poses/DIBs)
    return g_failures - startFailures;
}

// C entry point for the Swift test wrapper (BodyDrawTests.swift), which passes
// the anna.avb fixture path. Runs cc_selftest_bodydraw standalone (resets
// g_failures) so it can be asserted == 0 independently of cc_run_selftests.
extern "C" int32_t cc_run_bodydraw_selftest(const char* avatarPath) {
    g_failures = 0;
    if (avatarPath == NULL) return 1;
    cc_selftest_bodydraw(avatarPath);
    return g_failures;
}

// --- Plan 2 Task 8: camera + orchestrator characterization -------------------
// Needs the anna.avb fixture (loaded TWICE -> two distinct avatars A(id1)/B(id2)
// with identical art but distinct ids + distinct CUserInfo). The camera reads
// the talk-to graph off CUserInfo::m_udi.m_talkTos, so we wire each avatar's
// m_userInfo to its session-table CUserInfo (the WIRING INVARIANT: every
// participant's m_userInfo points at its session entry -- that is what makes
// both the DWORD-space comparisons and the userFromTalkTo reverse lookup exact).

// Load one avatar from the fixture and register it (IndexAvatar -> avatars[]),
// returning it. First call gets id 1, second gets id 2.
static CAvatarX* panelLoadAvatar(const char* avatarPath) {
    CAvatarFileStream* pStream = new CAvatarFileStream(avatarPath);
    CAvatarX* av = CAvatarX::LoadAvatar(pStream);
    if (!av) { delete pStream; return nullptr; }
    av->SetStream(pStream);
    av->IndexAvatar();  // assigns m_avatarID + registers into avatars[]
    return av;
}

// Wire avatar `av` to a fresh session user with the given id; returns the user.
// Establishes the wiring invariant for one participant.
static CUserInfo* panelWireUser(CAvatarX* av, UINT id) {
    CUserInfo* pui = ccContext().session.addUser(id);
    pui->SetAvatarID((USHORT)id);   // CUserInfo::GetAvatarID() -> this id
    av->m_userInfo = pui;           // the invariant: avatar -> its session user
    return pui;
}

// Set A's talk-to graph to exactly {B} (clearing first). B talks to nobody.
static void panelSetTalksTo(CUserInfo* speaker, CUserInfo* addressee) {
    speaker->m_udi.m_talkTos.RemoveAll();
    if (addressee) speaker->m_udi.m_talkTos.Add((DWORD)(uintptr_t)addressee);
}

// Build a single CUnitPanel with speakers `ids` (one SAY balloon each so
// IsSpeaker() is true for every one), run LayoutAvatars, and report each
// placed body's (avatarID, flip, bbox.Left) in left-to-right order via out
// arrays. Returns the body count. The panel is heap-owned by the caller (freed
// by delete). Fonts + metrics canvas must be set up before calling.
static int panelLayoutOne(const UINT* ids, int n,
                          UINT* outID, int* outFlip, int* outLeft) {
    CUnitPanel* panel = new CUnitPanel;
    for (int i = 0; i < n; i++) {
        CBody* spk = panel->FetchSpeaker(ids[i]);
        // one SAY balloon per speaker so LayoutAvatars keeps it (IsSpeaker).
        CBalloon* b = new CBWoodringNormal("hi", NULL, NULL);
        b->m_speaker = spk;
        panel->m_elements.AddTail(b);
    }
    panel->LayoutAvatars();

    // m_bodies now holds the placed bodies in left-to-right order (AddTail in
    // placed order, increasing xOffset). Read them out.
    int count = 0;
    POSITION pos = panel->m_bodies.GetHeadPosition();
    while (pos) {
        CBody* body = (CBody*)panel->m_bodies.GetNext(pos);
        outID[count] = body->m_avatarID;
        outFlip[count] = body->m_flip;
        outLeft[count] = body->m_bbox.Left;
        count++;
    }
    delete panel;
    return count;
}

static int cc_selftest_panel(const char* avatarPath) {
    int startFailures = g_failures;

    // Deterministic RNG: CPanel::CPanel() seeds each panel with rand() (m_seed),
    // and LayoutBalloons does srand(m_seed) so the per-panel balloon shift/place
    // is reproducible FROM that seed. But the seed itself comes from the global
    // rand() stream, which other tests (bodydraw's randfloat calls) advance --
    // so without a fixed srand() here the balloon fit (hence merge/break counts)
    // depends on test ordering. Pin it so every frozen count below is stable
    // regardless of what ran before.
    srand(12345);

    // --- setup: two avatars + two wired users; fonts + metrics canvas --------
    InitializeAvatars();
    ccContext().session.clearUsers();

    CAvatarX* avA = panelLoadAvatar(avatarPath);   // id 1
    CAvatarX* avB = panelLoadAvatar(avatarPath);   // id 2
    CC_CHECK(avA != NULL && avB != NULL);
    if (!avA || !avB) { DestroyAvatars(); return g_failures - startFailures; }
    CC_CHECK(avA->m_avatarID == 1 && avB->m_avatarID == 2);
    CC_CHECK(GetAvatar(1) == avA && GetAvatar(2) == avB);

    CUserInfo* uA = panelWireUser(avA, 1);
    CUserInfo* uB = panelWireUser(avB, 2);
    CC_CHECK((CUserInfo*)avA->m_userInfo == uA);
    CC_CHECK((CUserInfo*)avB->m_userInfo == uB);

    // metrics canvas (LayoutBalloons measures text through ccContext().metricsDC)
    static CCRecordingCanvas panelMetrics;
    cc_set_metrics_canvas(panelMetrics.handle());
    // fonts (MakeBalloon's CFontInfo comes from CUnitPanelPage::m_fiWNormal)
    LOGFONT lf;
    memset(&lf, 0, sizeof(lf));
    strcpy(lf.lfFaceName, "Comic Sans MS");
    lf.lfHeight = -240; lf.lfWeight = 400; lf.lfCharSet = 0;
    CC_CHECK(CUnitPanelPage::SetFonts(lf, RGB(0, 0, 0)) == TRUE);
    // sane panel geometry (defaults are MINUNITPANEL*-1 "resize me" sentinels).
    CUnitPanelPage::SetUnitPanelWidth(MINUNITPANELWIDTH);
    CUnitPanelPage::SetUnitPanelHeight(MINUNITPANELHEIGHT);

    // === (helper) userFromTalkTo through the WIRED users (not the Step-1 ones)
    CC_CHECK(ccContext().session.userFromTalkTo((DWORD)(uintptr_t)uB) == uB);
    CC_CHECK(ccContext().session.userFromTalkTo((DWORD)(uintptr_t)uA) == uA);

    // ================= (a) CAMERA characterization =======================
    // Two avatars, one talks-to the other, both present. After LayoutAvatars
    // the camera must (both hand-verified against EvalPair's scoring below):
    //   1. give the two bodies OPPOSITE facing (m_flip) -- they face each other;
    //   2. orient each toward the other: the LEFT body faces right (flip==FALSE,
    //      == EvalPair's desiredDir for "other is to my right"), the RIGHT body
    //      faces left (flip==TRUE);
    //   3. produce a deterministic order seeded by placement order + hysteresis.
    //
    // HAND-VERIFICATION (why the frozen values are what they are), traced
    // through DoGreedyOrdering -> EvalPlacement -> EvalPair with both avatars'
    // m_lastDir == FALSE (Initialize default) and empty hysteresis:
    //   * A is placed first (input index 0) at slot 0, flip defaults to its
    //     m_lastDir (FALSE) on the 1-body tie.
    //   * B is then scored at slot 0 ([B,A]) vs slot 1 ([A,B]). With A talks-to
    //     B: slot 1 wins (rating p vs p+42) with B.flip=TRUE. Final [A(0),B(1)].
    //   * CRUCIAL, verified: swapping the talk-to DIRECTION (B talks-to A
    //     instead) yields the IDENTICAL layout [A(0),B(1)] -- because the camera
    //     goal "face each other" is symmetric, and the ORDER is driven by
    //     placement order (A first) + hysteresis, NOT by who initiates. The
    //     order only flips when the PLACEMENT order flips (or hysteresis
    //     differs). This corrected the brief's "swap roles -> order flips"
    //     assumption: talk-to direction alone does not reorder a symmetric pair;
    //     placement order does. See p2-task-8-report.md for the full trace.
    avA->m_lastDir = avB->m_lastDir = FALSE;
    avA->m_lastLeft = avA->m_lastRight = 0;
    avB->m_lastLeft = avB->m_lastRight = 0;

    UINT ids_AB[2] = { 1, 2 };   // placement order: A(1) then B(2)
    UINT ids_BA[2] = { 2, 1 };   // placement order: B(2) then A(1)
    UINT oid[2]; int oflip[2]; int oleft[2];

    // --- run 1: A talks-to B, placement order A,B.
    panelSetTalksTo(uA, uB);   // A -> {B}
    panelSetTalksTo(uB, NULL); // B -> {}
    int nAB = panelLayoutOne(ids_AB, 2, oid, oflip, oleft);
    CC_CHECK(nAB == 2);
    CC_CHECK(oleft[0] < oleft[1]);       // outID[0] is the LEFT body
    CC_CHECK(oflip[0] != oflip[1]);      // opposite facing (face each other)
    // FROZEN (hand-verified): left = A(id1) facing right (flip 0), right = B(id2)
    // facing left (flip 1).
    CC_CHECK(oid[0] == 1 && oflip[0] == FALSE);   // A on the left, faces right
    CC_CHECK(oid[1] == 2 && oflip[1] == TRUE);    // B on the right, faces left

    // --- run 2: SWAP THE TALK-TO DIRECTION only (B talks-to A), same placement
    // order A,B. Verified identical layout -- talk-to direction alone does not
    // reorder a symmetric pair (the camera goal is symmetric).
    avA->m_lastDir = avB->m_lastDir = FALSE;
    avA->m_lastLeft = avA->m_lastRight = 0;
    avB->m_lastLeft = avB->m_lastRight = 0;
    panelSetTalksTo(uA, NULL);  // A -> {}
    panelSetTalksTo(uB, uA);    // B -> {A}
    int nBA = panelLayoutOne(ids_AB, 2, oid, oflip, oleft);
    CC_CHECK(nBA == 2);
    CC_CHECK(oflip[0] != oflip[1]);      // still face each other
    CC_CHECK(oid[0] == 1 && oflip[0] == FALSE);   // identical layout to run 1
    CC_CHECK(oid[1] == 2 && oflip[1] == TRUE);

    // --- run 3: SWAP THE PLACEMENT ORDER (B first), A talks-to B. This is what
    // actually flips the order: B is now placed first at slot 0, A scored
    // around it. Verified: layout mirrors to [B(left, faces right), A(right,
    // faces left)] -- the order flipped, they still face each other.
    avA->m_lastDir = avB->m_lastDir = FALSE;
    avA->m_lastLeft = avA->m_lastRight = 0;
    avB->m_lastLeft = avB->m_lastRight = 0;
    panelSetTalksTo(uA, uB);   // A -> {B}
    panelSetTalksTo(uB, NULL); // B -> {}
    int nBfirst = panelLayoutOne(ids_BA, 2, oid, oflip, oleft);
    CC_CHECK(nBfirst == 2);
    CC_CHECK(oflip[0] != oflip[1]);      // face each other
    CC_CHECK(oid[0] == 2 && oflip[0] == FALSE);   // B now on the LEFT, faces right
    CC_CHECK(oid[1] == 1 && oflip[1] == TRUE);    // A now on the RIGHT, faces left

    // === camera DISCRIMINATION (parent requirement): the graph distinguishes
    // users, not just "non-empty". A's talkTos={B} matches B (a talk-to link)
    // and NOT A itself -- verified at the stored-key level EvalPair compares on.
    panelSetTalksTo(uA, uB);
    CC_CHECK(uA->m_udi.m_talkTos.GetUpperBound() + 1 == 1);
    CC_CHECK(uA->m_udi.m_talkTos[0] == (DWORD)(uintptr_t)uB);   // matches B
    CC_CHECK(uA->m_udi.m_talkTos[0] != (DWORD)(uintptr_t)uA);   // NOT A
    panelSetTalksTo(uA, NULL);
    panelSetTalksTo(uB, NULL);

    // ================= (b) PANEL-BREAK rules + (c) ORCHESTRATION ==========
    // These drive the real orchestrator CUnitPanelPage::AddLine. AddLine's
    // break decision (panel.cpp:1079) is:
    //   NEW panel  <=  m_newPanel || last.elements>=5 || m_panels.count<2
    //                                                 || last.AvatarInPanel(uID)
    //   else       ->  clone the last panel, MERGE this speaker into it (replace)
    // plus BM_ACTION forces StartNewPanel() at the top (:1064).
    //
    // Need >2 avatars to exercise the merge + the >=5 cap, so load four more
    // (ids 3..6) and wire them. (The camera sub-tests above only needed A/B.)
    CAvatarX* extra[4];
    for (int i = 0; i < 4; i++) {
        extra[i] = panelLoadAvatar(avatarPath);   // ids 3,4,5,6
        CC_CHECK(extra[i] != NULL);
        if (extra[i]) panelWireUser(extra[i], (UINT)(3 + i));
    }

    // Helper: a fresh empty page (m_newPanel==TRUE, no panels). Heap so the
    // caller owns it (delete frees the cascade). m_doc==nullptr is safe --
    // it's only dereferenced in the R11-wrapped RefreshPanelN (no-op headless).
    auto makePage = []() -> CUnitPanelPage* {
        CUnitPanelPage* pg = new CUnitPanelPage(nullptr);
        pg->m_topY = pg->m_leftX = 0;
        return pg;
    };

    // --- (b1) same speaker again forces a new panel ---------------------
    // Lines: A, B  -> two panels [A],[B] (line 2 also breaks via count<2).
    // Then B again: last panel [B] contains B -> AvatarInPanel(B) -> NEW panel.
    {
        CUnitPanelPage* pg = makePage();
        pg->AddLine(1, "hi", BM_SAY, NULL);   // panel 1: [A]
        CC_CHECK(pg->m_panels.GetCount() == 1);
        pg->AddLine(2, "hi", BM_SAY, NULL);   // panel 2: [B] (count<2 break)
        CC_CHECK(pg->m_panels.GetCount() == 2);
        pg->AddLine(2, "hi", BM_SAY, NULL);   // B already in last panel -> break
        CC_CHECK(pg->m_panels.GetCount() == 3);
        delete pg;
    }

    // --- (b1') distinct speaker MERGES (no break) ----------------------
    // Lines: A, B (two panels), then C (not in last panel [B]) -> merges into
    // the last panel (clone+replace), panel count stays 2, last panel now has
    // 2 elements.
    {
        CUnitPanelPage* pg = makePage();
        pg->AddLine(1, "hi", BM_SAY, NULL);   // [A]
        pg->AddLine(2, "hi", BM_SAY, NULL);   // [A],[B]
        CC_CHECK(pg->m_panels.GetCount() == 2);
        pg->AddLine(3, "hi", BM_SAY, NULL);   // C merges into [B] -> [B,C]
        CC_CHECK(pg->m_panels.GetCount() == 2);
        CUnitPanel* last = (CUnitPanel*)pg->m_panels.GetTail();
        CC_CHECK(last->m_elements.GetCount() == 2);   // merged (2 balloons)
        delete pg;
    }

    // --- (b2) a panel never exceeds the 5-element cap ------------------
    // The orchestrator caps a panel at 5 elements two ways: the explicit
    // `last.elements >= 5` break guard (:1079), AND -- reached FIRST under these
    // fixture unit dimensions -- LayoutBalloons overflow (a panel that can't fit
    // the next balloon returns FALSE, and AddLine's overflow path deletes the
    // clone + StartNewPanel + retries, panel.cpp:1110-1116). Either way the
    // OBSERVABLE invariant the >=5 rule guarantees holds: no panel ever settles
    // with more than 5 elements, and distinct speakers keep merging into the
    // current panel until it fills, then a new panel opens.
    //
    // HAND-VERIFIED against the observed run (six distinct speakers merging):
    //   spk1 -> panels 1, last elems 1        (m_newPanel)
    //   spk2 -> panels 2, last elems 1        (count<2 break)
    //   spk3 -> panels 2, last elems 2        (merge into [B])
    //   spk4 -> panels 2, last elems 3        (merge -> [B,C,D])
    //   spk5 -> panels 2, last elems 4        (merge -> [B,C,D,E])
    //   spk6 -> panels 3, last elems 1        (5th balloon overflows the panel
    //                                          -> overflow break, new panel)
    // So the panel fills to 4 balloons then breaks on the 5th add -- the 5-cap
    // is enforced (overflow fires just before the count guard would). The count
    // guard is genuine code; it is simply not the proximate trigger at these
    // dimensions (documented as a characterization note in the report).
    {
        // Deterministic under srand(12345) (pinned at function entry): six
        // distinct speakers merge into the current panel, filling it to exactly
        // the 5-element cap without overflow:
        //   spk1 -> panels 1, last 1   (m_newPanel)
        //   spk2 -> panels 2, last 1   (count<2 break)
        //   spk3..spk6 -> panels 2, last 2,3,4,5   (each merges into [B..])
        // The panel reaches exactly 5 elements [B,C,D,E,F] and STOPS there.
        CUnitPanelPage* pg = makePage();
        int maxElemsSeen = 0;
        for (int spk = 1; spk <= 6; spk++) {
            pg->AddLine((UINT)spk, "hi", BM_SAY, NULL);
            // after each line, NO panel may exceed the 5-element cap.
            POSITION pp = pg->m_panels.GetHeadPosition();
            while (pp) {
                CUnitPanel* p = (CUnitPanel*)pg->m_panels.GetNext(pp);
                int n = p->m_elements.GetCount();
                CC_CHECK(n <= 5);   // the hard cap the >=5 rule guarantees
                if (n > maxElemsSeen) maxElemsSeen = n;
            }
        }
        // FROZEN: fills to exactly 5 elements in a single (2nd) panel, proving
        // distinct-speaker merging accumulates up to the cap.
        CC_CHECK(maxElemsSeen == 5);
        CC_CHECK(pg->m_panels.GetCount() == 2);
        CUnitPanel* filled = (CUnitPanel*)pg->m_panels.GetTail();
        CC_CHECK(filled->m_elements.GetCount() == 5);

        // A 7th line now hits the explicit `last.elements >= 5` break guard
        // (panel.cpp:1079): even a speaker NOT in the full panel gets a fresh
        // panel. This directly exercises the >=5 rule (not overflow).
        pg->AddLine(1, "hi", BM_SAY, NULL);   // A(id1) not in [B,C,D,E,F]
        CC_CHECK(pg->m_panels.GetCount() == 3);        // broke -> new panel
        CUnitPanel* afterCap = (CUnitPanel*)pg->m_panels.GetTail();
        CC_CHECK(afterCap->m_elements.GetCount() == 1); // the 7th line alone
        delete pg;
    }

    // --- (b3) BM_ACTION always breaks ----------------------------------
    // An action line forces StartNewPanel() before the break logic even runs,
    // so it always lands in its OWN new panel -- even when the same speaker's
    // prior SAY line would otherwise have merged.
    {
        CUnitPanelPage* pg = makePage();
        pg->AddLine(1, "hi", BM_SAY, NULL);       // [A]
        pg->AddLine(2, "hi", BM_SAY, NULL);       // [A],[B]
        int before = pg->m_panels.GetCount();     // 2
        pg->AddLine(3, "hi", BM_ACTION, NULL);    // action -> its own new panel
        CC_CHECK(pg->m_panels.GetCount() == before + 1);
        // and the action panel holds exactly its one box element.
        CUnitPanel* actPanel = (CUnitPanel*)pg->m_panels.GetTail();
        CC_CHECK(actPanel->m_elements.GetCount() == 1);
        delete pg;
    }

    // --- (c) ORCHESTRATION: 2 speakers x 4 alternating lines -----------
    // A,B,A,B. Trace: L1 A -> [A] (count 1). L2 B -> count<2 break -> [A],[B]
    // (count 2). L3 A -> last=[B], A not in it -> merge -> [B,A] (count 2). L4 B
    // -> last=[B,A], B IS in it -> break -> [B,A],[B] (count 3). Frozen
    // panel_count == 3. Every balloon bbox must lie inside its panel's unit rect
    // (0..m_unitWidth, -m_unitHeight..0).
    {
        CUnitPanelPage* pg = makePage();
        pg->AddLine(1, "hi", BM_SAY, NULL);   // [A]
        pg->AddLine(2, "hi", BM_SAY, NULL);   // [A],[B]
        pg->AddLine(1, "hi", BM_SAY, NULL);   // [B,A]
        pg->AddLine(2, "hi", BM_SAY, NULL);   // [B,A],[B]
        int panelCount = pg->m_panels.GetCount();
        CC_CHECK(panelCount == 3);   // FROZEN (hand-traced above)

        // every balloon bbox inside its panel's unit rect.
        int unitW = CUnitPanelPage::GetUnitPanelWidth();
        int unitH = CUnitPanelPage::GetUnitPanelHeight();
        int balloonsChecked = 0;
        POSITION pp = pg->m_panels.GetHeadPosition();
        while (pp) {
            CUnitPanel* panel = (CUnitPanel*)pg->m_panels.GetNext(pp);
            POSITION ep = panel->m_elements.GetHeadPosition();
            while (ep) {
                CPanelElement* e = (CPanelElement*)panel->m_elements.GetNext(ep);
                RECT bb;
                e->GetBBox(&bb);
                // panel-local coords: x in [0, unitW], y in [-unitH, 0].
                CC_CHECK(bb.left >= 0 && bb.right <= unitW);
                CC_CHECK(bb.bottom >= -unitH && bb.top <= 0);
                balloonsChecked++;
            }
        }
        CC_CHECK(balloonsChecked >= 3);   // at least the 3 SAY balloons placed
        delete pg;
    }

    // ================= (d) Establishing() via the real reroute ===========
    // Establishing() (lifted_singles.cpp, pageview.cpp:832 verbatim arithmetic +
    // R17 page-source reroute) gates LayoutAvatars' zoom-in (panel.cpp:788,
    // `bZoomIn && !Establishing()`): TRUE suppresses zoom on the first 1-2
    // "establishing" panels. It reads the composing page's live panel count
    // (via s_composingPage, set by AddLine) + g_bNewedPanel. Drive it BOTH ways
    // through the real function + real setter, exactly as LayoutAvatars does.
    {
        extern BOOL Establishing();
        extern void ccSetComposingPage(CUnitPanelPage* p);
        extern BOOL g_bNewedPanel;

        // No composing page -> count treated as 0 -> TRUE (the conservative
        // "early composition, don't zoom" default; matches original count<=1).
        ccSetComposingPage(nullptr);
        CC_CHECK(Establishing() == TRUE);

        CUnitPanelPage* pg = makePage();
        ccSetComposingPage(pg);

        // 0 panels: count 0 <= 1 -> TRUE.
        CC_CHECK(pg->m_panels.GetCount() == 0);
        CC_CHECK(Establishing() == TRUE);

        // 1 panel: count 1 <= 1 -> TRUE (establishing shot, zoom suppressed).
        pg->m_panels.AddTail(new CUnitPanel);
        CC_CHECK(Establishing() == TRUE);

        // 2 panels: count 2 > 1, so the second clause decides:
        //   (!g_bNewedPanel && count<=2). With g_bNewedPanel FALSE -> TRUE.
        pg->m_panels.AddTail(new CUnitPanel);
        g_bNewedPanel = FALSE;
        CC_CHECK(Establishing() == TRUE);
        //   With g_bNewedPanel TRUE (the last add opened a NEW panel) -> FALSE.
        g_bNewedPanel = TRUE;
        CC_CHECK(Establishing() == FALSE);

        // 3 panels: count 3 > 2 -> FALSE regardless of g_bNewedPanel (past the
        // establishing shots -> zoom-in ENABLED).
        pg->m_panels.AddTail(new CUnitPanel);
        g_bNewedPanel = FALSE;
        CC_CHECK(Establishing() == FALSE);
        g_bNewedPanel = TRUE;
        CC_CHECK(Establishing() == FALSE);

        delete pg;
        ccSetComposingPage(nullptr);   // don't leave a dangling composing page
        g_bNewedPanel = FALSE;
    }

    // ============ (e) 3-party ABSENT-addressee pull-in (Cat-4 path) =======
    // The one talkTos-derived DEREFERENCE (AddTalkTos:357) fires ONLY when a
    // talked-to user is NOT already in the panel: AddTalkTos reconstructs that
    // user's CUserInfo* from the DWORD key (now via userFromTalkTo -- R13+R17)
    // and pulls its body into the panel. Drive it: a panel with only speaker A,
    // A talks-to an ABSENT C (id 3). Expect C pulled in -> 2 bodies laid out.
    // This is the coverage the parent flagged; it's feasible with 3 avatars, so
    // it's covered here rather than deferred.
    {
        CUserInfo* uC = ccContext().session.lookupUser(3);
        CC_CHECK(uC != NULL);   // extra[0] wired id 3 above
        avA->m_lastDir = FALSE; avA->m_lastLeft = avA->m_lastRight = 0;
        if (extra[0]) { extra[0]->m_lastDir = FALSE; extra[0]->m_lastLeft = extra[0]->m_lastRight = 0; }

        panelSetTalksTo(uA, uC);   // A -> {C}, C absent from the panel
        panelSetTalksTo(uC, NULL);

        UINT ids_Aonly[1] = { 1 };   // only A speaks
        UINT oid2[5]; int oflip2[5]; int oleft2[5];
        int n = panelLayoutOne(ids_Aonly, 1, oid2, oflip2, oleft2);
        // C was pulled in via the reconstruction path -> 2 bodies placed.
        CC_CHECK(n == 2);
        // one of the two placed bodies is C (id 3), the pulled-in addressee.
        bool sawA = false, sawC = false;
        for (int i = 0; i < n; i++) {
            if (oid2[i] == 1) sawA = true;
            if (oid2[i] == 3) sawC = true;
        }
        CC_CHECK(sawA && sawC);   // A stayed, C was reconstructed + pulled in
        panelSetTalksTo(uA, NULL);
    }

    DestroyAvatars();
    ccContext().session.clearUsers();
    return g_failures - startFailures;
}

// C entry point for the Swift wrapper (PanelTests.swift), which passes the
// anna.avb fixture path (loaded twice for two avatars). Runs standalone
// (resets g_failures) so it can be asserted == 0 independently.
extern "C" int32_t cc_run_panel_selftest(const char* avatarPath) {
    g_failures = 0;
    if (avatarPath == NULL) return 1;
    cc_selftest_panel(avatarPath);
    return g_failures;
}

// --- Plan 2 Task 8: panel orchestrator + camera -----------------------------
// Step 1 (R17): session extensions -- the user table (holds CUserInfo objects
// carrying the camera's talk-to graph), backdropID, comicsTitle. This first
// block pins the documented defaults + the add/lookup helper contract before
// anything drives the camera through them.

static void cc_selftest_panel_session() {
    CCEngineContext& ctx = ccContext();

    // (Step 1) doc-settings defaults (R17: were GetChatDoc()->GetBackDropID()
    // /GetComicsTitle()). backdropID 0 == "no backdrop"; comicsTitle empty.
    ctx.session.backdropID = 0;
    ctx.session.comicsTitle[0] = '\0';
    CC_CHECK(ctx.session.backdropID == 0);
    CC_CHECK(ctx.session.comicsTitle[0] == '\0');
    ctx.session.backdropID = 7;
    CC_CHECK(ctx.session.backdropID == 7);
    strcpy(ctx.session.comicsTitle, "The Adventures of Anna");
    CC_CHECK(strcmp(ctx.session.comicsTitle, "The Adventures of Anna") == 0);
    ctx.session.backdropID = 0;  // restore defaults for subsequent tests
    ctx.session.comicsTitle[0] = '\0';

    // (Step 1) user table add/lookup helpers.
    ctx.session.clearUsers();
    CC_CHECK(ctx.session.userCount == 0);
    CC_CHECK(ctx.session.lookupUser(1) == nullptr);   // miss on empty table

    CUserInfo* u1 = ctx.session.addUser(1);
    CUserInfo* u2 = ctx.session.addUser(2);
    CC_CHECK(u1 != nullptr && u2 != nullptr);
    CC_CHECK(u1 != u2);
    CC_CHECK(ctx.session.userCount == 2);

    // add is idempotent per id (find-or-create): re-adding id 1 returns the
    // SAME CUserInfo (talkTos entries are raw addresses into the table, so the
    // address must be stable).
    CC_CHECK(ctx.session.addUser(1) == u1);
    CC_CHECK(ctx.session.userCount == 2);  // no new entry

    // lookup resolves by avatarID.
    CC_CHECK(ctx.session.lookupUser(1) == u1);
    CC_CHECK(ctx.session.lookupUser(2) == u2);
    CC_CHECK(ctx.session.lookupUser(3) == nullptr);

    // the returned CUserInfo is mutable and its m_udi.m_talkTos holds the
    // (DWORD)CUserInfo* addressee graph the camera reads (panel.cpp EvalPair).
    u1->m_udi.m_talkTos.RemoveAll();
    u1->m_udi.m_talkTos.Add((DWORD)(uintptr_t)u2);
    CC_CHECK(u1->m_udi.m_talkTos.GetUpperBound() + 1 == 1);
    CC_CHECK(u1->m_udi.m_talkTos[0] == (DWORD)(uintptr_t)u2);

    // (Step 1) userFromTalkTo round-trip (R13+R17): the reverse lookup that
    // AddTalkTos:357 uses to recover the FULL CUserInfo* from a truncated DWORD
    // key. A real key (the low-32 of a session user's &info) returns exactly
    // that pointer; an unknown key returns nullptr. This is what makes the
    // multi-party "pull an absent addressee into the panel" path sound on LP64.
    DWORD keyU2 = (DWORD)(uintptr_t)u2;
    CC_CHECK(ctx.session.userFromTalkTo(keyU2) == u2);
    CC_CHECK(ctx.session.userFromTalkTo((DWORD)(uintptr_t)u1) == u1);
    CC_CHECK(ctx.session.userFromTalkTo(0xDEADBEEF) == nullptr);  // unknown key

    ctx.session.clearUsers();
    CC_CHECK(ctx.session.userCount == 0);
    CC_CHECK(ctx.session.userFromTalkTo(keyU2) == nullptr);  // table cleared
}

// --- Plan 2 Task 9 R9 shim addition: CString::LoadString + the string-
// resource registry (mfc_compat.h/.cpp) it's backed by. Exercised directly
// here (hit/miss/overwrite) per R9's own mandate, separately from
// cc_selftest_textpose()'s higher-level characterization (which exercises it
// only transitively, through InitializeEmotionRules's real ID_RULE_* lookups).
static void testLoadStringResource() {
    // miss: an id nobody has registered.
    CString s;
    CC_CHECK(s.LoadString(0xFFFFF000) == FALSE);
    CC_CHECK(s.IsEmpty());  // untouched by a miss, matching real MFC's contract

    // hit: register then look up by the same id.
    RegisterStringResource(0xFFFFF001, "hello world");
    CC_CHECK(s.LoadString(0xFFFFF001) == TRUE);
    CC_CHECK(s == "hello world");

    // overwrite: re-registering the same id replaces the prior value.
    RegisterStringResource(0xFFFFF001, "goodbye");
    CString s2;
    CC_CHECK(s2.LoadString(0xFFFFF001) == TRUE);
    CC_CHECK(s2 == "goodbye");

    // a second, distinct id is independent of the first.
    RegisterStringResource(0xFFFFF002, "second");
    CString s3;
    CC_CHECK(s3.LoadString(0xFFFFF002) == TRUE);
    CC_CHECK(s3 == "second");
    CC_CHECK(s2 == "goodbye");  // unaffected by registering a different id

    // the real ID_RULE_* registry (seeded by ccSeedEmotionRuleStrings, R17) is
    // populated on ccContext()'s first call, regardless of test order -- force
    // that here rather than relying on some earlier test having touched it.
    ccContext();
    CString shout;
    CC_CHECK(shout.LoadString(ID_RULE_SHOUT) == TRUE);
    CC_CHECK(shout == "AllCaps(\"\");9\nFindString(\"!!!\");9");
}

// --- Plan 2 Task 9: textpose.cpp text -> emotion rule tables -----------------
// Characterization over GetEmotionsFromString (textpose.cpp:271) driven through
// the REAL rule tables InitializeEmotionRules() (textpose.cpp:131) builds from
// the verbatim chat.rc STRINGTABLE content ccSeedEmotionRuleStrings() seeds
// (engine_context.cpp) -- so this exercises the actual production parse chain
// (LoadCompositeRule/LoadSingleRule/RegisterRule), not a hand-built rule set.
//
// This is a PURE rule-table test: it touches neither the avatar registry nor
// ccContext().session.users (it only reads ccContext().session.comicView,
// which ChatPreSendText needs but GetEmotionsFromString does not), so per the
// task brief it runs in the regular (non-serialized) selftest suite rather
// than EngineGlobalStateSelfTests. InitializeEmotionRules/DestroyEmotionRules
// DO mutate process-global static lists (generalRules/wordRules/sentenceRules,
// textpose.cpp:208-212) -- but this function brackets every rule-table use
// with its own Initialize/Destroy pair, leaving no state behind for other
// tests to race against (unlike the avatar registry, which persists across
// calls until DestroyAvatars()).
//
// Every case below was hand-traced against the specific rule-table line that
// fires (cited per case) BEFORE being frozen here, per the brief's mandate.
static void cc_selftest_textpose() {
    void InitializeEmotionRules();
    void DestroyEmotionRules();
    void GetEmotionsFromString(CString &str, CEmotionOpts &emOpts);

    InitializeEmotionRules();

    // (1) ALL-CAPS -> the AllCaps("");9 clause of ID_RULE_SHOUT
    // (chat.rc:2290, seeded verbatim in ccSeedEmotionRuleStrings). No lowercase
    // letters and >1 uppercase letter -> CheckForUppers() TRUE (textpose.cpp:
    // 26-35); capsStrength/capsEmotion were set to (9, EM_SHOUT) by that
    // AllCaps clause (RegisterRule, textpose.cpp:249-252). The string avoids
    // every other keyword (no "!!!" substring, no smileys, no ROTFL/LOL/HEHE,
    // no i'm/i will/i'll/i am/are you/will you/did you/aren't/don't you, and
    // does not start a sentence with You/I/Hi/Bye/Hello/Welcome/Howdy) so this
    // is the ONLY rule that fires -> exactly one CEmotionOpts entry.
    {
        CEmotionOpts em;
        CString s("THANKS FOR THAT");
        GetEmotionsFromString(s, em);
        CC_CHECK(em.m_nOpts == 1);
        if (em.m_nOpts >= 1) {
            CC_CHECK(em.m_emotions[0].m_emotion == EM_SHOUT);
            CC_CHECK(em.m_emotions[0].m_intensity == 1.0f);
            CC_CHECK(em.m_priorities[0] == 9);
        }
    }

    // (2) rule-table keyword -> CheckWord*("ROTFL");11, the first clause of
    // ID_RULE_LAUGH (chat.rc:2291). CheckWord (textpose.cpp:37-49) matches
    // "rotfl" as a whole word (bounded by start-of-string/whitespace before,
    // and whitespace after) in the lowercased buffer (case-insensitive:
    // CheckWord* stores caseSensitive=FALSE, textpose.cpp:259-260). Mixed-case
    // input -> CheckForUppers is FALSE (has lowercase letters) so AllCaps does
    // NOT also fire; no smiley/other keyword substrings present -> exactly one
    // rule fires.
    {
        CEmotionOpts em;
        CString s("That was ROTFL funny");
        GetEmotionsFromString(s, em);
        CC_CHECK(em.m_nOpts == 1);
        if (em.m_nOpts >= 1) {
            CC_CHECK(em.m_emotions[0].m_emotion == EM_LAUGH);
            CC_CHECK(em.m_emotions[0].m_intensity == 1.0f);
            CC_CHECK(em.m_priorities[0] == 11);
        }
    }

    // (3) neutral / no-rule-fires default. Not all-caps (mixed case), contains
    // no substring/word/sentence-start keyword from ANY of the 8 populated
    // rule tables (SHOUT/LAUGH/HAPPY/SAD/POINTOTHER/POINTSELF/WAVE/COY --
    // ANGRY/SCARED/BORED are genuinely empty strings in chat.rc, registering
    // no rules at all, textpose.cpp:249 RegisterRule never reached for them).
    // GetEmotionsFromString's emOpts.m_nOpts is reset to 0 at entry
    // (textpose.cpp:274) and nothing here bumps it -> the default is exactly
    // "zero opinions", i.e. the avatar's GetBodyFromEmotion(emo) call in
    // ChatPreSendText sees an empty CEmotionOpts (falls through to whatever
    // GetBodyFromEmotion's own empty-opts default is -- out of scope here).
    {
        CEmotionOpts em;
        CString s("The weather today");
        GetEmotionsFromString(s, em);
        CC_CHECK(em.m_nOpts == 0);
    }

    // (4) CheckStart* rule (a different mechanism from cases 2/5's CheckWord*):
    // CheckStart*("Hello");5, from ID_RULE_WAVE (chat.rc:2296). StartCompare2
    // (textpose.cpp:267-269) matches the lowercased buffer's PREFIX against
    // "hello" (5 chars) with a non-alnum char following (a space here) ->
    // fires EM_WAVE at priority 5. Not all-caps; contains no other rule's
    // keyword (no "You"/"I"/"Hi"/"Bye"/"Welcome"/"Howdy" sentence start, no
    // smiley, no ROTFL/LOL/HEHE, no are-you/i'm family) -> exactly one rule.
    {
        CEmotionOpts em;
        CString s("Hello there, friend!");
        GetEmotionsFromString(s, em);
        CC_CHECK(em.m_nOpts == 1);
        if (em.m_nOpts >= 1) {
            CC_CHECK(em.m_emotions[0].m_emotion == EM_WAVE);
            CC_CHECK(em.m_emotions[0].m_intensity == 1.0f);
            CC_CHECK(em.m_priorities[0] == 5);
        }
    }

    // (5) punctuation/question sentence, exercising CheckWord* again but on a
    // DIFFERENT rule (POINTOTHER, not LAUGH) to show the mechanism generalizes:
    // CheckWord*("are you");8, from ID_RULE_POINTOTHER (chat.rc:2294). "are
    // you" appears as a whole word/phrase (preceded by string-start, followed
    // by a space) in the lowercased "are you sure?" -> fires EM_POINTOTHER at
    // priority 8. The trailing "?" is a sentence terminator (textpose.cpp:85's
    // sentenceTerminator = ".!?") but GetNextSentenceStart finds nothing after
    // it (end of string), so the sentence-walk loop runs its CheckStart* pass
    // exactly once here too; "are" does not match any CheckStart* keyword
    // (You/I/Hi/Bye/Hello/Welcome/Howdy) so only the CheckWord* rule fires.
    {
        CEmotionOpts em;
        CString s("Are you sure?");
        GetEmotionsFromString(s, em);
        CC_CHECK(em.m_nOpts == 1);
        if (em.m_nOpts >= 1) {
            CC_CHECK(em.m_emotions[0].m_emotion == EM_POINTOTHER);
            CC_CHECK(em.m_emotions[0].m_intensity == 1.0f);
            CC_CHECK(em.m_priorities[0] == 8);
        }
    }

    DestroyEmotionRules();
}

// --- Plan 2 Task 10: cc_strip session API + headless compositor --------------
// Drives the ENTIRE lifted layout engine end-to-end for the first time: opens
// two participants (anna.avb loaded twice -> two avatars with distinct ids +
// distinct CUserInfo), sets a backdrop (field.bgb), ingests a fixed 2x4
// alternating conversation through the panel orchestrator, and composites the
// finished page onto a recording canvas via cc_strip_compose (the R16 headless
// replacement for CUnitPanelPage::Draw).
//
// This runs in the serialized suite (StripTests.swift): like the panel/bodydraw
// selftests it mutates the process-global avatar registry + session + font
// statics + backdrop registries. Determinism is now ENGINE-owned
// (cc_strip_create seeds srand(0x5EED) -- Task 8 review amendment), so the
// frozen panel counts + snapshot are stable regardless of what ran before; the
// srand pin here is redundant belt-and-braces kept for pattern consistency with
// cc_selftest_panel (which predates the engine-side seeding).

// Forward-declare the cc_strip internals the metrics assertions reach: the
// GetBBox comparison needs the live page, so cc_strip exposes it. Declared here
// (cc_compose.cpp defines the struct) rather than in comicchat.h because it is
// a test-only C++ view into the opaque C handle.
struct cc_strip;
extern CUnitPanelPage* cc_strip_page(cc_strip* s);   // cc_compose.cpp (test hook)

static int cc_selftest_strip(const char* avatarPath, const char* backdropPath) {
    int startFailures = g_failures;

    // Redundant belt-and-braces RNG pin (see header comment) -- cc_strip_create
    // seeds it engine-side too.
    srand(12345);

    // --- recording metrics canvas: layout-time text measurement routes here.
    static CCRecordingCanvas stripMetrics;
    cc_set_metrics_canvas(stripMetrics.handle());

    cc_strip* s = cc_strip_create();
    CC_CHECK(s != NULL);
    if (!s) return g_failures - startFailures;

    // --- two participants (anna.avb twice -> ids 1, 2).
    int32_t a = cc_strip_add_participant(s, "Anna", avatarPath);
    int32_t b = cc_strip_add_participant(s, "Boris", avatarPath);
    CC_CHECK(a == 1);
    CC_CHECK(b == 2);
    if (a < 0 || b < 0) { cc_strip_destroy(s); return g_failures - startFailures; }

    // --- backdrop (field.bgb): registered so composed panels blit it.
    CC_CHECK(cc_strip_set_backdrop(s, backdropPath) == 0);

    // --- fixed 2x4 alternating conversation, each line addressing the other.
    int32_t aAddr[1] = { b };
    int32_t bAddr[1] = { a };
    CC_CHECK(cc_strip_add_line(s, a, "Hello there", CC_MODE_SAY, aAddr, 1) == 0);
    CC_CHECK(cc_strip_add_line(s, b, "Hi yourself", CC_MODE_SAY, bAddr, 1) == 0);
    CC_CHECK(cc_strip_add_line(s, a, "How are you", CC_MODE_SAY, aAddr, 1) == 0);
    CC_CHECK(cc_strip_add_line(s, b, "Doing great", CC_MODE_SAY, bAddr, 1) == 0);

    // --- panel_count > 0.
    int32_t panelCount = cc_strip_panel_count(s);
    CC_CHECK(panelCount > 0);

    // --- get_size matches CUnitPanelPage::GetBBox exactly.
    int32_t w = 0, h = 0;
    cc_strip_get_size(s, &w, &h);
    {
        CUnitPanelPage* page = cc_strip_page(s);
        CC_CHECK(page != NULL);
        if (page) {
            RECT bbox;
            page->GetBBox(&bbox);
            CC_CHECK(w == bbox.right - bbox.left);
            CC_CHECK(h == bbox.top - bbox.bottom);   // y-up: top > bottom
        }
    }

    // --- compose onto a recording canvas: non-empty log; each panel emits
    //     >= 1 text (a balloon) and >= 1 image (backdrop and/or body blit).
    CCRecordingCanvas compose;
    CC_CHECK(cc_strip_compose(s, compose.handle()) == 0);
    const std::vector<std::string>& log = compose.log();
    CC_CHECK(!log.empty());

    int textLines = 0, imageLines = 0;
    for (size_t i = 0; i < log.size(); i++) {
        if (log[i].compare(0, 5, "text ") == 0) textLines++;
        if (log[i].compare(0, 6, "image ") == 0) imageLines++;
    }
    // Aggregate sanity (Step 1's coarse gate): at least one text (a balloon) +
    // one image (backdrop/body blit) per panel. The Step 3 snapshot below pins
    // the EXACT per-panel line sequence, which strictly subsumes this.
    CC_CHECK(textLines >= panelCount);
    CC_CHECK(imageLines >= panelCount);

    // ================= STEP 3: FULL-LOG SNAPSHOT =========================
    // The entire recording-canvas compose log, frozen after hand-reviewing every
    // line for plausibility. Because determinism is engine-owned (cc_strip_create
    // seeds srand(0x5EED)), this log is byte-identical across runs AND
    // independent of test order (verified: the same 64 lines whether the strip
    // selftest runs standalone or after the whole serialized battery).
    //
    // STRUCTURE (2x4 alternating A,B,A,B -> FOUR panels in a 2-per-row grid;
    // unitWidth==unitHeight==2300, both interstices 144, so the row/col advance
    // is 2300+144 = 2444):
    //   Panel 1 origin (0,0)        rows[0..13]
    //   Panel 2 origin (2444,0)     rows[14..29]   (x += 2444 = unitW+vInterstice)
    //   Panel 3 origin (0,-2444)    rows[30..45]   (new row: y -= 2444 = unitH+hInterstice)
    //   Panel 4 origin (2444,-2444) rows[46..63]
    // The A,B,A,B script yields four panels (not three like the panel selftest's
    // 2x4 trace): this strip ALSO sets a backdrop and runs ChatPreSendText emotion
    // inference (both absent from the panel selftest), and seeds srand(0x5EED) not
    // 12345 -- all of which shift balloon fit, so line 3 (A) opens its own panel
    // here rather than merging. panel_count == 4, get_size == 4744x4744 (2x2 grid)
    // are frozen from this same run and cross-checked against GetBBox above.
    //
    // PER-PANEL ORDER exactly follows CUnitPanel::Draw (panel.cpp:714-759):
    //   1. clip setup: GetClipBox (no log) then IntersectClipRect(&rect) +
    //      IntersectClipRect(dmgRect) -> TWO identical "clip+" lines at the panel
    //      unit rect in canvas space (panel.cpp:725-727). For panel 1 that is
    //      0,0,2300,-2300; for panel 2, shifted by the (-2444,0) window origin to
    //      2444,0,4744,-2300; etc. (SetWindowOrg sign per Task 3's locked
    //      semantics: org = -loc, so a panel-local rect maps to +loc in output.)
    //   2. backdrop: m_backDrop.Draw (panel.cpp:729) -> one "image" blitting the
    //      315x315 field.bgb to the full panel rect. Panels 3/4 show a non-zero
    //      src top (src=0,74,... / 0,78,...) because their taller emotion poses
    //      drive a larger LayoutAvatars zoom, which shifts m_backDrop's SetBBox
    //      crop (backdrop.cpp:353-356 src-rect math). This is faithful, not noise.
    //   3. bodies (head->tail, panel.cpp:733-742): each of the two bodies
    //      (Anna + Boris) emits its pose-plane blits -- the doubled image lines
    //      are the image+mask compositing the recording canvas logs per body.
    //   4. balloon (elements tail->head, panel.cpp:745-752): the CBWoodring cloud
    //      spline ("path ... fillc=FFFFFF strokec=000000", the white cloud with a
    //      black outline) then the balloon "text". Text color 000000 == the
    //      session comicsColor default (RGB 0,0,0).
    //   5. border (panel.cpp:754-755, DrawBorder): a 5-point closed "path" tracing
    //      the panel unit rectangle in stroke width 120 (2*m_borderWidth).
    //   6. clip restore: SelectClipRgn(NULL) -> "clip-" then
    //      IntersectClipRect(&oldClip) -> "clip+" of the +/-2^28 base sentinel
    //      (panel.cpp:757-758). The trailing clip+ of the last panel is the final
    //      line of the whole log.
    // Every balloon "text" carries the exact line bytes in A,B,A,B order:
    //   "Hello there" (panel 1), "Hi yourself" (panel 2), "How are you" (panel 3),
    //   "Doing great" (panel 4) -- confirming speaker->panel->balloon threading.
    // No line was inexplicable; the snapshot is frozen verbatim from that run.
    static const char* kExpected[] = {
        "clip+ 0,0,2300,-2300",
        "clip+ 0,0,2300,-2300",
        "image 0,0,2300,-2300 src=0,0,315,315",
        "image 457,-1319,951,-2301 src=0,0,185,368",
        "image 406,-1090,911,-1451 src=0,0,189,135",
        "image 457,-1319,951,-2301 src=0,0,185,368",
        "image 406,-1090,911,-1451 src=0,0,189,135",
        "image 1884,-1309,1355,-2301 src=0,0,198,372",
        "image 1892,-1090,1387,-1451 src=0,0,189,135",
        "image 1884,-1309,1355,-2301 src=0,0,198,372",
        "image 1892,-1090,1387,-1451 src=0,0,189,135",
        "path n=53 fill=1 fillc=FFFFFF stroke=1 strokec=000000 w=28 dashed=0 [M 1474,-373 C 1474,-373 1474,-373 1483,-376 C 1492,-379 1531,-392 1575,-386 C 1618,-381 1779,-344 1851,-335 C 1923,-327 2084,-327 2119,-295 C 2155,-265 2155,-124 2119,-84 C 2083,-43 1922,-6 1851,-6 C 1779,-6 1618,-43 1547,-43 C 1475,-44 1314,-7 1243,-7 C 1171,-6 1010,-43 939,-51 C 867,-60 706,-60 671,-92 C 635,-122 635,-263 671,-303 C 707,-344 868,-381 939,-381 C 1011,-381 1172,-344 1216,-338 C 1260,-332 1297,-344 1305,-347 C 1314,-349 1314,-349 1314,-349 C 1145,-574 932,-758 689,-890 C 983,-768 1249,-592 1474,-373 Z]",
        "text 735,-80 color=000000 \"Hello there\"",
        "path n=5 fill=0 fillc=000000 stroke=1 strokec=000000 w=120 dashed=0 [M 0,-2300 L 0,0 L 2300,0 L 2300,-2300 Z]",
        "clip-",
        "clip+ -268435456,268435456,268435456,-268435456",
        "clip+ 2444,0,4744,-2300",
        "clip+ 2444,0,4744,-2300",
        "image 2444,0,4744,-2300 src=0,0,315,315",
        "image 2873,-1285,3409,-2301 src=0,0,206,391",
        "image 2839,-1090,3331,-1442 src=0,0,189,135",
        "image 2873,-1285,3409,-2301 src=0,0,206,391",
        "image 2839,-1090,3331,-1442 src=0,0,189,135",
        "image 4296,-1319,3802,-2301 src=0,0,185,368",
        "image 4347,-1090,3842,-1451 src=0,0,189,135",
        "image 4296,-1319,3802,-2301 src=0,0,185,368",
        "image 4347,-1090,3842,-1451 src=0,0,189,135",
        "path n=53 fill=1 fillc=FFFFFF stroke=1 strokec=000000 w=28 dashed=0 [M 3734,-373 C 3734,-373 3734,-373 3743,-376 C 3752,-379 3791,-392 3835,-386 C 3878,-381 4039,-344 4111,-335 C 4183,-327 4344,-327 4379,-295 C 4415,-265 4415,-124 4379,-84 C 4343,-43 4182,-6 4111,-6 C 4039,-6 3878,-43 3807,-43 C 3735,-44 3574,-7 3503,-7 C 3431,-6 3270,-43 3199,-51 C 3127,-60 2966,-60 2931,-92 C 2895,-122 2895,-263 2931,-303 C 2967,-344 3128,-381 3199,-381 C 3271,-381 3432,-344 3476,-338 C 3520,-332 3557,-344 3565,-347 C 3574,-349 3574,-349 3574,-349 C 3704,-558 3870,-741 4064,-890 C 3917,-743 3805,-567 3734,-373 Z]",
        "text 2995,-80 color=000000 \"Hi yourself\"",
        "path n=5 fill=0 fillc=000000 stroke=1 strokec=000000 w=120 dashed=0 [M 2444,-2300 L 2444,0 L 4744,0 L 4744,-2300 Z]",
        "clip-",
        "clip+ -268435456,268435456,268435456,-268435456",
        "clip+ 0,-2444,2300,-4744",
        "clip+ 0,-2444,2300,-4744",
        "image 0,-2444,2300,-4744 src=0,74,160,233",
        "image 178,-3994,1177,-5924 src=0,0,191,369",
        "image 0,-3534,989,-4256 src=0,0,189,138",
        "image 178,-3994,1177,-5924 src=0,0,191,369",
        "image 0,-3534,989,-4256 src=0,0,189,138",
        "image 2233,-3918,1176,-5924 src=0,0,206,391",
        "image 2300,-3534,1330,-4227 src=0,0,189,135",
        "image 2233,-3918,1176,-5924 src=0,0,206,391",
        "image 2300,-3534,1330,-4227 src=0,0,189,135",
        "path n=53 fill=1 fillc=FFFFFF stroke=1 strokec=000000 w=28 dashed=0 [M 899,-2817 C 899,-2817 899,-2817 908,-2820 C 917,-2823 956,-2836 1000,-2830 C 1043,-2825 1204,-2788 1276,-2779 C 1348,-2771 1509,-2771 1544,-2739 C 1580,-2709 1580,-2568 1544,-2528 C 1508,-2487 1347,-2450 1276,-2450 C 1204,-2450 1043,-2487 972,-2487 C 900,-2488 739,-2451 668,-2451 C 596,-2450 435,-2487 364,-2495 C 292,-2504 131,-2504 96,-2536 C 60,-2566 60,-2707 96,-2747 C 132,-2788 293,-2825 364,-2825 C 436,-2825 597,-2788 641,-2782 C 685,-2776 722,-2788 730,-2791 C 739,-2793 739,-2793 739,-2793 C 709,-2985 641,-3169 539,-3334 C 691,-3184 813,-3009 899,-2817 Z]",
        "text 160,-2524 color=000000 \"How are you\"",
        "path n=5 fill=0 fillc=000000 stroke=1 strokec=000000 w=120 dashed=0 [M 0,-4744 L 0,-2444 L 2300,-2444 L 2300,-4744 Z]",
        "clip-",
        "clip+ -268435456,268435456,268435456,-268435456",
        "clip+ 2444,-2444,4744,-4744",
        "clip+ 2444,-2444,4744,-4744",
        "image 2444,-2444,4744,-4744 src=0,78,151,229",
        "image 2461,-3991,3565,-6063 src=0,0,198,372",
        "image 2444,-3534,3497,-4287 src=0,0,189,135",
        "image 2461,-3991,3565,-6063 src=0,0,198,372",
        "image 2444,-3534,3497,-4287 src=0,0,189,135",
        "image 4674,-3953,3563,-6061 src=0,0,206,391",
        "image 4744,-3533,3725,-4278 src=0,0,189,138",
        "image 4674,-3953,3563,-6061 src=0,0,206,391",
        "image 4744,-3533,3725,-4278 src=0,0,189,138",
        "path n=50 fill=1 fillc=FFFFFF stroke=1 strokec=000000 w=28 dashed=0 [M 3920,-2817 C 3920,-2817 3920,-2817 3946,-2812 C 3972,-2807 4091,-2783 4153,-2777 C 4216,-2771 4377,-2771 4413,-2739 C 4449,-2709 4449,-2568 4413,-2528 C 4377,-2487 4216,-2450 4145,-2450 C 4073,-2450 3912,-2487 3841,-2487 C 3769,-2488 3608,-2451 3537,-2451 C 3465,-2450 3304,-2487 3233,-2495 C 3161,-2504 3000,-2504 2965,-2536 C 2929,-2566 2929,-2707 2965,-2747 C 3001,-2788 3162,-2825 3233,-2825 C 3305,-2825 3466,-2788 3528,-2785 C 3589,-2782 3707,-2806 3733,-2811 C 3759,-2816 3759,-2816 3759,-2816 C 3869,-3014 4015,-3189 4189,-3334 C 4064,-3183 3972,-3007 3920,-2817 Z]",
        "text 3029,-2524 color=000000 \"Doing great\"",
        "path n=5 fill=0 fillc=000000 stroke=1 strokec=000000 w=120 dashed=0 [M 2444,-4744 L 2444,-2444 L 4744,-2444 L 4744,-4744 Z]",
        "clip-",
        "clip+ -268435456,268435456,268435456,-268435456",
    };
    const size_t nExpected = sizeof(kExpected) / sizeof(kExpected[0]);
    CC_CHECK(log.size() == nExpected);
    if (log.size() == nExpected) {
        for (size_t i = 0; i < nExpected; i++) {
            if (log[i] != kExpected[i]) {
                g_failures++;
                ccLog("STRIP SNAPSHOT MISMATCH at line %zu:\n  expected: %s\n  actual:   %s",
                      i, kExpected[i], log[i].c_str());
            }
        }
    }

    cc_strip_destroy(s);
    return g_failures - startFailures;
}

// C entry point for the Swift wrapper (StripTests.swift), which passes the
// anna.avb + field.bgb fixture paths. Runs standalone (resets g_failures).
extern "C" int32_t cc_run_strip_selftest(const char* avatarPath,
                                         const char* backdropPath) {
    g_failures = 0;
    if (avatarPath == NULL || backdropPath == NULL) return 1;
    cc_selftest_strip(avatarPath, backdropPath);
    return g_failures;
}

// --- Plan 3 Task 1: cc_session C boundary skeleton --------------------------
// No parsing yet: creates a cc_session, feeds it a byte string (accumulates
// into an internal buffer only), then drives the test-only echo hook and
// asserts it produced exactly one send() with the expected bytes and one
// on_event() call. Proves the boundary compiles, links, round-trips a byte,
// and invokes a callback -- nothing more.
static int cc_selftest_session_skeleton() {
    // Review fix round 1: NULL-safety. cc_session_feed_bytes/fire_timer/
    // test_echo must tolerate a NULL handle exactly like cc_session_destroy
    // already does (the binding contract in comicchat.h: "no-op if s is
    // NULL"). Reaching the CC_CHECK calls below without crashing IS the
    // assertion -- a NULL deref would abort the process before we get here.
    cc_session_feed_bytes(nullptr, reinterpret_cast<const uint8_t*>("x"), 1);
    cc_session_fire_timer(nullptr, 0);
    cc_session_test_echo(nullptr);
    cc_session_destroy(nullptr);
    CC_CHECK(true);  // survived all four NULL-handle calls above

    struct Cap { std::string sent; int events = 0; } cap;
    cc_session_config cfg = {};
    cfg.user_data = &cap;
    cfg.send = [](void* ud, const uint8_t* d, size_t n) {
        static_cast<Cap*>(ud)->sent.append(reinterpret_cast<const char*>(d), n);
    };
    cfg.on_event = [](void* ud, const cc_proto_event*) { static_cast<Cap*>(ud)->events++; };

    cc_session* s = cc_session_create(&cfg);
    CC_CHECK(s != nullptr);
    cc_session_feed_bytes(s, reinterpret_cast<const uint8_t*>("PING x\r\n"), 8);
    cc_session_test_echo(s);          // test-only: drives one send + one event
    CC_CHECK(cap.sent == "ECHO\r\n");
    CC_CHECK(cap.events == 1);
    cc_session_destroy(s);
    return 0;
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
    cc_selftest_panel_session();  // Task 8 Step 1: R17 session extensions
    testLoadStringResource();     // Task 9 R9 shim: CString::LoadString
    cc_selftest_textpose();       // Task 9: text -> emotion rule tables
    cc_selftest_session_skeleton();  // Plan 3 Task 1: cc_session C boundary
    return g_failures;
}
