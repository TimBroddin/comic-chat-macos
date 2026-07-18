# Layout Metrics Discovery — Plan 4 (real CoreText layout vs the fake recording metrics)

Scope: discovery task 3 of `docs/superpowers/plans/2026-07-18-plan-4-handoff.md` — decide
whether Plan 4 switches balloon/panel LAYOUT from the deterministic fake-metrics recording
canvas to real CoreText metrics, and what that costs. All findings are read-only observations
of `macos/ComicChatKit/Sources/` + `Tests/`, the read-only original in `v2.5-beta-1-modern/`,
and one standalone CoreText probe run outside the package build (numbers in §3).

**Verdict up front: switch, EARLY in Plan 4 (before the comic `NSView`'s geometry work), and
keep the fake-metrics canvas permanently as the deterministic golden harness (two-mode). The
re-baseline cost is near zero because every byte-exact golden already carries its own fake
metrics canvas; the blocking precondition the roadmap feared (Capitalize) was already restored
in Plan 3 Task 2. Full rationale in §6.**

## 1. The metrics plumbing as it stands

**Definition.** `cc_set_metrics_canvas` is declared at
`macos/ComicChatKit/Sources/cchat-engine/include/comicchat.h:164` and defined at
`macos/ComicChatKit/Sources/cchat-engine/shim/engine_context.cpp:48-53`: it stores the
`cc_canvas*` into `ccContext().metricsCanvas`
(`shim/engine_context.h:135`) and resets the cached `CDC`. Layout code reaches it through
`CCEngineContext::metricsDC()` (`engine_context.cpp:56-62`), which lazily builds a `CDC`
bound to the registered canvas and **ASSERTs if none is registered** — the port's replacement
for the original's shared MM_TWIPS desktop `CClientDC` (`engine_context.h:137-151`).

**Split between layout and drawing.** Layout happens entirely at ingest time:
`cc_strip_add_line` / `cc_strip_add_line_cooked` call `page->AddLine(...)`
(`bridge/cc_compose.cpp:267-292, 310-355`), and everything under it (balloon make/wrap/size,
panel fit) measures through `ccContext().metricsDC()` — i.e. through whatever
`cc_set_metrics_canvas` installed. Drawing happens later: `cc_strip_compose(s, canvas)` binds
a fresh `CDC dc(canvas)` to the **compose argument** and walks `panel->Draw(&dc, ...)`
(`bridge/cc_compose.cpp:412-440`). The two canvases are independent.

**Every caller installs a fake-metrics recorder — there is no real-metrics layout path in the
tree today.** Callers of `cc_set_metrics_canvas`:

| Caller | Canvas installed |
|---|---|
| `Sources/cc-dumpart/DemoStrip.swift:34-35` (`--strip`) | `RecordingCanvas` |
| `Sources/cc-dumpart/ScriptStrip.swift:15-16` (`--script`) | `RecordingCanvas` |
| `Sources/cc-dumpart/ReplayStrip.swift:272-273` (`--replay`, the Plan 3 exit path) | `RecordingCanvas` |
| `Tests/ComicChatKitTests/StripTests.swift:57, 96` | `RecordingCanvas` |
| `Tests/ComicChatKitTests/StripScriptTests.swift:29, 80` | `RecordingCanvas` |
| `Tests/ComicChatKitTests/ProtocolToStripTests.swift:62, 103, 134, 157` | `RecordingCanvas` |
| `Sources/cchat-engine/bridge/cc_selftest.cpp:704, 1083, 1155, 1694, 2324` | C++ `CCRecordingCanvas` |

(`Sources/ComicChatKit/StripScript.swift:196-199` documents the contract: `render()` requires
a metrics canvas to be pre-installed.) So the **production compose path — all three cc-dumpart
modes, including the Plan 3 exit-milestone replay — lays out with FAKE metrics and draws with
real CoreText**, which is exactly the roadmap's "balloon text sits high" divergence.

**Fake canvas values** (`Sources/ComicChatKit/RecordingCanvas.swift:39-47`, byte-identical C++
twin `bridge/cc_recording_canvas.cpp:62-63`):
- `measureText` → `(len * 120, 240)` twips regardless of font/content.
- `fontMetrics` → `{height:240, ascent:190, descent:50, internalLeading:40,
  externalLeading:20, aveCharWidth:120, maxCharWidth:240}` regardless of font.

**CGCanvas's real/heuristic mix** (`Sources/ComicChatKit/CGCanvas.swift:205-218`):
- REAL: `ascent = CTFontGetAscent*20`, `descent = CTFontGetDescent*20`,
  `externalLeading = CTFontGetLeading*20`, `height = ascent + descent`.
- PLACEHOLDER: `internalLeading = 0`; `aveCharWidth = ((ascent+descent)/2)`;
  `maxCharWidth = 2 * aveCharWidth` (the three roadmap-flagged heuristics).
- `measureText` (`CGCanvas.swift:190-203`) is real: `CTLineGetTypographicBounds` width,
  `ascent+descent+leading` height, rounded to twips.

The `CDC` adapter forwards both: `GetTextExtent` → `measure_text`
(`shim/mfc_compat.h:911-916`), `GetTextMetrics` → `font_metrics` with `tmCharSet` filled from
the *requested* `lfCharSet` (`shim/mfc_compat.h:924-938`).

## 2. What layout actually consumes

**TEXTMETRIC fields — grep-verified across `Sources/cchat-engine/engine/`:**
- `tm.tmHeight` + `tm.tmExternalLeading`: the ONLY live layout consumers, in the `CFontInfo`
  ctor (`engine/balloon.cpp:622-666`): `m_leading += tmExternalLeading` (:643),
  `m_lineHeight = tmHeight + m_leading` (:661). `m_lineHeight` then drives everything
  vertical: line stepping in `DrawFormattedText` (:938, :978, :1023), balloon bbox bottom
  (`CLabel::BreakIntoLines`, :732), `AreaEstimate`'s returned line height (:751), and
  max-lines-per-balloon in both `SplitHeight`s (:834, :1613).
- `tm.tmCharSet`: `engine/fonts.cpp:56` (font-substitution retry) and `:77`
  (Far-East-italic whisper gate) — filled from the requested charset by the shim, canvas
  metrics not involved.
- `tm.tmDescent`: only inside a commented-out block (`balloon.cpp:632-641`); `tmAscent`/
  `tmDescent` in `format.cpp` are likewise commented out (:802-804, :873-879).
- **`tmAveCharWidth`, `tmMaxCharWidth`, `tmInternalLeading`: ZERO live consumers in the
  lifted engine.** The only original consumer of `tmMaxCharWidth` was `intl.c:880`
  (`v2.5-beta-1-modern/intl.c`), which is not lifted — its `FindSubStringForINTLThatFits` is
  an unreachable trap stub (`engine/cc_link_stubs.cpp:117-124`, guarded by `GetMime()!=NULL`
  which is permanently NULL under the CP-1252 posture). **The roadmap's "these three
  heuristics feed balloon sizing / line wrap" is overstated: the placeholder fields are
  write-only today. What actually feeds layout is `tmHeight`, `tmExternalLeading`, and
  `measureText`.** (Fix the placeholders anyway — cheap, and they stop being write-only the
  day any further original code lifts.)

**`measureText` (`GetTextExtent`) consumers — these are the real geometry drivers:**
- `GetFormattedTextExtent` (`engine/format.cpp:723-890`; unformatted fast path :738) — the
  universal measure wrapper; height is the max `size.cy` across runs (:801, :877).
- Line wrap: `ForceLineBreak` (`engine/balloon.cpp:223-250`, width-vs-`iMaxWidth` loop) and
  `FindFurthestLineBreakIntl` (:253+) → the free `::BreakIntoLines` → `CLabel::BreakIntoLines`
  (:707-741): per-line widths → widest line → balloon bbox.
- Balloon/panel sizing: `CLabel::AreaEstimate` (:744-755, `1.3 * cx * (cy + lineHeight)`) and
  `CLabel::WidestWord` (:758-787, minimum balloon width) — consumed by
  `CUnitPanelPage`'s balloon layout (`engine/panel.cpp:939, 948`), which decides panel fit
  and therefore panel breaks and `strip.size`.
- `CFontInfo::m_continuationWidth` (:663-664), balloon split continuation.
- `SzControlLess` (`format.cpp:324`) is in this question's list but consumes **no** metrics —
  it strips control codes and builds the formatting array that the measurers take as input.

**The exact "sits high" mechanism.** Draw-time text runs through
`CLabel::iDrawFormattedTextLine` (`balloon.cpp:1030`), which re-measures each chunk **on the
COMPOSE canvas** (:1096/:1099) and vertically centers within the layout-time line cell:
`TextOut(x, iBaseY - (m_fontI->m_lineHeight - size.cy)/2, ...)` (:1112); `CGCanvas.drawText`
then converts that top-of-box to a baseline by subtracting the REAL ascent
(`CGCanvas.swift:245-247`). At the default 12pt (`lfHeight = -240`,
`bridge/cc_compose.cpp:90-96` from `session.comicsFontPts`; `engine_context.h:58`):
fake `m_lineHeight = 240 + (-40*(240/180) + 20) ≈ 207` twips, while the real CoreText line
box `size.cy ≈ 334` twips (§3) — the centering term is ≈ **−63 twips, pushing every line's
box ~3pt UP** in y-up space. Layout boxed the balloon for 207-twip lines; drawing renders
334-twip glyph runs into it. That is the divergence, seen from the code side; the test side
documents the same thing at `Tests/ComicChatKitTests/StripTests.swift:140-160`.

## 3. GDI ground truth vs CoreText (Comic Sans MS, 12 pt = 240 twips)

Measured on this machine with a standalone CoreText probe (Comic Sans MS ships with macOS —
`/System/Library/Fonts/Supplemental/Comic Sans MS.ttf`; `unitsPerEm = 2048`):

| Field | GDI meaning (TrueType) | Correct CoreText replacement | Real value @12pt (twips) | Fake | Current CGCanvas |
|---|---|---|---|---|---|
| `tmAscent` | `usWinAscent` × pt/upem | `CTFontGetAscent` | **264.5** (= 2257 units — matches GDI exactly) | 190 | 264 (real) |
| `tmDescent` | `usWinDescent` × pt/upem | `CTFontGetDescent` | **70.0** (= 597 units) | 50 | 70 (real) |
| `tmHeight` | ascent + descent (cell height) | sum of the above | **334.5** | 240 | 334 (real) |
| `tmInternalLeading` | cell height − em; with negative `lfHeight`, GDI maps \|lfHeight\| to the EM, so `il = tmHeight − |lfHeight|` | `(ascent+descent)*20 − |spec.height|`, clamp ≥ 0 (exact: `(winAsc+winDesc−upem)` × scale) | **94.5** | 40 | 0 (placeholder) |
| `tmExternalLeading` | `hhea.lineGap` × pt/upem | `CTFontGetLeading` | **0.0** (Comic Sans has no line gap) | 20 | 0 (real) |
| `tmAveCharWidth` | `OS/2.xAvgCharWidth` × pt/upem — in fonts of this era the WEIGHTED average of lowercase a–z + space (OS/2 v≤2 English-frequency weights); the classic app-side approximation is `GetTextExtent("A…Za…z", 52)` then `(cx/26+1)/2` (dialog-base-unit convention) | best: `CTFontCopyTable(kCTFontTableOS2)` → scale `xAvgCharWidth` (exact GDI parity); cheap: measure `"abcdefghijklmnopqrstuvwxyz"` ÷ 26 | **124.9** (a–z avg; 52-char convention gives 145.9) | 120 | 167 (placeholder `(asc+desc)/2`) |
| `tmMaxCharWidth` | `hhea.advanceWidthMax` × pt/upem (widest ADVANCE) | `CTFontCopyTable(kCTFontTableHhea)` → `advanceWidthMax`; or max measured advance over printable ASCII. **Not** `CTFontGetBoundingBox` — that is INK bounds (probe: 307 tw) not advance | **249.5** ('W' advance) | 240 | 334 (placeholder `2*ave`) |

Notes:
- CoreText's ascent/descent for this font are numerically identical to GDI's (both read the
  same `usWin`/`hhea` values from the same-lineage font file), so **real CoreText layout
  approximates the original Windows client's layout, and the fake metrics diverge from
  BOTH** — heights by ~40% (240 vs 334.5).
- Widths are already close: `"Hello there"` measures 1275 twips real vs 1320 fake
  (len×120). The user-visible divergence is vertical, not horizontal.
- GDI quantized metrics to device pixels before the twips mapping; byte-exact parity with the
  1998 client is neither achievable nor needed (§6: nothing layout-derived crosses the wire).

## 4. Capitalize status — RESTORED; not a Plan 4 precondition anymore

The roadmap's "Capitalize R11 no-op is a LATENT REAL-METRICS LAYOUT DIVERGENCE"
(`2026-07-17-macos-port-roadmap.md:87-93`) was discharged by **Plan 3 Task 2 (commit
`4f311f9`)**: `Capitalize` is restored verbatim at `engine/balloon.cpp:145-190` (sole reroute:
`theApp.m_charSet` → `ccContext().session.charSet`, R17), live at its original call sites
(`balloon.cpp:1530` — `CBWoodringNormal` ctor uppercases every normal balloon; `:1741`;
`engine/panel.cpp:1443`), against the R9 NLS shim surface (`shim/mfc_compat.h:289-338`), with
a dedicated selftest (`bridge/cc_selftest.cpp:2779-2812`, wired at `:4498`). The ledger
confirms (`.superpowers/sdd/progress.md`, "P3 Task 2" entry: "Capitalize restored verbatim
(Plan 2 no-op → real fold)"). Nothing to restore in Plan 4.

**Residual, and newly relevant:** the `CharUpperBuff` shim folds ASCII only
(`mfc_compat.h:315-336`); CP-1252's accented range passes through unfolded (the handoff's
first must-own item). Under fake metrics this was rendering-only (every byte measures 120
twips, so wrong case never changed a width). **Under real metrics, case changes measured
width, so the fold-table gap graduates to a layout-affecting divergence for accented text.**
Do the deterministic CP-1252 fold table (0xE0–0xFE → 0xC0–0xDE except 0xF7 `÷`; plus
0x9A/0x9C/0x9E → 0x8A/0x8C/0x8E and 0xFF → 0x9F) before or with the metrics switch.

## 5. Blast radius of switching

**Byte-exact goldens that encode fake metrics — none need re-baselining under two-mode:**
- `Tests/ComicChatKitTests/Fixtures/strip-golden.txt` — the frozen recording-canvas op log,
  compared line-for-line in `StripTests.swift:47-88`. It installs its OWN fake metrics canvas
  (:57), so it stays green regardless of what the app does. It remains valuable as the
  bridge-fidelity + layout-determinism proof, which is what it always tested (it never
  attested visual truth).
- The C++ selftest snapshots/characterizations in `bridge/cc_selftest.cpp` (e.g. the frozen
  balloon wrap `3 lines {600,1080,360}` twips, bbox `1280×751` — ledger Plan 2 Task 6 entry)
  install their own recorder (`cc_selftest.cpp:704, 1155, 1694, 2324`) — untouched.
- One test hard-codes a fake-metrics-derived pen position — `StripTests.swift:176-186` draws
  `"Hello there"` at `(735,-80)`, lifted from `strip-golden.txt` line 13 — and it also
  installs the fake metrics itself (:96), so it stays valid.

**No pixel goldens exist.** The CGCanvas tests are heuristic (non-white fraction / dark-pixel
count, `StripTests.swift:218-263`; `ProtocolToStripTests.swift:153-198`;
`CGCanvasMirrorTests.swift` is drawImage-only), and the exit PNGs are git-ignored human
artifacts under `.superpowers/sdd/`, not test inputs. A real-metrics layout will move balloon
geometry, but "some dark pixels exist where text was drawn" style assertions survive;
worst case a threshold nudge, not a re-baseline.

**Determinism.** For a fixed font file + macOS version, `CTFontGet*` values are straight
table reads — stable. `CTLineGetTypographicBounds` widths are shaping-dependent and CAN drift
across macOS releases or an Apple font-file update (rare for Comic Sans MS, but possible).
This is a personal project whose `swift test` runs on this machine only, so the practical
risk is low — but adopt the rule anyway: **never freeze byte-exact goldens under real
metrics**; real-metrics assertions use tolerances/invariants (balloon box ⊇ text box,
centering within ±N twips). The fake canvas is what byte-exact goldens are for.

**Engine constraint (real, but all current callers already conform):** fonts are measured
once per strip at `cc_strip_create` → `SetFonts` → `CFontInfo`/`m_lineHeight`
(`bridge/cc_compose.cpp:130-137`, `engine/fonts.cpp:41-95`). The metrics canvas must be
installed BEFORE `cc_strip_create`, and swapping it mid-strip does not re-measure existing
`CFontInfo`s (`cc_set_metrics_canvas` resets only the `CDC`). Any Plan 4 app code that
rebuilds strips (e.g. on font-size settings change) must destroy + recreate the strip, which
the one-room re-render flow does anyway.

**Does the fake canvas remain needed? Yes — two-mode is the design:**
- Fake (RecordingCanvas / CCRecordingCanvas): all byte-exact goldens and selftests, forever.
- Real: the app and cc-dumpart's default output path. Recommend a small **measurement-only
  `CTMetricsCanvas`** (real `fontMetrics`/`measureText`, draw ops unreachable) rather than a
  dummy 1×1 `CGCanvas`, because the metrics canvas must outlive any bitmap and "must work
  with no drawing surface active" (`Canvas.swift:109-111`) — and because the layout-time
  canvas and compose-time canvas are different objects with different lifetimes
  (`Canvas.swift:160-169`'s LIFETIME note).

Since layout and drawing would then BOTH be CoreText over the same `FontSpec`, the
draw-time centering at `balloon.cpp:1112` becomes self-consistent (measure ≈ cell), which is
precisely what kills the sits-high artifact.

## 6. Recommendation

**Switch during Plan 4, as an early task — immediately after the app skeleton and before the
comic `NSView`'s sizing/scrolling/incremental-render work.** Keep the fake path as the
permanent test harness.

Rationale, in order of weight:
1. **Interop cannot be affected — confirmed.** The wire carries text + annotations (pose,
   avatar identity, formatting; encoder `bInsertAnnotations` protsupp.cpp:3057-lineage,
   decoders `ProcessUDIData`/`ProcessSay` — see the Plan 3 discovery reports); no
   layout-derived geometry is ever serialized, and the protocol path
   (`ircsock.cpp`/`protsupp.cpp`/`ircproto.cpp`/`cc_session.cpp`) has zero
   `metricsDC`/canvas consumers (§2's grep: consumers are exclusively `fonts.cpp` +
   `balloon.cpp`). Plan 3's §8.2 byte-compare compares protocol bytes, not pixels. Layout
   metrics are a purely local, per-client rendering concern — the divergence is cosmetic by
   construction, and switching cannot regress interop.
2. **The app's visual quality demands it.** Fake metrics understate the line cell by ~40%
   (207 vs ~281–334 twips at 12pt, §2/§3); every balloon in the actual product renders its
   text ~3pt high per line. Shipping the Mac app on fake metrics means shipping the artifact.
3. **Fidelity points the same way.** CoreText reads the same font tables GDI did — real
   metrics move the Mac layout TOWARD the original Windows client (§3), which is what the
   spec §8 side-by-side acceptance will eyeball. Doing the switch before that acceptance
   makes the comparison meaningful.
4. **Do it early to avoid churn.** Metrics change `strip.size` and panel breaks; the comic
   view's geometry, scroll math, and any new visual baselines should be built once, on the
   final metrics — not built on fake metrics and revisited.
5. **The cost is small.** Two-mode keeps every existing golden green (§5). The work: (a) a
   `CTMetricsCanvas` + replacing the three placeholder `fontMetrics` fields with the §3
   table's formulas (they're write-only today, so this is debt-retirement, not behavior
   risk); (b) the CP-1252 `CharUpperBuff` fold table (§4 — now layout-relevant, and already a
   must-own); (c) flip cc-dumpart's default install to real metrics with a
   `--fake-metrics` escape hatch for reproducing goldens; (d) one or two tolerance-based
   real-metrics regression tests (in the `.serialized` engine-global suite tree, metrics
   canvas installed before `cc_strip_create`); (e) re-render the demo/replay PNGs and
   eyeball them (the "prove it with a real artifact" discipline).

Suggested task shape inside Plan 4: **Task A** metrics correctness (CTMetricsCanvas + real
TEXTMETRIC fields + fold table + tolerance tests), **Task B** flip the production default +
re-render/eyeball, then the comic-view tasks build on real geometry. Defer nothing except the
optional exact-GDI-parity table reads if measurement-based equivalents prove sufficient.

## Top risks / open questions for the planner

1. **CoreText width drift across macOS/font updates** → codify the rule "no byte-exact
   goldens under real metrics" in the plan text; real-metrics tests are tolerance-based only.
2. **The vkern tuning constants** (`fonts.cpp:89-95`: `-40/+30 × reduction` for Comic Sans)
   were tuned against GDI's 334-twip cell — they now finally apply to that intended cell, but
   verify balloons don't go tight (accents vs balloon edge, the original's own comment warns)
   during the §8 side-by-side; the Windows client is the arbiter.
3. **Font substitution is invisible to the engine**: the shim's `GetTextFace` returns the
   REQUESTED face (`mfc_compat.h:944-951`) and `tmCharSet` the requested charset (:936), so
   if CoreText silently substitutes (Comic Sans missing), the `doVKern` gate and metrics
   would disagree with what's drawn. Acceptable on macOS (font ships with the OS) — note it,
   maybe log when `CTFontCopyPostScriptName` ≠ requested.
4. **Mixed-mode is a trap**: real-metrics layout composed onto a `RecordingCanvas` would
   re-introduce a measure/cell mismatch in the opposite direction at `balloon.cpp:1112`
   (fake draw-time `size.cy=240` vs real 281-twip cells). Fine for op-grammar tests, but
   never write a test expecting old golden geometry from that combination.
5. **Open:** exact GDI parity for `aveCharWidth`/`maxCharWidth` (OS/2 + hhea table reads) vs
   measured equivalents (a–z average, max ASCII advance) — both defensible since the fields
   are write-only today (§2); pick one in the plan and document it in `CGCanvas`/
   `CTMetricsCanvas` so a future lift that consumes them inherits a stated convention.
6. **Open:** should `--fake-metrics` (or env var) be the flag spelling for cc-dumpart's
   escape hatch, and should `StripScript.render` grow an explicit metrics-mode parameter
   instead of the ambient pre-installed-canvas contract (`StripScript.swift:196-199`)? The
   ambient contract is easy to misuse from new app code (install-before-create constraint,
   §5).
