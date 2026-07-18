# macOS Port — Plan 4a: The Mac App (engine surface + MVP chat loop) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Use the **`p4-` prefix** for all SDD briefs/reports (ledger-recorded lesson from Plans 1↔2).

**Goal:** A running macOS app: connect → login → join a room → the comic strip renders live as annotated messages arrive → send a say — on top of the finished Plans 1–3 engine, with the engine-surface additions (login sequencing, real metrics, geometry, avatar + title APIs, outbound announce) the app and the later §8 acceptance need.

**Architecture:** Plan 4 splits in two. **4a (this plan)** = every engine/bridge/Kit addition discovery proved necessary, plus the MVP app slice (one window + connect sheet). **4b (backlog section at the end, planned after 4a lands)** = emotion wheel, whisper UI, pickers/Settings, avatar download, room ops, sounds, save/print, text view, and the full spec-§8 live acceptance. Every task in 4a produces working, `swift test`-verified software. Discovery ground truth: the four reports in `docs/superpowers/plans/2026-07-18-plan4-discovery/` (`app-skeleton-mvp.md` [D1], `comic-view-rendering.md` [D2], `layout-metrics.md` [D3], `live-interop.md` [D4]). Where this plan and a report disagree, **stop and re-read the report** — the reports carry file:line evidence.

**Tech Stack:** Existing SwiftPM package `macos/ComicChatKit` (C++ target `cchat-engine`, Swift target `ComicChatKit`, Swift Testing); new Xcode app project `macos/ComicChat` (SwiftUI lifecycle, one `NSViewRepresentable` comic view); CoreText/CoreGraphics; Network.framework. No new third-party dependencies.

## Global Constraints

- **Edit Rules R1–R21 are carried forward verbatim and binding** (Plan 1 §Edit Rules R1–R13, Plan 2 §Edit Rules R14–R17 incl. amendments, Plan 3 §Edit Rules R18–R21 incl. the R13 clarification and the `ChangeKeyString` preserved-forever quirk). Any *new* pattern in lifted code → escalate (BLOCKED/NEEDS_CONTEXT); rulings become amendments **R22+**. Never improvise. Most 4a tasks are NEW Swift/bridge code where the rules don't bite; they bite in Task 7 (un-R11 lift) and anywhere original files are touched.
- `v2.5-beta-1-modern/` and `artifacts*/` are **read-only reference — never edit**. Copy (build-phase copy for resources), never reference at runtime.
- **Engine is single-threaded, process-global, one strip at a time** (`comicchat.h` threading contract). ALL `cc_*` calls — session AND strip — must run on ONE serial queue (Task 1 makes this an API). **Never call `cc_strip_*` from inside the `on_event` callback stack** — handle events by enqueueing strip work (D2 top-risk 1).
- New engine-global-state test suites nest inside the `.serialized` ancestor `EngineGlobalStateSelfTests` (Tests/ComicChatKitTests — see `StripTests.swift:15-24` for the pattern). App-side logic lives in ComicChatKit so `swift test` from `macos/ComicChatKit/` stays the headless truth. SourceKit "No such module: cchat_engine / Testing" diagnostics are FALSE; the command line is the truth.
- **Real-metrics rule (D3):** never freeze byte-exact goldens under real CoreText metrics — real-metrics tests use tolerances/invariants only. Byte-exact goldens stay on the fake-metrics `RecordingCanvas` forever (two-mode design). The metrics canvas must be installed **before** `cc_strip_create` (fonts measured once per strip).
- Wire text is bytes end-to-end (CP-1252 default, UTF-8 per-connection via the existing `WireEncoding`); keep all new test vectors ASCII except where Task 3 deliberately exercises the accented fold.
- Commits are SSH-signed via 1Password; if `ssh-add -l` shows no identities, **ask Tim to unlock — never bypass signing without explicit authorization**; when authorized, `git commit --no-gpg-sign` and say so in the record. (Standing: `d1da855..47f65c4`+ is unsigned and must be re-signed before any merge to `main`.)
- Public chat servers (`crypthome.com`, `comic.dedoky.com`, `koach.com` — D4 §1.1) are small community boxes: **no scripted/automated traffic**, human-driven sessions only, scratch rooms, introduce the project. Nothing in 4a's automated tests touches them.
- `swift build`/`swift test` run from `macos/ComicChatKit/`; the app builds with `xcodebuild -project macos/ComicChat/ComicChat.xcodeproj -scheme ComicChat build` (XcodeBuildMCP's macOS workflow tools are NOT currently enabled in this environment — D1 risk R4 — use Bash `xcodebuild`/`open`/`screencapture`, or ask Tim to enable the `macos`+scaffolding workflows).

---

## File structure

```
macos/ComicChatKit/
  Sources/cchat-engine/
    include/comicchat.h          MODIFY  + cc_session_login, own_user/own_realname config,
                                          cc_session_announce_avatar, cc_strip_set_panel_geometry/
                                          get_panel_geometry, cc_strip_set_participant_avatar,
                                          cc_strip_set_title/set_self, cc_avatar_icon_image
    bridge/cc_session.cpp        MODIFY  login entry, announce builder/routing (Tasks 2, 8)
    bridge/cc_compose.cpp        MODIFY  geometry, participant-avatar, title/self (Tasks 5, 6, 7)
    bridge/bridge_art.cpp/.h     MODIFY  cc_avatar_icon_image (Task 6)
    bridge/cc_selftest.cpp       MODIFY  selftests for every C addition
    engine/panel.cpp             MODIFY  un-R11 AddTitle/UpdateTitle/AddStars/AddStarsAux (Task 7)
    engine/balloon.cpp           MODIFY  un-R11 CStarLabel::Draw (Task 7)
    shim/mfc_compat.h/.cpp       MODIFY  CP-1252 CharUpperBuff fold table (Task 3);
                                          CDC::DrawTextEllipsis adapter (Task 7)
  Sources/ComicChatKit/
    ProtocolSession.swift        MODIFY  injectable engine queue + performOnEngineQueue (Task 1);
                                          login sequencing + probe clamp (Task 2);
                                          announceAvatar (Task 8)
    ProtocolStripBridge.swift    MODIFY  post-create .appearsAs → setParticipantAvatar (Task 6);
                                          contract comment update (Task 1)
    Strip.swift                  MODIFY  setPanelGeometry/panelGeometry, setParticipantAvatar,
                                          setTitle/setSelf wrappers
    ArtFile.swift                MODIFY  AvatarFile.iconImage() (Task 6)
    CGCanvas.swift               MODIFY  fontMetrics → CoreTextMetrics (Task 4)
    CoreTextMetrics.swift        CREATE  shared real-metrics helpers (Task 4)
    CTMetricsCanvas.swift        CREATE  measurement-only real-metrics canvas (Task 4)
    PanelFit.swift               CREATE  FitPanelsWide/ProspectivePanelWidth port (Task 5)
    ChatSessionModel.swift       CREATE  the app's testable core (Task 9)
    SettingsStore.swift          CREATE  typed UserDefaults wrapper (Task 9)
    FixtureReplayServer.swift    CREATE  loopback replay server for app demo + tests (Task 9)
  Sources/cc-dumpart/            MODIFY  real-metrics default + --fake-metrics flag (Task 4)
  Tests/ComicChatKitTests/       MODIFY/CREATE per task (all engine-state suites in the
                                          .serialized EngineGlobalStateSelfTests tree)
macos/ComicChat/                 CREATE  Xcode app project (Task 10): ComicChat.xcodeproj,
                                          ComicChatApp.swift, ConnectSheet.swift,
                                          ComicStripView.swift (Task 11), ChatWindow.swift (Task 12),
                                          Info.plist, entitlements, comicart build-phase copy
```

**Not in 4a (→ 4b backlog at the end):** emotion wheel, whisper window, character/backdrop pickers, Settings scene, avatar `URLSession` download, room list/ops UI, sounds, save/print/export, text view, CTCP auto-replies, multi-room, per-panel tiles/decode cache, full §8 acceptance.

---

## Architecture reference (cited by all tasks)

- **One serial engine queue.** `ProtocolSession` runs every `cc_session_*` call on its private serial queue and emits events (synchronously, mid-`feed_bytes`) on it. Task 1 makes that queue injectable and exposes `performOnEngineQueue`/`enqueueEngineWork`, so `cc_strip_*`/compose work interleaves *serialized* with session work. The app's flow per message: event arrives on stream → consumer (a `Task`) → `enqueueEngineWork { bridge.apply(ev); recompose() }` → main-thread image swap.
- **The transcript is the event log.** Reflow/replay = destroy strip → recreate (geometry) → re-`apply` the kept `[ProtocolEvent]` in order → recompose (the original's `ExecuteHistory(HM_RELOAD)`, D2 §2.3). Anything not derivable from the event log is lost on reflow — so the app never stores derived strings as canon.
- **Existing API this plan builds on (exact):** `ProtocolSession.init(host:port:nick:encoding:)`, `.connect()`, `.join(_:key:)`, `.say(_:text:annotations:)`, `.events: AsyncStream<ProtocolEvent>`, `.ownNick`, `.room(_:)`; `Strip.addParticipant(nick:avbPath:) -> Int32`, `.addLineCooked(speaker:text:modes:addressees:annotations:encoding:)`, `.panelCount`, `.size`, `.compose(onto:)`; `ProtocolStripBridge.init(strip:resolver:encoding:)`, `.apply(_ event:)`, `.participantIDs`; `CGCanvas.init(widthTwips:heightTwips:scale:)`, `.makeCGImage()`, `.pngData()`; `RecordingCanvas` (fake metrics: measureText `(len*120, 240)`, fontMetrics `{240,190,50,40,20,120,240}`); C: `cc_set_metrics_canvas`, `cc_session_probe_ircx`, `cc_session_send_say`, `CC_TIMER_ISIRCX_PROBE`.

---

### Task 1: Shared engine queue + session/strip interleaving proof

The single highest-risk unknown (D1 R1, D2 top-risk 1): Plan 3 only ever ran *drain-then-render*; a live app must interleave `cc_session_feed_bytes` and `cc_strip_*` on one serial queue, and that has never been executed.

**Files:**
- Modify: `macos/ComicChatKit/Sources/ComicChatKit/ProtocolSession.swift` (queue injection + two accessors)
- Modify: `macos/ComicChatKit/Sources/ComicChatKit/ProtocolStripBridge.swift` (doc contract, lines 9–22)
- Create: `macos/ComicChatKit/Tests/ComicChatKitTests/EngineInterleaveTests.swift`

**Interfaces:**
- Consumes: `ProtocolSession` internals (`sessionQueue`, `ProtocolSession.swift:93`), `LoopbackIRCServer` (test helper, `Tests/ComicChatKitTests/LoopbackIRCServer.swift`), `ProtocolStripBridge.apply(_:)`, `CGCanvas.pngData()`.
- Produces: `ProtocolSession.init(host:port:nick:encoding:engineQueue:)` (new optional last param), `func performOnEngineQueue<T>(_ body: () throws -> T) rethrows -> T`, `func enqueueEngineWork(_ body: @escaping @Sendable () -> Void)`. Tasks 9/11/12 rely on these exact names.

- [ ] **Step 1: Write the failing test.** New file `EngineInterleaveTests.swift`, suite nested in the `.serialized` tree (copy the nesting idiom from `StripTests.swift:15-24`). The test drives ONE annotated conversation two ways and byte-compares the PNGs:

```swift
import Testing
import Foundation
@testable import ComicChatKit

// Both runs feed the same scripted server lines. `interleaved: false` is the
// Plan 3 drain-then-render reference; `interleaved: true` applies each event
// to the strip (on the engine queue) as it arrives, mid-session — the live
// app's shape. Identical PNGs prove serialized interleaving is safe.
private func runConversation(interleaved: Bool) async throws -> Data {
    let server = try LoopbackIRCServer()          // existing helper
    let session = ProtocolSession(host: "127.0.0.1", port: server.port,
                                  nick: "Mac", encoding: .cp1252)
    try await session.connect()
    try await server.send(":srv 001 Mac :Welcome")
    async let _ = session.join("#p4")
    try await server.send(":Mac!u@h JOIN #p4")
    let root = repoRootFromFilePath()             // FIVE deletingLastPathComponent, see ArtTests
    let art = root + "/v2.5-beta-1-modern/comicart"
    var events: [ProtocolEvent] = []
    var bridge: ProtocolStripBridge? = interleaved
        ? ProtocolStripBridge(resolver: .init(comicartDir: art)) : nil
    // Two annotated says from a peer + one plain line.
    let lines = [
        ":Win!u@h PRIVMSG #p4 :(#G295E193M1)hello there",
        ":Win!u@h PRIVMSG #p4 :(#G012E345M1)second panel",
        ":Win!u@h PRIVMSG #p4 :plain trailer",
    ]
    var it = session.events.makeAsyncIterator()
    // consume until joined
    while let ev = await it.next() { if case .selfJoined = ev { break } }
    for line in lines {
        try await server.send(line)
        guard let ev = await it.next() else { break }
        events.append(ev)
        if interleaved, let bridge {
            session.performOnEngineQueue { try? bridge.apply(ev) }   // mid-session strip work
        }
    }
    session.disconnect()
    if bridge == nil {                             // reference: drain first, then render
        bridge = ProtocolStripBridge(resolver: .init(comicartDir: art))
        try bridge!.apply(events)
    }
    let (w, h) = bridge!.size
    let canvas = CGCanvas(widthTwips: w, heightTwips: h, scale: 2.0)
    return try session.performOnEngineQueue {
        try bridge!.compose(onto: canvas)
        return canvas.pngData() ?? Data()
    }
}

// Nest inside the EngineGlobalStateSelfTests .serialized ancestor exactly as
// StripTests does; RecordingCanvas metrics installed per the StripTests idiom
// (cc_set_metrics_canvas BEFORE the bridge creates its Strip).
@Test func interleavedMatchesDrainThenRender() async throws {
    let reference = try await runConversation(interleaved: false)
    let live = try await runConversation(interleaved: true)
    #expect(!reference.isEmpty)
    #expect(reference == live)
}
```

(The implementer wires the metrics-canvas install and exact `LoopbackIRCServer` call shapes from the existing `ProtocolSessionTests.swift:20-40` — the helper API is already there; do not invent a new server.)

- [ ] **Step 2: Run to verify it fails.** `cd macos/ComicChatKit && swift test --filter EngineInterleave 2>&1 | tail -20`. Expected: compile FAILURE — `performOnEngineQueue`/`enqueueEngineWork` do not exist.

- [ ] **Step 3: Implement the queue API.** In `ProtocolSession.swift`: change line 93's `private let sessionQueue = DispatchQueue(label: "com.comicchat.ProtocolSession")` to `private let sessionQueue: DispatchQueue`, and extend the init (line 133):

```swift
public init(host: String, port: UInt16, nick: String,
            encoding: WireEncoding = .cp1252,
            engineQueue: DispatchQueue? = nil) {
    self.sessionQueue = engineQueue ?? DispatchQueue(label: "com.comicchat.ProtocolSession")
    // ... existing body unchanged ...
}
```

Add (near the public accessors, after `currentConnectionStatus`):

```swift
/// Run engine-touching work (cc_strip_*, bridge, compose) serialized with
/// this session's cc_session_* calls. The engine is process-global and
/// single-threaded (comicchat.h threading contract): every cc_* call in the
/// process must go through this queue.
/// MUST NOT be called from inside an event-handling closure that is itself
/// running on the engine queue (sync would deadlock) — event consumers run
/// on their own Task and hop here, which is the supported shape.
public func performOnEngineQueue<T>(_ body: () throws -> T) rethrows -> T {
    dispatchPrecondition(condition: .notOnQueue(sessionQueue))
    return try sessionQueue.sync(execute: body)
}

/// Async variant: enqueue engine-touching work after any in-flight
/// cc_session_* call completes. Safe from anywhere, including event handlers.
public func enqueueEngineWork(_ body: @escaping @Sendable () -> Void) {
    sessionQueue.async(execute: body)
}
```

- [ ] **Step 4: Run the new test.** `swift test --filter EngineInterleave`. Expected: PASS — byte-identical PNGs. If the PNGs DIFFER or the engine crashes: **STOP — do not patch around it.** That is the D1-R1 fallback scenario (shared engine globals unsafe under interleaving); report NEEDS_CONTEXT with the failure detail so the snapshot-rebuild fallback (buffer events, rebuild a fresh Strip per render tick) becomes a plan amendment.

- [ ] **Step 5: Update the bridge contract comment.** `ProtocolStripBridge.swift:9-22` documents drain-then-render as the ONLY proven pattern. Rewrite that comment block to state: serialized interleaving via `ProtocolSession.performOnEngineQueue`/`enqueueEngineWork` is proven (EngineInterleaveTests); the invariant is "one serial queue for every cc_* call", not phase separation; never apply/compose from inside the `on_event` stack.

- [ ] **Step 6: Full suite + commit.** `swift test` (expect 33 existing + new green). `git add -A && git commit -m "macos: Plan 4a Task 1 - shared engine queue + session/strip interleaving proof"` (signing rule from Global Constraints).

### Task 2: Live login sequencing (probe → 451-fallback / 800-pivot → NICK/USER → loggedIn)

D4 §2's hard prerequisite: no code anywhere sends `NICK`/`USER` (verified: the loopback tests push `:srv 001` unprompted). The engine has the lifted plain-login builder (`HrIrcLogin` was kept in Task 5b when SSPI was R21-dropped); Swift must drive it.

**Files:**
- Modify: `macos/ComicChatKit/Sources/cchat-engine/include/comicchat.h` (config fields + `cc_session_login`)
- Modify: `macos/ComicChatKit/Sources/cchat-engine/bridge/cc_session.cpp`
- Modify: `macos/ComicChatKit/Sources/ComicChatKit/ProtocolSession.swift`
- Modify: `macos/ComicChatKit/Sources/cchat-engine/bridge/cc_selftest.cpp`
- Create: `macos/ComicChatKit/Tests/ComicChatKitTests/LoginSequencingTests.swift`

**Interfaces:**
- Consumes: `cc_session_probe_ircx` (comicchat.h:502), the lifted login block (`engine/ircsock.cpp:476-501` header comment; `HrIrcLogin` in the same file), `cfg.set_timer`/`cancel_timer`, `CC_TIMER_ISIRCX_PROBE`.
- Produces: `cc_session_config.own_user` + `.own_realname` (appended `const char*` fields), `int32_t cc_session_login(cc_session* s)`, `ProtocolSession` auto-login (no new public API; `connect()` now carries through to `.loggedIn`), `public var probeTimeoutMs: Int32` (default 5000). Task 9 relies on: after `connect()` returns and the server answers, a `.loggedIn` event arrives with no further calls.

- [ ] **Step 1: Read the lifted login block FIRST.** Read `engine/ircsock.cpp` around the probe machinery (the `ccModeIsIrcXFailure` body and the `800` handler, cited in D4 §2 at :487-501, :1753-1828, header comment :476-486) and locate `HrIrcLogin` — confirm (a) its exact signature and what identity fields it reads, (b) that the 451 path calls `cfg.cancel_timer` (Swift's observable), (c) how the second `800` surfaces (second `CC_EV_SERVER_CAPS` vs nothing). Record findings in the task report. If `HrIrcLogin` is not linkable/complete, STOP → NEEDS_CONTEXT (do not hand-compose NICK/USER bytes without a ruling).

- [ ] **Step 2: Write the failing C selftest.** In `cc_selftest.cpp`, `cc_selftest_session_login()`: create a session whose config sets `own_user = "Anonymous"`, `own_realname = "Anonymous"`, `local_host = "testhost"`, capture `send`; call `cc_session_login(s)`; assert the captured bytes contain `"NICK "` and `"USER "` lines whose shape matches the real 1998 capture (`Tests/ComicChatKitTests/Fixtures/captures/smoke-2.jsonl` has the client's c2s `USER Anonymous Tims-Mac …` line — mirror field order/spacing exactly, substituting config values). Register in `cc_run_selftests`. Run `swift test --filter Engine` → expected FAIL (undeclared `cc_session_login`).

- [ ] **Step 3: Implement the C side.** Append to `cc_session_config` (comicchat.h, AFTER `encoding` — append-only, R9-style): `const char* own_user; const char* own_realname;` (both nullable → fall back to own_nick). Declare `int32_t cc_session_login(cc_session* s);` next to `cc_session_probe_ircx`. In `cc_session.cpp` implement it as a thin call into the lifted `HrIrcLogin` path (whatever Step 1 found — the wrapper supplies nick via the existing `ccSessionOwnNick()` resolver and user/realname/host from config). No new wire-format code: the lifted builder emits the bytes. Selftest green.

- [ ] **Step 4: Write the failing Swift tests.** `LoginSequencingTests.swift` (serialized tree), three scenarios against `LoopbackIRCServer`, asserting the server-side received-bytes log (the helper records c2s traffic — reuse its accessor from `ProtocolSessionTests`):
  1. **Plain-IRC fallback:** `connect()` → assert first c2s line is `MODE ISIRCX`; server replies `:srv 451 * :not registered`; assert next c2s lines are `NICK Mac` then a `USER` line; server replies `:srv 001 Mac :Welcome`; assert `.loggedIn("Mac")` event.
  2. **IRCX pivot:** server replies `:srv 800 * 0 0 ANON 512 *` to the probe; assert c2s `IRCX`; server replies second `800`; assert NICK/USER follow; then 001 → `.loggedIn`.
  3. **Probe timeout:** `session.probeTimeoutMs = 200`; server stays silent; assert NICK/USER arrive within ~1s anyway (timer-driven fallback).
  Run → FAIL (no probe is ever sent today).

- [ ] **Step 5: Implement the Swift sequencing.** In `ProtocolSession.swift`:
  - Add `public var probeTimeoutMs: Int32 = 5000` and a private `var loginSent = false`, `var probing = false`.
  - In `onSocketReady()` (line 207), after `cc_session_create` + `receiveLoop()`: set `probing = true; _ = cc_session_probe_ircx(s)`.
  - In the `cSetTimer` static callback: when `timer_id == CC_TIMER_ISIRCX_PROBE`, schedule at `min(ms, probeTimeoutMs)`.
  - Trigger `sendLoginIfNeeded()` (sets `loginSent`, calls `cc_session_login`) from ALL of: the `cCancelTimer` callback when the probe timer is cancelled while `probing` (the 451 path — engine cancels on 451), the probe-timer fire path (after `cc_session_fire_timer`), and the `.serverCaps` handling per Step 1's finding about the second 800. Guard: `loginSent` one-shot; all on `sessionQueue`.
- [ ] **Step 6: Run all tests.** `swift test` — all three scenarios + existing suite green. If scenario 2's second-800 trigger doesn't match Step 1's reading, adjust per the code, not per this plan (report it).
- [ ] **Step 7: Commit.** `git commit -m "macos: Plan 4a Task 2 - live login sequencing (probe/451/800 -> cc_session_login)"`.

**Task 2 amendments (recorded 2026-07-18, reviewer-adjudicated):**
1. **Step 1's premise was wrong: `HrIrcLogin` was never ported** (only its trigger survived Plan 3 Task 5b). `cc_session_login` is implemented fresh in bridge code, byte-verified against the smoke-2.jsonl capture (`NICK <nick>\r\n` + `USER <user> <machine> . :<realname>\r\n`, capture lines 11-12 — the REAL client's lines; line 2 is a different client, not ground truth).
2. **R18 site added (adjudicated (a), no new rule):** `engine/ircsock.cpp` second-800/ANON branch — original called `HrIrcXLogin(TRUE)`→`HrIrcLogin` directly (v2.5 ircsock.cpp:2889-2894, :679-680); the port emits a second `CC_EV_SERVER_CAPS` as the login edge-trigger. Joins the individually-listed R18 sites. Semantic note: the second caps event carries no new caps — it is a pure edge-trigger (commented at both emit site and Swift consumer).
3. **Plan 3 Task 4 latent defect fixed:** `cc_session_probe_ircx` never set `m_bJustSentModeIsIrcX` (original OnConnect sets it — v2.5 ircsock.cpp:1050-1052), silently deadening the 451 fast-path guard at engine/ircsock.cpp:488. Now set when the probe sends.
4. **Reentrancy constraint (binding on future bridge work):** `cc_session_login` is the first `cc_session_*` entry point called from inside an active engine frame (via on_event). The `g_session` activation pattern must SAVE/RESTORE, never unconditionally null on exit — any future reentrant `cc_session_*` inherits this rule.

### Task 3: CP-1252 case-fold table + escaped-byte annotation vector (the two Plan 3 must-owns)

**Files:**
- Modify: `macos/ComicChatKit/Sources/cchat-engine/shim/mfc_compat.h` (+`.cpp` if the fold lives there — `CharUpperBuff` shim at `mfc_compat.h:315-336`)
- Modify: `macos/ComicChatKit/Sources/cchat-engine/bridge/cc_selftest.cpp`

**Interfaces:**
- Consumes: the ASCII-only `CharUpperBuff` shim; the lifted low-level quote/unquote pair in `engine/ccommon_str.cpp` (`bLowLevelQuoting`/`bLowLevelUnquoting`); the annotation decode entry (`ProcessUDIData` path) + `ccActivateSessionForTest` (test bridge, `cc_session.h`).
- Produces: a complete CP-1252 fold; selftests `cc_selftest_cp1252_fold` and `cc_selftest_annotation_escaped_bytes`. Task 4 depends on the fold (under real metrics, case changes measured width — D3 §4).

- [ ] **Step 1: Failing fold selftest.** Table-driven over all 256 bytes against an expected array. Spot rules the table must encode: `'a'..'z'→-0x20`; `0x9A→0x8A` (š→Š), `0x9C→0x8C` (œ→Œ), `0x9E→0x8E` (ž→Ž), `0xFF→0x9F` (ÿ→Ÿ); `0xE0..0xFE → -0x20` (à→À … þ→Þ) **except `0xF7` (÷, unchanged)**; `0xDF` (ß) unchanged (CP-1252 has no uppercase ß — matches Win32 `CharUpperBuffA`); `0xB5` (µ) unchanged; everything else identity. Run → FAIL (current shim uppercases ASCII only).
- [ ] **Step 2: Implement the fold** in the `CharUpperBuff` shim exactly per that rule set (a 256-entry `static const unsigned char` table is the cleanest and self-documenting). **Also fix the shim's comment**: it currently claims the accented mapping has "no authoritative source" — replace with "deterministic published Windows-1252 uppercase mapping" (handoff correction). Selftest green.
- [ ] **Step 3: Failing escaped-byte vector.** `cc_selftest_annotation_escaped_bytes()`: (a) direct pair test — run `bLowLevelQuoting` over a buffer containing `{0x0A, 0x0D, 0x10}` plus printable padding, assert the output contains the quote char `0x10` escapes and round-trips through `bLowLevelUnquoting` byte-identically (read the lifted functions FIRST for the exact escape alphabet; assert against what the 1998 code actually does, not an assumed CTCP table); (b) end-to-end decode — feed a hand-authored inbound `PRIVMSG` line whose inline `(#…)` annotation body contains quoted CR/LF bytes through the session (activate via `ccActivateSessionForTest`, feed via `cc_session_feed_bytes`, capture `on_event`), assert `CC_EV_TEXT` fires with `has_annotations==1` and the decoded fields — this makes Plan 3 Task 8's "unquoting ran as a no-op" gap falsifiable. Mark the vector `/* HAND-AUTHORED — promote a real captured escaped-byte exchange during the 4b live acceptance */`.
- [ ] **Step 4: Implement/adjust until green, full suite, commit.** `swift test`; `git commit -m "macos: Plan 4a Task 3 - CP-1252 CharUpperBuff fold table + escaped-byte annotation vectors"`.

### Task 4: Real CoreText layout metrics (two-mode: CTMetricsCanvas default, RecordingCanvas for goldens)

D3's verdict implemented: switch layout to real metrics EARLY, before any view geometry work. The three CGCanvas placeholder fields are write-only today (D3 §2) — fixing them is debt-retirement; the behavior change is the metrics-canvas flip.

**Files:**
- Create: `macos/ComicChatKit/Sources/ComicChatKit/CoreTextMetrics.swift`
- Create: `macos/ComicChatKit/Sources/ComicChatKit/CTMetricsCanvas.swift`
- Modify: `macos/ComicChatKit/Sources/ComicChatKit/CGCanvas.swift:205-218` (`fontMetrics`)
- Modify: `macos/ComicChatKit/Sources/cc-dumpart/DemoStrip.swift:34-35`, `ScriptStrip.swift:15-16`, `ReplayStrip.swift:272-273` (+ argument parsing in `main.swift`)
- Create: `macos/ComicChatKit/Tests/ComicChatKitTests/RealMetricsTests.swift`

**Interfaces:**
- Consumes: `FontSpec`/`TextMetrics`/`Canvas` (Canvas.swift), `CGCanvas`'s existing CTFont cache + `measureText` (CGCanvas.swift:190-203), `cc_set_metrics_canvas`.
- Produces: `enum CoreTextMetrics { static func metrics(for: FontSpec) -> TextMetrics; static func measure(_ f: FontSpec, bytes: UnsafePointer<CChar>?, len: Int32) -> (w: Int32, h: Int32) }`; `public final class CTMetricsCanvas: Canvas` (metrics/measure real; draw ops `assertionFailure` no-ops); cc-dumpart flag `--fake-metrics`. Tasks 9/11 install `CTMetricsCanvas` before `cc_strip_create`.

- [ ] **Step 1: Failing metrics-value tests.** `RealMetricsTests.swift` (serialized tree). Tolerance assertions for Comic Sans MS at `FontSpec(face:"Comic Sans MS", height:-240, …)` (12 pt), from D3 §3's measured table:

```swift
@Test func comicSansRealMetrics() {
    let m = CoreTextMetrics.metrics(for: FontSpec(face: "Comic Sans MS", height: -240,
                                                  weight: 400, italic: false,
                                                  underline: false, strikeout: false, charset: 0))
    #expect((260...270).contains(m.ascent))          // measured 264.5 tw
    #expect((66...74).contains(m.descent))           // measured 70.0
    #expect(m.height == m.ascent + m.descent)        // 334.5
    #expect((88...101).contains(m.internalLeading))  // height − |lfHeight| ≈ 94.5
    #expect(m.externalLeading == 0)                  // Comic Sans has no line gap
    #expect((115...155).contains(m.aveCharWidth))    // OS/2 xAvgCharWidth ≈ 124.9
    #expect((240...310).contains(m.maxCharWidth))    // hhea advanceWidthMax ≈ 249.5
}
```

Run → FAIL (`CoreTextMetrics` undefined).

- [ ] **Step 2: Implement `CoreTextMetrics`.** Factor CGCanvas's font-cache/measure logic into the shared helper; `metrics(for:)` computes: ascent/descent/leading via `CTFontGetAscent/Descent/Leading` (×20 to twips, as CGCanvas does today); `internalLeading = max(0, height - abs(Int(f.height)))`; `aveCharWidth` from the OS/2 table (`CTFontCopyTable(font, kCTFontTableOS2, [])` → `xAvgCharWidth` at byte offset 2, big-endian Int16, scaled `pt/unitsPerEm×20`) with fallback = measured `"abcdefghijklmnopqrstuvwxyz"` width / 26; `maxCharWidth` from the hhea table (`advanceWidthMax`, offset 10, UInt16, same scaling) with fallback = max measured single-ASCII advance. Document the convention in a header comment (D3 open Q5: table-reads primary, measurement fallback). Test green.
- [ ] **Step 3: `CTMetricsCanvas` + CGCanvas patch.** `CTMetricsCanvas: Canvas`: `measureText`/`fontMetrics` delegate to `CoreTextMetrics`; `drawText/fillRect/drawImage/path/clipPush/clipPop` are `assertionFailure("CTMetricsCanvas is measurement-only")` no-ops; `isPrinting() = false`. Replace `CGCanvas.fontMetrics`'s three placeholder lines with `CoreTextMetrics.metrics(for: f)` (drawing canvas and metrics canvas now agree by construction). Add a layout-effect test: same 2-line script composed twice (fake vs real metrics canvases installed before their `cc_strip_create`s), assert `realStripHeight > fakeStripHeight` (the ~207→~334 twip line cell must grow balloons) and both compose without error.
- [ ] **Step 4: Flip cc-dumpart's default.** All three metrics-canvas install sites use `CTMetricsCanvas()` unless a new `--fake-metrics` flag is passed (flag parsed in `main.swift`, threaded to the three modes; the flag exists to reproduce fake-metrics goldens on demand). The frozen `strip-golden.txt` tests are UNTOUCHED (they install their own `RecordingCanvas` — D3 §5).
- [ ] **Step 5: Full suite + visual proof.** `swift test` (every existing golden must stay green — if `strip-golden.txt` breaks, a test is NOT installing its own metrics canvas; fix the test setup, never the golden). Then `swift run -c release cc-dumpart --strip /tmp/p4-metrics.png` (or the mode's output convention) and **view the PNG**: balloon text must sit visually centered (the "sits high" artifact gone). Attach to the task report; the coordinator eyeballs it (real-artifact discipline).
- [ ] **Step 6: Commit.** `git commit -m "macos: Plan 4a Task 4 - real CoreText layout metrics (CTMetricsCanvas, two-mode)"`.

### Task 5: Panel geometry API + viewport-fit math

Panel size is hard-seeded to 2300 twips and panels-per-row to 2, with no accessors (D2 §1.3); the original derives square panels from the client width (D2 §2.3). The view (Task 11) needs both.

**Files:**
- Modify: `macos/ComicChatKit/Sources/cchat-engine/include/comicchat.h`
- Modify: `macos/ComicChatKit/Sources/cchat-engine/bridge/cc_compose.cpp` (seeding at :143-144)
- Modify: `macos/ComicChatKit/Sources/cchat-engine/bridge/cc_selftest.cpp`
- Modify: `macos/ComicChatKit/Sources/ComicChatKit/Strip.swift`
- Create: `macos/ComicChatKit/Sources/ComicChatKit/PanelFit.swift`
- Create: `macos/ComicChatKit/Tests/ComicChatKitTests/PanelFitTests.swift`

**Interfaces:**
- Consumes: `CUnitPanelPage::SetUnitPanelWidth/SetUnitPanelHeight/SetUnitPanelsPerRow` (engine/panel.h:156-158; width setter cascades `UpdateTitleFonts`), interstice statics (engine/panel.cpp:70-76), `MINUNITPANELWIDTH` 2300 (panel.h:165-166).
- Produces (exact):

```c
/* Mirrors the original CPageView::SetPanelsWide: unit panel size (square in
 * the original) + panels per row. FRESH STRIP ONLY — returns nonzero if any
 * line has been added (reflow ≡ destroy + recreate + replay, by design). */
int32_t cc_strip_set_panel_geometry(cc_strip* s, int32_t unit_w_twips,
                                    int32_t unit_h_twips, int32_t panels_per_row);
void    cc_strip_get_panel_geometry(const cc_strip* s, int32_t* unit_w,
                                    int32_t* unit_h, int32_t* per_row,
                                    int32_t* h_interstice, int32_t* v_interstice);
```

  Swift: `Strip.setPanelGeometry(unitTwips: Int32, panelsPerRow: Int32) throws`, `Strip.panelGeometry -> (unitW: Int32, unitH: Int32, perRow: Int32, hInter: Int32, vInter: Int32)`; `PanelFit.columns(forViewportWidthTwips:) -> Int32` and `PanelFit.unitPanelTwips(viewportWidthTwips:columns:) -> Int32`. Tasks 9/11 call exactly these.

- [ ] **Step 1: Failing C selftest** — set geometry (3200, 3200, 3) on a fresh strip → getter round-trips; add a line then attempt set → nonzero; `cc_strip_get_size` of a 3-line strip reflects the new arithmetic (`cols*W + (cols-1)*vInt` — D2 §1.3 table). Run → FAIL (undeclared).
- [ ] **Step 2: Implement** in `cc_compose.cpp` as thin wrappers over the panel.h setters + statics reads; the create-time hard-seed at :143-144 stays as the default. Selftest green.
- [ ] **Step 3: Failing Swift tests** — `PanelFitTests` (pure, NOT in the serialized tree — no engine state): port table from the original (`pageview.cpp:1394-1404, 1143-1158`, constants `COMFORTABLEPANELWIDTH=3000`, max 5 columns, clamp ≥2300):

```swift
@Test func fitColumns() {
    // viewport twips → expected columns: largest 1...5 whose panel width ≥ 3000
    #expect(PanelFit.columns(forViewportWidthTwips: 2500) == 1)   // below comfortable → 1
    #expect(PanelFit.columns(forViewportWidthTwips: 6200) == 2)
    #expect(PanelFit.columns(forViewportWidthTwips: 9400) == 3)
    #expect(PanelFit.columns(forViewportWidthTwips: 40000) == 5)  // capped at 5
    #expect(PanelFit.unitPanelTwips(viewportWidthTwips: 6200, columns: 2) >= 2300)
}
```

- [ ] **Step 4: Implement `PanelFit`** — new Swift (provenance comment citing pageview.cpp lines; this is a port of view code that was never lifted, not a lift): `columns` = largest n in 1...5 with `(viewport - interstices(n)) / n >= 3000`, else 1; `unitPanelTwips` = `(viewport - interstices(cols)) / cols` clamped `>= 2300` (square panels — height = width, `pageview.cpp:1114-1115`). Interstice = 144 twips between columns (from the Task 5 getter's constants). Tests green; expected-value table adjusted only if the port's arithmetic honestly disagrees with the guess above (fix the TABLE from the original's math, show the derivation in the report).
- [ ] **Step 5: Full suite + commit.** `git commit -m "macos: Plan 4a Task 5 - panel geometry API + PanelFit viewport math"`.

### Task 6: Avatar APIs — `cc_strip_set_participant_avatar` + `cc_avatar_icon_image` + bridge wiring

**Files:**
- Modify: `macos/ComicChatKit/Sources/cchat-engine/include/comicchat.h`
- Modify: `macos/ComicChatKit/Sources/cchat-engine/bridge/cc_compose.cpp`
- Modify: `macos/ComicChatKit/Sources/cchat-engine/bridge/bridge_art.cpp` (+`.h`)
- Modify: `macos/ComicChatKit/Sources/cchat-engine/bridge/cc_selftest.cpp`
- Modify: `macos/ComicChatKit/Sources/ComicChatKit/Strip.swift`, `ArtFile.swift`, `ProtocolStripBridge.swift:123-129`
- Modify: `macos/ComicChatKit/Tests/ComicChatKitTests/ProtocolToStripTests.swift` (bridge test)

**Interfaces:**
- Consumes: `cc_strip_add_participant`'s load half (cc_compose.cpp:188-206), the original wiring `SetUserAvatarID` (`v2.5-beta-1-modern/userinfo.cpp:38-41`: `pui->SetAvatarID(avID); GetAvatar(avID)->m_userInfo = pui;`), `cc_avatar_pose_image` (bridge_art.cpp:470-492), `CAvatarX::GetIconPose()`/`m_icon` (engine/avatar.h:251), the D2 §6b provenance (`ChangeAvatarEntry::Execute`, histent.cpp:368-413 — future panels only).
- Produces (exact):

```c
/* Load the .avb at avb_path, register it, re-point participant's session user
 * at it. Existing panels keep the old avatar (original ChangeAvatarEntry
 * behavior); subsequent lines render with the new one. 0 = ok. */
int32_t cc_strip_set_participant_avatar(cc_strip* s, int32_t participant,
                                        const char* avb_path);
/* The member-list/picker icon pose (excluded from the pose API by design —
 * Plan 1 handover). NULL mask plane decodes opaque. 0 = ok. */
int32_t cc_avatar_icon_image(const cc_avatar* av, cc_image* out);
```

  Swift: `Strip.setParticipantAvatar(_ participant: Int32, avbPath: String) throws`; `AvatarFile.iconImage() throws -> ArtImage`. Bridge behavior change: post-create `.appearsAs` now resolves via its `AvatarResolver` and calls `setParticipantAvatar` (replacing the documented no-op).

- [ ] **Step 1: Failing C selftests.** (a) `cc_avatar_icon_image` on a comicart avatar → rc 0, `out` width/height > 0; (b) `cc_strip_set_participant_avatar`: strip + participant(anna) + one line, switch to armando.avb → rc 0, add another line, compose to the C recording canvas → ops stream still valid (panel count 2+, compose rc 0); bad participant id → nonzero. Run → FAIL.
- [ ] **Step 2: Implement.** `cc_avatar_icon_image` = ~20-line clone of `cc_avatar_pose_image` using `GetPoseFromID(av->avatar->m_icon)` (bridge code, no lift; NULL-mask→opaque already handled by `decodeDibToRgba`). `cc_strip_set_participant_avatar` = R16 bridge code: mirror the add_participant load half (open stream → `LoadAvatar` → `IndexAvatar`), then the `SetUserAvatarID` two-liner against the participant's session user; provenance comments cite userinfo.cpp:38-41 + histent.cpp:368-413. Selftests green.
- [ ] **Step 3: Swift wrappers + bridge.** `Strip.setParticipantAvatar` (same error-wrapping idiom as `addParticipant`); `AvatarFile.iconImage()` (same shape as the existing pose-image accessor in ArtFile.swift). In `ProtocolStripBridge.apply`, replace the `.appearsAs` no-op branch: existing participant → `resolver` resolves the announced name → `strip.setParticipantAvatar(id, avbPath: path)`; unknown participant → keep the current stash behavior. Update `announcedAvatarNames` in both branches.
- [ ] **Step 4: Failing→green bridge test.** In `ProtocolToStripTests` (serialized tree): apply `.userJoined("Win")` + one `.text` (participant created with resolver default), then `.appearsAs(nick:"Win", avatarName:"armando", …)`, then another `.text`; assert no throw and `bridge.announcedAvatarNames["Win"] == "armando"`; compose → non-empty render. Full suite green.
- [ ] **Step 5: Commit.** `git commit -m "macos: Plan 4a Task 6 - set_participant_avatar + avatar icon accessor + appearsAs wiring"`.

### Task 7: Title/starring rendering (un-R11 lift) + `cc_strip_set_title`/`cc_strip_set_self`

The one real LIFT task in 4a — **Edit Rules fully binding**; every reroute individually listed in the task report. Provenance map is D2 §6a.

**Files:**
- Modify: `macos/ComicChatKit/Sources/cchat-engine/engine/panel.cpp` (un-wrap the `#ifndef CC_NO_UI` regions holding `AddTitle`/`UpdateTitle`/`AddStars`/`AddStarsAux` — currently :1370-1422 and :517-587; original sources panel.cpp:1279-1297, :1300-1314, :1391-1446, :477-514)
- Modify: `macos/ComicChatKit/Sources/cchat-engine/engine/balloon.cpp` (`CStarLabel::Draw`, original balloon.cpp:1112-1129)
- Modify: `macos/ComicChatKit/Sources/cchat-engine/shim/mfc_compat.h/.cpp` (`CDC::DrawTextEllipsis` adapter, R9)
- Modify: `macos/ComicChatKit/Sources/cchat-engine/include/comicchat.h`, `bridge/cc_compose.cpp`, `bridge/cc_selftest.cpp`
- Modify: `macos/ComicChatKit/Sources/ComicChatKit/Strip.swift`

**Interfaces:**
- Consumes: `ccContext().session.comicsTitle` (already populated — shim/engine_context.h:69), the session user table, `cc_strip_add_line`'s speaker bookkeeping.
- Produces (exact):

```c
int32_t cc_strip_set_self(cc_strip* s, int32_t participant); /* starring order: self first */
int32_t cc_strip_set_title(cc_strip* s, const char* title_bytes); /* CP-1252; adds/updates panel 0 */
```

  Swift: `Strip.setSelf(_ participant: Int32) throws`, `Strip.setTitle(_ title: String) throws`. **Panel 0 becomes the title panel once `set_title` is called** — `panelCount`/`get_size` include it; `UpdateTitle` is the ONE mutation of a non-last panel (D2 §1.2) — Task 11's "only last panel dirty" reasoning must treat panel 0 as dirty after any participant/title change.

- [ ] **Step 1: Failing selftest.** `cc_selftest_strip_title_starring()`: strip → 2 participants → `cc_strip_set_self(s, p1)` → `cc_strip_set_title(s, "MY COMIC")` → 2 lines → compose onto the C recording canvas. Assert: panel_count ≥ 3 (title + 2), ops stream contains a text op with bytes `MY COMIC`, a text op with `Starring`, and per-participant star rows (nickname text ops); then add a 3rd participant → `UpdateTitle` path → recompose → its nickname appears. Also assert the frozen `strip-golden.txt` Swift test still passes untouched (no `set_title` call → `AddTitle` never runs — byte-identical is the requirement).
- [ ] **Step 2: Lift.** Un-R11 the four panel.cpp functions + `CStarLabel::Draw`, applying (and individually listing) these reroutes:
  - `g_mapNickToPtr` iteration in `AddStarsAux` → the session user table (`ccContext().session` users), ordered: self first (from `set_self`), others by `m_nSends` descending, departed last — **verify the lifted session user struct has a sends counter; if absent, add one (R17-listed) incremented in the `cc_strip_add_line`/`_cooked` bridge entry per speaker** (mirrors the original's per-say `m_nSends++` — locate the original increment site and cite it in the report).
  - `MyAvatarID()`/`MyAvatar()` → the `set_self` participant's session user (note: `AddStars` early-returns when self is unset, panel.cpp:1394 — preserve; `set_title` before `set_self` renders title-only, which is valid).
  - `starringStr.LoadString(ID_STARRING)` → R9 constant `"Starring"` (chat.rc string resource value — verify against `v2.5-beta-1-modern/chat.rc` and use the exact resource text).
  - `DrawTextEx(…, DT_END_ELLIPSIS …)` in `CStarLabel::Draw` → new shim member `CDC::DrawTextEllipsis(const char* sz, RECT* rc, UINT flags)` implemented over the existing `measure_text` + `draw_text` canvas ops (binary-search the longest prefix that fits with `"..."` appended; single-line; transparent background). R9: selftest it directly (short string untruncated; long string ends in `...` and fits).
  - `RefreshPanelN`/view pokes → existing headless no-ops (unchanged).
  Any original pattern not covered above → STOP, NEEDS_CONTEXT (candidate R22).
- [ ] **Step 3: The C API + Swift wrappers.** `cc_strip_set_title` stores to `session.comicsTitle` and calls `AddTitle` (no panels yet) / `UpdateTitle` (panels exist) per the original's `UpdateTitle` decision (panel.cpp:1300-1314); `cc_strip_set_self` records the participant and triggers `UpdateTitle` when a title exists. Wire `UpdateTitle` on participant add too (original: member join → `UpdateTitle`, protsupp.cpp:461). Swift wrappers follow `addParticipant`'s idiom.
- [ ] **Step 4: All selftests + full suite green.** Pay attention to: `strip-golden.txt` byte-identical; `UpdateTitleFonts` cascade from Task 5's width setter now touches real code (title fonts scale with panel width — panel.h:156).
- [ ] **Step 5: Visual proof.** Extend one cc-dumpart mode (`--script` input already carries a conversation; add optional `"title"` + `"self"` keys to its JSON) → render → **view the PNG**: title panel with "Starring" + icons + names. Attach to report.
- [ ] **Step 6: Commit.** `git commit -m "macos: Plan 4a Task 7 - title/starring lift (un-R11) + set_title/set_self"`.

### Task 8: Outbound avatar announce — `cc_session_announce_avatar` + auto-announce rules

D1 R2 / D4 §4b: without an outbound `# Appears as`, Windows peers render us as a random stand-in. The announce must ride the annotations argument so the IRCX-DATA-vs-plain-PRIVMSG branch is inherited (D4 §4).

**Files:**
- Modify: `macos/ComicChatKit/Sources/cchat-engine/include/comicchat.h`
- Modify: `macos/ComicChatKit/Sources/cchat-engine/bridge/cc_session.cpp`
- Modify: `macos/ComicChatKit/Sources/cchat-engine/bridge/cc_selftest.cpp`
- Modify: `macos/ComicChatKit/Sources/ComicChatKit/ProtocolSession.swift`
- Create: `macos/ComicChatKit/Tests/ComicChatKitTests/AnnounceTests.swift`

**Interfaces:**
- Consumes: the lifted announce grammar builder (`ChatAnnounceNewAvatar`, original protsupp.cpp:817-843 — check the lifted `engine/protsupp.cpp` first: it is likely R20-wrapped; un-wrap the pure string-builder half or reimplement as R16 bridge code with the citation), the lifted channel/priv send paths (`bChatSendToChannel`/`bChatSendPrivMesg` via the Task-4 Plan-3 lift).
- Produces (exact):

```c
/* "# Appears as <name>" (url NULL) or "# Appears as <name>.<url>", sent as
 * the ANNOTATIONS argument (rides PRIVMSG inline on plain IRC, DATA on IRCX —
 * inherited from bChatSendToTarget). to_nick NULL = channel-wide; non-NULL =
 * private reply-announce (the protsupp.cpp:868-877 rule). 0 = ok. */
int32_t cc_session_announce_avatar(cc_session* s, uint32_t room_token,
                                   const char* to_nick, const char* name,
                                   const char* url);
```

  Swift: `ProtocolSession.announceAvatar(channel: String, toNick: String? = nil, name: String, url: String? = nil) async throws` (onQueueGated like `say`). Task 9's model announces on `.selfJoined` and reply-announces privately on first `.appearsAs` from an unseen nick.

- [ ] **Step 1: Read the lifted announce/send path.** Locate `ChatAnnounceNewAvatar` in `engine/protsupp.cpp` (or its R20 wrap) and confirm how a `#`-comment travels through `bChatSendToChannel` (annotations argument, empty message). Byte ground truth: the real 1998 client's announce is IN the committed capture — find the `# Appears as` c2s line in `Tests/ComicChatKitTests/Fixtures/captures/smoke-2.jsonl`. Record the exact wire form in the report.
- [ ] **Step 2: Failing C selftest.** Capture `cfg.send`; `cc_session_announce_avatar(s, token, NULL, "Anna", NULL)` → sent bytes match the smoke-2 wire form modulo name/channel; with `to_nick="Win"` → private form; with `url` → `<name>.<url>` grammar. Run → FAIL; implement per Step 1 (lift the builder if clean, else R16 with citation); green.
- [ ] **Step 3: Swift + loopback test.** `announceAvatar` forwarding (mirror `say`'s `onQueueGated` + CP-1252 conversion idiom). `AnnounceTests` (serialized tree): loopback session logs in (Task 2), joins, `announceAvatar(channel:"#p4", name:"Anna")` → server-side c2s log contains the exact announce line. Green.
- [ ] **Step 4: Full suite + commit.** `git commit -m "macos: Plan 4a Task 8 - outbound avatar announce (cc_session_announce_avatar)"`.

### Task 9: `ChatSessionModel` + `SettingsStore` + `FixtureReplayServer` (the app's headless core)

All logic in ComicChatKit, only chrome in the app (D1 §3.2 binding recommendation). This task is the app, minus pixels.

**Files:**
- Create: `macos/ComicChatKit/Sources/ComicChatKit/ChatSessionModel.swift`
- Create: `macos/ComicChatKit/Sources/ComicChatKit/SettingsStore.swift`
- Create: `macos/ComicChatKit/Sources/ComicChatKit/FixtureReplayServer.swift`
- Create: `macos/ComicChatKit/Tests/ComicChatKitTests/ChatSessionModelTests.swift`
- Create: `macos/ComicChatKit/Tests/ComicChatKitTests/SettingsStoreTests.swift`

**Interfaces:**
- Consumes: Tasks 1–8 exactly as produced (`engineQueue:` injection, auto-login, `CTMetricsCanvas`, `PanelFit`, `setPanelGeometry`, `setParticipantAvatar` via bridge, `setTitle`/`setSelf`, `announceAvatar`).
- Produces (exact — Tasks 10–12 build on these):

```swift
public struct ChatConfig: Sendable {
    public var host: String; public var port: UInt16
    public var nick: String; public var room: String
    public var encoding: WireEncoding
    public var characterName: String     // bare avb name, e.g. "anna"
    public var backdropName: String      // bare bgb name, e.g. "field"
    public var artDir: String            // absolute dir holding comicart files
    public init(host: String, port: UInt16, nick: String, room: String,
                encoding: WireEncoding = .cp1252, characterName: String = "anna",
                backdropName: String = "field", artDir: String)
}

public final class ChatSessionModel: @unchecked Sendable {
    public init(config: ChatConfig)
    public var onStripImage: (@Sendable (CGImage, CGSize) -> Void)?   // main-thread; size in points
    public var onMembers:    (@Sendable ([String]) -> Void)?          // main-thread, sorted nicks
    public var onStatus:     (@Sendable (String) -> Void)?            // main-thread status/error line
    public func start() async throws          // connect → (auto-login) → join → announce
    public func send(_ text: String) async throws        // say, annotations nil (see note)
    public func setViewport(widthPoints: CGFloat, scale: CGFloat)     // reflow (destroy+replay)
    public func shutdown()
    public private(set) var transcript: [ProtocolEvent]   // the canonical event log
}

public struct SettingsStore {                  // typed UserDefaults wrapper (injectable)
    public init(defaults: UserDefaults = .standard)
    public var server: String        // key "connect.server", default ""
    public var port: Int             // "connect.port", 6667
    public var room: String          // "connect.room", ""
    public var encoding: WireEncoding// "connect.encoding", .cp1252
    public var nick: String          // "persona.nick", ""
    public var character: String     // "persona.character", "anna"
    public var backdrop: String      // "comic.backdrop", "field"
}

public final class FixtureReplayServer {       // offline demo/E2E: replays a capture's s2c lines
    public init(fixtureURL: URL) throws        // jsonl {t,dir,hex,latin1}; uses s2c hex lines
    public var port: UInt16 { get }
    public func start() throws                 // NWListener on 127.0.0.1; on client connect,
                                               // replays s2c lines paced by protocol echoes
    public func stop()
}
```

  **Outbound annotation note (scope decision, deliberate):** MVP sends `annotations: nil` — an unannotated say. Receiving 1998 clients run their own text→pose inference on unannotated text (`chatdoc.cpp:451` gate), so peers still see a posed comic; this is exactly a 1998 client with ComicsData off. Cooked outbound poses arrive with the emotion wheel in 4b.

- [ ] **Step 1: Failing SettingsStore test.** Round-trip each property against an injected `UserDefaults(suiteName: "p4-test")!` (removePersistentDomain in setup); defaults match the table above. Implement `SettingsStore` (plain computed properties over `defaults`). Green. (No engine state — outside the serialized tree.)
- [ ] **Step 2: Failing ChatSessionModel test — the live loop.** In `ChatSessionModelTests` (serialized tree), against `LoopbackIRCServer`:

```swift
@Test func liveLoopRendersAndSends() async throws {
    let server = try LoopbackIRCServer()
    let art = repoRoot() + "/v2.5-beta-1-modern/comicart"
    let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                               nick: "Mac", room: "#p4", artDir: art))
    var images: [CGSize] = []
    let imagesArrived = AsyncStream<Void>.makeStream()
    model.onStripImage = { _, size in images.append(size); imagesArrived.continuation.yield() }
    try await model.start()
    // login handshake (Task 2): reply 451 → NICK/USER → 001 → JOIN echo
    try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")
    // a peer's annotated message must produce a strip image
    try await server.send(":Win!u@h PRIVMSG #p4 :(#G295E193M1)hello mac")
    var iter = imagesArrived.stream.makeAsyncIterator()
    _ = await iter.next()
    #expect(images.count >= 1 && images.last!.width > 0)
    // sending a say emits PRIVMSG bytes
    try await model.send("hi win")
    let sent = await server.receivedLines()
    #expect(sent.contains { $0.hasPrefix("PRIVMSG #p4 :") && $0.contains("hi win") })
    // the self-join announce (Task 8) went out
    #expect(sent.contains { $0.contains("# Appears as Anna") || $0.contains("# Appears as anna") })
    model.shutdown()
}
```

  (`replyToProbeWith451ThenWelcomeAndJoin` is a small test-helper extension the implementer adds beside `LoopbackIRCServer`, composing the Task-2 scenario-1 lines; `receivedLines` is the existing c2s accessor — reuse, don't reinvent.) Run → FAIL (`ChatSessionModel` undefined).

- [ ] **Step 3: Implement `ChatSessionModel`.** Core rules (each is load-bearing; the report lists how each is honored):
  - One `DispatchQueue(label: "com.comicchat.engine")` injected into `ProtocolSession(engineQueue:)` and used for ALL strip work.
  - Init order on the engine queue at `start()`: install a `CTMetricsCanvas` (kept alive as a property) via `cc_set_metrics_canvas` BEFORE creating the `Strip`; geometry from `PanelFit` defaults (3 columns until a viewport arrives); `Strip` + `ProtocolStripBridge(strip:resolver:)` with `AvatarResolver(comicartDir: config.artDir)`; `setBackdrop(artDir + "/" + backdropName + ".bgb")`; self participant = `addParticipant(nick: config.nick, avbPath: artDir + "/" + characterName + ".avb")` → `setSelf`.
  - Event consumer `Task`: `for await ev in session.events` → append to `transcript` → route: `.selfJoined` → `announceAvatar(channel: room, name: characterName.capitalized)`; first `.appearsAs` from an unseen nick → private reply-announce (`toNick:`) then strip work; strip-relevant events → `session.enqueueEngineWork { try? bridge.apply(ev); self.recomposeLocked() }`; membership events → recompute sorted member list from `session.room(config.room)?.members` → `DispatchQueue.main.async { onMembers?(...) }`; `.statusLine`/`.error`/`.disconnectedHint` → `onStatus`.
  - `recomposeLocked()` (engine queue only): `strip.size` → skip if zero panels → `CGCanvas(widthTwips:heightTwips:scale:)` at the current viewport scale → `bridge.compose(onto:)` → `makeCGImage()` → main-thread `onStripImage(image, sizePoints)` where `sizePoints = CGSize(width: CGFloat(w)/20, height: CGFloat(h)/20)`.
  - `send(_:)`: `session.say(room, text: text, annotations: nil)` then locally append the echo? **No** — the original renders own says from the server echo/local history entry; our loopback echoes PRIVMSG back only if the server does. Decision (faithful + simple): render own says immediately by synthesizing the `.text` event locally (the 1998 client adds its own SayEntry locally at send time — `bChatSendText` → local `AddAndExecute`, protsupp provenance) — append the synthetic event to `transcript` AND feed it through the same enqueue path, so own lines appear without server echo.
  - `setViewport(widthPoints:scale:)`: compute `columns = PanelFit.columns(forViewportWidthTwips: Int32(widthPoints*20))`, `unit = PanelFit.unitPanelTwips(...)`; if unchanged → just recompose at new scale; else `enqueueEngineWork`: destroy strip (close), recreate (metrics canvas still installed) with `setPanelGeometry`, re-add backdrop/self, fresh bridge, re-`apply(transcript)` in order, recompose (D2 §2.3 / risk 2: replay is the reflow).
  - `shutdown()`: cancel consumer task, `session.disconnect()`, release strip on engine queue.
- [ ] **Step 4: `FixtureReplayServer`.** NWListener that accepts one connection and replays the fixture's `dir=="s2c"` hex payloads: send the pre-001 chunk on connect; thereafter pace by watching for the client's `MODE ISIRCX`/`NICK`/`JOIN` lines (reply from the fixture stream in order). Keep it dumb: strip/replace the fixture's nick with the connecting client's is NOT needed if the app connects with the fixture's nick ("Anonymous") — document that the demo uses `--replay-fixture` with the fixture nick. Test: `FixtureReplayServerTests` — point a `ChatSessionModel` at it with the `smoke-2.jsonl`-derived fixture already committed, assert ≥1 strip image and `.selfJoined` in transcript.
- [ ] **Step 5: Full suite + commit.** `git commit -m "macos: Plan 4a Task 9 - ChatSessionModel + SettingsStore + FixtureReplayServer"`.

### Task 10: Xcode app skeleton (`macos/ComicChat`) — boots to a connect sheet

**Files:**
- Create: `macos/ComicChat/ComicChat.xcodeproj/project.pbxproj`
- Create: `macos/ComicChat/ComicChat/ComicChatApp.swift`, `ConnectSheet.swift`, `AppState.swift`
- Create: `macos/ComicChat/ComicChat/Info.plist`, `ComicChat.entitlements`
- Create: `macos/ComicChat/ComicChat/copy-comicart.sh` (build phase)

**Interfaces:**
- Consumes: `SettingsStore`, `ChatConfig` (Task 9). Produces: a launchable `ComicChat.app`; `AppState` (`@Observable`, owns optional `ChatSessionModel`), consumed by Tasks 11–12.

- [ ] **Step 1: The project.** Write a minimal Xcode-16 `project.pbxproj` using **file-system-synchronized groups** (objectVersion 77, `PBXFileSystemSynchronizedRootGroup` for the `ComicChat/` folder) with: one app target `ComicChat` (macOS 14.0 deployment, SwiftUI lifecycle, `GENERATE_INFOPLIST_FILE = NO` + the explicit Info.plist, code signing = local dev/ad-hoc `CODE_SIGN_IDENTITY="-"`), a `XCLocalSwiftPackageReference` to `../ComicChatKit` with product dependency `ComicChatKit`, and a run-script build phase invoking `copy-comicart.sh` (rsync `$SRCROOT/../../v2.5-beta-1-modern/comicart/` + the 6 unique `artpack1` avatars + 2 bgb — list in the script — into `$BUILT_PRODUCTS_DIR/$UNLOCALIZED_RESOURCES_FOLDER_PATH/comicart`; the read-only tree is a build-time SOURCE, never a runtime reference). Entitlements: `com.apple.security.network.client` only if sandboxing; **decision: NOT sandboxed** (personal project; simplifies 4b sounds/save) — record it. If hand-authoring the pbxproj fights back (agent can't open Xcode), the implementer escalates ONCE with the exact error — fallback ruling: generate via `swift package init` + XcodeBuildMCP scaffolding after Tim enables the macOS workflow (D1 R4), not hand-fiddling forever.
- [ ] **Step 2: The app code.**

```swift
// ComicChatApp.swift
import SwiftUI
import ComicChatKit

@main
struct ComicChatApp: App {
    @State private var appState = AppState()
    var body: some Scene {
        WindowGroup("Comic Chat") {
            ContentRoot().environment(appState)
        }
        .commands { AppCommands(appState: appState) }
    }
}

// AppState.swift
import Observation
import ComicChatKit

@Observable @MainActor
public final class AppState {
    public var model: ChatSessionModel?
    public var settings = SettingsStore()
    public var showConnectSheet = true
    public var statusLine = ""
    public var members: [String] = []
    public var stripImage: CGImage?
    public var stripSizePoints: CGSize = .zero

    public func connect() async {
        let artDir = Bundle.main.resourceURL!.appendingPathComponent("comicart").path
        let cfg = ChatConfig(host: settings.server, port: UInt16(settings.port),
                             nick: settings.nick, room: settings.room,
                             encoding: settings.encoding,
                             characterName: settings.character,
                             backdropName: settings.backdrop, artDir: artDir)
        let m = ChatSessionModel(config: cfg)
        m.onStripImage = { [weak self] img, size in
            Task { @MainActor in self?.stripImage = img; self?.stripSizePoints = size } }
        m.onMembers = { [weak self] nicks in Task { @MainActor in self?.members = nicks } }
        m.onStatus = { [weak self] s in Task { @MainActor in self?.statusLine = s } }
        model = m
        do { try await m.start(); showConnectSheet = false }
        catch { statusLine = "Connect failed: \(error)" }
    }
}
```

  `ConnectSheet.swift`: a `Form` sheet bound to `settings` (Server/Port/Nickname/Room text fields + Encoding `Picker` CP-1252|UTF-8) with a Connect button calling `appState.connect()` — fields persist through `SettingsStore` on every edit (write-through computed bindings). `ContentRoot` (temporary, replaced in Task 12): a placeholder `Text(appState.statusLine)` + `.sheet(isPresented: $appState.showConnectSheet) { ConnectSheet() }`. `AppCommands`: File → New Connection ⌘N (`showConnectSheet = true`), Room → Leave/Disconnect (call `model?.shutdown()`), standard Edit/Window menus (SwiftUI defaults).
- [ ] **Step 3: Build + launch proof.** `xcodebuild -project macos/ComicChat/ComicChat.xcodeproj -scheme ComicChat -configuration Debug build` → BUILD SUCCEEDED. `open` the built app; `screencapture -x /tmp/p4-skeleton.png`; the connect sheet is visible with the comicart resources present in the bundle (`ls …/ComicChat.app/Contents/Resources/comicart | wc -l` ≥ 32). Attach screenshot to report.
- [ ] **Step 4: Commit** (including the pbxproj). `git commit -m "macos: Plan 4a Task 10 - Xcode app skeleton + connect sheet"`.

### Task 11: `ComicStripView` — the live comic NSView

D2 §5's recommendation, verbatim: layer-backed flipped NSView in an NSScrollView, cached CGImage swaps, faithful stick-to-bottom, debounced destroy-and-replay reflow.

**Files:**
- Create: `macos/ComicChat/ComicChat/ComicStripView.swift`

**Interfaces:**
- Consumes: `AppState.stripImage`/`stripSizePoints` (Task 10), `ChatSessionModel.setViewport(widthPoints:scale:)` (Task 9).
- Produces: `struct ComicStripView: NSViewRepresentable` used by Task 12's window.

- [ ] **Step 1: Implement** (complete file):

```swift
import SwiftUI
import AppKit
import ComicChatKit

/// The comic strip document view: a flipped, layer-backed NSView whose layer
/// contents is the composed strip CGImage (D2 §5 option (a)). Scrolling is
/// NSScrollView's; stick-to-bottom replicates CPageView::m_bAtBottom
/// (pageview.cpp:355-403): auto-scroll on growth ONLY if the user was already
/// at the bottom before the growth.
final class StripDocumentView: NSView {
    override var isFlipped: Bool { true }
    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.contentsGravity = .topLeft
    }
    required init?(coder: NSCoder) { fatalError() }
    func present(image: CGImage, sizePoints: CGSize, scale: CGFloat) {
        layer?.contents = image
        layer?.contentsScale = scale
        setFrameSize(sizePoints)
    }
}

struct ComicStripView: NSViewRepresentable {
    @Environment(AppState.self) private var appState

    func makeCoordinator() -> Coordinator { Coordinator() }

    final class Coordinator: NSObject {
        let docView = StripDocumentView(frame: .zero)
        weak var scrollView: NSScrollView?
        var wasAtBottom = true
        var resizeDebounce: Timer?

        @objc func boundsDidChange(_ note: Notification) {
            guard let sv = scrollView else { return }
            // Recompute "at bottom" on every user scroll, BEFORE any growth
            // (the original re-caches m_bAtBottom in OnVScroll).
            let visible = sv.contentView.bounds
            let docH = docView.frame.height
            wasAtBottom = docH <= visible.height ||
                          visible.maxY >= docH - 2.0   // tolerance, matches AtBottom's intent
        }
        func scrollToBottom() {
            guard let sv = scrollView else { return }
            let y = max(0, docView.frame.height - sv.contentView.bounds.height)
            docView.scroll(NSPoint(x: 0, y: y))
        }
    }

    func makeNSView(context: Context) -> NSScrollView {
        let sv = NSScrollView()
        sv.hasVerticalScroller = true
        sv.documentView = context.coordinator.docView
        sv.contentView.postsBoundsChangedNotifications = true
        context.coordinator.scrollView = sv
        NotificationCenter.default.addObserver(context.coordinator,
            selector: #selector(Coordinator.boundsDidChange(_:)),
            name: NSView.boundsDidChangeNotification, object: sv.contentView)
        return sv
    }

    func updateNSView(_ sv: NSScrollView, context: Context) {
        let co = context.coordinator
        let scale = sv.window?.backingScaleFactor ?? 2.0
        if let img = appState.stripImage {
            let stick = co.wasAtBottom                    // captured BEFORE growth
            co.docView.present(image: img, sizePoints: appState.stripSizePoints, scale: scale)
            if stick { co.scrollToBottom() }
        }
        // Debounced reflow on width change (the original's posted
        // WM_AUTOFITPANELS after resize — pageview.cpp:1379-1420). Live
        // resize does NOTHING per frame; content letterboxes.
        let width = sv.contentSize.width
        co.resizeDebounce?.invalidate()
        co.resizeDebounce = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: false) {
            [weak model = appState.model] _ in
            model?.setViewport(widthPoints: width, scale: scale)
        }
    }
}
```

- [ ] **Step 2: Build.** `xcodebuild … build` → SUCCEEDED (the view compiles standalone; it renders in Task 12's window).
- [ ] **Step 3: Commit.** `git commit -m "macos: Plan 4a Task 11 - ComicStripView (layer-backed strip view + stick-to-bottom + debounced reflow)"`.

### Task 12: The MVP chat window — connect → join → live strip → send ★

**Files:**
- Create: `macos/ComicChat/ComicChat/ChatWindow.swift`
- Modify: `macos/ComicChat/ComicChat/ComicChatApp.swift` (`ContentRoot` → `ChatWindow`)
- Modify: `macos/ComicChat/ComicChat/AppState.swift` (compose + replay-demo argument)

**Interfaces:**
- Consumes: everything above. Produces: the running MVP app + the 4a exit artifact.

- [ ] **Step 1: The window.**

```swift
import SwiftUI
import ComicChatKit

struct ChatWindow: View {
    @Environment(AppState.self) private var appState
    @State private var composeText = ""

    var body: some View {
        @Bindable var state = appState
        HSplitView {
            VStack(spacing: 0) {
                ComicStripView()
                Divider()
                HStack {
                    TextField("Say something…", text: $composeText)
                        .textFieldStyle(.roundedBorder)
                        .onSubmit { sendSay() }
                    Button("Say", action: sendSay).keyboardShortcut(.defaultAction)
                }.padding(8)
                Text(appState.statusLine)
                    .font(.caption).foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 8).padding(.bottom, 4)
            }
            List(appState.members, id: \.self) { nick in Text(nick) }
                .frame(minWidth: 140, maxWidth: 220)
        }
        .frame(minWidth: 640, minHeight: 480)
        .sheet(isPresented: $state.showConnectSheet) { ConnectSheet() }
    }

    private func sendSay() {
        let text = composeText.trimmingCharacters(in: .whitespaces)
        guard !text.isEmpty, let model = appState.model else { return }
        composeText = ""
        Task { try? await model.send(text) }
    }
}
```

- [ ] **Step 2: The offline demo hook.** In `AppState`: if `ProcessInfo.processInfo.arguments` contains `--replay-fixture <path>`, `connect()` first starts a `FixtureReplayServer(fixtureURL:)` and overrides host/port/nick to `127.0.0.1:server.port` / the fixture nick ("Anonymous"), room `#comicrig`. This makes the MVP demonstrable with zero network and real 1998 bytes.
- [ ] **Step 3: Build + the ★ exit artifact.** Build; launch `ComicChat.app --replay-fixture <repo>/macos/ComicChatKit/Tests/ComicChatKitTests/Fixtures/captures/smoke-2.jsonl` (or the annotated fixture — pick whichever renders panels; `hand-authored-annotation.jsonl` guarantees a cooked-pose panel). Type a say. `screencapture -x /tmp/p4a-exit.png` → the window shows: multi-panel comic strip (peer's annotated line rendered with the cooked pose), our own say as a panel, member sidebar, status line. Copy to `.superpowers/sdd/p4a-exit.png`. **A human (Tim or the coordinator) must VIEW the screenshot** — the milestone is a viewed artifact, not a claim.
- [ ] **Step 4 (stretch, human-driven only): first live smoke.** With Tim at the keyboard (etiquette constraint — no automated traffic): `./run-rig.sh` (local ngircd + Wine 1998 client) and connect the app to `127.0.0.1:6667`, exchange a say each way; optionally repeat against `comic.dedoky.com:6667` (NICKLEN=9 — pick a short nick). Screenshot both sides if it works. NOT a gate for 4a completion; failures here become 4b findings, not 4a blockers.
- [ ] **Step 5: Commit.** `git commit -m "macos: Plan 4a Task 12 - MVP chat window (connect/join/live strip/send)"`.

---

## Exit milestone (Plan 4a)

The viewed `p4a-exit.png`: the ComicChat.app window rendering a live multi-panel comic strip from replayed real-1998-client bytes — cooked poses honored, our own say rendered, members listed — with `swift test` fully green (all Plans 1–3 suites + the new Task 1–9 suites) and the app building via `xcodebuild`. Engine additions (login, metrics, geometry, avatar, title, announce) each landed with their own selftests/loopback tests and Edit-Rules-clean review.

## Plan 4b backlog (planned after 4a lands — NOT in this plan)

Each item cites its discovery ground truth; 4b's planning session starts from these.

1. **Emotion wheel + cooked outbound poses** (D1 §1.4: `CBodyCam`, bodycam.cpp; face BMPs `res/fc_*_l.bmp`; typing-preview via `ChatPreSendText`). Replaces Task 9's `annotations: nil` scope decision.
2. **Whisper UI** — ONE tabbed window, not per-peer windows (D1 §0 correction to spec §5; whisprbx.cpp model) + think/action send modes (trivial once wheel exists).
3. **Character/backdrop pickers + Settings scene + persona** (D1 §1.9: they are Options property pages — proppage.cpp; `cc_avatar_icon_image` from Task 6 feeds thumbnails; full UserDefaults key table in D1 §5).
4. **Avatar `URLSession` download** (D4 §4: unknown-name-with-URL → fetch → validate → `setParticipantAvatar`; 2 MB cap + one retry mirrors chat.cpp:2361-2363; `# GetCharInfo` is currently swallowed eventlessly — needs an explicit defer-or-event decision).
5. **Room list + room ops** (roomlist.cpp filters; the 7 not-yet-called C fns: `cc_session_create_room/kick/invite/ban/set_mode/away` + room-props UI).
6. **Sounds** (D1 §4.2 finding: NO WAVs exist anywhere — original played from the Windows media dir; ship an empty Application Support sounds folder + `NSSound.beep()`, or Tim supplies period WAVs — **Open Q for Tim**, spec §5 amendment needed).
7. **Save/reopen JSON transcript + print/PDF/PNG export** (spec §5 deviation; the transcript is already the event log — Task 9 — so save = encode `[ProtocolEvent]`, reopen = replay through a fresh bridge; export renders through the same `Canvas`).
8. **Text view toggle** (`NSTextView` transcript from the same event log; textview.cpp reference).
9. **CTCP auto-replies** (D4 §5: cosmetic, nothing in §8 depends on them; currently R20-suppressed with NO event — wiring them needs new engine events or a Swift-side `\x01` scan; decide then, not now).
10. **Multi-room** (engine is single-`CIrcProto`; recommendation D1 R7: one-room-per-connection-window UX, defer true multi-room).
11. **Performance fast-follows** (D2: per-panel tiles via `cc_strip_compose_panel`, the DIB decode cache at the CGCanvas boundary keyed with a generation counter, panel cap) — only when panel counts/pixel sizes demand.
12. **Optional `cc_strip_hit_test_avatar`** (D2 §6d.2 — context menus/tooltips on the comic).
13. **The FULL spec-§8 live acceptance** (D4 §6's ordered checklist verbatim: Topology A local ngircd + Topology B `crypthome.com` IRCX; both directions poses/avatars/whispers; artifacts: dual JSONL captures + screenshots + `cc-dumpart --replay` reproduction; promote an escaped-byte + annotated capture into the fixture corpus). Prerequisites P1–P3 are delivered by 4a Tasks 2/6/8; P4 (ASCII-only) is delivered by Task 3's fold table making accented text safe.
14. **Re-sign the unsigned range** `d1da855..HEAD` before any merge to `main` (standing precondition; ask Tim to unlock 1Password signing).

## Self-review (against the handoff, the spec, and the four reports)

- **Handoff must-owns:** CP-1252 fold table → Task 3 ✓ (with the graduated layout relevance from D3 §4); escaped-byte vector → Task 3 ✓ (hand-authored now, real-capture promotion listed in 4b item 13).
- **Handoff discovery tasks:** MVP slice (D1) → Tasks 9–12 ✓; comic view + incremental-vs-recompose (D2) → Task 11 + measured verdict adopted (full recompose; tiles deferred) ✓; metrics decision (D3) → Task 4 (switch early, two-mode) ✓; interop scoping (D4) → Tasks 2/8 prerequisites + 4b item 13 ✓.
- **Handoff standard scope routed:** avatar announce/download → Task 8 + 4b item 4; 7 outbound C fns → 4b item 5; title/starring → Task 7; icon accessor → Task 6; save/print → 4b item 7; settings → Task 9 (MVP keys) + 4b item 3; sounds → 4b item 6 (with the no-WAVs spec finding); multi-room → 4b item 10; CTCP → 4b item 9.
- **Spec §5 deviations surfaced, not buried:** whisper one-window correction (4b item 2), sounds-bundling impossibility (4b item 6), sandbox-off decision (Task 10) — all flagged for Tim at plan review.
- **Type consistency check:** `performOnEngineQueue`/`enqueueEngineWork` (T1) used in T9/T11-via-model; `cc_session_login` + config fields (T2) used by T9's `start()` flow implicitly via auto-login; `CTMetricsCanvas` (T4) installed in T9 Step 3; `PanelFit.columns/unitPanelTwips` (T5) called in T9's `setViewport`; `setParticipantAvatar` (T6) called by bridge + 4b item 4; `setTitle`/`setSelf` (T7) called in T9 init (self) — note: `setTitle` is exposed but the MVP window sets no title string yet (title panel appears only when 4b wires a title source; `set_self` alone renders nothing per the AddStars early-return, which is the documented-valid state); `announceAvatar` (T8) called in T9's `.selfJoined` handler; `ChatConfig`/`ChatSessionModel`/`SettingsStore`/`FixtureReplayServer` (T9) consumed by T10/T12 exactly as declared.
- **Placeholder scan:** every code step shows code; the two deliberate read-first steps (T2 S1, T8 S1) specify exactly what to find and the escalation if it's absent — discovery steps, not TODOs.
- **Risk honesty:** T1 S4 and T10 S1 carry explicit STOP/escalate outcomes (interleave unsafe; pbxproj unbuildable) — the two places discovery said reality might refuse.

