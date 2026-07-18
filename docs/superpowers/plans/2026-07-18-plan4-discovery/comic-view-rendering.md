# Comic View & Rendering Discovery — Plan 4 (the Mac app)

Scope: the live comic view (discovery task 2 of the Plan 4 handoff). Sources read:
`macos/ComicChatKit/Sources/cchat-engine/bridge/cc_compose.cpp`, `engine/panel.{h,cpp}`,
`shim/mfc_compat.{h,cpp}`, `bridge/bridge_art.{h,cpp}`, `include/comicchat.h`, Swift
`Strip.swift`/`StripScript.swift`/`CGCanvas.swift`/`ProtocolStripBridge.swift`/`ProtocolSession.swift`,
`cc-dumpart` (all four modes), and the originals `v2.5-beta-1-modern/pageview.cpp`,
`chatview.cpp`, `panel.cpp`, `balloon.cpp`, `protsupp.cpp`, `histent.cpp`, `userinfo.cpp`,
`avatar.cpp`. Measurements were run on this machine (Apple M2 Ultra, macOS 26.5.1,
release build) with a scratchpad harness driving the **public ComicChatKit API only**
(nothing in the repo was modified; harness + synthesized inputs live in the session
scratchpad, not the repo). All paths below are absolute or repo-relative;
`engine/…`/`bridge/…`/`shim/…` mean `macos/ComicChatKit/Sources/cchat-engine/…`.

## 0. Headline conclusions

1. **Layout is already incremental; only drawing is full-page.** `cc_strip_add_line`
   lays out *only the last panel* (clone-and-replace or append — engine/panel.cpp:1152-1199);
   `cc_strip_compose` then redraws *every* panel with full damage (bridge/cc_compose.cpp:425-438).
2. **Naive full-recompose-per-message is affordable at MVP scale**: ~2.6 ms/panel at 2×
   scale ⇒ 133 ms for a 50-panel strip, 264 ms at 100 panels. Layout (`add_line`) is
   ~0.01-0.05 ms/line — free. PNG encode is only needed for save/export, not the view.
3. **The `StretchDIBits` decode debt is real but secondary**: the engine-side
   DIB→RGBA decode is ~0.5 ms/panel (~20 % of compose). The *bigger* per-blit fixed cost
   is on the Swift side — `CGCanvas.drawImage` copies the buffer and creates a fresh
   `CGImage` per blit (CGCanvas.swift:308-317); together they make compose cost mostly
   per-blit-fixed, not per-pixel (see §3.3). MVP does not need the cache.
4. **Recommendation (§5): option (a)** — recompose the whole strip into one backing
   `CGImage` per message on the engine's serial queue, draw it in a flipped, layer-backed
   `NSView` inside `NSScrollView`; original-faithful stick-to-bottom autoscroll; debounced
   reflow on resize (the original replays the whole transcript on reflow — we do the same).
   Per-panel tiles (option b) are the fast-follow, not the MVP.
5. **API gaps (§6)**: title/starring entry point, a change-participant-avatar call,
   `cc_avatar_icon_image`, a panel-geometry setter/getter pair, and (optional) hit-testing
   and per-panel compose. All are bridge-level additions; only title/starring un-R11s
   lifted code.

## 1. Question 1 — how composition works today

### 1.1 The full render path

| Step | Where | What happens |
|---|---|---|
| `cc_strip_create` | bridge/cc_compose.cpp:102-152 | `srand(0x5EED)` (:110, determinism), `InitializeAvatars()` + `session.clearUsers()` (:115-116), `InitializeEmotionRules()` (:125), `CUnitPanelPage::SetFonts` from `session.comicsFontFace/Pts` (:131-132, shim/engine_context.h:53-55), **hard-seeds unit-panel geometry to `MINUNITPANELWIDTH/HEIGHT` = 2300 twips** (:143-144, engine/panel.h:165-166), news a `CUnitPanelPage` (:149). One strip at a time, process-global registries (include/comicchat.h:214-227). |
| `cc_strip_add_participant` | cc_compose.cpp:184-213 | Opens the .avb (`CAvatarFileStream`+`LoadAvatar` :188-189), `IndexAvatar()` into the global registry (:192), `session.addUser(id)` + `SetAvatarID` + `av->m_userInfo = pui` wiring invariant (:203-206). Returns the participant id. |
| `cc_strip_set_backdrop` | cc_compose.cpp:230-254 | Points `ccContext().backdropDir`, registers a `BDFileRec` (`SetBackDropAux`), stores `session.backdropID` — every *subsequently created* `CPanel` inherits it (engine/panel.cpp:608). |
| `cc_strip_add_line` | cc_compose.cpp:267-294 | (1) rebuilds the speaker's `m_talkTos` from addressees (:281-285); (2) `ChatPreSendText` text→pose inference (:289); (3) `page->AddLine(...)` (:292). |
| `cc_strip_add_line_cooked` | cc_compose.cpp:310-357 | Same, but when `ann->cooked`: `SetIndices` (ordinary avatar) or `BytesToEmotion`+`SetEmotions` (OTHERMAPPED) *instead of* inference (:330-346) — the ported `SayEntry::Execute` cooked path. |
| `cc_strip_panel_count` / `cc_strip_get_size` | cc_compose.cpp:359-379 | Count of `m_panels`; page bbox in twips via the live `CUnitPanelPage::GetBBox` (engine/panel.cpp:1359-1367). |
| `cc_strip_compose` | cc_compose.cpp:412-440 | The R16 headless `CUnitPanelPage::Draw` replacement: binds a `CDC` adapter to the target `cc_canvas` (:414), then walks every panel: `SetWindowOrg(-loc)`, **damage = the whole panel rect, unconditionally** (:429, design note :396-399 "no scroll viewport to cull against"), `panel->Draw`, row/column advance (:432-437, panel.cpp:1338-1342 arithmetic). |
| `CGCanvas` | ComicChatKit/CGCanvas.swift | RGBA8 `CGBitmapContext`; CTM maps user space to twips y-up, `pixels = twips/20 × scale` (:50-55, :75-83). `drawImage` builds a fresh `CGImage` per call from the engine's RGBA buffer (:308-317), handles the mirrored-dest-rect flip (:361-369). `makeCGImage()` (:490) / `pngData()` via ImageIO (:495-503). |
| Swift wrappers | Strip.swift:173-186, StripScript.swift:199-243 | `Strip.compose(onto:)` bridges any `Canvas` through a `CanvasBox` (Canvas.swift:170-213) held for the call. |

Layout-time text measurement goes through the **globally registered metrics canvas**
(`cc_set_metrics_canvas`, comicchat.h:162-164, defined shim/engine_context.cpp:48); every
current driver installs the deterministic fake-metrics `RecordingCanvas`
(cc-dumpart DemoStrip.swift:34-35, ScriptStrip.swift:15-16, ReplayStrip.swift:272-273).
The app view must install one too (real-CoreText metrics is discovery task 3's call).

### 1.2 What happens on each new line — incremental layout, full redraw

`CUnitPanelPage::AddLine` (engine/panel.cpp:1127-1213) is append-only with a
one-panel-deep mutation window:

- Decide new-vs-grow: a **new** `CUnitPanel` if `m_newPanel` was forced, the last panel
  already has ≥5 elements, fewer than 2 panels exist, or the speaker is already in the
  last panel; otherwise **clone the last panel** and replace it (:1152-1164, :1193-1199
  `RemoveLastPanel` + `AddPanel`).
- Layout runs **only on that panel**: `LayoutAvatars()` (:1179) + `LayoutBalloons()`
  (:1184). If the balloon doesn't fit, the clone is discarded and AddLine recurses onto a
  fresh panel (:1184-1190); leftover text recurses as extra panels (:1202-1209).
- **Invariant for the view: panels other than the last never change** after a line is
  ingested (the only later mutators are the title subsystem, §6a, which rewrites panel 0,
  and a reflow, which rebuilds everything).

Drawing has no such incrementality: `cc_strip_compose` walks all panels with full damage
every call (§1.1). There is no per-panel or clipped compose entry point today.

### 1.3 Geometry: what the API exposes (and what it doesn't)

Exposed: `cc_strip_panel_count` and `cc_strip_get_size` (twips) only.

Not exposed (all engine statics, comicchat.h has no accessor):

| Quantity | Value today | Where |
|---|---|---|
| Panels per row | 2 | `CUnitPanelPage::m_panelsPerRow` engine/panel.cpp:70 (setter panel.h:158, never called by the bridge) |
| Unit panel size | 2300 × 2300 twips (hard-seeded) | cc_compose.cpp:143-144; `MINUNITPANELWIDTH` panel.h:165-166 |
| Interstices | 144 twips each | panel.cpp:75-76 |
| Page bbox arithmetic | `nColumns·W + (nColumns−1)·vInt` wide, `nRows·H + (nRows−1)·hInt` tall | panel.cpp:1359-1367 |

**Panel width is currently fixed, not view-width dependent.** The original derives it
from the client width (§2.3); Plan 4 needs a setter (§6d) to be faithful. Note
`SetUnitPanelWidth` triggers `UpdateTitleFonts()` (panel.h:156) — title fonts scale with
panel width.

At the seeded 2300-twip size a panel is 115 pt (~230 px @2×) — noticeably smaller than
the original's typical runtime panel (its auto-fit targets ≥3000 twips/panel, §2.3), so
view-realistic panels are ~1.7-4× the pixel area measured in §3.

## 2. Question 2 — the original's view model (`CPageView`)

`CChatView` is just the splitter container (chatview.cpp:333-364 builds member list,
body-cam preview, and the comic `CPageView` panes; its `OnSize` only resizes the splitter,
chatview.cpp:297-313). The comic viewport is `CPageView : CScrollView` (pageview.cpp:61).

### 2.1 Scrolling & pagination: one infinite vertical strip, pages only for print

- `OnDraw` (pageview.cpp:189-244) walks the doc's `m_pages`, culls each page against the
  clip box (`bbox_overlap`, :230-233), and calls `page->Draw(..., &rectClip)`. On screen
  there is effectively **one growing `CUnitPanelPage`** in a single logical coordinate
  space — an infinite vertical scroll, `m_panelsPerRow` panels per row.
- Physical "pages" exist only for printing: `OnPrepareDC` maps printed page numbers via
  `GetPhysicalPageCount` (pageview.cpp:478-489); footers etc. (:493-550).
- Per-panel retained bitmap: `CreateRetainedPanel` allocates a single panel-sized
  offscreen DIB (pageview.cpp:159-172), and the original `CUnitPanelPage::Draw` renders
  each damaged panel into it then `BitBlt`s it to the view (panel.cpp:1335 in the lifted
  copy's citation of the original walk; R16 deliberately dropped this machinery,
  cc_compose.cpp:382-399).

### 2.2 Invalidation on a new message: damage exactly one panel + conditional autoscroll

`AddPanel` → `RefreshLastPanel` → `RefreshPanelN(n)` builds a `CDamage` whose rect is the
panel's page-space slot from pure grid arithmetic (engine/panel.cpp:1076-1090, identical
in the original) → `UpdateViewsX` → `CPageView::OnUpdate` (pageview.cpp:330-353):

- unions the damage into `m_bbox`, `InvalidateRect` **only that panel's rect** (:343);
- `UpdateScroll` (:367-403) grows the scroll extent (`SetScrollSizes`) and scrolls the new
  panel into view **only if the user was already at the bottom** — `m_bAtBottom`, computed
  by `AtBottom()` (:355-365, scroll-pos == scroll-limit with a no-scrollbar special case)
  and re-cached on every user scroll (`OnVScroll`, :1363). `OnSize` re-sticks to the
  bottom first thing if `m_bAtBottom` (:1381).

So the original repaints ~one panel per message and never auto-scrolls a user who has
scrolled up. That is the faithful viewport contract.

### 2.3 Resize: deferred auto-fit, square width-derived panels, full transcript reflow

- `OnSize` **posts** `WM_AUTOFITPANELS` instead of reflowing inline (re-entrancy guard,
  pageview.cpp:51-54 + :1379-1391).
- `OnAutoFitPanels` (:1409-1420) → `FitPanelsWide` (:1394-1404): the largest column count
  1..5 whose prospective panel width stays ≥ `COMFORTABLEPANELWIDTH` (3000 twips ≈ 2.1",
  :55). First activation defaults to `SetPanelsWide(DEFAULTPANELPERCOLUMN=3)`
  (:419-420, defines.h:120).
- `GetProspectivePanelWidth` (:1143-1158): panel width = (client width − scrollbar) /
  columns, shrunk so an integral number also fits vertically, clamped ≥ 2300.
  **Panels are square** — `SetPanelsWide` sets height = width (:1114-1115).
- `SetPanelsWide` (:1110-1125) then **destroys every page and replays the entire
  transcript**: `ResetExistingPanels(TRUE)` (:1209-1221; re-adds the title panel via
  `AddTitle`, :1218) + `GetDocument()->ExecuteHistory(HM_RELOAD)` (:1122), then
  `ScrollToBottom()`. There is no in-place panel reflow in the original — reflow ≡
  re-ingest history at the new geometry. Plan 4 can (and should) do exactly the same:
  destroy/recreate the `cc_strip` and replay the kept event transcript.
- Mouse features tied to view geometry: avatar hit-testing by grid arithmetic + body bbox
  (`FindAvatarUnderPoint`, pageview.cpp:663-702), URL-label hit-testing
  (`FindLabelUnderPoint` + `bURLPresent`, :704-745, :778-798), tooltips with the avatar's
  screen name (`OnToolHitTest`, :638-661), member context menu (:750-775).

## 3. Question 3 — cost measurements

Method: a scratchpad SwiftPM harness (release, Apple M2 Ultra) driving the public API —
`Strip` + `RecordingCanvas` metrics (installed via the `cc_set_metrics_canvas` symbol) +
`CGCanvas`, alternating two comicart avatars (`anna.avb`/`armando.avb`) with `field.bgb`,
~40-char lines so every line makes a new panel (line count == panel count in all runs).
Engine logging off (`CC_LOG_LEVEL=0`). Cross-checked against
`swift run -c release cc-dumpart --script` on synthesized JSON conversations and
`--replay` on the committed fixture. Panels are the seeded 2300-twip unit size (§1.3).

### 3.1 Full pipeline at several sizes (CGCanvas scale 2.0 ≈ Retina @1×)

| lines=panels | strip (twips) | bitmap (px) | add_participant ×2 + backdrop | add_line total | add_line avg | **compose avg** | canvas alloc | PNG encode | compose→RecordingCanvas |
|---|---|---|---|---|---|---|---|---|---|
| 2 | 4744×2300 | 474×230 | 0.5 ms | 0.7 ms | 0.36 ms | **7.2 ms** | 0.9 ms | 2.8 ms | 1.2 ms |
| 10 | 4744×12076 | 474×1208 | 0.1 ms | 0.6 ms | 0.06 ms | **27.6 ms** | 0.2 ms | 11.3 ms | 5.3 ms |
| 25 | 4744×31628 | 474×3163 | 0.1 ms | 0.6 ms | 0.02 ms | **68.8 ms** | 0.5 ms | 28.5 ms | 13.4 ms |
| 50 | 4744×60956 | 474×6096 | 0.1 ms | 0.7 ms | 0.01 ms | **132.8 ms** | 1.1 ms | 53.5 ms | 25.7 ms |
| 100 | 4744×122056 | 474×12206 | 0.1 ms | 0.9 ms | 0.01 ms | **264.1 ms** | 1.8 ms | 107.6 ms | 50.5 ms |

Perfectly linear: **≈2.6 ms/panel compose** to a CGCanvas at 2×, ≈0.5 ms/panel of which
is the engine-side walk + DIB decode (the RecordingCanvas column — same decode work, no
CG rasterization). `add_line` layout is noise (fake metrics; real CoreText measurement
will add some, but measureText is itself sub-ms). PNG encode ≈1 ms/panel — only needed
for export, never for the live view.

### 3.2 Per-message full recompose (the naive live-view strategy)

| message # | panels | compose for this message | cumulative |
|---|---|---|---|
| 1 | 1 | 2.9 ms | 2.9 ms |
| 10 | 10 | 26.9 ms | 151 ms |
| 20 | 20 | 53.9 ms | 562 ms |
| 30 | 30 | 79.3 ms | 1.24 s |
| 50 | 50 | 134.0 ms | 3.36 s |

### 3.3 Scale sensitivity (per-blit fixed cost vs per-pixel cost)

Compose avg per panel: **1.95 ms @scale 1.0, 2.64 ms @2.0, 4.40 ms @4.0** (same strip,
4× pixel area per step). A 4× area increase costs only ~1.7×, i.e. compose ≈
**~1.7 ms/panel fixed (per-blit decode + `CGImage` creation) + a per-pixel raster term**.
Realistic view panels (3000-4000 twips, §2.3) at Retina therefore land ≈3-6 ms/panel;
a 50-panel strip ≈ 150-300 ms per full recompose.

### 3.4 CLI cross-check

`cc-dumpart --script` (release binary, whole process incl. art load + PNG + write):
2 panels 0.34 s (first run, cold), 10 panels 0.06 s, 50 panels 0.21 s.
`cc-dumpart --replay` of `Tests/ComicChatKitTests/Fixtures/captures/hand-authored-annotation.jsonl`:
0.65 s — dominated by the replay driver's built-in ≥600 ms event-drain grace sleeps
(ReplayStrip.swift:142-153), *not* compose; don't use `--replay` wall time as a render
benchmark. The 2-panel script output PNG was visually verified (two panels, backdrop,
both avatars, balloons).

### 3.5 Verdict

**Naive full recompose per message is fine for the MVP** — 1998 code on 2026 hardware,
as predicted. At chat cadence (a message every few seconds) 130-300 ms of engine-queue
work per message is acceptable *off the main thread*; the main thread only ever swaps a
cached image. Two caveats that shape §5: (i) never recompose per *frame* (live resize,
scrolling) — only per message/reflow; (ii) beyond a few hundred panels the linear cost
and the backing-bitmap size (23 MB at 100 panels @2×; grows ~4× at realistic panel
sizes) argue for the per-panel tile fast-follow or a panel cap, not for shipping caching
in the MVP.

## 4. Question 4 — the `StretchDIBits` decode-cache debt

### 4.1 Where the re-decode happens

Every image blit decodes the palettized DIB → RGBA freshly, per call:

- `CDC::StretchDIBits` (shim/mfc_compat.h:1079-1096) → `bridge_decode_dib_to_rgba`
  (bridge/bridge_art.cpp:208-213 decl., used by the backdrop draw path).
- `CDC::DrawPoseImage` (shim/mfc_compat.cpp:98-120) → `bridge_decode_dib_pair_to_rgba`
  (image+mask planes; bridge_art.cpp:273).
- `CDC::DrawAuraImage` (shim/mfc_compat.cpp:130-147) → `bridge_decode_aura_to_white_alpha`
  (bridge_art.cpp:347).

And a *second* per-blit layer on the Swift side: `CGCanvas.drawImage` copies the RGBA
bytes into a `Data`, wraps a `CGDataProvider`, and creates a new `CGImage` every call
(CGCanvas.swift:308-317) — necessarily so today, because the engine frees the decoded
buffer right after the call (`cc_image_free`, mfc_compat.cpp:119/146).

### 4.2 Measured share

Blit census (RecordingCanvas compose of a 20-panel strip): **9 image blits/panel**
(180 blits, 370 total ops: 70 text, 40 path). Standalone decode cost via the public art
API (same decode code path): pose ≈0.047 ms, backdrop ≈0.158 ms ⇒ ≈0.55 ms/panel of pure
decode — matching the measured engine-side compose of ≈0.5 ms/panel (§3.1), i.e. decode
is essentially *all* of the engine-side compose cost but only **~20 % of total compose**
at 2×. The larger per-blit fixed share (~1.2 ms/panel more) is the Swift-side
copy + `CGImage` creation + draw setup (§3.3).

### 4.3 Cache sketch (when it's wanted)

Cache at the *view* boundary, not inside the CDC adapter, so one cache kills both layers:
have `CGCanvas` keep an LRU `[ObjectIdentifier-ish key: CGImage]`. The natural key is the
DIB identity the engine hands over — `(bits pointer, width, height)` — which is stable
while a pose/backdrop stays loaded (poses cache their DIBs in `CPose`; backdrop art is
registry-cached, backdrop.cpp). Risk: pointer reuse after a pose unload/avatar close ⇒
stale hit. Two safe variants: (a) plumb a small `generation`/`art id` through
`cc_image` (engine bump on `DestroyAvatars`/`DestroyBackDropArt`); (b) key on a cheap
`cc_crc32` of the *palette+first row* instead of pointers. Either is a contained change
in shim/mfc_compat + CGCanvas. Expected win: compose drops from ~2.6 to roughly ~1 ms/panel
at 2× (removes decode + image-creation fixed cost, leaves rasterization).

**MVP verdict: not needed.** Per-message full recompose is already within budget (§3.5);
the cache only matters if we later draw per-frame (zoom animation, live-resize preview)
or raise panel sizes substantially. Note it as a follow-up tied to option (b) of §5.

## 5. Question 5 — NSView architecture recommendation

**Recommend (a): full recompose into one backing image per new message, cached
`CGImage` drawn by a layer-backed `NSView` in an `NSScrollView`.** Details:

- **View shape.** A flipped (`isFlipped = true`) layer-backed `NSView` as the
  `documentView` of an `NSScrollView`. Its intrinsic/frame size = strip twips / 20 (pt)
  from `cc_strip_get_size`; `draw(_:)` (or directly `layer.contents`) draws the cached
  `CGImage`. No `CATiledLayer`/custom `CALayer` machinery needed at MVP panel counts
  (§3); `layer.contents = cgImage` is the cheapest correct swap and gets Retina scaling
  free via `contentsScale`.
- **Per message** (on the engine queue): `bridge.apply(event)` → `strip.size` →
  `CGCanvas(widthTwips:heightTwips:scale:)` → `strip.compose(onto:)` → `makeCGImage()`
  → hop to main: update view size, set image, autoscroll. 3-6 ms/panel × panel count;
  fine (§3.5). If bursts arrive, coalesce: apply N pending events, compose once.
- **Auto-scroll-to-newest**: replicate `m_bAtBottom` exactly (§2.2) — track "was at
  bottom" from `NSScrollView` bounds-change notifications *before* the content grows;
  after the image swap, scroll to bottom only if it was set. Never yank a user who
  scrolled up (pageview.cpp:355-365, :367-403 is the contract).
- **Retina/backing scale**: `CGCanvas.scale = view.window.backingScaleFactor` (2.0 on
  Retina — exactly the measured configuration). Recompose once on
  `viewDidChangeBackingProperties`.
- **Resize**: mirror the original (§2.3) — do **nothing** per live-resize frame (the
  bitmap just letterboxes/stretches under the scroll view; panel width is
  content-defined, not view-defined, so no per-frame reflow exists even in principle).
  On `viewDidEndLiveResize` (the modern analogue of the posted `WM_AUTOFITPANELS`),
  compute columns/panel width à la `FitPanelsWide`/`GetProspectivePanelWidth`
  (pageview.cpp:1394-1404, :1143-1158), and if changed: destroy the strip, recreate with
  the new geometry (§6d setter), **replay the kept transcript** (the app already owns the
  ordered `[ProtocolEvent]`; the original does literally this via
  `ExecuteHistory(HM_RELOAD)`, pageview.cpp:1122), recompose, scroll to bottom. Replay
  cost ≈ add_line (free) + one compose — well under 200 ms for realistic sessions.
- **Threading (binding)**: every `cc_*` call is process-global single-threaded
  (comicchat.h:172-182; one-strip-at-a-time :214-227). `ProtocolSession` already owns a
  private serial `sessionQueue` on which *all* `cc_session_*` runs and events are emitted
  (ProtocolSession.swift:89-93, :159-199); `ProtocolStripBridge`/`cc-dumpart --replay`
  today sidestep interleaving by strict phase separation (ProtocolStripBridge.swift:9-22,
  ReplayStrip.swift:14-28) — a live app can't phase-separate, so **the strip work must
  run on the same serial queue as the session's engine calls**. Concretely: either
  inject a shared "engine queue" into `ProtocolSession` at init and run
  bridge/strip/compose work on it, or add a `ProtocolSession.performOnSessionQueue {}`
  hook. This is a small `ProtocolSession` API addition the plan must include. The app's
  tests must join the `.serialized` `EngineGlobalStateSelfTests` tree (StripTests.swift:15-24;
  handoff requirement).
- **Live-resize cost**: zero by construction (no reflow until resize ends). The only
  live-resize artifact is stale letterboxing for <1 s, same as the original.
- **Why not (b) per-panel tiles now**: it is the *original's* design (retained panel
  bitmap + one-panel damage, §2.1-2.2) and the right eventual shape — the
  "only-the-last-panel-changes" invariant (§1.2) makes it easy (redraw last panel tile;
  append new tiles) — but it needs a per-panel compose API (§6d), tile lifecycle code,
  and buys nothing user-visible at MVP scale. Do it when panel counts/pixel sizes make
  full recompose or the backing bitmap hurt (≳200-300 panels, or zoom features).
- **Why not (c) CALayer exotica**: the content is a static image between messages; one
  layer-backed view is the simplest thing that is already GPU-composited during scroll.

## 6. Question 6 — API gaps the plan must add

### (a) Title / starring rendering

Original behavior (all currently R11'd out in the lift):

- `CUnitPanelPage::AddTitle` (v2.5 panel.cpp:1279-1297): borderless, backdrop-less panel 0
  with a title `CLabel` (`m_fiTitle`) + a "Starring" `CLabel` (`m_fiShout`, string
  resource `ID_STARRING`) + `AddStars`.
- `AddStars` (panel.cpp:1391-1446): rows of `CBodyUnary` icon (`m_bodyID = av->m_icon`,
  :1437-1438) + `CStarLabel` nickname, centered, `max(ICONSIZE, lineHeight)` row height.
- `AddStarsAux` (panel.cpp:477-514): builds the cast — self first, others ordered by
  `m_nSends` (message count) with departed users last, iterating the doc's nick→pui map
  `g_mapNickToPtr` (:483).
- `CStarLabel::Draw` (v2.5 balloon.cpp:1112-1129): single-line `DrawTextEx` with
  `DT_END_ELLIPSIS`, transparent background.
- `UpdateTitle` (panel.cpp:1300-1314): if no panels, `AddTitle`; else strip panel 0 back
  to the two labels and rebuild the star rows + `RefreshPanelN(0)` — this is **the one
  mutation of a non-last panel** (called on member join, protsupp.cpp:461, and on avatar
  change, histent.cpp:402). Title fonts rescale with panel width
  (`SetUnitPanelWidth` → `UpdateTitleFonts`, engine/panel.h:156, :161).

Lifted state: `AddTitle`/`UpdateTitle`/`AddStars`/`AddStarsAux` are `#ifndef CC_NO_UI`
no-ops (engine/panel.cpp:1370-1422, :517-587) with the R17 reroute to
`ccContext().session.comicsTitle` already written in the wrapped code (:1407;
data at shim/engine_context.h:69).

Plan-4 shape: un-R11 those four + `CStarLabel::Draw` (lift; R18-21 rules apply) with
R17 reroutes for: `g_mapNickToPtr` → the session user table (`ccContext().session`),
`MyAvatarID()/MyAvatar()` → a new "self participant" the bridge records (note
`AddStars` early-returns when `MyAvatarID()==0`, panel.cpp:1394 — starring *requires*
a self notion), `starringStr.LoadString(ID_STARRING)` → an R9 constant, `DrawTextEx` →
an adapter method over the existing `draw_text` (measure + ellipsize; no new canvas op
needed), and `RefreshPanelN` stays headless-no-op. New C API:

```c
int32_t cc_strip_set_self(cc_strip* s, int32_t participant);      /* who "I" am (starring order) */
int32_t cc_strip_set_title(cc_strip* s, const char* title_bytes); /* sets session.comicsTitle +
                                                                     AddTitle/UpdateTitle */
```

The view needs nothing else: the title panel is just panel 0, drawn by the same compose.

### (b) Avatar change for an existing participant

Original flow: "# Appears as name.url" → `ProcessComment` (protsupp.cpp:846-899) →
`ChangeAvatarEntry::Execute` (histent.cpp:368-413): `GetAvatar3(name, pui, randomIfNotFound)`
(avatar.cpp:678-707 — reuse an unused registry entry, else duplicate, else load, else
random + `OTHERMAPPED`), then for others `SetUserAvatarID(pui, avID)`
(userinfo.cpp:38-41: `pui->SetAvatarID(avID); GetAvatar(avID)->m_userInfo = pui;`)
+ `UpdateMemberListIcon` + `UpdateTitle`. Old panels keep the old look (bodies reference
the old avatar id, which stays in the registry); only future panels pick up the new id
via `FetchSpeaker`.

`ProtocolStripBridge` currently records-but-ignores post-create `.appearsAs`
(ProtocolStripBridge.swift:123-129) because `cc_strip` has no such call. New C API
(bridge code, R16-style — mirrors `add_participant` cc_compose.cpp:188-206 for the load
half and `SetUserAvatarID` for the wiring half; no lift needed):

```c
/* Load the .avb at avb_path, register it, and re-point participant's session
 * user at it. Existing panels keep the old avatar (original behavior);
 * subsequent lines render with the new one. Returns 0 on success. */
int32_t cc_strip_set_participant_avatar(cc_strip* s, int32_t participant,
                                        const char* avb_path);
```

Swift side: `Strip.setParticipantAvatar`, and `ProtocolStripBridge.apply(.appearsAs)`
resolves via its `AvatarResolver` and calls it (after Plan 4's `URLSession` download for
remote URLs). Follow the original's "affects future panels only" — no retro-recompose
of old panels (compose will naturally redraw them identically since their bodies still
point at the old registry entry).

### (c) `cc_avatar_icon_image`

The pose API deliberately skips the icon pose (`poseID == m_icon`,
bridge/bridge_art.cpp:404-416, filter :435; `CAvatarX::GetIconPose()` engine/avatar.h:251).
The accessor is ~20 lines of bridge code cloning `cc_avatar_pose_image`
(bridge_art.cpp:470-492) with `pose = av->avatar->GetPoseFromID(av->avatar->m_icon)`;
the icon has no mask plane (bridge_art.h:12-13 note) and `decodeDibToRgba` already
handles a NULL mask ⇒ opaque RGBA out, which matches the original's plain
`CreateDIBitmap`+`SRCCOPY` member-list blit (bridge_art.cpp:408-410 provenance note).

```c
int32_t cc_avatar_icon_image(const cc_avatar* av, cc_image* out); /* 0 = ok */
```

Needed by the character picker and by the title/starring rows' Swift-side sibling uses
(member list icons). No lift.

### (d) Other gaps the view needs

1. **Panel geometry setter/getter (required for faithful resize, §2.3/§5).**
   ```c
   /* Mirrors CPageView::SetPanelsWide: sets unit panel size (square in the
    * original) + panels per row. Call on a FRESH strip before add_line —
    * changing geometry mid-strip is a reflow, i.e. destroy + replay. */
   int32_t cc_strip_set_panel_geometry(cc_strip* s, int32_t unit_w_twips,
                                       int32_t unit_h_twips, int32_t panels_per_row);
   void    cc_strip_get_panel_geometry(const cc_strip* s, int32_t* unit_w,
                                       int32_t* unit_h, int32_t* per_row,
                                       int32_t* h_interstice, int32_t* v_interstice);
   ```
   Thin wrappers over `SetUnitPanelWidth/Height/SetUnitPanelsPerRow` (engine/panel.h:156-158
   — the width setter already cascades `UpdateTitleFonts`) and the statics
   (panel.cpp:70-76). The getter lets Swift compute panel rects for scroll math and
   hit-testing without duplicating constants.
2. **Hit-testing** (context menu / tooltips / avatar click, §2.3). Grid arithmetic can
   live in Swift (pure geometry from the getter above), but body bboxes are engine-side.
   Transliterate the inner loop of `FindAvatarUnderPoint` (pageview.cpp:687-697) as:
   ```c
   /* x/y in page twips (y-up, same space as get_size). Returns participant id
    * (>=1) or 0 for no avatar at that point. */
   int32_t cc_strip_hit_test_avatar(const cc_strip* s, int32_t x, int32_t y);
   ```
   Bridge code walking `m_panels`→`m_bodies` bboxes. MVP-optional (needed for the
   member context menu on the comic; the member *list* covers the same actions).
   URL-label hit-testing (`FindLabelUnderPoint`) can wait for formatted-text support.
3. **Rendering into a caller CGContext / per-panel compose.** Not needed for MVP:
   `cc_strip_compose` already renders into any `cc_canvas`, and `CGCanvas.makeCGImage()`
   (CGCanvas.swift:490) is the view's input — PNG is only the export path. For the
   option-(b) fast-follow add:
   ```c
   int32_t cc_strip_compose_panel(cc_strip* s, int32_t panel_index, cc_canvas* canvas);
   ```
   (same walk as cc_compose.cpp:425-438 with an index filter, panel drawn at local
   origin). Combined with the §1.2 invariant this gives exact per-panel tile caching.
4. **No "changed panel" query is needed**: Swift can infer the redraw set from
   `panelCount` before/after `add_line` — the last panel is always the (only) dirty one
   (§1.2), plus panel 0 after any title/starring update (§6a).
5. **Engine-queue access on `ProtocolSession`** (§5 threading) — a Swift API gap, not a
   C one: inject/expose the serial queue so strip work and session work share one
   executor. Without it the live app has no legal way to run both.

## Top risks / open questions for the planner

1. **Single engine queue is load-bearing and currently has no API.** `ProtocolSession`'s
   `sessionQueue` is private; the bridge's phase-separation contract cannot hold for a
   live app. The plan must add the shared-queue mechanism *first* — every view feature
   sits on top of it. Watch for `cc_session_feed_bytes` re-entrancy (events emitted
   mid-feed) vs. strip calls made from event handlers: handle events by *enqueueing*
   strip work, never calling `cc_strip_*` inside the `on_event` callback stack.
2. **Reflow = destroy + replay interacts with one-strip-at-a-time and determinism.**
   Destroying/recreating the strip resets global registries (comicchat.h:214-227) and
   reseeds `srand(0x5EED)` — replaying the same transcript at the same geometry is
   byte-identical (good), but any state not replayed from the transcript (backdrop from
   a ROOM_PROP, mid-session avatar changes, title) must be re-applied in event order.
   The transcript the app keeps must therefore be the *event* log, not derived strings.
3. **Measured costs used the 2300-twip seeded panel; the real view uses larger panels**
   (§1.3, §3.3): budget ~2-3× the §3.1 compose numbers at realistic sizes before
   declaring headroom. Still within MVP budget, but re-measure once the geometry setter
   exists.
4. **Title/starring is the only piece that mutates panel 0 and needs a "self" concept**
   (`AddStars` bails without `MyAvatarID()`). Decide early whether MVP renders starring
   (needs `cc_strip_set_self` + the R11 un-wrap + `m_nSends` bookkeeping) or ships title
   only.
5. **Real-CoreText layout metrics (discovery task 3) changes panel breaks.** All numbers
   and goldens here used the fake-metrics RecordingCanvas; switching the metrics canvas
   to CGCanvas re-flows balloon fits (different panels for the same transcript) and
   invalidates snapshot fixtures. Whatever task 3 decides, the view code is agnostic —
   it only consumes `get_size`/compose — but the plan should sequence the metrics switch
   before any view-level golden tests.
6. **Memory growth of the single backing image** (~0.9 MB/panel at realistic Retina
   panel sizes) is the first thing that will force option (b). Consider a soft panel cap
   (the original effectively had none; sessions were short) or plan the tile follow-up.
7. **`.appearsAs` avatar download** (Plan-4 `URLSession`) can complete *after* the
   participant has spoken — the change-avatar call intentionally affects only future
   panels (original behavior, §6b). Confirm that's acceptable UX; retroactive restyling
   would require transcript replay (cheap, §3) if wanted.
