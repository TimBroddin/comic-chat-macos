# Handoff: start Plan 4b (the full app + live acceptance)

**For the next session.** Read this first, then the documents below. Plans 1–3
(engine, compositor, protocol) and **Plan 4a (engine surface + the MVP chat
loop) are complete and final-reviewed clean** — the Mac app EXISTS and runs:
connect sheet → login → join → live comic strip → send, human-verified live
(Tim typed says; exit artifact `.superpowers/sdd/p4a-exit.png`). **Plan 4b
finishes the app** — the remaining spec-§5 features — **and runs the spec-§8
live acceptance**, the project's final milestone.

## Read in this order

1. `docs/superpowers/plans/2026-07-18-macos-port-plan-4a-app-mvp.md` — the
   **"Plan 4b backlog"** section (14 items, each citing its discovery ground
   truth) is 4b's scope skeleton; also the **Task 2 and Task 12 amendments**
   (binding: the R18 second-800 site, the fresh `cc_session_login`, the
   `g_session` SAVE/RESTORE rule for reentrant `cc_session_*`, the fonts.cpp
   R13 instance) and the Global Constraints (all still binding).
2. The four Plan-4 discovery reports in
   `docs/superpowers/plans/2026-07-18-plan4-discovery/` — still ground truth:
   `app-skeleton-mvp.md` (post-MVP verdicts per original dialog, the FULL
   settings/UserDefaults table §5, menu mapping §6, the no-WAVs finding §4.2),
   `live-interop.md` (the §8 acceptance procedure §6 verbatim, server survey,
   avatar-announce reality §4, CTCP verdict §5), `comic-view-rendering.md`
   (§6d optional APIs: per-panel compose, hit-testing; the decode-cache sketch
   §4.3), `layout-metrics.md` (background only now — the switch happened).
3. `.superpowers/sdd/progress.md` — the P4a section: 12 task entries + the
   final-review entry with the **21-item Minor triage** (8 FIX-IN-4B items are
   named carryovers below).
4. Spec `docs/superpowers/specs/2026-07-17-macos-port-design.md` §5 (features
   still missing), §8 (manual acceptance — 4b's exit).

## Current state (verified 2026-07-18)

- Branch `macos-port` (kept unmerged from `main` by choice), HEAD `0109da4`.
- `swift test` from `macos/ComicChatKit/`: **58/58 green, 15 suites**;
  `xcodebuild -project macos/ComicChat/ComicChat.xcodeproj -scheme ComicChat
  build` → BUILD SUCCEEDED.
- **What exists to build on:**
  - **The app** (`macos/ComicChat`, Xcode 16 synchronized-group project, ad-hoc
    signed, NOT sandboxed — recorded decision): `ComicChatApp`/`AppState`
    (@Observable @MainActor; `connect()` tears down first; `disconnect()` is
    the mandatory-shutdown path), `ConnectSheet` (write-through SettingsStore
    bindings), `ChatWindow` (strip + sidebar + compose + status),
    `ComicStripView` (**parameter-passing contract**: construct
    `ComicStripView(image:sizePoints:model:)` from observed reads in the parent
    `body` — that is what drives updates; never revert to @Environment reads
    inside it), `--replay-fixture <path>` demo hook (fixture-coupled to
    `hand-authored-annotation.jsonl`, nick "Anon"/room "#comicrig").
  - **ComicChatKit**: `ChatSessionModel` (ONE engine queue owns every `cc_*`
    call — interleaving is PROVEN safe; event log = the canonical transcript;
    reflow = destroy strip + replay events; `shutdown()` mandatory and now
    deregisters the metrics canvas), `SettingsStore` (MVP keys),
    `FixtureReplayServer` (one connection; pacing machinery INERT by
    construction — sends all s2c on connect, documented honestly),
    `CTMetricsCanvas`/`CoreTextMetrics` (real metrics are the production
    default; `RecordingCanvas` is the golden harness FOREVER — never freeze
    byte goldens under real metrics), `PanelFit`, `ProtocolSession` (auto-login
    probe→451/800/timeout→NICK/USER; `announceAvatar`; `probeTimeoutMs`).
  - **Engine C API added in 4a**: `cc_session_login` (+`own_user`/
    `own_realname` config), `cc_session_announce_avatar`,
    `cc_strip_set_panel_geometry`/`get_panel_geometry`,
    `cc_strip_set_participant_avatar`, `cc_strip_set_self`/`set_title`
    (title/starring renders — panel 0), `cc_avatar_icon_image`. Full CP-1252
    `CharUpperBuff` fold table; escaped-byte annotation vectors.
- `v2.5-beta-1-modern/` and `artifacts*/` are read-only reference. Never edit.

## Plan 4b process (same shape as 1–4a)

Spec approved — do NOT re-brainstorm. Most discovery is ALREADY DONE (the four
reports). Flow: a short discovery-lite pass only where 4a changed the ground
(see below) → `superpowers:writing-plans` (bite-sized TDD tasks, complete code,
Edit Rules table carried forward, amendments R22+ via escalation) → **Tim
reviews** → `superpowers:subagent-driven-development`. Use the **`p4b-` prefix**
for briefs/reports. The 4b scope = the plan doc's 14-item backlog; the exit
milestone = **the spec-§8 live acceptance** (live-interop.md §6's ordered
checklist: Topology A local rig + Topology B `crypthome.com` IRCX, both
directions poses/avatars/whispers, artifacts captured, fixture corpus
promoted — HUMAN-DRIVEN with Tim at the keyboard; etiquette rules in that
report are binding).

**Discovery-lite candidates (small, not four parallel agents):** (1) re-probe
the public servers the morning acceptance runs (2-line `nc` check — crypthome
is a 10-user memorial box, dedoky a personal server; either can vanish);
(2) the emotion-wheel art path (`res/fc_*_l.bmp` are 8-bit DIB resources —
extraction/conversion route needed); (3) whether IRC7-in-Docker is wanted as
the deterministic IRCX understudy (only if crypthome is gone).

## Named carryovers (final-review triage — put these IN the plan, not a footnote)

**§8-acceptance PREREQUISITE:**
- **`ProtocolSession.say`'s text param is UTF-8-only `withCString`** — accented
  outbound text diverges from the CP-1252 wire. Align with Task 8's
  `withEncodedCString` helper (same file, precedent in `announceAvatar`)
  BEFORE the acceptance exercises accented text. (Also: document the
  embedded-NUL truncation in `withEncodedCString` while there.)

**Feature-task inheritances:**
- Avatar-download task inherits: `s->avatars` accumulates old+new `CAvatarX*`
  per participant on avatar switch (bookkeeping-only today; guard/comment when
  switches become frequent); `# GetCharInfo` is swallowed EVENTLESSLY in the
  engine (blocks custom-avatar publishing — explicit defer-or-new-event
  decision needed; bundled-name announce needs nothing).
- User-editable titles inherit: `cc_strip_set_title` AFTER lines exist is a
  documented-not-guarded precondition (UpdateTitle rebuild assumes panel 0 is
  a title panel) — add the runtime guard when titles become user-settable.
- Live-server work inherits: own-say double-render if a server echoes PRIVMSG
  to the sender (synthetic local echo has no dedup — check target servers).
- Save/replay task inherits: add the reflow byte-compare determinism test
  (same transcript + geometry ⇒ identical PNG).
- Polish batch (one small task or fold-ins): serialize `emitMembers` (detached
  Tasks race, full-snapshot semantics make it benign today); `[weak co]` in
  ComicStripView's debounce timer (self-healing retain cycle); replay-fixture
  start-failure leaves the connect sheet dismissed (dev-flag-only).

**Open questions for Tim (ask at plan review, not mid-execution):**
- **Sounds source** (D1 §4.2): NO WAV files exist in any tree — the original
  played from the Windows media directory. Empty Application Support sounds
  folder + `NSSound.beep()`, or Tim supplies period WAVs? (Spec §5 amendment
  either way.)
- **Whisper UI**: the original is ONE tabbed box, not spec §5's "separate small
  windows" — recommend matching the original (spec amendment).
- **Multi-room**: recommendation stands (one-room-per-connection-window, defer
  true multi-room) — confirm.

## Merge preconditions (standing, Tim-gated — unchanged by 4b)

1. **Re-sign `d1da855..HEAD` in ONE history rewrite** (unsigned: the contiguous
   d1da855..93a536f span + 0109da4; 005192c..0b485c8 are signed). Needs Tim's
   1Password unlock; never bypass signing for a merge.
2. **Tim's `Tims-Mac` scrub decision** on
   `Tests/ComicChatKitTests/Fixtures/captures/smoke-2.jsonl` — fold into the
   same rewrite to avoid two rewrites.
3. Re-verify `swift test` + `xcodebuild` on the rewritten head (the rewrite
   invalidates the current evidence SHAs).

## Techniques that worked in 4a (keep using them)

- **The viewed-artifact discipline pays for itself**: the fonts.cpp
  dangling-font UB was findable ONLY by running the real app (every selftest
  re-registers the metrics canvas, which masks it). 4b's acceptance is the
  ultimate version of this — capture everything (dual JSONL + screenshots both
  sides) so any failure becomes a replayable fixture.
- **Reviewer adjudication of flagged concerns worked every time**: T2's
  lifted-file edit (R18-ratified), T7's three escalations (all in the
  implementer's favor), T12's fonts.cpp (R13-ratified). DONE_WITH_CONCERNS +
  adjudicate beats improvising; brief premises being WRONG is normal and fine
  when surfaced (HrIrcLogin never ported; no announce in the captures;
  chicdial isn't the picker).
- **Coordinator eyeballs every visual artifact** (metrics before/after, title
  panel, both app screenshots) — two review rounds were triggered by the
  coordinator's own visual findings (the "6.667" port field).
- **One fix commit for the final review's findings** (not per-finding fixers);
  fix subagents get exact file/line/change specs and re-run covering tests.
- **Model tiering held**: sonnet implementers throughout, haiku for the
  fully-specified one-file fix, opus for concurrency/lift reviews, top model
  for the whole-branch review.
- **The `.serialized` suite tree + ledger discipline** — unchanged, binding.

## Environment quirks (updated)

- **1Password SSH signing relocks mid-session** ("failed to fill whole
  buffer"); unsigned fallback (`git commit --no-gpg-sign`, note it) is
  authorized for WORK commits only, never merges.
- `screencapture` needs `-D 1` on this machine (plain `-x` fails). Launch the
  app with args via direct binary invocation
  (`ComicChat.app/Contents/MacOS/ComicChat --replay-fixture <path> &`);
  `open --args` also works. Synthetic keystrokes need Accessibility grants the
  agent doesn't have — typed input is Tim's job (matches §8's manual framing).
- **XcodeBuildMCP macOS/scaffolding workflows are NOT enabled** — `xcodebuild`
  via Bash works fine (scheme is shared). New .swift files in
  `macos/ComicChat/ComicChat/` join the target automatically (synchronized
  groups) — no pbxproj edits.
- `swift test` from `macos/ComicChatKit/` is the truth; SourceKit "No such
  module: cchat_engine / Testing" diagnostics are FALSE. Engine module is
  `cchat_engine`. Test files use FIVE `deletingLastPathComponent()` or
  `Bundle.module` (both patterns in-tree).
- **Engine rules that bit people in 4a** (all documented in code now): ALL
  `cc_*` on ONE serial queue (inject `engineQueue` into ProtocolSession; never
  strip work inside `on_event`; `performOnEngineQueue` traps on-queue); metrics
  canvas installed BEFORE `cc_strip_create` and kept alive;
  `cc_session_config` extensions are append-only; reentrant `cc_session_*`
  fns SAVE/RESTORE `g_session`; `session.room()` self-syncs (don't call it
  on-queue); strip geometry set on FRESH strips only.
- The Wine rig (`.superpowers/rig/`, `./run-rig.sh`) is unchanged and ready;
  `capture-proxy.ts` takes arbitrary targets (Topology B = point it at
  crypthome). Wine needs the quarantine-stripped clone at
  `~/Applications/Wine Stable.app`.
- SDD scratch + ledger in `.superpowers/` (git-ignored). All of Plans 1–4a is
  in `.superpowers/sdd/progress.md`.
