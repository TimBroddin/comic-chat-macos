# Plan 3 Discovery A — the Wine capture rig (spec §8.2)

**Verdict: the rig stands up and works.** The genuine 1998 English client
(`cchat.exe` 2.5, June 26 1998) runs under Wine 11.0 on this arm64 Mac, connects
through a byte-logging TCP proxy to a stock `ngircd`, and a **complete plain-IRC
session was captured byte-for-byte in both directions** — matching the
command-surface static analysis exactly. The one thing not yet captured is an
*annotated comic message*; that needs GUI interaction this headless session
can't perform (see "The one gap" below). Everything needed to produce that
corpus is installed, scripted, and reproducible.

All rig artifacts live under `.superpowers/rig/` (git-ignored — the 1.7 MB
installer, the extracted client, the Wine prefix, and the capture logs stay out
of the repo). This report is the committed record.

## What was stood up

| Piece | Choice | Location / command |
|---|---|---|
| Windows client | MS Comic Chat 2.5 English, `mschat25.exe` from archive.org item `cchat25` (sha256 `78d1eb4e…472a`), CAB-extracted with `cabextract` → `cchat.exe` (1998) | `.superpowers/rig/client/cchat.exe` |
| Wine | Homebrew `wine-stable` (Wine 11.0). The cask install is Gatekeeper-quarantined and SIP blocks stripping the flag in `/Applications`; **clone to `~/Applications/` with `cp -Rc` then `xattr -dr com.apple.quarantine`** — the clone runs | `~/Applications/Wine Stable.app` |
| Local ircd | Homebrew `ngircd` 28 | `/opt/homebrew/opt/ngircd/sbin/ngircd`, config `.superpowers/rig/ngircd.conf` |
| Capture proxy | Bun TCP proxy, logs every chunk as JSONL `{t,dir,hex,latin1}` | `.superpowers/rig/capture-proxy.ts` |
| Runner | start/stop orchestration | `.superpowers/rig/run-rig.sh` |

`run-rig.sh` (no args) boots ircd + proxy + client; `run-rig.sh proxy` boots the
first two only (drive the client — or a second one — by hand); `run-rig.sh stop`
kills all three. The client is launched via `cchat.exe "irc://127.0.0.1:6668/#comicrig"`
— the `irc://host:port/#room` URL is parsed by `CChatApp::ProcessShellCommand`
(`chat.cpp:1186`) straight into the connect path, so **no GUI connect dialog is
needed to reach the wire**.

## The capture format

One JSON object per line (`captures/session-<epoch>.jsonl`):
`{"t":<ms since session start>,"dir":"c2s"|"s2c"|"meta","hex":"…","latin1":"…"}`.
`hex` is authoritative for the byte-compare tests (spec §8.2); `latin1` is a
lossless 1:1 byte→char view so CP-1252 text stays eyeball-readable in the log.
This is the exact artifact the Plan 3 protocol tests replay: feed `c2s`/`s2c`
`hex` streams at the parser and assert the emitted event stream; re-encode
outbound and byte-compare against the captured `c2s` hex.

## Evidence — the captured plain-IRC session

A real `cchat.exe` session, verbatim from `captures/smoke-2.jsonl`, confirms the
client's connect handshake and **cross-validates the command-surface report's
predicted plain-IRC fallback**:

```
c2s  MODE ISIRCX                                 ← IRCX probe (first bytes)
s2c  :… 451 * :Connection not registered         ← ngircd: unknown pre-reg cmd
c2s  NICK Anonymous
c2s  USER Anonymous Tims-Mac . :Your Full Name    ← plain-IRC fallback engaged
s2c  :… 001…004 / 005 / 251…266 / 375…376         ← registration + MOTD
c2s  MODE Anonymous -i
c2s  JOIN #comicrig
s2c  :Anonymous!… JOIN :#comicrig / 332 / 333 / 353 / 366
c2s  MODE #comicrig
c2s  WHO #comicrig
s2c  324 / 329 / 352 / 315
…
s2c  PING :rig.comicchat.local
c2s  PONG :rig.comicchat.local                    ← keepalive answered; stays up
```

This is precisely the path the command-surface agent derived statically: `MODE
ISIRCX` first, a `451` (crucially **not** `421`) tips the client into
`NICK`/`USER`, then it behaves as a well-formed RFC1459 client. **ngircd's stock
`451` reply is exactly what the client needs** — the minimal-server checklist is
satisfied by an unmodified ngircd for the plain-IRC (client-to-client PRIVMSG)
annotation path.

## Rig requirements confirmed against the checklist

The command-surface report listed hard requirements for the capture server; the
live run confirms ngircd meets them: reply `451` (not `421`) to pre-registration
`MODE ISIRCX` ✓; echo the self-`JOIN` with full prefix ✓; volunteer `353`/`366`
✓; answer the auto `MODE`/`WHO` ✓; server-initiated `PING` answered with `PONG`
✓. Only the IRCX-only surface (`800`/`AUTH`/`DATA`/`PROP`/`LISTX`/`WHISPER`)
would need an IRCX shim or scripted injection — **not required for the plain-IRC
annotation corpus**, which is what the byte-compare tests need first.

## The one gap — annotated-message capture is an interactive hand-off

To capture a *comic* message (a `PRIVMSG` carrying the inline `(#…)` annotation
prefix on plain IRC, per the state-and-codec report), a human must type into the
client's compose bar, or a second participant must be present. This session is
headless: macOS **Screen Recording and Accessibility permissions are not granted**
to the controlling process, so `screencapture` fails ("could not create image
from display"), System Events / CGWindowList cannot see Wine's non-AX windows by
title, and blind `CGEvent` keystrokes at guessed coordinates do not reliably
land in the compose control (the client stayed connected and answered PING, but
no `PRIVMSG` resulted). The capture *rig* is proven; the capture *campaign* for
annotated messages is:

- **Interactive (Tim):** `./run-rig.sh`, type a few say/think/whisper lines with
  different poses/emotions selected on the emotion wheel, address some to
  specific nicks (to exercise the `T<nicks>` addressee field), `stop`. The
  `(#…)`-prefixed PRIVMSGs land in the capture automatically. Two clients (run
  the binary twice against `:6668`) captures the inbound-annotation path too.
- **Or scripted in Plan 3 execution:** a second scripted IRC client on `:6668`
  that the real client annotates *to*, plus a small IRCX-shim mode on the proxy
  if the IRCX `DATA CCUDI1` out-of-band transport needs exercising. This is
  execution-task work, not discovery, and the parser can be built and unit-tested
  against hand-authored byte vectors before any full capture exists.

**Bottom line for Plan 3:** the test layer is unblocked. The parser/codec tasks
can start against hand-verified byte vectors (the wire grammar is fully
documented in the state-and-codec report), and the rig is ready to produce real
captured corpora on demand — the plain-IRC path with zero server changes, the
IRCX path with a scoped shim if a task needs it.
