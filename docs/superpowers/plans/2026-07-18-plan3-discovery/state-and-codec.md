# State Ownership & Annotation Codec Discovery — Plan 3 (Protocol)

Scope: chatsrv.cpp (2833 lines), ircproto.cpp (1457), ircsock.cpp (~2600), protsupp.cpp (5260),
userinfo.cpp/.h, chatprot.h, chatdoc.cpp/.h, format.cpp (1507), avatario.cpp (98), histent.cpp,
plus the compiled-in external TU artifacts/core/ccommon.cpp (discovered mid-trace). All findings
are read-only observations of `/Users/timbroddin/Projects/comic-chat/v2.5-beta-1-modern/` compared
against the lifted engine at
`/Users/timbroddin/Projects/comic-chat/macos/ComicChatKit/Sources/cchat-engine/`.

## 0. Preliminary findings: compile status + naming corrections

Verified against `chat.mak` (`OBJS` list, chat.mak:54-138):

| File | Compiled? | Evidence |
|---|---|---|
| chatsrv.cpp, ircproto.cpp, ircsock.cpp, protsupp.cpp, userinfo.cpp, format.cpp, avatario.cpp, chatdoc.cpp, histent.cpp | YES | `chatsrv.obj` chat.mak:82, `ircproto.obj` :95, `ircsock.obj` :96, `protsupp.obj` :108, `userinfo.obj` :128, `format.obj` :92, `avatario.obj` :62, `chatDoc.obj` :80, `histent.obj` :93 |
| **nmproto.cpp** | **NO — dead code** | chat.mak:11 "NetMeeting (nmproto) are excluded"; no `nmproto.obj` in OBJS. Everything CNmProto is behind `#ifdef CB32SUPPORT` anyway (ircproto.cpp:19-21, 48-50, 64-67). Do not analyze as live. |
| ccommon.cpp | YES, but it's a 6-line wrapper | v2.5-beta-1-modern/ccommon.cpp:6 is literally `#include "..\core\ccommon.cpp"` — the **real TU is `/Users/timbroddin/Projects/comic-chat/artifacts/core/ccommon.cpp` (1260 lines)**, header `/Users/timbroddin/Projects/comic-chat/artifacts/inc/ccommon.h`, resolved via `/I "$(ARTINC)"` (chat.mak:42-43). This file supplies the UTF-8 nick/channel codec and CTCP low-level quoting the protocol depends on (see §3.4). |

Corrections to the spec's guesses:

1. **`chatsrv.cpp` holds ZERO nick/channel session state.** It is the server *directory*: registry-persisted
   server/network/service lists (CChatServer/CChatServerGroup/CChatServiceList, chatsrv.h:20-161) plus
   the multi-server connection racer `CChatServiceConnector` (chatsrv.h:246-319) that resolves DNS on a
   thread and hands the winning socket over. `grep -n "nick\|channel"` over all 2833 lines of
   chatsrv.cpp: **zero hits**. The real "chatsrv vs ircproto" split for Q1 is actually
   **ircsock.cpp (inbound parse + state writes) / protsupp.cpp (session-state functions + globals) /
   CChatDoc (per-channel containers) / theApp (own identity)** — see §1.
2. **`SzGetAnnotations` does not exist** anywhere in the tree (grep: zero hits). The annotation encoder
   is `static BOOL bInsertAnnotations(...)` at protsupp.cpp:3057; the decoders are `ProcessUDIData`
   (protsupp.cpp:1485) and the inline-parse block of `ProcessSay` (protsupp.cpp:1566-1612).
3. `format.h` is absent from v2.5-beta-1-modern/ (original resolved it from `$(ARTINC)` =
   artifacts/inc/); the lifted engine's copy documents this provenance
   (engine/format.h:1-7) and is the authoritative constants reference.

## 1. Question 1 — where nick/channel state lives

### 1.1 The actual architecture (4 layers + globals-as-aliases)

- **`CIrcSocket serverConn`** — a single global socket object (ircproto.cpp:27) shared by *all*
  rooms/documents (every `CIrcProto` points at it: ircproto.cpp:40). ircsock.cpp is the entire
  **inbound** path: `OnReceive` → line parse (`IRCPARSE`) → giant `switch` over commands
  (`HandleCommand`) and numerics (`HandleResultCode`, ircsock.cpp:1849). Owns connection-level state:
  `m_iConnected`, `m_bIrcXServer`, `m_nMaxMsgLength`, the pending-query queue `m_queries`.
- **`CIrcProto : public CRoomInfo`** (ircproto.h:10) — one per document (`doc->m_proto`, created by
  `NewDefaultProto`, ircproto.cpp:37-44; plus one default instance in `cui.m_pvIrcProto`,
  ircproto.cpp:52). ircproto.cpp is almost purely the **outbound** command formatter (JOIN/PART/NICK/
  KICK/MODE/TOPIC/PRIVMSG/NOTICE/DATA sprintf'ing, ircproto.cpp:475-1219) plus nick/channel/string
  encoding (§3.3). The base `CRoomInfo` **is** the room-properties record (chatprot.h:13-22).
- **`CChatDoc`** — one per channel window. Owns the per-channel user containers:
  `m_mapNickToPtr` (chatdoc.h:29), `m_allChannelPuis` (chatdoc.h:30), `m_puiSelf` (chatdoc.h:33),
  plus `m_proto` (chatdoc.h:23).
- **`theApp` (CChatApp)** — own identity: `m_myNick` (chat.h:124), `m_myName`, `m_myIdent`.
- **Globals are aliases to the *active* document's state, re-pointed on window activation**:
  `CChatDoc::LoadDocData` (chatdoc.cpp:2034-2041) does `currentRoom = m_proto; g_puiSelf =
  m_puiSelf; g_mapNickToPtr = &m_mapNickToPtr;`. The convenience macros `strCurrentChannel`/
  `dwCurrentChannelMode`/etc. (chatprot.h:100-104) all dereference `currentRoom`. Two globals are
  NOT aliases but real owners: `externalPuis` (users known outside any room; `ExternalPui`,
  protsupp.cpp:501-541) and `g_rgpuiWhisperees` (protsupp.cpp:69).

### 1.2 State-ownership table

| State item | Canonical owner | Written by | UI read path |
|---|---|---|---|
| **Own nick** | `theApp.m_myNick` (chat.h:124); accessor `GetMyNickName()` setupdlg.cpp:1023 | Pre-connect: `SetMyName` setupdlg.cpp:1075 (char dialog/registry, setupdlg.cpp:390). Post-connect **only on server echo of NICK**: ircsock.cpp:1576-1612 → `NickEntry` → `ProcessNick` → `ReinstallPui` protsupp.cpp:2040 → `SetMyNameNick` setupdlg.cpp:1102 (or directly ircsock.cpp:1602-1603 if no pui yet). Request path `ChatChangeNick` (ircproto.cpp:901-909) does NOT update locally. Collision → `TryNewNick` dialog (protsupp.cpp:177-212, from ircsock.cpp:3134) | status window banner ircsock.cpp:1605-1608; everywhere via `GetMyNickName()` |
| **Nick list per channel** | `CChatDoc::m_mapNickToPtr` (nick→`CUserInfo*`) + `CChatDoc::m_allChannelPuis` (chatdoc.h:29-30) | `CIUserJoin` protsupp.cpp:582-584 (writes through the `g_mapNickToPtr` alias + `AddHead`); rename `ReinstallPui` protsupp.cpp:2035-2042; **part is a soft delete** — `CIUserPart` protsupp.cpp:664-686 only sets `UF_DEPARTED` and removes the list-view row (map entry deliberately kept, comment :667); full teardown `DestroyUserInfos` protsupp.cpp:357-380 | The `CMemberList` list-view is populated *in the same call* (`AddToMembersList` protsupp.cpp:438-455, `lv.lParam = (long) pui` :454) — the UI control is a parallel copy keyed by pui pointer (`FindMemberListIndex` protsupp.cpp:647-654 finds rows by `LVFI_PARAM`) |
| **Self-in-channel** | `CChatDoc::m_puiSelf`, alias `g_puiSelf` (userinfo.h:159) | `CIUserJoin` protsupp.cpp:557-568 (`IsSelf()` match → binds `MyAvatar()` too); re-aliased chatdoc.cpp:2039 | admin-menu enable etc. (`doc->m_puiSelf->IsOperator()`, actions.cpp:799) |
| **User metadata** (`CUserInfo`: nick, full ident, screen name, flags op/owner/voice/away/ignored/comic-user/spectator, avatarID, avatar real name+URL, flood counters, `m_udi` display info) | the `CUserInfo` object held by the doc containers (userinfo.h:64-157) | op/voice/owner: `ChatChangeAdmin` protsupp.cpp:2072-2100 ← MODE parse ircsock.cpp:349-363; spectator recompute `UpdateSpectators` ← ircsock.cpp:394; away: `DoUserAway` protsupp.cpp:2197; avatar: `ProcessComment` "# Appears as" → `ChangeAvatarEntry` protsupp.cpp:894; per-utterance `m_udi`: `ProcessUDIData`/`ProcessSay` (§3.2) | member list icons/labels (LPSTR_TEXTCALLBACK, protsupp.cpp:452), panel layout reads `m_udi` + avatarID (panel.cpp:292-335) |
| **Room properties** (encoded+pretty channel name, topic, topic formatting, modes DWORD, max users, password, PROP CLIENT data) | `CRoomInfo` base of the doc's `CIrcProto` (chatprot.h:13-22; `m_strClientData` ircproto.h:16) | **ircsock.cpp writes them directly** — `ParseChannelMode` writes `m_dwModes`/`m_dwMaxUsers` (ircsock.cpp:338, 388-389), TOPIC/332 writes `m_strTopic` (ircsock.cpp:1745, 1797, 2241), reset on join (ircsock.cpp:2145, 1397-1399); PROP CLIENT change → `CIrcProto::HandleClientDataChange` ircproto.cpp:737-767 | via `currentRoom` macros chatprot.h:100-104; channel-props dialog round-trips them (protsupp.cpp:4155-4195) |
| **Whisper targets** | `g_rgpuiWhisperees` (`CPtrArray` of `CUserInfo*`, protsupp.cpp:69) | UI selection: saywnd.cpp:754 (`GetSelectedPuis`), whisprbx.cpp:517-518; slash-command protsupp.cpp:2582-2583; cleared autopage.cpp:126. **Talk-to targets for ordinary says live in the member-list selection itself**: `MListTalkTosToPuiself` (userinfo.cpp:85-105) walks `LVNI_SELECTED` rows and rebuilds `puiSelf->m_udi.m_talkTos` from item lParams at send time (called from `bChatSendText` protsupp.cpp:3229) | it *is* UI state (list selection); whisper box mirrors it |
| **Connection status** | composite: `serverConn.m_iConnected` + per-room `CIrcProto::m_bInRoom` (`Get/SetConnectionStatus` protsupp.cpp:214-253) | `SetConnectionStatus` calls from connect/join/part flows (e.g. ircproto.cpp:776-777) | status bar (`SaveConnectStatus`/`UpdateStatus` protsupp.cpp:229, 3653) |
| **Server directory** (networks, servers, ports, auth, last-used) | `theApp.m_listChatServices` (chatsrv.h:123-161), registry-backed | chatsrv.cpp only | connect dialog combo (`CChatServiceComboBox`, chatsrv.h:323) |

### 1.3 Direction of call flow

- **Inbound** (state updates): `serverConn` parse switch → *either* direct writes to `doc->m_proto`
  room properties (ircsock.cpp:338, 388-389, 1745…) *or* protsupp free functions, mostly via the
  **history-entry indirection**: ircsock creates `JoinEntry`/`PartEntry`/`NickEntry`/`SayEntry` and
  calls `AddAndExecute` (ircsock.cpp:1387 JOIN, 1596 NICK, 1687 PART, 1773 QUIT, 2537 NAMES-353 via
  `bSingleJoin` ircsock.cpp:270-276); each entry's `Execute` calls `CIUserJoin`
  (histent.cpp:261-265), `CIUserPart` (:332-334), `ProcessNick` (:601). Text/data messages route
  `OnTextMsg` (ircsock.cpp:1652/1658/1841 → protsupp.cpp:4358) and `OnDataMsg` (ircsock.cpp:1315/1319
  → protsupp.cpp:4374). **Inbound never goes through the `CIrcProto` virtuals — the class is
  bypassed entirely on receive.**
- **Outbound**: UI (saywnd/menus/whisper box) → `bChatSendText` (protsupp.cpp:3124) →
  `CRoomInfo` virtuals (chatprot.h:38-94) → `CIrcProto::bChatSendToTarget` (ircproto.cpp:481) /
  `bExecuteQuery` (ircproto.cpp:1034) → `SendMessageText` → `m_pSock->Send` (ircproto.cpp:475-478).
  So **chatprot.h's virtual interface is outbound-only**; `chatsrv.cpp` touches the protocol layer
  only at connect time (socket handoff from `CChatServiceConnector`).

### 1.4 Conclusion for the Swift bridge

The already-lifted engine consumes exactly: `CUserInfo` identity + `m_udi`
(pose/emotion/uModes/talkTos) + avatarID (panel.cpp:292-335 and downstream). Everything else in the
table is session state living in ircsock/protsupp/CChatDoc/theApp — **none of which is scheduled to
be lifted** (they are MFC-document/ListView/registry entangled through and through; e.g. the member
list's canonical selection state lives *inside the CListCtrl*, userinfo.cpp:97-104). Therefore the
**Swift bridge event model must TRACK: own nick (including the echo-only update rule), per-room
membership (nick→session-user map with soft-departs), user metadata (op/voice/owner/away/ignored/
avatar name+URL), room properties (topic+formatting, modes, max users, key, PROP CLIENT keystring),
whisper/talk-to target sets, and connection status.** Engine-internal state remains: avatar
pose/emotion state (`CAvatarX`), and the per-utterance `m_udi` record — the bridge constructs
`m_udi.m_talkTos` by looking up its session table and handing the engine `CUserInfo*`-derived
values, exactly as `GetTalkTos` does today (§2). The engine already stubs `GetMyNickName()`
(cc_link_stubs.cpp:52-61) — Plan 3 replaces that stub with a bridge-provided value.

## 2. Question 2 — talkTos wire-serialization audit: **CONFIRMED (never raw; wire uses nick strings)**

`m_udi.m_talkTos` is a `CDWordArray` whose elements are always `(DWORD) pui` pointer casts
(userinfo.h:58; e.g. protsupp.cpp:793, 1081, 1099, 1459, 1464, 1980; histent.cpp:214;
userinfo.cpp:103). Exhaustive audit of every serialization boundary:

### 2.1 Outbound (memory → wire): always dereference-to-nick

| Site | What it emits |
|---|---|
| `GetAddressees` protsupp.cpp:3006-3020 | casts each element back to `CUserInfo*` (:3014) and appends `pui2->GetName()` (encoded nick) or `GetScreenName()` (:3015) — **string**, never the DWORD |
| `GetWhisperedAddressees` protsupp.cpp:3023-3034 | same, from `g_rgpuiWhisperees` (:3029) |
| `bInsertAnnotations` protsupp.cpp:3084-3093 | appends `"T"` + comma-joined nick strings from the two functions above into the annotation block |
| history save (.ccc) `SayEntry::FormatOtherArgs` histent.cpp:148-157 | `" T:"` + comma-joined `GetName()` strings |

Both emitters clip to the first 5 addressees (`min(GetUpperBound(), 4)`, protsupp.cpp:3009, 3025) —
a wire-visible truncation the port must reproduce.

### 2.2 Inbound (wire → memory): always nick-string → session-table lookup

| Site | What it parses |
|---|---|
| `GetTalkTos(doc, CUserInfo*, str)` protsupp.cpp:1066-1083 | tokenizes nick strings, `LookupPui(szName, doc)` → stores `(DWORD) pui` (:1079-1081) |
| `GetTalkTos(doc, CDWordArray*, str)` protsupp.cpp:1086-1101 | same, comma-separated (:1097-1099) |
| `ProcessUDIData` (DATA/CCUDI1 path) protsupp.cpp:1532-1536 | `'T'` prefix → `GetTalkTos` |
| `ProcessSay` inline `(#...)` path protsupp.cpp:1595-1603 | `'T'` prefix → `GetTalkTos` |
| IRCX `WHISPER` verb ircsock.cpp:1832-1844 | `args[2]` nick list → `GetTalkTos` into a local array → `OnTextMsg(..., &talkTos)` → `IdentifyWhispers` protsupp.cpp:1448-1467 copies element-by-element |
| history load (.ccc) `SayEntry::ReadOtherArgs` histent.cpp:205-216 | `"T:"` → `LookupPui` → `(DWORD) pui` |

### 2.3 Non-boundary copies (memory ↔ memory only, listed for completeness)

`CopyPtrArrayToCWDordArray` (protsupp.cpp:689-694, used :3226/:3372), `CopyArray`
(histent.cpp:59/82), `MListTalkTosToPuiself` (userinfo.cpp:97-104, reads ListView lParams),
kicker→kickee synth (protsupp.cpp:1979-1980), AutoGreet (protsupp.cpp:792-793). None writes a DWORD
to any wire or file.

**Verdict: CONFIRMED.** The wire (and the .ccc file format) represent addressees exclusively as
comma-separated (encoded) nickname strings behind the `T` / `T:` prefix; `talkTos` DWORDs are
process-local pointer handles reconstructed on every hop via `LookupPui`. The layout-engine ruling
(pointer-derived DWORD equality compares + one session-table-recovery dereference, panel.cpp:302,
328, 334-335) is therefore safe: no truncated pointer can ever leak in from or escape to the wire.
The Swift bridge repeats the same recipe: nick strings on the wire → session lookup → opaque
per-user handles into the engine.

## 3. Question 3 — the protocol annotation codec map

### 3.1 format.cpp: what's already lifted vs what remains

Diffing original format.cpp (1507 lines) against the lifted
engine/format.cpp (1565 lines): the lift is the whole file with 7 regions wrapped in
`#ifndef CC_NO_UI` (engine/format.cpp:39, 396, 583, 1247, 1287, 1381, 1466, 1530). **Key
correction to the spec's assumption: the wire-format text codec is NOT in the "remaining half" —
it is already lifted and LIVE** (and self-tested: bridge/cc_selftest.cpp:974-1033 exercises
`SzControlLess`).

| format.cpp function (orig line) | Status in engine | Role |
|---|---|---|
| `SzSkipOneFormat` :23, `nResettingSequence` :252, `SzControlLess` :303, `nFillFormatting` :459, `SzControlFull` :517 | **LIVE** (engine :44/:273/:324/:486/:544) | the inline control-code ↔ run-array codec (both directions) — the formatting half of the wire format |
| `GetColorCode` :862 / `GetRBGColor` :902 | **LIVE** | 16-color palette byte codec (§3.3) |
| run-array utilities (`AddFormat` :1036, `InsertFormat` :1049, `CopyFormatting` :953, `CopyLinksFormatting` :971, `bFormattingsEqual` :1015, `CutFormattingArray` :1114, `Pull/PushFormattingOffsets` :1140/:1185/:1201, `bSizorPresent` :1297, `bURLPresent` :1323, `FreeAndNullFormatting` :942) | **LIVE** | in-memory `MAKELONG(wFormat, wOffset)` run manipulation |
| `GetFormattedTextExtent` :692 | LIVE | measurement (Plan 2 territory) |
| `SzReplaceFormattedString` :375 | CC_NO_UI | RichEdit-clipboard string replace — UI, skip |
| **`PRGDWGetFormatting` :556** | **CC_NO_UI** | RichEdit selection → run array. The only protocol-adjacent wrapped function: it is the *input capture* end of the outbound pipeline. Its `CRichEditCtrl`/`CHARFORMAT` dependency is irreducible — **replace with native Swift input → run array, do not lift** |
| `IdentifyURLs` :1216, `MarkHotLinks` :1250, `FLaunchBrowser` :1338 | CC_NO_UI | URL highlight/launch — UI, skip |
| `bLOGFONTToCHARFORMAT` :1418, `MatchFont` :1478 | CC_NO_UI | GDI/RichEdit font interop — skip |

The clean split found in Plan 2 **continues**: nothing in the live half touches RichEdit/CHARFORMAT;
the wrapped half is wholly UI. No surprises.

### 3.2 The annotation codec proper (the real "not-yet-lifted half")

It lives in protsupp.cpp + avatario.cpp + ircproto.cpp, not format.cpp:

| Function | Location | Deps | Lift difficulty |
|---|---|---|---|
| `IndexToByte` / `ByteToIndex` | protsupp.cpp:1023-1032 | none (`value ± '0'`) | trivial |
| `SM2BM` / `BM2SM` | protsupp.cpp:1035-1063 | defines.h SM_*/BM_* only | trivial |
| `EmotionToBytes` / `BytesToEmotion` | avatario.cpp:69-87 (+ `emFloats[]` :45-64) | `CEmotion` (avatar.h, lifted) | **already lifted**, parked under `#ifndef CC_NO_PROTOCOL` (engine/avatario.cpp:70-93) — Plan 3 just stops defining the flag and provides `IndexToByte`/`ByteToIndex` |
| `bInsertAnnotations` (encoder) | protsupp.cpp:3057-3099 | `MyAvatar()` → `CAvatarX::GetIndices/GetEmotions` (avatar.cpp:769-799, lifted), `CUserInfo` (engine/userinfo.h present), `g_rgpuiWhisperees` | easy; the addressee-set inputs become bridge parameters |
| `GetAddressees` / `GetWhisperedAddressees` | protsupp.cpp:3006-3034 | CUserInfo + arrays | easy |
| `GetTalkTos` ×2 (decoder half) | protsupp.cpp:1066-1101 | `GetToken`, **`LookupPui` (session table)** | easy if `LookupPui` becomes a bridge callback |
| `ProcessUDIData` (DATA decoder) | protsupp.cpp:1485-1542 | CUserInfo.m_udi, `theApp.m_bVIPMode` (policy — hoist out), GetTalkTos | easy, minus 2 policy lines |
| inline-annotation parse block | protsupp.cpp:1566-1612 (inside `ProcessSay`) | same grammar as ProcessUDIData, duplicated | **needs extraction** — ProcessSay itself (1545-1920) is heavily entangled (ignore/flood/rules/history/UI) but the parse block is self-contained |
| `nGetBreakingPoint` (chunk splitter) | ircproto.cpp:398-472 | `nFillFormatting`/`SzSkipOneFormat` (lifted), `my_isspace` (defines.h:159), `CharNext`/`SzNextUTF8Char` | easy |
| `bChatSendToTarget` chunking loop | ircproto.cpp:481-698 | serverConn buffer/`m_nMaxMsgLength`, `GetMyNickName`+ident length accounting (:527-535), CTCP prefix table (ircproto.h:91-118) | moderate — the loop is pure string math; socket/буffer go behind the bridge |
| `EncodeNick`/`DecodeNick`/`DecodeNickForScreen` | ircproto.cpp:204-296 | Win32 MBCS↔wide + `bConvertWideStringToUTF8`/`...ToWide` | see §3.4 |
| `EncodeChan`/`DecodeChan`, `EncodeString`/`DecodeString`, `CIrcProto::EncodeString/EncodingType` | ircproto.cpp:300-395, 1261-1299 | same + `ConvertEncodingIn/Out` (protsupp.cpp:4557-4671, JIS tables jis2sjis.obj/sjis2jis.obj chat.mak:134-135) | see §3.4 |
| key-string codec (PROP CLIENT data) | protsupp.cpp:5073-5241 (`FindInKeyString`/`ChangeKeyString`/`GetValueFromKeyString`/`EnumKeyString`) | CString only | trivial |
| tokenizers `GetToken`/`GetToken1`/`GetToken2`/`bForEachWord` | protsupp.cpp:257-422 | none | trivial |
| data-message grammar (encoder `ChatAnnounceNewAvatar` protsupp.cpp:817-843; decoder `ProcessComment` protsupp.cpp:846-1020) | protsupp.cpp | grammar itself clean; bodies interleave policy (ignore/flood/operator checks) and actions (history entries, avatar download) | extract grammar, leave policy to bridge |

### 3.3 The annotation wire format (as far as code reading supports)

**Per-utterance display-info block** (encoder protsupp.cpp:3077-3096; decoders §3.2):

```
#G<gp><ge><gi>E<ep><ee><ei>[R]M<m>[T<nick>[,<nick>...]]
```

- Every `<x>` byte is `IndexToByte(value)` = `value + '0'` (protsupp.cpp:1023-1026). No length
  fields, no version field; the decoder is a tolerant left-to-right scan where every prefixed group
  is optional (protsupp.cpp:1503-1536).
- `G` group = **torso/gesture**: `<gp>` pose index from `CAvatarX::GetIndices` (avatar.cpp:769),
  `<ge>` emotion index 1-17 into `emFloats[]` (avatario.cpp:45-64: HAPPY, COY, BORED, SCARED, SAD,
  ANGRY, SHOUT, LAUGH, NEUTRAL=9, WAVE, POINTOTHER, POINTSELF, DOUBLEPOINT, SHRUG, 3QRWALK,
  SIDEWALK, 3QFWALK), `<gi>` intensity 0-10 (`m_intensity * 10`, avatario.cpp:78).
- `E` group = **face/expression**, same encoding (prefixes `CGESTUREPREFIX`='G',
  `CEXPRESSIONPREFIX`='E', defines.h:73-74).
- `R` = `m_bbReq` "requested" flag, presence-only (`CREQUESTEDPREFIX`, defines.h:75).
- `M<m>` = balloon mode, `IndexToByte(BM2SM(uModes))`: SAY=1, WHISPER=2, THINK=3, SHOUT=4, ACTION=5
  (defines.h:57-61; protsupp.cpp:1051-1063). Inbound anti-spoof: on private messages SAY/THINK are
  force-masked to WHISPER (protsupp.cpp:1588-1592, comment "anti-hacker line").
- `T` = addressees, comma-separated encoded nicks, max 5 (§2.1).
- Receipt sets `m_bbCooked` only if both intensity fields arrived (protsupp.cpp:1538-1539, 1606-1608).

**Two transports for the same block** (encoder switch ircproto.cpp:540-557; `bIncludeParenthesis`
protsupp.cpp:3242):

1. **IRCX**: out-of-band `DATA <target> CCUDI1 :#G...` followed by a plain
   `PRIVMSG <target> :<text>` (ircproto.cpp:542-549). `CCUDI1` = "Comic Chat User Display Info
   version 1" (ircproto.h:89; ircsock.cpp:1280 comment) — **the only versioning in the whole
   scheme is this token**. Inbound filter ircsock.cpp:1281-1321 → `OnDataMsg` → `ProcessUDIData`.
2. **Plain IRC**: the block is parenthesized and prepended to the message text itself:
   `PRIVMSG <target> :(#G...M<m>[T...]) <text>` (ircproto.cpp:554-556; decoder trigger
   `strncmp(szMesg, "(#", 2)` + `") "` search, protsupp.cpp:1566, 1605-1609).

**Message text formatting** (already-lifted codec, §3.1): inline toggle control characters
`chCtlBold` 0x02, `chCtlColor` 0x03, `chCtlLink` 0x0C, `chCtlFixedPitchFont` 0x11, `chCtlSymbol`
0x12, `chCtlItalic` 0x16, `chCtlUnderline` 0x1F (engine/format.h:12-18 — mIRC-compatible codes).
Color grammar `^C<fg>[,<bg>]` with 1-2 decimal digits each, value mod 16 (format.cpp:35-160);
palette = the fixed 16-color IRC table (format.cpp:862-939). In-memory form: `CDWordArray` of
`MAKELONG(wFormat, wOffset)` runs with bit-flags wBold 0x0100 … wLink 0x8000 + fg nibble<<4 + bg
nibble (engine/format.h:21-28). `SzControlLess` = wire→(plain text + runs); `SzControlFull` =
runs→wire.

**Chunking** (ircproto.cpp:535-691): messages longer than `m_nMaxMsgLength` minus the
receiving-side prefix (`:<nick>!<ident> ` — computed from own nick+ident length,
ircproto.cpp:523-535) are split at `nGetBreakingPoint` (ircproto.cpp:398-472): prefers whitespace
within the last 20% (`* 0.8`, :417), never splits inside a control-code sequence
(`SzSkipOneFormat` walk, :433-436), is DBCS/UTF-8 aware (:452). Formatting state carries across
chunks: the closing `wFormatEnd` of chunk N is re-emitted via `nFillFormatting` as the opening
sequence of chunk N+1 (:678, :640-654). CTCP payloads keep prefix/suffix per chunk (ACTION/SOUND/
AWAY re-prefixing incl. SOUND→ACTION downgrade for continuation chunks, :576-605, :682-689);
DBCS-932 sends only one chunk (`bOnlySendOneChunk`, :573).

**Out-of-band comment messages** (text `#`-prefix grammar; encoder/decoder protsupp.cpp:817-1020;
prefixes ircproto.h:83-90): `# Appears as <avatarName>[.<url>]` (avatar announce; URL may be
deferred `?` → receiver replies `# GetCharInfo`, ircproto.h:125-126); `# GetInfo` → reply
`# HeresInfo: <profile>` (sent with `BM_HERESINFO` chunk rules ircproto.cpp:592-595);
`# BDrop: <name>` and its delimiter-safe successor `# BDrop2: <name>[,<url>]` (protsupp.cpp:985-987
comment — the only wire-format evolution visible in code). Standard CTCP verbs use 0x01-framed
`ACTION/SOUND/VERSION/PING/TIME/EMAIL/URL/AWAY/DCC…` constants (ircproto.h:91-118).

**The .ccc file format is a sibling, not the same encoding**: `SayEntry::WriteSelf` saves
`say\t<nick>\t(G:%d %d %d E:%d %d %d R:%d M:%d [T:n1,n2])\t<control-full text>`
(histent.cpp:131, 145-158) — colon-suffixed `SZ*PREFIX` tags (defines.h:79-84, including a
file-only `C:` cooked tag) with **decimal atoi values instead of `+'0'` byte packing**
(ReadOtherArgs histent.cpp:163-218). Same fields, different framing — a ported codec must keep
both encoders distinct.

### 3.4 Dependency & untangling verdict

- **No RichEdit/CHARFORMAT/CString-UI entanglement in the codec itself.** The annotation grammar
  functions touch only `CString`/`CDWordArray` (mfc_compat shim already provides these), `CUserInfo`
  (engine/userinfo.h already lifted, 167 lines), and `CAvatarX` (lifted). The feared
  "annotation codec vs UI notification" untangling is real but coarse-grained: whole *callers*
  (`ProcessSay`, `ProcessComment`, `bChatSendText`) mix codec + policy + history + UI, while the
  codec cores they call are already separate functions (§3.2) — the one extraction needed is the
  inline-parse block protsupp.cpp:1566-1612, which duplicates `ProcessUDIData` and should be unified.
- **The heavyweight true dependency is the charset/encoding layer**, not UI:
  `EncodeNick`/`EncodeChan`/`EncodeString` need `bConvertWideStringToUTF8`/`bConvertUTF8StringToWide`/
  `SzNextUTF8Char` (artifacts/core/ccommon.cpp:218/364/500, header artifacts/inc/ccommon.h) — a
  hand-rolled UTF-8 codec implementing the IRCX extended-nick convention (leading `'`, wildcard
  escaping) — plus CTCP low-level quoting `bLowLevelQuoting`/`bLowLevelUnquoting`
  (artifacts/core/ccommon.cpp:945…, quote char 0x10 = `g_chLLQuoteCTCP`, ccommon.h:41), plus the
  DBCS path (`ConvertEncodingIn/Out` protsupp.cpp:4557-4671 + jis2sjis/sjis2jis tables) and
  Win32 `MultiByteToWideChar`/`CharNext`. **Note this TU lives outside v2.5-beta-1-modern** (pulled
  in via the 6-line wrapper ccommon.cpp:6 and `$(ARTINC)`, chat.mak:42-43) — Plan 3 must either lift
  artifacts/core/ccommon.cpp's string half (it is windows.h-light, mostly pure byte math) or
  reimplement UTF-8/quoting natively in Swift and keep only the byte grammar in C++. For a
  macOS-native port targeting UTF-8-clean modern servers, the DBCS/JIS path can plausibly be
  scoped out (flag for the plan decision).
- **protsupp.cpp full inventory** (what else lives there besides the codec) — by category, with
  representative anchors:
  - *Codec & tokenizers* (§3.2): ~600 lines total, clean.
  - *Session-state management*: `LookupPui`/`ExternalPui`/`CIUserJoin`/`CIUserPart`/`ReinstallPui`/
    `ProcessNick`/`ChatChangeAdmin`/`DestroyUserInfos` (:349-704, 2035-2100) — bridge-replaced (§1.4).
  - *Inbound message pipeline*: `OnTextMsg`/`OnDataMsg`/`ProcessSay`/`ProcessComment`/
    `IdentifyWhispers`/`PrepareTextAction`/`PrepareComicsAction`/`PrepareSound`/`OnKick`/`ShowSay`/
    CTCP `Reply*`+`Show*` pairs (:846-1467, 1545-2004, 4294-4396, 4955-4991).
  - *Outbound pipeline*: `bChatSendText`/`bChatSendSound`/`bSendWhispers`/`ChatAnnounceNewAvatar`/
    `ProcessNonComicsMsg`/`StrGetSoundAction` (:3102-3414, 3498-3516).
  - *Slash-command layer*: `CIrcProto::Slash*` + `ProcessSlashCommand` + syntax tables (:2208-3003).
  - *Room/connection orchestration*: `ChatInitialize`/`InitializeChannelConnection`/
    `InitializeServerConnection`/`bChatServerConnect`/`ChatServerDisconnect`/`ReconnectToServer`/
    `bSwitchToRoom`/`bProcessAddChannel`/`ChatCreateRoom`/`bInitEnterInfo`/`TryNewNick`/
    `Set/GetConnectionStatus` (:113-255, 3990-4290, 4447-4954).
  - *Pure UI notification/dialog*: `DoConnectDialog`/`DoChannelDialog`/`DoKickDlg`/`DoBanDlg`/
    `ShowIdentity`/`ShowBadChannelName`/`ActivateWindow`/`ConfirmAway`/`AcknowledgeInvite`/
    room-and-user-list dialog feeders (`StartRoomList`…`CreateUserFromWhoReply`)/NetMeeting
    launchers/ratings prompts/ignore-list + macros/rules glue (:706-815, 1269-1381, 2101-2206,
    3518-3964, 4026-4116, 4398-4446, 4993-5071).

  Net: of protsupp.cpp's 5260 lines, roughly 12% is liftable codec; the rest is session/pipeline/UI
  that the Swift bridge reimplements natively, calling the codec at the boundaries.

**Codec-lift difficulty verdict: LOW-to-MODERATE.** ~15 small functions to lift fresh
(annotation grammar + byte packers + tokenizers + key-string codec, all CString/CDWordArray-only),
2 already lifted behind `CC_NO_PROTOCOL` (flip the flag), the entire inline-formatting wire codec
already live in the engine, 1 extraction (ProcessSay parse block), 1 moderate lift (chunking loop),
and one genuine dependency decision (artifacts/core/ccommon.cpp UTF-8/quoting: lift vs
Swift-native). The spec's fear of format.cpp-style RichEdit entanglement in the codec did not
materialize — the only RichEdit-bound protocol function (`PRGDWGetFormatting`) is input capture
that native Swift replaces outright.
