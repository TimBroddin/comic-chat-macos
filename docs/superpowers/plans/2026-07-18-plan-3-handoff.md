# Handoff: start Plan 3 (protocol — IRC/MS Chat + annotations)

**For the next session.** Read this first, then the documents below. The
goal of Plan 3 is the protocol layer: lift `ircproto.cpp`'s parse/build logic
and the annotation codec, with Swift owning the socket and event loop (spec
§4.4 — bytes in, events out).

## Read in this order

1. `docs/superpowers/plans/2026-07-17-macos-port-roadmap.md` — **Plan 3
   section** (lift list, knowledge captured, discovery tasks, entry debt from
   Plan 2) **and** the "Plan 2 — DONE" section just above it (the roadmap
   corrections recorded there — dead-code list, `textpose.cpp`/`panel.cpp`
   findings, the `format.cpp` de-risking note, and the full debt-to-Plan-3
   list — are load-bearing context, not just history).
2. `docs/superpowers/plans/2026-07-17-macos-port-plan-1-engine-foundation.md`
   §Edit Rules for **R1–R13** and
   `docs/superpowers/plans/2026-07-17-macos-port-plan-2-layout-canvas.md`
   §Edit Rules for **R14–R17** (including the R14(v) aura amendment and R15
   instance 2) — together these are the single authoritative policy for
   modifying lifted files. Extend via plan amendment (R18+), never improvise,
   never fork the table.
3. `docs/superpowers/specs/2026-07-17-macos-port-design.md` §4.4 (network
   boundary: engine never opens sockets, Swift `NWConnection` feeds bytes in,
   engine emits outbound buffers + typed events via C callbacks, timers
   requested by engine/scheduled by Swift) and §8.2 (test approach: record
   real sessions with the Windows 2.5 client under Wine against a local ircd;
   replay captured bytes; byte-compare re-encoded annotations).

## Current state (verified 2026-07-18)

- Branch `macos-port` (kept unmerged from `main` by choice), HEAD `21448f2`.
- `swift test` from `macos/ComicChatKit/`: **13/13 green** (verified twice,
  clean `rm -rf .build && swift test`).
- **Plan 2's exit milestone is achieved and stands as the current
  end-to-end proof:** the `cc_strip` API (`comicchat.h`) renders a scripted
  two-avatar, 4-line conversation to a PNG via the CoreGraphics canvas —
  readable Comic Sans MS text, correct balloon tails, camera-driven avatar
  facing/placement, backdrop — visually verified by the coordinator and by
  Tim. `swift run cc-dumpart --strip out.png` drives it from the CLI.
  Recording-canvas snapshot tests (both the C++ and Swift recorders, proven
  byte-identical to each other) are green and frozen.
- All 32 art files in `v2.5-beta-1-modern/comicart/` still parse via the
  golden catalog test (unchanged since Plan 1).
- `v2.5-beta-1-modern/` is read-only reference. Never edit it.

## Plan 3 process (same shape as Plans 1–2)

Spec is approved — do NOT re-brainstorm. Flow: run the discovery tasks below
→ invoke `superpowers:writing-plans` to write Plan 3 (bite-sized TDD tasks,
complete code, Edit-Rules table carried forward, amendments as R18+) → user
reviews → execute via `superpowers:subagent-driven-development` (fresh
implementer per task, task-scoped reviewer, fix loops, final whole-branch
review).

**Discovery tasks (from the roadmap's Plan 3 section — run these before
designing anything, same discipline as Plan 2's canvas-inventory-first
rule):**
1. **The exact parse/side-effect split inside `ircproto.cpp`** (1457 lines,
   low Win32 density — but the roadmap's density grep found 17 UI-ish call
   sites that need dispositioning individually, the same way Plan 2's
   discovery separated `textpose.cpp` from the dead `wmini.cpp`/`script.cpp`).
2. **Set up the Wine capture rig EARLY — it gates the test layer.** Spec
   §8.2's test approach requires real captured sessions (Windows 2.5 client
   under Wine against a local ircd, byte-compared re-encoded annotations).
   Do not treat this as an afterthought late in the plan; every protocol
   task's tests depend on having real bytes to replay. Stand this up as the
   first discovery task, not the last.
3. **Which MS Chat/IRCX commands the 2.5 client actually emits** — the
   original protocol code is permissive (spec §7); confirm the actual command
   surface before committing to a parse table.
4. **Where nick/channel state lives** — `chatsrv.cpp` vs `ircproto.cpp` —
   this determines what the bridge event model needs to track vs. what it
   can treat as engine-internal.

**Known seams already mapped (roadmap "Knowledge captured," still valid):**
`chatprot.h`'s `CRoomInfo` keeps annotations as a separate parameter from
message text (`bChatSendToChannel(const char* szAnnotations, const char*
szMesg, ...)`) — the bridge event model can mirror this shape directly.
CTCP-style low-level quoting exists (`bLowLevelQuoting`, `g_chLLQuoteCTCP`,
`ircproto.cpp:497`); byte-compare tests must account for the quote-escaping.
`ircsock.cpp` (`CAsyncSocket`) is replaced wholesale by Swift `NWConnection`
+ a thin framing layer — only `ircproto`'s parse/build logic lifts. Avatar
announcement/download (`ChatAnnounceNewAvatar`, `webreq.cpp`/wininet) →
Swift `URLSession`; the URL-handshake protocol logic stays in C++.

**Entry debt from Plan 2 (do this as entry work, same pattern as Plan 2's own
entry-debt Task 1) — full detail in the roadmap's "Plan 2 — DONE" section:**
- `intl.c` stubs (`GetMime`/`iBytesofChar`/`FindSubStringForINTLThatFits` in
  `engine/cc_link_stubs.cpp`) — delete when `intl.c` lifts, or when the
  CP-1252-only posture (spec §4.5) is made permanent (may be a documentation
  change rather than a lift).
- `GetQualifiedName`/`GetMyNickName` R12(b) trap stubs — delete when
  `userinfo.cpp` lifts.
- **Capitalize R11 no-op is a LATENT REAL-METRICS LAYOUT DIVERGENCE** —
  invisible under the deterministic fake-metrics recording canvas, real once
  actual text metrics are in play. Plan 3's NLS work must restore this
  verbatim, not leave it stubbed.
- **Confirm the wire format never serializes raw `talkTos` DWORDs** — Task
  8's pointer-truncation ruling (DWORD-space compares truncate; the one
  dereference site recovers via `ccUserFromTalkTo` session-table reverse
  lookup) depends on this. Verify against `ircproto.cpp`/`chatprot.h` before
  touching anything that serializes user references.
- The `CharNext` R9 shim member (added Task 5, compiled out under `CC_NO_UI`)
  needs a real selftest once `CC_NO_UI` shrinks far enough to reach it.
- `format.cpp` is **partially lifted**: the formatting/measurement half is
  live (Task 5); the protocol annotation codec half plus `protsupp.cpp` is
  Plan 3's discovery-and-lift work. This turned out cleaner than spec §9
  feared — no entanglement found in the half already lifted — so budget the
  discovery task, but expect it smaller than originally risked.
- `CC_NO_PROTOCOL` define (Package.swift) — remove it in Plan 3;
  `EmotionToBytes`/`BytesToEmotion` in `avatario.cpp` re-enable then.

## Techniques that worked in Plans 1–2 (keep using them)

- **Fidelity diff in every lift review:** the reviewer diffs each lifted file
  against its original and demands every hunk be rule-attributable.
  Unattributable rewrites = Critical. This caught real issues in Plan 2 too
  (e.g. Task 7's benign-but-disclosed method reordering under the `CC_NO_UI`
  split).
- **Implementer escalation:** BLOCKED/NEEDS_CONTEXT beats improvising. Plan 2
  grew the Edit Rules table from R14 to R17 (plus the R14(v) aura amendment
  and a second R15 instance) out of exactly this discipline — roughly five
  escalations across Tasks 2, 6, 7, 8, and 10 turned into binding plan
  amendments rather than silent implementer judgment calls. Two of those
  escalations surfaced **Critical** findings during review, and in one case
  (Task 11's 20×-too-small balloon text) **the reviewer's own prescribed fix
  was itself defective** (a `textMatrix` approach that collapsed glyph
  advances) — the implementer reproduced the defect and shipped a different,
  correct fix instead of complying blind. Lesson: review output is a
  hypothesis to verify, not an instruction to execute unquestioned, in both
  directions (implementer-on-implementer and implementer-on-reviewer).
- **Trap-stub-first for link errors** (R12): stub all missing symbols, run
  tests, promote to verbatim single-function lifts only what actually traps.
  Plan 2 used this repeatedly (arc.cpp's `DrawArc2`/`DashArc2`, the panel.h
  static data members via `lifted_singles.cpp`, `Establishing()`/
  `g_bNewedPanel`).
- **Characterization tests with hand-verified arithmetic.** Every
  non-obvious numeric result (balloon line-breaks, bbox dimensions, camera
  placement/flipping, textpose emotion-rule firing) was characterized once,
  hand-traced against the algorithm and the deterministic fake metrics, then
  frozen with the arithmetic documented in a comment. Do the same for
  protocol parsing: freeze expected event streams for captured byte
  sequences, with the parse logic cited line-by-line in the comment.
- **The serialized-suite pattern for global-state tests** (new in Plan 2,
  Task 11's Concern #1): Swift Testing's `.serialized` only serializes tests
  *within* a suite — two separate top-level `.serialized` suites still run in
  parallel and *will* race if both touch process-global engine state (this
  was caught the hard way: a Swift metrics canvas raced the C selftest's own
  metrics canvas and corrupted both). Fix: nest every new global-state test
  suite inside the existing `.serialized` ancestor (`EngineGlobalStateSelfTests`)
  so all such tests share one timeline. Protocol tests will very likely touch
  engine-global session/user-table state — join the same suite tree, don't
  start a new one.

## Environment quirks

- Commits are SSH-signed via 1Password; if `ssh-add -l` shows no identities,
  commits fail — ask Tim to unlock, never bypass signing without explicit
  authorization. When authorized, fall back to `git commit --no-gpg-sign` and
  say so plainly in the commit record. **Known live issue:** the 1Password
  signing agent intermittently fails mid-session with "failed to fill whole
  buffer" / "agent returned an error" even when unlocked — this is not
  necessarily an unlock problem, it can recur task-to-task. **Unsigned
  range:** commits `d1da855..21448f2` (Tasks 6 through 11, 13 commits) on
  this branch are UNSIGNED — verified directly against the raw commit
  objects (`git cat-file commit <sha> | grep gpgsig`; `git log --format=%G?`
  is unreliable in this environment because `gpg.ssh.allowedSignersFile`
  isn't configured for verification, it always reports `N`). This range
  could be re-signed (interactive rebase + re-sign) before any eventual
  merge to `main`, but has not been.
- Swift test files resolve the repo root with **FIVE**
  `deletingLastPathComponent()` calls from `#filePath` (established Plan 1,
  confirmed still correct through Plan 2 — count them if you add a new test
  file, one is easy to lose).
- The C++ target's module name in Swift is `cchat_engine` (hyphen →
  underscore). SourceKit shows false "No such module" diagnostics for SwiftPM
  files; `swift test` is the truth, not the IDE's live diagnostics.
- **Engine single-threaded contract + deterministic RNG** (Plan 2, binding
  since Task 10): the engine uses process-global mutable state (avatar
  registry, session, font statics, `s_composingPage`) with **no internal
  locking** — documented in `comicchat.h`; all `cc_*` calls must originate
  from one thread at a time. `cc_strip_create` seeds `srand(0x5EED)`
  deterministically so identical scripts yield identical output — this is
  engine-owned determinism, not a test-only scaffold; protocol-driven
  sessions that touch layout inherit the same contract.
- **The recording-canvas log grammar is a stable contract**, documented in
  `macos/ComicChatKit/Sources/cchat-engine/bridge/cc_recording_canvas.h` (and
  mirrored in `comicchat.h`'s `cc_canvas_ops` doc comment): exact per-op log
  line formats (`text`/`rect`/`image`/`path`/`clip+`/`clip-`), and critically,
  **`clip_pop` is a full RESET to the unclipped base state, not a balanced
  one-level pop of the matching `clip_push`** — the engine only ever emits a
  clip_pop to discard the *entire* accumulated clip in one call. A canvas
  that implements it as "undo the last push" will silently corrupt every
  panel after the first. Both the C++ and Swift recording canvases proved
  byte-identical against this grammar in Task 11 — if protocol work adds any
  new canvas consumer (unlikely, but if annotation rendering ever touches
  layout), preserve the grammar exactly.
- SDD scratch (briefs/reports/ledger) lives in `.superpowers/sdd/`
  (git-ignored). Plan 1 and Plan 2's ledgers are both there if history is
  needed (`.superpowers/sdd/progress.md` has both plans' sections).
