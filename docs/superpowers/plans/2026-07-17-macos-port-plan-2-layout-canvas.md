# macOS Port — Plan 2: Comic Layout Engine + Canvas

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Lift the original comic layout engine (balloons, splines, panels, body camera, text→pose) into `cchat-engine`, reroute all drawing/measurement through an abstract `Canvas` boundary, and prove it with the exit milestone: a scripted two-avatar conversation rendered to PNG via a CoreGraphics canvas, with recording-canvas snapshot tests green.

**Architecture:** Spec `docs/superpowers/specs/2026-07-17-macos-port-design.md` §4.3 binds the Canvas boundary (drawImage, fillPath/strokePath, drawText/measureText; measurement feeds back into layout; resolution-independent). This plan realizes it as: a pure-C `cc_canvas` function-pointer vtable in `comicchat.h` (the only thing Swift sees) → a thin C++ `CCanvas` wrapper → a concrete shim `CDC` **adapter** that implements the GDI methods the lifted code already calls, so lifted call sites stay textually identical (maximum fidelity, per the Edit Rules). Three canvas implementations: CoreGraphics/CoreText (Swift), a Swift recording canvas (snapshot tests), and a C++ recording canvas with deterministic fake metrics (engine selftests).

**Tech Stack:** SwiftPM (tools 6.0), C++17, Swift 6 + Swift Testing, CoreGraphics/CoreText/ImageIO.

**Discovery record:** `docs/superpowers/plans/2026-07-17-plan2-discovery/` (canvas-inventory.md, text-metrics.md, composition-map.md — committed copies of the discovery reports; cited as `[CI]`, `[TM]`, `[CM]` below). Key corrections vs the roadmap, all verified against `chat.mak`: `semantic.cpp`, `wmini.cpp`, `script.cpp/.h` are **dead code** (not in the original build) and are NOT lifted; the real text→pose engine is **`textpose.cpp`**; the body-placement "camera" lives in **`panel.cpp`**, not `bodycam.cpp`; `bodycam.cpp` = the `CBodyCam:CWnd` emotion widget (excluded, R11) + the needed `CBody*` draw/bbox methods.

## Global Constraints

- All work on branch `macos-port`; repo root is `/Users/timbroddin/Projects/comic-chat`.
- New code lives only under `macos/`; `v2.5-beta-1-modern/` is read-only reference. **Plan 2 addition:** `artifacts/inc/format.h` is also read-only lift source (the original build's `ARTINC` include dir; `format.h` does not exist in `v2.5-beta-1-modern/`).
- No Win32/MFC headers anywhere under `macos/` — the shim (`mfc_compat.h`) is the only provider of those names.
- The C bridge header `comicchat.h` is pure C (`extern "C"`, no C++ types) and is the **only** interface the Swift targets use.
- `CString` is byte-oriented (spec §4.5) — never widen to UTF-16. Canvas text parameters are raw bytes (CP-1252 by default); only the Swift canvas implementations convert for display.
- Lifted engine files keep their original names and as much original code as possible; deviations only via the Edit Rules table.
- **Units & coordinates:** the engine works in MM_TWIPS logical space exactly as the original (1 twip = 1/1440 inch, **y-up**: page content grows toward negative y). The Canvas vtable receives twips unchanged; implementations convert (CoreGraphics: 20 twips = 1 point). The adapter `CDC` reports `GetDeviceCaps(LOGPIXELSX/Y) = 1440` so original `MulDiv(pt, LOGPIXELSY, 72)` arithmetic yields twips.
- Every task ends with `swift test` green (run from `macos/ComicChatKit/`).
- Swift test files resolve the repo root with FIVE `deletingLastPathComponent()` calls from `#filePath` (established Plan 1 pattern — count them).
- Commits are SSH-signed via 1Password; if `ssh-add -l` shows no identities, ask Tim to unlock — never bypass signing.

## Edit Rules for lifted files (authoritative — applies to every lift task)

Rules R1–R13 are carried forward from Plan 1 **verbatim and unmodified** (see `2026-07-17-macos-port-plan-1-engine-foundation.md` §Edit Rules for the full R1–R13 table text; it remains binding here). Summary of the carried rules: R1 `stdafx.h`→`mfc_compat.h` · R2 `chat.h`/`theApp` → `engine_context.h`/`ccContext()` · R3 `\\` path separators → `/` · R4 GDI bodies wrapped `#ifndef CC_NO_RENDER` (Plan 1 scaffolding; this plan retires it, see R14) · R5 `<io.h>`/`_findfirst` → `CC_NO_DIRSCAN` · R6 delete resource-load/`CArchive`/write-path code · R7 `AfxMessageBox`→`ccLog` · R8 delete UI/doc header includes, forward-declare pointer-opaque types · R9 minimal shim additions only, each with a selftest · R10 re-enable originally-disabled code verbatim when needed · R11 whole-function `#ifndef CC_NO_UI` / `#ifndef CC_NO_PROTOCOL` wraps (vtable-completeness exception) · R12 two-tier fix for symbols owned by later-plan files (a: verbatim single lift / b: trap stub) · R13 minimal standard-conforming rewrite of pre-standard MSVC-isms, each listed.

New rules added by this plan:

| # | Pattern in original | Required action |
|---|---|---|
| R14 | GDI drawing/measurement call sites in lifted code (the `Canvas` reroute) | The shim provides a **concrete `CDC` adapter** over `cc_canvas`; GDI call sites stay **textually unchanged** wherever the adapter implements the method (adapter methods are added under R9 discipline: minimal, each with a selftest). Plan-1 `#ifndef CC_NO_RENDER` wrappers are removed (define deleted from Package.swift in Task 7) — removal restores original code and is not an edit. GDI idioms with **no faithful adapter mapping** get a documented per-instance transformation, each individually listed in the task report: (i) mask ROP pairs (`SetROP2`+`BitBlt` AND/PAINT sequences) → a single `drawImage` with alpha (the RGBA decode already honors the original mask/transparent-color handling, Plan 1); (ii) palette calls (`GetCurrentPalette`/`SelectPalette`/`RealizePalette`) → no-op shim methods (RGBA end-to-end); (iii) `CreateCompatibleDC`/retained-bitmap machinery → not ported (see R16); (iv) `Ellipse` → adapter-internal 4-cubic Bézier conversion (kappa 0.5522847498) so the vtable stays path-based; (v) MERGEPAINT-alone aura/nimbus whitening (`dest = ~aura \| dest` where the aura plane is black-on-silhouette) → decode the aura plane to RGBA with RGB forced white and alpha = aura-bit, then one `drawImage` — pixel-exact for the halo ring and background no-op (amendment 2026-07-17, Task 7 review). |
| R15 | Undefined behavior in original code confirmed by review (delete through base w/o virtual dtor; use-after-free on re-init) | Apply the minimal fix, individually listed in the report with a comment citing this rule. Instances: (1) `virtual ~CAvatarStream() {}` at `avbfile.h:283` (Task 1); (2) `list.RemoveAll()` in `textpose.cpp` `DestroyEmotionList` (Task 10 amendment 2026-07-17 — the original frees each `STRINGUNIT` but leaves dangling pointers in the static rule lists; benign under the original's init-once lifetime, UB when `cc_strip` re-runs init after destroy; no bridge-side alternative exists since the lists are file-statics). Future instances require a plan amendment naming them. |
| R16 | A function welded to the MFC view layer that headless composition must replace (this plan: only `CUnitPanelPage::Draw`, panel.cpp:1193 — retained-DIB + `CPageView::GetRetSec`/`AccountForScroll` + `BitBlt` scroll machinery `[CM §4]`) | Do **not** port the function (wrap the lifted definition whole in `#ifndef CC_NO_UI`, R11). Write a replacement in **bridge code** (never in a lifted file) that transliterates the original's arithmetic, citing file:line of every borrowed expression. This plan: `cc_compose.cpp` reuses the panel-origin walk of panel.cpp:1232–1255 and `GetBBox` row/column math of panel.cpp:1268–1276. |
| R17 | Layout code reaching UI/doc singletons for **data the headless engine must supply** (`GetClientDC()` text metrics; `GetChatDoc()->GetBackDropID()/GetComicsTitle()/m_bComicView`; `theApp.m_comicsColor`/`m_charSet`; per-user `CUserInfo`/`m_udi.m_talkTos` lookups; `UpdateViewsX`/`RefreshPanelN` view pokes) | Reroute through `ccContext()` (extending `engine_context.h` minimally, R9-style, each addition selftested): metrics via `ccContext().metricsDC()` (a `CDC` bound to the registered metrics canvas); doc/settings via `ccContext().session` fields; view pokes become no-ops behind `#ifndef CC_NO_UI` (R11) when the whole function is view-serving, else a one-line `ccContext()` no-op hook. Every rerouted site individually listed in the report. |

Anything not covered above: stop and flag rather than improvise.

## Debt retired / created by this plan

- **Retires (from Plan 1):** all 13 trap stubs in `engine/cc_link_stubs.cpp` — Task 6 deletes 2 (`CPanelElement` copy-ctor + `GetBBox`, owned by balloon.cpp:641/647), Task 7 deletes 10 (`CBodySingle`/`CBodyDouble`, owned by bodycam.cpp), Task 8 deletes the last (`CPanelElement::SetBBox`, panel.cpp:542) **and deletes the now-empty file**. `CC_NO_RENDER` define removed (Task 7). Entry debt from the Plan 1 final review: `CAvatarStream` vdtor, tautological-compare warning flag, `ccLog` quiet mode (Task 1).
- **Keeps (Plan 3 removes):** `CC_NO_PROTOCOL` define; `format.cpp`'s protocol-side annotation codec remains behind existing wraps (this plan lifts format.cpp for its formatting/measurement half — a bonus for Plan 3: the codec/UI split feared in spec §9 turned out clean, see `[TM]`).
- **Not lifted (dead code, verified not in `chat.mak`):** `semantic.cpp`, `wmini.cpp`, `script.cpp/.h`. `pageview.cpp/.h` is not lifted (view layer; only its arithmetic is transliterated per R16).

## File structure

```
macos/ComicChatKit/
  Sources/cchat-engine/
    include/comicchat.h        MODIFY  cc_canvas vtable, cc_font_spec, strip API, cc_set_log_level
    bridge/cc_canvas.h         CREATE  C++ CCanvas wrapper over cc_canvas (header-only)
    bridge/cc_recording_canvas.h/.cpp  CREATE  C++ recording canvas w/ deterministic metrics (selftests)
    bridge/cc_compose.cpp      CREATE  cc_strip session + headless compositor (R16 replacement)
    bridge/cc_selftest.cpp     MODIFY  per-task selftests
    shim/mfc_compat.h/.cpp     MODIFY  CDC adapter, CFont/LOGFONT/TEXTMETRIC, CPen, CBrush, log level
    engine/                    LIFT    defines.h, spline.h/.cpp, splinutl.cpp, traj.h/.cpp,
                                       format.h (from artifacts-modern/inc), format.cpp, userinfo.h,
                                       balloon.h/.cpp, fonts.cpp, bodycam.h/.cpp, panel.h/.cpp, textpose.cpp
    engine/cc_link_stubs.cpp   DELETE  (emptied across Tasks 6–8)
  Sources/ComicChatKit/
    Canvas.swift               CREATE  CanvasProtocol + cc_canvas bridging box
    RecordingCanvas.swift      CREATE  Swift recording canvas (snapshot tests)
    CGCanvas.swift             CREATE  CoreGraphics/CoreText canvas + PNG export
    Strip.swift                CREATE  typed wrapper over cc_strip API
  Sources/cc-dumpart/main.swift MODIFY --strip mode (script JSON → PNG)
  Tests/ComicChatKitTests/     MODIFY  snapshot + exit-milestone tests, Fixtures/strip golden
```

Original line counts for lift sizing: balloon.cpp 1935 / balloon.h 208 / panel.cpp 1583 / panel.h 154 / bodycam.cpp 1177 / bodycam.h 104 / textpose.cpp 334 / format.cpp 1507 / format.h 68 / fonts.cpp 173 / userinfo.h 167 / spline.cpp 320+h 57 / splinutl.cpp 297 / traj.cpp 107+h 54 / defines.h (check at lift).

---

### Task 1: Entry debt — CAvatarStream vdtor, warning silencer, ccLog levels

**Files:**
- Modify: `macos/ComicChatKit/Sources/cchat-engine/engine/avbfile.h:283`
- Modify: `macos/ComicChatKit/Package.swift`
- Modify: `macos/ComicChatKit/Sources/cchat-engine/shim/mfc_compat.h`, `shim/mfc_compat.cpp`
- Modify: `macos/ComicChatKit/Sources/cchat-engine/include/comicchat.h`
- Modify: `macos/ComicChatKit/Sources/cchat-engine/bridge/cc_selftest.cpp`

**Interfaces:**
- Consumes: existing `ccLog` (mfc_compat.cpp), existing selftest harness `cc_run_selftests()`/`CC_CHECK`.
- Produces: `void cc_set_log_level(int32_t level)` in comicchat.h (0=silent, 1=errors [ASSERT/VERIFY failures], 2=trace; default 2; env var `CC_LOG_LEVEL` read once at first log). Later tasks and tests rely on `cc_set_log_level(0)`.

- [ ] **Step 1: Write failing selftest for log levels** — in `cc_selftest.cpp`, add `cc_selftest_loglevel()`: capture that `ccLogWouldEmit(2)` is TRUE by default, FALSE after `cc_set_log_level(1)`, TRUE again after `cc_set_log_level(2)`. Expose a tiny query `int ccLogWouldEmit(int level)` from mfc_compat (declared in mfc_compat.h) so the test asserts the gate, not stdout. Register in `cc_run_selftests()`.
- [ ] **Step 2: Run to verify failure** — `swift test 2>&1 | tail -5` from `macos/ComicChatKit/`. Expected: compile FAIL (`cc_set_log_level`/`ccLogWouldEmit` undeclared).
- [ ] **Step 3: Implement** — in `mfc_compat.cpp`: static `int g_ccLogLevel = -1;` lazily initialized from `getenv("CC_LOG_LEVEL")` (default 2). `ccLog` gains a level gate: plain `ccLog` (TRACE) emits at ≥2; add `ccLogError` used by the `ASSERT`/`VERIFY` macros in mfc_compat.h emitting at ≥1. In comicchat.h add `void cc_set_log_level(int32_t level);` implemented in mfc_compat.cpp (also resets the lazy init). Keep signatures C-compatible.
- [ ] **Step 4: R15 fix** — in `avbfile.h:283` add to `class CAvatarStream` (public section): `virtual ~CAvatarStream() {}  // R15: ~CAvatarX deletes through base; original lacked vdtor (UB + leak)`.
- [ ] **Step 5: Warning flag** — in Package.swift cchat-engine `cxxSettings`, add `.unsafeFlags(["-Wno-tautological-undefined-compare"])`. (34 warnings from the faithful 1998 `this != NULL` idiom drown real ones — Plan 1 final review.)
- [ ] **Step 6: Verify** — `swift test 2>&1 | tail -5`: all 7 existing tests green (the new check runs inside the existing selftest-harness test). Confirm the tautological-compare warnings are gone from build output: `swift build 2>&1 | grep -c tautological` → `0`.
- [ ] **Step 7: Commit** — `git add -A macos/ && git commit -m "macos: Plan 2 entry debt - CAvatarStream vdtor (R15), log levels, warning silencer"`

### Task 2: Canvas C vtable + C++ wrapper + C++ recording canvas

**Files:**
- Modify: `macos/ComicChatKit/Sources/cchat-engine/include/comicchat.h`
- Create: `macos/ComicChatKit/Sources/cchat-engine/bridge/cc_canvas.h`
- Create: `macos/ComicChatKit/Sources/cchat-engine/bridge/cc_recording_canvas.h`, `.cpp`
- Modify: `macos/ComicChatKit/Sources/cchat-engine/bridge/cc_selftest.cpp`

**Interfaces:**
- Produces (C, comicchat.h): `cc_font_spec`, `cc_text_metrics`, `cc_path_pt` (+verb enum), `cc_canvas_ops`, `cc_canvas`, `void cc_set_metrics_canvas(cc_canvas*)`. Produces (C++, bridge-internal): `class CCRecordingCanvas` exposing `cc_canvas* handle()` and `const std::vector<std::string>& log()`.
- Consumed by: Task 3 (CDC adapter calls `cc_canvas_ops` through the engine context), Tasks 10–11.

- [ ] **Step 1: comicchat.h additions** (verbatim; pure C):

```c
/* ============================================================================
 * Canvas boundary (Plan 2, spec §4.3). All coordinates are MM_TWIPS logical
 * units (1/1440 inch, y-up) exactly as the original GDI code used them; the
 * implementation maps to device space. Text params are raw bytes (CP-1252 by
 * default). color values are GDI COLORREF (0x00BBGGRR). */

typedef struct cc_font_spec {
    char    face[64];     /* e.g. "Comic Sans MS" */
    int32_t height;       /* LOGFONT lfHeight in twips; negative = char height */
    int32_t weight;       /* 400 normal, 700 bold */
    uint8_t italic, underline, strikeout, charset;
} cc_font_spec;

typedef struct cc_text_metrics { /* the TEXTMETRIC fields the engine reads; twips */
    int32_t height, ascent, descent, internal_leading, external_leading;
    int32_t ave_char_width, max_char_width;
} cc_text_metrics;

enum { CC_PATH_MOVE = 0, CC_PATH_LINE = 1, CC_PATH_CUBIC = 2, CC_PATH_CLOSE = 3 };
/* CC_PATH_CUBIC appears as THREE consecutive entries (control1, control2,
 * endpoint), all with verb CC_PATH_CUBIC. */
typedef struct cc_path_pt { int32_t verb; int32_t x, y; } cc_path_pt;

typedef struct cc_canvas_ops {
    /* measurement — must work with no drawing surface active */
    void (*measure_text)(void* ctx, const cc_font_spec* f, const char* bytes,
                         int32_t len, int32_t* out_w, int32_t* out_h);
    void (*font_metrics)(void* ctx, const cc_font_spec* f, cc_text_metrics* out);
    /* drawing */
    void (*draw_text)(void* ctx, const cc_font_spec* f, int32_t x, int32_t y,
                      uint32_t color, int32_t bk_opaque, uint32_t bk_color,
                      const char* bytes, int32_t len);
    void (*fill_rect)(void* ctx, int32_t l, int32_t t, int32_t r, int32_t b,
                      uint32_t color);
    void (*draw_image)(void* ctx, const cc_image* img,
                       int32_t dl, int32_t dt, int32_t dr, int32_t db,
                       int32_t sl, int32_t st, int32_t sr, int32_t sb);
    void (*path)(void* ctx, const cc_path_pt* pts, int32_t n,
                 int32_t do_fill, uint32_t fill_color,
                 int32_t do_stroke, uint32_t stroke_color, int32_t stroke_width,
                 int32_t dashed);
    void (*clip_push)(void* ctx, int32_t l, int32_t t, int32_t r, int32_t b);
    void (*clip_pop)(void* ctx);
    int32_t (*is_printing)(void* ctx);
} cc_canvas_ops;

typedef struct cc_canvas { const cc_canvas_ops* ops; void* ctx; } cc_canvas;

/* Register the canvas used for LAYOUT-TIME text measurement (the original's
 * shared MM_TWIPS CClientDC). Must outlive all layout calls. */
void cc_set_metrics_canvas(cc_canvas* canvas);
```

- [ ] **Step 2: `bridge/cc_canvas.h`** — header-only C++ convenience: `class CCanvas { cc_canvas* c_; public: explicit CCanvas(cc_canvas* c); }` with inline forwarding methods mirroring each op (asserting `c_ && c_->ops`). Storage for the registered metrics canvas lives in `engine_context` (add `cc_canvas* metricsCanvas` member; `cc_set_metrics_canvas` sets it).
- [ ] **Step 3: C++ recording canvas** — `CCRecordingCanvas` implements the ops table over a `std::vector<std::string>` log with **deterministic fake metrics, documented in the header**: every byte measures 120 twips wide; text height 240; `font_metrics` = {height 240, ascent 190, descent 50, internal_leading 40, external_leading 20, ave 120, max 240} regardless of font. Log lines are exact strings, e.g. `text 100,-200 color=000000 "hi"`, `rect 0,0,2400,-2400 fill=FFFFFF`, `image 0,0,1200,-1600 src=0,0,60,80`, `path n=5 fill=1 fillc=FFFFFF stroke=1 strokec=000000 w=20 dashed=0 [M 0,0 L 10,0 ...]` (both color fields always present; meaningful only when the matching flag is 1 — plan amendment 2026-07-17, Task 2 review: colors were originally omitted, weakening later snapshot discrimination), `clip+ 0,0,2400,-2400`, `clip-`. Format is the contract for every selftest below — keep it stable.
- [ ] **Step 4: Selftests** — `cc_selftest_canvas()`: build a `CCRecordingCanvas`, drive each op once through `CCanvas`, assert the log matches expected strings exactly; assert `measure_text("hello",5)` → w=600 h=240. Register in `cc_run_selftests()`. Run `swift test` → expect FAIL first (missing files), then green after implementation.
- [ ] **Step 5: Commit** — `git commit -m "macos: cc_canvas C vtable + C++ wrapper + recording canvas (Plan 2 Task 2)"`

### Task 3: Shim CDC adapter (+CFont/CPen/CBrush/TEXTMETRIC/LOGFONT)

**Files:**
- Modify: `macos/ComicChatKit/Sources/cchat-engine/shim/mfc_compat.h`, `shim/mfc_compat.cpp`
- Modify: `macos/ComicChatKit/Sources/cchat-engine/shim/engine_context.h` (metrics canvas + `metricsDC()`)
- Modify: `macos/ComicChatKit/Sources/cchat-engine/bridge/cc_selftest.cpp`

**Interfaces:**
- Consumes: `cc_canvas`/`CCanvas` (Task 2).
- Produces (shim, exact signatures the lifted code calls — inventory from `[CI]`/`[TM]` + the GDI grep in `[CM]`): concrete `class CDC` replacing Plan 1's forward declaration, with: `m_bPrinting` (public BOOL); `SelectObject(CFont*/CPen*/CBrush*/CBitmap*)` returning prior; `GetTextExtent(const char*, int) -> CSize`; `GetTextMetrics(TEXTMETRIC*)`; `SetTextColor/SetBkMode/SetBkColor`; `TextOut(int,int,const char*,int)`; `MoveTo/LineTo/PolyBezier(POINT*,int)/Ellipse(l,t,r,b)/BeginPath/EndPath/CloseFigure/StrokePath/StrokeAndFillPath`; `FillSolidRect(RECT*,COLORREF)` and `(l,t,r,b,COLORREF)`; `StretchDIBits(...)` (palettized DIB → RGBA via the existing Plan 1 converter in `bridge_art`, then `draw_image`; `SRCCOPY` only — any other rop hits `ASSERT(0)` so R14 transformations are forced explicit); `IntersectClipRect/GetClipBox/SelectClipRgn(NULL, RGN_COPY)`; `OffsetWindowOrg/SetWindowOrg/GetMapMode/SetMapMode(MM_TWIPS)`; `IsPrinting()`; `GetDeviceCaps(LOGPIXELSX|LOGPIXELSY) -> 1440`; `GetSafeHdc()` (returns an opaque non-null token; only ever null-checked); `GetCurrentFont()`; palette/stretch-mode no-ops (`GetCurrentPalette/SelectPalette/RealizePalette/SetStretchBltMode/GetBrushOrgEx/SetBrushOrgEx` — file-scope free functions where the original called Win32 free functions). Plus `class CFont { LOGFONT m_lf; ... CreateFontIndirect(const LOGFONT*); cc_font_spec spec() const; }`, `LOGFONT`, `TEXTMETRIC` (twips), `CPen(PS_SOLID,width,COLORREF)`/`CreatePen`, `CBrush`/`CreateSolidBrush`, `CClientDC` (a `CDC` auto-bound to `ccContext().metricsCanvas`), constants `OPAQUE/TRANSPARENT/MM_TWIPS/PS_SOLID/RGN_COPY/LOGPIXELSX/LOGPIXELSY/SRCCOPY`.

Adapter semantics (implement exactly):
- Window origin: `SetWindowOrg(x,y)` means logical (x,y) maps to output origin → every emitted coordinate is `c - org`. `OffsetWindowOrg` accumulates. (GDI semantics; selftest locks them.)
- Clip: a stack of rects; `IntersectClipRect` pushes the intersection; `GetClipBox` returns current top (base = ±2^28 sentinel); `SelectClipRgn(NULL, RGN_COPY)` resets to base. `clip_push`/`clip_pop` are emitted so drawing canvases can honor it.
- Paths: between `BeginPath`/`EndPath`, `MoveTo/LineTo/PolyBezier/CloseFigure` accumulate `cc_path_pt`s; `StrokePath` emits `path(stroke=pen)`, `StrokeAndFillPath` emits `path(fill=brush, stroke=pen)`. Outside a path, `MoveTo` sets current position, `LineTo` emits an immediate 2-point stroke path with the current pen and advances position (traj.cpp draws this way). `Ellipse` emits a 4-cubic closed path (kappa 0.5522847498), filled with current brush and stroked with current pen (GDI semantics).
- Text: `TextOut` emits `draw_text` with current font spec, text color, bk mode/color. `GetTextExtent`/`GetTextMetrics` call `measure_text`/`font_metrics` with the current font.

- [ ] **Step 1: Write failing selftests** — `cc_selftest_dc()` driving a `CDC` bound to a `CCRecordingCanvas`: (a) font select + `GetTextExtent("hello",5)` == 600×240 and `GetTextMetrics` height 240/ascent 190; (b) `TextOut` log line carries color/bk state; (c) origin: after `SetWindowOrg(100,50)`, `TextOut(100,50,...)` logs `text 0,0`; (d) clip push/intersect/reset sequence logs `clip+`/`clip-` correctly; (e) `BeginPath..MoveTo(0,0),LineTo(10,0),CloseFigure,EndPath,StrokePath` logs one path with M/L/Z; (f) bare `MoveTo/LineTo` logs an immediate 2-pt path; (g) `Ellipse(0,0,100,-100)` logs a 12-entry cubic path (4×3); (h) `FillSolidRect` logs `rect`; (i) `GetDeviceCaps(LOGPIXELSY)`==1440. Register; run `swift test` → FAIL (no such members).
- [ ] **Step 2: Implement** the adapter per the semantics block above. Keep every member minimal (R9); no speculative methods.
- [ ] **Step 3: Run** — `swift test 2>&1 | tail -5` → green.
- [ ] **Step 4: Commit** — `git commit -m "macos: shim CDC adapter over cc_canvas + CFont/CPen/CBrush (Plan 2 Task 3)"`

### Task 4: Lift the pure-geometry files — defines.h, spline, splinutl, traj

**Files:**
- Create (lift): `engine/defines.h`, `engine/spline.h`, `engine/spline.cpp`, `engine/splinutl.cpp`, `engine/traj.h`, `engine/traj.cpp` (sources: `v2.5-beta-1-modern/<same name>`)
- Modify: `bridge/cc_selftest.cpp`

**Interfaces:**
- Consumes: CDC adapter (traj `Draw`/`Dash` bodies), `vector2d.h`/`bbox.h` (lifted Plan 1).
- Produces: `CSpline`, `CTraj`, spline utility functions, and the `BM_*` mode constants (defines.h:63–70: `BM_SAY 0x0001`, `BM_WHISPER 0x0002`, `BM_THINK 0x0004`, `BM_ACTION 0x0008`, `BM_SOUND 0x0010`, …) — Tasks 6/8/10 depend on these exact values.

- [ ] **Step 1: Copy** the six files byte-identical from `v2.5-beta-1-modern/`. Include edits per rules only: spline.cpp/traj.cpp `stdafx.h`→R1; splinutl.cpp `stdafx.h`→R1, `chat.h`→R2. defines.h: apply R6/R8 only if it references resource/UI headers (inspect at lift; constants stay verbatim). Add `#include "defines.h"` to `engine_context.h` so R2-rerouted files see the `BM_*` constants exactly as `chat.h` transitively provided them (note this in the report).
- [ ] **Step 2: Selftests** — `cc_selftest_geometry()`: (a) build a `CTraj` from 3 known points, `Draw` through a recording-canvas CDC, assert the logged path string; (b) one spline-utility numeric check with hand-computed expected values (pick a function with obvious semantics, e.g. midpoint/length; document the arithmetic in a comment). Run `swift test` → green.
- [ ] **Step 3: Fidelity check** — `diff v2.5-beta-1-modern/spline.cpp macos/.../engine/spline.cpp` (etc. for all six): every hunk attributable to a rule; note each in the report.
- [ ] **Step 4: Commit** — `git commit -m "macos: lift defines.h + spline/splinutl/traj (Plan 2 Task 4)"`

### Task 5: Lift format.h + format.cpp (formatting/measurement half)

**Files:**
- Create (lift): `engine/format.h` (source: **`artifacts/inc/format.h`**, 68 lines — provenance comment at top), `engine/format.cpp` (source: `v2.5-beta-1-modern/format.cpp`, 1507 lines)
- Modify: `bridge/cc_selftest.cpp`

**Interfaces:**
- Consumes: CDC adapter (`GetFormattedTextExtent` does per-run `SelectObject`+`GetTextExtent`), `CDWordArray` (shim).
- Produces (used by Tasks 6/8/10, exact decls in format.h): `GetFormattedTextExtent(CDC*, LPCTSTR, DWORD, CDWordArray*) -> CSize`, `SzControlLess`, `GetRBGColor`, `nGetSpecialFontIndex`, `InsertFormat`, `CopyFormatting`, `FreeAndNullFormatting`, `CutFormattingArray`, `PullFormattingOffsets`, `PushFormattingOffsets`, `bURLPresent`, plus the `w*` format-bit constants and `STRFMT`.

- [ ] **Step 1: Copy + rule edits.** format.cpp includes: `stdafx.h`→R1; `"..\inc\urlutil.h"`→R8 (delete; if URL helpers are genuinely called by kept functions, R12 decides per symbol); `chat.h`→R2; `userinfo.h` (lifted in Task 6 — forward-declare per R8 for now if needed, or reorder includes; flag if deeper); `chatprot.h`/`binddoc.h`/`chatDoc.h`/`ui.h`/`rtfctrl.h`→R8 delete. Functions taking `CRichEditCtrl*`/`CHARFORMAT` (`PRGDWGetFormatting`, `bLOGFONTToCHARFORMAT`, `MatchFont`, and any other RichEdit-touching function) → whole-function R11 `#ifndef CC_NO_UI`. The 11 produced functions and their static helpers stay live and verbatim.
- [ ] **Step 2: Selftests** — `cc_selftest_format()` against the recording canvas metrics (120 twips/byte): (a) `GetFormattedTextExtent(dc, "hello", 5, NULL)` == 600×240 (NULL formatting = single default run — verify against the lifted code's actual NULL handling and document); (b) a two-run case: text "aabb" with a formatting array switching bold at offset 2 (build with `InsertFormat`) still measures 480 wide (fake metrics ignore style) but exercises the run loop; (c) `SzControlLess` strips `chCtlBold` (0x02) markers from "a\x02b" → "ab" with a 1-entry formatting array. Expected values computed from the documented fake metrics; if observed behavior differs (e.g. trailing-space policy), hand-verify the observed value against the lifted code and freeze it with the arithmetic in a comment.
- [ ] **Step 3: Run + fidelity diff** — `swift test` green; diff format.cpp vs original, every hunk rule-attributed; R11 exclusions listed individually.
- [ ] **Step 4: Commit** — `git commit -m "macos: lift format.h/.cpp formatting half (Plan 2 Task 5)"`

### Task 6: Lift balloon.h/.cpp + fonts.cpp + userinfo.h + arc.cpp; delete trap stubs

**Files:**
- Create (lift): `engine/balloon.h`, `engine/balloon.cpp`, `engine/fonts.cpp`, `engine/userinfo.h`, `engine/arc.cpp`, `engine/panel.h` (sources: `v2.5-beta-1-modern/<same>`; arc.cpp added by plan amendment 2026-07-17 — it is in `chat.mak`, balloon.cpp:1526/1528 allocates `CArc` for tail arcs, and Task 4 discovered `CArc::Draw/Dash` (traj.cpp:98–106) call its `DrawArc2`/`DashArc2`; arc.cpp needs only R1, includes are traj.h/vector2d.h/math.h. panel.h added by plan amendment 2026-07-17: balloon.cpp:21 includes it and balloon.cpp/fonts.cpp reference `CUnitPanelPage` members (balloon.cpp:870/1460/1816); panel.cpp itself stays in Task 8. **R12 clarification (amendment):** static DATA members declared in panel.h but defined in panel.cpp (`m_unitWidth`/`m_unitHeight`/`m_panelsPerRow`/intersticies/…) are handled R12(a)-style — lift the definition lines verbatim (with original initializers) from panel.cpp into `engine/lifted_singles.cpp` with provenance comments; Task 8 deletes them when panel.cpp lands)
- Modify: `engine/cc_link_stubs.cpp` (delete `CPanelElement` copy-ctor + `GetBBox` stubs — owners balloon.cpp:641/647 — plus the Task 4 R12(b) stubs for `DrawArc2`/`DashArc2` once arc.cpp is lifted)
- Modify: `shim/engine_context.h` (+`.cpp` if split) — R17 additions
- Modify: `bridge/cc_selftest.cpp`

**Interfaces:**
- Consumes: format (Task 5), geometry (Task 4), CDC adapter, `pe.h`/`dib.h`/`avatar.h` (Plan 1).
- Produces: `CFontInfo` (balloon.h:47 — ctor at balloon.cpp:584 measures via `metricsDC()`), `CFormatInfo` (balloon.h:11), `CBalloon`/`CLabel`/`CBWoodring*` classes, `::BreakIntoLines` (balloon.cpp:347), `CBalloon::SetBBox`→`ComputeInternals` (balloon.cpp:1396/1764), `CUnitPanelPage::SetFonts(LOGFONT&, COLORREF)` + static `m_fiWNormal/m_fiWWhisper/m_fiTitle/m_fiShout` (fonts.cpp:30–40), `CUserInfo`/`CUserDisplayInfo::m_talkTos` (userinfo.h:58).

Known R-work for balloon.cpp (from `[TM]`/`[CM]` — implementer verifies the full list during lift): includes `userinfo.h` (now lifted), `chatprot.h`/`ircproto.h`/`ui.h`/`binddoc.h`/`chatdoc.h`/`pageview.h`→R8, `script.h`→delete (dead header, nothing used — note under R8 in report), `<tchar.h>`/`<winnls.h>`→R8+R9 (shim provides `_T`; any `winnls` charset call: minimal shim or R11 by function), `chat.h`→R2, `theApp.m_charSet`→R17, `GetClientDC()` ×8 → R17 `ccContext().metricsDC()`, URL/hot-link + `OnLButtonDown` interaction paths → R11, `CFontInfo::CFontInfo` keeps its original body with `GetClientDC()`→`metricsDC()` (R17, listed). userinfo.h: R8-trim UI includes; keep `CUserInfo`/`CUserDisplayInfo` data + inline methods; R11 anything protocol/session-reaching. fonts.cpp: `stdafx/chat`→R1/R2; `theApp.m_comicsColor`/font settings → R17 session fields (`ccContext().session.comicsColor`, default `RGB(0,0,0)`); resource-string font defaults (`ID_COMIC_FONT_NAME` "Comic Sans MS" / point size 12) → R17 session fields with those defaults (list in report, cite `[TM]` for the original registry/resource chain).

- [ ] **Step 1: R17 context extensions first** (failing selftest → implement): `ccContext().metricsDC()` returning a lazily-built `CDC` bound to `metricsCanvas` (ASSERT if unset); session struct `{ COLORREF comicsColor = RGB(0,0,0); char comicsFontFace[64] = "Comic Sans MS"; int comicsFontPts = 12; int charSet = 0; }`. Selftest: metricsDC measures through a registered recording canvas.
- [ ] **Step 2: Lift the four files** with the rule edits above. Build until link-clean (`swift build`). Any symbol owned by panel.cpp/bodycam.cpp not yet lifted → R12b trap stub (tagged; Tasks 7/8 delete them). Delete the two balloon-owned stubs from cc_link_stubs.cpp.
- [ ] **Step 3: Selftests** — `cc_selftest_balloon()` (recording canvas registered as metrics canvas): (a) `CFontInfo` ctor: lineHeight/leading fields match the fake metrics arithmetic (hand-compute from the ctor body; document); (b) `BreakIntoLines` characterization: input `"hello world foo bar"`, max width 1200 twips (=10 bytes at 120): expect the greedy wrap the lifted code produces — compute expected line count/breaks by hand from the algorithm + fake metrics, freeze with the arithmetic in a comment; (c) `CBalloon::SetBBox` on a fixed text yields a stable bbox (characterize once, hand-verify plausibility: width ≥ longest line, height ≈ nLines × lineHeight + margins). (d) `SetFonts` then check the four `CFontInfo` statics non-null with expected heights.
- [ ] **Step 4: Run + fidelity diff** — `swift test` green. Diff balloon.cpp/balloon.h/fonts.cpp/userinfo.h vs originals: every hunk rule-attributed; R11/R17 sites individually listed in the report.
- [ ] **Step 5: Commit** — `git commit -m "macos: lift balloon + fonts + userinfo, retire 2 link stubs (Plan 2 Task 6)"`

### Task 7: Lift bodycam.cpp/.h; delete 10 stubs; retire CC_NO_RENDER

**Files:**
- Create (lift): `engine/bodycam.h`, `engine/bodycam.cpp`
- Modify: `engine/cc_link_stubs.cpp` (delete all 10 `CBodySingle`/`CBodyDouble` stubs)
- Modify: `macos/ComicChatKit/Package.swift` (remove `.define("CC_NO_RENDER")`)
- Modify: `bridge/cc_selftest.cpp`

**Interfaces:**
- Consumes: CDC adapter incl. `StretchDIBits`, `avatar.h`/`dib.h` (Plan 1).
- Produces: live `CBodySingle`/`CBodyDouble` `DrawBody/Draw/GetBodyBox/FlipBodyBox/IsSame` (bodycam.cpp:447–691); re-enabled `CDIB::Draw` ×3 (dib.cpp:166–226) and `CBackDrop::Draw` (backdrop.cpp:338–377) — the four Plan 1 `CC_NO_RENDER` blocks `[CI]`.

- [ ] **Step 1: Lift** bodycam.h/.cpp. The `CBodyCam : CWnd` widget (paint/mouse/tooltip/context-menu handlers, `GetBodyCam()` singleton, `theApp.DoOptionsDialog`, connection-status reads) → whole-class/function R11 `#ifndef CC_NO_UI` (list every exclusion). Includes: `saywnd.h`/`resource.h`/`ui.h`/`binddoc.h`/`chatdoc.h`/`protsupp.h`→R8; `chat.h`→R2; `stdafx`→R1. The `CBody*` method bodies stay verbatim except R14-listed transformations: each `SetROP2`+`BitBlt` mask pair → single `drawImage` with alpha (cite line numbers of every replaced pair in the report; the RGBA conversion already encodes mask transparency — Plan 1 `bridge_art`).
- [ ] **Step 2: Retire CC_NO_RENDER** — remove the define from Package.swift; delete the four `#ifndef CC_NO_RENDER`/`#else ASSERT(0)/#endif` wrapper lines in dib.cpp/backdrop.cpp so the original bodies stand unwrapped (R14: restoring original code; the dead `MEMDC_NOT_STRETCHED` block in backdrop.cpp stays as-is — it never compiled in the original either `[CI]`). Their `StretchDIBits`/`FillSolidRect`/`IsPrinting` calls now hit the adapter. Delete the 10 bodycam-owned stubs.
- [ ] **Step 3: Selftests** — `cc_selftest_bodydraw()`: open `comicart/anna.avb` (fixture path per existing tests), build the body for a known pose, `Draw` through a recording-canvas CDC, assert: exactly one `image` log line per expected blit, dest rect matches `GetBodyBox` output, no `ASSERT` traps. Also re-run the existing pose-image golden test (unchanged) to prove the RGBA path didn't regress.
- [ ] **Step 4: Run + fidelity diff** — `swift test` green (all suites). Diffs rule-attributed; every R11 exclusion and R14 transformation listed.
- [ ] **Step 5: Commit** — `git commit -m "macos: lift bodycam CBody draw, retire CC_NO_RENDER + 10 stubs (Plan 2 Task 7)"`

### Task 8: Lift panel.cpp; delete last Plan-1 stub

**Files:**
- Create (lift): `engine/panel.cpp` (panel.h already lifted in Task 6 per amendment)
- Modify: `engine/cc_link_stubs.cpp` (delete the `CPanelElement::SetBBox` stub — its real body lands with panel.cpp. Amendment 2026-07-17: the FILE STAYS — it now also hosts the Task 6 intl.c stubs (`GetMime`/`iBytesofChar`/`FindSubStringForINTLThatFits`), which are Plan 3 debt; with SetBBox gone, all 13 original Plan 1 stubs are retired)
- Modify: `engine/lifted_singles.cpp` (delete the `m_unitWidth` static definition — panel.cpp defines it; the bbox.cpp singles and SRECTToRECT stay. **Amendment 2026-07-17 (Task 8 escalation ruling):** `Establishing()` + `g_bNewedPanel` (pageview.cpp:832/:830 — pageview.cpp is never lifted) are added here R12(a)-verbatim, with Establishing's one singleton read (`GetView()->GetDocument()->m_pages.GetHead()`) rerouted R17-style to a `s_composingPage` static set via `ccSetComposingPage(this)` — two one-line R17-listed additions at the top of `AddLine`/`AddReaction`. Faithful under the single-page headless model (composing page == first page). Title/starring functions (`AddTitle`/`UpdateTitle`/`ShowInfo`/`AddStars`) are R11-wrapped: title rendering is a known functional deferral this plan.)
- Modify: `shim/engine_context.h` — R17 session additions (users, backdrop id, title)
- Modify: `bridge/cc_selftest.cpp`

**Interfaces:**
- Consumes: everything from Tasks 4–7.
- Produces: `CPanel`/`CUnitPanel`/`CPage`/`CUnitPanelPage` (panel.h:45/96); the camera (`OrderAvatars/DoGreedyOrdering/EvalPlacement/EvalPair/AddTalkTos`, panel.cpp:272–460); `CUnitPanelPage::AddLine` (panel.cpp:1058 — the orchestrator); `MakeBalloon` (1036); `LayoutAvatars/LayoutBalloons/LayoutBalloon` (726/855/925); statics `m_unitWidth/m_unitHeight/m_panelsPerRow/m_vInterstice/m_hInterstice` + accessors (panel.h:99–145, original file-scope defaults preserved incl. `m_panelsPerColumn = -1`); `CUnitPanelPage::GetBBox` (panel.cpp:1268). Task 10 calls `AddLine`/`GetBBox`/panel iteration.

R-work (from `[CM §4]` — implementer verifies exhaustively): includes → R1/R2/R8 as in prior tasks (`pageview.h` delete; `protsupp.h` delete or R12 per symbol). `CUserInfo` lookups + `m_udi.m_talkTos` reads stay verbatim — the *lookup* call goes through R17 (`ccContext().session` user table, exact original call shape preserved as closely as the context allows; every site listed). `GetChatDoc()->GetBackDropID()/GetComicsTitle()` → R17 session fields. `UpdateViewsX/RefreshLastPanel/RefreshPanelN` view pokes → R11/R17 no-ops (listed). `theApp.m_comicsColor` → R17. `theApp.StartDownloadingAvatar` (dead hot-link path) → R11. `CUnitPanelPage::Draw` (1193) → R16: wrap whole definition in `#ifndef CC_NO_UI`; do NOT port (Task 10 replaces it). `GetBodyCamBody()` extern (dead) → R8/R11.

- [ ] **Step 1: R17 session extensions** (failing selftest first): user table `ccContext().session.users` — fixed array of `{ UINT id; CUserInfo info; }` with add/lookup helpers mirroring the original lookup the lifted call sites need; `UINT backdropID`; `char comicsTitle[128]`.
- [ ] **Step 2: Lift** panel.h/.cpp per the R-work block. Link-clean build; delete the SetBBox stub; delete the emptied `cc_link_stubs.cpp` and its Package.swift/target references if any. **All Plan 1 link-stub debt is now retired.**
- [ ] **Step 3: Selftests** — `cc_selftest_panel()` (recording metrics canvas; two test avatars loaded from comicart fixtures; two users in the session table): (a) **camera**: speaker A with `m_talkTos` = {B} and B present → after `LayoutAvatars`, assert A and B have opposite `m_flipped` states (they face each other) and deterministic left/right order; re-run with roles swapped and assert the ordering flips accordingly (characterize exact outputs once, hand-verify against `EvalPair`'s scoring, freeze); (b) **panel-break rules** (`[CM §2]`): `AddLine` same speaker twice → second line forces a new panel (speaker already in panel); ≥5 elements forces a break; `BM_ACTION` always breaks; (c) **orchestration**: two alternating speakers × 4 lines → `panel_count` matches the frozen characterization; every balloon bbox lies inside its panel's unit rect.
- [ ] **Step 4: Run + fidelity diff** — `swift test` green; diff panel.cpp/.h vs originals; every hunk rule-attributed; R16/R17 sites individually listed.
- [ ] **Step 5: Commit** — `git commit -m "macos: lift panel.cpp camera+orchestrator, all link stubs retired (Plan 2 Task 8)"`

### Task 9: Lift textpose.cpp (text → emotion/pose)

**Files:**
- Create (lift): `engine/textpose.cpp` (source: `v2.5-beta-1-modern/textpose.cpp`, 334 lines)
- Modify: `bridge/cc_selftest.cpp`

**Interfaces:**
- Consumes: `GetAvatar` / `CAvatarX::GetBodyFromEmotion` / `UpdateBody` (avatar.cpp, lifted Plan 1, live outside CC_NO_UI).
- Produces: `ChatPreSendText(CString&, int avID)` (textpose.cpp:119), `GetEmotionsFromString` (textpose.cpp:271) — Task 10's driver calls `ChatPreSendText` per line.

- [ ] **Step 1: Lift** with rule edits: `stdafx/chat`→R1/R2; `bodycam.h` include stays (lifted Task 7); `ui.h/userinfo.h/chatprot.h/binddoc.h/chatdoc.h/resource.h`→R8 (the `GetChatDoc()->m_bComicView` guard at :122 → R17 session flag `comicView`, default TRUE, listed); rule tables verbatim.
- [ ] **Step 2: Selftests** — `cc_selftest_textpose()`: characterization over fixed strings chosen by reading the lifted rule tables (e.g. an all-caps string, a string containing a rule-table keyword, a neutral string). For each: assert the exact `CEmotionOpts` output observed, after hand-cross-checking each against the specific rule that fires (cite rule-table line in the comment). Minimum 4 cases including one no-rule-fires default.
- [ ] **Step 3: Run + fidelity diff** — `swift test` green; diff rule-attributed.
- [ ] **Step 4: Commit** — `git commit -m "macos: lift textpose text->emotion engine (Plan 2 Task 9)"`

### Task 10: Strip session API + headless compositor (R16 replacement)

**Files:**
- Modify: `include/comicchat.h` (strip API below)
- Create: `bridge/cc_compose.cpp`
- Modify: `bridge/cc_selftest.cpp`

**Interfaces:**
- Consumes: everything above.
- Produces (C, comicchat.h — Task 11's Swift wrapper binds these exactly):

```c
/* Scripted-strip session (Plan 2). Modes mirror defines.h BM_* values. */
enum { CC_MODE_SAY = 0x0001, CC_MODE_WHISPER = 0x0002,
       CC_MODE_THINK = 0x0004, CC_MODE_ACTION = 0x0008 };

typedef struct cc_strip cc_strip;
cc_strip* cc_strip_create(void);
void      cc_strip_destroy(cc_strip* s);            /* no-op if NULL */
int32_t   cc_strip_add_participant(cc_strip* s, const char* nick,
                                   const char* avb_path);   /* >=0 id, -1 fail */
int32_t   cc_strip_set_backdrop(cc_strip* s, const char* bgb_path); /* 0 ok */
int32_t   cc_strip_add_line(cc_strip* s, int32_t speaker,
                            const char* text_bytes, uint32_t modes,
                            const int32_t* addressees, int32_t n_addr); /* 0 ok */
int32_t   cc_strip_panel_count(const cc_strip* s);
void      cc_strip_get_size(const cc_strip* s, int32_t* out_w, int32_t* out_h); /* twips */
int32_t   cc_strip_compose(cc_strip* s, cc_canvas* canvas); /* 0 ok */
```

**Amendments 2026-07-17 (Task 8 review debt, binding on this task):** (1) **Determinism is engine-owned, not test-owned** — the live layout path consumes `rand()` (`CPanel::CPanel` takes `m_seed = rand()` at panel.cpp:604; `LayoutBalloons` reseeds via `srand(m_seed)` and `randfloat()` drives balloon goalWidth/goalLines/startX). `cc_strip_create` MUST seed the global stream deterministically (`srand(0x5EED)` documented in comicchat.h next to the declaration) so identical scripts yield identical strips; the test-scaffold `srand` pin in cc_selftest.cpp then becomes redundant belt-and-braces (keep it, note it). (2) **Single-threaded contract** — the engine uses process-global mutable state (avatar registry, session, font statics, `s_composingPage`); add a documented constraint in comicchat.h ("all cc_* calls must originate from one thread at a time; no internal locking") and have the Swift `Strip` wrapper (Task 11) document the same. These two are review-mandated; the task reviewer will verify both.

- [ ] **Step 1: Failing selftest** — `cc_selftest_strip()`: recording metrics canvas registered; create strip, add 2 participants (fixture .avb paths), add 4 alternating lines (each addressing the other), `panel_count > 0`, `get_size` matches `CUnitPanelPage::GetBBox`, `compose` onto a recording canvas produces a non-empty log whose first line is the backdrop/first-panel content and which contains ≥1 `text` and ≥1 `image` entry per panel. Run → FAIL (no API).
- [ ] **Step 2: Implement `cc_compose.cpp`.** `cc_strip` owns: a `CUnitPanelPage`, the session user table entries, loaded avatars (Plan 1 open path), backdrop. `add_line` replicates the original ingestion chain `[CM §1]`: set the speaker's `m_udi.m_talkTos` from `addressees`; call `ChatPreSendText(text, speaker)` (textpose); call `page->AddLine(speaker, text, modes, NULL /*formatting*/, NULL /*url*/)` (exact lifted signature from panel.h). `set_fonts` at create: `CUnitPanelPage::SetFonts` with LOGFONT from session face/size defaults. `cc_strip_compose` transliterates the origin walk of `CUnitPanelPage::Draw` (panel.cpp:1232–1255) **without** the memDC/BitBlt/palette machinery (R16; cite lines):

```cpp
// R16 replacement for CUnitPanelPage::Draw (panel.cpp:1193) — headless.
// Origin walk transliterated from panel.cpp:1232-1255; full-page damage.
int32_t cc_strip_compose(cc_strip* s, cc_canvas* canvas) {
    if (!s || !canvas) return -1;
    CDC dc; dc.Attach(canvas);                 // adapter bound to the target canvas
    CUnitPanelPage* page = s->page;
    RECT full; page->GetBBox(&full);           // panel.cpp:1268
    int truePanelsPerRow = CUnitPanelPage::m_panelsPerRow;   // screen path
    RECT panelRect; SetRect(&panelRect, 0, 0,
        CUnitPanelPage::m_unitWidth, -CUnitPanelPage::m_unitHeight); // :1233
    int panelCount = 0; POINT loc; loc.x = loc.y = 0;        // :1234-1236
    POSITION pos = page->m_panels.GetHeadPosition();
    while (pos != NULL) {                                    // :1238
        CPanel* panel = (CPanel*)page->m_panels.GetNext(pos);
        panelCount++;
        dc.SetWindowOrg(-loc.x, -loc.y);       // panel-local (0,0) -> loc
        RECT dmg = panelRect;                  // full per-panel damage
        panel->Draw(&dc, &loc, &dmg);          // :1244 (panel draws at local 0,0)
        dc.SetWindowOrg(0, 0);
        if (panelCount % truePanelsPerRow == 0) {            // :1250-1254
            loc.x = 0;
            loc.y -= CUnitPanelPage::m_unitHeight + CUnitPanelPage::m_hInterstice;
        } else
            loc.x += CUnitPanelPage::m_unitWidth + CUnitPanelPage::m_vInterstice;
    }
    return 0;
}
```

(Adapter gains `Attach(cc_canvas*)` under R9 with selftest. Sign convention of `SetWindowOrg` per Task 3's locked semantics: mapping logical `loc` to output means org = −loc; the Task 3 selftest is the reference — if the sign is wrong the Step 3 snapshot catches it immediately.)
- [ ] **Step 3: Snapshot selftest** — extend `cc_selftest_strip()`: compose the fixed 2×4 conversation onto the recording canvas and compare the **full log** against a frozen expected-lines array committed in the test (first freeze: run once, hand-review every line for plausibility — panel origins advance by `unitWidth + vInterstice`, second row drops by `unitHeight + hInterstice`, backdrop before bodies before balloons per `CUnitPanel::Draw` order panel.cpp:679–702 — then freeze).
- [ ] **Step 4: Run** — `swift test` green.
- [ ] **Step 5: Commit** — `git commit -m "macos: cc_strip session API + headless compositor (Plan 2 Task 10)"`

### Task 11: Swift Canvas (protocol, recording, CoreGraphics) + PNG exit milestone

**Files:**
- Create: `Sources/ComicChatKit/Canvas.swift`, `RecordingCanvas.swift`, `CGCanvas.swift`, `Strip.swift`
- Modify: `Sources/cc-dumpart/main.swift` (`--strip` mode)
- Modify: `Tests/ComicChatKitTests/` (+ fixture `Tests/ComicChatKitTests/Fixtures/strip-golden.txt`)

**Interfaces:**
- Consumes: the C API from Tasks 2/10 (via the `cchat_engine` module — hyphen→underscore).
- Produces: `public protocol Canvas` (methods mirroring `cc_canvas_ops` with Swift types), `final class CanvasBox` (wraps a `Canvas` into a stable `cc_canvas` via `Unmanaged` context + static C thunks — the box must outlive engine use), `RecordingCanvas: Canvas` (log format **identical** to the C++ recorder, Task 2 Step 3), `CGCanvas: Canvas` (CGBitmapContext RGBA8; twips→points ÷20; y-up flip via CTM; CoreText: `CTFontCreateWithName(face, size)` cache keyed by spec, `CTLineDraw` for draw_text, `CTLineGetTypographicBounds` for measure_text with round-to-twips; bytes→String via CP-1252 `String(bytes:encoding:.windowsCP1252)` fallback `.isoLatin1`; paths via `CGMutablePath` honoring the 3-entry cubic convention; `pngData() -> Data` via ImageIO), `public struct Strip` (create/addParticipant/setBackdrop/addLine/compose/panelCount/size wrapping the C API, RAII via deinit or explicit close — follow the existing `Avatar` wrapper pattern from Plan 1).

- [ ] **Step 1: Failing Swift tests** — (a) `stripSnapshot()`: build the same fixed 2×4 conversation as Task 10 through `Strip` + Swift `RecordingCanvas`; assert log == `Fixtures/strip-golden.txt` (copy the frozen C++ expectation — the two recorders must agree line-for-line, proving the vtable bridge); (b) `stripPNG()` (the **exit milestone**): same conversation through `CGCanvas` sized from `strip.size` (twips÷20 points at 2× scale), assert PNG data non-empty, image width/height match expected pixels, and >1% of pixels are non-white; write the PNG to `.superpowers/sdd/plan2-exit.png` for human visual verification; (c) `cgCanvasMeasures()`: `measure_text("Hello", ComicSans12)` returns width in (300, 3000) twips and height in (200, 400) — loose bounds; CT metrics are OS-dependent, layout fidelity is bounded by snapshot tests on the recording canvas (spec §9 risk noted).
- [ ] **Step 2: Implement** the four Swift files per the Produces block.
- [ ] **Step 3: `cc-dumpart --strip`** — `cc-dumpart --strip out.png` renders a built-in demo script (two comicart avatars, 4 lines) via `CGCanvas` (keep arg handling consistent with the existing `--png` mode incl. its arg-count guard pattern).
- [ ] **Step 4: Run** — `swift test 2>&1 | tail -5` all green; run `swift run cc-dumpart --strip /tmp/strip.png` and confirm exit 0. **Human checkpoint: Tim visually verifies `plan2-exit.png`/`strip.png`** (two avatars, balloons with tails pointing at speakers, backdrop, readable Comic Sans text).
- [ ] **Step 5: Commit** — `git commit -m "macos: Swift Canvas + CGCanvas PNG render - Plan 2 exit milestone (Task 11)"`

### Task 12: Docs — roadmap corrections + Plan 3 handoff

**Files:**
- Modify: `docs/superpowers/plans/2026-07-17-macos-port-roadmap.md`
- Create: `docs/superpowers/plans/2026-07-17-plan-3-handoff.md`

- [ ] **Step 1:** Update the roadmap: mark Plan 2 done; record the dead-code corrections (semantic/wmini/script), textpose addition, `format.cpp` codec/UI split finding (de-risks the spec §9 unknown — Plan 3 inherits a partially-lifted format.cpp), remaining scaffolding (`CC_NO_UI` shrunk, `CC_NO_PROTOCOL` for Plan 3, `CC_NO_DIRSCAN` for Plan 4), and any new debt this plan created (from task reports).
- [ ] **Step 2:** Write the Plan 3 handoff in the same shape as the Plan 2 one: current verified state, read-in-order list, discovery starters (ircproto parse/side-effect split; Wine capture rig), environment quirks, techniques that worked.
- [ ] **Step 3:** `swift test` one final time (7+N green) → commit `git commit -m "docs: Plan 2 complete - roadmap corrections + Plan 3 handoff"`.

---

## Self-review record (writing-plans checklist)

- **Spec coverage:** §4.3 Canvas (Tasks 2/3/11 — drawImage/fillPath/strokePath/drawText/measureText all present in the vtable; measurement feeds layout via `cc_set_metrics_canvas`; resolution-independent twips; Comic Sans MS default; recording canvas for §8.3 layout snapshots ✓). Exit milestone (roadmap): Tasks 10/11 ✓. Plan 1 debt (roadmap entry list): Task 1 + stub retirement Tasks 6–8 ✓. `[CI]` non-fitting calls all dispositioned: rop→R14(i)+adapter ASSERT, FillSolidRect→`fill_rect`, IsPrinting→vtable op, dead block→documented stay.
- **Placeholders:** none — every lift step names its rules, exact sources, and listed transformations; characterization tests carry their freeze-and-hand-verify procedure explicitly.
- **Type consistency:** `cc_canvas`/`cc_font_spec`/`cc_path_pt` names match across Tasks 2/3/10/11; `BM_*`↔`CC_MODE_*` values pinned to defines.h:63–70; `AddLine`/`GetBBox`/`SetFonts` signatures cited from panel.h/fonts.cpp and re-verified at lift time.
