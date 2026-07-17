# Handoff: start Plan 2 (comic layout engine + Canvas)

**For the next session.** Read this first, then the three documents below. The
goal of Plan 2 is the comic layout engine rendering through an abstract Canvas
— exit milestone: a scripted two-avatar conversation rendered to PNG.

## Read in this order

1. `docs/superpowers/plans/2026-07-17-macos-port-roadmap.md` — Plan 2 section:
   lift list, dependency map, design commitments, discovery tasks, **and the
   "Debt handed over by Plan 1" list (entry work — do it first)**.
2. `docs/superpowers/plans/2026-07-17-macos-port-plan-1-engine-foundation.md` —
   for the **Edit Rules R1–R13** (the authoritative policy for modifying lifted
   files; extend via plan amendment, never improvise) and the task format.
3. `docs/superpowers/specs/2026-07-17-macos-port-design.md` — §4.3 (Canvas
   boundary) binds Plan 2's design.

## Current state (verified 2026-07-17)

- Branch `macos-port` (kept unmerged from `main` by choice), HEAD `485fba0`.
- `swift test` from `macos/ComicChatKit/`: **7/7 green**. `cc-dumpart` works
  (catalog + `--png` pose export; visually verified).
- All 32 art files in `v2.5-beta-1-modern/comicart/` parse via the lifted
  original code. 20 avatars have 14–30+ poses; glenda/pedagog/rainbow/tux/waf
  legitimately have 1 pose (AK_NBODIES=1). Golden catalog committed.
- `v2.5-beta-1-modern/` is read-only reference. Never edit it.

## Plan 2 process (same as Plan 1)

Spec is approved — do NOT re-brainstorm. Flow: run the discovery tasks from
the roadmap → invoke `superpowers:writing-plans` to write Plan 2 (bite-sized
TDD tasks, complete code, Edit-Rules table carried forward) → user reviews →
execute via `superpowers:subagent-driven-development` (fresh implementer per
task, task-scoped reviewer, fix loops, final whole-branch review).

Discovery starter — the Canvas method list IS this inventory, don't design
Canvas before running it:

```bash
grep -n -A2 "ifndef CC_NO_RENDER" \
  macos/ComicChatKit/Sources/cchat-engine/engine/*.cpp
```

Also inventory the 13 trap stubs in
`macos/ComicChatKit/Sources/cchat-engine/engine/cc_link_stubs.cpp` — each is
tagged with its owning file (`bodycam.cpp`/`panel.cpp`/`wmini.cpp`/
`balloon.cpp`); Plan 2 deletes each stub as it lifts the owner.

## Techniques that worked in Plan 1 (keep using them)

- **Fidelity diff in every lift review:** the reviewer diffs each lifted file
  against its original and demands every hunk be rule-attributable.
  Unattributable rewrites = Critical.
- **Implementer escalation:** BLOCKED/NEEDS_CONTEXT beats improvising. Four
  plan errors were caught this way in Plan 1; rules R10–R13 exist because of
  them.
- **Trap-stub-first for link errors** (R12): stub all missing symbols, run
  tests, promote to verbatim single-function lifts only what actually traps.
- **Engine-side C++ tests** go in `bridge/cc_selftest.cpp` behind
  `cc_run_selftests()`; Swift Testing for bridge-and-up. Every R9 shim
  addition needs a selftest (task reviews enforce this).

## Environment quirks

- Commits are SSH-signed via 1Password; if `ssh-add -l` shows no identities,
  commits fail — ask Tim to unlock, never bypass signing.
- Swift test files resolve the repo root with FIVE
  `deletingLastPathComponent()` calls from `#filePath`.
- The C++ target's module name in Swift is `cchat_engine` (hyphen →
  underscore). SourceKit shows false "No such module" diagnostics for SwiftPM
  files; `swift test` is the truth.
- SDD scratch (briefs/reports/ledger) lives in `.superpowers/sdd/`
  (git-ignored). Plan 1's ledger is there if history is needed.
