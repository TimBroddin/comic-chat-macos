# macOS Port — Plan 1: Engine Foundation & Art Pipeline

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up the `macos/ComicChatKit` SwiftPM package with the MFC-compat shim, lift the original `.avb`/`.bgb` art-format code into the `cchat-engine` C++ target, and expose character/backdrop loading to Swift — proven by golden tests over all 32 files in `comicart/` and a PNG pose export.

**Architecture:** Spec at `docs/superpowers/specs/2026-07-17-macos-port-design.md` (§2–§4). This plan is the first of four (1: engine foundation/art, 2: layout engine + Canvas, 3: protocol, 4: Mac app). Original sources are **copied** from `v2.5-beta-1-modern/` into `Sources/cchat-engine/engine/` and minimally edited per the Edit Rules below; `v2.5-beta-1-modern/` itself is never modified.

**Tech Stack:** SwiftPM (tools 6.0), C++17, Swift 6 + Swift Testing, system `libz`, CoreGraphics (PNG export only in this plan).

## Global Constraints

- All work on branch `macos-port`; repo root is `/Users/timbroddin/Projects/comic-chat`.
- New code lives only under `macos/`; `v2.5-beta-1-modern/` is read-only reference.
- No Win32/MFC headers anywhere under `macos/` — the shim (`mfc_compat.h`) is the only provider of those names.
- The C bridge header `comicchat.h` is pure C (`extern "C"`, no C++ types) and is the **only** interface the Swift targets use.
- `CString` is byte-oriented (spec §4.5) — never widen to UTF-16.
- Lifted engine files keep their original names and as much original code as possible; deviations only via the Edit Rules table.
- Every task ends with `swift test` green (run from `macos/ComicChatKit/`).

## Edit Rules for lifted files (authoritative — applies to every lift task)

When a copied original file fails to compile, fix it **only** in these ways, in this order of preference:

| # | Pattern in original | Required action |
|---|---|---|
| R1 | `#include "stdafx.h"` | Replace with `#include "mfc_compat.h"` |
| R2 | `#include "chat.h"` / `extern CChatApp theApp;` / `theApp.GetAvatarDir()` / `theApp.GetBackDropDir()` | Replace include with `#include "engine_context.h"`; delete the `extern`; calls become `ccContext().avatarDir` / `ccContext().backdropDir` |
| R3 | Windows path building: `"%s\\%s.avb"` etc. | Change `\\\\` separator to `/` in the format string |
| R4 | GDI drawing code — `CDC*` method bodies, `StretchDIBits`, `SRCCOPY`, DC handles | Keep declarations (`class CDC;` is forward-declared in the shim, pointer-only). Wrap bodies in `#ifndef CC_NO_RENDER` … `#else { ASSERT(0); } #endif` and define `CC_NO_RENDER` for this plan. Plan 2 replaces these bodies with `Canvas` calls — do not delete the original code, keep it inside the `#ifndef`. |
| R5 | `<io.h>` / `_findfirst` directory scans | Wrap in `#ifndef CC_NO_DIRSCAN` (defined for this plan). Swift owns directory listing. |
| R6 | Resource loading (`Load(WORD wResid)`), `CArchive`/`Serialize`, `AVATAR_WRITE`-only code | Delete from the lifted copy (write/resource paths are out of scope, spec §1) |
| R7 | `AfxMessageBox`, UI notification calls | Replace with `ccLog("...")` |
| R8 | Includes of UI/doc headers (`chatdoc.h`, `binddoc.h`, `ui.h`, `userinfo.h`, `pageview.h`, `histent.h`) | Delete the include. If a type from them is genuinely needed by model code, forward-declare it and keep usage pointer-opaque; if a code path needs their behavior, `#ifndef CC_NO_UI` it out. |
| R9 | A missing MFC type/member the shim lacks | Add the **minimal** member to `mfc_compat.h`, with a selftest exercising it. Never add speculatively. |
| R10 | Original code needed by a lifted call site or a task-mandated test, but disabled in the original build (`#if 0`, dead `#ifdef`) | Re-enable it **verbatim** (move the definition out of the disabled region, changing nothing else); note it in the report. Added 2026-07-17 during Task 3 for `CDIB::GetNumClrEntries()`. |
| R11 | A whole function in a lifted file serves the UI/session layer (my-avatar selection, screen-name lookup, body-cam refresh, chat-doc access) **or the protocol layer** (annotation byte codecs calling Plan-3 code) and is not required by the load/parse path | Wrap the **entire definition** in `#ifndef CC_NO_UI` (UI/session) or `#ifndef CC_NO_PROTOCOL` (protocol; define added to Package.swift, removed by Plan 3). Exception for vtable completeness: if it overrides a virtual that lifted code instantiates (e.g. `SetSequential`), keep the definition and wrap only the UI-dependent internals, with a safe no-op/failure `#else`. Header declarations stay. List every R11 exclusion individually in the report. Added 2026-07-17 during Task 4 for `avatar.cpp` (`SetMyAvatar` ×2, `GetScreenName`, `RefreshBodyCam`/`RefreshBodyPreview` call sites, `SetSequential` overrides); protocol clause added for `avatario.cpp` `EmotionToBytes`/`BytesToEmotion` (call `IndexToByte`/`ByteToIndex` from `protsupp.cpp`, Plan 3). |
| R12 | A lifted class's member functions (typically virtuals needed for vtable emission) are **defined in files scheduled for a later plan** (`bodycam.cpp`, `panel.cpp`, `wmini.cpp`, `balloon.cpp`, …), producing undefined symbols at link | Two-tier fix, decided per symbol: **(a)** if the load/parse path executes it (smoke test traps in the stub, or code reading shows a load-path call), lift **that single function definition verbatim** from its original file into `engine/lifted_singles.cpp` with a provenance comment (`// lifted verbatim from <file>:<line> — full file comes in Plan N`); **(b)** otherwise add a trap stub (`{ ASSERT(0); }`, or return failure for non-void) to `engine/cc_link_stubs.cpp`, tagged with the owning file. Later plans delete stubs/singles as they lift the owning files. Added 2026-07-17 during Task 4 for `CBody*`/`CPanelElement` virtuals. |
| R13 | Pre-standard MSVC-isms that clang rejects and no compiler flag restores (old `for`-scope variable reuse, temporaries bound to non-const references) | Apply the **minimal standard-conforming rewrite that preserves behavior exactly** (e.g. hoist the loop variable declaration; introduce a named local for the temporary). Each instance individually listed in the report with before/after. The Windows build tolerated these via `/Zc:forScope-` and MSVC permissiveness; clang has no equivalent. Added 2026-07-17 during Task 4 for two instances in `avatar.cpp`. |

Anything not covered above: stop and flag rather than improvise.

---

### Task 1: SwiftPM package scaffold with a C++↔Swift round trip

**Files:**
- Create: `macos/ComicChatKit/Package.swift`
- Create: `macos/ComicChatKit/Sources/cchat-engine/include/comicchat.h`
- Create: `macos/ComicChatKit/Sources/cchat-engine/bridge/engine.cpp`
- Create: `macos/ComicChatKit/Sources/ComicChatKit/Engine.swift`
- Test: `macos/ComicChatKit/Tests/ComicChatKitTests/EngineTests.swift`

**Interfaces:**
- Consumes: nothing (first task).
- Produces: the package layout every later task extends; C bridge header `comicchat.h`; `cc_engine_version(void) -> int32_t`; Swift `Engine.version: Int32`.

- [ ] **Step 1: Write Package.swift**

```swift
// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "ComicChatKit",
    platforms: [.macOS(.v14)],
    products: [
        .library(name: "ComicChatKit", targets: ["ComicChatKit"]),
    ],
    targets: [
        .target(
            name: "cchat-engine",
            path: "Sources/cchat-engine",
            publicHeadersPath: "include",
            cxxSettings: [
                .headerSearchPath("shim"),
                .headerSearchPath("engine"),
                .define("CC_NO_RENDER"),
                .define("CC_NO_DIRSCAN"),
                .define("CC_NO_UI"),
            ],
            linkerSettings: [
                .linkedLibrary("z")
            ]
        ),
        .target(
            name: "ComicChatKit",
            dependencies: ["cchat-engine"],
            path: "Sources/ComicChatKit"
        ),
        .testTarget(
            name: "ComicChatKitTests",
            dependencies: ["ComicChatKit"]
        ),
    ],
    cxxLanguageStandard: .cxx17
)
```

- [ ] **Step 2: Write the failing test**

`Tests/ComicChatKitTests/EngineTests.swift`:

```swift
import Testing
@testable import ComicChatKit

@Test func engineVersionIsOne() {
    #expect(Engine.version == 1)
}
```

- [ ] **Step 3: Run to verify it fails**

Run (from `macos/ComicChatKit/`): `swift test`
Expected: build error — `Engine` not defined.

- [ ] **Step 4: Implement bridge + wrapper**

`Sources/cchat-engine/include/comicchat.h`:

```c
#ifndef COMICCHAT_H
#define COMICCHAT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int32_t cc_engine_version(void);

#ifdef __cplusplus
}
#endif
#endif /* COMICCHAT_H */
```

`Sources/cchat-engine/bridge/engine.cpp`:

```cpp
#include "comicchat.h"

extern "C" int32_t cc_engine_version(void) {
    return 1;
}
```

`Sources/ComicChatKit/Engine.swift`:

```swift
import cchat_engine

public enum Engine {
    public static var version: Int32 { cc_engine_version() }
}
```

(Note: SwiftPM exposes the target as module `cchat_engine` — hyphens become underscores.)

- [ ] **Step 5: Run to verify it passes**

Run: `swift test`
Expected: `Test engineVersionIsOne passed`.

- [ ] **Step 6: Commit**

```bash
git -C /Users/timbroddin/Projects/comic-chat add macos/
git -C /Users/timbroddin/Projects/comic-chat commit -m "macos: scaffold ComicChatKit package with C++ engine target"
```

---

### Task 2: MFC-compat shim + engine context, self-tested from `swift test`

**Files:**
- Create: `macos/ComicChatKit/Sources/cchat-engine/shim/mfc_compat.h`
- Create: `macos/ComicChatKit/Sources/cchat-engine/shim/mfc_compat.cpp`
- Create: `macos/ComicChatKit/Sources/cchat-engine/shim/engine_context.h`
- Create: `macos/ComicChatKit/Sources/cchat-engine/shim/engine_context.cpp`
- Create: `macos/ComicChatKit/Sources/cchat-engine/bridge/cc_selftest.cpp`
- Modify: `macos/ComicChatKit/Sources/cchat-engine/include/comicchat.h`
- Test: `macos/ComicChatKit/Tests/ComicChatKitTests/SelfTests.swift`

**Interfaces:**
- Consumes: package layout from Task 1.
- Produces: every MFC/Win32 name the lift tasks compile against; `ccContext()` / `cc_set_art_dirs(const char*, const char*)`; `ccLog(const char* fmt, ...)`; `cc_run_selftests(void) -> int32_t` (0 = all passed) and the `CC_CHECK` pattern later tasks extend.

C++ unit tests live in `cc_selftest.cpp` behind one C entry point so everything runs under `swift test` — this is the pattern all later engine-side tests follow.

- [ ] **Step 1: Write the failing Swift test**

`Tests/ComicChatKitTests/SelfTests.swift`:

```swift
import Testing
import cchat_engine

@Test func engineSelfTestsPass() {
    #expect(cc_run_selftests() == 0)
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `swift test`
Expected: build error — `cc_run_selftests` not declared.

- [ ] **Step 3: Write the shim header**

`shim/mfc_compat.h` — the complete initial surface (grown later only via rule R9):

```cpp
// mfc_compat.h — minimal MFC/Win32 compatibility layer for lifted engine code.
// macOS port only. Add members ONLY when a lifted file requires them (rule R9),
// and add a selftest for each addition.
#ifndef MFC_COMPAT_H
#define MFC_COMPAT_H

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <string>
#include <vector>

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
#define BI_RGB  0u
#define BI_RLE8 1u
#define BI_RLE4 2u

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
    CString Mid(int i) const { return CString(m_s.substr((size_t)i).c_str()); }
    CString Mid(int i, int n) const { return CString(m_s.substr((size_t)i, (size_t)n).c_str()); }
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
```

`shim/mfc_compat.cpp`:

```cpp
#include "mfc_compat.h"

void ccLog(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
```

- [ ] **Step 4: Write the engine context**

`shim/engine_context.h`:

```cpp
// engine_context.h — replaces `theApp` for lifted code (rule R2).
#ifndef ENGINE_CONTEXT_H
#define ENGINE_CONTEXT_H

#include "mfc_compat.h"

struct CCEngineContext {
    CString avatarDir;    // replaces theApp.GetAvatarDir()
    CString backdropDir;  // replaces theApp.GetBackDropDir()
};

CCEngineContext& ccContext();

#endif
```

`shim/engine_context.cpp`:

```cpp
#include "engine_context.h"
#include "comicchat.h"

CCEngineContext& ccContext() {
    static CCEngineContext ctx;
    return ctx;
}

extern "C" void cc_set_art_dirs(const char* avatarDir, const char* backdropDir) {
    ccContext().avatarDir = avatarDir ? avatarDir : "";
    ccContext().backdropDir = backdropDir ? backdropDir : "";
}
```

Add to `comicchat.h` (inside the `extern "C"` block):

```c
void    cc_set_art_dirs(const char* avatar_dir, const char* backdrop_dir);
int32_t cc_run_selftests(void);
```

- [ ] **Step 5: Write the C++ selftests**

`bridge/cc_selftest.cpp`:

```cpp
#include "comicchat.h"
#include "mfc_compat.h"
#include "engine_context.h"

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

static void testStructSizes() {
    // Wire-format compatibility: these sizes must match Win32 exactly.
    CC_CHECK(sizeof(BITMAPFILEHEADER) == 14);
    CC_CHECK(sizeof(BITMAPINFOHEADER) == 40);
    CC_CHECK(sizeof(RGBQUAD) == 4);
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

extern "C" int32_t cc_run_selftests(void) {
    g_failures = 0;
    testCString();
    testGeometry();
    testColor();
    testStructSizes();
    testCollections();
    testContext();
    return g_failures;
}
```

- [ ] **Step 6: Run to verify it passes**

Run: `swift test`
Expected: both tests pass (`engineVersionIsOne`, `engineSelfTestsPass`).

- [ ] **Step 7: Commit**

```bash
git -C /Users/timbroddin/Projects/comic-chat add macos/
git -C /Users/timbroddin/Projects/comic-chat commit -m "macos: MFC-compat shim, engine context, C++ selftest harness"
```

---

### Task 3: Lift `dib` (DIB parsing without drawing)

> **Plan correction (2026-07-17, during execution):** `memblst.h/.cpp` was
> originally listed here on the mistaken assumption it was a memory-block
> utility. It is actually the member-list UI control
> (`CMemberListCtrl : public CListCtrl`) — UI code that is never lifted.
> `avatar.cpp`'s `#include "memblst.h"` is dead (no symbols used) and is
> deleted under R8 in Task 4.

**Files:**
- Create (copy from `v2.5-beta-1-modern/`, then edit per rules): `engine/dib.h`, `engine/dib.cpp`
- Modify: `Sources/cchat-engine/bridge/cc_selftest.cpp`

**Interfaces:**
- Consumes: shim from Task 2.
- Produces: `CDIB` (create-from-memory, `GetWidth/GetHeight`, `GetBitsAddress`, `GetClrTabAddress`, `GetNumClrEntries`, `Convert8ToNonRLE`, `StorageWidth`), `DIBStorageWidth(UINT,UINT)`, `NumDIBColorEntries(BITMAPINFO*)` — the exact classes `avbfile.cpp` links against in Task 4.

- [ ] **Step 1: Copy the two files**

```bash
cd /Users/timbroddin/Projects/comic-chat
cp v2.5-beta-1-modern/dib.h v2.5-beta-1-modern/dib.cpp \
   macos/ComicChatKit/Sources/cchat-engine/engine/
```

- [ ] **Step 2: Add failing selftests for the DIB helpers**

Append to `cc_selftest.cpp` (and call from `cc_run_selftests`):

```cpp
#include "dib.h"

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
```

- [ ] **Step 3: Run to verify it fails**

Run: `swift test`
Expected: compile failure — `dib.h` still includes `stdafx.h` / uses Win32 names.

- [ ] **Step 4: Apply Edit Rules until green**

Known edits (discovered during planning):
- `dib.cpp`: R1 (`stdafx.h` → `mfc_compat.h`); R4 — the three `CDIB::Draw(CDC*, ...)` overload bodies use `StretchDIBits`; wrap each body per R4; R6 — delete `Load(WORD wResid)` and both `Save` overloads (write path out of scope).
- `dib.h`: delete the `Load`/`Save` declarations removed above; keep everything else byte-identical.
Run: `swift test` after each edit round until: PASS (all selftests green).

- [ ] **Step 5: Commit**

```bash
git -C /Users/timbroddin/Projects/comic-chat add macos/
git -C /Users/timbroddin/Projects/comic-chat commit -m "macos: lift dib (parse-only, draw bodies stubbed per R4)"
```

---

### Task 4: Lift the avatar loading chain (`avbfile`, `avatario`, `avatar` + headers)

**Files:**
- Create (copies, then Edit Rules): `engine/bbox.h`, `engine/vector2d.h`, `engine/vector2d.cpp`, `engine/pe.h`, `engine/avatar.h`, `engine/avatar.cpp`, `engine/avatario.h`, `engine/avatario.cpp`, `engine/avbfile.h`, `engine/avbfile.cpp`, `engine/backdrop.h`
- *(Correction 2026-07-17: `backdrop.h` moved here from Task 5 — `avbfile.cpp` includes it and implements `CChatBackdrop::LoadBackdrop/LoadFromBmp/Load` in its trailing section, which stays in `avbfile.cpp` as original code. Task 5 lifts only `backdrop.cpp`.)*
- Modify: `bridge/cc_selftest.cpp`, `include/comicchat.h`, `bridge/engine.cpp`

**Interfaces:**
- Consumes: `CDIB`, shim, `ccContext()`.
- Produces: working `.avb` load path (original classes `CAvatarFileStream`, `CAvatarDIB`, `CAvatarX`/`CAvatarSimple`/`CAvatarComplex`, `CPose`); temporary C hook `cc_smoke_load_avatar(const char* path, char* name_out, size_t name_cap, int32_t* pose_count_out) -> int32_t` (0 on success) used by tests until Task 6 replaces it with the real API.

- [ ] **Step 1: Copy the ten files** (same `cp` pattern as Task 3, from `v2.5-beta-1-modern/`).

- [ ] **Step 2: Write the failing smoke test**

Append to `Tests/ComicChatKitTests/SelfTests.swift`:

```swift
@Test func smokeLoadAnna() throws {
    // comicart/ lives at the repo root; locate it relative to this source file.
    let repoRoot = URL(fileURLWithPath: #filePath)
        .deletingLastPathComponent()  // ComicChatKitTests
        .deletingLastPathComponent()  // Tests
        .deletingLastPathComponent()  // ComicChatKit
        .deletingLastPathComponent()  // macos
    let anna = repoRoot.appendingPathComponent("v2.5-beta-1-modern/comicart/anna.avb").path
    var name = [CChar](repeating: 0, count: 256)
    var poses: Int32 = 0
    let rc = cc_smoke_load_avatar(anna, &name, 256, &poses)
    #expect(rc == 0)
    #expect(String(cString: name).isEmpty == false)
    #expect(poses > 0)
}
```

(`import Foundation` at top of the file if not present.)

- [ ] **Step 3: Run to verify it fails**

Run: `swift test`
Expected: compile failure (`cc_smoke_load_avatar` undeclared) — then, after Step 4 begins, engine compile errors.

- [ ] **Step 4: Apply Edit Rules until the engine compiles**

Known edits (discovered during planning):
- `avbfile.cpp`, `avatario.cpp`: R1; R2 (`chat.h`/`theApp.GetAvatarDir()` → `engine_context.h`/`ccContext().avatarDir` — e.g. `avatario.cpp:20` `path.Format("%s\\%s.avb", ...)` also needs R3); zlib — keep the `ZLIB` namespace declarations in `avbfile.h` as-is; system `libz` (already linked in Task 1) provides `compress`/`compress2`/`uncompress` with exactly those signatures.
- `avatar.cpp`: R1; R8 (drop `binddoc.h`, `chatdoc.h`, `ui.h`, `userinfo.h`, `chat.h`, `chatprot.h`, and `memblst.h` includes — `memblst.h` is the member-list UI control and no symbol from it is used in `avatar.cpp`); R5 (`<io.h>` avatar-file scan); R4 (any `CBody::Draw`/GDI bodies); R9 for stragglers.
- `avatar.h`, `avbfile.h`, `pe.h`: keep byte-identical if possible; `pe.h`'s `virtual void Draw(CDC*, POINT*, RECT*) = 0` compiles against the shim's forward-declared `CDC`.
- `AVATAR_READ` is already the only mode enabled in `avbfile.h` (`AVATAR_WRITE` off) — leave as-is; R6 applies to any write-path stragglers.

Compile loop: `swift build` after each round; classify every remaining error against R1–R9.

- [ ] **Step 5: Implement the smoke hook**

Add to `comicchat.h`:

```c
int32_t cc_smoke_load_avatar(const char* path, char* name_out, size_t name_cap,
                             int32_t* pose_count_out);
```

In `bridge/engine.cpp`, implement it with the original loading entry point: construct a `CAvatarFileStream` on the given path and call `CAvatarX::LoadAvatar(pStream)` — exactly what `LoadAvatarInfo()` does at `avatario.cpp:17-28`, minus the dir+name path formatting (our hook receives a full path). Copy the avatar's name into `name_out`, count poses into `pose_count_out`, delete the object, return 0; non-zero on any failure.

- [ ] **Step 6: Run to verify it passes**

Run: `swift test`
Expected: `smokeLoadAnna` passes with a non-empty name and `poses > 0`.

- [ ] **Step 7: Commit**

```bash
git -C /Users/timbroddin/Projects/comic-chat add macos/
git -C /Users/timbroddin/Projects/comic-chat commit -m "macos: lift avbfile/avatario/avatar - anna.avb loads via original code"
```

---

### Task 5: Lift `backdrop` (`.bgb` loading)

**Files:**
- Create (copies, then Edit Rules): `engine/backdrop.cpp` (`backdrop.h` already lifted in Task 4; `CChatBackdrop`'s `Load*` methods already came with `avbfile.cpp`)
- Modify: `bridge/engine.cpp`, `include/comicchat.h`
- Test: `Tests/ComicChatKitTests/SelfTests.swift`

**Interfaces:**
- Consumes: everything from Tasks 2–4.
- Produces: `.bgb` load path; `cc_smoke_load_backdrop(const char* path, char* name_out, size_t name_cap, int32_t* width_out, int32_t* height_out) -> int32_t`.

- [ ] **Step 1: Write the failing test**

```swift
@Test func smokeLoadFieldBackdrop() throws {
    // 5 deletions: SelfTests.swift → ComicChatKitTests → Tests → ComicChatKit → macos → repo root
    // (a 4-deletion version of this landed at macos/ — bug found during Task 4)
    let repoRoot = URL(fileURLWithPath: #filePath)
        .deletingLastPathComponent().deletingLastPathComponent()
        .deletingLastPathComponent().deletingLastPathComponent()
        .deletingLastPathComponent()
    let field = repoRoot.appendingPathComponent("v2.5-beta-1-modern/comicart/field.bgb").path
    var name = [CChar](repeating: 0, count: 256)
    var w: Int32 = 0, h: Int32 = 0
    #expect(cc_smoke_load_backdrop(field, &name, 256, &w, &h) == 0)
    #expect(w > 0 && h > 0)
}
```

- [ ] **Step 2: Run to verify it fails** — `swift test`, expected: `cc_smoke_load_backdrop` undeclared.

- [ ] **Step 3: Lift and edit `backdrop.*`**

Known edits: R1; R2 (`theApp.GetBackDropDir()` at `backdrop.cpp:59,125,137` → `ccContext().backdropDir`, plus R3 on those `Format` strings); R5 (`<io.h>` scan); R8 (`binddoc.h`, `chatdoc.h`, `ui.h`, `userinfo.h`, `histent.h`); R4 (draw bodies). Declare + implement `cc_smoke_load_backdrop` alongside the avatar hook.

- [ ] **Step 4: Run to verify it passes** — `swift test`, all green.

- [ ] **Step 5: Commit**

```bash
git -C /Users/timbroddin/Projects/comic-chat add macos/
git -C /Users/timbroddin/Projects/comic-chat commit -m "macos: lift backdrop - field.bgb loads via original code"
```

---

### Task 6: Real C bridge art API + RGBA decode + Swift wrappers

**Files:**
- Create: `Sources/cchat-engine/bridge/bridge_art.cpp`
- Modify: `include/comicchat.h`, `bridge/engine.cpp` (delete both `cc_smoke_*` hooks)
- Create: `Sources/ComicChatKit/ArtFile.swift`
- Create: `Tests/ComicChatKitTests/Fixtures/anna.avb`, `Fixtures/field.bgb` (copied from `comicart/`)
- Modify: `Package.swift` (test resources), `Tests/ComicChatKitTests/ArtTests.swift`

**Interfaces:**
- Consumes: load paths from Tasks 4–5.
- Produces — the permanent art API (Plan 2 and the app build on exactly these):

```c
typedef struct cc_image { int32_t width; int32_t height; uint8_t* rgba; } cc_image;
void cc_image_free(cc_image* img);

typedef struct cc_avatar cc_avatar;
cc_avatar*  cc_avatar_open(const char* path);
void        cc_avatar_close(cc_avatar* av);
const char* cc_avatar_name(const cc_avatar* av);
int32_t     cc_avatar_pose_count(const cc_avatar* av);
const char* cc_avatar_pose_name(const cc_avatar* av, int32_t idx);
int32_t     cc_avatar_pose_image(const cc_avatar* av, int32_t idx, cc_image* out); /* 0 = ok */

typedef struct cc_backdrop cc_backdrop;
cc_backdrop* cc_backdrop_open(const char* path);
void         cc_backdrop_close(cc_backdrop* bd);
const char*  cc_backdrop_name(const cc_backdrop* bd);
int32_t      cc_backdrop_image(const cc_backdrop* bd, cc_image* out);
```

Swift: `public struct ArtImage { let width: Int; let height: Int; let rgba: Data }`, `public final class AvatarFile` (`init(path:) throws`, `name`, `poseCount`, `poseName(_:)`, `poseImage(_:) throws -> ArtImage`), `public final class BackdropFile` (`init(path:) throws`, `name`, `image() throws -> ArtImage`).

- [ ] **Step 1: Add fixtures + test resources**

```bash
cp v2.5-beta-1-modern/comicart/anna.avb v2.5-beta-1-modern/comicart/field.bgb \
   macos/ComicChatKit/Tests/ComicChatKitTests/Fixtures/
```

In `Package.swift`, the test target gains: `resources: [.copy("Fixtures")]`.

- [ ] **Step 2: Write the failing tests**

`Tests/ComicChatKitTests/ArtTests.swift`:

```swift
import Testing
import Foundation
@testable import ComicChatKit

// Internal (not private): PNGExportTests.swift (Task 8) uses this helper too.
func fixture(_ name: String) -> String {
    Bundle.module.url(forResource: name, withExtension: nil, subdirectory: "Fixtures")!.path
}

@Test func avatarOpensAndDecodes() throws {
    let anna = try AvatarFile(path: fixture("anna.avb"))
    #expect(!anna.name.isEmpty)
    #expect(anna.poseCount > 0)
    #expect(!anna.poseName(0).isEmpty)
    let img = try anna.poseImage(0)
    #expect(img.width > 0 && img.height > 0)
    #expect(img.rgba.count == img.width * img.height * 4)
    // Transparency: at least one pixel must be fully transparent (avatars are
    // sprites on a transparent field) and at least one fully opaque.
    let alphas = stride(from: 3, to: img.rgba.count, by: 4).map { img.rgba[$0] }
    #expect(alphas.contains(0))
    #expect(alphas.contains(255))
}

@Test func backdropOpensAndDecodes() throws {
    let field = try BackdropFile(path: fixture("field.bgb"))
    let img = try field.image()
    #expect(img.width > 0 && img.height > 0)
    #expect(img.rgba.count == img.width * img.height * 4)
}

@Test func openMissingFileThrows() {
    #expect(throws: (any Error).self) { try AvatarFile(path: "/nonexistent.avb") }
}
```

- [ ] **Step 3: Run to verify it fails** — `swift test`, expected: `AvatarFile` not defined.

- [ ] **Step 4: Implement `bridge_art.cpp`**

Implementation notes (bound during Tasks 4–5, applied here):
- `cc_avatar` / `cc_backdrop` are structs owning the original parsed objects.
- RGBA decode: run `ConvertToNonRLE()` if needed, then walk 8-bit (or 4-bit) indexed rows bottom-up (DIBs are bottom-up when `biHeight > 0`), mapping palette entries via `GetClrTabAddress()`. The transparent index/color comes from `CAvatarDIB`'s palette data (`CAvatarPalette`, `avbfile.h:351`) — read how the original draw path selects the transparent color and reuse that exact rule; transparent pixels get alpha 0, all others 255.
- Errors: return NULL/nonzero; never throw across the bridge (spec §7).

`Sources/ComicChatKit/ArtFile.swift` wraps the C API; `deinit` calls the close/free functions; failures throw `ComicChatError.artLoadFailed(path:)` — define `public enum ComicChatError: Error` in this file, it grows in later plans.

- [ ] **Step 5: Run to verify it passes** — `swift test`, all green (smoke tests from Tasks 4–5 now deleted along with their hooks).

- [ ] **Step 6: Commit**

```bash
git -C /Users/timbroddin/Projects/comic-chat add macos/
git -C /Users/timbroddin/Projects/comic-chat commit -m "macos: permanent C art API with RGBA decode + Swift ArtFile wrappers"
```

---

### Task 7: `cc-dumpart` tool + golden catalog over all 32 art files

**Files:**
- Create: `Sources/cc-dumpart/main.swift` (+ executable target in `Package.swift`, product `cc-dumpart`, depends on `ComicChatKit`)
- Create: `Tests/ComicChatKitTests/Fixtures/comicart-catalog.json` (generated then committed)
- Test: `Tests/ComicChatKitTests/GoldenCatalogTests.swift`

**Interfaces:**
- Consumes: `AvatarFile`, `BackdropFile`.
- Produces: `swift run cc-dumpart <dir>` → JSON catalog on stdout; the committed golden file; `CatalogEntry` + `buildCatalog(artDir:)` in `Sources/ComicChatKit/Catalog.swift` (single definition shared by tool and test).

- [ ] **Step 1: Write the failing golden test**

```swift
import Testing
import Foundation
@testable import ComicChatKit

// CatalogEntry and buildCatalog(artDir:) live in Sources/ComicChatKit/Catalog.swift
// (single definition, shared by this test and cc-dumpart):
//
// public struct CatalogEntry: Codable, Equatable {
//     public let file: String
//     public let kind: String          // "avatar" | "backdrop"
//     public let name: String
//     public let poseCount: Int        // 0 for backdrops
//     public let poseNames: [String]
//     public let imageCRCs: [UInt32]   // crc32 of RGBA bytes per pose (or the single backdrop image)
// }
// public func buildCatalog(artDir: String) throws -> [CatalogEntry]

@Test func comicartMatchesGoldenCatalog() throws {
    // 5 deletions to reach the repo root (see Task 5 note)
    let repoRoot = URL(fileURLWithPath: #filePath)
        .deletingLastPathComponent().deletingLastPathComponent()
        .deletingLastPathComponent().deletingLastPathComponent()
        .deletingLastPathComponent()
    let artDir = ProcessInfo.processInfo.environment["CC_COMICART_DIR"]
        ?? repoRoot.appendingPathComponent("v2.5-beta-1-modern/comicart").path
    let golden = try JSONDecoder().decode([CatalogEntry].self, from: Data(contentsOf:
        Bundle.module.url(forResource: "comicart-catalog", withExtension: "json", subdirectory: "Fixtures")!))
    let built = try buildCatalog(artDir: artDir)   // same walk cc-dumpart uses
    #expect(built.count == 32)
    #expect(built == golden)
}
```

`buildCatalog(artDir:)` lives in `ComicChatKit` (new file `Sources/ComicChatKit/Catalog.swift`) so tool and test share it: enumerate `*.avb`/`*.bgb` sorted by filename, open each with `AvatarFile`/`BackdropFile`, compute crc32 (use `zlib`'s `crc32` via the existing C target — add `uint32_t cc_crc32(const uint8_t* data, size_t len);` to the bridge) over each pose/backdrop RGBA.

- [ ] **Step 2: Run to verify it fails** — missing golden file / `buildCatalog` undefined.

- [ ] **Step 3: Implement `Catalog.swift`, `cc-dumpart`, generate the golden**

`cc-dumpart/main.swift`: parse `CommandLine.arguments[1]` as the art dir, `buildCatalog`, print pretty JSON. Generate:

```bash
swift run cc-dumpart ../../v2.5-beta-1-modern/comicart \
  > Tests/ComicChatKitTests/Fixtures/comicart-catalog.json
```

**Verification of the golden (manual, once):** open the JSON and check the roster against the known shipped cast (anna, armando, bolo, buck, connor, cro, dan, denise, glenda, hugh, jordan, kirby, lance, lynnea, margaret, mike, … 32 files total) and that every avatar has plausible pose counts and non-empty pose names. Record the check in the commit message. (Spec §8 calls for Windows-build ground truth; the Windows build is not runnable on this machine, so the golden locks in *current* parser output after human spot-check — cross-checking against the Windows client happens in Plan 4's manual acceptance.)

> **Corrected during execution + final review (2026-07-17):** the original ">= 5
> poses per avatar" sanity bar here was written blind and is wrong. Actual
> distribution: 20 avatars with 14–30+ poses, and **5 single-pose minimal
> avatars** (glenda, pedagog, rainbow, tux, waf — their `AK_NBODIES` tag counts
> 1 body; verified against the format, and tux renders correctly). Task 7's
> commit message repeats the wrong ">=5" claim; this note is the accurate
> record. These 5 files are flagged for Plan 4's Windows cross-check.

- [ ] **Step 4: Run to verify it passes** — `swift test`: catalog test green over all 32 files.

- [ ] **Step 5: Commit**

```bash
git -C /Users/timbroddin/Projects/comic-chat add macos/
git -C /Users/timbroddin/Projects/comic-chat commit -m "macos: golden catalog test over all comicart files + cc-dumpart tool

Golden spot-checked by hand: 32 files, all avatars have >=5 named poses."
```

---

### Task 8: PNG pose export (first visible pixels)

**Files:**
- Create: `Sources/cc-dumpart/PNGExport.swift`
- Modify: `Sources/cc-dumpart/main.swift`
- Test: `Tests/ComicChatKitTests/PNGExportTests.swift` (move the CGImage helper into `Sources/ComicChatKit/ArtImage+CGImage.swift` so both use it)

**Interfaces:**
- Consumes: `ArtImage`.
- Produces: `extension ArtImage { public func cgImage() -> CGImage? }` in ComicChatKit (Plan 2's canvas work and Plan 4's pickers reuse this); `cc-dumpart --png <file.avb> <poseIndex> <out.png>`.

- [ ] **Step 1: Write the failing test**

```swift
import Testing
import Foundation
import CoreGraphics
@testable import ComicChatKit

@Test func poseConvertsToCGImage() throws {
    let anna = try AvatarFile(path: fixture("anna.avb"))
    let img = try anna.poseImage(0)
    let cg = img.cgImage()
    #expect(cg != nil)
    #expect(cg!.width == img.width && cg!.height == img.height)
    #expect(cg!.alphaInfo == .premultipliedLast || cg!.alphaInfo == .last)
}
```

- [ ] **Step 2: Run to verify it fails** — `cgImage()` undefined.

- [ ] **Step 3: Implement**

`Sources/ComicChatKit/ArtImage+CGImage.swift`:

```swift
import CoreGraphics
import Foundation

extension ArtImage {
    public func cgImage() -> CGImage? {
        guard let provider = CGDataProvider(data: rgba as CFData) else { return nil }
        return CGImage(
            width: width, height: height,
            bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: width * 4,
            space: CGColorSpace(name: CGColorSpace.sRGB)!,
            bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.last.rawValue),
            provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
    }
}
```

`PNGExport.swift`: `CGImageDestinationCreateWithURL(url, UTType.png.identifier as CFString, 1, nil)` + add image + finalize (`import ImageIO`, `import UniformTypeIdentifiers`). Wire `--png` mode into `main.swift`.

- [ ] **Step 4: Run to verify it passes** — `swift test` green. Then the human check:

```bash
swift run cc-dumpart --png ../../v2.5-beta-1-modern/comicart/anna.avb 0 /tmp/anna-pose0.png && open /tmp/anna-pose0.png
```

Expected: Anna's first pose renders correctly — right colors, transparent background, not flipped vertically (a classic DIB bottom-up bug; if she's upside down, the row walk in `bridge_art.cpp` is inverted).

- [ ] **Step 5: Commit**

```bash
git -C /Users/timbroddin/Projects/comic-chat add macos/
git -C /Users/timbroddin/Projects/comic-chat commit -m "macos: CGImage conversion + PNG pose export - first visible pixels"
```

---

## Plan 1 exit criteria

- `swift test` green from `macos/ComicChatKit/`: shim selftests, DIB tests, art API tests, golden catalog over all 32 `comicart/` files.
- `swift run cc-dumpart --png` produces a visually correct pose PNG (human-checked).
- No Win32 headers under `macos/`; `v2.5-beta-1-modern/` untouched (`git status` clean there).

**Follow-on plans (written after this one lands):** Plan 2 — layout engine + Canvas (lifts `balloon`, `spline`, `splinutl`, `traj`, `bodycam`, `semantic`, `panel`/`pe` draw paths; replaces the R4 `#ifndef CC_NO_RENDER` bodies with Canvas calls; recording-canvas snapshot tests). Plan 3 — protocol (lifts `ircproto` + annotation codec from `format.cpp`/`protsupp.cpp`; bytes-in/events-out bridge; transcript replay tests). Plan 4 — the Mac app (Xcode project, all UI, sounds, save/print, manual interop acceptance).
