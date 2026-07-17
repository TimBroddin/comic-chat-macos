# Plan 2 Discovery — Comic Composition Architecture Map

Read-only discovery for the LAYOUT-engine lift. All paths under
`/Users/timbroddin/Projects/comic-chat/v2.5-beta-1-modern/` (read-only reference).
Build set verified against `chat.mak`.

---

## 0. HEADLINE CORRECTIONS TO THE ROADMAP (read this first)

The roadmap's file list is partly wrong about *what lives where*. Verified facts:

1. **`semantic.cpp` is DEAD CODE.** Not in `chat.mak`. Its only functions
   (`AddSemantics`, `HackLeft`, `PostSemantics`) are `#if 0`'d or their callers are
   commented out (`panel.cpp:1092`, `1124`). **Do not lift it.**
2. **`wmini.cpp` is DEAD CODE.** Not in `chat.mak`. It is an *older, pre-formatting-era
   ancestor* of `balloon.cpp` — it redefines `CBalloon`, `CLabel`, `CBWoodringNormal`,
   `CArrow`, spline creation, etc. with **different (simpler) constructor signatures**
   (e.g. `CBalloon::CBalloon(const char*, CFontInfo*)` — no formatting/URL args). The
   *live* versions of all those classes are in `balloon.cpp`. **Do not lift `wmini.cpp`.**
   The roadmap's claim that "CBody*/CPanelElement virtual definitions live in wmini" is
   false — those live in `bodycam.cpp` (bodies) and `avatar.h` (declarations).
3. **`bodycam.cpp` is NOT the body-placement/camera engine.** It is TWO things:
   (a) `CBodyCam : public CWnd` — the interactive emotion-selector *widget* (mouse/paint
   handlers, tooltips, context menus) — pure MFC UI, NOT needed for headless; and
   (b) the `CBody` / `CBodyDouble` / `CBodySingle` **draw + bbox** implementations
   (`DrawBody`, `GetBodyBox`, `FlipBodyBox`, `Draw`, `IsSame`) — these ARE needed and are
   arguably Plan-1 art-pipeline territory (`CBody` is declared in `avatar.h`).
4. **The actual "camera"/body-placement/left-right-ordering/flip logic lives in
   `panel.cpp`** (`OrderAvatars`, `EvalPair`, `EvalPlacement`, `DoGreedyOrdering`,
   `AddTalkTos`, `ComputeDisplacementPenalty`, `CUnitPanel::LayoutAvatars`). This is the
   real body-camera engine and it is the coupling hot-spot (reads the `CUserInfo` talk-to
   graph).
5. **Text→emotion/pose inference lives in `textpose.cpp` (BUILT), not `semantic.cpp`.**
   Entry point `ChatPreSendText` (textpose.cpp:119) + `GetEmotionsFromString`
   (textpose.cpp:271) + the rule tables. It resolves the emotion and calls
   `CAvatarX::GetBodyFromEmotion` / `UpdateBody` (avatar.cpp — Plan 1). **Add `textpose.cpp`
   to the Plan 2 lift list; drop `semantic.cpp` and `wmini.cpp`.**

Build set actually compiled (from `chat.mak`): `balloon.obj panel.obj spline.obj
splinutl.obj traj.obj bodycam.obj textpose.obj actions.obj rules.obj avatar.obj …`.
NOT compiled: `wmini.obj`, `semantic.obj`.

---

## 1. ENTRY-POINT CHAIN (message in → pixels out)

Message ingestion + pose inference + layout:

| # | Function | Location | Role |
|---|----------|----------|------|
| 1 | `CChatDoc::ProcessLine(uID, szLine, uModes, bbCooked, fmt)` | chatdoc.cpp:447 | Cook text; call pose inference; forward to AddLine |
| 2 | `ChatPreSendText(strMesg, uID)` | textpose.cpp:119 (called chatdoc.cpp:452) | text→emotion; `GetEmotionsFromString`→`av->GetBodyFromEmotion`→`av->UpdateBody` |
| 2a| `GetEmotionsFromString(str, emOpts)` | textpose.cpp:271 | Runs rule tables (caps, words, sentence-starts) → `CEmotionOpts` |
| 3 | `CChatDoc::AddLine(uID, szText, uModes, fmt)` | chatdoc.cpp:328 | Get last `CPage`; call `page->AddLine`; on fail `AddNewPage()` then retry |
| 4 | `CUnitPanelPage::AddLine(uID, szWords, uModes, fmt, url)` | panel.cpp:1058 | **THE ORCHESTRATOR** — panel-break decision, balloon+body build, layout, add/replace panel |
| 5 | `CUnitPanelPage::MakeBalloon(...)` | panel.cpp:1036 | Factory: `CBWoodringNormal/Whisper/Think/Box` from `uModes` |
| 6 | `CPanel::FetchSpeaker(uID)` / `ReplaceBody(uID)` | panel.cpp:606 / 626 | Clone `CAvatarX::m_body` into panel's `m_bodies` |
| 7 | `CUnitPanel::LayoutAvatars()` | panel.cpp:726 | Body ordering/flip/scale/zoom; calls `OrderAvatars` |
| 7a| `OrderAvatars → DoGreedyOrdering → EvalPlacement → EvalPair` | panel.cpp:403/358/272/ (OrderAvatars ~445) | **Camera**: pick left/right order + facing from talk-to graph |
| 8 | `CUnitPanel::LayoutBalloons(...)` | panel.cpp:855 | Loop balloons → `LayoutBalloon`; on overflow → `ForceFitBalloon`/return leftover |
| 8a| `CUnitPanel::LayoutBalloon(...)` → `GetCloudEstimate` | panel.cpp:925 / 885 | Size + place one balloon; `SetBBox`→`ComputeInternals`; route-region checks |
| 9 | `CBalloon::SetBBox` → `ComputeInternals` → `BreakIntoLines`+`CreateBalloonSpline`+`ComputeCloudBBox` | balloon.cpp:1396/1764/668, spline via wmini's live twin in balloon.cpp:1700 | **Pure layout** (text metrics + spline geometry, no GDI) |
| 10| `CUnitPanelPage::AddPanel` → `RefreshLastPanel` → `RefreshPanelN` → `UpdateViewsX` | panel.cpp:1029/1004/1011 | Compute damage rect, poke the MFC view to repaint |

Draw path (separate, triggered by MFC paint, NOT by AddLine):

| # | Function | Location | Role |
|---|----------|----------|------|
| D1| `CPageView::OnDraw(pDC)` | pageview.cpp:189 | Iterate `pDoc->m_pages`; bbox-cull; `page->Draw(this,pDC,…)` |
| D2| `CUnitPanelPage::Draw(pView, dc, …, damage)` | panel.cpp:1193 | Alloc mem-DC, per-panel `panel->Draw`, `BitBlt` to screen via `pView->GetRetSec`/`AccountForScroll` |
| D3| `CUnitPanel::Draw(dc, ul, dmgRect)` | panel.cpp:664 | Backdrop→bodies→elements(balloons); clip to panel |
| D4| `CBody::Draw` (CBodyDouble/Single) | bodycam.cpp:578/614 | Draw avatar body poses |
| D5| `CBWoodringNormal::Draw` / `CLabel::Draw` | balloon.cpp:1779 / 876 | Traj/spline stroke+fill, then `DrawText`/`TextOut` |

---

## 2. OWNERSHIP MODEL

```
CChatDoc            (chatdoc.h:13)     owns  m_pages      : CPtrList of CPage*
  └─ CUnitPanelPage (panel.h:96)       owns  m_panels     : CPtrList of CPanel*
       └─ CUnitPanel (panel.h:45)      owns  m_elements   : CPtrList of CPanelElement* (balloons/labels)
                                       owns  m_bodies     : CPtrList of CBody*
                                       owns  m_backDrop   : CBackDrop (value member)
```

- **Panel list owner:** `CPage` (concrete `CUnitPanelPage`) owns `m_panels`. `CChatDoc`
  owns the *pages*. Deletion cascades: `CPage::~CPage` (panel.cpp:984) deletes panels;
  panel dtor deletes elements+bodies.
- **Panel-element owner:** `CPanel` owns both `m_elements` (balloons — the `CPanelElement`
  subclasses `CLabel`/`CBalloon`/`CBWoodring*`) and `m_bodies` (`CBody` subclasses,
  separate list). Note: bodies live in a *separate* list from elements, not as
  `CPanelElement`s in `m_elements`.
- **Panel-break / new-panel decision:** `CUnitPanelPage::AddLine` (panel.cpp:1079). New
  panel when: `m_newPanel` flag set, OR old panel already has ≥5 elements, OR fewer than 2
  panels exist, OR the speaker's avatar is already in the old panel
  (`pOldP->AvatarInPanel(uID)`). Otherwise it *clones* the last panel and appends (so each
  panel is cumulative until a break). `BM_ACTION` (boxes) always forces a new panel
  (line 1064). Overflow (balloon won't fit) → delete trial panel, `StartNewPanel()`,
  recurse (line 1113); leftover text → recurse with remainder (line 1130).
- **Pagination / page reflow:** lives in **`CChatDoc::AddLine`** (chatdoc.cpp:333): if
  `page->AddLine` returns FALSE, `AddNewPage()` and retry. `CUnitPanelPage::AddLine`
  *itself never returns FALSE in the current code* (it recurses on overflow), so
  multi-*page* breaking is effectively latent/unused in the interactive path; panels grow
  in an unbounded single page (`m_panelsPerColumn = -1` → "page never ends", panel.cpp:58).
  Row wrapping is purely a *draw-time* concern in `CUnitPanelPage::Draw` (panel.cpp:1250,
  `panelCount % truePanelsPerRow`) and `GetBBox` (panel.cpp:1268). **Pagination is thin and
  can be deferred (see §7 fallback).**

---

## 3. LAYOUT-vs-DRAW SEPARABILITY — VERDICT: CLEANLY SEPARABLE

Layout and draw are in **distinct method families**, not interleaved.

**Layout functions (NO GDI drawing; use the client DC only for text metrics):**
- `CUnitPanel::LayoutAvatars` (panel.cpp:726), `LayoutBalloons` (855), `LayoutBalloon`
  (925), `GetCloudEstimate` (885), `RearrangeBalloons`, `AdjustArtToCoord` (948),
  `GetBalloonRect` (839), `IsSpeaker` (822), plus the ordering helpers
  `OrderAvatars/DoGreedyOrdering/EvalPlacement/EvalPair/AddTalkTos` (panel.cpp:272–460).
- `CBalloon::SetBBox` (balloon.cpp:1396) → `ComputeInternals` (1764) → `BreakIntoLines`
  (668) + `CreateBalloonSpline` (1700) + `ComputeCloudBBox` (1432); also `DockAtTop`,
  `AreaEstimate`, `WidestWord`, `SplitHeight`, `GetCloudBBox`, `QueryRouteRgn`.
- Geometry libs: `spline.cpp`, `splinutl.cpp`, `traj.cpp` — **zero** view/doc coupling,
  pure math (the only GDI is inside their own `Draw`/`Dash`, which are draw-time).

**Draw functions (all GDI, no layout):**
- `CUnitPanelPage::Draw` (panel.cpp:1193), `CUnitPanel::Draw` (664), `DrawBorder` (711),
  `CBody::Draw`/`DrawBody` (bodycam.cpp), `CBalloon`/`CLabel::Draw` (balloon.cpp:1779/876),
  `DrawFormattedText`/`DrawText`/`iDrawFormattedTextLine`.

**Coupling caveats (small, resolvable):**
- Layout measures text through the global `GetClientDC()` (a `CClientDC`) — see §4. This is
  the ONE cross-cut that layout needs from the environment: **text metrics**
  (`GetTextMetrics`, `GetTextExtent`). Everything else in layout is arithmetic on `SRECT`.
- `m_traj` (the drawable spline→segment list) is built lazily inside `Draw`
  (`SetBalloonTraj`, balloon.cpp:1788) rather than in layout, but `m_spline` (the geometry)
  IS produced by layout; trivial to hoist `SetBalloonTraj` into layout if desired.

Net: a Canvas abstraction that provides (a) text metrics for layout and (b) primitive
draw ops (stroke path, fill, text-out, blit body bitmaps) cleanly bisects the code.

---

## 4. PAGEVIEW / CHATDOC / BINDDOC / UI / USERINFO COUPLING (by file)

Legend: (a) trivially parameterizable · (b) needs small interface/context struct · (c) deep.

### balloon.cpp — VERY LOW COUPLING
- `theApp.m_charSet` (balloon.cpp:111) — font/charset selection. **(a)**
- `GetClientDC()` ×8 (587,670,707,721,783,1165,1270,1539) — **text metrics only**
  (`GetTextMetrics`/`GetTextExtent`/`BreakIntoLines`). **(b)** — must become
  `Canvas::measureText`.
- `OnLButtonDown`/`bURLHit`/hot-link handling (balloon.cpp URL paths) — interactive only;
  stub for headless. **(a)**
- No CPageView / CChatDoc / CUserInfo member access. Counts → **a:2, b:1, c:0.**

### panel.cpp — MODERATE COUPLING (the real work is here)
- `GetAvatar(id)` / `MyAvatar()` / `MyAvatarID()` — art pipeline (Plan 1). Many call sites
  (264,289,300,319,327,335,397,439,487,503,509,520,617,632,973,1371,1395). **(a)** —
  already lifted; just link.
- `CUserInfo* … ->m_userInfo` and **`pui->m_udi.m_talkTos`** (289–335) — the addressee/
  talk-to graph that drives body ordering + facing. This is the single most important
  *data* dependency for the camera. **(b)** — the scripted-conversation input must carry a
  per-message addressee list that populates `m_udi.m_talkTos`; can be a slim struct, does
  NOT need full `CUserInfo`.
- `CAvatarX::m_lastDir` (397) + `UpdateHistoresis` (818) — per-avatar facing hysteresis
  state. **(b)** — small mutable avatar-state, part of the injected avatar model.
- `theApp.m_comicsColor` (1218), `theApp.StartDownloadingAvatar` (977, in dead hot-link
  path) — color is settings **(a)**; download is interactive, stub **(a)**.
- `GetChatDoc()->GetBackDropID()` (558), `GetChatDoc()->GetComicsTitle()` (1302) — read
  document settings (backdrop id, title). **(a/b)** — pass in a settings/context struct.
- `m_doc->m_view` + `UpdateViewsX(...)` + `RefreshPanelN`/`RefreshLastPanel`
  (1004–1026) — the view-invalidation path. **(a)** for headless: make these no-ops (there
  is no live view to damage; you draw the whole thing once).
- `CUnitPanelPage::Draw` uses `pView->GetRetSec(dc)`, `pView->AccountForScroll(...)`,
  `theApp.m_comicsColor`, and the whole retained-DIB/BitBlt/scroll machinery
  (1193–1264). **(c) for THIS method only** — it is welded to `CPageView`'s retained-bitmap
  + scroll model. **This is the pageview-entanglement risk.** Mitigation: replace
  `CUnitPanelPage::Draw` with a headless compositor that walks `m_panels`, computes each
  panel's origin from row/col + intersticies (logic already present at 1233–1255), and
  calls `panel->Draw(canvas, &loc, fullDamage)` directly onto one big canvas — dropping
  `GetRetSec`/`AccountForScroll`/BitBlt entirely.
- `GetBodyCamBody()` extern (26) — only referenced in dead paths; ignore. **(a)**
- Counts → **a: ~6 groups, b:3, c:1** (the `Draw` blit/scroll method).

### bodycam.cpp — SPLIT PERSONALITY
- `CBodyCam : CWnd` widget half: `OnPaint/OnLButtonDown/OnMouseMove/OnContextMenu/OnSize/
  OnSetFocus/…`, `theApp.DoOptionsDialog` (351), `GetChatDoc()->GetConnectionStatus/
  ResetStatus` (368–371), `GetBodyCam()` singleton (444,861,869) — **all UI, NONE needed
  headless. Exclude the widget entirely. (n/a)**
- `CBody*::DrawBody`/`GetBodyBox`/`FlipBodyBox`/`Draw`/`IsSame` half: only `GetAvatar(id)`
  (519,592) — art pipeline. **(a).** These body-draw methods ARE needed; extract just them
  (they belong with `CBody` in the avatar/art layer).
- Counts (needed half only) → **a:1, b:0, c:0.**

### wmini.cpp — DEAD, EXCLUDE. (no coupling; no build) — **n/a.**

### semantic.cpp — DEAD, EXCLUDE. (includes pageview.h/chatdoc.h but body is `#if 0`) — **n/a.**

### textpose.cpp (the real "semantic") — LOW COUPLING
- `GetChatDoc()->m_bComicView` guard (122), `GetAvatar/MyAvatar` (124),
  `av->m_freeze` (125), `av->GetBodyFromEmotion`/`UpdateBody` (127–128) — all art-pipeline +
  a comic-view flag. **(a/b).** Inputs (message text, avatarID) are plain params (see §5).
- Counts → **a:2, b:1, c:0.**

### Aggregate coupling by class (needed files only: balloon, panel, textpose, body-draw):
- (a) trivially parameterizable: ~13 sites/groups (charset, colors, view-refresh no-ops,
  stub interactive/download, GetAvatar links).
- (b) small interface/context struct: ~8 (text-metrics Canvas; talk-to/addressee input;
  avatar facing-state; doc settings backdrop/title/comic-flag).
- (c) deep: **1** — `CUnitPanelPage::Draw`'s retained-DIB + scroll + BitBlt welding to
  `CPageView`. Replace, don't port.

---

## 5. semantic / textpose INPUTS — EXTRACTABLE AS PLAIN PARAMETERS: YES

`ChatPreSendText(CString &str, int avID)` (textpose.cpp:119) takes **message text + speaker
avatarID** as plain parameters. `GetEmotionsFromString(str, emOpts)` (271) takes text +
output emotion struct — no view/doc/user coupling in the analysis itself; the rule tables
are static (`InitializeEmotionRules`, textpose.cpp:131). The only environmental reads are
the comic-view flag and `GetAvatar`, both already parameterizable.

**Addressee / gesture history:** *addressee* is NOT consumed by textpose — it enters
separately. The addressee graph is `CUserInfo::m_udi.m_talkTos` (a `CDWordArray` of user
pointers), populated upstream from the protocol and read by the body-camera in
`panel.cpp` (§4). "Gesture history" ≈ `CAvatarX::m_lastDir` + `UpdateHistoresis` (facing
memory). Both are plain per-avatar state, injectable. There is also an explicit inline
addressee syntax `"a b|message"` parsed by `FindPose`/`FindAttribution`
(chatdoc.cpp:375,405) which sets face/torso targets directly — useful for a scripted
driver (you can encode addressees in the script line).

**Conclusion:** the scripted two-avatar input is fully expressible as a list of
`{speakerID, text, uModes(BM_SAY/WHISPER/THINK/ACTION), addressees[], formatting?}` — no
live objects required beyond the injected avatar art model.

---

## 6. wmini.cpp — WHAT IT ACTUALLY IS

`wmini.cpp` (not built) is a **superseded, standalone earlier implementation** of the
balloon/label/arrow/spline subsystem. It contains full definitions of `CBalloon`,
`CLabel`, `CBWoodringNormal/Whisper/Think`, `CArrow`, `CBWoodringNormal::CreateBalloonSpline
/AddArrow/Draw`, `BreakIntoLines`, `DashPath`, `DrawWhisperNimbus`, `CFontInfo`, etc. —
but with the **pre-formatting API** (constructors lack `CDWordArray* prgdwFormatting` and
`szURLStart`; e.g. wmini.cpp:147/163). The header `balloon.h` matches the **balloon.cpp**
signatures, not wmini's — so wmini would not even compile against the current headers.

Its role vs `bodycam.cpp`: **none overlapping.** `wmini.cpp` = old balloons;
`bodycam.cpp` = emotion-picker widget + `CBody` draw. The roadmap conflated wmini with the
body/camera code. **Action: exclude `wmini.cpp` from Plan 2 entirely; the live balloon code
is `balloon.cpp` (+`spline/splinutl/traj`). If wmini has any wanted variant behavior,
diff it against balloon.cpp — but assume balloon.cpp is authoritative.**

---

## 7. HEADLESS COMPOSITION DRIVER — RECOMMENDATION

### Entry object
`CUnitPanelPage` (panel.h:96) is the composition entry object. The driver should:
1. Instantiate one `CUnitPanelPage` (pass a lightweight doc/context, not a real
   `CChatDoc` — it only needs backdrop-id, title, comic-flag, and a settable `m_view`
   that the refresh calls can ignore).
2. Seed one initial panel (`AddNewPage` semantics create the page; first `AddLine` starts
   panel 1).
3. For each scripted line call the equivalent of `CChatDoc::ProcessLine` →
   `ChatPreSendText` (pose) → `CUnitPanelPage::AddLine` (layout).
4. After all lines, call a **new headless `Compose(canvas)`** that replaces
   `CUnitPanelPage::Draw` (see below) to render every panel onto one canvas → PNG.

### Context to inject (the "b" items)
- **Canvas / text-metrics interface** replacing `GetClientDC()` and the GDI `CDC` used in
  `Draw`: needs `measureText(font,str)→{width, tm}` for layout, and draw primitives
  (move/line, polybezier/stroke-path, fill, text-out, blit-DIB) for render. This is the
  keystone of the whole lift.
- **Fonts/settings context**: the `CUnitPanelPage` static `CFontInfo*` set
  (`m_fiWNormal/m_fiWWhisper/m_fiTitle/m_fiShout`, panel.h:110-113) built by
  `SetFonts(LOGFONT, COLORREF)` (pageview.cpp:OnCreate calls it). Provide a headless
  `SetFonts` that builds `CFontInfo` via the Canvas's text metrics. Plus panel geometry
  statics (`m_unitWidth/Height/panelsPerRow/intersticies`) — plain ints, set once.
- **Avatar art model** (Plan 1): `GetAvatar`, `CBody::Clone/Draw/GetBodyBox`,
  `GetBodyFromEmotion` — link the Plan-1 target.
- **Addressee / facing state**: per-line addressee list feeding `m_udi.m_talkTos`, and
  per-avatar `m_lastDir`/hysteresis. A slim `SpeakerState` struct suffices.

### Pagination: DEFER (take the fallback)
Full multi-*page* pagination is latent/unused in the live interactive path
(`m_panelsPerColumn = -1`, and `CUnitPanelPage::AddLine` never returns FALSE). Panels grow
on one logical page; rows wrap purely at draw time. **Recommendation: implement the
CPanel-level compositor now (fallback plan), defer real pagination.** Concretely: do NOT
port `CUnitPanelPage::Draw`; write `Compose(canvas)` that reuses the *origin arithmetic*
already at panel.cpp:1233-1255 (`m_unitWidth/Height`, intersticies, `panelCount %
panelsPerRow`) to place each panel, then calls `panel->Draw(canvas, &loc, fullBox)`
directly. This severs the one deep (c) coupling.

### The 2–4 riskiest entanglements the plan MUST resolve
1. **`GetClientDC()` text-metrics dependency (balloon.cpp ×8 + `CFontInfo` ctor +
   `BreakIntoLines`).** Layout is impossible without text measurement. Must land a Canvas
   `measureText` first; every layout function transitively depends on it.
2. **`CUnitPanelPage::Draw`'s retained-DIB + `CPageView` scroll/BitBlt weld
   (panel.cpp:1193-1264, uses `pView->GetRetSec`/`AccountForScroll`).** The only true
   deep-(c) coupling. Resolve by *replacing* the method with a headless compositor, not
   porting it.
3. **The `CUserInfo::m_udi.m_talkTos` addressee graph driving body ordering + facing
   (panel.cpp:289-355, `EvalPair`/`AddTalkTos`).** Without a faithful addressee input the
   two-avatar staging (who's on the left, who faces whom) will be wrong. Must design the
   scripted-line schema to populate this and the `m_lastDir` hysteresis.
4. **`CBody` split across `bodycam.cpp` (draw) and `avatar.h` (decl), tangled with the
   `CBodyCam` MFC widget in the same .cpp.** Must cleanly extract the `CBody*::DrawBody/
   GetBodyBox/FlipBodyBox/Draw` methods (art-layer) while leaving `CBodyCam:CWnd` behind —
   decide whether these belong in the Plan-1 art target or Plan-2 layout target (they
   straddle the line; recommend art target since `CBody` is declared in avatar.h).

### Recommended Plan-2 lift list (corrected)
`panel.cpp/.h` · `balloon.cpp/.h` · `spline.cpp/.h` · `splinutl.cpp` · `traj.cpp/.h` ·
`textpose.cpp` (the real semantic) · the `CBody` draw methods out of `bodycam.cpp` ·
`format.cpp` (formatting arrays used by `CLabel`/`CBalloon`) · `bbox.cpp/.h`,
`vector2d.cpp/.h` (already math).  **Drop: `semantic.cpp`, `wmini.cpp`, the
`CBodyCam:CWnd` widget, `pageview.cpp/.h` (replace `Draw` with headless compositor).**
