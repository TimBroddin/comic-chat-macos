# macOS Port — Plan 4b: The Full App + spec-§8 Live Acceptance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Use the **`p4b-` prefix** for all SDD briefs/reports.

**Goal:** Finish the app — emotion wheel + cooked outbound poses, whisper UI, pickers/Settings/persona, avatar auto-download, room list/ops, sounds, save/reopen/export/print, text view — then run the **spec-§8 live acceptance** (Topology A local rig + Topology B `crypthome.com` IRCX, both directions, human-driven), the project's final milestone.

**Architecture:** Everything sits on the finished 4a stack: `ChatSessionModel` (one engine queue owns every `cc_*` call; the event log is the canonical transcript; reflow = destroy strip + replay) consumed by the SwiftUI app through the `ComicStripView` parameter-passing contract. 4b adds ONE small engine surface (the emotion-wheel APIs over the already-lifted `bodycam.cpp`/`textpose.cpp`/`EmotionToBytes` machinery) — everything else is Swift on existing C API. Discovery ground truth: the four reports in `docs/superpowers/plans/2026-07-18-plan4-discovery/` (D1 `app-skeleton-mvp.md`, D2 `comic-view-rendering.md`, D3 `layout-metrics.md` [background only], D4 `live-interop.md`). Where this plan and a report disagree, **stop and re-read the report**.

**Tech Stack:** Existing SwiftPM package `macos/ComicChatKit` (C++ target `cchat_engine`, Swift target `ComicChatKit`, Swift Testing) + Xcode app `macos/ComicChat` (SwiftUI, synchronized groups — new .swift files join the target automatically). AVFoundation for sounds. No new third-party dependencies.

## Global Constraints

- **Edit Rules R1–R21 + all amendments are carried forward verbatim and binding** (incl. Plan 4a's Task 2 amendments — the R18 second-800 site, the fresh `cc_session_login`, the **`g_session` SAVE/RESTORE rule for every reentrant `cc_session_*`** — and the Task 12 fonts.cpp R13-clarification instance). Any *new* pattern in lifted code → escalate (BLOCKED/NEEDS_CONTEXT); rulings become amendments **R22+**. Never improvise.
- `v2.5-beta-1-modern/` and `artifacts*/` are **read-only reference — never edit**. Copy (build-phase copy for resources), never reference at runtime.
- **Engine threading/lifetime rules (all bit people in 4a; all documented in code):** ALL `cc_*` calls on ONE serial queue (the model's `engineQueue`); **never call `cc_strip_*` from inside the `on_event` callback stack**; `performOnEngineQueue` traps on-queue misuse; metrics canvas installed BEFORE `cc_strip_create` and kept alive (`metricsCanvasBox` retained); `cc_session_config` extensions are **append-only**; `session.room(_:)` does its own `sync` — never call it on-queue (use the detached-Task pattern); strip geometry set on FRESH strips only; **ONE STRIP AT A TIME process-wide** (comicchat.h's binding rule — this constrains Task 10's viewer, Task 4's whisper rendering, and dictates Task 7's multi-room strip-swap design; see those tasks).
- **`ComicStripView` parameter-passing contract (binding):** construct `ComicStripView(image:sizePoints:model:)` from observed reads in the parent `body`. Never revert to `@Environment` reads inside it.
- New engine-global-state test suites nest inside the `.serialized` ancestor `EngineGlobalStateSelfTests` (see `StripTests.swift:15-24`). App-side logic lives in ComicChatKit so `swift test` from `macos/ComicChatKit/` stays the headless truth. SourceKit "No such module: cchat_engine / Testing" diagnostics are FALSE; the command line is the truth.
- **Real-metrics rule (D3):** never freeze byte-exact goldens under real CoreText metrics — real-metrics tests use tolerances/invariants only; byte-exact goldens stay on the fake-metrics `RecordingCanvas` forever.
- Wire text is bytes end-to-end (CP-1252 default, UTF-8 per-connection via `WireEncoding`); keep new test vectors ASCII except where Task 1 deliberately exercises accented outbound text.
- Commits are SSH-signed via 1Password; if `ssh-add -l` shows no identities, **ask Tim to unlock — never bypass signing without explicit authorization**; when authorized, `git commit --no-gpg-sign` and say so in the record. (Standing: the unsigned range `d1da855..HEAD`-partial must be re-signed before any merge to `main` — see Merge Preconditions.)
- **Public chat servers (`crypthome.com`, `comic.dedoky.com`, `koach.com`) are small community boxes: no scripted/automated traffic, human-driven sessions only, scratch rooms, introduce the project.** Nothing in 4b's automated tests touches them. (Re-probed 2026-07-18 during 4b planning: crypthome still answers `800 * 0 0 ANON 512 *`, dedoky still answers `451` — both alive.)
- `swift build`/`swift test` run from `macos/ComicChatKit/`; the app builds with `xcodebuild -project macos/ComicChat/ComicChat.xcodeproj -scheme ComicChat build` (XcodeBuildMCP macOS workflows NOT enabled — use Bash). `screencapture` needs `-D 1` on this machine. Launch with args via `ComicChat.app/Contents/MacOS/ComicChat --replay-fixture <path> &`.
- The §8 acceptance is **HUMAN-DRIVEN — Tim at the keyboard** (synthetic keystrokes need Accessibility grants the agent doesn't have; matches spec §8's manual framing). The agent runs rigs/captures/screenshots; Tim types.

---

## File structure

```
macos/ComicChatKit/
  Sources/cchat-engine/
    include/comicchat.h          MODIFY  + cc_strip_set_self_emotion, cc_strip_preview_self_text,
                                          cc_strip_self_pose, cc_strip_self_annotations (Task 2)
    bridge/cc_compose.cpp        MODIFY  the four self-pose entry points (Task 2)
    bridge/cc_selftest.cpp       MODIFY  selftest for the Task 2 additions
  Sources/ComicChatKit/
    ProtocolSession.swift        MODIFY  encoded-text alignment say/whisper/setTopic (Task 1);
                                          userName/realName -> own_user/own_realname (Task 5);
                                          room-scoped ScopedEvent stream (Task 7);
                                          room-ops wrappers createRoom/kick/invite/ban/
                                          setRoomMode/setAway (Task 8)
    ChatSessionModel.swift       MODIFY  echo dedup + members ordering (Task 1); wheel state,
                                          send(text:mode:) + cooked echo, typing preview (Task 3);
                                          whisper routing (Task 4); character/backdrop switching
                                          (Task 5); avatar download hook (Task 6); multi-room
                                          per-room state + active-strip swap (Task 7); room list
                                          accumulation (Task 8); sound events (Task 9)
    SettingsStore.swift          MODIFY  + realName, sendComicsData, acceptWhispers, soundsEnabled,
                                          comicMode, autoDownloadAvatars, soundsFolder (Task 5/8)
    Strip.swift                  MODIFY  setSelfEmotion/previewSelfText/selfPoseIndex/
                                          selfAnnotations wrappers (Task 2)
    ProtocolStripBridge.swift    MODIFY  AvatarResolver extraDirs (App Support Characters) (Task 6)
    AvatarDownloader.swift       CREATE  URLSession fetch, 2MB cap, one retry, validate (Task 6)
    SoundLibrary.swift           CREATE  name->URL resolution over the user sounds folder (Task 9)
    ConversationFile.swift       CREATE  Codable transcript envelope + read/write (Task 10)
    TranscriptRenderer.swift     CREATE  offline ConversationFile -> CGImage replay (Task 10)
    TranscriptTextBuilder.swift  CREATE  [ProtocolEvent] -> AttributedString (Task 11)
  Tests/ComicChatKitTests/       MODIFY/CREATE per task (engine-state suites in the .serialized tree)
macos/ComicChat/ComicChat/
    BodyCamView.swift            CREATE  the emotion wheel control (Task 3)
    ComposeBar.swift             CREATE  extracted compose bar: field + mode picker + wheel (Task 3)
    WhisperBox.swift             CREATE  one tabbed whisper window (Task 4)
    SettingsScene.swift          CREATE  Settings tabs: Persona/Character/Backdrop/Sounds/Advanced (Task 5)
    CharacterPickerView.swift    CREATE  icon-thumbnail grid (Task 5)
    BackdropPickerView.swift     CREATE  backdrop-thumbnail grid (Task 5)
    RoomTabBar.swift             CREATE  room tabs + unread badges + join/close (Task 7)
    RoomListWindow.swift         CREATE  LIST results + filters + Go To (Task 8)
    TranscriptTextView.swift     CREATE  NSTextView-backed text toggle (Task 11)
    ChatWindow.swift             MODIFY  compose-bar swap, view toggle, member context menu
    AppState.swift               MODIFY  per-task wiring (wheel image, whispers, sounds, save/open)
    AppCommands.swift            MODIFY  File Save/Open/Export/Print, View toggle, Room/Member menus
    ComicStripView.swift         MODIFY  [weak co] debounce fix (Task 1)
    res/wheel/fc_*_l.bmp         CREATE  build-phase copy of the 8 face BMPs (Task 3)
.superpowers/rig/
    probe-servers.sh             CREATE  2-line nc reachability probes (Task 12)
    run-topology-a.sh            CREATE  local ngircd + dual capture proxies (Task 12)
    run-topology-b.sh            CREATE  dual proxies -> crypthome.com (Task 12)
docs/superpowers/plans/
    2026-07-18-plan-4b-acceptance-runbook.md  CREATE  the §8 checklist runbook (Task 12)
```

**Explicitly NOT in 4b (recorded decisions — reviewed and approved by Tim 2026-07-18):**
- **CTCP auto-replies** — DEFER (D4 §5 verdict: cosmetic, nothing in §8 depends on them; inbound probes are R20-suppressed with no event, so wiring them needs new engine events; the acceptance runbook documents the expected silence instead).
- **Custom-avatar publishing** (`# GetCharInfo` answering + HTTP hosting) — DEFER (D4 §4b: `# GetCharInfo` is swallowed eventlessly in the engine; bundled-name announce satisfies §8's avatar interop; we fetch THEIR custom avatars one-way in Task 6). This is the explicit defer-or-event decision the carryover demanded: **defer, no new event.**
- **`cc_strip_hit_test_avatar`** — DEFER (D2 §6d.2: optional; the member list covers the same actions).
- **Per-panel tiles / DIB decode cache / panel cap** — DEFER until panel counts demand (D2 §4.3 verdict).
- **User-editable comic titles** — the title stays the room name (set once on a fresh strip), so `cc_strip_set_title`-after-lines stays a documented-not-guarded precondition; the runtime guard is owed WHEN titles become user-settable (carryover recorded, not triggered by 4b).
- **Outbound sound send** (`#SOUND` composition) — DEFER (D4 §6 step 9: not an acceptance gate; inbound playback is Task 8).
- **RTF message formatting, text-font/color dialogs, server directory, macros/automation, favorites** — defer beyond Plan 4 (D1 §1.11-1.13, §6).

---

## Architecture reference (cited by all tasks — the EXACT existing API)

- **`ChatSessionModel`**: `init(config: ChatConfig)`, `start() async throws`, `send(_ text: String) async throws` (Task 3 extends), `setViewport(widthPoints:scale:)`, `shutdown()`, `transcript: [ProtocolEvent]`, callbacks `onStripImage: ((CGImage, CGSize) -> Void)?`, `onMembers: (([String]) -> Void)?`, `onStatus: ((String) -> Void)?`. Internals every task must respect: `engineQueue` private serial queue; `handleLocked(_:)` is ENGINE QUEUE ONLY; `enqueueHandle(_:)` is the single event funnel; `emitMembers()` uses the detached-Task pattern (never `session.room` on-queue); `reflowLocked()` = destroy strip → `setUpStripLocked()` → re-apply `_transcript`.
- **`ChatConfig`**: `host, port, nick, room, encoding, characterName, backdropName, artDir` (Task 5 appends `userName`, `realName`).
- **`ProtocolSession`**: `init(host:port:nick:encoding:engineQueue:)` (Task 5 appends identity params), `connect()`, `disconnect()`, `join(_:key:)`, `part(_:reason:)`, `changeNick(_:)`, `setTopic(_:topic:)`, `say(_ channel: String, text: String, annotations: Annotations? = nil, modes: UInt16 = UInt16(CC_MODE_SAY))`, `whisper(to nicks: [String], text: String, channel: String, annotations: Annotations? = nil)`, `announceAvatar(channel:toNick:name:url:)`, `who(_:)`, `list(_:)`, `events: AsyncStream<ProtocolEvent>`, `ownNick`, `room(_ channel:) -> RoomState?` (`RoomState.members: [String: RoomMember]`, `RoomMember.avatarName/avatarURL/isOp/...`), `performOnEngineQueue`, `enqueueEngineWork`, `probeTimeoutMs`. Private helpers `withEncodedCString`/`withOptionalEncodedCString` exist at ProtocolSession.swift:577-589 (Task 1 reuses them).
- **`Strip`**: `init()`, `close()`, `addParticipant(nick:avbPath:) -> Int32`, `setParticipantAvatar(_:avbPath:)`, `setBackdrop(_:)`, `setTitle(_:)`, `setSelf(_:)`, `addLine(speaker:text:modes:addressees:)`, `addLineCooked(speaker:text:modes:addressees:annotations:encoding:)`, `panelGeometry`, `setPanelGeometry(unitTwips:panelsPerRow:)`, `panelCount`, `size`, `compose(onto:)`, `Strip.Mode` = `.say/.whisper/.think/.action`.
- **`Annotations`** (ProtocolEvents.swift:8): `gesturePose/gestureEmotion/gestureIntensity/facePose/faceEmotion/faceIntensity: Int32`, `requested: Bool`, `mode: Int32` (SM_* 1..5), `addressees: [String]`, `cooked: Bool`; `toCAnnotations(encoding:)`/`init(cAnnotations:encoding:)`.
- **`ProtocolEvent`** cases 4b consumes: `.whisper(nick:ident:text:annotations:)`, `.sound(nick:file:text:)`, `.appearsAs(nick:avatarName:url:)`, `.roomListBegin/Item/End`, `.text(nick:ident:target:text:kind:annotations:)`, `.userJoined/...`, `.selfJoined(channel:)`.
- **Art**: `AvatarFile(path:)` → `.name`, `.poseCount`, `.poseName(_:)`, `.poseImage(_:) -> ArtImage`, `.iconImage() -> ArtImage`; `BackdropFile(path:)` → `.name`, `.image()`; `ArtImage.cgImage` (ArtImage+CGImage.swift); `buildCatalog(artDir:) -> [CatalogEntry]` (`file/kind/name/poseCount`); `ProtocolStripBridge.AvatarResolver(comicartDir:)`.
- **Canvas**: `CGCanvas(widthTwips:heightTwips:scale:)`, `.makeCGImage()`, `.pngData()`.
- **`SettingsStore`** existing keys: `server/port/room/encoding/nick/character/backdrop` (defaults "", 6667, "", cp1252, "", "anna", "field").
- **C surface** (comicchat.h): everything above plus the **6 not-yet-Swift-wrapped room ops** `cc_session_create_room/kick/invite/ban/set_mode/away` + `cc_session_set_topic` (wrapped), `cc_session_send_say(s, token, ann, text, modes)` (modes = CC_MODE_*), `cc_avatar_open/close/pose_image/icon_image`, `cc_image_free`.
- **App**: `AppState` (`model/settings/showConnectSheet/statusLine/members/stripImage/stripSizePoints`, `connect()`, `disconnect()`), `ChatWindow` (HSplitView: strip + compose + status | member List), `AppCommands`, `ConnectSheet` (write-through `settingsBinding`), `ComicStripView(image:sizePoints:model:)`.
- **Fixtures/rigs**: `FixtureReplayServer(fixtureURL:)` (one connection, sends all s2c on connect), `LoopbackIRCServer` (Tests — records c2s bytes; see `LoginSequencingTests` for the accessor pattern), the Wine rig `.superpowers/rig/run-rig.sh` + `capture-proxy.ts <listen> <host> <port> [log]`.

---

### Task 1: Outbound wire-encoding alignment + polish batch (§8 prerequisite + 4a carryovers)

The named §8 PREREQUISITE from the 4a final review, plus the four small triaged carryovers, in one reviewable batch. No new features.

**Files:**
- Modify: `macos/ComicChatKit/Sources/ComicChatKit/ProtocolSession.swift` (say :520-541, whisper :591-611, setTopic :510-518, helper doc :572-583)
- Modify: `macos/ComicChatKit/Sources/ComicChatKit/ChatSessionModel.swift` (send :384-390, handleLocked :264-320, emitMembers :351-359)
- Modify: `macos/ComicChat/ComicChat/ComicStripView.swift` (debounce timer closure)
- Modify: `macos/ComicChat/ComicChat/AppState.swift` (replay-fixture failure path :59-62)
- Create: `macos/ComicChatKit/Tests/ComicChatKitTests/OutboundEncodingTests.swift`
- Modify: `macos/ComicChatKit/Tests/ComicChatKitTests/ChatSessionModelTests.swift` (echo-dedup + members-ordering tests)

**Interfaces:**
- Consumes: the private `withEncodedCString`/`withOptionalEncodedCString` helpers (ProtocolSession.swift:577-589), `LoopbackIRCServer`'s received-bytes accessor (see `LoginSequencingTests`).
- Produces: no public API change. `say`/`whisper`/`setTopic` text now encodes per session `encoding`. `ChatSessionModel.handleLocked` gains a `fromServer: Bool` parameter (internal). Every later task inherits correct CP-1252 outbound text.

- [ ] **Step 1: Write the failing encoding test.** `OutboundEncodingTests.swift`, in the serialized tree, using `LoopbackIRCServer` exactly as `LoginSequencingTests` does (connect → drive login → join → then exercise the call under test and read the server's received-bytes log):

```swift
import Testing
import Foundation
@testable import ComicChatKit

@Suite("OutboundEncoding", .serialized) struct OutboundEncodingTests {
    // say(): "café" over a .cp1252 session must hit the wire as 0xE9, not UTF-8 0xC3 0xA9.
    @Test func sayEncodesCP1252() async throws {
        // setup mirrors LoginSequencingTests: LoopbackIRCServer, session
        // .cp1252, connect, server sends 451 -> NICK/USER -> 001, join, 332/353…
        // (copy the established scaffold from that suite verbatim)
        // …then:
        try await session.say("#t", text: "café", annotations: nil)
        let bytes = server.receivedBytes()   // the suite's existing accessor name — reuse, don't invent
        #expect(bytes.contains([0x63, 0x61, 0x66, 0xE9]))          // c a f é(CP-1252)
        #expect(!bytes.contains([0xC3, 0xA9]))                     // no UTF-8 é
    }
    @Test func whisperEncodesCP1252() async throws { /* same scaffold; whisper(to:["Bob"], text:"café", channel:"#t") ; same two #expect */ }
    @Test func topicEncodesCP1252() async throws  { /* same scaffold; setTopic("#t", topic:"café") ; same two #expect */ }
}
```

The `/* same scaffold */` bodies are written out in full in the actual test file (each is ~15 lines of the same connect/login/join preamble; the implementer copies it from `LoginSequencingTests` — the ONE thing that must not be invented is the server helper's real accessor names, read them from that file first).

- [ ] **Step 2: Run to verify it fails.** `swift test --filter OutboundEncoding` from `macos/ComicChatKit/`. Expected: FAIL — wire carries `0xC3 0xA9` (UTF-8) for all three.

- [ ] **Step 3: Fix the three call sites.** In `ProtocolSession.swift`, replace `text.withCString { … }` with `self.withEncodedCString(text) { … }` in `say` (both branches, :530 and :535), `whisper` (:599), and `setTopic` (:515). Also add to `withEncodedCString`'s doc comment the carryover truncation note: `/// NOTE: embedded NUL bytes in `s` truncate the wire string at the NUL (C-string boundary) — same behavior as the original's char* pipeline; IRC cannot carry NUL anyway (RFC 1459 §2.3.1).`

- [ ] **Step 4: Run the tests.** `swift test --filter OutboundEncoding` → PASS; full `swift test` → all green (58+3).

- [ ] **Step 5: Write the failing echo-dedup test.** In `ChatSessionModelTests.swift` (existing suite, its established scaffold): drive the model through login/join via the loopback server, `try await model.send("hello once")`, then have the server ECHO the same message back (`:<ownNick>!u@h PRIVMSG #room :hello once`), settle, and assert the transcript contains exactly ONE `.text` event with that text (today it would be two: synthetic + echo).

- [ ] **Step 6: Implement echo dedup.** In `ChatSessionModel`: add engine-queue-owned `private var pendingLocalEchoes: [String] = []`. In `send(_:)`, before enqueueing the synthetic event, `engineQueue.async { self.pendingLocalEchoes.append(text) }` — then route the synthetic through `enqueueHandle(synthetic, fromServer: false)`. Change `enqueueHandle`/`handleLocked` to carry `fromServer: Bool = true`. At the top of `handleLocked`'s `.text` case:

```swift
case .text(let nick, _, _, let text, _, _):
    // Own-say echo dedup (4a carryover): some servers echo PRIVMSG back to
    // the sender; our synthetic local echo (send(_:)) already rendered it.
    // Drop exactly one server copy per pending send. Synthetic events
    // (fromServer == false) never match — they are the render we keep.
    if fromServer, nick == session.ownNick,
       let i = pendingLocalEchoes.firstIndex(of: text) {
        pendingLocalEchoes.remove(at: i)
        return                      // not appended to _transcript either — reflow must not double-render
    }
    _transcript.append(ev); try? bridge?.apply(ev); recomposeLocked()
```

(Restructure the existing `.text, .whisper, .action` combined case accordingly — `.whisper`/`.action` keep the old path. `_transcript.append(ev)` moves INTO the cases for `.text` only if needed; simpler: keep the top-of-function append but `return` BEFORE it for the dropped echo — implementer's choice, the test pins the observable.)

- [ ] **Step 7: Write the failing members-ordering test.** In `ChatSessionModelTests.swift`: drive a burst of membership churn (server sends JOIN, PART, JOIN for several nicks back-to-back), settle, and assert the LAST `onMembers` callback delivered equals the final member set (today a slow earlier detached Task can overwrite a later one).

- [ ] **Step 8: Implement members-ordering guard.** In `ChatSessionModel`: engine-queue-owned `private var membersSeq = 0` and main-thread-owned `private var appliedMembersSeq = 0` (main-only — safe unsynchronized). In `emitMembers()` (still the detached-Task pattern — the same-queue `sync` trap is real):

```swift
private func emitMembers() {
    membersSeq += 1                              // engine queue — serialized
    let seq = membersSeq
    Task { [session, config, onMembers] in
        let members = session.room(config.room)?.members ?? [:]
        let sorted = members.values.filter { !$0.departed }.map(\.nick).sorted()
        DispatchQueue.main.async {
            guard seq > self.appliedMembersSeq else { return }   // stale snapshot — drop
            self.appliedMembersSeq = seq
            onMembers?(sorted)
        }
    }
}
```

(`self` capture becomes explicit; keep the `[weak self]`-vs-strong choice consistent with the file's existing style — strong is fine, the Task is short-lived.)

- [ ] **Step 9: The two one-liner app fixes.** (a) `ComicStripView.swift` debounce: the timer closure captures the coordinator strongly — change to `{ [weak co] _ in co?.… }` (the 4a-named `[weak co]` fix; find the `Timer.scheduledTimer` closure). (b) `AppState.connect()` replay-fixture failure (:59-62): add `showConnectSheet = true` after setting `statusLine`, so a failed `--replay-fixture` start leaves the sheet visible instead of a dismissed-sheet dead end.

- [ ] **Step 10: Run everything.** `swift test` from `macos/ComicChatKit/` → all green. `xcodebuild -project macos/ComicChat/ComicChat.xcodeproj -scheme ComicChat build` → BUILD SUCCEEDED.

- [ ] **Step 11: Commit.** `git commit -m "macos: Plan 4b Task 1 - outbound CP-1252 alignment + echo dedup + members ordering + polish"`

---

### Task 2: Emotion-wheel engine surface (`cc_strip_set_self_emotion` / preview / self-annotations)

The ONE engine addition in 4b. All the machinery is already lifted and live: `engine/bodycam.cpp` (`GetEmotionFromPoint` math stays Swift-side; `UpdateEmotion`'s `GetBodyFromEmotion`+`UpdateBody` chain is what we call), `engine/textpose.cpp:120` (`ChatPreSendText` — already called inside `cc_strip_add_line`, see bridge/cc_compose.cpp:397), `engine/avatario.cpp:74` (`EmotionToBytes`, live), `engine/protsupp.cpp:340-390` (the lifted outbound annotation builder that reads face/torso state via `EmotionToBytes`). Wire grammar ground truth: state-and-codec.md §3.3 (`#G<gp><ge><gi>E<ep><ee><ei>[R]M<m>[T…]`).

**Files:**
- Modify: `macos/ComicChatKit/Sources/cchat-engine/include/comicchat.h`
- Modify: `macos/ComicChatKit/Sources/cchat-engine/bridge/cc_compose.cpp`
- Modify: `macos/ComicChatKit/Sources/cchat-engine/bridge/cc_selftest.cpp`
- Modify: `macos/ComicChatKit/Sources/ComicChatKit/Strip.swift`
- Create: `macos/ComicChatKit/Tests/ComicChatKitTests/SelfEmotionTests.swift`

**Interfaces:**
- Consumes: `ccContext().session.selfParticipant` (R17, set by `cc_strip_set_self`), `GetAvatar(id)`, `GetBodyFromEmotion(CEmotion&)` (avatar.h:223), `UpdateBody`, `ChatPreSendText(CString&, int)` (declared bridge/cc_compose.cpp:54), `EmotionToBytes` (avatario.cpp:74), the pose-index accessor precedent from Task 4a-6 (`GetIconPose()`/`GetPoseFromID` pattern, avatar.h:251-255).
- Produces (exact C, appended to comicchat.h after `cc_strip_set_self`):

```c
/* Plan 4b Task 2: emotion-wheel surface. All operate on the SELF participant
 * (cc_strip_set_self must have been called; nonzero return otherwise).
 *
 * set_self_emotion: the wheel drag — CEmotion(angle_radians, intensity01)
 * -> GetBodyFromEmotion -> UpdateBody (the original CBodyCam::UpdateEmotion
 * chain, bodycam.cpp:418-436, minus the HWND drawing). intensity01 is
 * clamped to [0,1]; the 0.2 center detente is the CALLER's job (it is UI
 * behavior — GetEmotionFromPoint, which stays in Swift).
 *
 * preview_self_text: the typing preview — runs ChatPreSendText(text, self)
 * (textpose.cpp:120) so the self avatar's pose reflects what the text WOULD
 * infer, without adding a line. Mutates avatar pose state exactly like the
 * original's per-edit preview (saywnd.cpp:975).
 *
 * self_pose: current self pose index (for rendering the wheel's live
 * preview via cc_avatar_pose_image on a standalone handle of the same .avb).
 *
 * self_annotations: fills `out` with the outbound annotation block for the
 * CURRENT self pose/emotion state — pose indices + EmotionToBytes wire
 * emotion/intensity for both G (gesture/torso) and E (face) groups, cooked=1
 * — mirroring the original's outbound builder (the engine/protsupp.cpp
 * :340-390 lifted path). mode/addressees are NOT filled (caller's job). */
int32_t cc_strip_set_self_emotion(cc_strip* s, double angle_radians, double intensity01);
int32_t cc_strip_preview_self_text(cc_strip* s, const char* text_bytes);
int32_t cc_strip_self_pose(cc_strip* s, int32_t* out_pose_index);
int32_t cc_strip_self_annotations(cc_strip* s, cc_annotations* out);
```

- Swift wrappers on `Strip` (same file section as `setSelf`): `setSelfEmotion(angle: Double, intensity: Double) throws`, `previewSelfText(_ text: String) throws` (encodes via the strip's stored encoding — check how `addLine` encodes text and mirror it), `selfPoseIndex() throws -> Int32`, `selfAnnotations() throws -> Annotations` (via `Annotations(cAnnotations:encoding:)`).

- [ ] **Step 1: READ THE LIFTED CODE FIRST (mandatory; record findings in the report).** Read (a) `engine/bodycam.cpp` `UpdateEmotion` (:418-436) — confirm the `GetBodyFromEmotion`+`UpdateBody` pair is callable without a window; (b) `engine/textpose.cpp:120` `ChatPreSendText` — confirm signature `(CString&, int avID)` and that it only touches avatar pose state; (c) `engine/protsupp.cpp:340-390` — identify the EXACT lifted function that builds outbound annotations from avatar state (it calls `EmotionToBytes(face,…)`/`EmotionToBytes(torso,…)` at :369-370) and how it obtains pose indices + face/torso emotions from the avatar; (d) `avatar.h` — the accessor for "current pose index" (the `GetIconPose`/`GetPoseFromID` family, and what `UpdateBody` sets). (e) Confirm the SM_*-vs-CC_MODE_* mode mapping used by `ccEncodeAnnotations` (bridge/cc_session.cpp:339-382) so Task 3's Swift knows what `Annotations.mode` value each send mode carries. If the :340-390 builder is NOT reachable from bridge code (static, or entangled with CRoomInfo), STOP → NEEDS_CONTEXT with the exact entanglement — do NOT hand-build index math without a ruling (the fallback ruling will likely be an ccEncodeAnnotations-style bridge mirror, the established R16 precedent, but that is the reviewer's call).

- [ ] **Step 2: Write the failing C selftest.** In `cc_selftest.cpp`, `cc_run_self_emotion_selftest(const char* avatar_path)` (fixture-path pattern, like `cc_run_avatar_api_selftest`): create strip → add participant from `avatar_path` → `set_self` → assert (1) `cc_strip_self_pose` returns a valid index (>= 0, < pose count); (2) `cc_strip_set_self_emotion(s, 0.0, 1.0)` then `self_annotations` yields `cooked == 1` and plausible nonzero emotion/intensity wire values; (3) `cc_strip_preview_self_text(s, <a trigger phrase the textpose rules match — reuse the exact phrase the existing ChatPreSendText selftest at cc_selftest.cpp:2217-2284 uses>)` changes `self_pose`'s result vs the neutral baseline; (4) all four return nonzero on a strip with NO self set. Declare in comicchat.h next to the other fixture-path selftests. Expected: fails to link (functions undeclared).

- [ ] **Step 3: Implement the four functions in `cc_compose.cpp`.** Per Step 1's findings. Shape (subject to Step 1; deviations reported):

```cpp
int32_t cc_strip_set_self_emotion(cc_strip* s, double angle, double intensity01) {
    if (!s) return 1;
    int32_t self = ccContext().session.selfParticipant;   // R17 (exact accessor per Task 7's code)
    if (self <= 0) return 1;
    CAvatarX* av = GetAvatar(self);
    if (!av) return 1;
    if (intensity01 < 0) intensity01 = 0; if (intensity01 > 1) intensity01 = 1;
    CEmotion emo((float)angle, (float)intensity01);
    av->UpdateBody(av->GetBodyFromEmotion(emo));          // bodycam.cpp:436's exact chain
    return 0;
}
```

`preview_self_text`: build a shim `CString` from `text_bytes`, call `ChatPreSendText(str, self)`. `self_pose`: the Step-1-found accessor on the avatar's current body/pose. `self_annotations`: fill a `cc_annotations` from the avatar's current face/torso pose indices + `EmotionToBytes` outputs (or call the :340-390 lifted builder if Step 1 found it directly callable), set `cooked = 1`, zero `mode`/`addressee_count`.

- [ ] **Step 4: Selftest green.** `swift test --filter Engine` (the C selftest is driven by the existing Swift-side selftest runner pattern — register alongside `cc_run_avatar_api_selftest`'s Swift caller). Expected: PASS.

- [ ] **Step 5: Write the failing Swift tests.** `SelfEmotionTests.swift` in the serialized tree: (1) `Strip` wrapper round-trip — `setSelfEmotion(angle: 0, intensity: 1)` then `selfAnnotations()` has `cooked == true` and `faceEmotion/faceIntensity` nonzero; (2) `previewSelfText` changes `selfPoseIndex()`; (3) a cooked `addLineCooked` with those annotations composes without error (smoke: `panelCount` grows). Use the armando.avb fixture path pattern from `StripTests`.

- [ ] **Step 6: Implement the Swift wrappers.** In `Strip.swift`, mirroring `setSelf`'s error style. Run `swift test --filter SelfEmotion` → PASS. Full `swift test` → green.

- [ ] **Step 7: Commit.** `git commit -m "macos: Plan 4b Task 2 - emotion-wheel engine surface (self emotion/preview/pose/annotations)"`

---

### Task 3: The emotion wheel UI + send modes + cooked outbound poses

Replaces Task 4a-9's `annotations: nil` scope decision. The wheel is the signature control (D1 §1.4): bulls-eye + 8 face icons + live pose preview; drag = emotion (direction) × intensity (distance); typing previews the inferred gesture. Face icon BMPs verified this session: plain on-disk 8-bit BMPs (`res/fc_*_l.bmp`, 20×26) that macOS decodes natively (sips-verified) — build-phase copy, `NSImage(contentsOfFile:)`, drawn opaque (the original draws them with plain `CDIB::Draw`, no colorkey — bodycam.cpp:266).

**Files:**
- Modify: `macos/ComicChatKit/Sources/ComicChatKit/ChatSessionModel.swift`
- Create: `macos/ComicChat/ComicChat/BodyCamView.swift`
- Create: `macos/ComicChat/ComicChat/ComposeBar.swift`
- Modify: `macos/ComicChat/ComicChat/ChatWindow.swift`, `AppState.swift`
- Modify: `macos/ComicChat/ComicChat.xcodeproj` build phase (copy the 8 `fc_*_l.bmp` — extend the existing `alwaysOutOfDate` comicart copy script to also copy `v2.5-beta-1-modern/res/fc_{hap,coy,bor,sca,sad,ang,sho,laf}_l.bmp` into `Resources/wheel/`)
- Create: `macos/ComicChatKit/Tests/ComicChatKitTests/CookedSendTests.swift`

**Interfaces:**
- Consumes: Task 2's `Strip.setSelfEmotion/previewSelfText/selfPoseIndex/selfAnnotations`, `AvatarFile.poseImage(_:)` + `ArtImage.cgImage`, `ProtocolSession.say(_:text:annotations:modes:)`, `Strip.Mode`.
- Produces (later tasks rely on these exact names):
  - `ChatSessionModel.send(_ text: String, mode: Strip.Mode = .say) async throws` (replaces the old single-param `send`; annotations now built from wheel state; honors `sendComicsData` once Task 5 adds it — until then always annotated),
  - `ChatSessionModel.setEmotion(angle: Double, intensity: Double)` (fire-and-forget, engine-queue),
  - `ChatSessionModel.previewTyping(_ text: String)` (fire-and-forget, engine-queue),
  - `ChatSessionModel.onSelfPose: (@Sendable (CGImage?) -> Void)?` — fired after every emotion/preview/character change with the freshly rendered pose image (the model owns a standalone `AvatarFile` of the self character and renders `poseImage(selfPoseIndex())` on the engine queue),
  - `AppState.selfPoseImage: CGImage?`.
  - Wheel geometry constants (BodyCamView): icon order `[happy, coy, bored, scared, sad, angry, shout, laugh]` = files `fc_hap/coy/bor/sca/sad/ang/sho/laf_l.bmp` (bodycam.cpp:49-59 `lg_icons`), icon i at angle `2π·i/8` (y-up; happy = east), `GetEmotionFromPoint` math: `intensity = min(|p−center|/bullRadius, 1)`, detente `< 0.2 → 0`, `emotion = atan2(vec)` (bodycam.cpp:398-408).

- [ ] **Step 1: Write the failing cooked-send test.** `CookedSendTests.swift` (serialized): loopback scaffold → login/join → `model.setEmotion(angle: 0, intensity: 1.0)` → settle (engine queue drain via a sentinel `enqueueEngineWork` hop) → `try await model.send("posed line", mode: .say)` → assert the server's received bytes contain an annotation block (`(#G` prefix on plain IRC) AND the local transcript's synthetic `.text` event carries `annotations != nil` with `cooked == true`. Second test: `send("thought", mode: .think)` → wire bytes carry the think-mode annotation `M<x>` value found in Task 2 Step 1(e), and the strip renders (panelCount grows) with a think balloon (smoke-level: no crash, count grows).

- [ ] **Step 2: Run to verify it fails** (no `mode:` param, no `setEmotion`). `swift test --filter CookedSend` → compile FAIL.

- [ ] **Step 3: Implement the model side.**

```swift
// ChatSessionModel — engine-queue-owned wheel state
private var selfAvatarFile: AvatarFile?     // standalone handle of config character (lazy, engine queue)
public var onSelfPose: (@Sendable (CGImage?) -> Void)?

public func setEmotion(angle: Double, intensity: Double) {
    engineQueue.async { [weak self] in
        guard let self, !self.isShutDown, let strip = self.strip else { return }
        try? strip.setSelfEmotion(angle: angle, intensity: intensity)
        self.emitSelfPoseLocked()
    }
}
public func previewTyping(_ text: String) {
    engineQueue.async { [weak self] in
        guard let self, !self.isShutDown, let strip = self.strip else { return }
        try? strip.previewSelfText(text)
        self.emitSelfPoseLocked()
    }
}
private func emitSelfPoseLocked() {   // ENGINE QUEUE ONLY
    guard let strip else { return }
    if selfAvatarFile == nil {
        selfAvatarFile = try? AvatarFile(path: config.artDir + "/" + config.characterName + ".avb")
    }
    var image: CGImage? = nil
    if let idx = try? strip.selfPoseIndex(), let av = selfAvatarFile,
       let art = try? av.poseImage(Int(idx)) { image = art.cgImage }
    DispatchQueue.main.async { [onSelfPose] in onSelfPose?(image) }
}

public func send(_ text: String, mode: Strip.Mode = .say) async throws {
    // Build cooked annotations from the CURRENT wheel/preview state on the
    // engine queue (the original grabs the bodycam state at send time too).
    let ann: Annotations? = session.performOnEngineQueue {
        guard var a = try? strip?.selfAnnotations() else { return nil }
        a.mode = Self.smMode(for: mode)      // SM_* value per Task 2 Step 1(e)'s mapping
        return a
    }
    try await session.say(config.room, text: text, annotations: ann,
                          modes: UInt16(mode.rawValue))
    engineQueue.async { self.pendingLocalEchoes.append(text) }
    let synthetic = ProtocolEvent.text(nick: session.ownNick, ident: "", target: config.room,
                                       text: text, kind: 0, annotations: ann)
    enqueueHandle(synthetic, fromServer: false)
}
```

(`smMode(for:)` is a small static `Strip.Mode` → SM_* table from Task 2 Step 1(e) — write the actual values found there. NOTE `performOnEngineQueue` is on `ProtocolSession` and traps if already on-queue; `send` is called from the UI/main context so this is legal — same posture as the existing code calling `session.say`.) `reflowLocked` note: `selfAvatarFile` survives reflow (it's a standalone handle) but must reset on character change (Task 5 does that).

- [ ] **Step 4: Tests green.** `swift test --filter CookedSend` → PASS. Full suite green.

- [ ] **Step 5: Build the wheel + compose bar UI.** `BodyCamView.swift` — complete implementation:

```swift
import SwiftUI
import ComicChatKit

/// The emotion wheel (original CBodyCam, bodycam.cpp): bulls-eye + 8 face
/// icons at 2π·i/8 (y-up, happy=east; order = lg_icons, bodycam.cpp:49-59)
/// + the self avatar's live pose preview behind it. Drag inside the bull
/// radius sets emotion (direction) × intensity (distance, 0.2 detente).
struct BodyCamView: View {
    var poseImage: CGImage?
    var onEmotion: (Double, Double) -> Void       // (angleRadians, intensity01)

    private static let iconFiles = ["fc_hap_l", "fc_coy_l", "fc_bor_l", "fc_sca_l",
                                    "fc_sad_l", "fc_ang_l", "fc_sho_l", "fc_laf_l"]
    private static let icons: [NSImage] = iconFiles.compactMap {
        Bundle.main.url(forResource: $0, withExtension: "bmp", subdirectory: "wheel")
            .flatMap { NSImage(contentsOf: $0) }
    }

    var body: some View {
        GeometryReader { geo in
            let side = min(geo.size.width, geo.size.height)
            let center = CGPoint(x: geo.size.width/2, y: geo.size.height/2)
            let bullRadius = side * 0.28
            let iconOffset = bullRadius + side * 0.14
            ZStack {
                if let poseImage {
                    Image(decorative: poseImage, scale: 2).resizable()
                        .aspectRatio(contentMode: .fit).frame(width: side*0.5).opacity(0.9)
                }
                Circle().stroke(.secondary).frame(width: bullRadius*2, height: bullRadius*2)
                Circle().stroke(.secondary.opacity(0.5)).frame(width: bullRadius*0.4, height: bullRadius*0.4)
                ForEach(0..<8, id: \.self) { i in
                    let angle = 2 * .pi * Double(i) / 8
                    let pos = CGPoint(x: center.x + iconOffset * cos(angle),
                                      y: center.y - iconOffset * sin(angle))   // y-up -> AppKit y-down
                    if i < Self.icons.count {
                        Image(nsImage: Self.icons[i]).position(pos)
                    }
                }
            }
            .contentShape(Rectangle())
            .gesture(DragGesture(minimumDistance: 0).onChanged { g in
                let vx = g.location.x - center.x
                let vy = center.y - g.location.y                     // back to y-up
                var intensity = min(sqrt(vx*vx + vy*vy) / bullRadius, 1.0)
                if intensity < 0.2 { intensity = 0 }                  // the detente (bodycam.cpp:404)
                let angle = intensity == 0 ? 0 : atan2(vy, vx)        // bodycam.cpp:405
                onEmotion(angle, intensity)
            })
        }
        .frame(minWidth: 120, minHeight: 120)
    }
}
```

`ComposeBar.swift` — extract the compose row from `ChatWindow` and extend it: `TextField` (`.onChange(of: composeText) { appState.model?.previewTyping($0) }`) + `Picker("", selection: $mode)` segmented with `.say/.think/.action` (label them Say/Think/Action; whisper goes through the whisper box, Task 4) + Send button calling `model.send(text, mode: mode)`. `ChatWindow` swaps in `ComposeBar` and adds `BodyCamView(poseImage: appState.selfPoseImage, onEmotion: { a, i in appState.model?.setEmotion(angle: a, intensity: i) })` to the right column above the member list (the original's layout: members over bodycam, chatview.cpp:333-378 — put the wheel BELOW the member list to match). `AppState` gains `selfPoseImage: CGImage?` wired in `connect()`: `m.onSelfPose = { [weak self] img in Task { @MainActor in self?.selfPoseImage = img } }`.

- [ ] **Step 6: Build-phase copy of the face BMPs.** Extend the existing copy-comicart run-script phase in the pbxproj (Task 4a-10 created it, `alwaysOutOfDate`) with: `mkdir -p "${TARGET_BUILD_DIR}/${UNLOCALIZED_RESOURCES_FOLDER_PATH}/wheel"` + an `rsync`/`cp` of the 8 listed `fc_*_l.bmp` files from `v2.5-beta-1-modern/res/` (READ-only source — copy is the sanctioned pattern).

- [ ] **Step 7: Build + visual artifact.** `xcodebuild … build` → SUCCEEDED. Launch against the replay fixture (`ComicChat.app/Contents/MacOS/ComicChat --replay-fixture Tests/ComicChatKitTests/Fixtures/captures/hand-authored-annotation.jsonl &`), `screencapture -D 1` a window shot showing the wheel with icons + pose preview. **Coordinator eyeballs it** (4a discipline: every visual artifact gets coordinator eyes).

- [ ] **Step 8: Commit.** `git commit -m "macos: Plan 4b Task 3 - emotion wheel UI + think/action modes + cooked outbound poses"`

---

### Task 4: Whisper UI — ONE tabbed box (D1 §0 correction to spec §5)

The original is ONE floating dialog with a tab per correspondent (`CWhisperBox` + `CWhisperLeaf`, whisprbx.cpp:35-72), NOT spec §5's "separate small windows" — this task implements the original's model (spec amendment APPROVED by Tim at plan review). **Rendering decision (constrained by ONE-STRIP-AT-A-TIME, approved):** whisper leaves render as a TEXT transcript in 4b — a second live comic strip per whisper peer would need a second concurrent `cc_strip`, which comicchat.h forbids (use-after-free, not just a race). Room-scoped whispers ALSO keep rendering in the main strip as whisper balloons (existing 4a behavior — the model already routes `.whisper` through `bridge.apply`). (Task 7 later threads the active-room context through `sendWhisper`'s `channel:` — the box itself is room-agnostic, whispers are nick-scoped.)

**Files:**
- Modify: `macos/ComicChatKit/Sources/ComicChatKit/ChatSessionModel.swift`
- Create: `macos/ComicChat/ComicChat/WhisperBox.swift`
- Modify: `macos/ComicChat/ComicChat/AppState.swift`, `ChatWindow.swift` (member context menu), `ComicChatApp.swift` (aux Window scene), `AppCommands.swift` (Member > Whisper…)
- Create: `macos/ComicChatKit/Tests/ComicChatKitTests/WhisperRoutingTests.swift`

**Interfaces:**
- Consumes: `ProtocolSession.whisper(to:text:channel:annotations:)`, `.whisper(nick:ident:text:annotations:)` events, Task 3's `selfAnnotations` path.
- Produces:
  - `public struct WhisperLine: Sendable, Equatable { public let nick: String; public let text: String; public let isOwn: Bool }`,
  - `ChatSessionModel.whisperHistories: [String: [WhisperLine]]` (thread-safe snapshot reader, engine-queue-owned storage `_whisperHistories`),
  - `ChatSessionModel.onWhisper: (@Sendable (String, WhisperLine) -> Void)?` (peer, line — fires for inbound AND own outbound),
  - `ChatSessionModel.sendWhisper(to peer: String, text: String) async throws`,
  - `AppState.whisperPeers: [String]`, `AppState.whisperHistories: [String: [WhisperLine]]`, `AppState.showWhisperBox(peer: String?)`.

- [ ] **Step 1: Write the failing routing test.** `WhisperRoutingTests.swift` (serialized, loopback scaffold): (1) server sends `:Bob!u@h PRIVMSG <ownNick> :psst` (the whisper wire form — a PRIVMSG targeted at our nick, which the engine classifies as whisper; verify against the existing whisper fixture/tests how the loopback scaffold produces a `.whisper` event and mirror it) → assert `whisperHistories["Bob"] == [WhisperLine(nick: "Bob", text: "psst", isOwn: false)]` and `onWhisper` fired with peer "Bob". (2) `try await model.sendWhisper(to: "Bob", text: "back at you")` → assert the server received a `PRIVMSG Bob` line AND `whisperHistories["Bob"]` gained an `isOwn: true` line. (3) With Task-5's `acceptWhispers == false` (until Task 5 lands, hard-code the property default `true` and test the drop path via the internal seam): inbound whisper is dropped from histories and surfaces via `onStatus` instead. Run → FAIL.

- [ ] **Step 2: Implement the model routing.** Engine-queue-owned `private var _whisperHistories: [String: [WhisperLine]] = [:]`. In `handleLocked`'s `.whisper(let nick, _, let text, _)` case (currently folded into the `.text, .whisper, .action` bridge line — split it out): append `WhisperLine(nick: nick, text: text, isOwn: false)` to `_whisperHistories[nick]`, fire `onWhisper` via `DispatchQueue.main.async`, and KEEP the existing `bridge.apply + recomposeLocked` (main-strip whisper balloons — the 4a behavior, unchanged). `sendWhisper`:

```swift
public func sendWhisper(to peer: String, text: String) async throws {
    let ann: Annotations? = session.performOnEngineQueue {
        guard var a = try? strip?.selfAnnotations() else { return nil }
        a.mode = Self.smMode(for: .whisper)
        a.addressees = [peer]
        return a
    }
    try await session.whisper(to: [peer], text: text, channel: config.room, annotations: ann)
    engineQueue.async { [weak self] in
        guard let self, !self.isShutDown else { return }
        let line = WhisperLine(nick: self.session.ownNick, text: text, isOwn: true)
        self._whisperHistories[peer, default: []].append(line)
        let cb = self.onWhisper
        DispatchQueue.main.async { cb?(peer, line) }
    }
}
public var whisperHistories: [String: [WhisperLine]] { engineQueue.sync { _whisperHistories } }
```

- [ ] **Step 3: Tests green.** `swift test --filter WhisperRouting` → PASS; full suite green.

- [ ] **Step 4: Build the whisper box UI.** `WhisperBox.swift`: a `NavigationSplitView` (sidebar: peer list with unread badges; detail: the selected peer's `[WhisperLine]` as a text transcript — own lines right-aligned/secondary, plus a compose `TextField` + Send calling `appState.model?.sendWhisper`). `ComicChatApp.swift` adds `Window("Whispers", id: "whispers") { WhisperBox().environment(appState) }`. `AppState` gains `whisperPeers/whisperHistories` mirrors (updated from `onWhisper` on main) + `whisperUnread: [String: Int]` (cleared on tab selection) and `showWhisperBox(peer:)` (uses `@Environment(\.openWindow)` from the calling view — store the pending peer selection on AppState). `ChatWindow`'s member `List` rows gain `.contextMenu { Button("Whisper…") { … } }`; an inbound whisper for a peer with no open box shows a status line + unread badge (do NOT auto-open the window — modern-Mac behavior, deviation from the original's auto-popup noted in the report).

- [ ] **Step 5: Build + visual artifact.** `xcodebuild … build` → SUCCEEDED. Replay-fixture launch; drive an inbound whisper via a hand-extended fixture if the committed ones carry none (check first: `grep -l WHISPER Tests/ComicChatKitTests/Fixtures/captures/*.jsonl`; a PRIVMSG-to-own-nick line is also a whisper — the hand-authored fixture can gain one line, it is HAND-AUTHORED by design). Screenshot the box; coordinator eyeballs.

- [ ] **Step 6: Commit.** `git commit -m "macos: Plan 4b Task 4 - whisper box (one tabbed window) + whisper send modes"`

---

### Task 5: Settings scene + persona + character/backdrop pickers

D1 §1.9/§5: the original's Options property sheet becomes a SwiftUI `Settings` scene; the pickers are Settings tabs (they were Options property pages, NOT standalone dialogs). Persona plumbing closes a real gap: `ProtocolSession` never sets `cc_session_config.own_user/own_realname` today (falls back to nick).

**Files:**
- Modify: `macos/ComicChatKit/Sources/ComicChatKit/SettingsStore.swift`, `ProtocolSession.swift` (init + config build), `ChatSessionModel.swift` (`ChatConfig` + character/backdrop switching)
- Create: `macos/ComicChat/ComicChat/SettingsScene.swift`, `CharacterPickerView.swift`, `BackdropPickerView.swift`
- Modify: `macos/ComicChat/ComicChat/ComicChatApp.swift` (add `Settings` scene), `AppState.swift`
- Create: `macos/ComicChatKit/Tests/ComicChatKitTests/PersonaSettingsTests.swift`

**Interfaces:**
- Consumes: `buildCatalog(artDir:)`, `AvatarFile.iconImage()`, `BackdropFile.image()`, `ArtImage.cgImage`, `Strip.setParticipantAvatar`, `Strip.setBackdrop`, `ProtocolSession.announceAvatar`.
- Produces:
  - `SettingsStore` new keys (same `object(forKey:)` never-set discrimination as `port`): `realName` ("persona.realName", ""), `sendComicsData` ("protocol.sendComicsData", true), `acceptWhispers` ("protocol.acceptWhispers", true), `soundsEnabled` ("sounds.enabled", true), `soundsFolder` ("sounds.folder", default = the App Support path), `comicMode` ("view.comicMode", true), `autoDownloadAvatars` ("art.autoDownloadAvatars", true),
  - `ProtocolSession.init(host:port:nick:encoding:engineQueue:userName:realName:)` (new trailing optionals, default nil → nick fallback preserved; plumbs to `cc_session_config.own_user/.own_realname` — READ FIRST how `local_host`/nick strings are kept alive across `cc_session_create` in `onSocketReady`, mirror that exact storage pattern; if the bridge copies config strings at create, plain locals suffice — record which),
  - `ChatConfig` appended `userName: String? = nil`, `realName: String? = nil`,
  - `ChatSessionModel.changeCharacter(_ name: String)` — engine-queue: `strip.setParticipantAvatar(selfID, newPath)` (future panels only, histent.cpp:368-413 behavior), reset `selfAvatarFile`, `emitSelfPoseLocked()`, then fire-and-forget `session.announceAvatar(channel: config.room, name: name.capitalized)` (SetMyAvatar's announce-on-change, avatar.cpp:585-599) — and update the model's OWN notion `config.characterName` (make `config` a `var`),
  - `ChatSessionModel.changeBackdrop(_ name: String)` — engine-queue `bridge.setBackdrop(artDir/name.bgb)` (subsequent panels inherit — comicchat.h's set_backdrop contract; no reflow),
  - `ChatSessionModel.send` honors `sendComicsData == false` → `annotations: nil` (add `sendComicsData: Bool` to `ChatConfig`; AppState builds it from settings).

- [ ] **Step 1: Write the failing settings/persona tests.** `PersonaSettingsTests.swift`: (1) SettingsStore round-trips every new key against `UserDefaults(suiteName:)` incl. never-set defaults; (2) loopback login test (LoginSequencingTests scaffold) with `userName: "timb", realName: "Tim B"` asserting the wire `USER timb <host> . :Tim B`; (3) `changeCharacter` mid-session: after login/join + one line, switch to a second bundled character, send another line, assert no crash + `panelCount` grew + the announce (`# Appears as`) appeared in server-received bytes. Run → FAIL.

- [ ] **Step 2: Implement** per the Produces block. `SettingsScene.swift`: `TabView` with Persona (nick, real name — note "applies on next connect"), Character (`CharacterPickerView`), Backdrop (`BackdropPickerView`), Sounds (enabled toggle + folder path + "Reveal in Finder"), Advanced (send comics data, accept whispers, auto-download avatars, default encoding). Pickers: `LazyVGrid` over `buildCatalog(artDir:)` filtered by kind, thumbnail = `AvatarFile(path:).iconImage().cgImage` / `BackdropFile(path:).image().cgImage` (cache thumbnails in a `@State` dict — building 32 files is fast but do it once), selection writes `settings.character`/`settings.backdrop` AND (if `appState.model != nil`) calls `changeCharacter`/`changeBackdrop`. `ComicChatApp` adds the `Settings { SettingsScene().environment(appState) }` scene. Art dir for the pickers = the app bundle's comicart (AppState exposes it).

- [ ] **Step 3: Tests + build green.** `swift test` → green; `xcodebuild … build` → SUCCEEDED. Screenshot the Settings tabs + a live character switch (replay fixture, switch character, send — new panels show the new avatar); coordinator eyeballs.

- [ ] **Step 4: Commit.** `git commit -m "macos: Plan 4b Task 5 - Settings scene + persona plumbing + character/backdrop pickers"`

---

### Task 6: Avatar auto-download (`URLSession`)

D4 §4: unknown-name-WITH-URL announces enter the download path; 2 MB cap + one retry mirrors chat.cpp:2361-2363; validated file lands in Application Support and the participant is re-avatared. Publishing OURS stays deferred (recorded decision, header). Inherited note honored in code comment: `s->avatars` accumulates old+new `CAvatarX*` per participant on switch (bookkeeping-only today — comment at the `setParticipantAvatar` call site pointing at the engine's accumulation, guard owed if switches become frequent).

**Files:**
- Create: `macos/ComicChatKit/Sources/ComicChatKit/AvatarDownloader.swift`
- Modify: `macos/ComicChatKit/Sources/ComicChatKit/ChatSessionModel.swift`, `ProtocolStripBridge.swift` (resolver `extraDirs`)
- Create: `macos/ComicChatKit/Tests/ComicChatKitTests/AvatarDownloaderTests.swift`

**Interfaces:**
- Consumes: `.appearsAs(nick:avatarName:url:)`, `AvatarFile(path:)` (the validator — parse success == valid .avb), `Strip.setParticipantAvatar`, `ProtocolStripBridge.participantIDs`.
- Produces:
  - `public struct AvatarDownloader: Sendable { public init(session: URLSession = .shared, maxBytes: Int = 2_097_152); public func fetch(name: String, url: URL, into dir: URL) async throws -> URL }` — downloads (one retry on failure, matching chat.cpp:2361-2363), rejects > maxBytes, validates by `AvatarFile(path:)` parse on a temp file, then moves to `dir/<sanitized-name>.avb` (sanitize: strip path separators/dots from `name`),
  - `AvatarResolver` gains `public var extraDirs: [String]` searched BEFORE `comicartDir` (user characters shadow bundled ones — D1 §4.3's search order),
  - `ChatSessionModel`: `.appearsAs` handler extension — if the name resolves to no local art AND `url` is a well-formed http(s) URL AND `config.autoDownloadAvatars` (appended to `ChatConfig`, default true): detached `Task` → `downloader.fetch` → on success `engineQueue.async { bridge/strip re-avatar the nick's participant; recomposeLocked() }`; failures are SILENT status-line notes (the original's auto path is silent, chat.cpp:2242-2244),
  - App Support dir helper: `ChatSessionModel.userCharactersDir` (static, `~/Library/Application Support/Comic Chat/Characters`, created on first use).

- [ ] **Step 1: Write the failing downloader tests.** `AvatarDownloaderTests.swift` (NOT serialized — no engine state): (1) happy path with a `file://` URL pointing at the armando.avb fixture → returns a URL whose file parses as `AvatarFile`; (2) oversize: a temp file of 3 MB junk → throws; (3) junk content: 1 KB of zeros → throws (AvatarFile parse fails); (4) name sanitization: `name: "../evil"` lands INSIDE `dir`. (URLSession handles file:// — no HTTP server needed. The retry path: inject a `URLSession` with an ephemeral config pointing at a nonexistent file → throws after retry; assert via timing or a counting URLProtocol if cheap — a simple "throws" assertion is acceptable, the retry is one line.) Run → FAIL.

- [ ] **Step 2: Implement + model wiring** per Produces. In `handleLocked`'s `.appearsAs` case, AFTER the existing reply-announce + `bridge.apply`: the resolution check is `resolver`-style — reuse how `ProtocolStripBridge.AvatarResolver` decides known-vs-unknown (read its resolve logic; if it lacks a "did this name resolve to real art vs a cycled default" answer, add `AvatarResolver.resolvesName(_ name: String) -> Bool` — a pure path-existence check over `extraDirs + [comicartDir]`).
- [ ] **Step 3: Tests green; full suite green.**
- [ ] **Step 4: Commit.** `git commit -m "macos: Plan 4b Task 6 - avatar auto-download (2MB cap, one retry, App Support characters)"`

---

### Task 7: True multi-room (one connection, N rooms, one live strip) ★ scope added at plan review

**Tim's plan-review directive (2026-07-18): do true multi-room** — overriding D1 R7's defer recommendation. Design honors the engine's hard rules instead of fighting them:

- **One connection, N joined rooms.** IRC/IRCX allows it; the OUTBOUND surface is already multi-room (`cc_session_send_say(s, token, …)`, `cc_session_register_room` dense token scheme, `ProtocolSession.RoomState` is already per-channel with `room(_ channel:)`). What's missing is (a) room scope on the Swift EVENT stream and (b) a multi-room model + UI.
- **ONE live strip, owned by the ACTIVE room** (ONE-STRIP-AT-A-TIME is engine UB territory, not a preference). Every room keeps its own event-log transcript (the canonical-transcript doctrine, unchanged); activating a room = destroy the current strip → rebuild from that room's transcript → recompose — EXACTLY the proven reflow-replay machinery (`reflowLocked`), just parameterized by room. Background rooms show their last composed image (cached) + unread badges; they re-render on activation. No second strip ever exists.
- **UI: a room tab bar in the one chat window** (the original's MDI tab bar, tabbar.cpp, reborn) — tabs with unread badges; member sidebar/compose/strip all follow the active tab. Whisper box and sounds stay session-level.

**The event-scope gap (found at plan amendment):** `ProtocolEvent.from` returns `(event, roomToken)` (ProtocolEvents.swift:170) but `ProtocolSession.events: AsyncStream<ProtocolEvent>` DROPS the token — fine for one room, fatal for N. Membership/mode events (`.userJoined`, `.names`, …) carry no channel in their payloads; the token is the only scope carrier.

**Files:**
- Modify: `macos/ComicChatKit/Sources/ComicChatKit/ProtocolSession.swift` (scoped event stream), `ProtocolEvents.swift` (no shape change — the token already exists at the C boundary)
- Modify: `macos/ComicChatKit/Sources/ComicChatKit/ChatSessionModel.swift` (per-room state, active-strip ownership, join/leave/activate)
- Create: `macos/ComicChat/ComicChat/RoomTabBar.swift`
- Modify: `macos/ComicChat/ComicChat/ChatWindow.swift`, `AppState.swift`, `AppCommands.swift` (Room > Enter Room… joins an ADDITIONAL room)
- Create: `macos/ComicChatKit/Tests/ComicChatKitTests/MultiRoomTests.swift`

**Interfaces:**
- Consumes: `cc_session_room_channel(s, token)` (comicchat.h:575), the `(event, roomToken)` pair `ProtocolEvent.from` already returns, `reflowLocked`'s teardown/rebuild sequence, Task 3's send path.
- Produces:
  - `public struct ScopedEvent: Sendable { public let event: ProtocolEvent; public let channel: String? }` — `channel` nil for session-scoped events (token 0); resolved ON the engine queue at emit time via `cc_session_room_channel` + `WireCodec.decode`,
  - `ProtocolSession.events` becomes `AsyncStream<ScopedEvent>` (mechanical update of every existing consumer: `ChatSessionModel.startEventConsumer`, any test iterating `events` — grep `for await ev in`; single-room tests just use `.event`),
  - `ChatSessionModel`: `private struct RoomBox { var transcript: [ProtocolEvent] = []; var lastImage: CGImage?; var lastSizePoints: CGSize = .zero; var unread: Int = 0 }`, engine-queue-owned `private var rooms: [String: RoomBox]` + `private var activeRoom: String` (seeded from `config.room`); `public struct RoomInfo: Sendable, Equatable, Identifiable { public var id: String { name }; public let name: String; public let unread: Int; public let isActive: Bool }`; `onRoomsChanged: (@Sendable ([RoomInfo]) -> Void)?`; `joinRoom(_ room: String) async throws` (session.join; the `.selfJoined` handler creates the box + announces IN THAT CHANNEL — generalize the handler's `config.room` uses to the event's channel); `leaveRoom(_ room: String) async throws` (session.part; if it was active, activate another surviving room or clear the strip); `setActiveRoom(_ room: String)` (engine-queue: stash current image into the old box, tear down strip/bridge exactly as `reflowLocked` does, rebuild from the new room's transcript — title = NEW room name — recompose, zero its unread, fire callbacks); `send`/`sendWhisper` gain an explicit `room:`/keep `channel:` parameter (the compose bar passes its tab's room; `sendWhisper`'s annotations context uses the active room),
  - routing change in `handleLocked` (now `handleLocked(_ ev: ProtocolEvent, channel: String?, fromServer: Bool)`): channel-scoped events append to THAT room's transcript; if it's the active room → existing bridge.apply/recompose path; else → `unread += 1` (messages only) + `onRoomsChanged`. Session-scoped events (whisper/sound/status/login/appearsAs) behave as today — EXCEPT `.appearsAs`, which is channel-scoped on the wire (it rides room PRIVMSG/DATA): apply to the active strip only if its channel matches, but record the nick→avatar mapping session-wide so a later `setActiveRoom` rebuild resolves it (the transcript replay already does this — the event is IN the room's transcript),
  - `AppState.rooms: [RoomInfo]`, `AppState.activeRoom: String?`, `AppState.setActiveRoom/joinRoom/leaveRoom` passthroughs; `RoomTabBar` (a horizontal bar of tab buttons: room name + unread badge + close ×; a "+" button opening the Enter Room sheet); `ChatWindow` mounts it above the strip.
- **NOT produced (recorded):** per-room simultaneous live rendering (needs N strips — engine UB); background rooms update their comic only on activation. Reads as instant because rebuild-from-transcript is the same fast path resize already uses.

- [ ] **Step 1: READ FIRST — the engine's multi-room posture (record findings).** (a) `bridge/cc_session.h`'s token↔channel scheme comment (the "single-CIrcProto-per-session simplification" — confirm what it actually simplifies and that N registered tokens are supported); (b) grep the lifted `engine/ircproto.cpp`/`ircsock.cpp` for single-room state that would corrupt across two joined channels (a "current channel" member read by INBOUND parsing or the outbound builders we use — `cc_session_join/part/send_say/send_whisper/set_topic` all take explicit channel/token, so the suspects are parser-side); (c) confirm how `ProtocolSession` today tracks `RoomState` per channel (it does — `room(_:)`) and where the roomToken is dropped before the stream. If (b) finds load-bearing single-room engine state, STOP → NEEDS_CONTEXT with the exact member and call sites — the fallback (one `cc_session` per room sharing ONE engine queue) is a plan amendment, not an improvisation.

- [ ] **Step 2: Write the failing scoped-stream test.** In `MultiRoomTests.swift` (serialized, loopback scaffold): login → join `#a` AND `#b` (server confirms both) → server sends `:Bob!u@h JOIN #b` and a PRIVMSG to each channel → iterate `session.events` collecting `ScopedEvent`s → assert the `.userJoined` for Bob carries `channel == "#b"` and each `.text` carries its own channel. Run → compile FAIL (`events` yields bare `ProtocolEvent`).

- [ ] **Step 3: Implement the scoped stream.** In `ProtocolSession`: where `ProtocolEvent.from(ev, encoding:)` is consumed (the C `on_event` trampoline path), resolve `roomToken != 0 ? WireCodec.decode(cc_session_room_channel(s, token), encoding:) : nil` and yield `ScopedEvent(event:channel:)`. Update every consumer mechanically. Full `swift test` → green (this step touches many tests; keep the diff mechanical — `.event` unwraps).

- [ ] **Step 4: Write the failing multi-room model test.** Same suite: model start → join `#a` (initial) → `joinRoom("#b")` → server: 3 messages to `#a`, 2 to `#b` → assert: active room `#a` strip `panelCount` == title + 3; `rooms` info shows `#b` unread == 2; `setActiveRoom("#b")` → strip rebuilds (panelCount == title + 2), `#b` unread == 0, `onStripImage` fired with a fresh image; messages to now-background `#a` bump ITS unread without touching the live strip's panelCount; `leaveRoom("#b")` → active falls back to `#a` with its full 3-message strip. Run → FAIL.

- [ ] **Step 5: Implement the model** per Produces. Key mechanics: `_transcript` becomes `rooms[channel].transcript` (the SESSION-scoped events — whisper/sound/status — go into a session log kept for Task 10/11's save/text view: `private var sessionTranscript: [ProtocolEvent]`; the active room's SAVE payload in Task 10 = its room transcript; whispers are already separately stored); `reflowLocked` generalizes to `rebuildStripLocked(for room: String)` (same body, parameterized title/transcript; `setViewport`'s reflow calls it with `activeRoom`); `pendingLocalEchoes`/`announcedBackTo` stay session-level.

- [ ] **Step 6: Tests green; then UI.** `RoomTabBar` + `ChatWindow` mount + `AppCommands` "Enter Room… ⌘J" → `joinRoom`. Build → SUCCEEDED. Replay-fixture launch still works (single room, tab bar shows one tab).

- [ ] **Step 7: Visual artifact.** Two-room live proof needs a local server: `.superpowers/rig/run-rig.sh proxy` gives ngircd on 127.0.0.1:6667 — connect the app, `⌘J` join a second room, send in both, screenshot the tab switch (both strips correct). Coordinator eyeballs.

- [ ] **Step 8: Commit.** `git commit -m "macos: Plan 4b Task 7 - true multi-room (scoped events, per-room transcripts, active-strip swap, room tabs)"`

---

### Task 8: Room list + room ops + member-list upgrades

D1 §1.6 + backlog item 5: `CRoomList`'s LIST browser with client-side filters + a Go To that joins; plus the ops menu (create/kick/invite/ban/mode/away). With Task 7's multi-room landed: **Go To = `joinRoom` + `setActiveRoom`** (a new tab; no part-first — leaving rooms is the tab's close button).

Also the spec-§5 member-list items the 4a backlog skeleton didn't name (self-review catch, spec-honest): **avatar thumbnails + op badges** on the member rows, the **user-info popover** (Get Info → `whois` → `.whoisResult` popover), and **selection-as-addressee** (D1 §1.5: the member list's selection is the original's canonical talk-to state — selected nicks fill `Annotations.addressees` on Task 3's `send`, which drives the camera's facing/order AND §8 step 7's multi-addressee whisper variant).

**Files:**
- Modify: `macos/ComicChatKit/Sources/ComicChatKit/ProtocolSession.swift` (6 wrappers), `ChatSessionModel.swift` (room list accumulation, `getInfo`, member rows)
- Create: `macos/ComicChat/ComicChat/RoomListWindow.swift`
- Modify: `macos/ComicChat/ComicChat/AppCommands.swift` (Room menu: Room List…, Create Room…, Away toggle), `ChatWindow.swift` (member context menu: Kick/Ban, op-gated), `AppState.swift`, `ComicChatApp.swift` (Window scene)
- Create: `macos/ComicChatKit/Tests/ComicChatKitTests/RoomOpsTests.swift`

**Interfaces:**
- Consumes: `cc_session_create_room/kick/invite/ban/set_mode/away` (comicchat.h:579-586), `.roomListBegin/Item/End`, `ProtocolSession.list()`, `part(_:reason:)`, `join(_:key:)`, `RoomMember.isOp`.
- Produces:
  - `ProtocolSession` wrappers, all `onQueueGated` + `withEncodedCString` for user text, mirroring `setTopic`'s exact shape: `createRoom(_ channel: String, modes: String? = nil, maxUsers: UInt32 = 0, key: String? = nil)`, `kick(_ channel: String, nick: String, reason: String? = nil)`, `invite(_ channel: String, nick: String)`, `ban(_ channel: String, pattern: String, banning: Bool)`, `setRoomMode(_ channel: String, mode: UInt32, maxUsers: UInt32, password: String? = nil)`, `setAway(_ isAway: Bool, message: String? = nil)` (away is session-scoped — no token),
  - `public struct RoomListItem: Sendable, Equatable { public let name: String; public let users: Int32; public let topic: String }`,
  - `ChatSessionModel.requestRoomList() async throws` + `onRoomList: (@Sendable ([RoomListItem]) -> Void)?` (accumulate `.roomListBegin` → items → `.roomListEnd`, then fire once with the full array),
  - `AppState.roomList: [RoomListItem]`, `AppState.requestRoomList()`, `AppState.goToRoom(_ name: String)`,
  - member-list upgrades: `ChatSessionModel.onMembers` payload widens to `[MemberRow]` where `public struct MemberRow: Sendable, Equatable, Identifiable { public var id: String { nick }; public let nick: String; public let isOp: Bool; public let avatarName: String }` (built inside the existing `emitMembers` detached-Task from `RoomMember` — same snapshot, richer rows; `AppState.members` becomes `[MemberRow]` and `ChatWindow`'s `List` shows a small icon thumbnail resolved via the avatar name → `AvatarFile.iconImage()` with an AppState-level `[String: CGImage]` cache + an op badge),
  - `AppState.selectedMembers: Set<String>` (the `List` selection) — `ChatSessionModel.send` (Task 3's) gains `addressees: [String] = []`; `ChatWindow` passes the selection; the model puts them into `ann.addressees` AND the wire `T<nick>` list rides for free,
  - `ChatSessionModel.getInfo(_ nick: String)` + `onUserInfo: (@Sendable (String, String) -> Void)?` (nick, formatted result) backing a member-row "Get Info" popover — implemented over the EXISTING `ProtocolSession.who(_ mask:)` + `.whoResult` events (there is NO `cc_session_whois` builder in the outbound surface — comicchat.h:600 has `who` only; do not invent one, WHO-by-nick returns the user/host/real info the popover needs).

- [ ] **Step 1: Write the failing tests.** `RoomOpsTests.swift` (serialized, loopback scaffold): (1) each wrapper emits its expected wire verb (assert server-received bytes contain `KICK`, `INVITE`, `MODE` +b / room modes, `AWAY`, and create-room's join-with-modes shape — assert on the VERBS, not exact grammar: the engine builders own the grammar, the test pins that the wrapper reaches the right builder); (2) room-list accumulation: server replies `321`/`322`×3/`323` → `onRoomList` fires once with 3 items; (3) Go To semantics: `goToRoom("#other")` → server sees `JOIN #other` (NO `PART`), `rooms` gains a tab, active room is `#other` with a fresh title-only strip. Run → FAIL.

- [ ] **Step 2: Implement** per Produces. `RoomListWindow.swift`: `Table` of `RoomListItem` (Name/Users/Topic), a filter `TextField` (name/topic substring, case-insensitive — client-side like the original roomlist.cpp:27-60), min-users `Stepper`, Refresh button (`requestRoomList`), Go To button (`goToRoom` + close). Room menu gains "Room List…" (opens the window), "Create Room…" (an alert-with-textfield sheet → `createRoom` + `goToRoom`), "Away" toggle (`setAway`). Member context menu gains Kick/Ban…, `.disabled(!selfIsOp)` where `selfIsOp` comes from `session.room(config.room)?.members[ownNick]?.isOp == true` surfaced via a model accessor (`ChatSessionModel.selfIsOp` — detached-Task-read into AppState on membership changes, same pattern as `emitMembers`; simplest: extend `emitMembers` to also push `selfIsOp` through a new `onSelfOp: ((Bool) -> Void)?`).
- [ ] **Step 3: Tests + build green; commit.** `git commit -m "macos: Plan 4b Task 8 - room list window + room ops surface + member-list upgrades"`

---

### Task 9: Sounds — inbound playback over a user sounds folder

D1 §4.2 finding (binding): **NO WAV files exist in any tree** — the original played from the Windows media directory by filename; sounds were never downloaded (plan3 command-surface.md:288). So: empty Application Support sounds folder + playback of inbound `.sound` events when a matching file exists; silent-with-status-note otherwise. **DECIDED (Tim, plan review): ship the empty folder** — the spec-§5 "original WAV assets bundled" line is amended. Outbound `#SOUND` send: deferred (header).

**Files:**
- Create: `macos/ComicChatKit/Sources/ComicChatKit/SoundLibrary.swift`
- Modify: `macos/ComicChatKit/Sources/ComicChatKit/ChatSessionModel.swift` (`.sound` routing), `macos/ComicChat/ComicChat/AppState.swift` (player)
- Create: `macos/ComicChatKit/Tests/ComicChatKitTests/SoundLibraryTests.swift`

**Interfaces:**
- Consumes: `.sound(nick:file:text:)`, `SettingsStore.soundsEnabled/soundsFolder` (Task 5).
- Produces:
  - `public struct SoundLibrary: Sendable { public init(folder: URL); public func resolve(_ name: String) -> URL? }` — case-insensitive match on basename, `.wav` only (MIDI dropped per spec §5), strips any path components from `name` (a remote nick names the file — never let it traverse), tries `<name>` then `<name>.wav`,
  - `ChatSessionModel.onSound: (@Sendable (String, String) -> Void)?` (nick, file) — fired from `handleLocked`'s `.sound` case (transcript-appended like everything else),
  - `AppState`: an `AVAudioPlayer` holder; on `onSound`, if `settings.soundsEnabled`, `SoundLibrary(folder:).resolve(file)` → play; else/miss → status line `"<nick> played <file>"` (visible either way — playback is an overlay, not a replacement),
  - the folder is created on first app launch (`AppState` init: `FileManager.createDirectory(… "Comic Chat/Sounds" …, withIntermediateDirectories: true)`).

- [ ] **Step 1: Failing tests.** `SoundLibraryTests.swift` (not serialized): temp dir with `Boing.wav` → `resolve("boing")`, `resolve("BOING.WAV")` hit; `resolve("../etc/passwd")` → nil-or-inside-folder; `resolve("missing")` → nil; a `.mid` file present → NOT resolved. Plus in `ChatSessionModelTests`: a replayed `.sound` event fires `onSound` and lands in the transcript.
- [ ] **Step 2: Implement; tests green; build green.**
- [ ] **Step 3: Commit.** `git commit -m "macos: Plan 4b Task 9 - inbound sound playback (user sounds folder, wav-only)"`

---

### Task 10: Save/reopen JSON transcript + PNG/PDF export + print

Spec §5's deliberate deviation: conversations save as JSON transcripts (messages/annotations/participants) and reopen re-renders identically — the transcript IS already the event log (`ChatSessionModel.transcript`), so save = encode, reopen = replay through a fresh strip. Includes the carryover **reflow byte-compare determinism test**. **ONE-STRIP-AT-A-TIME consequence (APPROVED by Tim at plan review):** the reopen viewer renders via the same process-global engine, so opening a saved conversation while connected is refused with an alert ("Disconnect first") — a second concurrent strip is engine UB, not a UI choice.

**Files:**
- Create: `macos/ComicChatKit/Sources/ComicChatKit/ConversationFile.swift`, `TranscriptRenderer.swift`
- Modify: `macos/ComicChatKit/Sources/ComicChatKit/ProtocolEvents.swift` (`Codable` conformances)
- Modify: `macos/ComicChat/ComicChat/AppCommands.swift` (File menu), `AppState.swift`, `ChatWindow.swift` (viewer presentation)
- Create: `macos/ComicChatKit/Tests/ComicChatKitTests/ConversationFileTests.swift`, `ReflowDeterminismTests.swift`

**Interfaces:**
- Consumes: `ChatSessionModel.transcript`, `Strip`/`ProtocolStripBridge`/`CGCanvas` (the offline replay mirrors `reflowLocked`'s exact sequence), `PanelFit`.
- Produces:
  - `Annotations: Codable`, `ProtocolEvent: Codable` (both synthesized — enums with associated values synthesize since Swift 5.5; add the conformance clauses and fix any member that resists),
  - `public struct ConversationFile: Codable, Sendable { public var formatVersion: Int; public var host: String; public var room: String; public var nick: String; public var characterName: String; public var backdropName: String; public var encodingRaw: Int32; public var events: [ProtocolEvent]; public static func read(from url: URL) throws -> ConversationFile; public func write(to url: URL) throws }` (`formatVersion` 1; write pretty-printed JSON),
  - `ChatSessionModel.conversationFile() -> ConversationFile` (snapshot of config + the ACTIVE room's transcript — post-Task-7 the save unit is one room's conversation, matching the original's per-doc save),
  - `public final class TranscriptRenderer` — `init(file: ConversationFile, artDir: String)`, `func render(columns: Int32, unitTwips: Int32, scale: CGFloat) throws -> (image: CGImage, pngData: Data)` — its OWN serial queue, installs a metrics canvas, fresh `Strip` (geometry → title → backdrop → self participant → bridge → apply all events → compose), tears everything down (strip close + `cc_set_metrics_canvas(nil)`) BEFORE returning — the whole call is one atomic engine occupation, honoring one-strip-at-a-time as long as no live session exists (documented; the App gates it),
  - App: File menu **Save Transcript… ⌘S** (`NSSavePanel`, `.json`), **Open Transcript…** (`NSOpenPanel` → refuse if `appState.model != nil` → render via `TranscriptRenderer` at the current window width → present in a plain viewer sheet/window reusing `ComicStripView(image:sizePoints:model: nil)` — the view already tolerates a nil model), **Export as PNG…** (current live strip: reuse the last composed image → `NSSavePanel` + `pngData`), **Export as PDF… / Print… ⌘P** (`NSPrintOperation` over a `ComicPrintView: NSView` that draws the composed `CGImage` scaled to fit page width; vertical pagination free via `NSPrintInfo` — PDF export = the print panel's PDF button, plus a direct `dataWithPDF(inside:)` save for Export as PDF).

- [ ] **Step 1: Failing round-trip test.** `ConversationFileTests.swift`: build a `ConversationFile` with a representative event array (text-with-cooked-annotations, whisper, appearsAs, sound, joins) → `write` → `read` → `#expect(decoded == original)`-style comparisons (add `Equatable` where free). Run → compile FAIL (no Codable).
- [ ] **Step 2: Implement Codable + ConversationFile; test green.**
- [ ] **Step 3: Failing determinism test.** `ReflowDeterminismTests.swift` (serialized): a fixed `ConversationFile` (reuse the fixture-derived transcript: run `FixtureReplayServer`+model once and snapshot, or hand-build 6 events) rendered TWICE through separate `TranscriptRenderer` instances at identical geometry → `#expect(png1 == png2)` (byte-equal — legal under the real-metrics rule because it compares real-vs-real within one run, not against a frozen golden; note this in the test's doc comment).
- [ ] **Step 4: Implement `TranscriptRenderer`; determinism green.** (This test also pays the 4a carryover: same transcript + geometry ⇒ identical PNG.)
- [ ] **Step 5: App wiring** per Produces; build green; screenshot the reopened-transcript viewer next to a saved session; coordinator eyeballs.
- [ ] **Step 6: Commit.** `git commit -m "macos: Plan 4b Task 10 - JSON transcript save/reopen + PNG/PDF export + print + reflow determinism test"`

---

### Task 11: Plain-text view toggle

D1 §1.2: `NSTextView` transcript from the same event log; View menu toggles comic/text per spec §5 (persisted `view.comicMode`).

**Files:**
- Create: `macos/ComicChatKit/Sources/ComicChatKit/TranscriptTextBuilder.swift`
- Create: `macos/ComicChat/ComicChat/TranscriptTextView.swift`
- Modify: `macos/ComicChat/ComicChat/ChatWindow.swift`, `AppCommands.swift` (View menu), `AppState.swift`
- Create: `macos/ComicChatKit/Tests/ComicChatKitTests/TranscriptTextBuilderTests.swift`

**Interfaces:**
- Consumes: `[ProtocolEvent]`, `SettingsStore.comicMode`.
- Produces: `public enum TranscriptTextBuilder { public static func attributedString(for events: [ProtocolEvent], showArrivals: Bool = true) -> AttributedString }` — per-kind styling: `nick: text` plain; whisper `nick whispers: text` italic secondary; action `• nick text` italic; sound `♪ nick played file`; arrivals `→ nick joined` / `← nick left` secondary (only when `showArrivals`); status/errors secondary. App: `TranscriptTextView` (`NSViewRepresentable` wrapping a non-editable `NSTextView` in a scroll view, stick-to-bottom on append — set the attributed string from `AppState.transcriptText`); `ChatWindow` shows it instead of `ComicStripView` when `!appState.comicMode`; View menu "Comic Strip view ⌘1"/"Plain Text view ⌘2"; `AppState.transcriptText` recomputed from `model.transcript` on each strip/status update it already observes (cheap at chat scale; a `.text`-count-gated cache if it ever isn't).

(Post-Task-7 note: the text view shows the ACTIVE room's transcript — `AppState.transcriptText` recomputes on tab switch too.)

- [ ] **Step 1: Failing builder tests** (content assertions on the produced string for one event of each styled kind; arrivals suppressed when `showArrivals: false`). **Step 2: Implement; green.** **Step 3: App wiring + build; screenshot both views of the same replayed session; coordinator eyeballs.** **Step 4: Commit.** `git commit -m "macos: Plan 4b Task 11 - plain-text transcript view + view toggle"`

---

### Task 12: §8 acceptance prep — rig scripts + runbook (agent-runnable half)

Everything the acceptance needs staged so the human session is pure driving. No public-server traffic in this task beyond the sanctioned 2-line probes.

**Files:**
- Create: `.superpowers/rig/probe-servers.sh`, `.superpowers/rig/run-topology-a.sh`, `.superpowers/rig/run-topology-b.sh`
- Create: `docs/superpowers/plans/2026-07-18-plan-4b-acceptance-runbook.md`

**Interfaces:**
- Consumes: the existing rig (`run-rig.sh`, `capture-proxy.ts <listen> <host> <port> [log]`, ngircd config, the quarantine-stripped Wine at `~/Applications/Wine Stable.app`), D4 §6's checklist.
- Produces: three executable scripts + the runbook the human session follows.

- [ ] **Step 1: `probe-servers.sh`** — the 2-line checks (exactly what 4b planning ran): `printf 'MODE ISIRCX\r\nQUIT\r\n' | nc -w 6 www.crypthome.com 6667` (expect `800 * 0 0 ANON 512 *`) and same for `comic.dedoky.com` (expect `451`). Prints PASS/FAIL per server. Run it: both PASS today.
- [ ] **Step 2: `run-topology-a.sh`** — starts `./run-rig.sh` (ngircd + win-side proxy :6668 + Wine client into `#comicrig`) PLUS the Mac-side proxy: `bun capture-proxy.ts 6669 127.0.0.1 6667 captures/p4b-a-mac.jsonl`. Echoes the Mac app connect instructions (`127.0.0.1:6669`, `#comicrig`). `stop` argument kills everything (mirror run-rig.sh's own stop).
- [ ] **Step 3: `run-topology-b.sh`** — dual proxies at crypthome: `bun capture-proxy.ts 6668 www.crypthome.com 6667 captures/p4b-b-win.jsonl` + `bun capture-proxy.ts 6669 www.crypthome.com 6667 captures/p4b-b-mac.jsonl`, then launches Wine `cchat.exe "irc://127.0.0.1:6668/#cctest"`. Prints the etiquette preamble (scratch room `#cctest`, say hello in `#Crypt` if people are present, keep it short, 10-user memorial box).
- [ ] **Step 4: The runbook** — D4 §6's ordered checklist **verbatim** (steps 1-11), once per topology, with: the P1-P4 prerequisites marked DELIVERED (P1/P3 = 4a Tasks 2/8; P2 = 4a Task 6; P4 = 4a Task 3 — accented text now additionally SAFE outbound via 4b Task 1, but keep acceptance text ASCII per D4 risk 7 anyway); the expected-silence notes (CTCP Get Version/Ping/Profile/Get Character → no reply; custom-avatar publishing deferred); artifact locations (`.superpowers/rig/captures/p4b-*`, `.superpowers/sdd/p4b-acceptance/` for screenshots); the `cc-dumpart --replay` reproduction step; the fixture-promotion step (an annotated + an escaped-byte + a real captured announce exchange → `Tests/ComicChatKitTests/Fixtures/captures/`, with the HAND-AUTHORED marker retired where a real capture replaces one); the IRC7-in-Docker contingency (ONLY if crypthome probe fails on the day: build from `github.com/irc7-com/irc7` Dockerfile, smoke-test that pre-registration `MODE ISIRCX` answers 800/451 — D4 risk 4 — before substituting it as Topology B); dedoky fallback (`NICKLEN=9` — pick short nicks); a **local multi-room sanity step appended to Topology A** (Tim joins two rooms on the local rig, verifies tab switch renders both correctly — Task 7's live validation; LOCAL ONLY, never on the public boxes).
- [ ] **Step 5: Dry-run Topology A end-to-end without Tim** (agent-legal: local only): scripts up, Mac app connects through the proxy, Wine client visible, one say from the Mac side typed by… nobody — verify everything EXCEPT typed input (app connects, joins, strip renders the Wine client's presence). Screenshot both windows side by side; coordinator eyeballs. Fix script bugs now, not during Tim's session.
- [ ] **Step 6: Commit.** `git commit -m "macos: Plan 4b Task 12 - acceptance rig scripts + runbook (topology A dry-run verified)"`

---

### Task 13: THE spec-§8 LIVE ACCEPTANCE ★ (human-driven — Tim at the keyboard)

The project's exit milestone. The agent runs rigs and captures artifacts; **Tim drives both compose bars** (Mac app + Wine 1998 client). Etiquette rules from D4 §1.1 are binding on the Topology B session.

**Files:** artifacts only — `.superpowers/rig/captures/p4b-*`, `.superpowers/sdd/p4b-acceptance/*.png`, promoted fixtures under `Tests/ComicChatKitTests/Fixtures/captures/`, ledger entry in `.superpowers/sdd/progress.md`.

- [ ] **Step 1: Morning-of probe.** `./probe-servers.sh`. Crypthome down → invoke the runbook's IRC7 contingency (or dedoky fallback for plain-IRC-only, noting IRCX coverage gap to Tim).
- [ ] **Step 2: Topology A session** (local ngircd): run the runbook's 11 steps with Tim. Every step checked off in the runbook copy saved to `.superpowers/sdd/p4b-acceptance/runbook-a.md`.
- [ ] **Step 3: Topology B session** (crypthome IRCX): same, in `#cctest`; verify the 800→IRCX→800→NICK/USER pivot in the capture; verify DATA/CCUDI1 both directions; whisper via the WHISPER inbound verb at least once (D4: outbound whispers are always PRIVMSG — inbound verb is the IRCX-only variant).
- [ ] **Step 4: Replay reproduction.** `swift run cc-dumpart --replay` over the Mac-side s2c capture from each topology; compare the reproduced strip against the live screenshots (coordinator + Tim eyeball).
- [ ] **Step 5: Fixture promotion.** Promote from the captures into the corpus: (a) an annotated cooked-pose exchange, (b) an escaped-byte annotation line (the Plan-3 must-own finally captured for real — retire the HAND-AUTHORED marker note in the Task 3 vector's comment, keeping the hand-authored vector itself), (c) a real `# Appears as` announce (4a Task 8's brief-premise correction wanted one). Add/adjust replay tests referencing them; `swift test` green.
- [ ] **Step 6: Ledger + exit artifacts.** Progress-ledger entry with both topologies' outcomes, artifact paths, any deviations observed (own-say echo behavior of each server — the Task 1 dedup's live validation; DATA re-pairing under real interleaving — D4 risk 5). Any FAILURE at any step becomes a replayable fixture + a fix task — do not paper over (the 4a fonts.cpp lesson: live runs find what selftests mask).
- [ ] **Step 7: Commit** (fixtures + runbook copies + ledger). `git commit -m "macos: Plan 4b Task 13 - spec-§8 live acceptance (topology A+B) + fixture promotion"`

---

## Exit milestone (Plan 4b)

**The spec-§8 manual acceptance passes on both topologies** — poses, avatars, whispers in both directions against the genuine 1998 client; artifacts captured (dual JSONL + screenshots + replay reproduction); fixture corpus refreshed with real annotated/escaped/announce captures. With that, the port's spec is delivered end-to-end: engine (Plans 1-2), protocol (Plan 3), app (4a), features + live interop (4b).

## Plan-review decisions (Tim, 2026-07-18 — all recorded, binding)

1. **Sounds source (Task 9):** APPROVED — empty Application Support sounds folder; playback works the moment WAVs are dropped in. Spec §5's "original WAV assets bundled" is amended accordingly.
2. **Whisper UI (Task 4):** APPROVED — ONE tabbed box (spec §5 "separate small windows" amended), TEXT leaves, room whispers still balloon in the main strip.
3. **Multi-room:** **TIM OVERRODE THE RECOMMENDATION — true multi-room is IN SCOPE** → Task 7 (one connection, N rooms, one live strip, room tabs). D1 R7's defer is retired.
4. **Auto-download default (Task 6):** APPROVED — ON by default (deviation from the original's OFF, chat.cpp:208, recorded).
5. **Saved-transcript viewer gating (Task 10):** APPROVED — refuses to open while connected (one-strip-at-a-time).

## Merge preconditions (standing, Tim-gated — unchanged by 4b)

1. **Re-sign the unsigned range in ONE history rewrite** (unsigned: the contiguous `d1da855..93a536f` span + `0109da4`, plus any 4b commits made under the fallback authorization; `005192c..0b485c8` are signed). Needs Tim's 1Password unlock; never bypass signing for a merge.
2. **Tim's `Tims-Mac` scrub decision** on `Tests/ComicChatKitTests/Fixtures/captures/smoke-2.jsonl` — fold into the same rewrite.
3. Re-verify `swift test` + `xcodebuild` on the rewritten head.

## Self-review (against the handoff, the spec, and the four reports)

- **14-item backlog coverage:** 1→T2+T3; 2→T4 (+think/action in T3); 3→T5; 4→T6; 5→T8; 6→T9; 7→T10; 8→T11; 9→DEFERRED (recorded decision, header); 10→**T7 (true multi-room — Tim's plan-review override of the defer recommendation)**; 11→DEFERRED (perf, until demanded); 12→DEFERRED (hit-test); 13→T12+T13; 14→Merge preconditions.
- **8 named carryovers:** say-encoding §8 prerequisite → T1 (incl. whisper/setTopic, found wider than named); NUL-truncation doc → T1 S3; `s->avatars` accumulation comment → T6; GetCharInfo defer-or-event → DECIDED defer, header + T6; set_title-after-lines guard → not triggered (title stays room name; recorded); own-say double-render → T1 dedup + T12 S6 live validation; reflow byte-compare → T9 S3; emitMembers ordering → T1 S7-8; `[weak co]` → T1 S9; replay-fixture sheet → T1 S9.
- **Spec-§5 features:** wheel ✓T3, whisper boxes ✓T4 (amended one-window), multi-room ✓T7, room list ✓T8, member list incl. thumbnails/op badges/user-info popover/selection-as-addressee ✓T8 (self-review catch — these were in spec §5 but NOT in the 14-item backlog skeleton), pickers ✓T5, text view ✓T11, sounds ✓T9 (amended no-bundle), save/print/export ✓T10, connect/say/think/action ✓ (4a+T3). §8 ✓T12-13. NOTE: T1's members-ordering test is written against the `[String]` payload; T8 widens `onMembers` to `[MemberRow]` and updates that test in the same task. Signature evolution is deliberate and ordered: T3 defines `send(_:mode:)`; T7 adds `room:`; T8 adds `addressees:` — final shape `send(room:text:mode:addressees:)`, each task updating the compose-bar call site it owns.
- **Type consistency spot checks:** `send(_:mode:)` (T3) used by ComposeBar (T3) and unchanged by later tasks; `smMode(for:)` defined T3, reused T4's sendWhisper; `WhisperLine` defined T4 Produces, used in its tests/UI; `fromServer:` handleLocked param defined T1, used by T3's synthetic send; `selfAnnotations()` defined T2, consumed T3/T4; `changeCharacter` (T5) resets T3's `selfAvatarFile`; `extraDirs` (T6) feeds T5's picker search-order note; `ConversationFile` (T9) standalone; `TranscriptTextBuilder` (T10) standalone.
- **Read-first discipline:** T2 S1 (the one engine task) and T7 S1 (the engine's multi-room posture) carry mandatory read+escalate steps with exact citations; T4 S1 and T5 (config-string lifetime) name their read-first questions inline. No task edits original files beyond `cc_compose.cpp`/`cc_selftest.cpp` bridge code — Edit Rules bite nowhere new; if T2/T7 Step 1 finds otherwise, escalation is mandatory.
- **Placeholder scan:** the deliberate not-shown code (scaffold copies from named existing suites, Step-1-dependent C internals) is always accompanied by the exact source file to copy from or the exact escalation path — discovery steps, not TODOs.
- **Risk honesty:** T2 S1 (builder entanglement), T7 S1 (single-room engine state — the one place multi-room could hit an engine wall; explicit NEEDS_CONTEXT escalation with a named fallback that itself requires a ruling), T10 (one-strip gating), T13 S1 (server availability) carry explicit STOP/contingency outcomes.



