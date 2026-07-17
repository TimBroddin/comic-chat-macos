# Microsoft Comic Chat 2.5 — Native macOS Port: Design

**Date:** 2026-07-17
**Status:** Approved design, pre-implementation
**Source tree:** `v2.5-beta-1-modern/` (stays untouched as reference)
**New code:** `macos/` (new top-level folder)

## 1. Goal and constraints

Port Microsoft Comic Chat 2.5 beta-1 (the ~95k-line 1998 MFC/Win32 C++ client,
already modernized to build under current Visual Studio in
`v2.5-beta-1-modern/`) to a **native macOS application**.

Decisions made during brainstorming:

- **Purpose:** personal project. No notarization/distribution pipeline
  required; local dev signing is enough.
- **Look and feel:** a real Mac app (menu bar, standard windows, SwiftUI/AppKit
  chrome). The comic strip rendering itself stays pixel-faithful; the Windows 98
  chrome is not imitated.
- **Stack:** original C++ engine lifted into a library + Swift shell
  (Approach A, "shim-and-lift" — see §2).
- **Interop is a hard requirement:** the port must connect to the hobbyist
  MS Chat servers still running (and plain IRC) and exchange comic annotations
  correctly with real Windows Comic Chat clients — they see our poses, we see
  theirs. This is why the original protocol/layout code is ported rather than
  reimplemented.
- **Target:** Apple Silicon, current macOS on the developer's machine.

### Feature scope

**In:** IRC/MS Chat connection, comic-strip view, avatars + emotion wheel,
say/think/whisper/action, whisper boxes, room list, member list, character and
backdrop pickers, **plain-text view** (toggleable), **sound events** (WAV;
event sounds and received `/sound`), **save and print** (print strip, export
PDF/PNG, JSON conversation save — see §5 deviation).

**Out:** DCC file transfer, OLE/COM embedding (the app-as-OLE-server machinery),
NetMeeting integration, Content Advisor (`msrating`), WinHelp, MIDI playback,
reading/writing the original MFC-serialized `.ccc` conversation format.

## 2. Approach decision

Three approaches were considered:

- **A. Shim-and-lift (chosen):** compile the original engine sources into a
  C++ library against a small MFC-compat shim; Swift UI on top. Maximum
  original code survives; interop and rendering fidelity come from the genuine
  1998 logic. Clean, testable boundary.
- **B. Direct Objective-C++:** original files compiled straight into the Xcode
  app target with a fat compat header. Fastest first pixel, but boundaries stay
  muddy forever; rejected for a months-scale project.
- **C. Clean-room rewrite:** reimplement the engine from the source + the
  Kurlander/Skelly/Salesin SIGGRAPH '96 paper with golden-master tests.
  Cleanest end state, but reimplementation drift is exactly where annotation
  and layout interop breaks; rejected because interop is a hard requirement.

## 3. Architecture and repository layout

```
macos/
  ComicChatKit/            SwiftPM package
    Sources/
      cchat-engine/        C++ target: lifted engine sources + shim/ + C bridge (comicchat.h)
      ComicChatKit/        Swift target: typed wrappers, Network.framework TCP,
                           persistence; the only client of the C bridge
    Tests/                 headless engine + protocol tests (`swift test`)
  ComicChat/               Xcode app project (SwiftUI lifecycle + AppKit views),
                           depends on ComicChatKit; UI code only
```

- The C bridge (`comicchat.h`) is the **only** interface Swift sees; no C++
  types cross it. No Win32 headers anywhere in `macos/`.
- SwiftPM builds the C++ target directly, so engine and protocol tests run
  headless via `swift test` without booting the GUI — the main reason for the
  package/app split.
- The `comicart/` `.avb`/`.bgb` art (and the original WAV assets) are bundled
  into the app as resources and loaded by the original loader code.

## 4. The engine library (`cchat-engine`)

### 4.1 Lifted sources

Copied from `v2.5-beta-1-modern/`, edited as little as possible:

| Subsystem | Files |
|---|---|
| Art format (.avb/.bgb) | `avbfile.cpp`, `avatario.cpp`, `avatar.cpp`, `backdrop.cpp`, DIB parsing from `dib.cpp` |
| Comic layout | `balloon.cpp`, `bbox.cpp`, `spline.cpp`, `splinutl.cpp`, `traj.cpp`, `vector2d.cpp`, `bodycam.cpp` |
| Semantics (text → pose/emotion) | `semantic.cpp` |
| Protocol | `ircproto.cpp` plus the annotation encode/decode paths it uses (parts of `format.cpp` / `protsupp.cpp`; the exact split is confirmed during porting, since those files mix protocol logic with UI notification) |

Call sites in lifted files that reach UI-layer facilities are deleted or
rerouted through the boundaries below — the shim does not grow to absorb them.

### 4.2 The MFC-compat shim

`shim/mfc_compat.h` (+ small `.cpp`) reimplements **only what the lifted files
use**:

- `CString` — byte-oriented, matching MFC semantics (this codebase is
  `char*`/MBCS throughout).
- `CPoint`, `CSize`, `CRect` with Win32 semantics.
- The collection classes the engine touches (`CDWordArray`, etc.) over
  `std::vector`.
- Scalar/typedef layer: `BOOL`, `DWORD`, `WORD`, `BYTE`, `UINT`, `COLORREF`,
  `RGB()`, `LPCTSTR`, `TRUE`/`FALSE`.
- `ASSERT` / `TRACE` — logging assert, trap only in debug builds (§7).

Explicitly **not** a mini-MFC: nothing is added speculatively.

### 4.3 Boundary 1 — rendering (`Canvas`)

An abstract C++ interface the layout/drawing code calls:

- `drawImage` — RGBA images, decoded **once at load** from the palette-based
  DIBs inside `.avb`/`.bgb` files, preserving the original palette and
  transparent-color handling.
- `fillPath` / `strokePath` — balloon splines, tails, panel borders.
- `drawText` / `measureText` — text **measurement flows back into layout**
  (balloon sizing depends on it); the original GDI `GetTextExtent` calls become
  `measureText`. Default font is Comic Sans MS, which macOS ships, keeping
  metrics close to the original.

Swift implements `Canvas` with CoreGraphics + CoreText. The interface is
resolution-independent, which yields Retina rendering, printing, and PDF export
from the same code path. A second, recording implementation of `Canvas` backs
the layout snapshot tests (§8).

### 4.4 Boundary 2 — network (bytes in, events out)

The engine never opens sockets:

- Swift (`NWConnection`) owns TCP (and TLS where a server supports it) and
  feeds received bytes into the ported `ircproto` parser through the bridge.
- The engine emits (a) outbound byte buffers to send, and (b) typed events —
  message with annotations, join/part, user-list and room-list updates,
  whispers, topic, errors — through C callbacks that ComicChatKit adapts into
  `AsyncStream`s.
- Timers (PING, etc.) are requested by the engine, scheduled by Swift.

Because the original client's protocol code does the talking, MS Chat
server/IRCX behavior and client-to-client annotation compatibility are
inherited rather than reimplemented.

### 4.5 Text encoding

Wire text stays **bytes end-to-end inside the engine** (as in the original).
The Swift layer converts for display: **CP-1252 by default** (what 1998
Windows clients send), **UTF-8 as a per-connection setting**. The original
JIS/SJIS conversion units (`jis2sjis.cpp`, `sjis2jis.cpp`) are not ported
unless a real server need appears.

## 5. The Mac app

One main window per connection, native chrome throughout:

- **Chat window:** comic strip canvas — a custom `NSView` in a scroll view,
  drawn entirely by the engine through `Canvas` — with a view toggle to a
  plain-text transcript (`NSTextView`-backed). Right sidebar: member list with
  avatar thumbnails and op/voice badges. Bottom compose bar: text field,
  say/think/whisper/action controls, and the **emotion wheel** as a custom
  control using the original art (2D emotion/intensity picker).
- **Windows/sheets:** connect sheet (server, nick, persona), room list window,
  character picker and backdrop picker (thumbnails rendered by the engine),
  user-info popover, whisper boxes as separate small windows (the original's
  model).
- **Settings:** SwiftUI Settings scene — persona (name, character), display
  options, sounds, connection defaults. Stored in `UserDefaults`, replacing the
  registry wholesale.
- **Sounds:** `AVAudioPlayer`; original WAV assets bundled. WAV only (MIDI
  dropped with MCI).
- **Save & print:** `NSPrintOperation` for printing and PDF/PNG export of the
  strip, both rendered through the same `Canvas` path (paginated panels).

**Deviation (the one intentional break from "full port"):** conversations save
as **JSON transcripts** — messages, annotations, participants — which the app
reopens and re-renders identically (the comic is deterministic given the
annotations). The MFC `CArchive` `.ccc` format is not ported; original `.ccc`
files remain unreadable by this app.

## 6. Persistence

`UserDefaults` for all settings (replaces registry). JSON files for saved
conversations (user-chosen location via save panel). Bundled resources for art
and sounds; a user-addable characters folder in Application Support is
supported by pointing the original loader at both locations.

## 7. Error handling

- The C bridge never throws across the boundary. Engine errors surface as
  status codes plus an error-event callback.
- Swift policy: alerts for user-facing failures (connection failed, malformed
  `.avb`); logs for recoverable protocol oddities. The engine keeps the
  original's **permissive** protocol handling — added strictness would break
  interop.
- Network drops: Swift-side auto-reconnect, then the engine's session re-join
  logic.
- `ASSERT` in lifted code: logs in release, traps in debug.

## 8. Testing

All headless via `swift test`:

1. **Art golden tests** — parse every `.avb`/`.bgb` in `comicart/`; assert
   character/pose/gesture counts, names, and bitmap checksums. Ground truth
   captured once from the Windows build.
2. **Protocol/annotation round-trip** — replay captured transcripts of real
   Comic Chat sessions (recorded from the Windows 2.5 client under Wine against
   a test ircd) through the parser; assert emitted events; re-encode outbound
   messages and **byte-compare** the annotations.
3. **Layout snapshots** — deterministic scripted conversations rendered through
   the recording `Canvas`; diff the draw-call stream to catch layout
   regressions.

**Manual acceptance:** connect to a live hobbyist MS Chat server alongside the
real Windows 2.5 client (Wine or a VM) and verify pose/avatar/whisper interop
in both directions.

## 9. Risks and open points

- **`format.cpp`/`protsupp.cpp` untangling** (annotation codec vs UI
  notification) is the least-mapped part of the lift; it is called out as a
  discovery task in the implementation plan.
- **Text metrics:** balloon layout fidelity depends on `measureText` behaving
  like GDI's `GetTextExtent` for Comic Sans MS; layout snapshots plus visual
  comparison against the Windows client bound this risk.
- **Live servers:** hobbyist MS Chat server availability can shift; the
  captured-transcript tests keep interop verifiable offline.
