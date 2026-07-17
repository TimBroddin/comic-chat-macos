# macOS Port — Roadmap: Plans 2–4 Scoping Notes

> **Not an implementation plan.** This captures cross-plan knowledge gathered while
> writing Plan 1, so each future planning session starts warm. Detailed plans for
> 2–4 are written only after their predecessor lands (their contents depend on
> what execution uncovers). Spec: `2026-07-17-macos-port-design.md` (same date,
> `specs/` folder). Plan 1: `2026-07-17-macos-port-plan-1-engine-foundation.md`.

## Cross-cutting conventions (established in Plan 1, extended in Plan 2)

- **Edit Rules R1–R13** (Plan 1) + **R14–R17** (Plan 2) are the single
  authoritative policy for modifying lifted files (full table:
  `2026-07-17-macos-port-plan-1-engine-foundation.md` §Edit Rules for R1–R13,
  `2026-07-17-macos-port-plan-2-layout-canvas.md` §Edit Rules for R14–R17,
  including amendments R14(v) and R15 instance 2). Future plans extend the
  table (R18+); never fork it.
- **Scaffolding defines — current state (as of Plan 2 exit):**
  - `CC_NO_RENDER` — **gone.** Removed in Plan 2 Task 7 (draw bodies rerouted
    through `Canvas`).
  - `CC_NO_UI` — **shrunk, still present.** Plan 2 moved the live camera/
    layout/orchestration/text-pose paths outside it; what remains is UI-chrome
    (CBodyCam widget, dialogs, view pokes) plus title/starring rendering
    (deferred to Plan 4) and the whole of `CUnitPanelPage::Draw` (replaced by
    the R16 headless compositor, not ported). Plan 3 shrinks it further; gone
    by Plan 4.
  - `CC_NO_PROTOCOL` — **still defined, unchanged.** `avatario.cpp`'s
    `EmotionToBytes`/`BytesToEmotion` stay wrapped. **Plan 3 removes it.**
  - `CC_NO_DIRSCAN` — unchanged, still for **Plan 4** (Swift owns directory
    listing; `<io.h>`/`_findfirst` blocks get deleted, not ported).
- The C++ selftest harness (`cc_run_selftests`, `CC_CHECK`) is the pattern for all
  engine-side tests; Swift Testing for everything bridge-side and up. Plan 2
  added the **serialized-suite pattern** for tests that mutate process-global
  engine state (metrics canvas, avatar registry, RNG seed): nest new
  global-state test suites inside the existing `.serialized` ancestor suite,
  never assume `.serialized` alone isolates across separate top-level suites.
- Engine module name in Swift is `cchat_engine` (hyphen → underscore).

## Plan 2 — Comic layout engine + Canvas — **DONE** (2026-07-18)

Exit milestone achieved: a scripted two-avatar, 4-line conversation renders
through the CoreGraphics canvas to a PNG with readable Comic Sans MS text,
correct balloon tails, and camera-driven avatar placement/flipping — visually
verified by the coordinator and by Tim. Recording-canvas snapshot tests green.
Full plan: `2026-07-17-macos-port-plan-2-layout-canvas.md`. Ledger:
`.superpowers/sdd/progress.md` (Plan 2 section, all 12 tasks). Final state:
branch `macos-port` @ `21448f2`, 13/13 `swift test` green.

**Corrections to this roadmap's own Plan 2 section discovered during
execution** (all verified against `chat.mak`, see plan discovery reports
`[CI]`/`[TM]`/`[CM]`):
- `semantic.cpp`, `wmini.cpp`, `script.cpp/.h` are **dead code** — not in
  `chat.mak`, never lifted. The lift list below (written before Plan 2) named
  them; they were correctly dropped at planning time and are not part of the
  macOS port.
- The real text→pose (text→emotion) engine is **`textpose.cpp`**, not
  `wmini.cpp`/`script.cpp` as an uninformed reading of the original tree might
  suggest — lifted in Task 9.
- The body-placement "camera" (avatar ordering/flipping/facing logic:
  `OrderAvatars`/`DoGreedyOrdering`/`EvalPlacement`/`EvalPair`/`AddTalkTos`)
  lives in **`panel.cpp`**, not `bodycam.cpp`. `bodycam.cpp` turned out to be
  the `CBodyCam:CWnd` emotion-widget (UI, excluded via R11) plus the `CBody*`
  draw/bbox methods the camera in panel.cpp calls.

**`format.cpp` finding — de-risks the spec §9 unknown.** The codec (protocol
annotation encode/decode)/UI split inside `format.cpp` that spec §9 flagged as
"least-mapped" turned out clean: Task 5 lifted the formatting/measurement half
(`GetFormattedTextExtent`, `SzControlLess`, formatting-array helpers, etc.)
live and verbatim, while the RichEdit/`CHARFORMAT`-touching functions
(`PRGDWGetFormatting`, `bLOGFONTToCHARFORMAT`, `MatchFont`, …) were cleanly
R11-wrapped under `CC_NO_UI` with no entanglement discovered. **Plan 3
inherits a partially-lifted `format.cpp`**: the formatting half is live and in
use by balloon layout today; the protocol annotation codec in
`format.cpp`/`protsupp.cpp` is the piece Plan 3 still needs to untangle and
lift, and the risk budget for that discovery task can be smaller than spec §9
anticipated.

**Debt handed to Plan 3** (see ledger Task 6/8/9 entries and the plan's R12/R15
table for exact citations):
- `intl.c`'s non-trapping stubs (`GetMime`/`iBytesofChar`/
  `FindSubStringForINTLThatFits`, in `engine/cc_link_stubs.cpp`) are
  byte-identical reimplementations of the CP-1252-only originals — delete them
  when `intl.c` itself lifts, or when the CP-1252-only posture is made a
  permanent design decision (spec §4.5 already defaults to CP-1252, so this
  may just be a documentation change rather than a lift).
- `GetQualifiedName`/`GetMyNickName` R12(b) trap stubs (owned by
  `userinfo.cpp`, not yet lifted) — delete when `userinfo.cpp` lifts.
- **Capitalize R11 no-op is a LATENT REAL-METRICS LAYOUT DIVERGENCE.** Task 6
  R11-wrapped `Capitalize`'s NLS-touching internals as a no-op under
  `CC_NO_UI`; this is invisible today because layout runs against the
  deterministic fake-metrics recording canvas, but it means capitalization
  behavior silently diverges from the original whenever real text metrics are
  in play. Plan 3's NLS/protocol work must restore this verbatim, not leave it
  stubbed.
- **Confirm the wire format never serializes raw `talkTos` DWORDs.** Task 8's
  talkTos pointer-truncation ruling (hybrid: DWORD-space equality compares
  truncate; the one dereference site recovers via a session-table reverse
  lookup, `ccUserFromTalkTo`) depends on `talkTos` values never crossing the
  wire as raw pointers. Plan 3's protocol work must verify this against
  `ircproto.cpp`/`chatprot.h` before touching anything that serializes user
  references.
- The `CharNext` R9 shim member (added Task 5, currently compiled out because
  its only caller sits under `CC_NO_UI`) goes live and needs a real selftest
  once Plan 3 shrinks `CC_NO_UI` far enough to reach it.

**Debt handed to Plan 4:**
- **Title/starring rendering deferral.** `AddTitle`/`UpdateTitle`/`ShowInfo`/
  `AddStars`/`CStarLabel::Draw` are R11-wrapped in Task 8 as a known functional
  deferral — the comic-strip title bar and star-rating UI are not rendered by
  the headless compositor. `session.comicsTitle` already exists in
  `engine_context` (populated, just not drawn) so Plan 4 has the data, only
  the rendering is missing.
- `cc_avatar_icon_image` accessor note (Plan 1 handover, unchanged): the icon
  pose is excluded from the pose API by design; Plan 4 pickers need this
  bridge accessor added. Keep this entry — Plan 2 did not touch it.

**Scaffolding state at Plan 2 exit** (see conventions section above for full
detail): `CC_NO_RENDER` gone; `CC_NO_UI` shrunk but present; `CC_NO_PROTOCOL`
still defined (Plan 3 removes it); `CC_NO_DIRSCAN` untouched (Plan 4).

---

### Plan 2 section as originally written (2026-07-17, pre-execution)

**Lift list:** `balloon.cpp/.h`, `spline.cpp/.h`, `splinutl.cpp`, `traj.cpp/.h`,
`bodycam.cpp/.h`, `semantic.cpp`, `panel.cpp/.h`, `wmini.cpp` (holds some
`CBody*`/`CPanelElement` virtual definitions — discovered during Plan 1 Task 4),
plus reactivating the `CC_NO_RENDER` bodies stubbed in Plan 1 (`dib.cpp` Draw
overloads, `avatar.cpp` body/pose drawing, `backdrop.cpp`).

**Debt handed over by Plan 1 (delete as the owning files are lifted):**
- `engine/cc_link_stubs.cpp` — trap stubs for virtuals owned by
  `bodycam.cpp`/`panel.cpp`/`wmini.cpp`/`balloon.cpp` (Edit Rule R12b).
  (13 stubs, all tier-b. `lifted_singles.cpp`/R12a was never needed — no
  load-path symbol required a verbatim single lift; do not expect that file.)
- `CC_NO_PROTOCOL` define in Package.swift — removed by Plan 3
  (`EmotionToBytes`/`BytesToEmotion` in `avatario.cpp` re-enable then).
- **From the Plan 1 final review (entry work for Plan 2):**
  - `CAvatarStream` (avbfile.h:283) has no virtual destructor; `~CAvatarX`
    deletes through the base → UB + small object leak per close. Fix via a
    documented Edit Rule (add `virtual ~CAvatarStream() {}`), not silently.
  - Add `-Wno-tautological-undefined-compare` to the engine target's
    cxxSettings (34 warnings from the faithful `this != NULL` 1998 idiom are
    drowning real ones).
  - Before Plan 3's transcript tests: give `ccLog` a quiet mode
    (`cc_set_log_level` or env var) — engine TRACE noise already leaks into
    test output ("Deleting avatar: Xeno.") and will get much worse.

**Dependency map (from include analysis, 2026-07-17):**
- `balloon.cpp` includes `panel.h`, `script.h`, `pageview.h`, `format.h`,
  `ircproto.h`, `chatdoc.h`, `binddoc.h`, `ui.h`, `userinfo.h`, `ccommon.h`,
  `<tchar.h>`, `<winnls.h>`. The UI/doc includes (`pageview.h`, `chatdoc.h`,
  `binddoc.h`, `ui.h`) are the untangling work — likely sources of text-metric
  and DC access that become `Canvas` calls.
- `semantic.cpp` includes `chatdoc.h`/`pageview.h`/`panel.h` — expect its *inputs*
  (message text, speaker, addressee) to be extractable as plain parameters.
- `splinutl.cpp`, `vector2d.cpp`, `bbox.h` have **zero** Win32 usage (verified by
  grep) — free wins, `vector2d`/`bbox` already lifted in Plan 1.

**Design commitments (spec §4.3):** abstract C++ `Canvas` — `drawImage`,
`fillPath`/`strokePath`, `drawText`/`measureText` — with text measurement feeding
back into balloon layout. Three implementations: CoreGraphics/CoreText (Swift),
recording canvas (tests), and nothing else. Resolution-independent; Comic Sans MS
as the metric-compatible default font.

**Discovery tasks for the Plan-2 planning session:**
1. Inventory every call inside `#ifndef CC_NO_RENDER` blocks after Plan 1 — this
   *is* the Canvas method list; don't design it before this exists.
2. Find the `GetTextExtent`/text-metric call sites that drive balloon sizing.
3. Map the panel composition entry point: how `pageview`/`panel` drive
   layout-then-draw, and what the minimal headless equivalent is.
4. Determine what `script.h` provides to `balloon.cpp`.

**Exit milestone:** scripted two-avatar conversation rendered to PNG via the CG
canvas; recording-canvas snapshot tests green.

## Plan 3 — Protocol (IRC/MS Chat + annotations)

**Entry debt from Plan 2 (see the Plan 2 — DONE section above for full
detail):** `intl.c` stubs, `GetQualifiedName`/`GetMyNickName` stubs, the
Capitalize R11 real-metrics-layout divergence to restore, confirming
`talkTos` is never serialized raw, and the compiled-out `CharNext` shim
member. Also inherits a **partially-lifted `format.cpp`** (formatting half
live; protocol/codec half still to untangle — see the de-risking note above).

**Lift list:** `ircproto.cpp/.h` (1457 lines, low Win32 density), annotation
encode/decode from `format.cpp` and `protsupp.cpp`, `chatprot.h`.

**Knowledge captured:**
- `chatprot.h`'s `CRoomInfo` is the natural seam: virtuals like
  `bChatSendToChannel(const char* szAnnotations, const char* szMesg, ...)` keep
  **annotations as a separate parameter** from message text — the bridge event
  model can mirror this shape directly.
- CTCP-style low-level quoting exists (`bLowLevelQuoting`, `g_chLLQuoteCTCP`,
  `ircproto.cpp:497`) — annotation bytes are quote-escaped; byte-compare tests
  must account for it.
- `ircsock.cpp` (`CAsyncSocket`) is **replaced wholesale** by Swift
  `NWConnection` + a thin framing layer; only `ircproto`'s parse/build logic is
  lifted (spec §4.4: bytes in, events out, timers scheduled by Swift).
- Avatar announcement/download (`ChatAnnounceNewAvatar`, `webreq.cpp`/wininet)
  → Swift `URLSession`; the URL-handshake protocol logic stays in C++.

**Discovery tasks:** exact parse/side-effect split inside `ircproto.cpp` (it had
17 UI-ish call sites in the density grep); which MS Chat/IRCX commands the 2.5
client actually emits; where nick/channel state lives (`chatsrv.cpp` vs
`ircproto.cpp`).

**Test approach (spec §8.2):** record real sessions with the Windows 2.5 client
under Wine against a local ircd; replay captured bytes; byte-compare re-encoded
annotations. Capture tooling is part of Plan 3, not an afterthought.

## Plan 4 — The Mac app

**Original → Mac mapping (from source inventory):**
- `chatview`/`pageview` (comic view) → custom `NSView` + engine `Canvas`.
- `textview` → `NSTextView` transcript; `saywnd` → compose bar;
  emotion wheel (art in `res/`) → custom control.
- `chicdial` (character picker), `roomlist`, `userlist`, `whisprbx`, `setupdlg`,
  `sounddlg`, `txtfntdg`, `colordlg`, `bothdlg` → Swift sheets/windows/popovers.
- `coolbar`/`tabbar`/`chatbars`/`doskey` → dropped; native toolbar + standard
  text editing instead.
- Sounds: WAV assets via `AVAudioPlayer` (locate originals in `res/` during
  planning). Save/print: JSON transcript + `NSPrintOperation`/PDF (spec §5).
- Settings → `UserDefaults`; encoding toggle (CP-1252 default / UTF-8 opt-in,
  spec §4.5) lives here.

**Tooling note:** XcodeBuildMCP is available in this environment for building,
running, and screenshotting the app during Plan 4 execution.

**Acceptance (spec §8, manual):** live interop on a hobbyist MS Chat server
against the real Windows 2.5 client — poses/avatars/whispers both directions.

## Sequencing risks

- Plan 2 is the highest-uncertainty plan (balloon/pageview untangling). If
  `balloon.cpp`'s coupling to `pageview` proves deeper than an interface seam,
  fall back to lifting a *minimal* `CPanel`-level compositor first and defer
  page pagination.
- Plan 3's transcript capture needs the Windows client running under Wine (or
  CrossOver/VM) — set this up early in Plan 3, it gates the test layer.
- `format.cpp`/`protsupp.cpp` untangling was the flagged spec §9 unknown;
  **Plan 2 partially de-risked it** — the formatting/measurement half of
  `format.cpp` lifted clean with no entanglement, so Plan 3's discovery task
  is scoped down to the annotation codec in `format.cpp`'s remaining
  (RichEdit/`CHARFORMAT`) half plus `protsupp.cpp` — still budget a
  discovery-only task at the start of Plan 3, just a narrower one.

**Task 6 handover note (Plan 1; tracked in the "Debt handed to Plan 4" list
under Plan 2 — DONE, above):** the icon pose is excluded from the pose API by
design; Plan 4 pickers need a `cc_avatar_icon_image` accessor added to the
bridge.
