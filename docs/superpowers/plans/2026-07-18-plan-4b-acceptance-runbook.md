# Plan 4b Task 13 — §8 live acceptance runbook

Everything needed for the human-driven live acceptance session (Task 13,
Tim at the keyboard) is staged here. This runbook is pure driving: the rig
scripts, the ordered checklist, and the artifact/promotion steps. The
checklist below is copied **verbatim** from
`docs/superpowers/plans/2026-07-18-plan4-discovery/live-interop.md` §6.

## 0. Prerequisites — status

All four build items §8 blocked on are DELIVERED:

| # | Item | Delivered by |
|---|---|---|
| P1 | Swift connect sequencing: probe → 451-fallback/800-pivot → NICK/USER → `CC_EV_LOGGED_IN` | Plan 4a Task 2 (`cc_session_login`, byte-verified against `smoke-2.jsonl`) |
| P2 | `cc_strip` avatar assignment/refresh + `ProtocolStripBridge` wiring so a post-create `.appearsAs` re-renders | Plan 4a Task 6 (`cc_strip_set_participant_avatar` + `cc_avatar_icon_image`) |
| P3 | Outbound `# Appears as` announce on self-join / character change | Plan 4a Task 8 (`cc_session_announce_avatar`) |
| P4 | Keep all test text ASCII until the CP-1252 case-fold debt is paid | Plan 4a Task 3 (case-fold table + escaped-byte annotation vector). 4b Task 1 additionally made accented text SAFE outbound, but **acceptance text stays ASCII anyway** per D4 risk 7 — this isn't the moment to also validate encoding edges. |

No other build item blocks §8. The app is feature-complete through 4b Task 11
(multi-room, wheel, whispers, pickers, downloads, room ops, sounds,
save/print, text view).

## 1. Expected-silence notes (documented non-goals — do not treat as bugs)

- **CTCP auto-replies are not implemented.** A Windows user clicking Get
  Version / Ping / Get Profile / Get Character on us gets silence — their
  client just shows no result (ping shows no time). This is intentional
  (D4 §5): no acceptance assertion depends on it.
- **Custom-avatar publishing is deferred.** We fetch/render *their*
  announced avatars (including custom ones with URLs), but we only announce
  *ours* by bundled character name, never a URL — `# GetCharInfo` requests
  from peers about us are swallowed with no reply. This is the documented
  one-way avatar interop posture (D4 §4); not a defect to chase during
  acceptance.
- **Accented/CP-1252 text is out of scope for this session** (P4 above) —
  keep all typed test text ASCII even though 4b Task 1 made the outbound
  path safer for it. Encoding edges are a follow-up, not an acceptance gate.

## 2. Watch-for-live items (known carryover flags — not new work, just observe)

- **`addLineCooked` measurement-path crash reports (4b Task 3 flag).** Two
  stale crash reports were found during Task 3 development
  (`ComicChat-2026-07-18-21{02,05}*.ips`, SIGTRAP/assertion in
  `CoreTextMetrics.measure` → `CLabel::WidestWord()` reached via
  `cc_strip_add_line_cooked` → `CUnitPanel::LayoutBalloon` →
  `CCanvas::measure_text`). They predate any 4b build and were not
  reproducible at the time, but the failure mode (a premature `CanvasBox`
  dealloc surfacing the next time ANY strip measures text — see
  `ChatSessionModel.swift`'s `metricsCanvasBox` doc comment) is exactly the
  kind of thing a real multi-hour live session with real message volume
  could reproduce. **Watch Console.app / `~/Library/Diagnostic Reports`
  during acceptance; if a crash occurs, capture the `.ips` file as an
  artifact alongside the JSONL captures.**
- **Own-say echo dedup — record per server.** The app renders its own sent
  line synthetically and immediately (most IRCds don't echo `PRIVMSG` back
  to sender), registering the sent text in a short-lived
  `pendingLocalEchoes` list; if the server DOES echo, the matching text is
  dropped once on arrival (`ChatSessionModel.swift` `send(_:)` /
  `handleLocked`). This ordering argument (append-async vs. wire
  round-trip) has only been exercised against non-echoing test doubles.
  **For each topology/server used in this session, note explicitly whether
  it echoes our own `PRIVMSG` back, and whether the dedup worked cleanly
  (no visible duplicate line) or not.** This is the first real-server trial
  of that code path.

## 3. Rig scripts

All three live in `.superpowers/rig/`, executable, mirroring `run-rig.sh`'s
conventions (background processes, `stop` argument, `pkill -f` teardown).

- **`probe-servers.sh`** — the sanctioned 2-line reachability check (`MODE
  ISIRCX\r\nQUIT\r\n` over raw `nc`, no NICK/USER, no join). Run once the
  morning of the acceptance session (spec §9 flags live-server availability
  drift as a risk). Expected: crypthome → `800 * 0 0 ANON 512 *` (full
  IRCX); dedoky → `451 ...` (plain-IRC fallback trigger). Prints PASS/FAIL
  per server.
- **`run-topology-a.sh`** — local plain-IRC (primary). Starts the existing
  rig (ngircd `:6667` + win-side proxy `:6668` + Wine client into
  `#comicrig`) plus a second, independent proxy on `:6669` for the Mac app.
  `run-topology-a.sh proxy` starts ircd + both proxies without the Wine
  client (drive it yourself); `run-topology-a.sh stop` tears everything
  down. Captures land in `.superpowers/rig/captures/p4b-a-mac.jsonl` (Mac
  side) and the standard `run-rig.sh` capture path (Windows side).
- **`run-topology-b.sh`** — live IRCX against `www.crypthome.com`. Dual
  proxies (`:6668` and `:6669`, both targeting crypthome directly) each get
  their own capture file (`captures/p4b-b-win.jsonl`,
  `captures/p4b-b-mac.jsonl`). Launches the Wine client into `#cctest`
  through its proxy; prints the etiquette preamble on every run (see §4
  below — it's also baked into the script's own banner, not just this
  doc).

## 4. Public-server etiquette (BINDING)

crypthome (`www.crypthome.com`) is a small hobbyist IRCX server — **max 10
local users, run as a memorial box for the Comic Chat community.** This is
not a throwaway test target; be a good guest.

- **Scratch room: `#cctest`.** Never run test/acceptance traffic in
  `#Crypt`.
- **Say hello in `#Crypt`** if people are present, and mention you're
  testing the new open-source Mac port — it's more likely to draw help than
  complaints, especially post-open-source-announcement.
- **Human-driven only.** Every message, pose, whisper, and avatar change
  sent during Topology B must be typed by a human (Tim) in the moment.
  Nothing in this repo's scripts sends chat traffic automatically — the
  rig scripts only open proxy pipes and launch clients.
- **Keep the session short.**
- dedoky (`comic.dedoky.com`) and koach are more broadly Comic-Chat-
  friendly per the community server list, but the same spirit applies if
  either is used as a fallback (§6 below): own scratch room, keep it short,
  don't script traffic.

## 5. Ordered checklist (VERBATIM from D4 §6 — run once per topology, A then B)

> Copied from
> `docs/superpowers/plans/2026-07-18-plan4-discovery/live-interop.md` §6,
> "Ordered checklist (run once per topology; A then B)".

1. **Setup:** start rig per §3; Windows client into the room; Mac app
   connect → assert probe visible in capture, correct login path (A: `MODE
   ISIRCX`→`451`→`NICK/USER`; B: `800`→`IRCX`→`800`→`NICK/USER`→`001`), join.
2. **Presence:** both member lists show both nicks; JOIN echoes in both
   captures.
3. **Avatar announce, ours→theirs:** Mac announces a bundled character
   (e.g. Anna); Windows member list/comic shows us as that character.
4. **Avatar announce, theirs→ours:** Windows side picks/changes character;
   our app receives `.appearsAs` and the strip renders their character
   (exercises P2). Also observe their automatic private `"?"` reply-announce
   in the capture (`protsupp.cpp:868-877` behavior).
5. **Pose, ours→theirs:** send a say with explicit gesture/expression; their
   comic renders our chosen pose; capture shows `(#G…E…M…)` inline (A) or
   `DATA …CCUDI1` + PRIVMSG pair (B).
6. **Pose, theirs→ours:** Windows sends emotion-wheel poses; our strip
   renders the cooked pose (not text-inferred); on B this validates Swift's
   DATA→PRIVMSG re-pairing against real interleaving.
7. **Whisper both directions:** Windows→Mac (whisper rendering/box on ours,
   `CC_EV_WHISPER`); Mac→ours as `PRIVMSG <nick>` with whisper-mode
   annotations, rendered as whisper on Windows. Multi-addressee `T<nick>`
   variant once.
8. **Think + action modes** one message each, both directions.
9. **Sound (stretch):** Windows sends `/sound` → our `CC_EV_SOUND` fires
   (play if wired, log otherwise). Outbound sound only if Plan 4 wires
   `BM_SOUND` composition — not an acceptance gate.
10. **Expected-silence checks (documented non-goals):** Windows-side Get
    Version/Ping/Profile/Get Character on us produce no reply (§5);
    accented text out of scope (P4).
11. **Artifacts:** both JSONL captures archived; screenshots of both comic
    panes at steps 3-7; `cc-dumpart --replay` of the Mac-side capture
    reproduces the strip (ties live run to the deterministic test layer);
    promote an annotated + escaped-byte exchange from the capture into the
    §8.2 fixture corpus.

### Topology A addendum: local multi-room sanity (LOCAL ONLY)

Appended to Topology A's run (never on the public boxes in Topology B):
after step 2 (or any time convenient), also join a **second** room on the
local rig (e.g. `#comicrig2`) from the Mac app and tab-switch between the
two room tabs. Verify both strips render correctly on switch — this is
Task 7's true-multi-room live validation, which unit/fixture tests can't
fully exercise against a real server's JOIN/PART/NAMES sequencing. This
step is baked into `run-topology-a.sh`'s own startup banner as a reminder.

## 6. Server choice / fallback order

Primary = **Topology A** (local ngircd, deterministic, fully captured).
Secondary = **Topology B** (`www.crypthome.com:6667`, live IRCX — 800
pivot, ANON login, DATA transport). Fallback if crypthome is unreachable
the day of acceptance = `comic.dedoky.com:6667` (live plain-IRC community
server) — mind **`NICKLEN=9`**: pick a nick that is 9 characters or fewer
before connecting, since the server truncates/rejects longer ones.

**IRC7-in-Docker contingency (ONLY if the crypthome probe fails on the
day):** build a local deterministic-IRCX understudy from
[github.com/irc7-com/irc7](https://github.com/irc7-com/irc7)'s Dockerfile.
Before substituting it as Topology B, **smoke-test that its
pre-registration `MODE ISIRCX` answers `800` or `451`** (not `421` or
silence — there is no `421` handler in the lifted parser, so an
unexpected reply stalls the client the full 50s fallback timer; D4 risk
4). If IRC7 answers anything other than `800`/`451` pre-registration, do
not use it for Topology B — fall back to dedoky instead and note the
discrepancy for a future IRC7 investigation.

## 7. Artifact locations

- **Byte captures:** `.superpowers/rig/captures/p4b-*.jsonl` (`p4b-a-mac`,
  and the Windows-side file from `run-rig.sh`'s own `session-<epoch>.jsonl`
  naming for Topology A; `p4b-b-win`/`p4b-b-mac` for Topology B).
- **Screenshots:** `.superpowers/sdd/p4b-acceptance/` (create if absent) —
  both comic panes side by side at checklist steps 3-7, one pair per
  topology at minimum.
- **Crash reports (if any):** copy the `.ips` file(s) into
  `.superpowers/sdd/p4b-acceptance/` alongside the screenshots (see §2).

### `cc-dumpart --replay` reproduction step

From `macos/ComicChatKit/`:

```sh
swift run cc-dumpart --replay <path-to-mac-side-capture.jsonl> <out.png>
```

Run this against the Mac-side capture from **each** topology after the
live session. It replays the captured `s2c` bytes through the same
`ProtocolSession` + `ProtocolStripBridge` path the live app used and
renders the resulting strip to a PNG — confirming the deterministic engine
reproduces the same strip from the same bytes the live session actually
saw. This ties the live run to the existing deterministic test layer and
is also how the fixture-promotion step (below) gets its PNG for visual
sanity-check before a capture is committed as a test fixture.

### Fixture-promotion procedure

Promote **three** exchanges from this session's captures into
`macos/ComicChatKit/Tests/ComicChatKitTests/Fixtures/captures/`:

1. **An annotated exchange** — a captured `s2c` chunk carrying a cooked
   pose annotation (inline `(#G…E…M…)` from Topology A, or a `DATA
   …CCUDI1` + PRIVMSG pair from Topology B).
2. **An escaped-byte annotation exchange** — a captured exchange that hits
   the escaped-byte annotation vector (Plan 3's handoff wanted this
   specifically; watch for it in the raw hex during steps 5-6, especially
   on Topology B's DATA transport).
3. **A real avatar-announce exchange** — a captured `# Appears as …`
   exchange (steps 3-4), replacing the currently HAND-AUTHORED equivalent.

For each: trim the source JSONL down to the relevant chunk(s) (same format
as the existing fixtures — one JSON object per line, `hex` authoritative),
name it descriptively (e.g. `crypthome-pose-data-real.jsonl`), and add it
to `Fixtures/captures/`. **Retire the HAND-AUTHORED marker note** on
`hand-authored-annotation.jsonl` (the `*** HAND-AUTHORED FIXTURE, NOT A
REAL CAPTURE ***` comment in `ProtocolCodecTests.swift`) once a real
captured annotation exchange from this session covers the same assertion
— either by pointing that test at the new real fixture, or by adding a
parallel test against the real one and noting in the hand-authored file's
own header that a real capture now also exists. Don't delete the
hand-authored fixture outright unless the new one is a strict superset of
what it exercises.

## 8. Dry-run status (Task 12 — see report for full evidence)

Topology A was dry-run at the byte level with a scripted stand-in for the
Mac app (the coordinator's session had a locked screen — see the Task 12
report, `.superpowers/sdd/p4b-task-12-report.md`, for exactly what was and
wasn't verified). The app-side visual verification (does the real app
connect, join, and render the Wine client's presence on screen) is
deferred to this session — do it as checklist step 1-2 naturally, no
separate action needed.
