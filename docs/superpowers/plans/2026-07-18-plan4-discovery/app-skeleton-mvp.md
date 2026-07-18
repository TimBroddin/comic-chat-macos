# App Skeleton & MVP Discovery — Plan 4 (the Mac app)

Scope: the discovery task 1 questions from `docs/superpowers/plans/2026-07-18-plan-4-handoff.md`
— original UI inventory, MVP slice, app-target shape, resources, settings, menu chrome. All
original citations are files in `/Users/timbroddin/Projects/comic-chat/v2.5-beta-1-modern/`
(read-only reference); all Swift/engine citations are
`/Users/timbroddin/Projects/comic-chat/macos/ComicChatKit/`. No code was modified; `swift
build`/`swift test` were not run (owned by another agent this session).

## 0. Corrections to the handoff/roadmap's naming assumptions

Verified against the source; these OVERRIDE the roadmap's "Original → Mac mapping" bullet list:

| Assumption | Reality | Evidence |
|---|---|---|
| `chicdial.cpp` is the character picker | **No.** It is MFC context-sensitive-help dialog base classes (`CCSDialog`/`CCSPropertyPage`/`CCSPropertySheet`) that every other dialog subclasses | chicdial.cpp:22-50, 85-97, 176-196 |
| Character/backdrop pickers are standalone dialogs | They are **property pages inside the Options sheet**: `CCharacterPage` (proppage.h:145-197, impl proppage.cpp:728-880) and `CBackgroundPage` (proppage.h:199-246). The bodycam double-click opens Options at the character page (bodycam.cpp:351) |
| `userlist.cpp` is the member list | **No.** `userlist.cpp` is the *server-wide user search dialog* (`CUserList : CCSDialog`, userlist.cpp:41-58, menu `ID_USER_LIST`). The in-room member pane is **`memblst.cpp`** (`CMemberListCtrl : CListCtrl` + `CMemberList : CFrameWnd`, memblst.h:8/44), populated by protsupp's `AddToMembersList` |
| Whisper boxes are "separate small windows" (spec §5) | The original is **one tabbed dialog**: `CWhisperBox : CDialog` with a `CTabCtrl`, one `CWhisperLeaf` per correspondent, Ignore + Delete-tab buttons (whisprbx.cpp:35-72, whisprbx.h:10/35) |
| "original WAV assets bundled" (spec §5) | **There are no WAV files anywhere in the repo** (`find . -iname "*.wav"` → zero hits, all trees). The original plays sounds from a user-configurable *sound path* that defaults to the **Windows media directory** (sounddlg.cpp:362-366; utils.cpp:5). See §4.2 |
| The emotion wheel lives in `saywnd.cpp`/res art | It is **`CBodyCam : CWnd`** in bodycam.cpp (the whole file), created as the bottom-right splitter pane (chatview.cpp:360). Its art is the eight `res/fc_*_l.bmp` face bitmaps (chat.rc:1903-1910) plus the live avatar-pose preview drawn from the avatar itself |

## 1. Original UI inventory

Original chat-window layout (chatview.cpp:333-378, `CreateComicView`): one MDI child per room,
split vertically — **left column**: `CPageView` (comic strip) over `CSayWnd` (compose bar, 40px);
**right column**: `CMemberList` over `CBodyCam` (emotion wheel). View mode chooses `CPageView` vs
`CTextView` at creation (chatview.cpp:289-291).

### 1.1 Comic view — `chatview.cpp` + `pageview.cpp` — **MVP**

`CChatView` is a paintless container that owns the three splitters and routes printing to the
inner view (chatview.cpp:87-129, 333-378). `CPageView : CScrollView` is the actual comic strip:
`OnDraw` walks the document's `m_pages` list and draws each page that intersects the clip rect
into a retained DIB section (pageview.cpp:189-244), scrolls in MM_TWIPS logical space
(pageview.cpp:367-375, 839-935), auto-sticks to bottom (`AtBottom`, pageview.cpp:355), shows
balloon-text tooltips via hit-testing (pageview.cpp:638), and does avatar click-selection for
talk-to targeting plus a right-click context menu (pageview.cpp:778-806, 750). Printing runs
through the same `OnDraw` (pageview.cpp:527-529).
**Mac equivalent:** custom `NSView` inside `NSScrollView` (via `NSViewRepresentable`), drawing the
`CGImage` produced by composing the `Strip` onto a `CGCanvas` (CGCanvas.swift:47, 490); the
engine already owns pagination/panel layout via `cc_strip`. Tooltips/click-selection/print reuse
the same canvas later. **Verdict: MVP** (draw + scroll + stick-to-bottom only; tooltips,
avatar-click talk-to, context menu are post-MVP Plan 4; printing post-MVP via the same path).

### 1.2 Text view — `textview.cpp` — **post-MVP Plan 4**

`CTextView : CView` hosts a RichEdit transcript (`IDC_RICHEDIT`, textview.cpp:21, 40-73):
per-message-type character formats (the 18-slot `m_cfArray`), clickable links (`EN_LINK`
→ `HandleLink`, textview.cpp:69), host-message highlighting, optional timestamps. It reads
`theApp.m_cfArray`/`m_textSpacing`/`m_iHostHighlight` and writes nothing (persists via the app's
`SaveToReg` on destroy, textview.cpp:50).
**Mac equivalent:** `NSTextView`-backed transcript fed from the same `ProtocolEvent` stream (an
`AttributedString` log). Spec §5 keeps the comic/text toggle in scope. **Verdict: post-MVP
Plan 4** — the comic view is the point of the app; a plain transcript is cheap and useful for
debugging but not needed for the first render loop.

### 1.3 Compose bar — `saywnd.cpp` — **MVP (minimal)**

`CSayWnd : CFrameWnd` = a `CSayCtrl` RichEdit input plus a `CSayToolBar` with five send actions:
Say / Think / Whisper / Action / Sound (buttons set at chatview.cpp:369, command map
saywnd.cpp:411-422). Each handler grabs the text + RTF formatting array and calls
`bChatSendText(str, BM_SAY|BM_THINK|BM_WHISPER|BM_ACTION, TRUE, prgdwFormatting)`
(saywnd.cpp:765, 805, 842, 927); Enter sends a Say (saywnd.cpp:216). It also live-previews the
typed text's inferred gesture into the bodycam on every edit (`ChatPreSendText`,
saywnd.cpp:969-975) and hosts the bold/italic/color formatting state.
**Mac equivalent:** an `NSTextField`/SwiftUI `TextField` + segmented control or buttons for the
four modes; `ProtocolSession.say/whisper` and `Strip.Mode` (say/whisper/think/action) already
exist (ProtocolSession.swift:386-429, Strip.swift:33-44). **Verdict: MVP = text field + Enter
sends Say.** Think/Action buttons: post-MVP Plan 4 (trivial once wired). Whisper + Sound buttons:
post-MVP Plan 4. RTF formatting of message text: **defer beyond Plan 4.** Typing-preview into the
wheel: post-MVP with the wheel.

### 1.4 Emotion wheel — `bodycam.cpp` (`CBodyCam`) — **post-MVP Plan 4**

The 2D emotion/intensity picker: a bulls-eye with eight face icons around it (emotion names
loaded bodycam.cpp:41-47, icon DIBs bodycam.cpp:49-59 → `res/fc_hap_l.bmp` etc., chat.rc:1903-1910);
dragging the cursor maps point→`CEmotion` (direction = emotion, distance = intensity,
bodycam.cpp:398, 854) and the pane redraws the **user's own avatar posed live** for that emotion
(bodycam.cpp:164-233). Context menu: Freeze (lock expression) and Send Expression (fires a DATA
UDI immediately, bodycam.cpp:1041, 1063). Double-click opens the character picker
(bodycam.cpp:351). State: reads the self avatar + writes the current emotion attached to the next
outbound message's annotations.
**Mac equivalent:** custom SwiftUI/AppKit control drawing the bulls-eye + pose preview through
`AvatarFile.poseImage` (ArtFile.swift:56); the outbound side is just filling
`Annotations.faceEmotion/faceIntensity/...` on `say(...)`. The eight face BMPs must be converted
or redrawn (they're 8-bit DIB resources). **Verdict: post-MVP Plan 4** — the signature Comic Chat
control, but the MVP can send neutral annotations; do it right after the chat loop works.

### 1.5 Member list pane — `memblst.cpp` (NOT `userlist.cpp`) — **MVP (basic)**

`CMemberListCtrl : CListCtrl` + `CMemberList : CFrameWnd` frame: shows every room member with
avatar-derived icon or text style (icon vs list mode = `m_bIconMembers`, registry
`MemberListStyle`), op/owner/away badges via image-list callbacks (populated from protsupp's
`AddToMembersList`, see plan3 state-and-codec §1.2), key-forwarding to the compose bar
(memblst.cpp:57-70). Crucially the list's *selection* is the canonical talk-to/whisper-target
state (`MListTalkTosToPuiself`, userinfo.cpp:85-105).
**Mac equivalent:** SwiftUI `List` bound to `ProtocolSession.RoomState.members`
(ProtocolSession.swift:60-70), selection driving the outbound `Annotations.addressees`.
**Verdict: MVP = plain nick list (no icons)**; op/voice/away badges + avatar thumbnails +
selection-as-addressee: post-MVP Plan 4.

### 1.6 Room list — `roomlist.cpp` — **post-MVP Plan 4**

`CRoomList : CCSDialog`: a list-view of LIST/LISTX results with client-side filters — name/topic
substring, "search descriptions", min/max members, registered-only (roomlist.cpp:27-60) — a
Go To button that joins the selected room (roomlist.cpp:315), and persisted filter state
(`CRoomListPersist`, roomlist.h:20).
**Mac equivalent:** a table-backed sheet/window over `ProtocolSession.list()` +
`.roomListBegin/.roomListItem/.roomListEnd` events (ProtocolEvents.swift:149-151) — the protocol
side is 100% done. **Verdict: post-MVP Plan 4** (MVP types a room name into the connect sheet).

### 1.7 User list dialog — `userlist.cpp` — **defer beyond Plan 4**

`CUserList : CCSDialog` (userlist.cpp:41-58): server-wide *user search* (filter by nick mask
and/or room), backed by WHO-style queries, with per-row actions (whisper, join their room, get
info). Reads/writes only its own persisted filters (`CUserListPersist`, userlist.h:37).
**Mac equivalent:** a search window over `.whoResult` events (already emitted,
ProtocolEvents.swift:153). **Verdict: defer beyond Plan 4** — niche against modern hobbyist
servers; nothing blocks doing it later since the events exist.

### 1.8 Whisper box — `whisprbx.cpp` — **post-MVP Plan 4**

`CWhisperBox : CDialog`: one floating window with a tab per whisper correspondent
(`CWhisperLeaf` holds each leaf's history + state, whisprbx.h:10), an embedded whisper-mode
`CSayWnd`, Ignore and Delete-tab buttons (whisprbx.cpp:35-72); size/position persisted
(`WhisperDims` registry, setupdlg.cpp:582-584). Incoming whispers route here when the user has
`AcceptWhispers` on.
**Mac equivalent:** a single auxiliary `Window` with a tab bar (or sidebar list) per peer —
match the original's one-window model rather than spec §5's "separate small windows" phrasing.
`ProtocolSession.whisper(to:text:channel:)` + `.whisper` events exist. **Verdict: post-MVP
Plan 4** (whisper interop is part of the §8 acceptance, so it must land within Plan 4).

### 1.9 Settings / Options — `setupdlg.cpp` + `proppage.cpp` — **connect sheet MVP; Options post-MVP**

Two distinct things share these files:
- **`CSetupPage`** (setupdlg.cpp:140-207): the *connect* UI — server combo (from the registry
  server directory), room name, and an "on connect" radio (go to room / show room list / connect
  only; `m_iOnConnectAction`). Plus the modal prompts `CNicknameDlg`/`CChannelDlg`/`CPasswordDlg`
  (setupdlg.h:169-243) for nick-collision, join-by-name and +k rooms.
- **The Options property sheet** (proppage.h): `CPersonalPage` (nick, real name, email, homepage,
  RTF profile, proppage.h:85-141), `CCharacterPage` (avatar listbox + live `CBodyCam` preview,
  proppage.h:145-197), `CBackgroundPage` (backdrop listbox + DIB preview, proppage.h:199-246),
  `CSettingsPage` (send-comics-data, prompt-for-save, accept whispers, show arrivals, sound path,
  play sounds, allow invites/file-tx, show identity, NetMeeting, ratings — proppage.h:22-42),
  `CComicsPropPage` (panels-per-row combo, comic font, auto-download chars/backdrops,
  proppage.h:305-366), `CTextFontPage` (18 per-message-type fonts, proppage.h:248-303),
  `CServersPage` (server directory editor).
- `CChatApp::LoadFromReg`/`SaveToReg` (setupdlg.cpp:287-623, 626-853) is the persistence spine —
  full key inventory in §5.
**Mac equivalent:** a connect *sheet* (MVP) + a SwiftUI `Settings` scene with Persona /
Characters / Backgrounds / Comic / Sounds / Advanced tabs (post-MVP), all on `UserDefaults`.
**Verdict:** connect sheet **MVP**; Settings scene + pickers **post-MVP Plan 4**; text-font page
and server-directory editor **defer beyond Plan 4**.

### 1.10 Sounds dialog — `sounddlg.cpp` — **post-MVP Plan 4 (reduced)**

`CSoundDlg : CCSDialog`: browses the *sound path* for `wav/mid/rmi` files (sounddlg.cpp:24-27,
enumeration at :537), type-to-filter, Test button (:444), and OK sends the selected sound as a
`#SOUND` message (via `CSayWnd::OnPlaySound`, saywnd.cpp:848). Playback engine:
`bFindAndPlaySound` searches `theApp.m_soundPath` and `sndPlaySound`s the match
(sounddlg.cpp:263-290, 336-358). Inbound `/sound` plays the named local file if `PlaySounds` is
on (protsupp.cpp:1415-1416); the engine already emits `CC_EV_SOUND` → `.sound(nick:file:text:)`
(ProtocolEvents.swift:140). **The app ships no sounds** — see §4.2.
**Mac equivalent:** picker over a `~/Library/Application Support/Comic Chat/Sounds` folder +
`AVAudioPlayer`; WAV only (MIDI dropped, spec §5). **Verdict: post-MVP Plan 4**; inbound-`.sound`
playback is the valuable half (tiny), the send-dialog is the second half.

### 1.11 Text font dialog — `txtfntdg.cpp` — **defer beyond Plan 4**

`CMyFontDialog : CWin4FontDialog`: a customized ChooseFont dialog with a message-type dropdown
(18 types + "All", txtfntdg.cpp:34) editing `theApp.m_cfArray` `CHARFORMAT`s for the *text view*.
**Mac equivalent:** `NSFontPanel` + a per-message-type table in Settings, only meaningful once
the text view has per-type styling. **Verdict: defer beyond Plan 4** — MVP text view (when it
comes) uses one font; the comic font is a separate, simpler setting (`ComicsFont`).

### 1.12 Color dialog — `colordlg.cpp` — **defer beyond Plan 4**

`CColorDlg : CDialog`: a 16-swatch palette picker (the fixed IRC 16-color `clrTable`,
colordlg.cpp:16-30) for message text color (Format menu → Ctrl+K), feeding the RTF formatting of
outbound text. **Mac equivalent:** none needed until RTF message formatting exists.
**Verdict: defer beyond Plan 4** (with the whole Format menu, §6).

### 1.13 `bothdlg.cpp` — **drop**

`CBotherDlg`: a one-checkbox "don't bother me again" dialog bound to `theApp.m_bBother`
(bothdlg.cpp:18-33). No other live references to `m_bBother` were found in the tree; vestigial.
**Verdict: drop** (macOS alerts get a native "do not ask again" suppression checkbox when needed).

### 1.14 Explicitly dropped chrome (roadmap-confirmed)

`coolbar.cpp`/`chatbars.cpp`/`tabbar.cpp`/`doskey.cpp` (toolbars, MDI tab bar, input history
hack) → native toolbar/standard text editing; `status.cpp` status window → post-MVP status/
transcript line; MDI itself → one window per connection (spec §5).

## 2. The MVP slice

Target loop (handoff discovery task 1): **connect → join a room → comic strip updates live as
messages arrive → send a say.** Everything in one window plus one sheet.

### 2.1 Connect sheet (MVP fields)

| Field | Type | Default | Backing |
|---|---|---|---|
| Server | text | last used (`connect.server`) | `ProtocolSession(host:)` |
| Port | number | 6667 | `ProtocolSession(port:)` |
| Nickname | text | last used (`persona.nick`) | `ProtocolSession(nick:)` |
| Room | text | last used (`connect.room`, e.g. `#comicrig`) | `session.join(_:)` after `.loggedIn` |
| Encoding | picker CP-1252 / UTF-8 | CP-1252 (spec §4.5) | `ProtocolSession(encoding:)` — already plumbed (ProtocolSession.swift:133) |

No server directory, no on-connect radio, no password/nick-collision dialogs (surface
`.nickRejected`/`.error` as an alert; retry manually).

### 2.2 Chat window (MVP contents)

- **Comic strip view** (the custom `NSView` + `NSScrollView`; §3.3): recomposed after each
  strip-affecting event, stick-to-bottom.
- **Member sidebar**: plain SwiftUI `List` of nicks from `RoomState.members` (no icons/badges).
  Cheap, and it visually proves membership tracking during acceptance.
- **Compose bar**: one text field; Enter → `session.say(channel, text:, annotations: neutral)`.
- **Status line** (one `Text`): connection state + last `.statusLine`/`.error` text — replaces
  the original status window for MVP debugging.

### 2.3 Hardcoded/deferred in the MVP

| Thing | MVP behavior | Original behavior it temporarily replaces |
|---|---|---|
| Own character | fixed default (e.g. `anna.avb`; or random pick à la setupdlg.cpp:601-606) | `CCharacterPage` picker + registry `Character` |
| Peer avatars | `ProtocolStripBridge.AvatarResolver(comicartDir:)` — honors `# Appears as` names that match bundled art, cycles defaults otherwise (ProtocolStripBridge.swift:29-64) | auto-download via `webreq.cpp` (post-MVP: `URLSession`) |
| Backdrop | `field.bgb` (the original default: `IDS_DEFAULT_BACKDROP` = "FIELD", chat.rc:2327, loaded chat.cpp:505) | `CBackgroundPage` |
| Annotations on outbound say | neutral pose/emotion (all zeros), `sendComicsData` always on | emotion wheel + `ComicsData` toggle |
| Own-avatar announce | **not sent** (see risk R2 — no outbound `# Appears as` API yet) | `ChatAnnounceNewAvatar` on join |
| Panels per row / panel size | engine defaults | `UPNLWidth/UPNLHeight/UnitsWide` registry |
| Rooms | exactly one room at a time (engine is single-`CIrcProto`; handoff "Multi-room") | MDI multi-room |
| Whisper/think/action | not exposed (say only) | compose toolbar |

### 2.4 MVP proof

Reuse the Plan 3 assets: `LoopbackReplayServer` (LoopbackReplayServer.swift:23) + the committed
capture fixtures (`Tests/.../Fixtures/captures`) give an offline scripted end-to-end (app connects
to localhost, sees the 1998 client's captured annotated messages render live); the Wine rig
(`./run-rig.sh`) gives the live proof. Exit artifact: a screenshot of the app window with a
multi-panel strip rendered from replayed real bytes, plus one outbound say echoed.

## 3. App target shape

### 3.1 Recommendation: Xcode app project (as spec §3 says), thin over the package

`macos/ComicChat/ComicChat.xcodeproj`, single app target **ComicChat**, macOS 14+ (matches
Package.swift:6), SwiftUI lifecycle (`@main struct ComicChatApp: App`), consuming
**ComicChatKit as a local package dependency** (`../ComicChatKit`, path-based — Xcode resolves
local SwiftPM packages natively; no publishing needed). Reasons over a SwiftPM
`.executableTarget`:

- A real `.app` bundle: Info.plist, icon, **bundled `comicart/` resources**, and the
  **`com.apple.security.network.client` entitlement** if sandboxed (NWConnection outbound;
  personal project → sandbox optional, but signing+entitlements are Xcode-native either way).
- `NSApplicationDelegateAdaptor`, Settings scene, printing, and window restoration all assume a
  bundle identity; SwiftPM executables need hand-rolled bundle scaffolding for each.
- The package/app split keeps `swift test` headless (spec §3's stated reason). The app project
  adds **no test target initially** — see 3.2.

Scaffolding: XcodeBuildMCP advertises project-scaffolding + macOS workflows, but **only
simulator-workflow tools are exposed in this session's tool list** (no `scaffold_macos_project`,
`build_macos`, `build_run_macos`, `launch_mac_app`). Before execution, enable the `macos` +
scaffolding workflows in XcodeBuildMCP config, or fall back to `xcodebuild
-project ... -scheme ComicChat build` and `open` via Bash — both fine. (Screenshot capture of the
running app for exit artifacts can use `screencapture -l <windowid>` if the MCP macOS tools stay
unavailable.)

### 3.2 Testability split (binding recommendation)

Put **all logic in ComicChatKit, only chrome in the app**:

- New in ComicChatKit (AppKit-free, tested by `swift test` in the existing
  `EngineGlobalStateSelfTests` serialized tree where engine state is touched):
  - `ChatSessionModel` (or similar): owns `ProtocolSession` + `ProtocolStripBridge` + `Strip`,
    consumes the `AsyncStream<ProtocolEvent>`, maintains transcript/member arrays, exposes "strip
    changed" with a freshly composed `CGImage` (CoreGraphics is fine headless — `CGCanvas`
    already lives in the package).
  - Settings store (typed `UserDefaults` wrapper, §5.3).
  - Art catalog/lookup (bundle dir + Application Support dir search order, §4.3).
- In the app target: `ComicChatApp` (scenes/menus), `ComicStripView`
  (`NSViewRepresentable` → custom `NSView` in `NSScrollView`), connect sheet, sidebar, compose
  bar views. If app-side unit tests are ever wanted, add an XCTest bundle to the Xcode project,
  but the goal is that nothing in the app target is worth unit-testing.

### 3.3 Where AppKit meets SwiftUI

SwiftUI window/scene chrome (`WindowGroup` or `Window` per connection + `Settings` scene +
`.commands` for the menu bar), with exactly one `NSViewRepresentable`: the comic strip view — a
`CALayer`-backed `NSView` whose `contents` is the composed `CGImage`, sized to
`Strip.size` (twips → points via `CGCanvas.scale`), inside `NSScrollView`. Full AppKit
`NSApplicationDelegate` lifecycle is not needed; `@NSApplicationDelegateAdaptor` can be added
later if termination hooks demand it. MVP redraw = **full recompose per message** (compose whole
strip onto a fresh `CGCanvas`, swap the image). That bounds the `StretchDIBits` re-decode debt
(roadmap "Debt handed to Plan 4") to once per message rather than once per frame — acceptable
for MVP; incremental composition and decode caching become a Plan 4 performance task, not an MVP
blocker.

### 3.4 Threading (feeds risk R1)

Every `cc_*` call is process-global single-threaded. `ProtocolSession` already serializes on its
private `sessionQueue`; `ProtocolStripBridge` documents a **stronger** rule: no `cc_session_*`
call and no `cc_strip_*` call may ever be in flight at the same time, and Plan 3's only proven
pattern is *drain the session fully, then feed the bridge*
(ProtocolStripBridge.swift:9-22). A live app cannot drain-then-render — it must interleave
(serialized) feed_bytes and strip composition. Whether serialized interleaving is actually safe
(shared avatar registry / engine context state) is **unverified** — top risk R1.

## 4. Resources

### 4.1 Comic art

| Source (read-only) | Contents | Size |
|---|---|---|
| `v2.5-beta-1-modern/comicart/` | **25 `.avb` avatars** (anna, armando, bolo, buck, connor, cro, dan, denise, glenda, hugh, jordan, kirby, lance, lynnea, margaret, mike, pedagog, rainbow, susan, tiki, tongtyed, tux, veronica, waf, xeno) + **7 `.bgb` backdrops** (buckroom, clouds, field, pastoral, room, space, yellow) | 1.5 MB |
| `v2.5-beta-1-modern/artpack1/` | 10 `.avb` (6 unique: kevin, kwensa, maynard, rebecca, sage, scotty; 4 dupes) + 2 `.bgb` (den, volcano) | 3.6 MB (incl. `archive/` dupes) |

Recommendation: bundle `comicart/` (all 32 files) as a folder reference in the app bundle
(`Bundle.main.resourceURL/comicart`); add the 6 unique artpack1 avatars + 2 backdrops too (total
still ~3 MB — trivial). Copy at build time from the reference tree; **never** reference the
read-only tree at runtime.

### 4.2 Sounds — finding: nothing to bundle

Zero `.wav`/`.mid` files exist in any tree in the repo. The original's sound model
(sounddlg.cpp): a *sound path* defaulting to the Windows media directory
(`SetSoundPath` → `GetWindowsMediaDirectory`, sounddlg.cpp:362-366; utils.cpp:5), from which it
plays by filename. Event usage inventory:

| Event | Original call | Citation |
|---|---|---|
| Inbound `/sound` (`CC_EV_SOUND` → `.sound`) | `bFindAndPlaySound(szFile, FALSE, FALSE)` gated on `m_bPlaySounds` | protsupp.cpp:1415-1416 |
| Send sound (compose toolbar / Sounds dialog) | `#SOUND` message + local play | saywnd.cpp:848, sounddlg.cpp:416-434 |
| Sound test button | `bFindAndPlaySound(strSound, TRUE, TRUE)` | sounddlg.cpp:444-452 |
| Logon notification match | `sndPlaySound("Default sound", SND_ASYNC)` (the system default beep) | notipage.cpp:970 |
| MIDI (`.mid`/`.rmi`) | `sndPlayMidiSound` MCI wrapper | utils.cpp:1142; **dropped** per spec §5 |

Mac plan: create `~/Library/Application Support/Comic Chat/Sounds/`; play inbound `.sound(file:)`
via `AVAudioPlayer` only if a matching file exists there (this matches the original — sounds are
never downloaded, see plan3 command-surface.md:288); use `NSSound.beep()` for notification-style
events. Spec §5's "original WAV assets bundled" should be amended to "user sounds folder;
nothing bundled" (or Tim supplies a period-correct set of Windows media WAVs out of band).

### 4.3 Default character/backdrop + art lookup API

Original defaults: random avatar when unset (`GetNextAvatarName`, setupdlg.cpp:601-606,
avatar.cpp:648), backdrop "FIELD" (chat.rc:2327 → `field.bgb`, chat.cpp:505). The Swift loaders
are **all explicit-path**: `AvatarFile(path:)`/`BackdropFile(path:)` (ArtFile.swift:33/72),
`Strip.addParticipant(nick:avbPath:)` (Strip.swift:78), `AvatarResolver(comicartDir:)`
(ProtocolStripBridge.swift:39). `cc_set_art_dirs` exists (comicchat.h:12) but is only used by a
selftest — Swift owns lookup (consistent with `CC_NO_DIRSCAN`, Package.swift:23). So spec §6's
"user-addable characters folder" is a pure-Swift search order:
`[Application Support/Comic Chat/Characters, bundle comicart/]` resolved before any engine call.
`buildCatalog(artDir:)` (Catalog.swift:21) already enumerates a directory with pose counts/names
— the character picker can be built on it (plus the still-missing `cc_avatar_icon_image`
accessor for proper icon thumbnails, Plan 1 debt).

## 5. Settings inventory

### 5.1 Every persisted value in the original

From `LoadFromReg` (setupdlg.cpp:287-623) / `SaveToReg` (setupdlg.cpp:626-853), HKCU root key
plus subkeys; HKLM holds only install paths (`BaseDir`/`ArtDir`, setupdlg.cpp:294-320).

| Registry value | Type | Meaning | Mac disposition |
|---|---|---|---|
| `XFrame/YFrame/CXFrame/CYFrame/Maximized` | DWORD×5 | main window frame | native `windowResizability`/frame autosave — no key needed |
| `UPNLWidth/UPNLHeight/UnitsWide` | DWORD | unit panel size + panels per row (`CUnitPanelPage`) | `comic.panelsPerRow` post-MVP; size stays engine default |
| `FavoritesDir/LastFavorite/FileTXDir` | SZ | favorites + file-transfer dirs | drop (favorites defer; DCC out of scope) |
| `IRCServer` | SZ | last service/server | **MVP** `connect.server` |
| `IRCChannel` | SZ | last room | **MVP** `connect.room` |
| `Name` | SZ | nick (also seeds myName) | **MVP** `persona.nick` |
| `RealName`, `Email`, `HomePage`, `Profile` (RTF), `AwayMsg` | SZ | persona/identity | post-MVP `persona.*` (profile as plain string) |
| `Character` | SZ | avatar name | **MVP** `persona.character` (hardcoded default until picker) |
| `Backdrop` | SZ | last backdrop | **MVP** `comic.backdrop` (default "field") |
| `ShowComicView` | DWORD | comic vs text view | post-MVP `view.comicMode` (Bool, true) |
| `ComicsData` | DWORD | **send annotations toggle** | post-MVP `protocol.sendComicsData` (Bool, true) |
| `MemberListStyle` | DWORD | icon vs list members | post-MVP `view.memberIcons` |
| `PromptForSave` | DWORD | prompt to save transcript on close | post-MVP `save.prompt` |
| `AcceptWhispers` | DWORD | accept inbound whispers | post-MVP `protocol.acceptWhispers` (true) |
| `AutoDownloadChars/AutoDownloadBackdrops` | DWORD | fetch announced art via HTTP | post-MVP `art.autoDownloadAvatars/Backdrops` (with the URLSession work) |
| `FloodControl`, `RulesControl` | packed DWORD | flood limits | defer (engine protects; revisit if needed) |
| `ComicsFont` | BINARY LOGFONT | comic balloon font | post-MVP `comic.fontName/fontSize` (default Comic Sans MS 12 — but see risk R5) |
| `ComicsColor` | BINARY COLORREF | comic text color | defer |
| `SoundPath` | SZ | sound folder | post-MVP `sounds.folder` (App Support default) |
| `AutoGreeting`, `AutoGreetType` | SZ+DWORD | auto-greeting macro | defer |
| `TextFonts`, `HighlightedTextFonts` | BINARY CHARFORMAT×18+ | text-view per-type fonts | defer (single text-view font first) |
| `HostHighlight`, `TextSpacing` | DWORD | text view display | defer |
| `ShowArrivals` | DWORD | join/part lines in transcript | post-MVP `view.showArrivals` (true) |
| `AllowInvites`, `AllowFileTXs`, `AcceptNMCalls`, `ShowIdentity`, `ListRegistered` | DWORD | misc protocol perms | defer (`AllowFileTXs`/`AcceptNMCalls` out of scope entirely) |
| `PlaySounds`, `NoMIDI` | DWORD | sound gates | post-MVP `sounds.enabled` (true); MIDI dropped |
| `Flags0/Flags1` | DWORD bitfields | first-run + assorted | fold into individual keys as needed |
| `WhisperDims`, `NotifDims` | BINARY RECT | aux window frames | native frame autosave |
| `OnConnect` | DWORD | post-connect action radio | defer (MVP always joins the given room) |
| `ServersMigrated` | DWORD | migration marker | drop |
| `Macros\*` subkey | MULTI_SZ×10 | text macros | defer beyond Plan 4 (setupdlg.cpp:244-285) |
| server directory (`m_listChatServices`) | subtree | networks/servers/ports | defer; MVP is a free-text host field (chatsrv.cpp owns this; not lifted) |
| rules/notifications (`m_dynaRules`/`m_dynaNotifs`) | subtrees | automation + logon notifications | defer beyond Plan 4 |

### 5.2 The encoding toggle (spec §4.5)

Not in the original (it was implicitly CP-1252/MBCS). New keys:
`connection.encoding` = `"cp1252"` (default) | `"utf8"`, stored per saved connection (MVP: one
global default + the connect-sheet picker overriding per session). Plumbs straight into
`ProtocolSession(encoding:)` / `cc_session_config.encoding` (comicchat.h:328) — zero engine work.

### 5.3 Proposed MVP UserDefaults key list

| Key | Type | Default | MVP? |
|---|---|---|---|
| `connect.server` | String | "" | ✓ |
| `connect.port` | Int | 6667 | ✓ |
| `connect.room` | String | "" | ✓ |
| `connect.encoding` | String | "cp1252" | ✓ |
| `persona.nick` | String | "" | ✓ |
| `persona.character` | String | "anna" (or random-on-first-run) | ✓ |
| `comic.backdrop` | String | "field" | ✓ |
| `protocol.sendComicsData` | Bool | true | post-MVP (hard-true in MVP) |
| `protocol.acceptWhispers` | Bool | true | post-MVP |
| `sounds.enabled` | Bool | true | post-MVP |
| `view.comicMode` | Bool | true | post-MVP |
| `view.showArrivals` | Bool | true | post-MVP |
| `comic.panelsPerRow` | Int | engine default | post-MVP |
| `art.autoDownloadAvatars` | Bool | true | post-MVP |
| `sounds.folder` | String | App Support path | post-MVP |

Recommend a typed `Settings` struct in ComicChatKit (testable; `UserDefaults` injected) rather
than raw `@AppStorage` scattered through views.

## 6. Menu bar + app chrome for MVP

Original menus: chat.rc:156-289 (main), duplicated reduced sets for text-view/whisper contexts
(chat.rc:295+). Mapping to Mac conventions:

| Mac menu | Items (MVP in **bold**) | Original source |
|---|---|---|
| **ComicChat** | **About**, Settings… (post-MVP), **Quit** | ID_APP_ABOUT, ID_VIEW_OPTIONS (chat.rc:213, 289) |
| **File** | **New Connection…** ⌘N (opens connect sheet), **Close** ⌘W; post-MVP: Save Transcript… ⌘S (JSON), Export PNG/PDF…, Print… ⌘P | ID_SESSION_CONNECT/OPEN/SAVE/PRINT (chat.rc:158-167); `.ccc` open dropped (spec §5 deviation) |
| **Edit** | **standard** (Cut/Copy/Paste/Select All — free from SwiftUI) | chat.rc:171-181; Clear History post-MVP |
| **View** | Comic Strip / Plain Text toggle (post-MVP), Member List toggle (post-MVP) | ID_VIEW_COMICS/ID_VIEW_TEXT (chat.rc:195-196), member list popups |
| **Room** | **Enter Room…** ⌘J, **Leave Room**, **Disconnect**; post-MVP: Room List…, Create Room…, Room Properties… | chat.rc:224-234 (ID_SESSION_NEWROOM/LEAVE, ID_CHATROOM_LIST, ID_ROOM_CREATEROOM, ID_CHANNELPROPS — the last three map to the not-yet-called `cc_session_create_room`/`set_mode` fns) |
| **Member** | post-MVP: Whisper Box…, Get Profile, Ignore, Away toggle; defer: invite/kick/ban admin items (wired to `cc_session_kick/invite/ban`), lag time, local time, version | chat.rc:236-255; Send File/NetMeeting dropped (out of scope) |
| **Window** | **standard** SwiftUI window menu | replaces MDI cascade/tile (chat.rc:262-268) |
| **Help** | stub | WinHelp dropped |

Dropped menus: **Format** (color/bold/italic/fixed/symbol — RTF formatting, defer beyond Plan 4,
chat.rc:215-222), **Favorites** (chat.rc:257-260, defer), Macros/Automation/Logon-Notifications
(View menu extras, defer), "Microsoft on the Web" (chat.rc:274-284, drop).

MVP toolbar: none (menu + in-window controls suffice; native toolbar post-MVP if wanted).

## 7. Top risks / open questions for the planner

- **R1 — Live interleaving of `cc_session_*` and `cc_strip_*` is unproven.**
  `ProtocolStripBridge`'s contract says *collect the full event stream first, then compose*
  (ProtocolStripBridge.swift:9-22); Plan 3 only ever ran drain-then-render. A live app
  interleaves them by nature (serialized on one queue, but alternating). **First Plan 4
  discovery/test task: prove (or fix) serialized interleaving** — e.g. a `.serialized` test that
  alternates `feed_bytes` and `bridge.apply`/`compose` and byte-compares against the
  drain-then-render result. If the engine's shared globals (avatar registry, engine context)
  make it unsafe, the fallback is a snapshot design: buffer events, and rebuild a fresh
  `Strip` from the full event list on each render tick while `feed_bytes` is quiescent.
- **R2 — No outbound avatar announce.** Inbound `# Appears as` → `CC_EV_APPEARS_AS` is lifted,
  but there is **no `cc_session_*` API to send our own announce** (comicchat.h:457-502 has no
  announce fn), which the original broadcasts on every self-join and avatar change
  (plan3 command-surface.md §"# Appears as", protsupp.cpp:833-877). Without it, real Windows
  peers see us as a default character — a §8 acceptance failure. Needs a small engine addition
  (`cc_session_announce_avatar`) or a ruled-safe Swift-side raw send; also the *reply-on-receipt*
  rule (private announce back to unknown comic users, protsupp.cpp:870-877) needs a home
  (engine event → Swift responds).
- **R3 — `cc_strip` has no change-avatar/participant-refresh API.** Post-join `# Appears as` and
  avatar downloads can't update an existing participant
  (ProtocolStripBridge.swift:77-81 documents the no-op). Known handoff debt; MVP tolerates it,
  the avatar-download feature does not.
- **R4 — XcodeBuildMCP macOS + scaffolding workflows are not currently enabled** (only
  simulator tools are in this session's tool list). Enable them in XcodeBuildMCP config before
  execution or plan around `xcodebuild`/`open`/`screencapture` via Bash.
- **R5 — Comic Sans MS availability.** Spec §4.3 says "macOS ships Comic Sans MS" — it ships
  with some macOS installs (supplemental fonts) but is not guaranteed on a clean modern system.
  Verify on the target machine early; if absent, pick/bundle a metric-compatible fallback and
  re-baseline layout goldens.
- **R6 — Real-CoreText layout metrics decision** (handoff discovery task 3, not this report's
  question, but it gates the comic view's visual quality): MVP can ship on the current
  fake-metrics layout + real-metrics drawing divergence ("text sits high"), but the planner must
  schedule the switch (incl. replacing `CGCanvas`'s three `TEXTMETRIC` heuristics,
  roadmap:125-132) somewhere inside Plan 4.
- **R7 — One room at a time.** Engine models a single room's protocol state; MVP embraces it.
  If Plan 4's UX wants multiple simultaneous rooms (original MDI model), that's an engine-level
  decision (per-room sessions vs engine change) — recommend explicitly deferring multi-room
  beyond Plan 4 and designing the window chrome as one-room-per-connection.
- **Open Q1 — sounds source.** Nothing to bundle (§4.2). Decide: ship with an empty user sounds
  folder + system beep only, or have Tim provide period WAVs (e.g. from a Windows install he
  owns) dropped into Application Support.
- **Open Q2 — where whisper UI lands.** Original = one tabbed box (not per-peer windows, §0).
  Recommend matching the original (also simpler); spec §5's wording should be amended.
- **Open Q3 — connect sheet vs original setup flow.** MVP drops the on-connect action radio and
  server directory. Confirm Tim doesn't want the server list UI in Plan 4 at all (the
  registry-backed `chatsrv.cpp` directory was never lifted and shouldn't be).
