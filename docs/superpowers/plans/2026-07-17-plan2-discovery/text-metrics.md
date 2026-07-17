# Text Metrics & Measurement Discovery — Plan 2 (Comic Layout Engine)

Scope: balloon.cpp (1935 lines), wmini.cpp (949), panel.cpp (1583), pageview.cpp (1478),
plus format.cpp (text-metric helpers these files delegate to) and fonts.cpp (font
construction, discovered while tracing font origin). All findings are read-only
observations of `/Users/timbroddin/Projects/comic-chat/v2.5-beta-1-modern/`.

## 0. Critical preliminary finding: wmini.cpp is DEAD CODE, not compiled

`wmini.cpp` defines `BreakIntoLines`, `CBWoodringNormal::m_pen`,
`CBWoodringWhisper::m_nimbusPen`, and several `CB*::Draw`/measurement methods —
**the exact same symbols** that `balloon.cpp` defines (balloon.cpp:97-98 defines
`CBWoodringNormal::m_pen` / `m_nimbusPen` again). Two TUs defining the same statics
would fail to link if both were compiled into one binary.

Confirmed by checking the actual build file, `chat.mak`: its `OBJS` list includes
`balloon.obj`, `bodycam.obj`, `fonts.obj`, `format.obj`, `PageView.obj`, `panel.obj` —
but **no `wmini.obj`** anywhere. `wmini.cpp` is legacy/superseded source left in the
tree; it is not part of the shipping build. It also contains code that wouldn't
compile as-is (`wmini.cpp:63,85` reference undeclared locals `szLastLength`,
`iLineEnd` — typos for `iLastLength`/`szLineEnd`), reinforcing that it's stale.

**Implication for Plan 2**: the word-wrap / measurement logic to port is the one in
`balloon.cpp` (richer: per-run formatting, INTL/DBCS path, hotlinks). wmini.cpp's
parallel simpler implementation should be treated as historical reference only, not
as a second code path that needs replicating.

## 1. Measurement call-site inventory

### balloon.cpp (compiled / live path) — 10 raw GDI measurement calls, all funneled through 2 wrapper functions

| Line | Enclosing function | Call | Quantity computed | Feeds into |
|---|---|---|---|---|
| 199 | `ForceLineBreak` | `GetFormattedTextExtent(pdc, szString, iLength, prgdwFormatting)` | running width/height of string prefix, char-by-char (via `iBytesofChar` for DBCS) | last-resort break point when even one "word" is too wide for the balloon (character-level break) |
| 220 | `FindFurthestLineBreakIntl` | `GetFormattedTextExtent(pdc, szString, nBytes, ...)` | whole-remaining-string extent | decide if entire remaining string fits on one line (INTL/DBCS path) |
| 277 | `FindFurthestLineBreakIntl` | `GetFormattedTextExtent(pdc, szString, szFirstBreak-szString, ...)` | extent up to last whitespace before overflow | final width of the line when trimmed back to a space boundary |
| 303 | `FindFurthestLineBreak` (non-INTL) | `GetFormattedTextExtent(pdc, szString, iThisLength, ...)` | extent of candidate line (grown word-by-word via `GetNextEnd`) | word-wrap loop: "does one more word still fit?" |
| 367 | `BreakIntoLines` (the live wrap driver) | `GetFormattedTextExtent(pdc, szString, iThisLength, prgdwPulledFormatting)` | extent of candidate line, same word-by-word growth pattern as above | core per-line width test that produces `rgiWidths[]`/`rgszStarts[]`/`rgiLengths[]` |
| 589 | `CFontInfo::CFontInfo` ctor | `pdc->GetTextMetrics(&tm)` | `tmHeight`, `tmExternalLeading`, `tmCharSet` | `m_lineHeight = tmHeight + leading`; charset-based leading/baseline-add tuning; triggers Far-East top-offset flag |
| 625 | `CFontInfo::CFontInfo` ctor | `pdc->GetTextExtent(szContinuationStr1, strlen(...))` (raw, not via wrapper — string has no formatting) | width of the literal `"..."` string | `m_continuationWidth`, subtracted from available width when a balloon must truncate with an ellipsis (`SplitHeight`) |
| 709 | `CLabel::AreaEstimate` | `GetFormattedTextExtent(pdc, m_str, strlen(m_str), m_prgdwFormatting)` | extent of the **entire unwrapped string** (cx = "as if one line", cy = font extent height) | `return 1.3 * cx * (cy + lineHeight)` — a rough *area* heuristic used before any wrap has happened, to guess a starting balloon width/goal-width in `panel.cpp::GetCloudEstimate` |
| 738 | `CLabel::WidestWord` | `GetFormattedTextExtent(pdc, szStart, szEnd-szStart+1, prgdwPulledFormatting)` | width of a single whitespace-delimited token | establishes an absolute **minimum** balloon width (a balloon can never be narrower than its widest unbreakable word) — consumed by `panel.cpp:898` |

format.cpp (the shared helper both balloon.cpp and wmini.cpp call into):

| Line | Function | Call | Quantity | Notes |
|---|---|---|---|---|
| 707 | `GetFormattedTextExtent` (fast path) | `pdc->GetTextExtent(szInput, cbLen)` | plain single-font extent | Used when `!bSizorPresent(prgdwFormatting)` — i.e., no rich formatting runs in this string, so it's a straight passthrough |
| 758/761 | `GetFormattedTextExtent` (rich path, mid-loop) | `pdc->GetTextExtent(...)` per formatting run | per-run width, summed into total `cx`; `cy = max(cy, run cy)` | iterates the `CDWordArray` of `MAKELONG(wFormat, wOffset)` run-boundary records, rebuilding a `CFont` per run (bold/italic/underline/fixed-pitch/symbol/transparent) via `CreateFontIndirect`, measuring, restoring old font |
| 837/840 | `GetFormattedTextExtent` (rich path, tail run) | `pdc->GetTextExtent(...)` | width of final run after loop | same mechanism for the last format segment |

**wmini.cpp** (dead code, listed for completeness only): `GetTextExtent` at lines
55(`BreakIntoLines`), 664/714/747 (`CLabel`-equivalent methods), `GetTextMetrics` at
line 938. Same shape as balloon.cpp's live logic but without the rich-formatting
(`CDWordArray`) layer or the INTL/DBCS branch — an earlier, simpler iteration.

**panel.cpp**: zero direct GDI text-metric calls. It *consumes* the outputs of
balloon.cpp's measurement (`AreaEstimate`, `WidestWord`) — see the wrap-algorithm
trace below — but does no measurement itself.

**pageview.cpp**: zero GDI text-metric calls (`GetTextExtent`/`GetTextMetrics`/
`DrawText…DT_CALCRECT` all absent). It only *draws* text (footer, see §3) and
manages DC mapping mode / DPI (see §4). Not part of the balloon-layout measurement
path at all — its only text is the printed page footer ("Microsoft Chat", page
number, date), which is single-line, alignment-drawn (`TA_LEFT/CENTER/RIGHT`), no
measurement needed because Windows handles the alignment internally.

**Total raw `GetTextExtent`-family calls in the two shared measurement funnels**
(`GetFormattedTextExtent` in format.cpp, plus the one raw call in the `CFontInfo`
ctor for the continuation string): every measurement in balloon.cpp funnels through
exactly **one** function, `GetFormattedTextExtent`, except the ellipsis-width and
the `GetTextMetrics` line-height setup.

## 2. Word-wrap / balloon-sizing algorithm — high-level trace

Driving call chain (all in balloon.cpp / panel.cpp, `balloon.cpp` line refs):

1. **`CUnitPanel::LayoutBalloons`** (panel.cpp:855) — panel-level entry point;
   iterates all balloons in a panel, calls `LayoutBalloon` per balloon.
2. **`CUnitPanel::LayoutBalloon`** (panel.cpp:925) → calls
   **`GetCloudEstimate`** (panel.cpp:885) first: this is the *pre-wrap heuristic*.
   It calls `balloon->AreaEstimate(&len, &lineHeight)` (balloon.cpp:705, whole-string
   extent × 1.3 fudge factor) and `balloon->WidestWord()` (balloon.cpp:719, longest
   token) to guess a `goalWidth` for the balloon's bounding rect — this happens
   *before* any line-wrapping, purely from string-level metrics.
3. `LayoutBalloon` then calls **`balloon->SetBBox(brect.left, brect.bottom,
   brect.right, brect.top)`** (balloon.cpp:1396) with that guessed rect.
4. `CBalloon::SetBBox` calls **`ComputeInternals()`** (balloon.cpp:1407,
   `CBWoodringNormal::ComputeInternals` at line 1764) which is the real driver:
   - `BreakIntoLines(*m_fInfo)` (balloon.cpp:668, thin wrapper around the free
     function `::BreakIntoLines` at balloon.cpp:347) — **this is the actual
     word-wrap algorithm**. It walks the string word-by-word (`GetNextEnd`),
     accumulating candidate-line width via `GetFormattedTextExtent`
     (balloon.cpp:367) until a line overflows `iMaxWidth` (the balloon's desired
     interior width), then backs off to the last word boundary that fit
     (falling back to `ForceLineBreak`/character-level break if even one word is
     too wide). Produces `m_rgszStarts[]`, `m_rgiLengths[]`, `m_rgiWidths[]`,
     `m_nLines` in a `CFormatInfo`.
   - `ShiftLines(*m_fInfo)` (balloon.cpp:751) — per-line random horizontal jitter
     for the hand-drawn comic look, bounded by the line's slack
     (`m_iMaxWidth - m_rgiWidths[i]`).
   - `CreateBalloonSpline(*m_fInfo)` (balloon.cpp:1700) — the payoff: builds the
     wavy cloud-balloon outline (`CBeta` spline) by tracing left/right boundary
     points (`GetFilters`) derived directly from each line's `m_rgiLeftX` /
     `m_rgiWidths`. **The balloon's visual shape is molded around the wrapped
     text's per-line geometry.**
   - `ComputeCloudBBox()` — bbox of the resulting spline control points.
5. Separately, **`AddArrow`** (balloon.cpp:1466, called from `GetBalloonSpline`/
   `SetBalloonTraj`) reads `m_fInfo->m_rgiLeftX[nLines-1]` and
   `m_rgiWidths[nLines-1]` (the last line's horizontal extent) to steer the
   balloon's tail so it doesn't cover the last word and points sensibly at the
   speaking character.
6. If a balloon still doesn't fit the panel's free rect after this,
   **`CLabel::SplitHeight`** / **`CBWoodringNormal::SplitHeight`**
   (balloon.cpp:777, 1533) truncate the string to a maximum line count
   (`iHeight / m_fontI->m_lineHeight`) and re-measure the truncation point with
   **`FindFurthestLineBreak`** (balloon.cpp:287), appending a `"..."`
   continuation string sized via `m_fontI->m_continuationWidth` (from the
   `CFontInfo` ctor's one-time `GetTextExtent` on `"..."`).

So: **`BreakIntoLines` (balloon.cpp:347) is the wrap driver**; **`AreaEstimate`
+ `WidestWord` (balloon.cpp:705, 719) are the pre-wrap sizing heuristics**; and
the wrap's line-by-line output (`m_rgiWidths`, `m_rgiLeftX`) is reused
downstream for both spline-shape generation and tail placement — i.e.
measurement output is consumed at least 3 distinct times per balloon (sizing
guess, spline shape, arrow placement), not just once for line-breaking.

## 3. Font source — where CFontInfo's font comes from

Traced via `balloon.h:47` (`class CFontInfo { CFont* m_font; ... }`, constructed
at balloon.cpp:584) back to its only construction sites: **fonts.cpp**
(`CUnitPanelPage::SetFonts` / `UpdateTitleFonts`).

- `CUnitPanelPage::SetFonts(LOGFONT &logFont, COLORREF crTextColor)` (fonts.cpp:40)
  builds `m_fontBalloon` (`CreateFontIndirect(&logFont)`) and `m_fontWhisper`
  (italicized variant), then wraps each in a `CFontInfo` (`m_fiWNormal`,
  `m_fiWWhisper`) — this is the "speech balloon body text" font used by
  ordinary `CBWoodringNormal` balloons.
- `UpdateTitleFonts()` (fonts.cpp:98) derives `m_fontTitle`/`m_fontShout` from
  the same base `theApp.m_comicsFont` LOGFONT, scaled to panel width
  (`nFontHeightTitle = -576` twips, `nFontHeightShout = -252` twips, from
  defines.h:137-138), wrapped as `m_fiTitle`/`m_fiShout` — used for comic-title
  and "shout" style labels.
- `SetFonts` is called from exactly 3 places: `pageview.cpp:998` (on view
  creation), `proppage.cpp:1539/1558` (when the user changes the Format→Font
  dialog).

**The `LOGFONT` itself comes from `theApp.m_comicsFont` (chat.h:69), populated by
`CChatApp::InitializeComicsFonts()` (chat.cpp:375):**
- Default face name: string resource `ID_COMIC_FONT_NAME` = `"Comic Sans MS"`
  (chat.rc:2288).
- Default point size: string resource `IDS_DFLT_COMICSPNTSIZE` = `"12"`
  (chat.rc:2336), converted point→twips via `PointsToTwips()` (pageview.cpp:174).
- Bold/weight default: `IDS_COMICS_BOLD_DFLT` resource string.
- Charset: `GetCorrectCharSet()` (locale-dependent).
- **User override**: `setupdlg.cpp:490-491` — `RegQueryValueEx(hKey,
  "ComicsFont", ...)` reads a persisted `LOGFONT` blob from the registry,
  overwriting the resource-string default. So the effective font is
  **registry-configurable, defaulting to "Comic Sans MS" 12pt**, not a
  hardcoded, unconditional Comic Sans.
- A special-case string compare, `strcmp(szPhysFaceName, "Comic Sans MS") == 0`
  (fonts.cpp:83, 125), is used only to decide whether to apply a vertical-kerning
  fudge factor (`doVKern`) for line spacing — Comic Sans MS is special-cased for
  a cosmetic tweak, not as a requirement.
- **Ownership**: `CFont*` objects (`m_fontBalloon`, `m_fontWhisper`,
  `m_fontTitle`, `m_fontShout`) are `static` members of `CUnitPanelPage`,
  heap-allocated and tracked in a `CPtrList m_fonts`; `CFontInfo` objects
  likewise in `CPtrList m_fontInfos`. Both are torn down together in
  `CUnitPanelPage::DestroyFonts()` (fonts.cpp:144). `CLabel`'s comment
  ("font is reffed elsewhere", balloon.h:84) confirms `CLabel`/`CBalloon`
  instances never own the font — they hold a raw, non-owning `CFontInfo*`.

## 4. Text DRAWING call sites and shared state with measurement

| File:line | Call | Shares font/color state with measurement? |
|---|---|---|
| balloon.cpp:894 | `pdc->TextOut(fi.m_rgiLeftX[i], iBaseY, fi.m_rgszStarts[i], fi.m_rgiLengths[i])` (`CLabel::Draw`) | Yes — same `m_fontI->m_font` selected into `pdc` just before (line 883), same `pdc` used moments earlier by `GetFormatInfoCommon` (line 881) which itself calls `::BreakIntoLines` for layout. Same DC, same font object. |
| balloon.cpp:1074 | `pdc->TextOut(iLeftX, iBaseY - (m_fontI->m_lineHeight - size.cy)/2, ...)` (`iDrawFormattedTextLine`) | Yes, and notably **re-measures immediately before drawing**: `size = pdc->GetTextExtent(...)` (line 1058/1061) is called on the *same run* right before the `TextOut`, using a per-run rebuilt `CFont` (`font.CreateFontIndirect(&logFont)`, line 1048) that mirrors the exact bold/italic/underline/fixedpitch/symbol logic as `GetFormattedTextExtent` in format.cpp. This duplicated font-rebuild logic between measure and draw is a spot to unify in the port. |
| balloon.cpp:1254 | `pdc->TextOut(m_fInfo->m_rgiLeftX[i], ...)` (`CBWoodringNormal::DrawText`) | Yes — draws using the same `m_fInfo` (`CFormatInfo`) that `ComputeInternals`/`BreakIntoLines` populated; font selected at balloon.cpp:1783 (`Draw`) / consumed via `m_fontI->m_font`. |
| balloon.cpp:1125 | `DrawTextEx(pdc->m_hDC, fi.m_rgszStarts[0], -1, &rect, DT_LEFT\|DT_NOPREFIX\|DT_SINGLELINE\|DT_END_ELLIPSIS, NULL)` (`CStarLabel::Draw`) | Font selected identically (line 1119) but this call performs **OS-internal measurement + ellipsis truncation in one step** — no explicit `GetTextExtent` companion call exists for this one; Windows measures internally to decide where to truncate. This is the one drawing call that folds measurement into itself rather than consuming a pre-computed extent. |
| wmini.cpp:468/505/677 (dead code) | `TextOut` | N/A — unreachable |
| pageview.cpp:503/511/520 | `pDC->TextOut(...)` (`PrintFooter`) | Independent font (`m_footerFont`, its own `CFont` created at pageview.cpp:563 via `CreateFontIndirect`), independent of the balloon-measurement font entirely. Uses `SetTextAlign(TA_LEFT/TA_CENTER/TA_RIGHT \| TA_BOTTOM)` so Windows does the centering/right-alignment math — no manual measurement needed for this footer text at all. |

Font/color state is consistently threaded via `pdc->SelectObject(m_fontI->m_font)`
+ `pdc->SetTextColor(m_fontI->m_crDefaultForeColor)` immediately before both
measuring and drawing — i.e., **whichever font is currently selected into the DC
is implicitly "the" font for both GetTextExtent and TextOut**. There is no
independent font parameter passed alongside the string to either operation; it's
pure DC state.

## 5. DC state the measurement depends on

- **Font selection**: `SelectObject(CFont*)` on the DC immediately before
  every measurement/draw call, always saved/restored (`pOldFont`) around the
  call. `GetFormattedTextExtent` additionally does its own nested
  save/restore/rebuild-font cycle per formatting run (format.cpp:712, 725-726,
  741-763).
- **Mapping mode**: The **entire measurement/layout subsystem runs in
  `MM_TWIPS`.** The DC used for all balloon-layout measurement is a single
  global `CClientDC` on the desktop window, created once in
  `CPageView::OnCreate` (pageview.cpp:993-998):
  ```
  CClientDC *dc = new CClientDC(GetDesktopWindow());
  dc->SetMapMode(MM_TWIPS);
  InitializeDPI(dc);
  cui.m_pvClientDC = dc;
  ```
  retrieved everywhere via the macro `GetClientDC()` → `((CClientDC
  *)cui.GetClientDCPv())` (ui.h:7). None of the measurement call sites in
  balloon.cpp/format.cpp change the mapping mode — they inherit MM_TWIPS from
  this shared DC, so `GetTextExtent`/`GetTextMetrics` results come back
  directly in twips (1/1440 inch), the same unit as every balloon-layout
  constant (`XBOXDELTA`, `BUBBLEHEIGHT`, `MINROUTEWIDTH`, etc. in balloon.cpp's
  `#define`s) and the same unit `LOGFONT.lfHeight` is expressed in
  (`m_iFontHeightBalloon = PointsToTwips(atoi(strDefaultFontHeight))`,
  chat.cpp:381). This is why no explicit unit conversion appears anywhere
  around a measurement call — everything downstream (wrap width comparisons,
  spline point coordinates, arrow geometry) is already twips-for-twips
  consistent by construction of this one shared DC.
  - Two other, unrelated mapping-mode switches exist in pageview.cpp
    (`MM_TEXT` at line 1431, used transiently for something else; `MM_TWIPS`
    again at lines 620/995) but none of them touch the *client measurement DC*
    concurrently with a measurement call — no evidence of mapping-mode
    thrashing mid-measurement.
- **Text color / background mode**: `SetTextColor`/`SetBkMode(TRANSPARENT)`
  affect drawing only, not `GetTextExtent`/`GetTextMetrics` — safe to ignore
  for a `measureText` abstraction.
- **No DPI dependency beyond the one-time desktop DC setup**: `InitializeDPI`
  (pageview.cpp:617) and `PointsToTwips` (pageview.cpp:174) both use a
  transient `CClientDC(GetDesktopWindow())` at `MM_TWIPS` to read
  `LOGPIXELSY`/`LPtoDP` once, converting point sizes to twips at font-creation
  time — after that, no measurement call site queries DPI again.

## 6. Proposed measureText / drawText signature sketch

Given the above, here's what a `Canvas` abstraction needs to reproduce this
exactly:

**Font identity.** Nothing in the measurement path passes a font "spec" as a
parameter — it's always "whatever's currently selected in the DC," sourced
from a `CFontInfo*` that wraps one `CFont*` (a fixed face/size/weight/italic/
underline combination created once at startup or on Format>Font change). A
`Canvas`-based `measureText`/`drawText` should take an explicit, opaque font
handle (mirroring `CFontInfo`) rather than reading ambient state — e.g.:

```cpp
struct FontSpec {
    std::string faceName;   // from theApp.m_comicsFont.lfFaceName / registry override
    int         pointSize;  // or twips height, see units note below
    bool        bold;
    bool        italic;
    bool        underline;
    bool        fixedPitch; // maps to wFixedPitch run-format bit
    bool        symbol;     // maps to wSymbol run-format bit
};

class Canvas {
public:
    // Per-string extent, single uniform style. Mirrors pdc->GetTextExtent().
    virtual Size measureText(const FontSpec&, std::string_view text) = 0;

    // Line-metrics, mirrors pdc->GetTextMetrics(): needed once per font to
    // derive lineHeight/leading/baseAdd/topOffset (CFontInfo ctor logic).
    virtual FontMetrics measureFont(const FontSpec&) = 0;

    // Draws left-aligned at baseline-relative origin, mirrors TextOut().
    virtual void drawText(const FontSpec&, Point origin, Color,
                           std::string_view text) = 0;
};
```

**Per-string extents are sufficient — no per-character positions needed.**
Every layout decision (word-wrap break test, area estimate, widest-word,
continuation-string width, final line width for spline/arrow geometry) only
ever needs the extent (cx, cy) of a whole substring at a time — the algorithm
grows/shrinks the substring boundary (word-by-word via `GetNextEnd`, or
character-by-character in `ForceLineBreak`'s degenerate case) and re-measures
the *whole prefix* each time. It never asks for individual glyph advances or
per-character x-positions. **One exception**: `ForceLineBreak` (balloon.cpp:185)
does re-measure once per character when it must break mid-word (no whitespace
fits), but it still does so via whole-prefix `measureText` calls in a loop, not
a single batched per-glyph query — a naive port can keep that loop as-is
without needing a richer "positions" API. (An optional perf improvement would
add a `measureTextPositions`/cumulative-width array, but it is not required for
behavioral fidelity.)

**No average-char-width heuristic exists anywhere** in balloon.cpp/format.cpp —
every width decision is a real `GetTextExtent`-equivalent call, never an
estimate from `tmAveCharWidth` or similar. The one "estimate" in the codebase
(`CLabel::AreaEstimate`) is a *geometric area* heuristic (`1.3 * cx * (cy +
lineHeight)`) built from a real whole-string extent, not a char-width guess.

**Rich per-run formatting must be a first-class concept**, not bolted on:
`GetFormattedTextExtent` (format.cpp:692) is the one non-trivial measurement
wrapper — it splits a string into runs by a `CDWordArray` of
`(format-bits, offset)` pairs, measures each run with a font mutated from the
base font (bold/italic/underline/fixedPitch/symbol/transparent-background
toggles), and sums `cx` while taking `max` of `cy`. A `measureText` that
accepts a single `FontSpec` plus a list of `(offset, FontSpec delta)` run
overrides (or simply: caller pre-splits the string into runs and sums extents
itself, calling `measureText` once per run) would reproduce this without
needing the abstraction to understand "bold nibble" bit-packing at all — push
that decoding up into the porting layer, keep `Canvas` itself run-agnostic
(single style in, one extent out).

**Units**: the original always measures in twips because of the fixed
`MM_TWIPS` DC. The port doesn't need to preserve twips as a wire format, but
`measureFont`'s derived `lineHeight`/`baseAdd`/`leading` values feed directly
into balloon-geometry constants that are twips-scaled (`BUBBLEHEIGHT = 150`,
etc.) — so either keep twips as the internal coordinate system for the ported
comic layout (simplest, avoids re-deriving every magic constant), or introduce
one clean unit-conversion boundary right at the `Canvas` interface and rescale
every magic constant in balloon.cpp/panel.cpp consistently. Given how many
magic numbers exist throughout the layout math, preserving twips end-to-end
inside the ported layout engine (converting only at the final Canvas draw
call, e.g. to points/pixels) is almost certainly the lower-risk path.

**`measureFont` is needed once per font, not once per string** — mirrors
`CFontInfo`'s constructor pattern exactly: call it once when a font is
selected/created, cache `lineHeight`/`leading`/`baseAdd`/`continuationWidth`/
`topOffset` on the equivalent of a `CFontInfo`, and never call
`GetTextMetrics` again for that font. This should be preserved in the port for
both fidelity and performance (font-metric queries are the more expensive OS
call relative to a hot per-line `measureText`).
