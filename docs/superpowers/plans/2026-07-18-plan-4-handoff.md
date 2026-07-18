# Handoff: start Plan 4 (the Mac app)

**For the next session.** Read this first, then the documents below. Plans 1–3
are complete: the engine (art + layout + Canvas), the comic compositor
(`cc_strip`), and the protocol layer (IRC/IRCX + annotations) all exist and are
reviewed clean. **Plan 4 builds the actual macOS application** on top of them —
the last of the four plans.

## Read in this order

1. `docs/superpowers/plans/2026-07-17-macos-port-roadmap.md` — **Plan 4 section**
   ("The Mac app") for the original→Mac view/dialog/sound mapping, plus the
   "Debt handed to Plan 4" items scattered under the Plan 2 — DONE section
   (title/starring rendering, `cc_avatar_icon_image` accessor, real-CoreText
   layout metrics, `StretchDIBits` decode caching).
2. `docs/superpowers/specs/2026-07-17-macos-port-design.md` §5 (the Mac app:
   chat window, sheets/windows, settings, sounds, save/print — the JSON
   transcript deviation), §6 (persistence: UserDefaults + JSON), §4.5 (the
   CP-1252/UTF-8 encoding toggle lives in settings), §8 (manual acceptance:
   live interop with the real Windows 2.5 client).
3. `docs/superpowers/plans/2026-07-18-macos-port-plan-3-protocol.md` — the whole
   thing is context, but especially the **C boundary reference** section (the
   `cc_session` API the app drives) and the **Edit Rules R18–R21 + amendments**
   (still binding if Plan 4 lifts any further original file — e.g. `userinfo.cpp`,
   `webreq.cpp`).
4. The four Plan 3 discovery reports in
   `docs/superpowers/plans/2026-07-18-plan3-discovery/` — reference for how the
   protocol/state/codec actually work (state-and-codec §1.4 is the definitive
   "what Swift tracks" list, already realized in `ProtocolSession`).

## Current state (verified 2026-07-18)

- Branch `macos-port` (kept unmerged from `main` by choice), HEAD `47f65c4`.
- `swift test` from `macos/ComicChatKit/`: **33/33 green, 8 suites** (verified
  on a clean `rm -rf .build && swift test`).
- **Plan 3's exit milestone stands as the current end-to-end proof:** a stream
  of `ProtocolEvent`s carrying decoded annotations drives the `cc_strip`
  compositor to a rendered comic panel via the *cooked-pose* path (the sender's
  wire-supplied pose, not text-inferred). Exit PNG at `.superpowers/sdd/p3-exit.png`
  (git-ignored) — two panels, two avatars, "HELLO"/"HI THERE" balloons.
  `swift run cc-dumpart --replay <capture.jsonl> out.png` drives it from the CLI.
- **What exists to build on:**
  - Engine C API (`comicchat.h`): art loading (`cc_avatar_*`/`cc_backdrop_*`),
    the strip compositor (`cc_strip_*`, incl. `cc_strip_add_line_cooked`), and
    the protocol session (`cc_session_*`: create/feed_bytes/fire_timer, outbound
    join/part/change_nick/set_topic/send_say/send_whisper/who/list + the
    not-yet-called create_room/kick/invite/ban/set_mode/away, resolver/timer/send
    callbacks, 34-variant `cc_proto_event`).
  - Swift (`Sources/ComicChatKit/`): `ProtocolSession` (NWConnection driver +
    §1.4 state model + `AsyncStream<ProtocolEvent>`), `ProtocolStripBridge`
    (events→strip), `WireCodec` (CP-1252/UTF-8), `CGCanvas` (CoreGraphics),
    `Strip`/`StripScript`, art loaders. `cc-dumpart` is the headless CLI harness.
  - The Wine capture rig (`.superpowers/rig/`, git-ignored, `./run-rig.sh`) —
    reusable for interop capture during Plan 4.
- `v2.5-beta-1-modern/` and `artifacts*/` are read-only reference. Never edit.

## Plan 4 process (same shape as Plans 1–3)

Spec is approved — do NOT re-brainstorm. Flow: run the discovery tasks below →
invoke `superpowers:writing-plans` (bite-sized TDD tasks, complete code, the
Edit-Rules table carried forward if any lift happens, amendments as R22+) → user
reviews → execute via `superpowers:subagent-driven-development` (fresh implementer
per task, task-scoped reviewer, fix loops, final whole-branch review). Use the
`p4-` prefix for briefs/reports to avoid the filename collision that bit Plans
1↔2 (the ledger records this).

**Plan 4 is different from 1–3 in one big way: it is mostly NEW Swift/AppKit
code, not lifting.** The engine is done; Plan 4 wires it into a real app. So the
Edit Rules matter far less (only if you lift `userinfo.cpp`/`webreq.cpp`), and
the risk shifts from fidelity to UX/AppKit integration and to the manual
live-interop acceptance (spec §8), which finally exercises the whole stack
against a real server + the real Windows client.

**Discovery tasks (run before designing):**
1. **The app skeleton + which windows are MVP.** Spec §5 lists a lot (chat
   window, member sidebar, compose bar with emotion wheel, connect sheet, room
   list, character/backdrop pickers, user-info popover, whisper windows,
   settings). Decide the minimal first-render slice: probably connect → join →
   see the comic strip update live as messages arrive → send a say. The emotion
   wheel and the pickers can come after a working chat loop.
2. **The comic view: custom `NSView` in a scroll view driven by the engine
   `Canvas`.** The engine already composes to a `cc_canvas`; `CGCanvas` already
   renders it. The Plan 4 work is an `NSView` that hosts a `CGCanvas`-backed
   layer and re-composes as the `AsyncStream<ProtocolEvent>` (via
   `ProtocolStripBridge`) grows the strip. Figure out the incremental-render vs
   full-recompose story (Plan 2 noted `StretchDIBits` re-decodes per blit — if
   the view redraws every frame, that caching debt becomes real).
3. **Real-CoreText layout metrics.** Plans 2/3 ran layout against the
   deterministic fake-metrics recording canvas; the roadmap flags that a real
   interactive view should drive layout from real CoreText metrics
   (`CGCanvas`'s `TEXTMETRIC` heuristics are placeholders). Decide whether Plan 4
   switches layout to real metrics (and re-baselines the "text sits high in
   balloon" §9 cosmetic divergence) or defers.
4. **Live-interop acceptance rig (spec §8).** The Wine rig captured plain-IRC in
   Plan 3; Plan 4's acceptance is a live session against a hobbyist MS Chat /
   IRCX server alongside the real Windows 2.5 client, poses/avatars/whispers both
   directions. Scope what server is reachable and whether an IRCX shim is needed.

## Debt / follow-ups handed to Plan 4 (from Plan 3 + earlier)

**Must-own (Plan 3 whole-branch review flagged these two so they aren't lost):**
- **The accented-byte CP-1252 case-fold table.** The `CharUpperBuff` shim
  (`mfc_compat.h`) folds ASCII only; real CP-1252 uppercases the accented range
  0xE0–0xFE→0xC0–0xDE (é→É). Latent today (all fixtures are ASCII) but a real
  divergence the moment accented text renders. The mapping is a **deterministic
  published codepage table** — implement it (the shim comment overstates it as
  "no authoritative source"). This is the gate on replaying/rendering any
  accented capture.
- **An escaped-byte annotation round-trip test vector.** Plan 3's byte-compare
  used a vector with no `{0x0A,0x0D,0x10}`, so low-level unquoting ran as a
  no-op in that test (unquoting itself has direct coverage). Add an
  annotation-with-escaped-byte vector, ideally from a real annotated capture.

**Standard Plan-4 scope (from the roadmap + Plan 3):**
- **Avatar announce/download.** The `# Appears as <name>.<url>` protocol handshake
  is lifted and emits `CC_EV_APPEARS_AS`; the actual HTTP fetch of the `.avb` is
  Plan 4's Swift `URLSession` (replacing `webreq.cpp`/WinInet). `ProtocolStripBridge`
  currently no-ops post-create `.appearsAs` because `cc_strip` has no change-avatar
  API — Plan 4 adds avatar assignment/refresh.
- **The 7 not-yet-called outbound C fns** (`cc_session_create_room/kick/invite/
  ban/set_mode/away`) — the complete command vocabulary exists; Plan 4 wires the
  UI (room-props dialog, kick/ban menus, away toggle) to them.
- **Title/starring rendering.** `AddTitle`/`AddStars`/`CStarLabel::Draw` were
  R11-wrapped in Plan 2 as a known deferral; `session.comicsTitle` data exists,
  only the rendering is missing. Plan 4 draws the comic-strip title bar + star
  rating.
- **`cc_avatar_icon_image` accessor** — the icon pose is excluded from the pose
  API by design; the character picker needs this bridge accessor added (Plan 1
  handover, still open).
- **Save/print** — JSON transcript save/reopen (spec §5 deviation; the comic is
  deterministic given annotations) + `NSPrintOperation`/PDF/PNG export through
  the same `Canvas` path.
- **Settings** — `UserDefaults` (persona, display options, sounds, connection
  defaults) + the CP-1252-default / UTF-8-opt-in encoding toggle (spec §4.5;
  `ProtocolSession` already takes an `encoding` config).
- **Sounds** — `AVAudioPlayer`, bundled WAV assets (locate originals in `res/`);
  inbound `\x01SOUND\x01` already emits `CC_EV_SOUND{file}`.
- **Multi-room** — the engine currently models one room's protocol state at a
  time (single `CIrcProto` per session); the Swift `rooms` dict is canonical for
  membership/props. True simultaneous multi-room protocol state (mode/topic per
  room) needs either per-room engine sessions or an engine change — decide if
  Plan 4's UX needs it or one-room-at-a-time suffices.
- **CTCP auto-replies** — VERSION/PING/TIME/EMAIL/URL auto-NOTICE replies were
  R20-dropped in Plan 3 (the engine emits events, doesn't auto-answer). If real
  peers expect answers, Plan 4 wires them through the outbound API.

## Techniques that worked in Plans 1–3 (keep using them)

- **Discovery-first, then plan, then subagent-driven execution.** Fresh
  implementer per task; task-scoped reviewer that diffs against originals (for
  any lift) and adjudicates the implementer's flagged concerns; fix loops;
  final whole-branch review. This caught real defects every plan (Plan 3: the
  Task-1 NULL-safety Critical, the KICK-as-part event-loss, the IRCX
  DATA-pairing architecture gap, the multi-chunk framer bug the real capture
  exposed).
- **Implementer escalation over improvising** — DONE_WITH_CONCERNS / NEEDS_CONTEXT
  beats a wrong guess. Plan 3's KICK routing should have escalated (it didn't,
  the review caught it); the DATA-pairing and the cooked-pose entry point were
  correct escalations that became plan amendments. Review output is a hypothesis
  to verify in both directions.
- **Real-artifact verification.** The exit milestone is a *viewed* PNG, not a
  claim; the §8.2 replay uses *real captured bytes* from the 1998 client, which
  found a bug hand-authored vectors missed. Plan 4's acceptance is live interop —
  keep the "prove it with a real artifact" discipline.
- **The `.serialized` suite tree.** Every engine-global-state test suite nests
  in `EngineGlobalStateSelfTests` (both `cc_strip` and `cc_session` touch the
  process-global single-threaded engine; a non-nested suite races). Plan 4's
  view/session tests must join the same tree.
- **Ledger discipline.** `.superpowers/sdd/progress.md` has every task of every
  plan; it's the recovery map after compaction. Append one line per task on
  clean review.

## Environment quirks (unchanged from Plan 3)

- **Commits are SSH-signed via 1Password; the agent relocks mid-session**
  ("failed to fill whole buffer" / no identities in `ssh-add -l`). Tim authorized
  the unsigned fallback (`git commit --no-gpg-sign`, say so in the record).
  **The entire Plan 2+3 range `d1da855..47f65c4` is UNSIGNED and must be
  re-signed before any merge to `main`** — the whole-branch review named this the
  one pre-merge precondition. Ask Tim to unlock before a merge; never bypass
  signing for a merge without authorization.
- `swift test` from `macos/ComicChatKit/` is the truth; SourceKit shows false
  "No such module: cchat_engine" / "No such module: Testing" — ignore them.
- Test files resolve the repo root with **FIVE** `deletingLastPathComponent()`
  from `#filePath` (test-target fixtures); Plan 3's committed fixtures use
  `Bundle.module` resources instead — both patterns are in the tree.
- The C++ engine module name in Swift is `cchat_engine` (hyphen→underscore).
- **XcodeBuildMCP is available** in this environment (per the roadmap) for
  building, running, and screenshotting the actual `.app` during Plan 4 — this
  is the plan where it finally matters (Plans 1–3 were headless `swift test`).
- SDD scratch (briefs/reports/ledger + the exit PNGs) lives in `.superpowers/`
  (git-ignored). Plans 1/2/3 ledgers are all in `.superpowers/sdd/progress.md`.
