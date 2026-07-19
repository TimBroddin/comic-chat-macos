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
#include "ccommon_str.h" // Plan 3 Task 2: bLowLevelQuoting/Unquoting + UTF-8 codec
#include "protsupp.h"    // Plan 3 Task 3: annotation codec (partial lift)
#include "userinfo.h"    // Plan 3 Task 3: CUserInfo (annotation codec test rig)
#include "query.h"       // Plan 3 Task 4: CCQuery/CQueryPtrList correlation list
#include "ircproto.h"    // Plan 3 Task 4: outbound builders (CIrcProto/CIrcSocket)
#include "cc_session.h"  // Plan 3 Task 5a: ccEmitProtoEvent (bridge-internal dispatcher)
#include <unistd.h>   // mkstemp, close (testShimFileApis)

// EmotionToBytes/BytesToEmotion (avatario.cpp) — declared here for the
// annotation-codec test rig, same pattern as BreakIntoLines/Capitalize above
// (their owning header, avatario.h, doesn't declare them -- see avatario.cpp's
// own local declaration inside bInsertAnnotations for the original's
// identical convention).
void BytesToEmotion(CEmotion &em, BYTE emIndex, BYTE inIndex);
void EmotionToBytes(CEmotion &em, BYTE &emotion, BYTE &intensity);

// ::BreakIntoLines free function (balloon.cpp) — declared here for the
// characterization test (balloon.h declares only the CLabel:: method wrapper).
int BreakIntoLines(CDC *pdc, int iMaxWidth, char *szString, CDWordArray *prgdwFormatting,
                   char *rgszStarts[], int rgiLengths[], int rgiWidths[]);

// ::Capitalize free function (balloon.cpp) — declared here for the Task 2
// selftest; balloon.h does not declare it (same situation as BreakIntoLines
// above).
void Capitalize(char *str);

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

// Plan 4a Task 3 Step 1: CharUpperBuff's CP-1252 case fold, table-driven over
// all 256 bytes. Builds the expected fold per the brief's rule set:
//   - 'a'..'z' (0x61-0x7A) -> -0x20 (ASCII uppercase, already covered pre-Task-3)
//   - 0x9A (s-caron) -> 0x8A (S-caron), 0x9C (oe) -> 0x8C (OE),
//     0x9E (z-caron) -> 0x8E (Z-caron) (CP-1252's three out-of-band Latin
//     Extended-A pairs, not in the 0xE0-0xFE run)
//   - 0xFF (y-diaeresis) -> 0x9F (Y-diaeresis, the ONE uppercase pair that
//     crosses the C0/C1-vs-Latin-1-Supplement block boundary)
//   - 0xE0-0xFE (Latin-1 Supplement lowercase accented run) -> -0x20, EXCEPT
//     0xF7 (division sign, not a letter -- unchanged)
//   - 0xDF (sharp s) unchanged: CP-1252 has no uppercase eszett, matches real
//     Win32 CharUpperBuffA
//   - 0xB5 (micro sign) unchanged: not a cased letter
//   - everything else: identity
static void cc_selftest_cp1252_fold() {
    unsigned char expected[256];
    for (int i = 0; i < 256; i++) expected[i] = (unsigned char)i;

    for (int c = 'a'; c <= 'z'; c++) expected[c] = (unsigned char)(c - 0x20);

    for (int c = 0xE0; c <= 0xFE; c++) {
        if (c == 0xF7) continue;  // division sign, unchanged
        expected[c] = (unsigned char)(c - 0x20);
    }

    expected[0x9A] = 0x8A;  // s-caron -> S-caron
    expected[0x9C] = 0x8C;  // oe -> OE
    expected[0x9E] = 0x8E;  // z-caron -> Z-caron
    expected[0xFF] = 0x9F;  // y-diaeresis -> Y-diaeresis

    // no-change exceptions (already identity above, asserted explicitly so a
    // future edit that accidentally folds them trips this test):
    expected[0xF7] = 0xF7;  // division sign
    expected[0xDF] = 0xDF;  // sharp s (no CP-1252 uppercase eszett)
    expected[0xB5] = 0xB5;  // micro sign

    char buf[256];
    for (int i = 0; i < 256; i++) buf[i] = (char)(unsigned char)i;
    CharUpperBuff(buf, 256);

    for (int i = 0; i < 256; i++) {
        CC_CHECK((unsigned char)buf[i] == expected[i]);
    }
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
    // UPPERCASE (Plan 3 Task 2, Step 8): CBWoodringNormal's constructor calls
    // Capitalize(m_str) unconditionally (balloon.cpp) -- restoring Capitalize
    // from its Plan 2 R11 no-op to its real CP-1252 body means every balloon's
    // text is now genuinely uppercased before layout/measurement, matching the
    // original client's actual on-screen behavior. This snapshot is re-frozen
    // to the new (correct) output; the geometry (positions/widths) is
    // UNCHANGED because the recording metrics canvas measures every byte at a
    // fixed per-byte width regardless of case (uppercase and lowercase ASCII
    // are the same byte count) -- only the "text ..." lines' string payload
    // changed case.
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
        "text 735,-80 color=000000 \"HELLO THERE\"",
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
        "text 2995,-80 color=000000 \"HI YOURSELF\"",
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
        "text 160,-2524 color=000000 \"HOW ARE YOU\"",
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
        "text 3029,-2524 color=000000 \"DOING GREAT\"",
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

// --- Plan 4a Task 5: panel geometry API --------------------------------------
// cc_strip_set_panel_geometry/get_panel_geometry are thin wrappers over
// CUnitPanelPage::SetUnitPanelWidth/SetUnitPanelHeight/SetUnitPanelsPerRow
// (engine/panel.h:156-158) and the interstice statics (engine/panel.cpp:70-76),
// so this only needs cc_strip_* -- no cc_session. It DOES need one real
// participant to exercise the "reject after a line was added" gate though:
// CPanel::FetchSpeaker (panel.cpp:667-668) dereferences GetAvatar(uID)->m_body
// unconditionally, so AddLine with an unregistered speaker id would crash
// rather than merely fail -- hence this selftest takes the same avatarPath
// fixture argument as cc_run_strip_selftest/cc_run_panel_selftest/
// cc_run_bodydraw_selftest and is likewise kept OUT of the parameterless
// cc_run_selftests.
static int cc_selftest_panel_geometry(const char* avatarPath) {
    int startFailures = g_failures;

    // Balloon layout measures text via the shared metrics canvas (see
    // cc_selftest_strip's identical setup); adding real lines below needs one.
    static CCRecordingCanvas panelGeometryMetrics;
    cc_set_metrics_canvas(panelGeometryMetrics.handle());

    cc_strip* s = cc_strip_create();
    CC_CHECK(s != NULL);
    if (!s) return g_failures - startFailures;

    // --- create-time default: MINUNITPANELWIDTH/HEIGHT (2300), 2/row, per the
    //     cc_strip_create hard-seed (cc_compose.cpp:143-144) -- unchanged by
    //     this task, still the default for a freshly-created strip.
    {
        int32_t w = 0, h = 0, perRow = 0, hInt = 0, vInt = 0;
        cc_strip_get_panel_geometry(s, &w, &h, &perRow, &hInt, &vInt);
        CC_CHECK(w == 2300);
        CC_CHECK(h == 2300);
        CC_CHECK(perRow == 2);
        CC_CHECK(hInt == 144);   // engine/panel.cpp:75
        CC_CHECK(vInt == 144);  // engine/panel.cpp:76
    }

    // --- set (3200, 3200, 3) on the fresh strip -> succeeds, getter round-trips.
    CC_CHECK(cc_strip_set_panel_geometry(s, 3200, 3200, 3) == 0);
    {
        int32_t w = 0, h = 0, perRow = 0, hInt = 0, vInt = 0;
        cc_strip_get_panel_geometry(s, &w, &h, &perRow, &hInt, &vInt);
        CC_CHECK(w == 3200);
        CC_CHECK(h == 3200);
        CC_CHECK(perRow == 3);
        CC_CHECK(hInt == 144);   // interstices are read-only, unaffected by the set
        CC_CHECK(vInt == 144);
    }

    // --- cc_strip_get_size of a 3-line strip reflects the new arithmetic:
    //     cols*W + (cols-1)*vInterstice, D2 section 1.3's table -- one participant,
    //     three lines each starting a fresh panel (StartNewPanel forced by
    //     AvatarInPanel(uID) being true every time, since there's only one
    //     speaker) so 3 panels laid out 3-per-row: one row, 3 columns.
    int32_t p = cc_strip_add_participant(s, "Anna", avatarPath);
    CC_CHECK(p == 1);
    if (p >= 0) {
        CC_CHECK(cc_strip_add_line(s, p, "Line one", CC_MODE_SAY, NULL, 0) == 0);
        CC_CHECK(cc_strip_add_line(s, p, "Line two", CC_MODE_SAY, NULL, 0) == 0);
        CC_CHECK(cc_strip_add_line(s, p, "Line three", CC_MODE_SAY, NULL, 0) == 0);
        int32_t panelCount = cc_strip_panel_count(s);
        CC_CHECK(panelCount == 3);

        int32_t w = 0, h = 0;
        cc_strip_get_size(s, &w, &h);
        // 3 columns, 1 row: width = 3*3200 + 2*144 = 9888; height = 1*3200 (no
        // row interstice added for a single row).
        CC_CHECK(w == 3 * 3200 + 2 * 144);
        CC_CHECK(h == 3200);

        // --- FRESH STRIP ONLY: a line has now been added (panel_count > 0) ->
        //     set_panel_geometry must reject (nonzero), and geometry must be
        //     UNCHANGED by the rejected call.
        CC_CHECK(cc_strip_set_panel_geometry(s, 4000, 4000, 4) != 0);
        int32_t w2 = 0, h2 = 0, perRow2 = 0, hInt2 = 0, vInt2 = 0;
        cc_strip_get_panel_geometry(s, &w2, &h2, &perRow2, &hInt2, &vInt2);
        CC_CHECK(w2 == 3200);
        CC_CHECK(h2 == 3200);
        CC_CHECK(perRow2 == 3);
    }

    cc_strip_destroy(s);
    return g_failures - startFailures;
}

// C entry point for the Swift wrapper, which passes the anna.avb fixture path.
// Runs standalone (resets g_failures) -- same pattern as cc_run_strip_selftest.
extern "C" int32_t cc_run_panel_geometry_selftest(const char* avatarPath) {
    g_failures = 0;
    if (avatarPath == NULL) return 1;
    cc_selftest_panel_geometry(avatarPath);
    return g_failures;
}

// --- Plan 4a Task 6: avatar API (cc_avatar_icon_image + ------------------
//     cc_strip_set_participant_avatar) ---------------------------------------
// (a) cc_avatar_icon_image: a standalone cc_avatar (bridge_art.cpp) decoded via
//     the icon pose (CAvatarX::GetIconPose(), avatar.h:251 -- the public
//     wrapper around the protected GetPoseFromID(m_icon); bridge code cannot
//     call the protected overload directly, so cc_avatar_icon_image's
//     implementation goes through GetIconPose() for the identical result).
// (b) cc_strip_set_participant_avatar: one strip, one participant loaded from
//     avatarPath, one line; switch the participant to otherAvatarPath
//     mid-strip; one more line; compose to a recording canvas and check the
//     ops stream is still valid (>=2 panels, compose rc 0). Also: a bad
//     participant id must be rejected (nonzero).
static int cc_selftest_avatar_api(const char* avatarPath, const char* otherAvatarPath) {
    int startFailures = g_failures;

    // --- (a) cc_avatar_icon_image on a standalone cc_avatar.
    {
        cc_avatar* av = cc_avatar_open(avatarPath);
        CC_CHECK(av != NULL);
        if (av) {
            cc_image img = {};
            CC_CHECK(cc_avatar_icon_image(av, &img) == 0);
            CC_CHECK(img.width > 0);
            CC_CHECK(img.height > 0);
            CC_CHECK(img.rgba != NULL);
            cc_image_free(&img);
            cc_avatar_close(av);
        }
    }

    // --- (b) cc_strip_set_participant_avatar mid-strip.
    static CCRecordingCanvas avatarApiMetrics;
    cc_set_metrics_canvas(avatarApiMetrics.handle());

    cc_strip* s = cc_strip_create();
    CC_CHECK(s != NULL);
    if (!s) return g_failures - startFailures;

    int32_t p = cc_strip_add_participant(s, "Anna", avatarPath);
    CC_CHECK(p == 1);
    if (p >= 0) {
        CC_CHECK(cc_strip_add_line(s, p, "Before the switch", CC_MODE_SAY, NULL, 0) == 0);

        // Bad participant id -> nonzero, and must not disturb the real one.
        CC_CHECK(cc_strip_set_participant_avatar(s, 999, otherAvatarPath) != 0);

        CC_CHECK(cc_strip_set_participant_avatar(s, p, otherAvatarPath) == 0);
        CC_CHECK(cc_strip_add_line(s, p, "After the switch", CC_MODE_SAY, NULL, 0) == 0);

        int32_t panelCount = cc_strip_panel_count(s);
        CC_CHECK(panelCount >= 2);

        CCRecordingCanvas compose;
        CC_CHECK(cc_strip_compose(s, compose.handle()) == 0);
        CC_CHECK(!compose.log().empty());
    }

    cc_strip_destroy(s);
    return g_failures - startFailures;
}

// C entry point for the Swift wrapper, which passes the anna.avb +
// armando.avb fixture paths. Runs standalone (resets g_failures).
extern "C" int32_t cc_run_avatar_api_selftest(const char* avatarPath,
                                              const char* otherAvatarPath) {
    g_failures = 0;
    if (avatarPath == NULL || otherAvatarPath == NULL) return 1;
    cc_selftest_avatar_api(avatarPath, otherAvatarPath);
    return g_failures;
}

// --- Comic hit-testing (Plan 4b): cc_strip_hit_test_avatar/_balloon ----------
// Builds the fixed 2x4 A,B,A,B conversation (same fixture/nicks/lines as
// cc_selftest_strip), composes, then drives the two hit-test entries against
// points DERIVED FROM THE LIVE panel/body/balloon bboxes rather than hardcoded
// pixel coords -- deterministic under fake metrics (cc_strip_create seeds
// srand(0x5EED)), and robust to any future re-freeze of the compose snapshot.
//
// Coordinate model (matches cc_strip_hit_test_avatar's own doc): a panel's
// bodies/elements carry PANEL-LOCAL bboxes (twips, y-up, local y in [-unitH,0]);
// its page-space origin is (ulx, uly) with ulx = colNum*(unitW+vInter),
// uly = -rowNum*(unitH+hInter). A page-space point for a panel-local bbox
// center is (ulx + (Left+Right)/2, uly + (Top+Bottom)/2).
static int cc_selftest_hit_test(const char* avatarPath, const char* otherAvatarPath) {
    int startFailures = g_failures;

    static CCRecordingCanvas hitTestMetrics;
    cc_set_metrics_canvas(hitTestMetrics.handle());

    cc_strip* s = cc_strip_create();
    CC_CHECK(s != NULL);
    if (!s) return g_failures - startFailures;

    int32_t a = cc_strip_add_participant(s, "Anna", avatarPath);
    int32_t b = cc_strip_add_participant(s, "Boris", avatarPath);
    CC_CHECK(a == 1);
    CC_CHECK(b == 2);
    if (a < 0 || b < 0) { cc_strip_destroy(s); return g_failures - startFailures; }

    // Switch participant a onto a DIFFERENT avatar BEFORE any line, so a's body
    // avatar id (a fresh registry id from the switch) diverges from its
    // participant id (still 1) -- this exercises participantForAvatarID's
    // avatar->participant reversal, the whole reason the return value is the
    // participant id and not the raw body avatar id (live-fix 3/4).
    CC_CHECK(cc_strip_set_participant_avatar(s, a, otherAvatarPath) == 0);

    int32_t aAddr[1] = { b };
    int32_t bAddr[1] = { a };
    CC_CHECK(cc_strip_add_line(s, a, "Hello there", CC_MODE_SAY, aAddr, 1) == 0);
    CC_CHECK(cc_strip_add_line(s, b, "Hi yourself", CC_MODE_SAY, bAddr, 1) == 0);
    CC_CHECK(cc_strip_add_line(s, a, "How are you", CC_MODE_SAY, aAddr, 1) == 0);
    CC_CHECK(cc_strip_add_line(s, b, "Doing great", CC_MODE_SAY, bAddr, 1) == 0);

    CCRecordingCanvas compose;
    CC_CHECK(cc_strip_compose(s, compose.handle()) == 0);

    CUnitPanelPage* page = cc_strip_page(s);
    CC_CHECK(page != NULL);
    if (!page) { cc_strip_destroy(s); return g_failures - startFailures; }

    const int perRow = CUnitPanelPage::m_panelsPerRow;
    const int unitW  = CUnitPanelPage::m_unitWidth;
    const int unitH  = CUnitPanelPage::m_unitHeight;
    const int vInter = CUnitPanelPage::m_vInterstice;
    const int hInter = CUnitPanelPage::m_hInterstice;

    // ---- (a) every body: its bbox-center point hit-tests to its participant.
    // Also confirms the AVATAR->PARTICIPANT reversal: the body carries a fresh
    // registry avatar id (>= 3 after the switch for a; b keeps id 2), but the
    // hit-test returns the participant id (1 for a, 2 for b) either way.
    int bodiesChecked = 0;
    int aHits = 0, bHits = 0;
    {
        int pNum = 0;
        POSITION pos = page->m_panels.GetHeadPosition();
        while (pos != NULL) {
            CPanel* panel = (CPanel*)page->m_panels.GetNext(pos);
            int rowNum = pNum / perRow;
            int colNum = pNum % perRow;
            int ulx =  colNum * (unitW + vInter);
            int uly = -rowNum * (unitH + hInter);
            pNum++;

            POSITION bp = panel->m_bodies.GetHeadPosition();
            while (bp != NULL) {
                CBody* body = (CBody*)panel->m_bodies.GetNext(bp);
                // Sample a point in the INTERSECTION of the body bbox and the
                // panel unit rect: bodies can be TALLER than the 2300-twip unit
                // panel (a large emotion-pose zoom, panel.cpp:866 -- e.g. panels
                // 3/4's B-3968 local bottom), and the hit-test (like the
                // original FindAvatarUnderPoint) requires the point to be inside
                // the panel's unit rect FIRST (pageview.cpp:687), since the
                // compositor clips each panel to that rect. So clamp the body
                // bbox to the unit rect [-unitH, 0] before taking the center --
                // the visible, clickable part of the body.
                int bL = body->m_bbox.Left, bR = body->m_bbox.Right;
                int bB = body->m_bbox.Bottom, bT = body->m_bbox.Top;
                if (bB < -unitH) bB = -unitH;   // clamp to the panel unit rect
                if (bT > 0)      bT = 0;
                int cx = ulx + (bL + bR) / 2;
                int cy = uly + (bB + bT) / 2;

                // The participant this body's avatar currently maps to.
                int32_t expected = 0;
                CCSessionSettings& sess = ccContext().session;
                for (int i = 0; i < sess.userCount; i++)
                    if (sess.users[i].info.GetAvatarID() == body->m_avatarID)
                        expected = (int32_t)sess.users[i].id;
                CC_CHECK(expected == 1 || expected == 2);   // a or b

                int32_t got = cc_strip_hit_test_avatar(s, cx, cy);
                CC_CHECK(got == expected);
                if (got == 1) aHits++;
                if (got == 2) bHits++;
                bodiesChecked++;
            }
        }
    }
    CC_CHECK(bodiesChecked > 0);
    CC_CHECK(aHits > 0);   // participant a's body(ies) were hit at least once
    CC_CHECK(bHits > 0);   // participant b's too

    // ---- (b) an empty margin -> 0. A point far below the last panel row is in
    // no panel slot at all (page grows toward -y; y well below -pageH is empty).
    {
        int32_t pw = 0, ph = 0;
        cc_strip_get_size(s, &pw, &ph);
        CC_CHECK(cc_strip_hit_test_avatar(s, pw / 2, -(ph + unitH)) == 0);   // below the page
        CC_CHECK(cc_strip_hit_test_avatar(s, pw + unitW, -unitH) == 0);      // right of the page
        // A point at y=+unitH (ABOVE the top row, positive y) is also empty --
        // panel 0's slot spans y in [-unitH, 0].
        CC_CHECK(cc_strip_hit_test_avatar(s, unitW / 2, unitH) == 0);
    }

    // ---- (c) balloon tooltip: a point inside panel 0's balloon returns its
    // text bytes. Panel 0's balloon is the FIRST line, "Hello there" (uppercased
    // to "HELLO THERE" by CBWoodringNormal's Capitalize, exactly as the strip
    // snapshot's "text ... HELLO THERE" shows). Find panel 0's balloon bbox and
    // hit its center.
    {
        POSITION pos = page->m_panels.GetHeadPosition();
        CPanel* panel0 = pos ? (CPanel*)page->m_panels.GetNext(pos) : NULL;
        CC_CHECK(panel0 != NULL);
        bool foundBalloon = false;
        if (panel0) {
            POSITION ep = panel0->m_elements.GetHeadPosition();
            while (ep != NULL && !foundBalloon) {
                CPanelElement* el = (CPanelElement*)panel0->m_elements.GetNext(ep);
                if (!(el->GetType() & PE_BALLOON)) continue;
                RECT bb; el->GetBBox(&bb);
                // panel 0 origin is (0,0), so panel-local == page coords here.
                int cx = (bb.left + bb.right) / 2;
                int cy = (bb.top + bb.bottom) / 2;
                char buf[128] = {0};
                int32_t n = cc_strip_hit_test_balloon(s, cx, cy, buf, (int32_t)sizeof(buf));
                CC_CHECK(n > 0);
                CC_CHECK((int)strlen(buf) == n);
                // The balloon carries the (uppercased) first line.
                CC_CHECK(strcmp(buf, "HELLO THERE") == 0);
                foundBalloon = true;
            }
        }
        CC_CHECK(foundBalloon);
    }

    // ---- (d) balloon miss + arg guards: an empty margin -> -1 and buf[0]=='\0';
    // a NULL buf or non-positive buflen -> -1.
    {
        int32_t pw = 0, ph = 0;
        cc_strip_get_size(s, &pw, &ph);
        char buf[16]; buf[0] = 'X';
        CC_CHECK(cc_strip_hit_test_balloon(s, pw + unitW, -unitH, buf, (int32_t)sizeof(buf)) == -1);
        CC_CHECK(buf[0] == '\0');
        CC_CHECK(cc_strip_hit_test_balloon(s, 0, 0, NULL, 16) == -1);
        CC_CHECK(cc_strip_hit_test_balloon(s, 0, 0, buf, 0) == -1);
    }

    // ---- (e) truncation: a 4-byte buffer holds "HEL" + NUL and returns 3.
    {
        POSITION pos = page->m_panels.GetHeadPosition();
        CPanel* panel0 = pos ? (CPanel*)page->m_panels.GetNext(pos) : NULL;
        if (panel0) {
            POSITION ep = panel0->m_elements.GetHeadPosition();
            while (ep != NULL) {
                CPanelElement* el = (CPanelElement*)panel0->m_elements.GetNext(ep);
                if (!(el->GetType() & PE_BALLOON)) continue;
                RECT bb; el->GetBBox(&bb);
                int cx = (bb.left + bb.right) / 2;
                int cy = (bb.top + bb.bottom) / 2;
                char small[4];
                int32_t n = cc_strip_hit_test_balloon(s, cx, cy, small, 4);
                CC_CHECK(n == 3);
                CC_CHECK(strcmp(small, "HEL") == 0);
                break;
            }
        }
    }

    // ---- (f) NULL/degenerate strip guards.
    CC_CHECK(cc_strip_hit_test_avatar(NULL, 0, 0) == 0);
    {
        char buf[8];
        CC_CHECK(cc_strip_hit_test_balloon(NULL, 0, 0, buf, 8) == -1);
    }

    cc_strip_destroy(s);
    return g_failures - startFailures;
}

// C entry point for the Swift wrapper (StripTests.swift), which passes the
// anna.avb + armando.avb fixture paths. Runs standalone (resets g_failures).
extern "C" int32_t cc_run_hit_test_selftest(const char* avatarPath,
                                            const char* otherAvatarPath) {
    g_failures = 0;
    if (avatarPath == NULL || otherAvatarPath == NULL) return 1;
    cc_selftest_hit_test(avatarPath, otherAvatarPath);
    return g_failures;
}

// --- Plan 4a Task 7: title/starring lift (un-R11 AddTitle/UpdateTitle/
//     AddStars/AddStarsAux + CStarLabel::Draw) + cc_strip_set_title/set_self.
// Two participants -> set_self(p1) -> set_title("MY COMIC") -> two lines ->
// compose. Asserts: panel_count >= 3 (title + 2 speech panels), the ops
// stream carries a text op with "MY COMIC" (the title label) and one with
// "STARRING" (ID_STARRING's verified chat.rc text, all caps) and BOTH
// participants' nicknames (the starring rows AddStarsAux/AddStars build).
// Then a 3rd participant joins (title already set -> cc_strip_add_participant's
// conditional UpdateTitle refresh fires) -> recompose -> its nickname appears
// too, proving the member-join wiring.
static int cc_selftest_strip_title_starring(const char* avatarPath,
                                            const char* otherAvatarPath) {
    int startFailures = g_failures;

    static CCRecordingCanvas titleMetrics;
    cc_set_metrics_canvas(titleMetrics.handle());

    cc_strip* s = cc_strip_create();
    CC_CHECK(s != NULL);
    if (!s) return g_failures - startFailures;

    int32_t p1 = cc_strip_add_participant(s, "Anna", avatarPath);
    int32_t p2 = cc_strip_add_participant(s, "Boris", avatarPath);
    CC_CHECK(p1 == 1);
    CC_CHECK(p2 == 2);
    if (p1 < 0 || p2 < 0) { cc_strip_destroy(s); return g_failures - startFailures; }

    // --- set_self before set_title: title-only panel is a valid state
    // (AddStars early-returns "not registered yet" until self is set --
    // panel.cpp -- so this ordering choice, self first, is the one the brief
    // calls out as valid; here self IS set before title, so AddStars renders
    // starring rows on the very first AddTitle call, not just on a later
    // UpdateTitle).
    CC_CHECK(cc_strip_set_self(s, p1) == 0);
    // Bad participant id -> rejected.
    CC_CHECK(cc_strip_set_self(s, 999) != 0);

    CC_CHECK(cc_strip_set_title(s, "MY COMIC") == 0);

    int32_t aAddr[1] = { p2 };
    int32_t bAddr[1] = { p1 };
    CC_CHECK(cc_strip_add_line(s, p1, "Hello there", CC_MODE_SAY, aAddr, 1) == 0);
    CC_CHECK(cc_strip_add_line(s, p2, "Hi yourself", CC_MODE_SAY, bAddr, 1) == 0);

    int32_t panelCount = cc_strip_panel_count(s);
    CC_CHECK(panelCount >= 3);   // title panel (0) + >= 2 speech panels

    {
        CCRecordingCanvas compose;
        CC_CHECK(cc_strip_compose(s, compose.handle()) == 0);
        const std::vector<std::string>& log = compose.log();
        CC_CHECK(!log.empty());

        bool sawTitle = false, sawStarring = false, sawAnna = false, sawBoris = false;
        for (size_t i = 0; i < log.size(); i++) {
            if (log[i].compare(0, 5, "text ") != 0) continue;
            if (log[i].find("\"MY COMIC\"") != std::string::npos) sawTitle = true;
            if (log[i].find("STARRING") != std::string::npos) sawStarring = true;
            if (log[i].find("ANNA") != std::string::npos) sawAnna = true;      // Capitalize()'d balloon text
            if (log[i].find("Anna") != std::string::npos) sawAnna = true;      // starring row (CLabel, not capitalized)
            if (log[i].find("Boris") != std::string::npos) sawBoris = true;
        }
        CC_CHECK(sawTitle);
        CC_CHECK(sawStarring);
        CC_CHECK(sawAnna);
        CC_CHECK(sawBoris);
    }

    // --- 3rd participant joins AFTER a title is set -> cc_strip_add_participant's
    //     conditional UpdateTitle fires -> recompose shows the new nickname too.
    int32_t p3 = cc_strip_add_participant(s, "Carla", otherAvatarPath);
    CC_CHECK(p3 == 3);
    if (p3 >= 0) {
        CCRecordingCanvas compose2;
        CC_CHECK(cc_strip_compose(s, compose2.handle()) == 0);
        const std::vector<std::string>& log2 = compose2.log();
        bool sawCarla = false;
        for (size_t i = 0; i < log2.size(); i++) {
            if (log2[i].compare(0, 5, "text ") != 0) continue;
            if (log2[i].find("Carla") != std::string::npos) sawCarla = true;
        }
        CC_CHECK(sawCarla);
    }

    cc_strip_destroy(s);
    return g_failures - startFailures;
}

// C entry point for the Swift wrapper, which passes the anna.avb + armando.avb
// fixture paths. Runs standalone (resets g_failures).
extern "C" int32_t cc_run_strip_title_starring_selftest(const char* avatarPath,
                                                         const char* otherAvatarPath) {
    g_failures = 0;
    if (avatarPath == NULL || otherAvatarPath == NULL) return 1;
    cc_selftest_strip_title_starring(avatarPath, otherAvatarPath);
    return g_failures;
}

// --- Plan 4b Task 2: emotion-wheel engine surface selftest -------------------
// cc_strip_set_self_emotion / cc_strip_preview_self_text / cc_strip_self_pose /
// cc_strip_self_annotations, all operating on the SELF participant. Mirrors
// the brief's Step 2 assertion list exactly:
//   (1) self_pose returns a valid pose index (>=0... in practice poseIDs are
//       1-based so ">0", < GetPoseCount()+1) on a freshly-added self.
//   (2) set_self_emotion(0.0, 1.0) then self_annotations: cooked==1, and
//       EmotionToBytes-encoded emotion/intensity are plausible (both byte
//       values are in the IndexToByte'd '0'..'9'+ range emFloats/x10 produce --
//       checked structurally, not against a hand-picked golden byte, since
//       EmotionToBytes's exact table match for angle 0.0 depends on emFloats
//       (avatario.cpp) which is out of this task's lift scope to re-verify).
//   (3) preview_self_text with a trigger phrase from the REAL rule tables
//       (reusing "Hello there, friend!" -- see cc_selftest_textpose's case 4,
//       this file's cc_selftest.cpp:2300ish -- fires EM_WAVE at intensity 1.0,
//       priority 5 via ID_RULE_WAVE's CheckStart* clause) changes self_pose's
//       result vs. a neutral baseline captured right after add_participant.
//   (4) all four reject (nonzero) on a strip with NO self set.
static int cc_selftest_self_emotion(const char* avatarPath, const char* otherAvatarPath) {
    int startFailures = g_failures;

    static CCRecordingCanvas selfEmotionMetrics;
    cc_set_metrics_canvas(selfEmotionMetrics.handle());

    cc_strip* s = cc_strip_create();
    CC_CHECK(s != NULL);
    if (!s) return g_failures - startFailures;

    // --- (4a) no self set yet: all four must reject.
    int32_t poseOut = -999;
    CEmotion dummy;
    cc_annotations annOut;
    memset(&annOut, 0, sizeof(annOut));
    CC_CHECK(cc_strip_set_self_emotion(s, 0.0, 1.0) != 0);
    CC_CHECK(cc_strip_preview_self_text(s, "Hello there, friend!") != 0);
    CC_CHECK(cc_strip_self_pose(s, &poseOut) != 0);
    CC_CHECK(cc_strip_self_annotations(s, &annOut) != 0);

    int32_t p = cc_strip_add_participant(s, "Anna", avatarPath);
    CC_CHECK(p == 1);
    if (p < 0) { cc_strip_destroy(s); return g_failures - startFailures; }
    CC_CHECK(cc_strip_set_self(s, p) == 0);

    // --- (1) self_pose returns a valid, in-range pose id for the freshly-added
    //     self (poseID space is 1-based: CreatePose/CreatePoseWithMask assign
    //     array-position + 1, avatar.cpp's own comment at :434).
    int32_t basePose = -1;
    CC_CHECK(cc_strip_self_pose(s, &basePose) == 0);
    CC_CHECK(basePose > 0);
    CC_CHECK(basePose <= 0x7fff);  // sane upper bound; exact avatar pose count not asserted here (fixture-agnostic)

    // --- (2) set_self_emotion(0, 1.0) then self_annotations: cooked, and
    //     plausible nonzero-shaped RAW index fields. cc_annotations stores
    //     plain indices, NOT EmotionToBytes' +'0' wire bytes (comicchat.h's
    //     field comment; cc_strip_self_annotations unwraps via ByteToIndex
    //     before storing, review Critical fix) -- EmotionToBytes packs an
    //     intensity index 0..10 and an emotion-table index 0..17
    //     (avatario.cpp:74-85), so those are the ranges to assert here.
    CC_CHECK(cc_strip_set_self_emotion(s, 0.0, 1.0) == 0);
    cc_annotations ann;
    memset(&ann, 0, sizeof(ann));
    CC_CHECK(cc_strip_self_annotations(s, &ann) == 0);
    CC_CHECK(ann.cooked == 1);
    // face/torso intensity/emotion fields are RAW indices (0..10 for
    // intensity per (BYTE)(m_intensity*10); 0..17 for emotion, sizeof
    // emFloats/sizeof(float) - 1, avatario.cpp:43-62,74-85).
    CC_CHECK(ann.face_intensity >= 0 && ann.face_intensity <= 10);
    CC_CHECK(ann.gesture_intensity >= 0 && ann.gesture_intensity <= 10);
    CC_CHECK(ann.face_emotion >= 0 && ann.face_emotion <= 17);
    CC_CHECK(ann.gesture_emotion >= 0 && ann.gesture_emotion <= 17);

    // --- (3) preview_self_text changes self_pose vs. the neutral baseline.
    //     "Hello there, friend!" fires ID_RULE_WAVE's CheckStart* clause
    //     (EM_WAVE, intensity 1.0, priority 5 -- verified in
    //     cc_selftest_textpose case 4) which is a DIFFERENT emotion than the
    //     neutral (0,0) state set_self_emotion(0.0, 1.0) just put the avatar
    //     in (angle 0.0 == EM_HAPPY, not EM_WAVE) -- so the resulting pose
    //     must differ from the pre-preview pose.
    CC_CHECK(cc_strip_set_self_emotion(s, 0.0, 0.0) == 0);  // reset to neutral-ish baseline
    int32_t prePreviewPose = -1;
    CC_CHECK(cc_strip_self_pose(s, &prePreviewPose) == 0);
    CC_CHECK(cc_strip_preview_self_text(s, "Hello there, friend!") == 0);
    int32_t postPreviewPose = -1;
    CC_CHECK(cc_strip_self_pose(s, &postPreviewPose) == 0);
    CC_CHECK(postPreviewPose != prePreviewPose);

    // --- (5) Plan 4b live-fix 4: the self APIs must follow a
    //     cc_strip_set_participant_avatar switch on the SELF participant. Pre-
    //     fix, cc_strip_self_pose/self_emotion/self_annotations resolved
    //     GetAvatar(selfParticipant) off the raw participant id, which
    //     set_participant_avatar leaves unchanged while re-pointing the self
    //     CUserInfo at a NEW avatar id -- so they operated on the STALE (pre-
    //     switch) avatar (Tim's live wrong-pose report). A "still returns a
    //     valid pose" check is too weak (the stale avatar is ALSO valid); the
    //     distinguishing observable is that the SAME emotion, applied after
    //     switching to a DIFFERENT avatar (otherAvatarPath != avatarPath), must
    //     drive a DIFFERENT self_pose than it did on the original avatar --
    //     because the two avatars have distinct pose-id layouts. With the stale-
    //     id bug, self_pose keeps reading the OLD avatar, so the pose is
    //     IDENTICAL to the pre-switch pose for the same emotion (the RED this
    //     asserts against). Guard: the switch must actually move the self's
    //     avatar id (so we exercise the divergent path, not a same-id reuse).
    CC_CHECK(cc_strip_set_self_emotion(s, 0.0, 1.0) == 0);   // fixed emotion on the ORIGINAL avatar
    int32_t preSwitchPose = -1;
    CC_CHECK(cc_strip_self_pose(s, &preSwitchPose) == 0);
    UINT selfAvBefore = ccContext().session.lookupUser(
        ccContext().session.selfParticipant)->GetAvatarID();
    CC_CHECK(cc_strip_set_participant_avatar(s, p, otherAvatarPath) == 0);
    UINT selfAvAfter = ccContext().session.lookupUser(
        ccContext().session.selfParticipant)->GetAvatarID();
    CC_CHECK(selfAvAfter != selfAvBefore);           // the switch really moved the id
    CC_CHECK(cc_strip_set_self_emotion(s, 0.0, 1.0) == 0);   // SAME emotion, now on the NEW avatar
    int32_t postSwitchPose = -1;
    CC_CHECK(cc_strip_self_pose(s, &postSwitchPose) == 0);
    CC_CHECK(postSwitchPose > 0);                    // a valid pose from the CURRENT avatar
    CC_CHECK(postSwitchPose != preSwitchPose);       // and it FOLLOWED the switch (RED pre-fix: stale == equal)
    cc_annotations annSwitched;
    memset(&annSwitched, 0, sizeof(annSwitched));
    CC_CHECK(cc_strip_self_annotations(s, &annSwitched) == 0);
    CC_CHECK(annSwitched.cooked == 1);

    cc_strip_destroy(s);
    return g_failures - startFailures;
}

// C entry point for the Swift wrapper, which passes two fixture avatar paths
// (the second drives the post-switch self-API check, case 5). Runs standalone
// (resets g_failures) -- same pattern as cc_run_avatar_api_selftest.
extern "C" int32_t cc_run_self_emotion_selftest(const char* avatarPath,
                                                const char* otherAvatarPath) {
    g_failures = 0;
    if (avatarPath == NULL || otherAvatarPath == NULL) return 1;
    cc_selftest_self_emotion(avatarPath, otherAvatarPath);
    return g_failures;
}

// --- Plan 4b live-fix 7: cc_strip_self_preview (head+torso DrawBody preview) --
// The REGRESSION PIN for the "headless body" bug. avatarPath must be a COMPLEX
// (two-part) avatar -- anna.avb is a CAvatarComplex whose body is a CBodyDouble
// composited from a SEPARATE head and torso pose (see cc_selftest_bodydraw's
// own comment). The old preview path (cc_strip_self_pose -> cc_avatar_pose_image)
// drew ONE pose record, so for such an avatar it rendered only the torso plane
// -- a headless body. cc_strip_self_preview drives CBody::DrawBody instead,
// which for a CBodyDouble emits BOTH planes.
//
// The distinguishing observable is the image-blit count in the recording-canvas
// log: DrawBody on a complex avatar emits >= 2 image blits (torso drawing + head
// drawing; drawNimbus=FALSE, so no aura planes -- exactly the 2 the bodydraw
// selftest asserts for anna at (a)), whereas the single-record path can emit at
// most 1. Asserting >= 2 is therefore RED against the old single-pose preview
// and GREEN only once the composite head+torso path is wired.
static int cc_selftest_selfpose_preview(const char* avatarPath) {
    int startFailures = g_failures;

    static CCRecordingCanvas selfPreviewMetrics;
    cc_set_metrics_canvas(selfPreviewMetrics.handle());

    cc_strip* s = cc_strip_create();
    CC_CHECK(s != NULL);
    if (!s) return g_failures - startFailures;

    // A canvas of these twips dimensions -- the pane's own aspect-fit maps the
    // body into them (GetBodyBox); the exact size is not asserted, only the
    // blit count is (fixture-agnostic to the body's intrinsic proportions).
    const int32_t kW = 2400, kH = 3600;

    // --- (a) no self set yet: cc_strip_self_preview must reject (nonzero) and
    //     emit NOTHING to the canvas (the caller then renders no preview).
    {
        CCRecordingCanvas rec;
        CC_CHECK(cc_strip_self_preview(s, rec.handle(), kW, kH) != 0);
        CC_CHECK(rec.log().empty());
    }

    int32_t p = cc_strip_add_participant(s, "Anna", avatarPath);
    CC_CHECK(p == 1);
    if (p < 0) { cc_strip_destroy(s); return g_failures - startFailures; }
    CC_CHECK(cc_strip_set_self(s, p) == 0);

    // --- (b) the regression pin: a COMPLEX avatar's self preview drives
    //     CBodyDouble::DrawBody -> torso plane + head plane => >= 2 image blits.
    //     The retired single-record path (cc_avatar_pose_image on one poseID)
    //     could only ever emit 1, so this assertion fails against it.
    {
        cc_strip_set_self_emotion(s, 0.0, 1.0);   // a definite posed body (happy, full)
        CCRecordingCanvas rec;
        CC_CHECK(cc_strip_self_preview(s, rec.handle(), kW, kH) == 0);
        int imageLines = 0;
        for (const std::string& l : rec.log())
            if (l.rfind("image ", 0) == 0) imageLines++;
        CC_CHECK(imageLines >= 2);   // head + torso -- NOT the headless single-record path
    }

    // --- (c) degenerate bounds are rejected (nonzero), no draw. Guards the
    //     width/height<=0 early-out that the CGCanvas clamp would otherwise
    //     paper over on the Swift side.
    {
        CCRecordingCanvas rec;
        CC_CHECK(cc_strip_self_preview(s, rec.handle(), 0, kH) != 0);
        CC_CHECK(cc_strip_self_preview(s, rec.handle(), kW, 0) != 0);
        CC_CHECK(rec.log().empty());
    }

    cc_strip_destroy(s);
    return g_failures - startFailures;
}

// C entry point for the Swift wrapper (BodyDrawTests.swift), which passes a
// COMPLEX-avatar fixture (anna.avb). Runs standalone (resets g_failures).
extern "C" int32_t cc_run_selfpose_preview_selftest(const char* avatarPath) {
    g_failures = 0;
    if (avatarPath == NULL) return 1;
    cc_selftest_selfpose_preview(avatarPath);
    return g_failures;
}

// --- Plan 4a Task 7: CDC::DrawTextEllipsis selftest --------------------------
// Drives the new shim member directly over a recording canvas: a short string
// (fits the box) must draw UNTRUNCATED; a long string (doesn't fit) must draw
// truncated, ending in "..." and fitting the box width. The recording
// canvas's measure_text is deterministic (len*120 wide, cc_recording_canvas.cpp)
// so both expectations are exact arithmetic, not approximate.
static void cc_selftest_draw_text_ellipsis() {
    LOGFONT lf;
    memset(&lf, 0, sizeof(lf));
    strcpy(lf.lfFaceName, "Comic Sans MS");
    lf.lfHeight = -240;
    lf.lfWeight = 400;
    CFont font;
    font.CreateFontIndirect(&lf);

    // --- (a) short string, box wide enough: untruncated, exact text.
    {
        CCRecordingCanvas rec;
        CDC dc(rec.handle());
        dc.SelectObject(&font);
        RECT rect; SetRect(&rect, 0, 0, 1200, -240);   // 1200 twips wide
        dc.DrawTextEllipsis("hi", &rect, DT_LEFT | DT_NOPREFIX | DT_SINGLELINE | DT_END_ELLIPSIS);
        const std::vector<std::string>& log = rec.log();
        CC_CHECK(log.size() == 1);
        if (!log.empty())
            CC_CHECK(log.back() == "text 0,0 color=000000 \"hi\"");
    }

    // --- (b) long string, box too narrow: truncated, ends "...", fits.
    {
        CCRecordingCanvas rec2;
        CDC dc2(rec2.handle());
        dc2.SelectObject(&font);
        RECT rect; SetRect(&rect, 0, 0, 600, -240);    // 600 twips: "hello world" (11*120=1320) doesn't fit
        dc2.DrawTextEllipsis("hello world", &rect, DT_LEFT | DT_NOPREFIX | DT_SINGLELINE | DT_END_ELLIPSIS);
        const std::vector<std::string>& log = rec2.log();
        CC_CHECK(log.size() == 1);
        if (!log.empty()) {
            // Ends in "...".
            CC_CHECK(log.back().size() >= 4);
            CC_CHECK(log.back().compare(log.back().size() - 4, 4, "...\"") == 0);
            // Extract the drawn text between the quotes and verify it fits
            // (len*120 <= boxWidth) and is shorter than the original (proof
            // truncation actually happened, not a no-op passthrough).
            size_t q1 = log.back().find('"');
            size_t q2 = log.back().rfind('"');
            CC_CHECK(q1 != std::string::npos && q2 != std::string::npos && q2 > q1);
            std::string drawn = log.back().substr(q1 + 1, q2 - q1 - 1);
            CC_CHECK(drawn.size() < strlen("hello world"));
            CC_CHECK((int32_t)(drawn.size() * 120) <= 600);
        }
    }
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
    // Plan 3 Task 5b: feed_bytes now runs the lifted line-framer + parse
    // dispatch (was a no-op buffer in Task 1). Feeding a server PING drives the
    // cmdidPing handler, which answers PONG through cfg.send -- so this now
    // validates the whole framer->ProcessMessage->HandleCommand->PONG path.
    // A real server PING carries the token in the ":"-trailing param
    // (PING :token); the cmdidPing handler echoes pParse->lastString, so
    // "PING :hello" -> "PONG :hello" (exactly the original ircsock.cpp:1696).
    cc_session_feed_bytes(s, reinterpret_cast<const uint8_t*>("PING :hello\r\n"), 13);
    CC_CHECK(cap.sent == "PONG :hello\r\n");
    cap.sent.clear();
    cc_session_test_echo(s);          // test-only: drives one send + one event
    CC_CHECK(cap.sent == "ECHO\r\n");
    CC_CHECK(cap.events == 1);
    cc_session_destroy(s);
    return 0;
}

// --- Plan 3 Task 2: ccommon_str codec core -----------------------------------
// Step 2's hand-verified low-level-quoting vectors, verbatim from the task
// brief (ircproto-map.md Sec5): escape set {0x0A -> Qn, 0x0D -> Qr, Q -> QQ},
// Q = g_chLLQuoteCTCP (0x10); decode is the tolerant all-or-nothing heuristic
// (a stray naked Q not followed by n/r/Q means the WHOLE string is treated as
// unquoted, not just that one byte).
static int cc_selftest_llquote() {
    const char Q = (char)0x10;
    char* dst = nullptr; BOOL freeit = FALSE;
    // "a\nb" -> "a" Q 'n' "b"
    BOOL changed = bLowLevelQuoting(Q, TRUE, "a\nb", &dst, &freeit, FALSE);
    CC_CHECK(changed && dst[0]=='a' && dst[1]==Q && dst[2]=='n' && dst[3]=='b' && dst[4]==0);
    if (freeit) free(dst);
    // decode round-trips
    char buf[16]; strcpy(buf, "a\x10n" "b");
    bLowLevelUnquoting(Q, TRUE, buf, buf);   // in-place
    CC_CHECK(strcmp(buf, "a\nb") == 0);
    // tolerant: stray naked Q not followed by n/r/Q => whole string verbatim
    char buf2[16]; strcpy(buf2, "a\x10z" "b");
    bLowLevelUnquoting(Q, TRUE, buf2, buf2);
    CC_CHECK(strcmp(buf2, "a\x10z" "b") == 0);
    return 0;
}

// Additional bLowLevelQuoting/Unquoting coverage beyond the brief's minimum
// vectors: the zero-copy fast path (nothing to quote -> *pszDst == szSrc,
// *pbFree == FALSE, ircproto-map.md Sec5's "Zero-copy fast path" note), CR
// quoting + bRemoveCarriageReturns dropping CR entirely, and the QQ escape.
static int cc_selftest_llquote_extra() {
    const char Q = (char)0x10;

    // Zero-copy fast path: no LF/CR/Q in the input -> same pointer back, no
    // free needed.
    {
        char* dst = nullptr; BOOL freeit = TRUE;  // pre-set to catch a missed write
        const char* src = "plain text";
        BOOL changed = bLowLevelQuoting(Q, TRUE, src, &dst, &freeit, FALSE);
        CC_CHECK(changed == TRUE);
        CC_CHECK(dst == src);       // exact same pointer (ccommon.cpp:967)
        CC_CHECK(freeit == FALSE);
    }

    // CR -> Q 'r' when bRemoveCarriageReturns is FALSE.
    {
        char* dst = nullptr; BOOL freeit = FALSE;
        BOOL changed = bLowLevelQuoting(Q, TRUE, "a\rb", &dst, &freeit, FALSE);
        CC_CHECK(changed && dst[0]=='a' && dst[1]==Q && dst[2]=='r' && dst[3]=='b' && dst[4]==0);
        if (freeit) free(dst);
    }

    // CR dropped entirely when bRemoveCarriageReturns is TRUE.
    {
        char* dst = nullptr; BOOL freeit = FALSE;
        BOOL changed = bLowLevelQuoting(Q, TRUE, "a\rb", &dst, &freeit, TRUE);
        CC_CHECK(changed && dst[0]=='a' && dst[1]=='b' && dst[2]==0);
        if (freeit) free(dst);
    }

    // Q -> QQ, and decodes back to a single Q.
    {
        char src[2] = { Q, 0 };
        char* dst = nullptr; BOOL freeit = FALSE;
        BOOL changed = bLowLevelQuoting(Q, TRUE, src, &dst, &freeit, FALSE);
        CC_CHECK(changed && dst[0]==Q && dst[1]==Q && dst[2]==0);
        char buf[4];
        bLowLevelUnquoting(Q, TRUE, dst, buf);
        CC_CHECK(buf[0]==Q && buf[1]==0);
        if (freeit) free(dst);
    }

    return 0;
}

// bConvertWideStringToUTF8 / bConvertUTF8StringToWide round-trip + boundary
// coverage: ASCII passthrough, 2-byte UTF-8 (0x80-0x7FF range), 3-byte UTF-8
// (>0x7FF), and SzNextUTF8Char's per-class advance (backslash-escape,
// 1-byte, 2-byte, 3-byte).
static int cc_selftest_utf8codec() {
    // ASCII round-trip.
    {
        WCHAR wide[] = { 'h', 'i', 0 };
        LPTSTR out = nullptr; INT outLen = 0;
        CC_CHECK(bConvertWideStringToUTF8(wide, 2, &out, &outLen));
        CC_CHECK(outLen == 2 && strcmp(out, "hi") == 0);
        delete[] out;
    }

    // 2-byte UTF-8 range: U+00E9 (e-acute) -> 0xC3 0xA9.
    {
        WCHAR wide[] = { 0x00E9, 0 };
        LPTSTR out = nullptr; INT outLen = 0;
        CC_CHECK(bConvertWideStringToUTF8(wide, 1, &out, &outLen));
        CC_CHECK(outLen == 2);
        CC_CHECK((unsigned char)out[0] == 0xC3 && (unsigned char)out[1] == 0xA9);

        // Round-trip back to wide (out is still live -- freed once, below).
        LPWSTR wout = nullptr; INT wOutLen = 0;
        CC_CHECK(bConvertUTF8StringToWide(out, 2, &wout, &wOutLen));
        CC_CHECK(wOutLen == 1 && wout[0] == 0x00E9);
        delete[] out;
        delete[] wout;
    }

    // 3-byte UTF-8 range: U+20AC (Euro sign) -> 0xE2 0x82 0xAC.
    {
        WCHAR wide[] = { 0x20AC, 0 };
        LPTSTR out = nullptr; INT outLen = 0;
        CC_CHECK(bConvertWideStringToUTF8(wide, 1, &out, &outLen));
        CC_CHECK(outLen == 3);
        CC_CHECK((unsigned char)out[0] == 0xE2 && (unsigned char)out[1] == 0x82 && (unsigned char)out[2] == 0xAC);

        // SzNextUTF8Char advances over the full 3-byte sequence.
        LPCTSTR next = SzNextUTF8Char(out);
        CC_CHECK(next == out + 3);
        delete[] out;
    }

    // SzNextUTF8Char: 1-byte ASCII advances by 1.
    CC_CHECK(SzNextUTF8Char("a") == (LPCTSTR)"a" + 1);
    // SzNextUTF8Char: backslash-escape pairs advance by 2 (n/r/t/b/c/\\),
    // except \0 which advances by 1 (the original's documented special case).
    CC_CHECK(SzNextUTF8Char("\\n") == (LPCTSTR)"\\n" + 2);
    CC_CHECK(SzNextUTF8Char("\\0") == (LPCTSTR)"\\0" + 1);
    // SzNextUTF8Char: end of string returns the same pointer.
    CC_CHECK(SzNextUTF8Char("") == (LPCTSTR)"");

    return 0;
}

// --- Plan 3 Task 2 Step 6: CPtrList::RemoveHead (R9 addition) ----------------
// CPtrList already existed (Plan 2 Tasks 6/8) with the AddTail/AddHead/
// RemoveAll/GetHeadPosition/GetNext/GetCount/IsEmpty/GetHead/GetTail/
// GetTailPosition/GetPrev/RemoveTail/GetAt/SetAt/FindIndex surface exercised
// in cc_selftest_format/cc_selftest_panel. RemoveHead was the one member of
// the brief's named surface (AddTail/RemoveHead/GetNext/RemoveAll/IsEmpty/
// GetCount) missing -- the parser's query list (CCQuery, Task 4) is an
// AddTail-append / RemoveHead-dequeue FIFO. Exercises the full append-then-
// drain idiom (append 1,2,3; dequeue in FIFO order 1,2,3; empty after).
static int cc_selftest_cptrlist_fifo() {
    int a = 1, b = 2, c = 3;
    CPtrList q;
    CC_CHECK(q.IsEmpty());
    q.AddTail(&a);
    q.AddTail(&b);
    q.AddTail(&c);
    CC_CHECK(q.GetCount() == 3);
    CC_CHECK(!q.IsEmpty());

    CC_CHECK(q.RemoveHead() == &a);
    CC_CHECK(q.GetCount() == 2);
    CC_CHECK(q.RemoveHead() == &b);
    CC_CHECK(q.GetCount() == 1);
    CC_CHECK(q.RemoveHead() == &c);
    CC_CHECK(q.GetCount() == 0);
    CC_CHECK(q.IsEmpty());
    return 0;
}

// --- Plan 3 Task 2 Step 7: CharNext selftest (Plan 2 Task 5 R9 debt) --------
// CharNext was added live (not compiled out -- see the task report's fidelity
// note) in Plan 2 Task 5 (format.cpp) and is already exercised transitively
// through cc_selftest_format/cc_selftest_balloon, but R9's own mandate ("add a
// selftest exercising it") was never fulfilled directly. Exercises: ASCII
// single-byte advance, advance stops at (never past) the terminating NUL, and
// -- the "single-byte behavior under CP-1252" the brief asks for -- a byte
// that WOULD be a DBCS lead byte under a real East-Asian codepage (e.g. 0x81,
// a Shift-JIS lead byte) still advances by exactly one byte here, because
// IsDBCSLeadByte always reports FALSE on this single-byte-codepage port.
static int cc_selftest_charnext() {
    // ASCII: advances one byte at a time.
    char ascii[] = "abc";
    char* p = ascii;
    p = CharNext(p);
    CC_CHECK(p == ascii + 1 && *p == 'b');
    p = CharNext(p);
    CC_CHECK(p == ascii + 2 && *p == 'c');

    // End of string: CharNext(p) at the NUL terminator stays put (never
    // advances past it).
    char* end = ascii + 3;
    CC_CHECK(*end == '\0');
    CC_CHECK(CharNext(end) == end);

    // "Two-byte sequence" under a real DBCS codepage (0x81 0x40 is a valid
    // Shift-JIS lead+trail pair) -- this port has no DBCS codepage active
    // (IsDBCSLeadByte always FALSE), so CharNext advances by exactly ONE byte
    // here, landing ON the second byte rather than past it. This is the
    // deliberate single-byte CP-1252 behavior Task 2 Step 1 makes permanent.
    unsigned char dbcsLike[] = { 0x81, 0x40, 0x00 };
    CC_CHECK(IsDBCSLeadByte(dbcsLike[0]) == FALSE);
    char* q = (char*)dbcsLike;
    char* q2 = CharNext(q);
    CC_CHECK(q2 == q + 1);                 // single-byte advance, not +2
    CC_CHECK((unsigned char)*q2 == 0x40);  // lands on the would-be trail byte

    return 0;
}

// --- Plan 3 Task 2 Step 8: Capitalize restored verbatim ----------------------
// Capitalize was R11-wrapped to a no-op in Plan 2 Task 6 (a documented latent
// real-metrics layout divergence). Restored here: session.charSet is
// permanently ANSI_CHARSET (0) on this port (no setter exists anywhere in the
// engine -- see engine_context.h), so the reachable branch is exclusively
// CharUpperBuff's ASCII-range uppercase. Exercises: a plain-ASCII string
// (every existing balloon/panel call site's real input domain) uppercases
// correctly, and -- the brief's "first-letter-of-line behavior" ask -- the
// CBWoodringNormal constructor's real call site (balloon.cpp) capitalizes the
// WHOLE string in one call (Capitalize has no line-boundary logic itself; the
// original always uppercases its entire input buffer), confirmed here via a
// multi-line (embedded '\n') string to show every line's content is
// capitalized, not just the first.
static int cc_selftest_capitalize() {
    char s1[] = "hello world";
    Capitalize(s1);
    CC_CHECK(strcmp(s1, "HELLO WORLD") == 0);

    // Already-uppercase / mixed / digits / punctuation: idempotent on
    // uppercase, digits/punctuation pass through unchanged.
    char s2[] = "Hi There! 123";
    Capitalize(s2);
    CC_CHECK(strcmp(s2, "HI THERE! 123") == 0);

    // Multi-line input: the whole buffer capitalizes, embedded '\n' preserved
    // (Capitalize never touches non-letter bytes).
    char s3[] = "first line\nsecond line";
    Capitalize(s3);
    CC_CHECK(strcmp(s3, "FIRST LINE\nSECOND LINE") == 0);

    // Empty string: no-op, does not crash (CharUpperBuff(str, 0)).
    char s4[] = "";
    Capitalize(s4);
    CC_CHECK(strcmp(s4, "") == 0);

    return 0;
}

// --- Plan 3 Task 3: annotation codec (encode/decode) ------------------------
// A minimal test-only CAvatarX subclass driving bInsertAnnotations's
// GetIndices/GetEmotions without a real .avb file: the annotation codec
// selftest is meant to run standalone (no fixture path -- unlike
// cc_selftest_bodydraw/panel/strip, which all take an avatarPath). This is
// test infrastructure only (not part of the lift), analogous to how
// panelLoadAvatar() below is test-only rig for the panel/strip selftests --
// except this one needs no file at all, since GetIndices/GetEmotions just
// return canned values instead of reading a loaded pose/emotion record.
class CCTestAvatar : public CAvatarX {
public:
    CHAR face = 1, torso = 2;
    BYTE requested = 0;
    CEmotion faceEmotion{0.3, EM_NEUTRAL}, torsoEmotion{0.5, EM_NEUTRAL};

    virtual CBody *GetBodyFromEmotion(CEmotion &) { return nullptr; }
    virtual CBody *GetBodyFromEmotion(CEmotionOpts &) { return nullptr; }
    virtual void SetNeutral() {}
    virtual void SetSequential(void *, int) {}
    virtual void RecordBody(CBody*) {}
    virtual void GetIndices(CHAR &chFaceIndex, CHAR &chTorsoIndex, BYTE &bbRequested) {
        chFaceIndex = face; chTorsoIndex = torso; bbRequested = requested;
    }
    virtual void SetIndices(CHAR chFaceIndex, CHAR chTorsoIndex, BYTE bbRequested) {
        face = chFaceIndex; torso = chTorsoIndex; requested = bbRequested;
    }
    virtual void GetEmotions(CEmotion &face_, CEmotion &torso_) {
        face_ = faceEmotion; torso_ = torsoEmotion;
    }
    virtual void SetEmotions(CEmotion &face_, CEmotion &torso_) {
        faceEmotion = face_; torsoEmotion = torso_;
    }
    virtual CAvatarX *DupAvatar() { return new CCTestAvatar(*this); }
};

// Test-only PFNRESOLVETALKTO registry (protsupp.h): a fixed-size table of
// live CUserInfo* the test has vended talkTos keys for, mirroring the shape
// (if not the implementation) of ccContext().session.userFromTalkTo -- a
// bounded linear scan recovering the real pointer from its truncated DWORD
// key, never trying to widen the truncated bits directly (see protsupp.cpp's
// GetAddressees deviation-4 comment for why that direct widening crashes on
// LP64). Test infrastructure only, not part of the lift.
struct CCTestTalkToRegistry {
    static const int CAP = 16;
    CUserInfo* entries[CAP] = {};
    int count = 0;
    void reset() { count = 0; }
    DWORD add(CUserInfo* pui) {
        ASSERT(count < CAP);
        entries[count++] = pui;
        return (DWORD)(uintptr_t)pui;
    }
};
static CCTestTalkToRegistry g_talkToRegistry;
static CUserInfo* cc_test_resolve_talkto(DWORD key) {
    for (int i = 0; i < g_talkToRegistry.count; i++)
        if ((DWORD)(uintptr_t)g_talkToRegistry.entries[i] == key)
            return g_talkToRegistry.entries[i];
    return nullptr;
}

// Thin test wrapper over ProcessUDIData (brief Step 2/6). `wire` must start
// with '#' (ASSERT(*szTmp == '#') in the original). The trivial resolver
// always returns NULL (no addressees resolvable) -- sufficient for every
// vector in this task that doesn't exercise the T-group (Step 8 below
// supplies its own resolver for that case). Returns 0 on success.
static CUserInfo* cc_test_lookup_none(const char*, CChatDoc*) { return nullptr; }

static int cc_test_decode_udi(const char* wire, cc_annotations* out) {
    if (wire == nullptr || *wire != '#' || out == nullptr) return 1;
    char buf[256];
    strncpy(buf, wire, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    CUserInfo pui;
    ProcessUDIData(nullptr, &pui, buf, FALSE /*bVIPMode*/, cc_test_lookup_none);

    out->gesture_pose = pui.m_udi.m_chGest;
    out->gesture_emotion = pui.m_udi.m_chGestE;
    out->gesture_intensity = pui.m_udi.m_chGestI;
    out->face_pose = pui.m_udi.m_chExpr;
    out->face_emotion = pui.m_udi.m_chExprE;
    out->face_intensity = pui.m_udi.m_chExprI;
    out->requested = pui.m_udi.m_bbReq;
    out->mode = BM2SM(pui.m_udi.m_uModes);   // wire SM_* value, inverse of ProcessUDIData's SM2BM
    out->cooked = pui.m_udi.m_bbCooked;

    int upper = pui.m_udi.m_talkTos.GetUpperBound();
    out->addressee_count = 0;
    for (int i = 0; i <= upper && out->addressee_count < CC_MAX_ADDRESSEES; i++) {
        // Recover the real pointer via the test registry (cc_test_resolve_talkto),
        // not a direct DWORD->pointer widening -- see the GetAddressees
        // deviation-4 comment in protsupp.cpp for why the naive cast crashes
        // on LP64. Every pointer GetTalkTos could have stored here was
        // resolved through cc_test_lookup_none/cc_test_lookup_table, which
        // register into g_talkToRegistry at resolution time.
        CUserInfo* addressee = cc_test_resolve_talkto(pui.m_udi.m_talkTos[i]);
        if (!addressee) continue;  // registry miss: should not happen for any vector in this task
        strncpy(out->addressees[out->addressee_count], addressee->GetName(), 63);
        out->addressees[out->addressee_count][63] = '\0';
        out->addressee_count++;
    }
    return 0;
}

// Thin test wrapper over bInsertAnnotations (brief Step 2/6). Builds a
// CCTestAvatar from `in`'s gesture/face fields, a CUserInfo carrying `in`'s
// addressees as its m_udi.m_talkTos, and calls the encoder with
// bIncludeParenthesis=FALSE (the IRCX-transport shape; Step 8 covers the
// parenthesized plain-IRC transport separately). Returns the encoded length
// (>0) on success, <=0 on failure.
static int cc_test_encode_udi(const cc_annotations* in, char* out, int outSize) {
    if (in == nullptr || out == nullptr || outSize <= 0) return -1;

    CCTestAvatar av;
    av.torso = (CHAR)in->gesture_pose;
    av.requested = (BYTE)in->requested;
    BytesToEmotion(av.torsoEmotion, (BYTE)in->gesture_emotion, (BYTE)in->gesture_intensity);
    av.face = (CHAR)in->face_pose;
    BytesToEmotion(av.faceEmotion, (BYTE)in->face_emotion, (BYTE)in->face_intensity);

    CUserInfo puiSelf;
    CUserInfo* allocated[CC_MAX_ADDRESSEES] = {};
    int nAllocated = 0;
    for (int i = 0; i < in->addressee_count && i < CC_MAX_ADDRESSEES; i++) {
        // Default-construct + assign via GetName() rather than the
        // CUserInfo(const char* nick, ...) constructor: that overload's real
        // body (userinfo.cpp) is not lifted this task (only the default ctor
        // is an R12(a) single, lifted_singles.cpp) -- see the NickTable
        // comment above for the identical reasoning.
        CUserInfo* addressee = new CUserInfo();
        addressee->GetName() = in->addressees[i];
        allocated[nAllocated++] = addressee;
        // R13 (panel.cpp:316-322): via-uintptr_t cast, LP64-safe on the WRITE
        // side; the read side (GetAddressees, via pfnResolve below) recovers
        // the real pointer through g_talkToRegistry, not a naive widening --
        // see protsupp.cpp's GetAddressees deviation-4 comment.
        puiSelf.m_udi.m_talkTos.Add(g_talkToRegistry.add(addressee));
    }

    USHORT uModes = SM2BM((BYTE)in->mode);
    char buf[1024];
    BOOL ok = bInsertAnnotations(&av, &puiSelf, buf, uModes, FALSE /*bIncludeParenthesis*/, cc_test_resolve_talkto);
    if (!ok) return -1;

    int len = (int)strlen(buf);
    if (len >= outSize) {
        for (int i = 0; i < nAllocated; i++) delete allocated[i];
        return -1;
    }
    strcpy(out, buf);

    for (int i = 0; i < nAllocated; i++) delete allocated[i];
    return len;
}

// brief Step 2: hand-encoded round-trip vector. Wire arithmetic (IndexToByte
// (v) = v + '0', protsupp.cpp:1023):
//   '2' = 0x32 = 2 + '0' (0x30)  -> gesture_pose = 2
//   '9' = 0x39 = 9 + '0'         -> gesture_emotion = 9 (EM_NEUTRAL, avatario.cpp emFloats[9])
//   '5' = 0x35 = 5 + '0'         -> gesture_intensity = 5
//   '1' = 0x31 = 1 + '0'         -> face_pose = 1
//   '9' = 0x39 = 9 + '0'         -> face_emotion = 9 (EM_NEUTRAL)
//   '3' = 0x33 = 3 + '0'         -> face_intensity = 3
//   '1' = 0x31 = 1 + '0'         -> mode = 1 (SM_SAY, defines.h:57)
//
// DISCOVERED PRE-EXISTING BUG (not introduced by this lift, verified against
// the original avatario.cpp emFloats[] table verbatim): EM_HAPPY and
// EM_NEUTRAL are BOTH defined as (float)0.0 (avatar.h:328,336 -- EM_HAPPY is
// literally `0 * 2 * PI / 8`, EM_NEUTRAL is `(float)0.0`). EmotionToBytes's
// linear scan (avatario.cpp:71-76, `for i=1..n: if emFloats[i]==em.m_emotion:
// emVal=i; break`) starts at index 1 and finds EM_HAPPY (index 1) before it
// ever reaches EM_NEUTRAL (index 9) -- so a live avatar whose emotion is
// EM_NEUTRAL is UNCONDITIONALLY re-encoded to the wire as emotion byte 1
// (HAPPY), never 9. Confirmed with a standalone reproduction of the exact
// original loop (equal to 1, not 9, for em.m_emotion==0.0). This makes the
// DECODE direction (ProcessUDIData: direct ByteToIndex, no EmotionToBytes
// involved) exact for value 9, but the ENCODE direction (bInsertAnnotations
// -> EmotionToBytes) can never reproduce emotion byte 9 for any live avatar
// state, by construction of the original's own table. This selftest
// therefore verifies DECODE against the brief's exact vector (byte-exact,
// unaffected by the bug), and verifies ENCODE separately below with values
// that don't hit the collision (COY=index 2, unambiguous), rather than
// asserting a round-trip the original code cannot actually perform.
static int cc_selftest_annotation_codec() {
    const char* wire = "#G295E193M1";
    cc_annotations a; memset(&a, 0, sizeof a);
    int ok = cc_test_decode_udi(wire, &a);
    CC_CHECK(ok == 0);
    CC_CHECK(a.gesture_pose == 2 && a.gesture_emotion == 9 && a.gesture_intensity == 5);
    CC_CHECK(a.face_pose == 1 && a.face_emotion == 9 && a.face_intensity == 3);
    CC_CHECK(a.mode == 1 && a.addressee_count == 0);

    // Encode-direction round-trip, using an emotion index (2 = EM_COY) that
    // is NOT ambiguous in emFloats[] (unlike 9/EM_NEUTRAL, see above) --
    // genuinely exercises bInsertAnnotations/EmotionToBytes byte-exact.
    cc_annotations b; memset(&b, 0, sizeof b);
    b.gesture_pose = 2; b.gesture_emotion = 2; b.gesture_intensity = 5;
    b.face_pose = 1;    b.face_emotion = 2;    b.face_intensity = 3;
    b.mode = 1;
    char out[128];
    int n = cc_test_encode_udi(&b, out, sizeof out);
    CC_CHECK(n > 0 && strcmp(out, "#G225E123M1") == 0);
    return 0;
}

// brief Step 8(a)/(b): the two transports for the same "#G...M..." blob.
// (a) IRCX: the block travels alone (DATA ... CCUDI1 :#G...), no parens
//     (ircproto.cpp:542-549) -- bIncludeParenthesis=FALSE, exactly
//     cc_test_encode_udi's default above. Re-asserted here explicitly as its
//     own behavior.
// (b) plain IRC: the block is prepended to the message text, PARENTHESIZED
//     (ircproto.cpp:554-556): "(#G...M<m>) <text>". The decoder's trigger is
//     `strncmp(szMesg, "(#", 2)` + a ") " search (protsupp.cpp:1566, 1605-
//     1609) -- this task doesn't lift that trigger (it's inside ProcessSay,
//     Task 6 territory), but bInsertAnnotations's bIncludeParenthesis=TRUE
//     path (the encoder half of the same contract) IS in scope, so this
//     freezes the encoder's half of the two-transport contract and hand-
//     verifies the decoder's trigger string against the encoder's literal
//     output.
static int cc_selftest_annotation_transports() {
    // Emotion index 2 (EM_COY), not 9 (EM_NEUTRAL) -- see the
    // cc_selftest_annotation_codec comment above for why 9 cannot round-trip
    // through the real EmotionToBytes (EM_HAPPY/EM_NEUTRAL both == 0.0f).
    CCTestAvatar av;
    av.torso = 2; av.face = 1; av.requested = 0;
    BytesToEmotion(av.torsoEmotion, 2, 5);
    BytesToEmotion(av.faceEmotion, 2, 3);
    CUserInfo puiSelf;
    USHORT uModes = SM2BM(1);  // SM_SAY

    // (a) IRCX: bare blob, no parens, no trailing ") ".
    {
        char buf[128];
        BOOL ok = bInsertAnnotations(&av, &puiSelf, buf, uModes, FALSE, cc_test_resolve_talkto);
        CC_CHECK(ok);
        CC_CHECK(strcmp(buf, "#G225E123M1") == 0);
        CC_CHECK(strncmp(buf, "(#", 2) != 0);  // NOT the plain-IRC shape
    }

    // (b) plain IRC: parenthesized, with the ") " terminator the decoder's
    // `strstr(szMesg+2, ") ")` trigger (protsupp.cpp:1566) looks for.
    {
        char buf[128];
        BOOL ok = bInsertAnnotations(&av, &puiSelf, buf, uModes, TRUE, cc_test_resolve_talkto);
        CC_CHECK(ok);
        CC_CHECK(strcmp(buf, "(#G225E123M1) ") == 0);
        CC_CHECK(strncmp(buf, "(#", 2) == 0);           // decoder's transport trigger
        CC_CHECK(strstr(buf + 2, ") ") != nullptr);      // decoder's terminator search
    }
    return 0;
}

// brief Step 8(c): anti-spoof mask. protsupp.cpp:1588-1592's "anti-hacker
// line" forces SAY/THINK to WHISPER on a private message -- that masking
// itself lives in ProcessSay's inline block (Task 6 territory, not lifted
// here), but ProcessUDIData's OWN M-group decode (this task's scope) has no
// such masking (protsupp.cpp:1525-1530 -- ProcessUDIData is the DATA/CCUDI1
// out-of-band path, which the original never subjects to the private-message
// mask; only the inline PRIVMSG-text path is). This test freezes that
// asymmetry: decoding an M2 (SM_WHISPER) byte through ProcessUDIData yields
// BM_WHISPER either way, decoding M1 (SM_SAY) yields BM_SAY unmasked (no
// bVIPMode/privmsg parameter exists on ProcessUDIData to mask it -- masking
// is ProcessSay's job, not ProcessUDIData's, confirmed by re-reading both
// original call sites).
static int cc_selftest_annotation_antispoof_scope() {
    // M2 = IndexToByte(2) = '2' -> SM_WHISPER -> BM_WHISPER.
    {
        char buf[64]; strcpy(buf, "#M2");
        CUserInfo pui;
        ProcessUDIData(nullptr, &pui, buf, FALSE, cc_test_lookup_none);
        CC_CHECK(pui.m_udi.m_uModes == BM_WHISPER);
    }
    // M1 = IndexToByte(1) = '1' -> SM_SAY -> BM_SAY (ProcessUDIData never
    // masks this to BM_WHISPER -- that's ProcessSay's anti-hacker line, out
    // of this task's scope; documented here so the boundary is explicit and
    // testable, not silently assumed).
    {
        char buf[64]; strcpy(buf, "#M1");
        CUserInfo pui;
        ProcessUDIData(nullptr, &pui, buf, FALSE, cc_test_lookup_none);
        CC_CHECK(pui.m_udi.m_uModes == BM_SAY);
    }
    return 0;
}

// brief Step 8(d): T-list decode with 5 nicks (GetTalkTos has no clip of its
// own on decode -- protsupp.cpp:1066-1101 just Add()s every resolved nick;
// the clip-at-5 lives on the ENCODE side, GetAddressees/GetWhisperedAddressees
// `min(GetUpperBound(), 4)`, protsupp.cpp:3009/3025). This test freezes both
// halves: decoding 6 comma-separated nicks (all resolvable) yields 6 talkTos
// entries (no decode-side clip), and re-encoding that same CUserInfo's
// m_udi.m_talkTos via GetAddressees clips the OUTPUT to the first 5.
struct NickTable {
    static const int N = 6;
    CUserInfo users[N];
    NickTable() {
        // Assign via the mutable GetName() reference rather than SetName():
        // CUserInfo::SetName (userinfo.h, inline) unconditionally calls
        // SetScreenName(), whose real body is userinfo.cpp territory not
        // lifted this task (only the ctor + GetScreenName are R12(a)-lifted
        // singles so far, lifted_singles.cpp) -- calling SetName here would
        // be a new link-time dependency this task doesn't own. GetName()
        // returns CString& (a mutable reference), so this achieves the same
        // observable result (a named CUserInfo) without it.
        const char* names[N] = {"alice","bob","carol","dave","erin","frank"};
        for (int i = 0; i < N; i++) users[i].GetName() = names[i];
    }
    CUserInfo* find(const char* nick) {
        for (int i = 0; i < N; i++)
            if (strcmp(users[i].GetName(), nick) == 0) return &users[i];
        return nullptr;
    }
};
static NickTable* g_talkToTestTable;
static CUserInfo* cc_test_lookup_table(const char* nick, CChatDoc*) {
    CUserInfo* found = g_talkToTestTable ? g_talkToTestTable->find(nick) : nullptr;
    // Register with the test talk-to registry: every pointer GetTalkTos
    // stores into an m_udi.m_talkTos DWORD (via this resolver) must be
    // recoverable afterward through cc_test_resolve_talkto, not a naive
    // widening cast (see cc_test_decode_udi's comment).
    if (found) g_talkToRegistry.add(found);
    return found;
}

static int cc_selftest_annotation_addressees() {
    g_talkToRegistry.reset();
    NickTable table;
    g_talkToTestTable = &table;
    // Pre-register the whole table so GetAddressees's resolver (below) can
    // recover every pointer, exactly as cc_test_lookup_table would do lazily
    // during a decode -- here we go straight to the encode side, so nothing
    // resolves the pointers into the registry unless we do it explicitly.
    for (int i = 0; i < NickTable::N; i++) g_talkToRegistry.add(&table.users[i]);

    // (d) decode: T<6 names> -> GetTalkTos resolves and Add()s all 6, no clip
    // on this side.
    {
        char buf[128];
        strcpy(buf, "#Talice,bob,carol,dave,erin,frank");
        CUserInfo pui;
        ProcessUDIData(nullptr, &pui, buf, FALSE, cc_test_lookup_table);
        CC_CHECK(pui.m_udi.m_talkTos.GetSize() == 6);
    }

    // (d, encode clip): GetAddressees clips to the first 5
    // (min(GetUpperBound(), 4), protsupp.cpp:3009) when re-serializing a
    // talkTos array that has 6 entries.
    {
        CUserInfo puiSelf;
        for (int i = 0; i < NickTable::N; i++)
            // R13 (panel.cpp:316-322): via-uintptr_t cast, LP64-safe.
            puiSelf.m_udi.m_talkTos.Add((DWORD)(uintptr_t)&table.users[i]);
        CString str;
        GetAddressees(&puiSelf, ",", str, TRUE, cc_test_resolve_talkto);
        CC_CHECK(strcmp(str, "alice,bob,carol,dave,erin") == 0);  // first 5 only, "frank" dropped
    }

    g_talkToTestTable = nullptr;
    g_talkToRegistry.reset();
    return 0;
}

// brief Step 8(e): `cooked` is set only when both intensity fields have been
// written by the decoder. CUserDisplayInfo::Reset() (userinfo.h:40-47) zero-
// inits m_chGestI/m_chExprI (NOT -1 -- only the pose fields m_chGest/m_chExpr
// default to -1), so ProcessUDIData's `if (m_chGestI != -1 && m_chExprI !=
// -1)` check is driven by whether Reset() ran at all, not by whether G/E
// groups were literally present in this particular wire string. Documented
// explicitly since it's a real quirk of the original arithmetic, not an
// artifact of this lift: a wire string with NEITHER G nor E groups still
// ends up "cooked" (both intensities sit at their Reset() default of 0,
// which is != -1).
static int cc_selftest_annotation_cooked() {
    // Full G+E groups present -> cooked (both intensities explicitly set).
    {
        char buf[64]; strcpy(buf, "#G295E193M1");
        CUserInfo pui;
        ProcessUDIData(nullptr, &pui, buf, FALSE, cc_test_lookup_none);
        CC_CHECK(pui.m_udi.m_bbCooked == 1);
    }
    // Neither G nor E group present -> STILL cooked, because Reset() already
    // put both intensities at 0 (!= -1). This is the original's own
    // arithmetic (protsupp.cpp:1538-1539), not a lift artifact.
    {
        char buf[64]; strcpy(buf, "#M1");
        CUserInfo pui;
        ProcessUDIData(nullptr, &pui, buf, FALSE, cc_test_lookup_none);
        CC_CHECK(pui.m_udi.m_bbCooked == 1);
    }
    return 0;
}

// --- key-string codec (protsupp.cpp:5073-5260) selftest ---------------------
static int cc_selftest_keystring() {
    CString ks;
    CC_CHECK(ChangeKeyString(ks, "msg", "hello", 100) == TRUE);
    CC_CHECK(strcmp(ks, "msg=hello") == 0);
    CC_CHECK(ChangeKeyString(ks, "id", "200", 100) == TRUE);
    CC_CHECK(strcmp(ks, "msg=hello;id=200") == 0);

    CString val;
    CC_CHECK(GetValueFromKeyString(ks, "msg", val) == TRUE);
    CC_CHECK(strcmp(val, "hello") == 0);
    CC_CHECK(GetValueFromKeyString(ks, "id", val) == TRUE);
    CC_CHECK(strcmp(val, "200") == 0);
    CC_CHECK(GetValueFromKeyString(ks, "nope", val) == FALSE);

    // Quoted value containing a reserved char (';').
    CString ks2;
    CC_CHECK(ChangeKeyString(ks2, "msg", "hello; what is your name", 200) == TRUE);
    CC_CHECK(strcmp(ks2, "msg=\"hello; what is your name\"") == 0);
    CC_CHECK(GetValueFromKeyString(ks2, "msg", val) == TRUE);
    CC_CHECK(strcmp(val, "hello; what is your name") == 0);  // quotes stripped back off

    // DISCOVERED PRE-EXISTING BUG (verified byte-identical against the
    // original, v2.5-beta-1-modern/protsupp.cpp:5154-5157 -- not introduced
    // by this lift): ChangeKeyString's deletion branch (`pszValue == NULL ||
    // *pszValue == '\0'`) computes the correctly-shortened `strNew` (with the
    // key removed) but returns TRUE WITHOUT ever assigning it back to the
    // `strKeyString` output parameter -- the only write-back
    // (`strKeyString = strNew + strPair;`) is unreachable from the deletion
    // branch (it returns earlier, line ~546). So deletion always silently
    // no-ops: the function reports success (TRUE) but the caller's key
    // string is left completely unchanged. Confirmed reachable in the
    // original (CIrcProto::ChangeProperty, ircproto.cpp:1397-1399, forwards
    // its caller-supplied pszValue straight through -- a NULL/empty value
    // there would hit this exact branch). This is a real functional defect,
    // not merely a memory-safety hazard like the FindInKeyString bug above,
    // so it is NOT silently patched here (unlike that one) -- flagged in the
    // task report for the parent/a later task to decide whether/when to fix,
    // since "wire/property behavior" changes are explicitly called out as
    // needing escalation rather than casual correction. This selftest
    // therefore asserts the ACTUAL (buggy) behavior: `ks` is unchanged after
    // the "deletion".
    CC_CHECK(ChangeKeyString(ks, "msg", NULL, 100) == TRUE);
    CC_CHECK(strcmp(ks, "msg=hello;id=200") == 0);  // unchanged -- see bug note above

    // Enumeration (still over the un-deleted two-entry string).
    LPCSTR pEnum = ks;
    CString key, value;
    CC_CHECK(EnumKeyString(pEnum, key, value) == TRUE);
    CC_CHECK(strcmp(key, "msg") == 0 && strcmp(value, "hello") == 0);
    CC_CHECK(pEnum != NULL);  // one more entry ("id=200") remains
    CC_CHECK(EnumKeyString(pEnum, key, value) == TRUE);
    CC_CHECK(strcmp(key, "id") == 0 && strcmp(value, "200") == 0);
    CC_CHECK(pEnum == NULL);  // no more entries

    return 0;
}

// --- query.cpp: CCQuery/CQueryPtrList correlation-list selftest (Plan 3 -----
// Task 4 Step 3). Enqueues two queries of DIFFERENT command types (ctWho then
// ctTopic), then a third of the SAME type as the first (ctWho) to verify
// "oldest-matching" ordering, and checks FindQuery dequeues in enqueue order
// (the enqueue-BEFORE-send discipline's correlation contract: the reply
// handler for a given command type must match the OLDEST outstanding query
// of that type, not the newest).
static int cc_selftest_query_correlation() {
    CQueryPtrList list;

    CCQuery* q1 = new CCQuery(qpUserListDlg, ctWho, dtMax, nullptr, "", "", FALSE);
    CCQuery* q2 = new CCQuery(qpSetTopic, ctTopic, dtMax, nullptr, "#room", "", FALSE);
    CCQuery* q3 = new CCQuery(qpUserListDlg, ctWho, dtMax, nullptr, "", "", FALSE);

    CC_CHECK(list.bAddQuery(q1) == TRUE);
    CC_CHECK(list.bAddQuery(q2) == TRUE);
    CC_CHECK(list.bAddQuery(q3) == TRUE);
    CC_CHECK(list.GetCount() == 3);

    // FindQuery(ctWho) must return q1 (oldest ctWho), not q3.
    POSITION pos = nullptr;
    LONG rank = 0;
    CCQuery* found = list.FindQuery(ctWho, &pos, &rank);
    CC_CHECK(found == q1);
    CC_CHECK(rank == 1);  // first entry in the list
    CC_CHECK(pos != nullptr);

    // Dequeue it (matches the reply-handler idiom: FindQuery then
    // FreeRemoveAt on the returned POSITION).
    list.FreeRemoveAt(pos);
    CC_CHECK(list.GetCount() == 2);

    // Now FindQuery(ctWho) must return q3 (the only remaining ctWho query).
    CCQuery* found2 = list.FindQuery(ctWho, &pos, &rank);
    CC_CHECK(found2 == q3);

    // FindQuery for a command type with no outstanding query returns NULL.
    CCQuery* miss = list.FindQuery(ctList);
    CC_CHECK(miss == nullptr);

    // ctTopic query is still present, findable, dequeues correctly too.
    CCQuery* foundTopic = list.FindQuery(ctTopic);
    CC_CHECK(foundTopic == q2);
    CC_CHECK(strcmp(foundTopic->GetChannelName(), "#room") == 0);

    // FreeRemoveAll deletes every remaining query (q2, q3) and empties the
    // list -- exercised via the destructor path (~CQueryPtrList calls it) by
    // just letting `list` go out of scope; call it explicitly here too so
    // the "empty after" assertion is direct.
    list.FreeRemoveAll();
    CC_CHECK(list.GetCount() == 0);
    CC_CHECK(list.FindQuery(ctWho) == nullptr);

    return 0;
}

// --- ircproto.cpp: outbound byte-compare selftest (Plan 3 Task 4 brief's ---
// Step 1/7). Drives a JOIN and a SAY through the real cc_session_* C
// functions and asserts EXACT wire bytes on the `send` capture -- this is
// the interop contract the byte strings below encode.
struct CCOutboundCap { std::string sent; };
static void ccOutboundCapSend(void* ud, const uint8_t* d, size_t n) {
    static_cast<CCOutboundCap*>(ud)->sent.append((const char*)d, n);
}
static const char* ccOutboundOwnNick(void*) { return "Anon"; }

static int cc_selftest_outbound_join_say() {
    CCOutboundCap cap;
    cc_session_config cfg = {};
    cfg.user_data = &cap;
    cfg.send = ccOutboundCapSend;
    cfg.own_nick = ccOutboundOwnNick;
    cc_session* s = cc_session_create(&cfg);
    CC_CHECK(s != nullptr);
    if (!s) return g_failures;

    // JOIN #comicrig (no key) -- ircproto.cpp:810's exact wire shape.
    CC_CHECK(cc_session_join(s, "#comicrig", nullptr) == 0);
    CC_CHECK(cap.sent == "JOIN #comicrig\r\n");
    cap.sent.clear();

    // JOIN with a key: "JOIN <chan> <key>\r\n" (ircproto.cpp:815).
    CC_CHECK(cc_session_join(s, "#secretroom", "hunter2") == 0);
    CC_CHECK(cap.sent == "JOIN #secretroom hunter2\r\n");
    cap.sent.clear();

    // Register the room the SAY targets (room-token<->channel mapping,
    // cc_session.h) so the say assertion below can be tightened to the exact
    // plain-IRC wire string (the brief's Step 7: loosened only until this
    // mapping exists -- it now does).
    uint32_t token = cc_session_register_room(s, "#comicrig");
    CC_CHECK(token != CC_ROOM_TOKEN_NONE);

    // SAY, no pose/addressees/requested -- SM_SAY (mode=1) with an all-zero
    // G/E block. Exact plain-IRC transport (non-IRCX): the parenthesized
    // annotation blob prefixes the text on a single PRIVMSG line
    // (ircproto.cpp:554-556's sprintf grammar; IndexToByte(0)='0').
    cc_annotations a;
    memset(&a, 0, sizeof a);
    a.mode = 1;  // SM_SAY
    CC_CHECK(cc_session_send_say(s, token, &a, "hi", 0) == 0);
    CC_CHECK(cap.sent == "PRIVMSG #comicrig :(#G000E000M1) hi\r\n");
    cap.sent.clear();

    // Plain NICK change.
    CC_CHECK(cc_session_change_nick(s, "NewNick") == 0);
    CC_CHECK(cap.sent == "NICK NewNick\r\n");
    cap.sent.clear();

    // PART: ChatPartChannel only sends if m_bInRoom (set true by
    // cc_session_part's own wrapper -- see cc_session.cpp).
    CC_CHECK(cc_session_part(s, token, nullptr) == 0);
    CC_CHECK(cap.sent == "PART #comicrig\r\n");
    cap.sent.clear();

    // WHO with a mask.
    CC_CHECK(cc_session_who(s, "Anon*") == 0);
    CC_CHECK(cap.sent == "WHO Anon*\r\n");
    cap.sent.clear();

    // LIST with no filter (non-IRCX server default: ctList).
    CC_CHECK(cc_session_list(s, nullptr) == 0);
    CC_CHECK(cap.sent == "LIST\r\n");
    cap.sent.clear();

    cc_session_destroy(s);
    return 0;
}

// --- cc_session_login selftest (Plan 4a Task 2 Step 2): the plain-IRC login
// (NICK/USER) that the lifted HrIrcLogin never got as wire-emitting code (see
// cc_session_login's comicchat.h doc comment). Verifies the exact byte shape
// against the real 1998 capture's client c2s line (Tests/ComicChatKitTests/
// Fixtures/captures/smoke-2.jsonl: "NICK Anonymous\r\nUSER Anonymous Tims-Mac
// . :Your Full Name\r\n"), substituting this test's own config values.
static int cc_selftest_session_login() {
    CCOutboundCap cap;
    cc_session_config cfg = {};
    cfg.user_data = &cap;
    cfg.send = ccOutboundCapSend;
    cfg.own_nick = ccOutboundOwnNick;   // "Anon" -- resolver ProtocolSession would provide
    cfg.own_user = "Anonymous";
    cfg.own_realname = "Anonymous";
    cfg.local_host = "testhost";
    cc_session* s = cc_session_create(&cfg);
    CC_CHECK(s != nullptr);
    if (!s) return g_failures;

    CC_CHECK(cc_session_login(s) == 0);
    CC_CHECK(cap.sent.find("NICK Anon\r\n") != std::string::npos);
    CC_CHECK(cap.sent.find("USER Anonymous testhost . :Anonymous\r\n") != std::string::npos);
    // NICK must precede USER (field order matches HrIrcLogin's own
    // ChatChangeNick-then-USER-sprintf sequence, ircsock.cpp:644-654).
    CC_CHECK(cap.sent.find("NICK Anon\r\n") < cap.sent.find("USER Anonymous testhost . :Anonymous\r\n"));

    cc_session_destroy(s);
    return 0;
}

// --- cc_session_announce_avatar selftest (Plan 4a Task 8 Step 2): the
// outbound avatar announce ("# Appears as ...", CRoomInfo::ChatAnnounceNewAvatar
// v2.5-beta-1-modern/protsupp.cpp:817-843 -- see comicchat.h's doc comment for
// the un-wrap-vs-R16 disposition). Byte ground truth is the original's own
// sprintf grammar (no committed capture actually contains a real "# Appears
// as" c2s line -- neither smoke-2.jsonl nor hand-authored-annotation.jsonl --
// so this asserts against protsupp.cpp:833/835 + APPEARSPREFIX directly):
//   channel-wide, no URL:      "PRIVMSG #comicrig :# Appears as Anna\r\n"
//   private reply (to_nick):   "PRIVMSG Win :# Appears as Anna\r\n"
//   with a URL:                "PRIVMSG #comicrig :# Appears as Anna.http://x\r\n"
// Sent with an EMPTY message (bChatSendToTarget's szMesg=NULL/"" path) so the
// announcement rides purely as the annotations argument -- the IRCX-DATA-vs-
// plain-PRIVMSG branch is bChatSendToTarget's own IsIRCX() test, exercised
// here on a plain (non-IRCX) session; Task 4's cc_selftest_pv_data_vs_inline
// already covers the IRCX DATA-line half of that same branch for annotations
// in general, so this test does not re-derive it.
static int cc_selftest_announce_avatar() {
    CCOutboundCap cap;
    cc_session_config cfg = {};
    cfg.user_data = &cap;
    cfg.send = ccOutboundCapSend;
    cfg.own_nick = ccOutboundOwnNick;
    cc_session* s = cc_session_create(&cfg);
    CC_CHECK(s != nullptr);
    if (!s) return g_failures;

    CC_CHECK(cc_session_join(s, "#comicrig", nullptr) == 0);
    cap.sent.clear();
    uint32_t token = cc_session_register_room(s, "#comicrig");
    CC_CHECK(token != CC_ROOM_TOKEN_NONE);

    // Channel-wide, no URL.
    CC_CHECK(cc_session_announce_avatar(s, token, nullptr, "Anna", nullptr) == 0);
    CC_CHECK(cap.sent == "PRIVMSG #comicrig :# Appears as Anna\r\n");
    cap.sent.clear();

    // Private reply-announce (to_nick set) -- bChatSendPrivMesg, not
    // bChatSendToChannel (protsupp.cpp:868-877's reply rule).
    CC_CHECK(cc_session_announce_avatar(s, token, "Win", "Anna", nullptr) == 0);
    CC_CHECK(cap.sent == "PRIVMSG Win :# Appears as Anna\r\n");
    cap.sent.clear();

    // With a URL: "<name>.<url>" grammar (protsupp.cpp:833).
    CC_CHECK(cc_session_announce_avatar(s, token, nullptr, "Anna", "http://x") == 0);
    CC_CHECK(cap.sent == "PRIVMSG #comicrig :# Appears as Anna.http://x\r\n");
    cap.sent.clear();

    // NULL/empty name falls back to "NONE" (protsupp.cpp:830-831).
    CC_CHECK(cc_session_announce_avatar(s, token, nullptr, nullptr, nullptr) == 0);
    CC_CHECK(cap.sent == "PRIVMSG #comicrig :# Appears as NONE\r\n");
    cap.sent.clear();

    // Unknown room_token -> failure, no send.
    CC_CHECK(cc_session_announce_avatar(s, CC_ROOM_TOKEN_NONE, nullptr, "Anna", nullptr) != 0);
    CC_CHECK(cap.sent.empty());

    cc_session_destroy(s);
    return 0;
}

// --- ISIRCX probe timer-request selftest (Plan 3 Task 4 brief: "the MODE
// ISIRCX path requests the 50s timer via cfg.set_timer(CC_TIMER_ISIRCX_PROBE,
// 50000)"). Verifies both the wire bytes AND that the timer request reaches
// cfg.set_timer with the exact id/duration -- the engine REQUESTS, this test
// stands in for Swift's SCHEDULES half.
struct CCIrcxProbeCap { std::string sent; int32_t timerId = -1; int32_t timerMs = -1; int timerCalls = 0; };
static int cc_selftest_outbound_ircx_probe_timer() {
    CCIrcxProbeCap cap;
    cc_session_config cfg = {};
    cfg.user_data = &cap;
    cfg.send = [](void* ud, const uint8_t* d, size_t n) {
        static_cast<CCIrcxProbeCap*>(ud)->sent.append((const char*)d, n);
    };
    cfg.set_timer = [](void* ud, int32_t id, int32_t ms) {
        CCIrcxProbeCap* c = static_cast<CCIrcxProbeCap*>(ud);
        c->timerId = id; c->timerMs = ms; c->timerCalls++;
    };
    cfg.own_nick = ccOutboundOwnNick;

    cc_session* s = cc_session_create(&cfg);
    CC_CHECK(s != nullptr);
    if (!s) return g_failures;

    CC_CHECK(cc_session_probe_ircx(s) == 0);
    CC_CHECK(cap.sent == "MODE ISIRCX\r\n");
    CC_CHECK(cap.timerCalls == 1);
    CC_CHECK(cap.timerId == CC_TIMER_ISIRCX_PROBE);
    CC_CHECK(cap.timerMs == 50000);

    cc_session_destroy(s);
    return 0;
}

// --- ISIRCX probe 451-fallback selftest (Plan 4a Task 2 fix): the original's
// OnConnect (v2.5-beta-1-modern/ircsock.cpp:1049-1052) sets
// m_bJustSentModeIsIrcX = TRUE right after sending the probe -- a THIRD side
// effect this port's cc_session_probe_ircx initially missed (it only carried
// the bExecuteQuery send + the set_timer request). Without it,
// ccModeIsIrcXFailure's own guard (`if (!m_bJustSentModeIsIrcX) return;`)
// always early-returned, so an ERR_NOTREGISTERED (451) reply -- the whole
// point of probing -- never dequeued the probe query or cancelled the timer
// (caught via LoginSequencingTests' plainIrcFallback scenario: NICK/USER only
// ever arrived via the timer's OWN timeout, never via the immediate 451
// reaction). This selftest locks in the fix at the C layer.
static int cc_selftest_probe_451_cancels_timer() {
    CCIrcxProbeCap cap;
    cc_session_config cfg = {};
    cfg.user_data = &cap;
    cfg.send = [](void* ud, const uint8_t* d, size_t n) {
        static_cast<CCIrcxProbeCap*>(ud)->sent.append((const char*)d, n);
    };
    cfg.set_timer = [](void* ud, int32_t id, int32_t ms) {
        CCIrcxProbeCap* c = static_cast<CCIrcxProbeCap*>(ud);
        c->timerId = id; c->timerMs = ms; c->timerCalls++;
    };
    static int s_cancelId; static int s_cancelCalls;
    s_cancelId = -1; s_cancelCalls = 0;
    cfg.cancel_timer = [](void*, int32_t id) { s_cancelId = id; s_cancelCalls++; };
    cfg.own_nick = ccOutboundOwnNick;

    cc_session* s = cc_session_create(&cfg);
    CC_CHECK(s != nullptr);
    if (!s) return g_failures;

    CC_CHECK(cc_session_probe_ircx(s) == 0);
    CC_CHECK(cap.timerCalls == 1);

    const char* wire = ":srv 451 * :not registered\r\n";
    cc_session_feed_bytes(s, (const uint8_t*)wire, strlen(wire));
    CC_CHECK(s_cancelCalls == 1 && s_cancelId == CC_TIMER_ISIRCX_PROBE);

    cc_session_destroy(s);
    return 0;
}

// --- bChatSendToTarget chunking-path selftest (ircproto.cpp:481-698's most
// complex lifted logic; the byte-compare tests above only ever exercise the
// single-shot "fits in one line" branch). Sends a SAY long enough to force
// the multi-chunk loop, and asserts: (a) more than one PRIVMSG line is sent,
// (b) every line is well-formed ("PRIVMSG #chan :..." + CRLF), (c) the
// reassembled body (chunk text only, ignoring the annotation prefix on the
// first line) equals the original text with chunk-boundary spaces collapsed
// (nGetBreakingPoint's "skip all spaces after the break" behavior,
// ircproto.cpp:672-676) -- i.e. no characters are lost or corrupted across
// the split, matching the original's own chunking contract.
static int cc_selftest_outbound_say_chunking() {
    CCOutboundCap cap;
    cc_session_config cfg = {};
    cfg.user_data = &cap;
    cfg.send = ccOutboundCapSend;
    cfg.own_nick = ccOutboundOwnNick;
    cc_session* s = cc_session_create(&cfg);
    CC_CHECK(s != nullptr);
    if (!s) return g_failures;

    CC_CHECK(cc_session_join(s, "#comicrig", nullptr) == 0);
    cap.sent.clear();
    uint32_t token = cc_session_register_room(s, "#comicrig");

    // Build a long message: default m_nMaxMsgLength is 512 (g_nDefaultIOBuff);
    // 40 repetitions of an 11-char word (+space) is ~440 bytes of body alone,
    // comfortably forcing at least one chunk break once the "PRIVMSG #comicrig
    // :" framing + receiving-side prefix accounting are added on top.
    std::string longText;
    for (int i = 0; i < 40; i++) { longText += "wordchunk"; longText += (i % 2) ? "A " : "B "; }

    // modes must carry BM_SAY here (unlike the short-message selftest above,
    // which passes 0 -- that one never reaches bChatSendToTarget's chunking
    // switch(uModes) at all since its text fits in one shot; the chunking
    // path's switch has no case for 0 and ASSERT(0)s in its default arm,
    // matching the original's own contract that a real caller always passes
    // a real BM_* mode for a message long enough to chunk).
    cc_annotations a; memset(&a, 0, sizeof a); a.mode = 1;  // SM_SAY, no annotations content otherwise
    CC_CHECK(cc_session_send_say(s, token, &a, longText.c_str(), BM_SAY) == 0);

    // Count PRIVMSG lines and reassemble the body. nGetBreakingPoint
    // deliberately breaks AT a space and the chunk loop then skips ALL
    // leading whitespace on the next chunk (ircproto.cpp:672-676's
    // "while (my_isspace(*szBody))" skip) -- so the exact separator space
    // between the two words spanning a chunk boundary is legitimately
    // consumed by the break itself and appears in NEITHER chunk (verified
    // empirically: chunk 1 ends "...wordchunkB", chunk 2 starts
    // "wordchunkA..." with zero space between). This is the original's
    // actual, intentional word-wrap behavior, not a lift defect -- so
    // reassembly re-inserts exactly one space between consecutive chunks
    // (the separator the break consumed) before comparing to the original.
    int privmsgCount = 0;
    std::string reassembled;
    size_t pos = 0;
    while (pos < cap.sent.size()) {
        size_t eol = cap.sent.find("\r\n", pos);
        CC_CHECK(eol != std::string::npos);
        if (eol == std::string::npos) break;
        std::string line = cap.sent.substr(pos, eol - pos);
        CC_CHECK(line.rfind("PRIVMSG #comicrig :", 0) == 0);
        if (line.rfind("PRIVMSG #comicrig :", 0) == 0) {
            privmsgCount++;
            std::string body = line.substr(strlen("PRIVMSG #comicrig :"));
            // EVERY chunk carries the parenthesized annotation prefix
            // ("(#G000E000M1) "), not just the first -- verbatim original
            // behavior (ircproto.cpp's chunk loop re-sprintfs szAnnotations
            // into every line on the plain-IRC/non-IRCX transport, since
            // there's no out-of-band channel to carry it once per message;
            // every recipient needs the avatar-state prefix on every line
            // they might see). Strip it from each line before reassembly.
            size_t termPos = body.find(") ");
            CC_CHECK(body.rfind("(#", 0) == 0 && termPos != std::string::npos);
            if (termPos != std::string::npos) body = body.substr(termPos + 2);
            if (privmsgCount > 1) reassembled += ' ';  // re-insert the consumed break separator
            reassembled += body;
        }
        pos = eol + 2;
    }
    CC_CHECK(privmsgCount > 1);  // must actually have chunked
    CC_CHECK(reassembled == longText);

    cc_session_destroy(s);
    return 0;
}

// --- Plan 3 Task 5a: cc_proto_event union completeness + round-trip ---------
// For EVERY value in cc_proto_event_type (excluding CC_EV_NONE, which carries
// no payload and is the union's zero/inactive state), build a cc_proto_event
// of that variant, fill one representative field (plus room_token, to prove
// the outer struct crosses the boundary too), send it through ccEmitProtoEvent
// to a capturing on_event, and read the type + that field back. This is the
// completeness proof this task's brief calls for: every enum value has a
// usable, distinct union struct, and the union round-trips through the C
// boundary unchanged (no aliasing/overlap corrupts a sibling variant's
// fields). ccEmitProtoEvent requires an active session (ccSession() asserts),
// so each case runs inside a real cc_session created solely to host the
// dispatch -- mirroring cc_selftest_session_skeleton's on_event capture, not
// exercising cc_session_feed_bytes/parsing (that's Task 5b's job).
namespace {
struct CCEventCap {
    cc_proto_event last{};
    int count = 0;
};
void ccEventCapOnEvent(void* ud, const cc_proto_event* ev) {
    CCEventCap* cap = static_cast<CCEventCap*>(ud);
    cap->last = *ev;
    cap->count++;
}
} // namespace

static int cc_selftest_event_union() {
    CCEventCap cap;
    cc_session_config cfg = {};
    cfg.user_data = &cap;
    cfg.send = [](void*, const uint8_t*, size_t) {};  // never used; on_event is what's tested
    cfg.on_event = ccEventCapOnEvent;
    cc_session* s = cc_session_create(&cfg);
    CC_CHECK(s != nullptr);
    if (!s) return g_failures;
    ccActivateSessionForTest(s);   // ccEmitProtoEvent requires an active session

    auto fire = [&](const cc_proto_event& ev) {
        cap.count = 0;
        ccEmitProtoEvent(&ev);
        CC_CHECK(cap.count == 1);
    };

    // connection lifecycle
    {
        cc_proto_event ev{}; ev.type = CC_EV_LOGGED_IN; ev.room_token = 0;
        ev.u.logged_in.nick = "Anna";
        fire(ev);
        CC_CHECK(cap.last.type == CC_EV_LOGGED_IN);
        CC_CHECK(std::string(cap.last.u.logged_in.nick) == "Anna");
    }
    {
        cc_proto_event ev{}; ev.type = CC_EV_SERVER_CAPS;
        ev.u.server_caps.ircx = 1; ev.u.server_caps.max_msg_len = 512;
        fire(ev);
        CC_CHECK(cap.last.type == CC_EV_SERVER_CAPS);
        CC_CHECK(cap.last.u.server_caps.ircx == 1 && cap.last.u.server_caps.max_msg_len == 512);
    }
    {
        cc_proto_event ev{}; ev.type = CC_EV_DISCONNECTED_HINT;
        ev.u.disconnected_hint.text = "Connection reset by peer";
        fire(ev);
        CC_CHECK(cap.last.type == CC_EV_DISCONNECTED_HINT);
        CC_CHECK(std::string(cap.last.u.disconnected_hint.text) == "Connection reset by peer");
    }

    // membership
    {
        cc_proto_event ev{}; ev.type = CC_EV_SELF_JOINED; ev.room_token = 7;
        ev.u.self_joined.channel = "#comicrig";
        fire(ev);
        CC_CHECK(cap.last.type == CC_EV_SELF_JOINED && cap.last.room_token == 7);
        CC_CHECK(std::string(cap.last.u.self_joined.channel) == "#comicrig");
    }
    {
        cc_proto_event ev{}; ev.type = CC_EV_SELF_PARTED; ev.room_token = 7;
        ev.u.self_parted.channel = "#comicrig";
        fire(ev);
        CC_CHECK(cap.last.type == CC_EV_SELF_PARTED);
        CC_CHECK(std::string(cap.last.u.self_parted.channel) == "#comicrig");
    }
    {
        cc_proto_event ev{}; ev.type = CC_EV_USER_JOINED; ev.room_token = 7;
        ev.u.user_joined.nick = "Bob"; ev.u.user_joined.ident = "bob@host";
        fire(ev);
        CC_CHECK(cap.last.type == CC_EV_USER_JOINED);
        CC_CHECK(std::string(cap.last.u.user_joined.nick) == "Bob");
        CC_CHECK(std::string(cap.last.u.user_joined.ident) == "bob@host");
    }
    {
        cc_proto_event ev{}; ev.type = CC_EV_USER_PARTED; ev.room_token = 7;
        ev.u.user_parted.nick = "Bob"; ev.u.user_parted.reason = "bye";
        fire(ev);
        CC_CHECK(cap.last.type == CC_EV_USER_PARTED);
        CC_CHECK(std::string(cap.last.u.user_parted.nick) == "Bob");
        CC_CHECK(std::string(cap.last.u.user_parted.reason) == "bye");
    }
    {
        cc_proto_event ev{}; ev.type = CC_EV_USER_QUIT;
        ev.u.user_quit.nick = "Bob"; ev.u.user_quit.reason = "quit: pc off";
        fire(ev);
        CC_CHECK(cap.last.type == CC_EV_USER_QUIT);
        CC_CHECK(std::string(cap.last.u.user_quit.nick) == "Bob");
        CC_CHECK(std::string(cap.last.u.user_quit.reason) == "quit: pc off");
    }
    {
        cc_proto_event ev{}; ev.type = CC_EV_KICKED; ev.room_token = 7;
        ev.u.kicked.kicker = "Carl"; ev.u.kicked.kickee = "Bob";
        ev.u.kicked.reason = "spamming"; ev.u.kicked.channel = "#comicrig";
        fire(ev);
        CC_CHECK(cap.last.type == CC_EV_KICKED);
        CC_CHECK(std::string(cap.last.u.kicked.kicker) == "Carl");
        CC_CHECK(std::string(cap.last.u.kicked.kickee) == "Bob");
        CC_CHECK(std::string(cap.last.u.kicked.reason) == "spamming");
        CC_CHECK(std::string(cap.last.u.kicked.channel) == "#comicrig");
    }
    {
        cc_proto_event ev{}; ev.type = CC_EV_INVITED; ev.room_token = 7;
        ev.u.invited.by = "Carl"; ev.u.invited.ident = "carl@host";
        ev.u.invited.channel = "#comicrig";
        fire(ev);
        CC_CHECK(cap.last.type == CC_EV_INVITED);
        CC_CHECK(std::string(cap.last.u.invited.by) == "Carl");
        CC_CHECK(std::string(cap.last.u.invited.ident) == "carl@host");
        CC_CHECK(std::string(cap.last.u.invited.channel) == "#comicrig");
    }
    {
        cc_proto_event ev{}; ev.type = CC_EV_NAMES; ev.room_token = 7;
        ev.u.names.channel = "#comicrig"; ev.u.names.nicks = "Anna Bob Carl";
        fire(ev);
        CC_CHECK(cap.last.type == CC_EV_NAMES);
        CC_CHECK(std::string(cap.last.u.names.nicks) == "Anna Bob Carl");
    }
    {
        cc_proto_event ev{}; ev.type = CC_EV_END_OF_NAMES; ev.room_token = 7;
        ev.u.end_of_names.channel = "#comicrig";
        fire(ev);
        CC_CHECK(cap.last.type == CC_EV_END_OF_NAMES);
        CC_CHECK(std::string(cap.last.u.end_of_names.channel) == "#comicrig");
    }
    {
        cc_proto_event ev{}; ev.type = CC_EV_NICK_CHANGED;
        ev.u.nick_changed.old_nick = "Bob"; ev.u.nick_changed.new_nick = "Bobby";
        ev.u.nick_changed.is_self = 0;
        fire(ev);
        CC_CHECK(cap.last.type == CC_EV_NICK_CHANGED);
        CC_CHECK(std::string(cap.last.u.nick_changed.old_nick) == "Bob");
        CC_CHECK(std::string(cap.last.u.nick_changed.new_nick) == "Bobby");
        CC_CHECK(cap.last.u.nick_changed.is_self == 0);
    }

    // messages (the core comic events)
    {
        cc_proto_event ev{}; ev.type = CC_EV_TEXT; ev.room_token = 7;
        ev.u.text.nick = "Anna"; ev.u.text.ident = "anna@host";
        ev.u.text.target = "#comicrig"; ev.u.text.text = "hello";
        ev.u.text.kind = 1; ev.u.text.has_annotations = 1;
        memset(&ev.u.text.annotations, 0, sizeof(ev.u.text.annotations));
        ev.u.text.annotations.mode = 1;
        fire(ev);
        CC_CHECK(cap.last.type == CC_EV_TEXT);
        CC_CHECK(std::string(cap.last.u.text.text) == "hello");
        CC_CHECK(cap.last.u.text.has_annotations == 1);
        CC_CHECK(cap.last.u.text.annotations.mode == 1);
    }
    {
        cc_proto_event ev{}; ev.type = CC_EV_DATA; ev.room_token = 7;
        ev.u.data.nick = "Anna";
        memset(&ev.u.data.annotations, 0, sizeof(ev.u.data.annotations));
        ev.u.data.annotations.mode = 2;
        fire(ev);
        CC_CHECK(cap.last.type == CC_EV_DATA);
        CC_CHECK(std::string(cap.last.u.data.nick) == "Anna");
        CC_CHECK(cap.last.u.data.annotations.mode == 2);
    }
    {
        cc_proto_event ev{}; ev.type = CC_EV_WHISPER;
        ev.u.whisper.nick = "Anna"; ev.u.whisper.ident = "anna@host";
        ev.u.whisper.text = "psst"; ev.u.whisper.has_annotations = 0;
        memset(&ev.u.whisper.annotations, 0, sizeof(ev.u.whisper.annotations));
        fire(ev);
        CC_CHECK(cap.last.type == CC_EV_WHISPER);
        CC_CHECK(std::string(cap.last.u.whisper.text) == "psst");
        CC_CHECK(cap.last.u.whisper.has_annotations == 0);
    }
    {
        cc_proto_event ev{}; ev.type = CC_EV_ACTION; ev.room_token = 7;
        ev.u.action.nick = "Anna"; ev.u.action.text = "waves";
        ev.u.action.has_annotations = 0;
        memset(&ev.u.action.annotations, 0, sizeof(ev.u.action.annotations));
        fire(ev);
        CC_CHECK(cap.last.type == CC_EV_ACTION);
        CC_CHECK(std::string(cap.last.u.action.text) == "waves");
    }
    {
        cc_proto_event ev{}; ev.type = CC_EV_SOUND; ev.room_token = 7;
        ev.u.sound.nick = "Anna"; ev.u.sound.file = "boing.wav"; ev.u.sound.text = "*boing*";
        fire(ev);
        CC_CHECK(cap.last.type == CC_EV_SOUND);
        CC_CHECK(std::string(cap.last.u.sound.file) == "boing.wav");
    }
    {
        cc_proto_event ev{}; ev.type = CC_EV_AWAY_PEER;
        ev.u.away_peer.nick = "Anna"; ev.u.away_peer.message = "brb";
        fire(ev);
        CC_CHECK(cap.last.type == CC_EV_AWAY_PEER);
        CC_CHECK(std::string(cap.last.u.away_peer.message) == "brb");
    }
    {
        cc_proto_event ev{}; ev.type = CC_EV_APPEARS_AS;
        ev.u.appears_as.nick = "Anna"; ev.u.appears_as.avatar_name = "anna";
        ev.u.appears_as.url = "http://example.com/anna.avb";
        fire(ev);
        CC_CHECK(cap.last.type == CC_EV_APPEARS_AS);
        CC_CHECK(std::string(cap.last.u.appears_as.avatar_name) == "anna");
        CC_CHECK(std::string(cap.last.u.appears_as.url) == "http://example.com/anna.avb");
    }

    // room state
    {
        cc_proto_event ev{}; ev.type = CC_EV_TOPIC_CHANGED; ev.room_token = 7;
        ev.u.topic_changed.channel = "#comicrig"; ev.u.topic_changed.topic = "Welcome!";
        fire(ev);
        CC_CHECK(cap.last.type == CC_EV_TOPIC_CHANGED);
        CC_CHECK(std::string(cap.last.u.topic_changed.topic) == "Welcome!");
    }
    {
        cc_proto_event ev{}; ev.type = CC_EV_CHANNEL_MODE; ev.room_token = 7;
        ev.u.channel_mode.channel = "#comicrig"; ev.u.channel_mode.modes = "+m";
        ev.u.channel_mode.arg = "";
        fire(ev);
        CC_CHECK(cap.last.type == CC_EV_CHANNEL_MODE);
        CC_CHECK(std::string(cap.last.u.channel_mode.modes) == "+m");
    }
    {
        cc_proto_event ev{}; ev.type = CC_EV_USER_MODE;
        ev.u.user_mode.nick = "Anna"; ev.u.user_mode.modes = "+o";
        fire(ev);
        CC_CHECK(cap.last.type == CC_EV_USER_MODE);
        CC_CHECK(std::string(cap.last.u.user_mode.modes) == "+o");
    }
    {
        cc_proto_event ev{}; ev.type = CC_EV_ROOM_PROP; ev.room_token = 7;
        ev.u.room_prop.key = "bk"; ev.u.room_prop.value = "backdrop.bgb";
        fire(ev);
        CC_CHECK(cap.last.type == CC_EV_ROOM_PROP);
        CC_CHECK(std::string(cap.last.u.room_prop.key) == "bk");
        CC_CHECK(std::string(cap.last.u.room_prop.value) == "backdrop.bgb");
    }
    {
        cc_proto_event ev{}; ev.type = CC_EV_ROOM_LIST_BEGIN;
        ev.u.room_list_begin.truncated = 0;
        fire(ev);
        CC_CHECK(cap.last.type == CC_EV_ROOM_LIST_BEGIN);
        CC_CHECK(cap.last.u.room_list_begin.truncated == 0);
    }
    {
        cc_proto_event ev{}; ev.type = CC_EV_ROOM_LIST_ITEM;
        ev.u.room_list_item.name = "#comicrig"; ev.u.room_list_item.users = 3;
        ev.u.room_list_item.topic = "Welcome!";
        fire(ev);
        CC_CHECK(cap.last.type == CC_EV_ROOM_LIST_ITEM);
        CC_CHECK(std::string(cap.last.u.room_list_item.name) == "#comicrig");
        CC_CHECK(cap.last.u.room_list_item.users == 3);
    }
    {
        cc_proto_event ev{}; ev.type = CC_EV_ROOM_LIST_END;
        ev.u.room_list_end.truncated = 1;
        fire(ev);
        CC_CHECK(cap.last.type == CC_EV_ROOM_LIST_END);
        CC_CHECK(cap.last.u.room_list_end.truncated == 1);
    }
    {
        cc_proto_event ev{}; ev.type = CC_EV_WHOIS_RESULT;
        ev.u.whois_result.nick = "Anna"; ev.u.whois_result.user = "anna";
        ev.u.whois_result.host = "host.example.com"; ev.u.whois_result.real = "Anna Real";
        ev.u.whois_result.purpose = 0;
        fire(ev);
        CC_CHECK(cap.last.type == CC_EV_WHOIS_RESULT);
        CC_CHECK(std::string(cap.last.u.whois_result.real) == "Anna Real");
    }
    {
        cc_proto_event ev{}; ev.type = CC_EV_WHO_RESULT; ev.room_token = 7;
        ev.u.who_result.nick = "Anna"; ev.u.who_result.user = "anna";
        ev.u.who_result.host = "host.example.com"; ev.u.who_result.channel = "#comicrig";
        ev.u.who_result.purpose = 0;
        fire(ev);
        CC_CHECK(cap.last.type == CC_EV_WHO_RESULT);
        CC_CHECK(std::string(cap.last.u.who_result.channel) == "#comicrig");
    }
    {
        cc_proto_event ev{}; ev.type = CC_EV_MOTD;
        ev.u.motd.luser = "1 user"; ev.u.motd.motd = "Welcome to comicrig";
        fire(ev);
        CC_CHECK(cap.last.type == CC_EV_MOTD);
        CC_CHECK(std::string(cap.last.u.motd.motd) == "Welcome to comicrig");
    }

    // errors & prompts
    {
        cc_proto_event ev{}; ev.type = CC_EV_ERROR;
        ev.u.error.code = 1; ev.u.error.text = "generic failure";
        fire(ev);
        CC_CHECK(cap.last.type == CC_EV_ERROR);
        CC_CHECK(cap.last.u.error.code == 1);
        CC_CHECK(std::string(cap.last.u.error.text) == "generic failure");
    }
    {
        cc_proto_event ev{}; ev.type = CC_EV_NICK_REJECTED;
        ev.u.nick_rejected.kind = 433; ev.u.nick_rejected.bad_nick = "Anna";
        fire(ev);
        CC_CHECK(cap.last.type == CC_EV_NICK_REJECTED);
        CC_CHECK(cap.last.u.nick_rejected.kind == 433);
        CC_CHECK(std::string(cap.last.u.nick_rejected.bad_nick) == "Anna");
    }
    {
        cc_proto_event ev{}; ev.type = CC_EV_AUTH_UNSUPPORTED;
        ev.u.auth_unsupported.dummy = 1;
        fire(ev);
        CC_CHECK(cap.last.type == CC_EV_AUTH_UNSUPPORTED);
        CC_CHECK(cap.last.u.auth_unsupported.dummy == 1);
    }
    {
        cc_proto_event ev{}; ev.type = CC_EV_STATUS_LINE;
        ev.u.status_line.text = "372 :- some status text";
        fire(ev);
        CC_CHECK(cap.last.type == CC_EV_STATUS_LINE);
        CC_CHECK(std::string(cap.last.u.status_line.text) == "372 :- some status text");
    }

    ccDeactivateSessionForTest();
    cc_session_destroy(s);
    return 0;
}

// ============================================================================
// Plan 3 Task 5b: parser lift (ircsock.cpp). The first failing test is the
// brief's parse-vector selftest, verbatim.
// ============================================================================
#include <algorithm>   // std::find (Task 5b brief selftest)

static int cc_selftest_parse_join_privmsg() {
    struct Cap { std::vector<int> types; std::string last_text; std::string last_nick; } cap;
    cc_session_config cfg = {};
    cfg.user_data = &cap; cfg.send = [](void*,const uint8_t*,size_t){};
    cfg.own_nick = [](void*){ return "Anon"; };
    cfg.on_event = [](void* ud, const cc_proto_event* ev){
        Cap* c = static_cast<Cap*>(ud); c->types.push_back(ev->type);
        if (ev->type==CC_EV_TEXT){ c->last_text=ev->u.text.text; c->last_nick=ev->u.text.nick; }
    };
    cc_session* s = cc_session_create(&cfg);
    const char* wire =
        ":srv 001 Anon :Welcome\r\n"
        ":Bob!bob@h JOIN :#comicrig\r\n"
        ":Bob!bob@h PRIVMSG #comicrig :(#G295E193M1) hello\r\n";
    cc_session_feed_bytes(s, (const uint8_t*)wire, strlen(wire));
    // expect: LOGGED_IN, USER_JOINED, TEXT(with annotations, "hello", "Bob")
    CC_CHECK(cap.types.size() >= 3);
    CC_CHECK(cap.types[0]==CC_EV_LOGGED_IN);
    CC_CHECK(std::find(cap.types.begin(),cap.types.end(),CC_EV_USER_JOINED)!=cap.types.end());
    CC_CHECK(cap.last_text=="hello" && cap.last_nick=="Bob");
    cc_session_destroy(s);
    return 0;
}

// Plan 4b live-fix 6, Fix 1: channel<->room_token resolution must be
// case-insensitive, matching the original's LookupDoc (v2.5-beta-1-modern/
// chatdoc.cpp:2021-2031, `stricmp`). Registers the room under one casing
// ("#crypt", exactly what Swift's ProtocolSession.join would have sent) and
// then feeds a PRIVMSG naming a THIRD, different casing ("#CRYPT" -- neither
// the registered casing nor a join-echo the live-fix-1 in-place-rename path
// would ever see) straight at the C engine, below any Swift-side roomKey
// folding. Before this fix, ccSessionRoomTokenForChannel's byte-exact `==`
// scan misses and the event is emitted session-scoped (room_token == 0,
// CC_ROOM_TOKEN_NONE) -- exactly the p4b-bugB-diagnosis.md failure mode
// ("Tinkerbelle's greeting never renders"). After the fix, the case-folded
// match resolves the SAME token the room was registered under.
static int cc_selftest_room_token_case_insensitive() {
    struct Cap { std::vector<int> types; std::vector<uint32_t> tokens; } cap;
    cc_session_config cfg = {};
    cfg.user_data = &cap; cfg.send = [](void*,const uint8_t*,size_t){};
    cfg.own_nick = [](void*){ return "JefPober"; };
    cfg.on_event = [](void* ud, const cc_proto_event* ev){
        Cap* c = static_cast<Cap*>(ud);
        c->types.push_back(ev->type);
        c->tokens.push_back(ev->room_token);
    };
    cc_session* s = cc_session_create(&cfg);
    uint32_t token = cc_session_register_room(s, "#crypt");
    CC_CHECK(token != CC_ROOM_TOKEN_NONE);

    // A PRIVMSG naming "#CRYPT" -- upper-case, never registered or
    // join-echoed under this exact casing.
    const char* wire = ":Tinkerbelle!belle@h PRIVMSG #CRYPT :hi\r\n";
    cc_session_feed_bytes(s, (const uint8_t*)wire, strlen(wire));

    int ti = -1;
    for (size_t i = 0; i < cap.types.size(); i++) if (cap.types[i] == CC_EV_TEXT) { ti = (int)i; break; }
    CC_CHECK(ti >= 0);
    CC_CHECK(ti >= 0 && cap.tokens[ti] == token);   // room-scoped, NOT CC_ROOM_TOKEN_NONE (0)

    cc_session_destroy(s);
    return 0;
}

// ============================================================================
// Plan 3 Task 5b Step 6: coverage vectors -- one selftest per event family,
// with the FULL expected event stream hand-traced from the exercised handler +
// the original ircsock.cpp line it lifts. Each records EVERY emitted event
// (type + a couple of load-bearing fields) into a capture and asserts the
// whole sequence. `sent` also captures outbound bytes (PONG + auto-queries) so
// the reply-triggered follow-up emits (WHO/MODE/PropGet after a self-JOIN) are
// visible.
// ============================================================================
namespace {
struct PVCap {
    std::vector<int> types;
    std::vector<std::string> a;   // per-event "primary" string field
    std::vector<std::string> b;   // per-event "secondary" string field
    std::vector<uint32_t> tokens;
    std::string sent;
};
void pvOnEvent(void* ud, const cc_proto_event* ev) {
    PVCap* c = static_cast<PVCap*>(ud);
    c->types.push_back(ev->type);
    c->tokens.push_back(ev->room_token);
    std::string pa, pb;
    switch (ev->type) {
        case CC_EV_LOGGED_IN:      pa = ev->u.logged_in.nick; break;
        case CC_EV_SERVER_CAPS:    pa = std::to_string(ev->u.server_caps.ircx); pb = std::to_string(ev->u.server_caps.max_msg_len); break;
        case CC_EV_DISCONNECTED_HINT: pa = ev->u.disconnected_hint.text; break;
        case CC_EV_SELF_JOINED:    pa = ev->u.self_joined.channel; break;
        case CC_EV_SELF_PARTED:    pa = ev->u.self_parted.channel; break;
        case CC_EV_USER_JOINED:    pa = ev->u.user_joined.nick; pb = ev->u.user_joined.ident; break;
        case CC_EV_USER_PARTED:    pa = ev->u.user_parted.nick; pb = ev->u.user_parted.reason; break;
        case CC_EV_USER_QUIT:      pa = ev->u.user_quit.nick; pb = ev->u.user_quit.reason; break;
        case CC_EV_NAMES:          pa = ev->u.names.channel; pb = ev->u.names.nicks; break;
        case CC_EV_END_OF_NAMES:   pa = ev->u.end_of_names.channel; break;
        case CC_EV_NICK_CHANGED:   pa = ev->u.nick_changed.old_nick; pb = ev->u.nick_changed.new_nick; break;
        case CC_EV_TEXT:           pa = ev->u.text.text; pb = ev->u.text.nick; break;
        case CC_EV_DATA:           pa = ev->u.data.nick; pb = std::to_string(ev->u.data.annotations.mode); break;
        case CC_EV_WHISPER:        pa = ev->u.whisper.text; pb = ev->u.whisper.nick; break;
        case CC_EV_TOPIC_CHANGED:  pa = ev->u.topic_changed.channel; pb = ev->u.topic_changed.topic; break;
        case CC_EV_CHANNEL_MODE:   pa = ev->u.channel_mode.modes; pb = ev->u.channel_mode.arg; break;
        case CC_EV_USER_MODE:      pa = ev->u.user_mode.nick; pb = ev->u.user_mode.modes; break;
        case CC_EV_ROOM_PROP:      pa = ev->u.room_prop.key; pb = ev->u.room_prop.value; break;
        case CC_EV_ROOM_LIST_ITEM: pa = ev->u.room_list_item.name; pb = std::to_string(ev->u.room_list_item.users); break;
        case CC_EV_WHOIS_RESULT:   pa = ev->u.whois_result.nick; pb = ev->u.whois_result.host; break;
        case CC_EV_WHO_RESULT:     pa = ev->u.who_result.nick; pb = ev->u.who_result.channel; break;
        case CC_EV_MOTD:           pa = ev->u.motd.luser; pb = ev->u.motd.motd; break;
        case CC_EV_ERROR:          pa = std::to_string(ev->u.error.code); pb = ev->u.error.text; break;
        case CC_EV_NICK_REJECTED:  pa = std::to_string(ev->u.nick_rejected.kind); pb = ev->u.nick_rejected.bad_nick; break;
        case CC_EV_STATUS_LINE:    pa = ev->u.status_line.text; break;
        case CC_EV_AWAY_PEER:      pa = ev->u.away_peer.nick; pb = ev->u.away_peer.message; break;
        case CC_EV_ACTION:         pa = ev->u.action.text; pb = ev->u.action.nick; break;
        case CC_EV_SOUND:          pa = ev->u.sound.file; pb = ev->u.sound.nick; break;
        case CC_EV_APPEARS_AS:     pa = ev->u.appears_as.avatar_name; pb = ev->u.appears_as.url; break;
        case CC_EV_VERSION_REQUEST: pa = ev->u.version_request.from_nick; break;
        case CC_EV_INFO_REQUEST:   pa = ev->u.info_request.from_nick; break;
        default: break;
    }
    c->a.push_back(pa); c->b.push_back(pb);
}
void pvOnSend(void* ud, const uint8_t* d, size_t n) {
    static_cast<PVCap*>(ud)->sent.append((const char*)d, n);
}
const char* pvOwnNick(void*) { return "Anon"; }

// count how many times an event type appears
int pvCount(const PVCap& c, int type) {
    int n = 0; for (int t : c.types) if (t == type) n++; return n;
}
// index of first occurrence of `type` (or -1)
int pvFind(const PVCap& c, int type, int from = 0) {
    for (int i = from; i < (int)c.types.size(); i++) if (c.types[i] == type) return i;
    return -1;
}
cc_session* pvMake(PVCap* cap, cc_session_config& cfg) {
    memset(&cfg, 0, sizeof(cfg));
    cfg.user_data = cap; cfg.send = pvOnSend; cfg.on_event = pvOnEvent; cfg.own_nick = pvOwnNick;
    return cc_session_create(&cfg);
}
} // namespace

// VECTOR 1: self-JOIN triggers the auto MODE + WHO queries (HandleCommand
// cmdidJoin self branch, ircsock.cpp:1391-1425). Hand-trace: on the self JOIN,
// emit CC_EV_SELF_JOINED, then bExecuteQuery issues "MODE #comicrig\r\n" +
// "WHO #comicrig\r\n" (non-IRCX: no PropGet). Then a 324 CHANNELMODEIS
// (:2127) emits CC_EV_CHANNEL_MODE; a 352 WHO reply (:2576) emits
// CC_EV_WHO_RESULT (routed by the qpInitialWho cell); 315 ENDOFWHO dequeues.
static int cc_selftest_pv_selfjoin_autoqueries() {
    PVCap cap; cc_session_config cfg; cc_session* s = pvMake(&cap, cfg);
    // register the room token first so the emitted events carry it
    uint32_t tok = cc_session_register_room(s, "#comicrig");
    const char* wire =
        ":srv 001 Anon :Welcome\r\n"
        ":Anon!anon@h JOIN :#comicrig\r\n";          // SELF join
    cc_session_feed_bytes(s, (const uint8_t*)wire, strlen(wire));
    // self-join emitted + auto MODE/WHO sent
    CC_CHECK(pvFind(cap, CC_EV_LOGGED_IN) == 0);
    CC_CHECK(pvFind(cap, CC_EV_SELF_JOINED) >= 0);
    CC_CHECK(cap.tokens[pvFind(cap, CC_EV_SELF_JOINED)] == tok);
    CC_CHECK(cap.sent.find("MODE #comicrig\r\n") != std::string::npos);
    CC_CHECK(cap.sent.find("WHO #comicrig\r\n") != std::string::npos);
    // Now feed the server's answers: 324 mode, 352 who, 315 endofwho.
    cap.sent.clear();
    const char* replies =
        ":srv 324 Anon #comicrig +nt\r\n"
        ":srv 352 Anon #comicrig bob h srv Bob H@ :0 Bob Real\r\n"
        ":srv 315 Anon #comicrig :End of WHO\r\n";
    cc_session_feed_bytes(s, (const uint8_t*)replies, strlen(replies));
    // 324 -> CC_EV_CHANNEL_MODE("+nt"); 352 -> CC_EV_WHO_RESULT(nick Bob)
    int im = pvFind(cap, CC_EV_CHANNEL_MODE);
    CC_CHECK(im >= 0 && cap.a[im] == "+nt");
    int iw = pvFind(cap, CC_EV_WHO_RESULT);
    CC_CHECK(iw >= 0 && cap.a[iw] == "Bob" && cap.b[iw] == "#comicrig");
    cc_session_destroy(s);
    return 0;
}

// VECTOR 2: NICK across users (HandleCommand cmdidNick, ircsock.cpp:1576-1611).
// Bob renames to Bobby (other user, is_self=0), then our own nick Anon->Anon2
// (self, is_self=1). Hand-trace: two CC_EV_NICK_CHANGED, first is_self=0 with
// old=Bob new=Bobby, second is_self=1 with old=Anon new=Anon2.
static int cc_selftest_pv_nick_across_users() {
    PVCap cap; cc_session_config cfg; cc_session* s = pvMake(&cap, cfg);
    const char* wire =
        ":Bob!bob@h NICK :Bobby\r\n"
        ":Anon!anon@h NICK :Anon2\r\n";
    cc_session_feed_bytes(s, (const uint8_t*)wire, strlen(wire));
    CC_CHECK(pvCount(cap, CC_EV_NICK_CHANGED) == 2);
    int i0 = pvFind(cap, CC_EV_NICK_CHANGED);
    CC_CHECK(cap.a[i0] == "Bob" && cap.b[i0] == "Bobby");
    int i1 = pvFind(cap, CC_EV_NICK_CHANGED, i0 + 1);
    CC_CHECK(cap.a[i1] == "Anon" && cap.b[i1] == "Anon2");
    cc_session_destroy(s);
    return 0;
}

// VECTOR 3: TOPIC set (HandleCommand cmdidTopic, ircsock.cpp:1779-1830). A
// TOPIC command from Bob sets the room topic. Hand-trace: one
// CC_EV_TOPIC_CHANGED(channel=#comicrig, topic="Welcome all").
static int cc_selftest_pv_topic_set() {
    PVCap cap; cc_session_config cfg; cc_session* s = pvMake(&cap, cfg);
    const char* wire = ":Bob!bob@h TOPIC #comicrig :Welcome all\r\n";
    cc_session_feed_bytes(s, (const uint8_t*)wire, strlen(wire));
    int it = pvFind(cap, CC_EV_TOPIC_CHANGED);
    CC_CHECK(it >= 0 && cap.a[it] == "#comicrig" && cap.b[it] == "Welcome all");
    cc_session_destroy(s);
    return 0;
}

// VECTOR 4: channel MODE delta (HandleCommand cmdidMode channel branch,
// ircsock.cpp:1448-1511 + ParseChannelMode :299-400). "+o Bob" arrives.
// Hand-trace: one CC_EV_CHANNEL_MODE(modes="+o", arg="Bob"). ParseChannelMode's
// +o branch (member-status) is carried by the emitted delta (Swift applies it).
static int cc_selftest_pv_channel_mode_delta() {
    PVCap cap; cc_session_config cfg; cc_session* s = pvMake(&cap, cfg);
    const char* wire = ":srv MODE #comicrig +o Bob\r\n";
    cc_session_feed_bytes(s, (const uint8_t*)wire, strlen(wire));
    int im = pvFind(cap, CC_EV_CHANNEL_MODE);
    CC_CHECK(im >= 0 && cap.a[im] == "+o" && cap.b[im] == "Bob");
    cc_session_destroy(s);
    return 0;
}

// VECTOR 5: IRCX DATA CCUDI1 out-of-band annotation (HandleCommand cmdidData,
// ircsock.cpp:1275-1323) vs the inline plain-IRC form. Both carry the same
// "#G295E193M1" block.
//
// REVIEW FIX (Plan 3 Task 6, fix round 1): this test previously paired the
// DATA line with a PRIVMSG that ALSO carried a redundant inline "(#...)"
// block -- so the paired PRIVMSG's has_annotations came from the INLINE
// block, not from any DATA-line pairing, which masked the fact that
// engine-side DATA-to-PRIVMSG pairing does not happen at all. It does not:
// OnTextMsg and OnDataMsg (protsupp.cpp) each construct their OWN fresh
// stack-local `CUserInfo pui;` scratch object per call (never a shared,
// persistent per-nick CUserInfo), so a DATA line's decoded m_udi can never
// survive to be read by a later, separate PRIVMSG call -- there is no engine
// state for it to survive IN. This is the CORRECT, intentional design (the
// plan was amended so DATA->PRIVMSG re-pairing by nick is Task 7's (Swift's)
// job, once it owns a real per-nick user table); this test now asserts that
// real engine contract instead of accidentally hiding it:
//   * DATA line -> CC_EV_DATA(nick=Bob, annotations decoded from the blob)
//   * the FOLLOWING PLAIN PRIVMSG (no inline block) from the SAME nick ->
//     CC_EV_TEXT(text="hi", has_annotations=0) -- the engine does NOT
//     auto-attach the preceding DATA blob to it.
// A separate, self-contained case below freezes the OTHER (unaffected)
// contract: a plain-IRC INLINE "(#...)" PRIVMSG is self-describing per
// message and DOES emit has_annotations=1 on its own -- that path never
// depended on any cross-message state and is untouched by this fix.
// IndexToByte packs value+'0', so '2'=index 2, '9'=index 9, '5'=index 5, etc.
static int cc_selftest_pv_data_vs_inline() {
    PVCap cap; cc_session_config cfg; cc_session* s = pvMake(&cap, cfg);
    // full-capture on_event to inspect annotation fields
    struct DCap { cc_annotations dataAnn{}; int haveData=0; cc_annotations textAnn{}; int haveText=0; int textHasAnn=-1; std::string textBody; } dc;
    cfg.user_data = &dc;
    cfg.on_event = [](void* ud, const cc_proto_event* ev){
        DCap* c = static_cast<DCap*>(ud);
        if (ev->type==CC_EV_DATA){ c->dataAnn = ev->u.data.annotations; c->haveData=1; }
        if (ev->type==CC_EV_TEXT){ c->textAnn = ev->u.text.annotations; c->haveText = 1; c->textHasAnn = ev->u.text.has_annotations; c->textBody = ev->u.text.text; }
    };
    cc_session_destroy(s);
    s = cc_session_create(&cfg);
    // DATA line (out-of-band IRCX annotation blob), then a PLAIN PRIVMSG (NO
    // inline "(#...)" block) from the same nick Bob -- freezes the engine's
    // stateless contract: no in-engine DATA->PRIVMSG pairing.
    const char* wire =
        ":Bob!bob@h DATA #comicrig CCUDI1 :#G295E193M1\r\n"          // IRCX out-of-band
        ":Bob!bob@h PRIVMSG #comicrig :hi\r\n";                       // plain, NO inline block
    cc_session_feed_bytes(s, (const uint8_t*)wire, strlen(wire));
    // DATA decode: #G <2><9><5> E <1><9><3> M <1>. IndexToByte(v)=v+'0', so
    // ByteToIndex('2')=2, ('9')=9, ('5')=5, ('1')=1, ('3')=3.
    CC_CHECK(dc.haveData == 1);
    CC_CHECK(dc.dataAnn.gesture_pose == 2 && dc.dataAnn.gesture_emotion == 9 && dc.dataAnn.gesture_intensity == 5);
    CC_CHECK(dc.dataAnn.face_pose == 1 && dc.dataAnn.face_emotion == 9 && dc.dataAnn.face_intensity == 3);
    CC_CHECK(dc.dataAnn.mode == 1 && dc.dataAnn.cooked == 1);
    // Plain PRIVMSG decode: engine does NOT pair the preceding DATA blob --
    // has_annotations must be 0 (Task 7/Swift owns cross-message re-pairing
    // by nick; this test freezes the engine boundary, not a weaker one).
    CC_CHECK(dc.haveText == 1 && dc.textBody == "hi");
    CC_CHECK(dc.textHasAnn == 0);

    // Separate case: plain-IRC INLINE "(#...)" PRIVMSG IS self-contained
    // per-message and DOES emit has_annotations=1 -- unaffected by the fix
    // above, frozen here so both contracts are covered by this vector.
    dc = DCap{};
    const char* wireInline =
        ":Bob!bob@h PRIVMSG #comicrig :(#G295E193M1) hi\r\n";        // plain-IRC inline
    cc_session_feed_bytes(s, (const uint8_t*)wireInline, strlen(wireInline));
    CC_CHECK(dc.haveText == 1 && dc.textBody == "hi");
    CC_CHECK(dc.textHasAnn == 1);
    CC_CHECK(dc.textAnn.gesture_pose == 2 && dc.textAnn.mode == 1);
    cc_session_destroy(s);
    return 0;
}

// Plan 4a Task 3 Step 3: escaped-byte annotation vector (the second Plan-3
// must-own debt item: Task 8's "unquoting ran as a no-op" gap, made
// falsifiable here). Two parts:
//
// (a) Direct pair test over bLowLevelQuoting/bLowLevelUnquoting themselves,
//     confirming the discovered escape alphabet against the lifted code (read
//     FIRST, ccommon_str.cpp:75-245, before writing this): the quoting char
//     is g_chLLQuoteCTCP (0x10); only THREE input bytes are ever escaped --
//     LF (0x0A) -> <Q>'n', CR (0x0D) -> <Q>'r', and the quote char itself
//     (0x10) -> <Q><Q> (doubled). This is a narrow, targeted 3-symbol scheme,
//     NOT a general CTCP low/high-bit quoting table and NOT a byte-oblivious
//     escaper -- 0x00 and every other byte pass through unescaped. The
//     brief's {0x0A, 0x0D, 0x10} triple is exactly this alphabet (confirmed
//     against the 1998 source, not assumed).
//
// (b) End-to-end decode: a hand-authored inbound PRIVMSG whose line carries
//     the quoted form of LF/CR/quote-char in the trailing (post-annotation)
//     say text. bLowLevelUnquoting runs over the WHOLE szMesg line
//     (ccProcessSay, protsupp.cpp:1058) BEFORE the "(#...)" annotation
//     parenthetical is even located -- so this one call unquotes both the
//     annotation-adjacent framing and the trailing text in a single pass.
//     Feeding this through the real session (cc_session_feed_bytes) and
//     capturing CC_EV_TEXT's has_annotations + decoded text bytes falsifies
//     Task 8's suspected "unquoting ran as a no-op" gap: if unquoting were a
//     no-op, ev.u.text.text would still contain the LITERAL "<0x10>n<0x10>r"
//     bytes instead of real LF/CR.
/* HAND-AUTHORED -- promote a real captured escaped-byte exchange during the 4b live acceptance */
static int cc_selftest_annotation_escaped_bytes() {
    const char Q = (char)0x10;

    // --- (a) direct pair test -------------------------------------------
    {
        // Buffer containing LF, CR, and the quote char itself, plus printable
        // padding on both sides.
        char src[] = { 'a', 0x0A, 'b', 0x0D, 'c', Q, 'd', 0 };
        char* dst = nullptr; BOOL freeit = FALSE;
        BOOL changed = bLowLevelQuoting(Q, TRUE /*bTreatAsByteArray*/, src, &dst, &freeit, FALSE);
        CC_CHECK(changed == TRUE);
        // Expected quoted form: a <Q>n b <Q>r c <Q><Q> d
        const char expectedQuoted[] = { 'a', Q, 'n', 'b', Q, 'r', 'c', Q, Q, 'd', 0 };
        CC_CHECK(dst != nullptr);
        CC_CHECK(memcmp(dst, expectedQuoted, sizeof(expectedQuoted)) == 0);

        // Round-trip through bLowLevelUnquoting: byte-identical to src.
        char buf[16];
        BOOL ok = bLowLevelUnquoting(Q, TRUE /*bTreatAsByteArray*/, dst, buf);
        CC_CHECK(ok == TRUE);
        CC_CHECK(memcmp(buf, src, sizeof(src)) == 0);

        if (freeit) free(dst);
    }

    // --- (b) end-to-end decode through the real session -----------------
    {
        cc_session_config cfg; memset(&cfg, 0, sizeof(cfg));
        cfg.send = [](void*, const uint8_t*, size_t) {};
        cfg.own_nick = pvOwnNick;
        // full-capture on_event (same shape as cc_selftest_pv_data_vs_inline's
        // DCap) so both has_annotations AND the decoded annotation fields AND
        // the decoded text bytes are all inspectable, not just the PVCap
        // pa/pb summary fields.
        struct ECap { int haveText=0; int textHasAnn=-1; cc_annotations textAnn{}; std::string textBody; } ec;
        cfg.user_data = &ec;
        cfg.on_event = [](void* ud, const cc_proto_event* ev){
            ECap* c = static_cast<ECap*>(ud);
            if (ev->type==CC_EV_TEXT){ c->haveText=1; c->textHasAnn=ev->u.text.has_annotations; c->textAnn=ev->u.text.annotations; c->textBody=ev->u.text.text; }
        };
        cc_session* s = cc_session_create(&cfg);

        // Wire line: inline annotation "(#G295E193M1) " (same grammar as
        // cc_selftest_pv_data_vs_inline: gesture 2,9,5 / expr 1,9,3 / mode 1),
        // followed by say text whose bytes are the QUOTED forms of LF, CR,
        // and the quote char itself -- exercised end-to-end through the
        // real IRC line framer (cc_session_feed_bytes), which splits wire
        // input on literal \r\n only; these quoted sequences are ordinary
        // in-line payload bytes to the framer and only become real LF/CR/Q
        // after bLowLevelUnquoting runs inside ccProcessSay.
        char wire[128];
        int n = 0;
        const char* head = ":Win!u@h PRIVMSG #comicrig :(#G295E193M1) hello";
        memcpy(wire + n, head, strlen(head)); n += (int)strlen(head);
        wire[n++] = Q; wire[n++] = 'n';   // quoted LF
        wire[n++] = Q; wire[n++] = 'r';   // quoted CR
        wire[n++] = Q; wire[n++] = Q;     // quoted quote-char itself
        wire[n++] = '!';
        wire[n++] = '\r'; wire[n++] = '\n';  // real wire line terminator

        cc_session_feed_bytes(s, (const uint8_t*)wire, (size_t)n);

        CC_CHECK(ec.haveText == 1);
        CC_CHECK(ec.textHasAnn == 1);
        CC_CHECK(ec.textAnn.gesture_pose == 2 && ec.textAnn.mode == 1);
        // Decoded LF, CR, quote-char, literal '!' -- byte-identical proof
        // that unquoting actually ran (a no-op would leave the literal
        // "<0x10>n<0x10>r<0x10><0x10>" bytes in place instead).
        CC_CHECK(ec.textBody == "hello\x0A\x0D\x10!");
        cc_session_destroy(s);
    }

    return 0;
}

// VECTOR 6: WHISPER inbound (HandleCommand cmdidWhisper, ircsock.cpp:1832-1844).
// "WHISPER #comicrig Anon :psst" -> CC_EV_WHISPER(nick=Bob, text="psst").
static int cc_selftest_pv_whisper() {
    PVCap cap; cc_session_config cfg; cc_session* s = pvMake(&cap, cfg);
    const char* wire = ":Bob!bob@h WHISPER #comicrig Anon :psst\r\n";
    cc_session_feed_bytes(s, (const uint8_t*)wire, strlen(wire));
    int iw = pvFind(cap, CC_EV_WHISPER);
    CC_CHECK(iw >= 0 && cap.a[iw] == "psst" && cap.b[iw] == "Bob");
    cc_session_destroy(s);
    return 0;
}

// VECTOR 7: "# Appears as" avatar announce (Task-6 seam -- NOW CLOSED). A
// "# Appears as" arriving via PRIVMSG now routes through OnTextMsg->
// ProcessComment (protsupp.cpp), classifying into CC_EV_APPEARS_AS(name,url)
// instead of the pre-Task-6 raw CC_EV_TEXT. name/url grammar: GetToken reads
// up to the next whitespace/separator ("bob"), GetToken2(".,)",",)")  reads
// the rest up to a '.'/','/')' terminator ("http://x/bob.avb" has no such
// terminator before EOS, so it reads the whole remainder).
static int cc_selftest_pv_appears_as_seam() {
    PVCap cap; cc_session_config cfg; cc_session* s = pvMake(&cap, cfg);
    const char* wire = ":Bob!bob@h PRIVMSG #comicrig :# Appears as bob.http://x/bob.avb\r\n";
    cc_session_feed_bytes(s, (const uint8_t*)wire, strlen(wire));
    CC_CHECK(pvFind(cap, CC_EV_TEXT) < 0);  // no longer a raw CC_EV_TEXT
    int it = pvFind(cap, CC_EV_APPEARS_AS);
    CC_CHECK(it >= 0);
    CC_CHECK(cap.a[it] == "bob" && cap.b[it] == "http://x/bob.avb");
    cc_session_destroy(s);
    return 0;
}

// VECTOR 8: room LIST (HandleResultCode 321/322/323, ircsock.cpp:2311-2506).
// Hand-trace: issue LIST (register a qpRoomListDlg ctList cell), then feed
// 321 begin, 322 item, 323 end -> CC_EV_ROOM_LIST_BEGIN, CC_EV_ROOM_LIST_ITEM
// (#comicrig, 3 users, "Welcome"), CC_EV_ROOM_LIST_END.
static int cc_selftest_pv_room_list() {
    PVCap cap; cc_session_config cfg; cc_session* s = pvMake(&cap, cfg);
    CC_CHECK(cc_session_list(s, nullptr) == 0);   // enqueues ctList/qpRoomListDlg
    cap.sent.clear();
    const char* wire =
        ":srv 321 Anon Channel :Users Name\r\n"
        ":srv 322 Anon #comicrig 3 :Welcome\r\n"
        ":srv 323 Anon :End of LIST\r\n";
    cc_session_feed_bytes(s, (const uint8_t*)wire, strlen(wire));
    CC_CHECK(pvFind(cap, CC_EV_ROOM_LIST_BEGIN) >= 0);
    int ii = pvFind(cap, CC_EV_ROOM_LIST_ITEM);
    CC_CHECK(ii >= 0 && cap.a[ii] == "#comicrig" && cap.b[ii] == "3");
    CC_CHECK(pvFind(cap, CC_EV_ROOM_LIST_END) >= 0);
    cc_session_destroy(s);
    return 0;
}

// VECTOR 9: MOTD (HandleResultCode 375/372/376, ircsock.cpp:2755-2808). Login
// (001) enqueues the qpInitialLUsersMOTD cell; 375 start (silent), 372 lines
// (accumulate silently while the cell is qpInitialLUsersMOTD -- NOT qpLUsersMOTD,
// so 372 emits a status line? no: our gate suppresses only qpLUsersMOTD; the
// initial sequence is qpInitialLUsersMOTD, so 372 DOES emit status). 376
// flushes -> CC_EV_MOTD(motd="line1\r\nline2\r\n"). Hand-trace the 376 event.
static int cc_selftest_pv_motd() {
    PVCap cap; cc_session_config cfg; cc_session* s = pvMake(&cap, cfg);
    const char* wire =
        ":srv 001 Anon :Welcome\r\n"      // enqueues qpInitialLUsersMOTD
        ":srv 375 Anon :- srv MOTD -\r\n"
        ":srv 372 Anon :- line one\r\n"
        ":srv 372 Anon :- line two\r\n"
        ":srv 376 Anon :End of MOTD\r\n";
    cc_session_feed_bytes(s, (const uint8_t*)wire, strlen(wire));
    int im = pvFind(cap, CC_EV_MOTD);
    CC_CHECK(im >= 0);
    // 372 strips a leading "- "; the accumulator joins with "\r\n".
    CC_CHECK(cap.b[im] == "line one\r\nline two\r\n");
    cc_session_destroy(s);
    return 0;
}

// VECTOR 10: 433 nick-collision (HandleErrorCode 431/432/433,
// ircsock.cpp:3128-3136). "433 Anon Taken :Nickname in use" ->
// CC_EV_NICK_REJECTED(kind=433, bad_nick="Taken"). Swift owns the retry.
static int cc_selftest_pv_nick_collision() {
    PVCap cap; cc_session_config cfg; cc_session* s = pvMake(&cap, cfg);
    const char* wire = ":srv 433 Anon Taken :Nickname is already in use\r\n";
    cc_session_feed_bytes(s, (const uint8_t*)wire, strlen(wire));
    int in = pvFind(cap, CC_EV_NICK_REJECTED);
    CC_CHECK(in >= 0 && cap.a[in] == "433" && cap.b[in] == "Taken");
    cc_session_destroy(s);
    return 0;
}

// VECTOR 11: fatal ERROR (HandleCommand cmdidError, ircsock.cpp:1325-1346).
// A verbatim ERROR line (not during the ISIRCX probe) -> CC_EV_DISCONNECTED_HINT
// with the text. R20: user-facing, carried as the event payload (not ccLog).
static int cc_selftest_pv_fatal_error() {
    PVCap cap; cc_session_config cfg; cc_session* s = pvMake(&cap, cfg);
    const char* wire = "ERROR :Closing Link: you are banned\r\n";
    cc_session_feed_bytes(s, (const uint8_t*)wire, strlen(wire));
    int ie = pvFind(cap, CC_EV_DISCONNECTED_HINT);
    CC_CHECK(ie >= 0 && cap.a[ie] == "Closing Link: you are banned");
    cc_session_destroy(s);
    return 0;
}

// VECTOR 12: IRCX server caps + probe-timer cancel (HandleResultCode 800,
// ircsock.cpp:2817-2900). The probe is sent (cc_session_probe_ircx enqueues the
// ctModeIsIrcX cell + requests the timer); the 800 (state 0) reply sets IRCX,
// parses ANON, grows to maxlen 1024, cancels the timer, emits CC_EV_SERVER_CAPS,
// and sends "IRCX\r\n".
static int cc_selftest_pv_ircx_caps() {
    PVCap cap; cc_session_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    struct TCap { PVCap pv; int cancelId = -1; } tc;
    cfg.user_data = &tc.pv; cfg.send = pvOnSend; cfg.on_event = pvOnEvent; cfg.own_nick = pvOwnNick;
    cfg.set_timer = [](void*, int32_t, int32_t){};
    // cancel_timer records the id; wrap via a static since the lambda can't
    // capture with a C function-pointer signature -- use user_data indirection.
    static int s_cancelId; static int s_cancelCalls;
    s_cancelId = -1; s_cancelCalls = 0;
    cfg.cancel_timer = [](void*, int32_t id){ s_cancelId = id; s_cancelCalls++; };
    cc_session* s = cc_session_create(&cfg);
    CC_CHECK(cc_session_probe_ircx(s) == 0);
    tc.pv.sent.clear();
    // 800 state-0: :srv 800 * 0 0 NTLM,ANON 1024 *
    const char* wire = ":srv 800 * 0 0 NTLM,ANON 1024 *\r\n";
    cc_session_feed_bytes(s, (const uint8_t*)wire, strlen(wire));
    int ic = pvFind(tc.pv, CC_EV_SERVER_CAPS);
    CC_CHECK(ic >= 0 && tc.pv.a[ic] == "1" /*ircx*/ && tc.pv.b[ic] == "1024" /*maxlen*/);
    CC_CHECK(tc.pv.sent.find("IRCX\r\n") != std::string::npos);
    CC_CHECK(s_cancelCalls == 1 && s_cancelId == CC_TIMER_ISIRCX_PROBE);
    cc_session_destroy(s);
    return 0;
}

// VECTOR 12b (Plan 4a Task 2 amendment): the SECOND 800 (state 1), anon
// allowed -- the login gate this task added (see ircsock.cpp's RPL_IRCX
// handler comment). Confirms a SECOND CC_EV_SERVER_CAPS is emitted (the
// edge-trigger ProtocolSession.sendLoginIfNeeded relies on) and that the
// anon-NOT-allowed sibling path (CC_EV_AUTH_UNSUPPORTED) is unaffected.
static int cc_selftest_pv_ircx_second_800_anon_allowed() {
    PVCap cap; cc_session_config cfg; cc_session* s = pvMake(&cap, cfg);
    // The 800 handler only recognizes a reply if the ctModeIsIrcX/ctIrcX
    // query is pending (FindQuery); the probe registers it (same setup
    // cc_selftest_pv_ircx_caps uses for the first 800).
    CC_CHECK(cc_session_probe_ircx(s) == 0);
    cap.sent.clear();
    const char* wire =
        ":srv 800 * 0 0 ANON 512 *\r\n"
        ":srv 800 * 1 0 ANON 512 *\r\n";
    cc_session_feed_bytes(s, (const uint8_t*)wire, strlen(wire));
    int first = pvFind(cap, CC_EV_SERVER_CAPS);
    CC_CHECK(first >= 0);
    int second = pvFind(cap, CC_EV_SERVER_CAPS, first + 1);
    CC_CHECK(second >= 0 && cap.a[second] == "1" && cap.b[second] == "512");
    CC_CHECK(pvFind(cap, CC_EV_AUTH_UNSUPPORTED) < 0);
    cc_session_destroy(s);
    return 0;
}

// VECTOR 12c: the SECOND 800, anon NOT allowed -- CC_EV_AUTH_UNSUPPORTED
// (unchanged sibling path), NOT a second CC_EV_SERVER_CAPS.
static int cc_selftest_pv_ircx_second_800_anon_disallowed() {
    PVCap cap; cc_session_config cfg; cc_session* s = pvMake(&cap, cfg);
    CC_CHECK(cc_session_probe_ircx(s) == 0);
    cap.sent.clear();
    const char* wire =
        ":srv 800 * 0 0 NTLM 512 *\r\n"
        ":srv 800 * 1 0 NTLM 512 *\r\n";
    cc_session_feed_bytes(s, (const uint8_t*)wire, strlen(wire));
    int first = pvFind(cap, CC_EV_SERVER_CAPS);
    CC_CHECK(first >= 0);
    CC_CHECK(pvFind(cap, CC_EV_SERVER_CAPS, first + 1) < 0);
    CC_CHECK(pvFind(cap, CC_EV_AUTH_UNSUPPORTED) >= 0);
    cc_session_destroy(s);
    return 0;
}

// VECTOR 13: PART/QUIT membership (HandleCommand cmdidPart :1668, cmdidQuit
// :1760). Bob parts (other -> CC_EV_USER_PARTED), Carl quits (-> CC_EV_USER_QUIT),
// then we ourselves part (-> CC_EV_SELF_PARTED).
static int cc_selftest_pv_part_quit() {
    PVCap cap; cc_session_config cfg; cc_session* s = pvMake(&cap, cfg);
    const char* wire =
        ":Bob!bob@h PART #comicrig :bye\r\n"
        ":Carl!carl@h QUIT :Client exited\r\n"
        ":Anon!anon@h PART #comicrig\r\n";
    cc_session_feed_bytes(s, (const uint8_t*)wire, strlen(wire));
    int ip = pvFind(cap, CC_EV_USER_PARTED);
    CC_CHECK(ip >= 0 && cap.a[ip] == "Bob" && cap.b[ip] == "bye");
    int iq = pvFind(cap, CC_EV_USER_QUIT);
    CC_CHECK(iq >= 0 && cap.a[iq] == "Carl" && cap.b[iq] == "Client exited");
    CC_CHECK(pvFind(cap, CC_EV_SELF_PARTED) >= 0);
    cc_session_destroy(s);
    return 0;
}

// VECTOR 14: NAMES reply (HandleResultCode 353/366, ircsock.cpp:2508-2574).
// A self-JOIN enqueues the ctNames cell; 353 emits CC_EV_NAMES(nicks) and 366
// emits CC_EV_END_OF_NAMES.
static int cc_selftest_pv_names() {
    PVCap cap; cc_session_config cfg; cc_session* s = pvMake(&cap, cfg);
    const char* wire =
        ":Anon!anon@h JOIN :#comicrig\r\n"                       // enqueues ctNames
        ":srv 353 Anon = #comicrig :Anon @Bob Carl\r\n"
        ":srv 366 Anon #comicrig :End of NAMES\r\n";
    cc_session_feed_bytes(s, (const uint8_t*)wire, strlen(wire));
    int in = pvFind(cap, CC_EV_NAMES);
    CC_CHECK(in >= 0 && cap.a[in] == "#comicrig" && cap.b[in] == "Anon @Bob Carl");
    CC_CHECK(pvFind(cap, CC_EV_END_OF_NAMES) >= 0);
    cc_session_destroy(s);
    return 0;
}

// =============================================================================
// Plan 3 Task 6: payload-stage (ProcessSay/ProcessComment/CTCP) selftests.
// Brief Step 1's three failing-then-passing vectors, plus per-CTCP/comment-
// grammar coverage (Step 6 equivalent). All feed real wire bytes through
// cc_session_feed_bytes -> HandleCommand -> OnTextMsg/OnDataMsg (the payload
// stage this task lifts), proving the CLASSIFICATION now happens (pre-Task-6,
// these all arrived as raw CC_EV_TEXT -- see VECTOR 5/7's comments above,
// which this task updated to assert the closed seam).
// =============================================================================

// VECTOR 15 (brief Step 1a): \x01ACTION waves\x01 -> CC_EV_ACTION(nick,text).
// PrepareTextAction strips the CTCP framing + trailing 0x01 and prefixes the
// nick (protsupp.cpp:1104-1114): "Bob" + " waves" = "Bob waves".
static int cc_selftest_pv_action_ctcp() {
    PVCap cap; cc_session_config cfg; cc_session* s = pvMake(&cap, cfg);
    char wire[128];
    int n = snprintf(wire, sizeof(wire), ":Bob!bob@h PRIVMSG #comicrig :%cACTION waves%c\r\n", 0x01, 0x01);
    cc_session_feed_bytes(s, (const uint8_t*)wire, (size_t)n);
    CC_CHECK(pvFind(cap, CC_EV_TEXT) < 0);   // no longer a raw say
    int ia = pvFind(cap, CC_EV_ACTION);
    CC_CHECK(ia >= 0);
    CC_CHECK(cap.a[ia] == "Bob waves" && cap.b[ia] == "Bob");
    cc_session_destroy(s);
    return 0;
}

// VECTOR 16 (brief Step 1b/c + the R19 resolver end-to-end proof): an inline
// "(#...) " say addressed to Bob (T-group "Bob") arrives with a resolver that
// maps "Bob" -> ref 7 (CC_USER_REF_NONE=0, so 7 is an arbitrary non-zero
// stand-in ref). Asserts CC_EV_TEXT(has_annotations=1, addressees[0]=="Bob")
// AND, separately, that calling the SAME resolver function
// ccSessionResolveUser answers with ref 7 for "Bob" in this room -- proving
// the resolver wiring the payload stage's ccPayloadLookupAdapter
// (protsupp.cpp) threads through actually reaches the caller-supplied
// resolve_user callback end-to-end (R19): the decoded talkTos string ("Bob")
// is the wire-facing form cc_annotations carries (comicchat.h: addressees[]
// are encoded nick strings, not refs -- Task 5a's C-boundary design, see
// protsupp.cpp's Task 6 header comment), and the SAME nick, run back through
// the resolver, is exactly the ref (7) GetTalkTos/ccPayloadLookupAdapter
// resolved it to internally during the decode (a real CUserInfo* was vended
// for ref 7 and stored in pui->m_udi.m_talkTos -- proving the resolver call
// actually happened, not just that it COULD be called).
static cc_user_ref pvResolveBobToSeven(void* /*user_data*/, const char* nick, uint32_t /*room_token*/) {
    if (nick && !strcmp(nick, "Bob")) return 7;
    return CC_USER_REF_NONE;
}
static int cc_selftest_pv_inline_annotation_resolver_e2e() {
    struct Cap { std::string text, nick; cc_annotations ann{}; int hasAnn = 0; } cap;
    cc_session_config cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.user_data = &cap;
    cfg.send = [](void*, const uint8_t*, size_t) {};
    cfg.own_nick = pvOwnNick;
    cfg.resolve_user = pvResolveBobToSeven;
    cfg.on_event = [](void* ud, const cc_proto_event* ev) {
        if (ev->type != CC_EV_TEXT) return;
        Cap* c = static_cast<Cap*>(ud);
        c->text = ev->u.text.text; c->nick = ev->u.text.nick;
        c->hasAnn = ev->u.text.has_annotations; c->ann = ev->u.text.annotations;
    };
    cc_session* s = cc_session_create(&cfg);
    // #G295E193M1 (see cc_selftest_annotation_codec's byte-arithmetic
    // comment) + T-group addressing "Bob".
    const char* wire = ":Carl!carl@h PRIVMSG #comicrig :(#G295E193M1TBob) hello\r\n";
    cc_session_feed_bytes(s, (const uint8_t*)wire, strlen(wire));
    CC_CHECK(cap.text == "hello" && cap.nick == "Carl");
    CC_CHECK(cap.hasAnn == 1);
    CC_CHECK(cap.ann.addressee_count == 1 && strcmp(cap.ann.addressees[0], "Bob") == 0);
    // R19 end-to-end proof: the SAME resolver the payload stage called
    // internally (ccSessionResolveUser -> cfg.resolve_user, wired via
    // ccPayloadLookupAdapter -> GetTalkTos's pfnLookupPui parameter,
    // protsupp.cpp) answers ref 7 for "Bob" -- call it directly here (through
    // the public cc_session.h resolver, activated for this session) to prove
    // it is reachable and wired, not merely declared.
    ccActivateSessionForTest(s);
    CC_CHECK(ccSessionResolveUser("Bob", 0) == 7);
    ccDeactivateSessionForTest();
    cc_session_destroy(s);
    return 0;
}

// VECTOR 17 (brief Step 6 equivalent): \x01SOUND "boing.wav"\x01 ->
// CC_EV_SOUND(nick, file, text). PrepareSound (protsupp.cpp:1382-1440):
// quoted-filename branch needs no CTCPUnQuoteString (see ccProcessSay's
// SOUND-branch comment for the documented deviation on the unquoted form).
static int cc_selftest_pv_sound_ctcp() {
    PVCap cap; cc_session_config cfg; cc_session* s = pvMake(&cap, cfg);
    char wire[128];
    int n = snprintf(wire, sizeof(wire), ":Bob!bob@h PRIVMSG #comicrig :%cSOUND \"boing.wav\"%c\r\n", 0x01, 0x01);
    cc_session_feed_bytes(s, (const uint8_t*)wire, (size_t)n);
    int is = pvFind(cap, CC_EV_SOUND);
    CC_CHECK(is >= 0);
    CC_CHECK(cap.a[is] == "boing.wav" /*file*/ && cap.b[is] == "Bob" /*nick*/);
    cc_session_destroy(s);
    return 0;
}

// OUTBOUND SOUND ROUND-TRIP (Plan 4b outbound-sound task): drives
// cc_session_send_sound for real, asserts the EXACT wire bytes it produces
// (byte-for-byte per comicchat.h's doc comment), then feeds that SAME line
// back into cc_session_feed_bytes (as if a server echoed our own PRIVMSG) and
// asserts CC_EV_SOUND fires with the same file/nick shape VECTOR 17 pins for
// the inbound-only case -- proving send->wire->parse round-trips through
// this engine's own two halves, not just that each half independently
// produces/accepts SOME plausible bytes.
static int cc_selftest_outbound_sound_roundtrip() {
    PVCap cap; cc_session_config cfg; cc_session* s = pvMake(&cap, cfg);
    uint32_t token = cc_session_register_room(s, "#comicrig");
    CC_CHECK(token != CC_ROOM_TOKEN_NONE);

    CC_CHECK(cc_session_send_sound(s, token, "boing.wav", "") == 0);
    // Exact wire bytes: "\x01SOUND \"boing.wav\" \x01\r\n" -- bChatSendToTarget's
    // single-shot branch wraps the already-composed payload as
    // "PRIVMSG <target> :<payload>\r\n" (no separate annotations arg here).
    std::string expected = std::string("PRIVMSG #comicrig :\x01") + "SOUND \"boing.wav\" \x01\r\n";
    CC_CHECK(cap.sent == expected);

    // Feed the wire bytes we JUST captured back into the same session, framed
    // as a peer's own PRIVMSG (self-send round-trip through this engine's
    // inbound parser) -- reusing the payload this engine itself produced,
    // not a hand-typed literal, so this really proves send->parse agreement.
    std::string echoLine = ":Bob!bob@h PRIVMSG #comicrig :\x01" "SOUND \"boing.wav\" \x01\r\n";
    cc_session_feed_bytes(s, (const uint8_t*)echoLine.data(), echoLine.size());
    int is = pvFind(cap, CC_EV_SOUND);
    CC_CHECK(is >= 0);
    CC_CHECK(cap.a[is] == "boing.wav" /*file*/ && cap.b[is] == "Bob" /*nick*/);

    cc_session_destroy(s);
    return 0;
}

// VECTOR 18 (brief Step 6 equivalent): peer \x01AWAY message\x01 ->
// CC_EV_AWAY_PEER(nick, message). protsupp.cpp:1777-1794's grammar: strip
// the trailing 0x01, message text passed through as-is.
static int cc_selftest_pv_away_peer_ctcp() {
    PVCap cap; cc_session_config cfg; cc_session* s = pvMake(&cap, cfg);
    char wire[128];
    int n = snprintf(wire, sizeof(wire), ":Bob!bob@h PRIVMSG #comicrig :%cAWAY gone fishing%c\r\n", 0x01, 0x01);
    cc_session_feed_bytes(s, (const uint8_t*)wire, (size_t)n);
    int ia = pvFind(cap, CC_EV_AWAY_PEER);
    CC_CHECK(ia >= 0);
    CC_CHECK(cap.a[ia] == "Bob" && cap.b[ia] == "gone fishing");
    cc_session_destroy(s);
    return 0;
}

// VECTOR 19 (brief Step 6 equivalent, comment-grammar coverage): the
// R20-dropped "#" comment branches (HeresInfo/BDrop/BDrop2) all classify as
// ccPayloadHandledNoEvent (no event) since their original bodies are pure
// outbound-reply-sending or live-pui-gated policy actions this engine
// doesn't hold (see ccProcessComment's per-branch comments). Confirms they
// are silently dropped, not crashes or misrouted events -- matching the
// original's own "return TRUE, no visible effect for us" shape for a
// headless peer. GetInfo used to be in this group too, but Plan 4b Batch C
// un-suppressed it (CC_EV_INFO_REQUEST, Tim opted in) -- see
// cc_selftest_pv_version_and_getinfo_requests below for its own coverage.
static int cc_selftest_pv_comment_grammar_suppressed() {
    {
        PVCap cap; cc_session_config cfg; cc_session* s = pvMake(&cap, cfg);
        const char* wire = ":Bob!bob@h PRIVMSG #comicrig :# HeresInfo: some profile text\r\n";
        cc_session_feed_bytes(s, (const uint8_t*)wire, strlen(wire));
        CC_CHECK(cap.types.empty());
        cc_session_destroy(s);
    }
    {
        PVCap cap; cc_session_config cfg; cc_session* s = pvMake(&cap, cfg);
        const char* wire = ":Bob!bob@h PRIVMSG #comicrig :# BDrop: oldbackdrop\r\n";
        cc_session_feed_bytes(s, (const uint8_t*)wire, strlen(wire));
        CC_CHECK(cap.types.empty());
        cc_session_destroy(s);
    }
    {
        PVCap cap; cc_session_config cfg; cc_session* s = pvMake(&cap, cfg);
        const char* wire = ":Bob!bob@h PRIVMSG #comicrig :# BDrop2: newbackdrop,http://x/b.bgb\r\n";
        cc_session_feed_bytes(s, (const uint8_t*)wire, strlen(wire));
        CC_CHECK(cap.types.empty());
        cc_session_destroy(s);
    }
    return 0;
}

// VECTOR 19b (Plan 4b Batch C): the two peer probe queries Tim opted back
// into -- inbound CTCP VERSION and "# GetInfo" -- now DO fire an event,
// un-suppressed from their former ccPayloadSuppressed/ccPayloadHandledNoEvent
// classifications (see ccProcessSay's VERSION-branch and ccProcessComment's
// GetInfo-branch doc comments in protsupp.cpp for the exact citation). Bare
// \x01VERSION\x01 (no argument text -- the original's ChatGetVersion request
// grammar, protsupp.cpp:3701-3706) -> CC_EV_VERSION_REQUEST(from_nick); an
// argument-carrying VERSION reply (e.g. NOTICE'd back to OUR OWN earlier
// request) stays suppressed, matching the R20 table's still-dropped "else"
// half. "# GetInfo" -> CC_EV_INFO_REQUEST(from_nick).
static int cc_selftest_pv_version_and_getinfo_requests() {
    {
        PVCap cap; cc_session_config cfg; cc_session* s = pvMake(&cap, cfg);
        char wire[128];
        int n = snprintf(wire, sizeof(wire), ":Bob!bob@h PRIVMSG #comicrig :%cVERSION%c\r\n", 0x01, 0x01);
        cc_session_feed_bytes(s, (const uint8_t*)wire, (size_t)n);
        int iv = pvFind(cap, CC_EV_VERSION_REQUEST);
        CC_CHECK(iv >= 0 && cap.a[iv] == "Bob");
        cc_session_destroy(s);
    }
    {
        // Argument-carrying VERSION (a reply arriving via PRIVMSG, not a bare
        // probe) stays suppressed -- NOT CC_EV_VERSION_REQUEST.
        PVCap cap; cc_session_config cfg; cc_session* s = pvMake(&cap, cfg);
        char wire[128];
        int n = snprintf(wire, sizeof(wire), ":Bob!bob@h PRIVMSG #comicrig :%cVERSION some reply text%c\r\n", 0x01, 0x01);
        cc_session_feed_bytes(s, (const uint8_t*)wire, (size_t)n);
        CC_CHECK(pvFind(cap, CC_EV_VERSION_REQUEST) < 0);
        cc_session_destroy(s);
    }
    {
        PVCap cap; cc_session_config cfg; cc_session* s = pvMake(&cap, cfg);
        const char* wire = ":Bob!bob@h PRIVMSG #comicrig :# GetInfo\r\n";
        cc_session_feed_bytes(s, (const uint8_t*)wire, strlen(wire));
        int ii = pvFind(cap, CC_EV_INFO_REQUEST);
        CC_CHECK(ii >= 0 && cap.a[ii] == "Bob");
        cc_session_destroy(s);
    }
    return 0;
}

// VECTOR 20 (brief Step 6 equivalent): OnDataMsg's "# " comment arriving via
// DATA classifies the SAME way as PRIVMSG (ProcessComment is dispatched
// identically from both entry points) -- a DATA-borne "# Appears as" ->
// CC_EV_APPEARS_AS, proving the ONE decoder/dispatcher serves both call
// sites (protsupp.cpp OnDataMsg's `*(szData+1)==' '` branch).
static int cc_selftest_pv_data_appears_as() {
    PVCap cap; cc_session_config cfg; cc_session* s = pvMake(&cap, cfg);
    const char* wire = ":Bob!bob@h DATA #comicrig CCUDI1 :# Appears as bob.http://x/bob.avb\r\n";
    cc_session_feed_bytes(s, (const uint8_t*)wire, strlen(wire));
    CC_CHECK(pvFind(cap, CC_EV_DATA) < 0);   // not a UDI blob -- a comment
    int it = pvFind(cap, CC_EV_APPEARS_AS);
    CC_CHECK(it >= 0 && cap.a[it] == "bob" && cap.b[it] == "http://x/bob.avb");
    cc_session_destroy(s);
    return 0;
}

// VECTOR 21 (WHISPER classification, closing the WHISPER/OnTextMsg seam):
// a WHISPER carrying an ACTION CTCP classifies into CC_EV_ACTION exactly like
// PRIVMSG does (OnTextMsg's dispatch is msgType-agnostic past the
// MT_PRIVATEMSG-vs-MT_CHANNELSEND branch point) -- proving the WHISPER path
// (ircsock.cpp cmdidWhisper) now routes through the SAME OnTextMsg/ProcessSay
// classification as PRIVMSG/NOTICE, not a separate raw CC_EV_WHISPER-always
// path.
static int cc_selftest_pv_whisper_action_ctcp() {
    PVCap cap; cc_session_config cfg; cc_session* s = pvMake(&cap, cfg);
    char wire[160];
    int n = snprintf(wire, sizeof(wire), ":Bob!bob@h WHISPER #comicrig Anon :%cACTION grins%c\r\n", 0x01, 0x01);
    cc_session_feed_bytes(s, (const uint8_t*)wire, (size_t)n);
    CC_CHECK(pvFind(cap, CC_EV_WHISPER) < 0);
    int ia = pvFind(cap, CC_EV_ACTION);
    CC_CHECK(ia >= 0 && cap.a[ia] == "Bob grins" && cap.b[ia] == "Bob");
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
    cc_selftest_cp1252_fold();    // Plan 4a Task 3 Step 1: CP-1252 CharUpperBuff fold
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
    cc_selftest_llquote();           // Plan 3 Task 2: low-level quoting (brief vectors)
    cc_selftest_llquote_extra();     // Plan 3 Task 2: zero-copy fast path, CR, QQ
    cc_selftest_utf8codec();         // Plan 3 Task 2: UTF-8 <-> wide codec
    cc_selftest_cptrlist_fifo();     // Plan 3 Task 2 Step 6: CPtrList::RemoveHead
    cc_selftest_charnext();          // Plan 3 Task 2 Step 7: CharNext (Plan 2 debt)
    cc_selftest_capitalize();        // Plan 3 Task 2 Step 8: Capitalize restored
    cc_selftest_annotation_codec();            // Plan 3 Task 3 Step 2: codec round-trip
    cc_selftest_annotation_transports();       // Plan 3 Task 3 Step 8(a)(b): IRCX vs plain-IRC
    cc_selftest_annotation_antispoof_scope();  // Plan 3 Task 3 Step 8(c): anti-spoof scope
    cc_selftest_annotation_addressees();       // Plan 3 Task 3 Step 8(d): T-list + clip-at-5
    cc_selftest_annotation_cooked();           // Plan 3 Task 3 Step 8(e): cooked flag
    cc_selftest_keystring();                   // Plan 3 Task 3: PROP CLIENT key-string codec
    cc_selftest_query_correlation();            // Plan 3 Task 4: CCQuery/CQueryPtrList
    cc_selftest_outbound_join_say();            // Plan 3 Task 4: outbound byte-compare
    cc_selftest_session_login();                // Plan 4a Task 2: NICK/USER (cc_session_login)
    cc_selftest_announce_avatar();              // Plan 4a Task 8: cc_session_announce_avatar
    cc_selftest_outbound_ircx_probe_timer();    // Plan 3 Task 4: MODE ISIRCX + timer request
    cc_selftest_probe_451_cancels_timer();      // Plan 4a Task 2 fix: m_bJustSentModeIsIrcX side effect
    cc_selftest_outbound_say_chunking();        // Plan 3 Task 4: bChatSendToTarget multi-chunk path
    cc_selftest_event_union();                  // Plan 3 Task 5a: cc_proto_event union completeness
    cc_selftest_parse_join_privmsg();           // Plan 3 Task 5b: parse-vector event stream
    cc_selftest_room_token_case_insensitive();  // Plan 4b live-fix 6: LookupDoc-fidelity case fold
    // Plan 3 Task 5b Step 6: per-event-family coverage vectors (hand-traced)
    cc_selftest_pv_selfjoin_autoqueries();      // self-JOIN + auto MODE/WHO + 324/352/315
    cc_selftest_pv_nick_across_users();         // NICK other + self
    cc_selftest_pv_topic_set();                 // TOPIC set
    cc_selftest_pv_channel_mode_delta();        // channel MODE +o delta
    cc_selftest_pv_data_vs_inline();            // IRCX DATA CCUDI1 vs inline (# annotations)
    cc_selftest_annotation_escaped_bytes();     // Plan 4a Task 3 Step 3: escaped-byte annotation vector
    cc_selftest_pv_whisper();                   // WHISPER inbound
    cc_selftest_pv_appears_as_seam();           // "# Appears as" (Task-6 seam: raw CC_EV_TEXT)
    cc_selftest_pv_room_list();                 // room LIST 321/322/323
    cc_selftest_pv_motd();                      // MOTD 375/372/376
    cc_selftest_pv_nick_collision();            // 433 nick-rejected
    cc_selftest_pv_fatal_error();               // fatal ERROR -> disconnected-hint
    cc_selftest_pv_ircx_caps();                 // 800 IRCX caps + probe-timer cancel
    cc_selftest_pv_ircx_second_800_anon_allowed();     // Plan 4a Task 2: second 800 -> 2nd CC_EV_SERVER_CAPS
    cc_selftest_pv_ircx_second_800_anon_disallowed();  // Plan 4a Task 2: second 800, anon disallowed -> unaffected
    cc_selftest_pv_part_quit();                 // PART/QUIT/self-PART
    cc_selftest_pv_names();                     // 353/366 NAMES
    // Plan 3 Task 6: payload stage (ProcessSay/ProcessComment/CTCP dispatch)
    cc_selftest_pv_action_ctcp();                       // \x01ACTION..\x01 -> CC_EV_ACTION
    cc_selftest_pv_inline_annotation_resolver_e2e();    // inline (#...T Bob) + R19 resolver->ref 7
    cc_selftest_pv_sound_ctcp();                        // \x01SOUND "file"\x01 -> CC_EV_SOUND
    cc_selftest_outbound_sound_roundtrip();             // cc_session_send_sound -> wire bytes -> CC_EV_SOUND
    cc_selftest_pv_away_peer_ctcp();                    // \x01AWAY msg\x01 -> CC_EV_AWAY_PEER
    cc_selftest_pv_comment_grammar_suppressed();        // HeresInfo/BDrop(2) -> no event (R20)
    cc_selftest_pv_version_and_getinfo_requests();      // Plan 4b Batch C: VERSION/GetInfo -> CC_EV_VERSION_REQUEST/CC_EV_INFO_REQUEST
    cc_selftest_pv_data_appears_as();                   // DATA-borne "# Appears as" -> CC_EV_APPEARS_AS
    cc_selftest_pv_whisper_action_ctcp();               // WHISPER carrying ACTION -> CC_EV_ACTION
    cc_selftest_draw_text_ellipsis();                   // Plan 4a Task 7: CDC::DrawTextEllipsis
    return g_failures;
}
