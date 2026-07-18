# Plan 4 Discovery — live-interop acceptance (spec §8)

**Verdict: the §8 live acceptance is runnable today, on two levels.** The
plain-IRC level (poses, avatars-by-name, whispers, sounds — both directions)
needs nothing beyond the existing Wine rig and a local ngircd; the IRCX level
has a **live, verified-compatible hobbyist IRCX server on the public internet
right now** (`www.crypthome.com:6667`, an OfficeIRC box that answered the
`MODE ISIRCX` probe with the exact `800` pivot our lifted state machine
implements, relays the `DATA … CCUDI1` annotation verb verbatim, and allows
ANON login — no AUTH needed, matching our AUTH-unsupported-by-design posture).
The one hard prerequisite on our side: **Swift's plain-IRC login sequencing
(probe → 451/800 → NICK/USER → 001) is not yet written** — the engine
deliberately leaves PASS/NICK/USER emission to Swift and no Swift code sends
it yet. That, plus the known `cc_strip` change-avatar gap, are the two build
items §8 acceptance actually blocks on.

Probes in §1 were performed live on 2026-07-18 from this machine (raw `nc`,
anonymous, no room joins beyond verb existence checks, immediate `QUIT`).
**VERIFIED** = bytes seen / page fetched / code read this session.
**REPORTED** = search-result or README claim not independently exercised.

---

## 1. The 2026 server landscape

Context that changes the etiquette calculus: Microsoft **open-sourced Comic
Chat on 2026-07-16** (this repo — [blog](https://opensource.microsoft.com/blog/2026/07/16/microsoft-comic-chat-is-now-open-source/),
[project page](https://microsoft.github.io/comic-chat/), press at
[The New Stack](https://thenewstack.io/microsoft-comic-chat-open-source/),
[Windows Central](https://www.windowscentral.com/microsoft/windows-11/microsoft-comic-chat-an-irc-client-from-30-years-ago-that-helped-popularize-comic-sans-is-going-open-source),
[Phoronix](https://www.phoronix.com/news/Microsoft-Comic-Chat-OSS)). The
hobbyist servers below exist *for* Comic Chat users and are likely seeing a
nostalgia bump; a Mac-port interop test is exactly the traffic they welcome.
Microsoft's own chat servers closed 2001-02-21; the canonical community server
list is Mermaid Elizabeth's page at
[mermeliz.com/srvr_rms.htm](https://mermeliz.com/srvr_rms.htm) (VERIFIED
fetched; all three networks below come from it).

### 1.1 Public servers (all three probed live, 2026-07-18)

| Server | Address:port | Software (from wire) | Protocol level | Probe result | Status |
|---|---|---|---|---|---|
| **The Crypt** | `www.crypthome.com:6667` | OfficeIRC `oirc-3.0.102` (`belle2000.local`) | **Full IRCX** | `MODE ISIRCX` → `800 * 0 0 ANON 512 *`; `IRCX` → second `800 * 1 …`; plain `NICK`/`USER` → `001` (ANON, no AUTH); `005` advertises `IRCX CHANTYPES=%#&`; `DATA <nick> CCUDI1 :#G…` **relayed verbatim with full prefix**; `WHISPER`/`PROP` verbs recognized (403/461, not 421) | **VERIFIED live + protocol-compatible** |
| **Comic Chat Network** | `comic.dedoky.com:6667` | ngIRCd 27 (`irc.dedoky.com`, aarch64 Linux, "DedokyIRC") | Plain IRC | `MODE ISIRCX` → `451 :Connection not registered` — the exact plain-IRC fallback trigger; registration works; **`NICKLEN=9`**, `CHARSET=UTF-8` advertised | **VERIFIED live** |
| **Koach.com** | `chat1`–`chat4.koach.com:6667` | not identified (modern ircd; hostname-lookup NOTICEs) | Plain IRC (assumed) | `MODE ISIRCX` → `451 … :You must be fully connected` — still a 451, so fallback engages | **VERIFIED reachable** (registration not completed) |

Room conventions per mermeliz: `#Crypt` (Crypt, also `mic://` capable —
MIC protocol), `#garage` (dedoky), `#koachsworkshop` (koach).

**Etiquette.** All three are small hobbyist boxes run for the Comic Chat
community. Crypt is the smallest (max 10 local users, 5 online during the
probe; it is a memorial server) — use a scratch room (`#cctest`-style), say
hello in the main room if people are present, keep sessions short, and don't
script traffic at it. Dedoky ("Comic Chat Network") and koach are explicitly
Comic-Chat-friendly per the community list; polite anonymous testing in an own
scratch room is within the social contract, and announcing "testing the new
open-source Mac port" will more likely draw help than complaints. The 2-line
`MODE ISIRCX` reachability probes done for this report are harmless; a full
acceptance session should be human-driven, not automated.

### 1.2 Self-hostable IRCX server software (local fallback / deterministic IRCX)

| Project | Language / license | IRCX verb surface | Build status | Assessment |
|---|---|---|---|---|
| **IRC7** — [github.com/irc7-com/irc7](https://github.com/irc7-com/irc7) | C# / .NET 10, MIT | Command sources **VERIFIED present** via code search: `Commands/Isircx.cs`, `Ircx.cs`, `Auth.cs`, `Data.cs`, `Whisper.cs`, `Prop.cs`, plus `Protocols/IrcX.cs` | Active (440 commits), Dockerfiles, **no tagged releases** — build from source | Best open-source candidate; modeled on MSN Chat (an IRCX derivative). **Unverified:** whether a pre-registration `MODE ISIRCX` (our client's probe — not the bare `ISIRCX` verb) gets `800` or `451`; must be smoke-tested locally (see risk 4) |
| **pyRCX** — [github.com/cwebbtw/pyRCX](https://github.com/cwebbtw/pyRCX) | Python, Apache-2.0 | REPORTED: IRCX mode, `WHISPER`, `PROP`, `ACCESS`; **`DATA`/`AUTH`/`800` not documented** | Last release 2023, "heavy refactor" in progress | Weaker: without `DATA` the IRCX annotation transport can't be exercised |
| **OfficeIRC Server 3** — [officeirc.com](https://www.officeirc.com/Products/Server) | Commercial (.NET 10, closed source; trial download exists) | RFC1459/2812 + IRCX + IRCv3 (REPORTED) — and it is **the exact software crypthome runs**, so compatibility is transitively VERIFIED | Maintained (© 2023-2026) | Highest-fidelity self-host if a local IRCX box is wanted without building IRC7 |
| **ngircd 28** (Homebrew, already in the rig) | C, GPL | None (plain IRC; replies `451` to the probe) | In use | The plain-IRC baseline — already validated against the real 1998 client in Plan 3 |

Sources: [mermeliz server list](https://mermeliz.com/srvr_rms.htm) ·
[IRC7](https://github.com/irc7-com/irc7) · [pyRCX](https://github.com/cwebbtw/pyRCX) ·
[OfficeIRC](https://www.officeirc.com/Products/Server) ·
[IRCX background (ircv3-ideas #48)](https://github.com/ircv3/ircv3-ideas/issues/48) ·
[MS open-source announcement](https://opensource.microsoft.com/blog/2026/07/16/microsoft-comic-chat-is-now-open-source/).

---

## 2. What the engine actually needs from the server

All paths below are in the lifted tree
`macos/ComicChatKit/Sources/cchat-engine/` unless marked *(Swift)*.

**The probe and the two login paths.**
- `cc_session_probe_ircx` sends `MODE ISIRCX` and asks Swift to arm the 50 s
  timer (`bridge/cc_session.cpp:316-326`; builder string at
  `engine/ircproto.cpp:1109`; API doc `include/comicchat.h:487-502`).
- **451** → `ccModeIsIrcXFailure` (`engine/ircsock.cpp:1996-1997` dispatch,
  body at `:487-501`): dequeues the probe, cancels the timer, **emits
  nothing** — the header comment (`ircsock.cpp:476-486`) says explicitly
  "Swift observes the fallback by the absence of CC_EV_SERVER_CAPS and drives
  its own plain login (PASS/NICK/USER)".
- **800** → `RPL_IRCX` pivot (`engine/ircsock.cpp:1753-1828`): first instance
  sets `m_bIrcXServer`, parses `ANON` from the package list, grows buffers to
  the advertised max-msg-len, emits `CC_EV_SERVER_CAPS{ircx=1}`, and sends
  `IRCX`; second instance → if ANON allowed, *Swift* again does the plain
  NICK/USER login, else `CC_EV_AUTH_UNSUPPORTED` (R21).
- **Gap (hard §8 prerequisite):** no Swift code performs that login.
  `ProtocolSession` has no `probe`/`login` member and never composes
  `NICK`/`USER` bytes (grep-verified across `Sources/ComicChatKit/` and
  `Sources/cc-dumpart/`); nothing calls `cc_session_probe_ircx` outside the
  C self-tests. Plan 3's exit path replayed captured bytes, so it never
  needed to connect for real. **Plan 4 must implement connect sequencing:**
  on socket ready → `cc_session_probe_ircx` → on (451-silence ∧ no caps
  event) or on second-800 → send `NICK`/`USER` via the existing send path →
  `CC_EV_LOGGED_IN` gates the join queue (`ProtocolSession.swift:305`).
- A server that answers the probe with `421` (or nothing) is handled by
  **timeout only** — there is no 421 case in the lifted parser (grep
  confirms) — costing the full 50 s before fallback. Swift schedules the
  timer, so Plan 4 may shorten it for interactive UX; all three public
  servers above answer 451 or 800, avoiding the stall.
- Keepalive is engine-internal: inbound `PING` is answered with `PONG`
  in-parser (`engine/ircsock.cpp:1030-1033`); Swift does nothing.

**Annotation transport selection — the one behavior IRCX changes.** Outbound,
`bChatSendToTarget` picks per message: `if (*szAnnotations && IsIRCX())` →
`DATA <target> CCUDI1 :<blob>` followed by a separate bare `PRIVMSG`
(`engine/ircproto.cpp:574-591`, chunked variant `:669-688`); otherwise the
blob is parenthesized inline ahead of the text in one `PRIVMSG`.
`ccEncodeAnnotations`' `bIncludeParenthesis` mirrors the same `IsIRCX()` test
(`bridge/cc_session.cpp:339-382`, call sites `:398`, `:413`). Inbound, both
transports are handled: inline `(#…)` in `ccProcessSay`
(`engine/protsupp.cpp:1068-1110`), out-of-band `DATA CCUDI1` →
`CC_EV_DATA` (`engine/ircsock.cpp:636-668`) with the DATA→next-PRIVMSG
re-pairing done by nick in Swift (`ProtocolSession.swift:520-545`) — the
live IRCX session will be that re-pairing's first non-unit-test exercise.

**Whispers.** Outbound whispers are **always plain `PRIVMSG <nick>`** — in
the original and in the port (`bridge/cc_session.cpp:406-423` →
`bChatSendPrivMesg` → `bChatSendToTarget`; the original client never emits
the `WHISPER` verb — grep of `v2.5-beta-1-modern/` shows only `BM_WHISPER`
mode uses). The IRCX `WHISPER` verb is an *inbound* server push, handled at
`engine/ircsock.cpp:1137-1168` → `CC_EV_WHISPER`. So whisper interop works
identically on plain IRC; IRCX adds only the inbound-verb variant.

**Minor IRCX-only behaviors:** `MODE +f` (no-format) flips `CM_NOFORMAT`
(`engine/ircsock.cpp:414-418`); ban-string shape differs (`:468-474`);
extended `^A`-prefixed nicknames decode only when `IsIRCX()`
(`engine/ircproto.cpp:862,884,1322`); `PROP CLIENT` pushes (backdrop sync)
→ `CC_EV_ROOM_PROP` (`engine/ircsock.cpp:1040-1044`); `LISTX` selected over
`LIST` on IRCX (`bridge/cc_session.cpp:310`).

**Answer to the question:** §8's substance — poses, avatars, whispers, sound,
both directions — is **fully exercised on plain IRC** (annotations inline,
avatar announce as a `#`-comment over PRIVMSG, whisper as PRIVMSG). The
rig's existing ngircd (now cross-confirmed by dedoky running the *same
ircd* in production for this community) suffices for that. What IRCX-mode
*additionally* proves: the 800/`IRCX`/ANON login pivot, the out-of-band
`DATA` transport in both directions (outbound build + Swift re-pairing),
inbound `WHISPER`-verb handling, `PROP`/room-property flow, and
server-advertised message-length growth. Since a compatible live IRCX server
exists (§1.1), the acceptance should run **both modes** — plain-IRC locally
(deterministic, captured) and IRCX against crypthome (or a local IRC7 if
privacy/capacity argues for it).

---

## 3. Rig extension for the live acceptance

What the rig does today (`.superpowers/rig/run-rig.sh`, read this session):
starts ngircd on `127.0.0.1:6667` (config `ngircd.conf`), the Bun byte-logging
proxy on `:6668` (`capture-proxy.ts <listen> <host> <port> [log]` — target is
parameterized), then launches the genuine 1998 `cchat.exe` under Wine 11 with
`irc://127.0.0.1:6668/#comicrig` (URL connect, no dialog). `run-rig.sh proxy`
starts only ircd+proxy; `stop` kills everything. Captures are JSONL
`{t,dir,hex,latin1}` — `hex` is the byte-compare artifact.

**Topology A — local plain-IRC (primary).** Zero new software:

1. `./run-rig.sh` (ngircd + proxy + Windows client into `#comicrig`).
2. Start a *second* proxy instance for our side:
   `bun capture-proxy.ts 6669 127.0.0.1 6667 captures/mac-side.jsonl` —
   the script already takes listen/target/log arguments, so both clients get
   independent byte logs.
3. Mac app connects to `127.0.0.1:6669`, joins `#comicrig`.
4. Human drives both compose bars side by side.

**Topology B — live IRCX (crypthome).** The proxy is target-agnostic:
`bun capture-proxy.ts 6668 www.crypthome.com 6667 captures/win-ircx.jsonl`
and launch `cchat.exe "irc://127.0.0.1:6668/#cctest"`; second instance on
`:6669` for the Mac app the same way. Both clients' full IRCX byte streams
land in JSONL. (Local IRCX alternative: IRC7 via its Dockerfile, or an
OfficeIRC trial — smoke-test the `MODE ISIRCX` pre-registration reply first,
risk 4.)

**Observing both directions.** The Windows client's comic pane renders live
under Wine on the same screen as our app — visual verification is
side-by-side eyeballing, which §8 defines as manual anyway. Evidence:
`screencapture` of the Mac side, Wine window screenshot for the Windows side,
plus both JSONL captures. The captured Mac-side `s2c` stream can then be fed
to `swift run cc-dumpart --replay` to show the *deterministic* engine
reproduces the same strip from the same bytes — the live session doubles as
a §8.2 corpus refresh (and is the chance to finally capture the
**escaped-byte annotation vector** the Plan 3 handoff wants).

**Wine quirks (from `wine-capture-rig.md`, still current):** use the
quarantine-stripped clone at `~/Applications/Wine Stable.app` (Gatekeeper/SIP
block the `/Applications` cask); `WINEPREFIX=.superpowers/rig/wineprefix`,
`WINEDEBUG=-all`; the `irc://host:port/#room` argument bypasses the connect
dialog entirely; **synthetic keystrokes into Wine windows are unreliable and
the agent has no Screen Recording/Accessibility grants** — the acceptance
session must be human-driven (Tim types, Tim screenshots), which matches
§8's "manual acceptance" framing. Running the client binary twice gives two
Windows participants if needed.

---

## 4. Avatar announce / download reality-check

**Where the URL comes from in the original:** the `.avb` file itself. On
join (`v2.5-beta-1-modern/histent.cpp:271-281`, `JoinEntry::Execute` for
self) and on avatar change (`avatar.cpp:585-599`, `SetMyAvatar`), the client
calls `ChatAnnounceNewAvatar(GetMyCharacter(), MyAvatarURL())`;
`MyAvatarURL()` → `CAvatarX::Url()` → `m_pszNewURL ?: m_pszOriginalURL`
(`avatar.cpp:545-547`, `:955-959`), fields parsed straight out of the AVB
stream (`avbfile.cpp:878-882`, `:1837`). It is **not** a user web dir and not
a Microsoft service — character authors embedded a download URL when building
the `.avb`. The bundled 1998 roster carries **no URL** (strings-scan of the
rig client's `anna.avb`/`margaret.avb`/`mike.avb` shows only copyright), so
stock clients announce the name-only form.

**Wire grammar** (`protsupp.cpp:817-843`): `# Appears as <name>.<url>` (or
just `<name>`), sent channel-wide as the *annotations* argument — i.e. it
rides `PRIVMSG` on plain IRC and `DATA … CCUDI1` on IRCX automatically. The
receiving side (`ProcessComment`, `protsupp.cpp:846-900`): resolves `<name>`
against the **local** roster first (`histent.cpp:380-395` →
`GetAvatar3(..., randomIfNotFound)`); only an unknown name *with* a URL
enters the download state machine. First announce from a new comic user also
triggers a private reply-announce with the deferred URL `"?"`
(`protsupp.cpp:868-877`); the actual URL is later requested via
`# GetCharInfo` (`protsupp.cpp:926-939`, request sender `:3424-3430`), and
the fetch is `CChatApp::StartDownloadingAvatar` → WinInet
(`chat.cpp:2166-2249`, `webreq.cpp`), 2 MB cap, one retry
(`chat.cpp:2361-2363`).

**Unreachable URL:** benign. Auto-download failure is silent; interactive
failure shows `AvatarTransferError` (`chat.cpp:2242-2244`, `:2463`); either
way the peer keeps the stand-in character already assigned at announce time
(random local pick when the name is unknown). Crucially,
**auto-download defaults OFF** (`chat.cpp:208`,
`m_bAutoDownloadAvatars = FALSE`) — a stock 1998 client never fetches unless
the user opted in or explicitly requests the character. (The modernized
reference tree even hard-disables the fetch with a "art servers are gone"
notice, `chat.cpp:2150-2178` — but the rig runs the *genuine 1998 binary*,
which still would fetch if asked.)

**What OUR client must do for §8:**

- **(a) Fetching theirs:** engine already emits `CC_EV_APPEARS_AS`
  (`engine/protsupp.cpp:916-944`; also via DATA and WHISPER carriers,
  `engine/ircsock.cpp:667-668,977-978,1199-1200`). `ProtocolStripBridge`
  resolves announced *names* against bundled art for not-yet-seen
  participants but is a **documented no-op for existing participants**
  (`ProtocolStripBridge.swift:123-129`) because `cc_strip` has no
  change-avatar entry point — the known debt. Plan 4 needs the avatar
  assignment/refresh API for the "their announce updates our strip"
  assertion. The `URLSession` `.avb` fetch (unknown name + URL → download →
  validate → `cc_avatar_open`, size-capped like the original) is the custom-
  avatar stretch goal, not the acceptance gate.
- **(b) Publishing ours:** to be *fetchable* we would need to host our
  `.avb` over HTTP **and** answer `# GetCharInfo` — which the lifted engine
  currently swallows with **no event** (`engine/protsupp.cpp:958-964`,
  `ccPayloadHandledNoEvent`), so publishing requires an engine change (new
  event or lifted auto-reply) before a tiny local HTTP server would even be
  reachable. **Pragmatic Plan 4 answer: don't publish.** Announce a bundled
  character name (no URL): the Windows client resolves it locally and renders
  us correctly with zero HTTP — that satisfies "avatar interop both
  directions" for the standard roster, which is what §8's spirit requires.
  Accept custom avatars as one-way (we fetch theirs) or defer entirely.
- **Outbound announce wiring:** there is **no `cc_session` announce API**
  (`include/comicchat.h:457-502` is the full outbound surface). Options:
  (1) *recommended* — add a small `cc_session_announce_avatar(name, url)`
  routing through `bChatSendToChannel(szAnnotations=…)` so the
  IRCX-DATA-vs-PRIVMSG branch is inherited for free, matching
  `ChatAnnounceNewAvatar`'s shape; (2) *zero-engine-change hack* —
  `cc_session_send_say(ann=NULL, text="# Appears as Anna")` produces
  byte-identical wire form on plain IRC (and the 1998 client parses
  `#`-comments arriving over PRIVMSG on IRCX servers too,
  `v2.5-beta-1-modern/ircsock.cpp:1643-1644`), though on IRCX it deviates
  from the original's DATA carriage. Send it on `CC_EV_SELF_JOINED` and on
  character change, mirroring `JoinEntry::Execute`/`SetMyAvatar`.

---

## 5. CTCP auto-replies — verdict: cosmetic, defer

**What the original sends** (`v2.5-beta-1-modern/protsupp.cpp:1126-1163`):
`ReplyVersion`/`ReplyPing`/`ReplyTime`/`ReplyEmail`/`ReplyHomePage` — NOTICE
CTCP responses; plus `CLIENTINFO` inline (`:1803`), and the non-CTCP
`#`-comment auto-replies `# HeresInfo:` (profile, `:914-920`) and the
GetCharInfo re-announce (§4).

**Who triggers them:** exclusively explicit peer actions — user-info dialog
and member-list commands (`chatdoc.cpp:1787,1797,1929,1935`,
`memblst.cpp:284`) or user-configured automation rules (`actions.cpp:859-886`);
the senders are `ChatGetVersion`/`ChatPingUser`/etc.
(`protsupp.cpp:3701-3717`). **Nothing in the pose/avatar/whisper/sound
pipeline probes peers with CTCP first** — annotations are `(#…)`/`DATA`
blobs, avatar sync is the `#`-comment grammar, sounds are one-way
`\x01SOUND\x01` announcements needing no reply. Servers don't originate CTCP
either.

**Port status:** inbound CTCP VERSION/PING/TIME/EMAIL/URL/CLIENTINFO/NETMEET
are R20-suppressed with **no event emitted**
(`engine/protsupp.cpp:1008-1020`), as are `# GetInfo`/`# GetCharInfo`
(`:946-964`) — the handoff's "engine emits events instead" overstates it;
Swift currently cannot even observe a probe.

**Verdict for §8:** auto-replies are **not required** — no acceptance
assertion depends on them. Observable degradation is limited to: a Windows
user clicking Get Version / Ping / Get Profile / Get Character on us gets
silence (their client just never shows a result; ping shows no time). Note it
in the acceptance script as expected. If Plan 4 later wants parity, it needs
either new engine events for the probes or a Swift-side scan of inbound
PRIVMSG for `\x01` verbs (Swift owns the socket bytes), plus
`# GetCharInfo` handling if custom-avatar publishing ever lands (§4b).

---

## 6. Proposed §8 acceptance procedure

**Server choice: primary = local ngircd rig** (Topology A — deterministic,
fully captured, same ircd family as the community's own dedoky box);
**secondary = `www.crypthome.com:6667`** (Topology B — live IRCX: 800 pivot,
ANON login, DATA transport; verified compatible §1.1); **fallback =
`comic.dedoky.com:6667`** (live plain-IRC community server; mind
`NICKLEN=9`). Optional deterministic-IRCX understudy: IRC7 in Docker, after
the `MODE ISIRCX` smoke test.

**Prerequisites (build items §8 blocks on):**

- P1. Swift connect sequencing: probe → 451-fallback/800-pivot → NICK/USER →
  `CC_EV_LOGGED_IN` (§2 gap).
- P2. `cc_strip` avatar assignment/refresh + `ProtocolStripBridge` wiring so
  a post-create `.appearsAs` re-renders (§4a).
- P3. Outbound `# Appears as` announce on self-join / character change (§4,
  either mechanism).
- P4. Keep all test text ASCII until the CP-1252 case-fold debt is paid
  (handoff must-own item).

**Ordered checklist (run once per topology; A then B):**

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

---

## Top risks / open questions for the planner

1. **Swift login sequencing does not exist yet** — without P1 there is no
   live connect at all; it must be an early Plan 4 task, and the 50 s
   fallback timer probably wants shortening for interactive UX (Swift owns
   the timer).
2. **`cc_strip` has no change-avatar API** (`ProtocolStripBridge.swift:123-129`
   no-op) — acceptance step 4 fails without P2; this is an engine-surface
   addition, so it carries Edit-Rules/review weight.
3. **Live-server availability drift** (spec §9 already flags it): crypthome
   is a 10-user memorial box, dedoky a personal aarch64 server — either can
   vanish. Mitigation: everything in §6 also runs against local
   ngircd/IRC7; re-probe (2-line `nc` check) the morning of the acceptance.
4. **IRC7's pre-registration `MODE ISIRCX` reply is unverified** — if it
   answers `421`/nothing instead of `800`/`451`, our client stalls 50 s
   (no 421 handler in the parser). One-evening Docker smoke test before
   relying on it; irrelevant if crypthome is used for IRCX.
5. **The IRCX DATA→PRIVMSG re-pairing has only unit-test coverage** — a
   busy room interleaving multiple senders' DATA/PRIVMSG pairs is its first
   real trial (step 6, Topology B). Capture everything so any failure
   becomes a replayable fixture.
6. **`# GetCharInfo` is swallowed eventlessly** — harmless for the
   bundled-name avatar plan, but silently blocks any future custom-avatar
   publishing; decide explicitly (defer vs. new event) so it doesn't
   resurface as a surprise during acceptance.
7. **Encoding edges:** dedoky advertises `CHARSET=UTF-8` and `NICKLEN=9`;
   the 1998 client sends CP-1252 regardless. Keep acceptance ASCII (P4) and
   note the per-connection UTF-8 toggle (spec §4.5) as the follow-up.
8. **Etiquette/consent on public servers:** small communities, real people —
   human-driven sessions in a scratch room, introduce the project, keep the
   Windows client's "send MS Chat specific information" default ON (that's
   the point), and never run scripted load against them.
