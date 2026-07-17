# macOS Port — Roadmap: Plans 2–4 Scoping Notes

> **Not an implementation plan.** This captures cross-plan knowledge gathered while
> writing Plan 1, so each future planning session starts warm. Detailed plans for
> 2–4 are written only after their predecessor lands (their contents depend on
> what execution uncovers). Spec: `2026-07-17-macos-port-design.md` (same date,
> `specs/` folder). Plan 1: `2026-07-17-macos-port-plan-1-engine-foundation.md`.

## Cross-cutting conventions (established in Plan 1)

- **Edit Rules R1–R9** (Plan 1) are the single authoritative policy for modifying
  lifted files. Future plans extend the table (R10+); never fork it.
- **Scaffolding defines** are temporary and each has a designated remover:
  - `CC_NO_RENDER` — removed by **Plan 2** (draw bodies rerouted through `Canvas`).
  - `CC_NO_DIRSCAN` — removed by **Plan 4** (Swift owns directory listing;
    `<io.h>`/`_findfirst` blocks get deleted, not ported).
  - `CC_NO_UI` — shrinks in Plans 2–3, gone by Plan 4.
- The C++ selftest harness (`cc_run_selftests`, `CC_CHECK`) is the pattern for all
  engine-side tests; Swift Testing for everything bridge-side and up.
- Engine module name in Swift is `cchat_engine` (hyphen → underscore).

## Plan 2 — Comic layout engine + Canvas

**Lift list:** `balloon.cpp/.h`, `spline.cpp/.h`, `splinutl.cpp`, `traj.cpp/.h`,
`bodycam.cpp/.h`, `semantic.cpp`, `panel.cpp/.h`, `wmini.cpp` (holds some
`CBody*`/`CPanelElement` virtual definitions — discovered during Plan 1 Task 4),
plus reactivating the `CC_NO_RENDER` bodies stubbed in Plan 1 (`dib.cpp` Draw
overloads, `avatar.cpp` body/pose drawing, `backdrop.cpp`).

**Debt handed over by Plan 1 (delete as the owning files are lifted):**
- `engine/cc_link_stubs.cpp` — trap stubs for virtuals owned by
  `bodycam.cpp`/`panel.cpp`/`wmini.cpp`/`balloon.cpp` (Edit Rule R12b).
- `engine/lifted_singles.cpp` — verbatim single-function lifts the load path
  needed early (R12a); fold back into their owning files when those are lifted.
- `CC_NO_PROTOCOL` define in Package.swift — removed by Plan 3
  (`EmotionToBytes`/`BytesToEmotion` in `avatario.cpp` re-enable then).

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
- `format.cpp`/`protsupp.cpp` untangling remains the flagged unknown (spec §9);
  budget a discovery-only task at the start of Plan 3.
