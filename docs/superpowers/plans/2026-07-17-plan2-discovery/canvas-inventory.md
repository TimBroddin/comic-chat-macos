# Plan 2 Discovery — CC_NO_RENDER Canvas Inventory

Scope: `/Users/timbroddin/Projects/comic-chat/macos/ComicChatKit/Sources/cchat-engine/engine/*.cpp` and `*.h`.

Read-only discovery. No source files were modified.

## 0. Summary of what exists

`grep -lE '#ifndef CC_NO_RENDER' *.cpp *.h` matches exactly two files:

- `backdrop.cpp` — 1 block
- `dib.cpp` — 3 blocks

No `.h` file contains a `CC_NO_RENDER` block. Total: **4 blocks**, all GDI drawing calls confined to these two files. (`avatar.cpp` has `CC_NO_UI` and `CC_NO_DIRSCAN` guards but no `CC_NO_RENDER` — those guard session/doc-layer and directory-scan code, not drawing, and are out of scope for the Canvas design.)

---

## 1. Block-by-block inventory

### Block A — `backdrop.cpp:338-377`, `CBackDrop::Draw(CDC *dc, RECT *panelRect, RECT *)`

```cpp
void CBackDrop::Draw(CDC *dc, RECT *panelRect, RECT *)
{
#ifndef CC_NO_RENDER  // R4                              // line 338
    ...
#else
    ASSERT(0);
#endif // CC_NO_RENDER                                     // line 377
}
```

Enclosing function: `CBackDrop::Draw` (defined `backdrop.cpp:336-378`; declared `backdrop.h:46`, overriding `CPanelElement::Draw` from `pe.h:17`... actually note: `CBackDrop::Draw(CDC*,RECT*,RECT*)` is a *different, non-virtual* overload from the virtual `CPanelElement::Draw(CDC*,POINT*,RECT*)` — see backdrop.h note below).

Signature: `void CBackDrop::Draw(CDC *dc, RECT *panelRect, RECT * /*unnamed*/)` — **takes `CDC*` directly.** This is the Plan 2 reroute point for backdrop drawing.

Calls/types inside the guarded body (lines 339-374):

| Line(s) | Call / type | Detail |
|---|---|---|
| 339 | `dc->IsPrinting()` | CDC state query (screen vs printer target) |
| 340-341 | `CBackDropArt`, `CDIB* drawing = art->m_backdrop->GetDrawing()` | non-GDI: fetches the decoded DIB (not a GDI call itself) |
| 344-348 (`#ifdef MEMDC_NOT_STRETCHED`, dead code — macro never defined) | `GetBrushOrgEx(dc->GetSafeHdc(), &point)`, `dc->SetStretchBltMode(STRETCHMODE)`, `SetBrushOrgEx(...)` | DC state management (brush origin, stretch mode) — **entire block is dead code**, guarded by an always-undefined macro (`MEMDC_NOT_STRETCHED`), never compiled in the original either |
| 350-359 | plain arithmetic (`ROUND`, panel/bitmap width/height, src rect calc) | no GDI, pure math to map bbox onto bitmap pixels |
| 362-364 | `drawing->Draw(dc, panelRect->left, panelRect->top, panelWidth, panelHeight, srcLeft, srcTop, srcWidth, srcHeight, SRCCOPY)` | **image blit** — calls `CDIB::Draw(CDC*, int destX, int destY, int destW, int destH, int srcX, int srcY, int srcW, int srcH, int rop)` (the 4-arg-plus-rop overload in dib.cpp/dib.h), passing raster-op `SRCCOPY` |
| 365-369 (dead, same `#ifdef MEMDC_NOT_STRETCHED`) | `GetBrushOrgEx`/`SetStretchBltMode`/`SetBrushOrgEx` restore | DC state management — dead code |
| 373 | `dc->FillSolidRect(panelRect, RGB(255,255,255))` | **CDC method** — solid-color rect fill (fallback path when no art found); raster fill, not a path stroke |

Categorization:

| Call | Category |
|---|---|
| `dc->IsPrinting()` | DC state management |
| `GetBrushOrgEx` / `SetStretchBltMode` / `SetBrushOrgEx` (dead code, `MEMDC_NOT_STRETCHED`) | DC state management (dead — can be dropped, not ported) |
| `drawing->Draw(...SRCCOPY)` → `CDIB::Draw` | Image blit |
| `dc->FillSolidRect(panelRect, RGB(255,255,255))` | Other (solid-fill / "clear" — not an image blit, not a path stroke in the vector sense) |

CDC* in signature: **yes** — `CBackDrop::Draw(CDC *dc, ...)`.

---

### Block B — `dib.cpp:166-183`, `CDIB::Draw(CDC* pDC, int x, int y)`

```cpp
void CDIB::Draw(CDC* pDC, int x, int y)
{
#ifndef CC_NO_RENDER                                       // line 166
    ::StretchDIBits(pDC->GetSafeHdc(),
                    x, y, DibWidth(), DibHeight(),
                    0, 0, DibWidth(), DibHeight(),
                    GetBitsAddress(), GetBitmapInfoAddress(),
                    DIB_RGB_COLORS, SRCCOPY);
#else
    { ASSERT(0); }
#endif                                                       // line 183
}
```

Enclosing function: `CDIB::Draw(CDC*, int, int)` (1-of-3 overloads; declared `dib.h:32`).

Signature: **takes `CDC*` directly** (first parameter). Plan 2 reroute point.

Calls/types:

| Call | Detail |
|---|---|
| `pDC->GetSafeHdc()` | CDC accessor → raw `HDC`, feeds the Win32 call below (DC state / handle extraction) |
| `::StretchDIBits(hdc, destX, destY, destW, destH, srcX, srcY, srcW, srcH, bits, BITMAPINFO*, DIB_RGB_COLORS, SRCCOPY)` | Global Win32 GDI call — **image blit**, DIB source, `DIB_RGB_COLORS` color-table mode, `SRCCOPY` raster op |
| `DibWidth()`, `DibHeight()`, `GetBitsAddress()`, `GetBitmapInfoAddress()` | non-GDI helpers on `CDIB` (own class), supply blit params |

Categorization: entirely **image blit** (`StretchDIBits` + its `GetSafeHdc()` DC-handle plumbing).

CDC* in signature: **yes**.

---

### Block C — `dib.cpp:187-204`, `CDIB::Draw(CDC* pDC, int x, int y, int destWidth, int destHeight, int rop)`

```cpp
void CDIB::Draw(CDC* pDC, int x, int y, int destWidth, int destHeight, int rop)
{
#ifndef CC_NO_RENDER                                       // line 187
    ::StretchDIBits(pDC->GetSafeHdc(),
                    x, y, destWidth, destHeight,
                    0, 0, DibWidth(), DibHeight(),
                    GetBitsAddress(), GetBitmapInfoAddress(),
                    DIB_RGB_COLORS, rop);
#else
    { ASSERT(0); }
#endif                                                       // line 204
}
```

Enclosing function: `CDIB::Draw(CDC*, int, int, int, int, int)` overload 2-of-3 (declared `dib.h:33-36`, default `rop = SRCCOPY`).

Signature: **takes `CDC*` directly**.

Calls: same shape as Block B — `pDC->GetSafeHdc()` + `::StretchDIBits(...)`, except **`rop` is a caller-supplied parameter, not hardcoded `SRCCOPY`** (default value `SRCCOPY` lives in the header default-arg, `dib.h:36`). This is the one call site in-scope where a raster op *other than* `SRCCOPY` could actually be selected by a caller (need to check Plan 2-era callers of this overload for non-SRCCOPY rop values — none exist yet since all render call sites are still disabled, but the parameter is a live flag for later).

Categorization: **image blit**, with a **rop-flexibility flag** (see Section 3).

CDC* in signature: **yes**.

---

### Block D — `dib.cpp:209-226`, `CDIB::Draw(CDC* pDC, int destX, int destY, int destWidth, int destHeight, int srcX, int srcY, int srcWidth, int srcHeight, int rop)`

```cpp
void CDIB::Draw(CDC* pDC, int destX, int destY, int destWidth, int destHeight,
                int srcX, int srcY, int srcWidth, int srcHeight, int rop)
{
#ifndef CC_NO_RENDER                                       // line 209
	   ::StretchDIBits(pDC->GetSafeHdc(),
                    destX, destY, destWidth, destHeight,
                    srcX, srcY, srcWidth, srcHeight,
                    GetBitsAddress(), GetBitmapInfoAddress(),
                    DIB_RGB_COLORS, rop);
#else
    { ASSERT(0); }
#endif                                                       // line 226
}
```

Enclosing function: `CDIB::Draw(CDC*, ...9 args...)` overload 3-of-3 (declared `dib.h:37-38`, no default for `rop` — always explicit here). This is the general source-rect-to-dest-rect overload, and it's the one `CBackDrop::Draw` (Block A) actually calls, with `rop = SRCCOPY` explicit.

Signature: **takes `CDC*` directly**.

Calls: same shape — `pDC->GetSafeHdc()` + `::StretchDIBits(...)` with independent src/dest rects and caller-supplied `rop`.

Categorization: **image blit**, with the same rop-flexibility flag as Block C.

CDC* in signature: **yes**.

---

## 2. Category rollup (all 4 blocks combined)

| Category | Calls | Blocks |
|---|---|---|
| Image blit | `::StretchDIBits` (×3, one per `CDIB::Draw` overload), `drawing->Draw(...)` call site in Block A that dispatches to Block D | A, B, C, D |
| DC state management | `dc->IsPrinting()`; `GetSafeHdc()` (×3, blit plumbing); dead-code `GetBrushOrgEx`/`SetStretchBltMode`/`SetBrushOrgEx` in Block A (`MEMDC_NOT_STRETCHED`, never compiled) | A, B, C, D |
| Palette | none | — |
| Path drawing (splines/tails/panel borders) | none | — |
| Text | none | — |
| Other | `dc->FillSolidRect(panelRect, RGB(255,255,255))` (Block A fallback fill) | A |

Notably absent from these 4 blocks: **no palette calls, no path/spline/polygon drawing, no text drawing** anywhere in `backdrop.cpp`/`dib.cpp`. Those categories (fillPath/strokePath, drawText/measureText) must come from files Plan 1 has not yet lifted (e.g. `bodycam.cpp`, `panel.cpp`, `wmini.cpp`, `balloon.cpp` — currently only stubbed via `cc_link_stubs.cpp`, real bodies not yet in this tree) and were **not observed** in the disabled code inventoried here. This is itself an important finding for Plan 2 sequencing: the *currently lifted* CC_NO_RENDER code proves out `drawImage` only; `fillPath`/`strokePath`/`drawText`/`measureText` have zero call-site evidence yet in this codebase slice and their design still rests entirely on the not-yet-lifted files.

---

## 3. Mapping to proposed Canvas methods + explicit misfits

| GDI call / pattern | Proposed Canvas method | Fits cleanly? | Notes |
|---|---|---|---|
| `::StretchDIBits(hdc, dst rect, src rect, bits, BITMAPINFO*, DIB_RGB_COLORS, rop)` | `drawImage` | **Mostly yes**, with caveats below | Maps to "blit a decoded RGBA image into a dest rect, optionally cropped from a src rect." `DIB_RGB_COLORS` mode + `GetBitsAddress()`/`GetBitmapInfoAddress()` is exactly the "RGBA image decoded once at load" case the design assumes — `CDIB` already holds raw pixel bits. |
| `rop` parameter on `CDIB::Draw` overloads C & D (`int rop`, only `SRCCOPY` ever passed in current call sites) | **Does NOT fit `drawImage` as a same-signature passthrough** | **FLAG** | `StretchDIBits`'s raster-op parameter is a general Win32 ROP code (`SRCCOPY`, `SRCAND`, `SRCPAINT`, `SRCINVERT`, `NOTSRCCOPY`, etc.), i.e. bit-level combine-with-destination logic. Today every call site in this repo slice passes `SRCCOPY` (plain overwrite), so *as currently used* `drawImage` with plain alpha-composite semantics is sufficient. But the parameter still exists in the API and its historical purpose in Comic Chat was almost certainly transparency/masking via ROP codes on the *original* 256-color art pipeline (the classic "AND-mask then XOR/OR-mask" or "SRCAND doorway" trick used before real alpha existed). Plan 2 should decide explicitly: (a) confirm via the not-yet-lifted callers (bodycam.cpp/panel.cpp) that no caller ever requests a non-`SRCCOPY` rop, or (b) keep a `blendMode`/`rop` enum on `drawImage` if any surviving caller needs anything but straight copy. Do not silently drop the parameter without checking those callers first — they are exactly the files this discovery pass could not see. |
| `dc->FillSolidRect(panelRect, RGB(255,255,255))` | **Does not fit any of the 5 proposed methods** | **FLAG** | This is a flat, solid-color axis-aligned rectangle fill — not an image (no source pixels), not really "fillPath" in the spline/vector sense the design describes (though a Canvas `fillPath` *could* subsume it if given a simple rect path). It's used as a fallback ("white background if we can't find the art"). Recommend either: (1) special-case it as `fillPath` with a literal rect path, or (2) add a lightweight `clear`/`fillRect(rect, color)` Canvas method — cheaper than routing a 4-point rectangle through a general spline/path API for what's just an erase-to-white. |
| `dc->IsPrinting()` | **Does not fit any of the 5 proposed methods** | **FLAG (design gap, not a drawing primitive)** | This is a *query*, not a draw call — it selects which backdrop art cache (screen vs. printer, `backMapS` vs `backMapP`) and DPI-relevant scaling path to use. Plan 2's Canvas needs *some* way to answer "is this a print/off-screen target" or Plan 2 must eliminate the print path entirely (macOS printing model differs enough that this may be intentionally dropped — needs an explicit decision, not silent removal). |
| `GetSafeHdc()` | N/A — plumbing, not a drawing primitive | fits by elimination | Purely extracts the raw device-context handle to pass into the Win32 call; once `StretchDIBits`/`FillSolidRect` are replaced by Canvas methods, `GetSafeHdc()` calls disappear entirely — no Canvas equivalent needed. |
| `GetBrushOrgEx` / `SetStretchBltMode` / `SetBrushOrgEx` (Block A, `MEMDC_NOT_STRETCHED`) | N/A — dead code | **FLAG (but low priority)** | Entire block is inside `#ifdef MEMDC_NOT_STRETCHED`, a macro that is never `#define`d anywhere in this tree (confirmed by grep — no `#define MEMDC_NOT_STRETCHED` exists). This was already dead in the original Win32 build too. Safe to drop entirely during Plan 2's Canvas rewrite; flagging only so it isn't mistaken for a live requirement. |
| `DIB_RGB_COLORS` mode flag on `StretchDIBits` | N/A — API detail absorbed by `drawImage`'s image representation | fits by elimination | Since Canvas commits to "RGBA images decoded once at load," the DIB color-table/`DIB_RGB_COLORS` machinery (and all of `dib.cpp`'s 8bpp/4bpp RLE decode, `MapColorsToPalette`, palette remapping in `CDIB::MapColorsToPalette` guarded under `#if 0`) is pre-Canvas decode-time concern, not something `drawImage` itself needs to know about — as long as the image is already RGBA by the time it reaches Canvas. |

### Things that do NOT fit the five proposed Canvas methods — condensed flag list

1. **Raster-op (`rop`) parameter on `StretchDIBits`** — general bit-combine codes (SRCCOPY/SRCAND/SRCPAINT/SRCINVERT/etc.), currently always `SRCCOPY` in visible call sites, but the parameter is plumbed through 2 of 3 `CDIB::Draw` overloads and its historical use (mask-based transparency) is a known GDI idiom not visible in this discovery slice. **Needs explicit design decision**, not implicit "drawImage handles it."
2. **`FillSolidRect(rect, RGB)`** — flat rect fill, doesn't cleanly map to `drawImage` (no image) or `fillPath` (not really a "path" in the spline sense) without stretching one of those APIs.
3. **`dc->IsPrinting()`** — a DC *query*, not a draw call; determines screen-vs-print asset selection. No Canvas method category covers "ask the canvas what kind of surface it is." Needs either a Canvas capability query or an explicit decision to drop print support.
4. **Dead `MEMDC_NOT_STRETCHED` block** (`GetBrushOrgEx`/`SetStretchBltMode`/`SetBrushOrgEx`) — not live functionality, flagged only to confirm it's discardable, not something Plan 2 needs to model in Canvas.
5. **Absence of palette calls, path/spline calls, and text calls in the currently-lifted code** — the two files inventoried here (`backdrop.cpp`, `dib.cpp`) contain zero calls in the palette, path-drawing, or text categories. The `fillPath`/`strokePath`/`drawText`/`measureText` portions of the proposed Canvas API have **no supporting evidence from this discovery pass** — they rest entirely on files not yet lifted into this SwiftPM target (`bodycam.cpp` for body/pose rendering, `panel.cpp`/`wmini.cpp`/`balloon.cpp` for panel borders and speech-balloon tails/text, per the `cc_link_stubs.cpp` header comments citing those exact owning files and line numbers). Recommend Plan 2 do a matching discovery pass over those files (once lifted) before finalizing the path/text portions of the Canvas interface.

---

## 4. CDC* pointer-level uses outside CC_NO_RENDER blocks (grep `CDC` across engine/*.cpp, *.h)

Full grep of `CDC` across all `.cpp`/`.h` in the engine dir — every hit is a **signature**, not a call, and every hit is in a `Draw`/`DrawBody` method:

| File:line | Signature | Status |
|---|---|---|
| `backdrop.cpp:336` | `void CBackDrop::Draw(CDC *dc, RECT *panelRect, RECT *)` | Definition; body is the CC_NO_RENDER Block A above |
| `backdrop.h:46` | `void Draw(CDC *dc, RECT *panelBox, RECT *dmgBox);` | Declaration matching backdrop.cpp:336 |
| `backdrop.h:47` | `virtual void Draw(CDC* dc, POINT* ul, RECT *rect) { ASSERT(0); }  // never to be called` | Override of `CPanelElement::Draw` — inline no-op stub, **not** a CC_NO_RENDER block (small enough it's just inline `ASSERT(0)`, never guarded by the macro at all) |
| `pe.h:17` | `virtual void Draw(CDC* dc, POINT* ul, RECT *rect) = 0;` | Pure virtual base — `CPanelElement::Draw`, the abstract root all panel-element drawing overrides |
| `dib.h:32` | `virtual void Draw(CDC* pDC, int x, int y);` | Declaration matching Block B |
| `dib.h:33-36` | `virtual void Draw(CDC* pDC, int x, int y, int destWidth, int destHeight, int rop = SRCCOPY);` | Declaration matching Block C (note the header-side `SRCCOPY` default) |
| `dib.h:37-38` | `virtual void Draw(CDC* pDC, int destX, int destY, int destWidth, int destHeight, int srcX, int srcY, int srcWidth, int srcHeight, int rop);` | Declaration matching Block D |
| `dib.cpp:164,185,206` | (definitions) | Match Blocks B, C, D |
| `avatar.h:94` | `virtual RECT DrawBody(CDC *dc, RECT &clientArea, BOOL drawNimbus) = 0;` | Pure virtual on `CBody` (base class) — **not yet lifted body**, real implementations live in `bodycam.cpp` (not in this tree) |
| `avatar.h:142-143` | `virtual RECT DrawBody(CDC *dc, RECT &clientArea, BOOL drawNimbus);` / `virtual void Draw(CDC *dc, POINT *ul, RECT *dmgRect);` | `CBodyDouble` overrides — declarations only; bodies stubbed in `cc_link_stubs.cpp` (ASSERT(0) traps), real bodies in `bodycam.cpp:516,578` per stub comments |
| `avatar.h:160-161` | same shape | `CBodySingle` overrides — declarations only; stubbed in `cc_link_stubs.cpp`, real bodies in `bodycam.cpp:589,614` |
| `cc_link_stubs.cpp:54,61,84,91` | `CBodySingle::DrawBody`, `CBodySingle::Draw`, `CBodyDouble::DrawBody`, `CBodyDouble::Draw` — all params commented out (`CDC* /*dc*/`) | **Trap stubs only** — `ASSERT(0)` bodies, no real drawing code present in this tree yet. These are the biggest un-inventoried surface: real GDI bodies for avatar body/head rendering live in `bodycam.cpp`, which is **not part of this lift** and thus contributed **zero** GDI calls to this report. Plan 2 must treat `bodycam.cpp` (and `panel.cpp`/`wmini.cpp`/`balloon.cpp`) as a follow-up discovery target before the Canvas interface can be considered complete — path/text/palette categories are expected to originate there. |

### Signature-level reroute summary

Every `CDC*`-taking signature found (8 distinct signatures across 5 files: `backdrop.cpp/h`, `dib.cpp/h`, `pe.h`, `avatar.h`, `cc_link_stubs.cpp`) already takes `CDC*` as its first (or first non-this) drawing-related parameter, consistent with the Plan 2 approach of rerouting by changing what `CDC*` (or its replacement `Canvas*`) points to / is defined as, rather than needing new plumbing to get a DC reference into these call sites. The one exception to double check in Plan 2: `CBackDrop::Draw`'s 3-arg overload (`CDC*, RECT*, RECT*`) is a *different overload* from the virtual `CPanelElement::Draw(CDC*, POINT*, RECT*)` it also implements as a no-op stub (`backdrop.h:47`) — Plan 2 should confirm which of the two is the real call path used by the panel-rendering system (likely the 3-RECT* one, called directly by whatever owns panel layout, given the POINT*-based virtual is explicitly marked "never to be called").

---

## 5. Files with no CC_NO_RENDER content (confirmed via grep, listed for completeness)

`avatar.cpp`, `avatario.cpp`, `avbfile.cpp`, `bbox.h`, `avatar.h`, `avatario.h`, `avbfile.h`, `pe.h`, `backdrop.h`, `dib.h`, `vector2d.cpp`, `vector2d.h`, `cc_link_stubs.cpp` — none contain a `CC_NO_RENDER` block. `avatar.cpp` contains `CC_NO_UI` (session/doc layer, e.g. `theApp`/`GetChatDoc()` calls) and `CC_NO_DIRSCAN` (directory enumeration) guards, which are separate concerns from rendering and out of scope for the Canvas method derivation.
