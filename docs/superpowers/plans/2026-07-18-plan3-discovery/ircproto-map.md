# Plan 3 Discovery — ircproto.cpp Parse/Side-Effect Map

Read-only discovery for the PROTOCOL lift. All paths under
`/Users/timbroddin/Projects/comic-chat/v2.5-beta-1-modern/` unless prefixed.
Build set verified against `chat.mak` (`ircproto.obj` chat.mak:95, `ircsock.obj`
chat.mak:96; `protsupp`, `query`, `ccommon`, `histent`, `status` all present in
the obj list at chat.mak:75–140; include path `/I "..\artifacts\inc"`
chat.mak:36,43).

---

## 0. HEADLINE CORRECTIONS TO THE ROADMAP (read this first)

1. **`ircproto.cpp` is NOT the parser.** The entire inbound parse + dispatch
   lives in **`ircsock.cpp`**: `CIrcSocket::ProcessMessage` (ircsock.cpp:1115)
   → `HandleCommand` (1162), `HandleResultCode` (1849), `HandleErrorCode`
   (3006) — ~2,350 of ircsock.cpp's 3,574 lines are protocol logic, not socket
   plumbing. The handoff's claim that "`ircsock.cpp` is replaced wholesale by
   Swift `NWConnection` — only `ircproto`'s parse/build logic lifts" is
   **wrong as stated**: only `CAsyncSocket` inheritance + `OnReceive`/
   `OnConnect`/`OnClose` (1000–1076) and the SSPI block (413–998) are
   replaceable; the three `Handle*` functions plus `ParseIt`/`NGetCmd`/
   `ParseChannelMode` ARE the parse layer and must lift.
2. **`CIrcProto` itself is split across three files.** ircproto.cpp holds the
   outbound builders + charset codec; **protsupp.cpp** holds `TryNewNick`
   (protsupp.cpp:177), `SetConnectionStatus`/`GetConnectionStatus` (214/244),
   all 16 `Slash*` methods (2208–2735), `ProcessSlashCommand` (2886) and
   `ChatSetNick` (3965); **ircsock.cpp** holds `CIrcProto::OnLogin`
   (ircsock.cpp:1079). "Lift ircproto.cpp" really means "lift the protocol
   thirds of ircproto.cpp + ircsock.cpp + protsupp.cpp (+ query.cpp)".
3. **`CRoomInfo` is an OUTBOUND-only seam.** Every virtual is an app→protocol
   *command*. There is **no inbound event interface anywhere**: the parse
   handlers call ~30 free functions directly (`OnTextMsg`, `OnDataMsg`,
   `OnKick`, `AddAndExecute(new JoinEntry…)`, `AddToStatus`, …; §4 tables).
   The Plan-3 "bytes in, events out" surface must be designed fresh from that
   free-function list — the existing seam only covers the command direction.
4. **`bLowLevelQuoting` is not in this tree.** ircproto.cpp:497 calls it; the
   definition is in the shared artifacts core:
   `/Users/timbroddin/Projects/comic-chat/artifacts/core/ccommon.cpp:945`
   (unquoting :1026), declared in `artifacts/inc/ccommon.h:59–60` with
   `g_chLLQuoteCTCP = 0x10` (ccommon.h:41) and `g_chLLQuoteIRCX = '\\'`
   (ccommon.h:39). It reaches the build via the local wrapper
   `v2.5-beta-1-modern/ccommon.cpp:6` (`#include "..\core\ccommon.cpp"`,
   resolved against the `/I "..\artifacts\inc"` search path). Algorithm in §5.
5. **The "17 UI-ish call sites" in ircproto.cpp are real at function
   granularity.** The raw pattern grep hits 45 lines; they collapse to exactly
   **17 distinct sites** (§6), of which only **3 are true UI** (1 message box +
   2 dialog-control reads); the rest are app-singleton/doc reads (R17
   territory) plus one whole embedded identd *server socket*.
6. **There is no keepalive timer.** Client never initiates PING; it only
   answers server PING with PONG inline (ircsock.cpp:1694–1701). The only
   protocol timer is the one-shot 50 s ISIRCX probe timeout (§7). Total timer
   surface Swift must provide: one one-shot timer, request + cancel.
7. **The modern tree already contains one protocol modernization**: cmdidJoin
   recovers RFC 2812-style `JOIN #chan` (channel as arg, no colon — e.g.
   Libera) at ircsock.cpp:1364–1369. Preserve it in the lift; captured-bytes
   tests against a modern ircd depend on it.

---

## 1. CLASS / OWNERSHIP STRUCTURE

```
CRoomInfo                      (chatprot.h:10–95)   abstract seam + room state
  └── CIrcProto               (ircproto.h:10–78)   the IRC implementation
        m_pSock ──────────────► CIrcSocket : CAsyncSocket  (ircsock.h:438–501)
                                  m_queries : CQueryPtrList (ircsock.h:492;
                                              query.h:121, : public CPtrList)
globals:
  CIrcSocket serverConn        (ircproto.cpp:27; extern ircproto.h:80)
  cui.m_pvIrcProto             (set ircproto.cpp:52; GetIrcProto() ui.h:20)
  CRoomInfo g_enterInfo, *currentRoom  (chatprot.h:98; strCurrentChannel…
                                        convenience macros chatprot.h:100–104)
  CPtrList g_docs              (extern ircproto.cpp:29 — one CChatDoc per room)
```

- One `CIrcProto` per `CChatDoc` (`NewDefaultProto`, ircproto.cpp:37–44; also
  `doc->m_proto`), **but all instances share the single global socket**
  `serverConn` (`proto->m_pSock = &serverConn`, ircproto.cpp:40). One default
  instance with `m_doc == NULL` lives in `cui.m_pvIrcProto` (ircproto.cpp:52).
- `CIrcProto` adds only 3 data members: `m_pSock`, `m_bInRoom`,
  `m_strClientData` (ircproto.h:14–16). Everything else is inherited
  `CRoomInfo` room state or lives in `CIrcSocket` (§8).
- `CIrcSocket` owns framing buffers, login/auth state, IRCX capability flags,
  MOTD/LUSER accumulators, and the **CCQuery correlation list** — the real
  protocol state machine: every outbound query-style command enqueues a
  `CCQuery(qp, ct, dt, data, channel, nickmask)` *before* sending
  (ircproto.cpp:1050–1061), and reply handlers dequeue by command-type +
  purpose to decide routing (`m_queries.FindQuery(ct…)` throughout §4).
  `query.cpp`/`query.h` must lift with the protocol.

### 1.1 CRoomInfo — the full virtual table (chatprot.h)

"stub" = inline `{ ASSERT(0); }` body in the header. Impl column = where the
base implementation lives when not a stub; Override = `CIrcProto`'s.

| chatprot.h | Virtual (signature abbreviated) | Base | CIrcProto override |
|---|---|---|---|
| :38 | `void ChatPartChannel(CDocument*, BOOL bHardDisconnect)` | `{}` | ircproto.cpp:769 |
| :39 | `void SendMessageText(char* szMesg)` | stub | ircproto.cpp:475 |
| :40 | `BOOL bChatSendToChannel(const char* szAnnotations, const char* szMesg, char* szNMText=NULL, USHORT uModes=0)` | stub | ircproto.cpp:707 |
| :42 | `void ChatAnnounceNewAvatar(const char* szAvName, const char* szURL, const char* szAddressee=NULL, BOOL bForce=FALSE)` | protsupp.cpp:817 | — |
| :43 | `BOOL bChatSendPrivMesg(const char* szAddressee, const char* szAnnotations, const char* szMesg, char* szNMText=NULL, BOOL bAsNotice=FALSE, USHORT uModes=0)` | stub | ircproto.cpp:701 |
| :45 | `BOOL ChatSetTopic(const char* szTopic)` | stub | ircproto.cpp:713 |
| :46 | `BOOL ChatKickUser(const char* szNickname, const char* szReason)` | stub | ircproto.cpp:850 |
| :47 | `void ChatKickUser(CUserInfo* pui)` | stub | ircproto.cpp:1004 |
| :48 | `void ChatBanUser(CUserInfo* pui)` | stub | ircproto.cpp:1010 |
| :49 | `BOOL ChatBanUser(const char* szBanPattern, BOOL bBan, const char* szEncodedChannel=NULL)` | stub | ircproto.cpp:860 |
| :50 | `void ChatGetIdentity(CUserInfo* pui, LPCTSTR szNickname=NULL)` | stub | ircproto.cpp:980 |
| :51 | `BOOL bChatShowMOTD()` | stub | ircproto.cpp:1222 |
| :52 | `BOOL ChatSendInvitation(const char* szNickname)` | stub | ircproto.cpp:893 |
| :53 | `BOOL ChatChangeNick(const char* szNewNick)` | stub | ircproto.cpp:901 |
| :54 | `void ReplyVersion(CUserInfo*)` | protsupp.cpp:1126 | — |
| :55 | `void ReplyPing(CUserInfo*, CString mesg)` | protsupp.cpp:1142 | — |
| :56 | `void ReplyTime(CUserInfo*)` | protsupp.cpp:1147 | — |
| :57 | `void ReplyEmail(CUserInfo*)` | protsupp.cpp:1155 | — |
| :58 | `void ReplyHomePage(CUserInfo*)` | protsupp.cpp:1160 | — |
| :59 | `void DoNetMeetingCX(CUserInfo*, CString strAddr)` | protsupp.cpp:1300 | — |
| :60 | `void ChatStartNetMeeting(CUserInfo*)` | protsupp.cpp:1360 | — |
| :61 | `void ChatGetVersion(CUserInfo*)` | protsupp.cpp:3701 | — |
| :62 | `void ChatPingUser(CUserInfo*)` | protsupp.cpp:3709 | — |
| :63 | `void ChatGetLocalTime(CUserInfo*)` | protsupp.cpp:3719 | — |
| :64 | `void ChatGetEmail(CUserInfo*)` | protsupp.cpp:3727 | — |
| :65 | `void ChatGetHomePage(CUserInfo*)` | protsupp.cpp:3735 | — |
| :66 | `bRegisterJoins` — commented out; impl commented out too (protsupp.cpp:2808 `/* Removed because of security hole`) — **dead** | | |
| :67 | `BOOL bRegisterMode(char* szMesg)` | stub | ircproto.cpp:1022 |
| :68 | `BOOL SlashRaw(char* szMesg, IRCPARSE*)` | protsupp.cpp:2849 | — |
| :69 | `BOOL ProcessSlashCommand(char*, CDWordArray*, USHORT, BOOL)` | protsupp.cpp:2879 | protsupp.cpp:2886 |
| :70 | `void ChatGetInfo(CUserInfo*)` | protsupp.cpp:3415 | — |
| :71 | `void ChatGetAvatarInfo(CUserInfo*, BOOL bInteractive)` | protsupp.cpp:3424 | — |
| :72 | `void ChatSyncBackDrop(CChatDoc*, const char* szBackdrop, const char* szURL)` | protsupp.cpp:3432 | — |
| :73 | `BOOL bSendWhispers(const char* szAnnotations, const char* szMesg, char* szNMText=NULL, USHORT uModes=BM_WHISPER, BOOL* pbJustToMe=NULL)` | protsupp.cpp:3498 | — |
| :74 | `void DoChannelDialog()` | protsupp.cpp:4026 | — |
| :75 | `BOOL ChatSetMode(DWORD newMode, DWORD newMaxUsers, const char* szNewPasswd)` | stub | ircproto.cpp:941 |
| :76 | `void DoKickDlg(const char* nick, const char* strBan)` | protsupp.cpp:3773 | — |
| :77 | `void ChatSetOperator(CUserInfo*, int mode)` | protsupp.cpp:3676 | — |
| :78 | `void ChatInvite()` | protsupp.cpp:3916 | — |
| :79 | `void ChatSendFile(CUserInfo*)` | filesend.cpp:130 | — |
| :80 | `void OnLogin()` | stub | **ircsock.cpp:1079** |
| :81 | `void ChatSetAway(BOOL bAway, const char* szMesg, CUserInfo* pui=NULL, BOOL bProtoNotify=TRUE)` | protsupp.cpp:3744 | ircproto.cpp:912 |
| :82 | `void SetStatusString(CString&)` | stub | — (not overridden by CIrcProto; CB32/NetMeeting path only) |
| :83 | `void SetConnectionStatus(int)` | stub | protsupp.cpp:214 |
| :84 | `int GetConnectionStatus()` | stub | protsupp.cpp:244 |
| :85 | `void UpdateStatus()` | protsupp.cpp:3653 | — |
| :86 | `void ChatJoinChannel(CRoomInfo& enterInfo)` | stub | ircproto.cpp:786 |
| :87 | `void ChatCreateChannel(CRoomInfo& enterInfo)` | stub | ircproto.cpp:796 |
| :88 | `int GetType()` | stub | inline ircproto.h:39 (`PC_IRC`) |
| :89 | `void ChatSetNick(const char*)` | stub | protsupp.cpp:3965 |
| :90 | `void OnIdle(LONG)` | `{}` | — (nothing to pump) |
| :91 | `BOOL IsIRCX()` | `FALSE` | inline ircproto.h:63 (`m_pSock->m_bIrcXServer`) |
| :92 | `void DoIgnoreUser(CUserInfo*, BOOL bIgnore, BOOL bAutoIgnore=FALSE, LPCTSTR szNickname=NULL)` | stub | ircproto.cpp:1228 |
| :93 | `BOOL ChangeProperty(CUserInfo* puiSelf, LPCSTR pszProperty, LPCSTR pszValue)` | stub | ircproto.cpp:1389 |
| :94 | `void OnPropertyChange(LPCSTR pszProperty, LPCSTR pszValue)` | protsupp.cpp:3455 | — |

CRoomInfo data members (chatprot.h:13–22): `m_strChannel` (encoded),
`m_strPrettyChannel`, `m_strPassword`, `m_strTopic`, `m_strCreationModes`,
`m_prgdwTopicFormatting` (CDWordArray*), `m_dwModes`, `m_dwMaxUsers`,
`m_bSetMode`, `m_doc` (**CDocument\*** — the one MFC type embedded in the seam).

**Seam quality verdict:** genuinely useful but leaky. Of ~45 virtuals: ~20 are
clean outbound protocol commands (all overridden in ircproto.cpp — these map
1:1 to bridge commands); ~11 are CTCP-reply/query conveniences implemented
app-side in protsupp.cpp on top of the clean ones; ~8 are **dialog/UI actions**
(`DoChannelDialog`, `DoKickDlg`, `ChatInvite`, `TryNewNick` via `ChatSetNick`)
that must NOT cross the bridge; plus NetMeeting/file-send outliers. The
annotations-separate-from-text property **holds**: `szAnnotations` stays a
distinct parameter all the way to the final `sprintf` (ircproto.cpp:542–556,
637–654), and on IRCX servers annotations travel **out-of-band entirely** as a
separate `DATA <target> CCUDI1 :<annotations>` line (ircproto.cpp:542, 637)
rather than being prefixed to the PRIVMSG. The bridge event/command model can
mirror `(annotations, text)` pairs directly.

---

## 2. INBOUND PATH (bytes → parse → dispatch)

| # | Step | Location | Notes |
|---|------|----------|-------|
| 1 | `CIrcSocket::OnReceive(nErrorCode)` | ircsock.cpp:1000–1032 | `Receive()` appends into `m_szInput` (cap `m_nMaxMsgLength`, default 512 = `g_nDefaultIOBuff` ircsock.h:12, grown to server max on RPL_IRCX ircsock.cpp:2875–2882). **Line framing**: loop `strchr(m_szInput,'\n')`, copy line (incl. terminator) to `m_szMessage`, shift remainder down, `ProcessMessage` per line. Comment at 1028: reentrant but single-threaded. This whole function is the Swift/NWConnection replacement boundary. |
| 2 | `ProcessMessage(szLine)` | ircsock.cpp:1115–1159 | `ParseIt` → dispatch on `parse.uCode`: 0 → `HandleCommand`; `bIsErrorCode(uCode)` (ircsock.h:290) → `HandleErrorCode`; else `HandleResultCode`. Always ends with `AddToStatus(ircPrint, szLine)` (status.cpp) — every handler's `CIrcPrint::SetFormat` call is really "how should this line appear in the Status Window" (PT_NONE = suppress). `#ifdef IRCLOG` raw-line logging at 1120–1130. |
| 3 | `ParseIt(szMessage, pParse, bDoubleQuotes=FALSE)` | ircsock.cpp:137–258 | Fills `IRCPARSE` (ircsock.h:313–324): `:prefix` → nick/user/machine (nick only if not `CHANNELPREFIX`, split on `!`/`@`, 165–187); tokens split on spaces into `args[MAXARGS=10]` (strdup'd), `:trailing` → malloc'd `lastString`; >MAXARGS overflow dumps rest into lastString (234–238); numeric `args[0]` → `uCode` (242–257). `bDoubleQuotes` mode is used only by the outbound slash parser. `FreeParse` ircsock.cpp:261. |
| 4a | `HandleCommand` | ircsock.cpp:1162–1846 | Verb dispatch: `NGetCmd(args[0])` (ircsock.cpp:92–134) **binary-searches** the sorted `g_rgIrcCmd[47]` table (ircsock.cpp:36–85; struct PRIRCCMD ircsock.h:304; ids `enumCmdId` ircsock.h:331–381) then a `switch(nCmd)`. |
| 4b | `HandleResultCode` | ircsock.cpp:1849–3003 | `switch(pParse->uCode)` over RPL_* numerics (constants ircsock.h:58–236). |
| 4c | `HandleErrorCode` | ircsock.cpp:3006–3496 | `switch` over ERR_* numerics (ircsock.h:165–285), plus a shared tail (3416–3490) that extracts the channel name per error code and abandons the pending join (`theApp.RemoveRoomInfo`). |
| 5 | Payload second stage | protsupp.cpp | Text/data lines exit the handlers via `OnTextMsg` (protsupp.cpp:4358) / `OnDataMsg` (4374) → `PuiFromDocNickIdent` (4305) → `ProcessSay` (1545) / `ProcessComment` (846) / `ProcessUDIData` (1485). This layer does CTCP-tag matching (0x01-prefixed ids `actionID`/`soundID`/… ircproto.h:91–118), `# `-prefix comic messages (`APPEARSPREFIX` etc. ircproto.h:83–89), and **inbound low-level unquoting** (protsupp.cpp:848, 1563). It is the annotation-codec half named in the handoff; it belongs to the lift but its side-effect mapping is chatsrv-adjacent (sibling territory — flagged, not mapped here). |

Handled command cases (24 of 47 table entries; all others hit the `default:`
TRACE-only arm at 1179): Auth, Clone, Create, Data, Error, Invite, Join, Kick,
Kill, Killed, Knock, Mode, Nick, Notice, Part, Ping, Pong, PrivMsg, Prop,
Quit, Topic, Whisper, plus Reply/Request as explicit no-ops (1193–1205).
(The other 23 ids — ME, MSG, SOUND, THINK, LIST, WHO, … — exist for the
**outbound** slash-command table, not inbound dispatch.)

---

## 3. PER-HANDLER SIDE-EFFECT INVENTORY

Legend — **(a)** pure protocol-object state; **(b)** CCQuery correlation-list
ops (protocol state proper); **(c)** app/doc/UI calls that do NOT go through
any seam (callee file in parens). Disposition column uses the Edit-Rules menu:
R7 (msgbox→ccLog), R11 (whole-function `#ifndef CC_NO_UI`), R17 (reroute
singleton read through engine context), **EVT** (becomes a typed bridge
event), CMD (already reaches app only via re-issuing a seam command — fine).

### 3.1 HandleCommand cases (ircsock.cpp:1162–1846)

| Case | Lines | (a) proto state | (b) queries | (c) app/UI calls | Disposition |
|---|---|---|---|---|---|
| AUTH | 1207–1234 | `m_bRegistered=TRUE` | — | none (drives `HrGenerateAndSendAuthMsg`/`HrIrcXLogin`/`HrIrcLogin` — outbound) | SSPI auth: drop or Swift-side (§7 Win32); IRC login path lifts |
| CLONE / KNOCK / KILLED / PONG | 1236, 1442, 1753, 1703 | — | — | status print only | EVT (status-line event) |
| CREATE (self) | 1242–1273 | zeroes `currentRoom` topic/mode/limit via macros (1249–1251) | enqueue qpInitialNames/qpInitialTopic (1253–1261); issue qpInitialMode/qpInitialWho/qpJoinBackUrl | `bProcessAddChannel` (protsupp.cpp:4224 — **creates the CChatDoc**) | EVT `self-joined(channel)`; doc creation is app-side reaction |
| DATA | 1275–1323 | — | — | `LookupDoc` (chatdoc.cpp), `LookupPui` (protsupp.cpp), `OnDataMsg` (protsupp.cpp:4374); "# Appears as" fan-out to all docs (1307–1317) | EVT `data(annotations)` — core comic event |
| ERROR | 1325–1346 | — | — | `CSInString`; `AfxMessageBox` ×2 (1333, 1335); `AfxGetMainWnd()->PostMessage(ID_FILE_NEW)` (1336); `HrModeIsIrcXFailure` fallback (1342) | EVT `fatal-server-error(text)`; msgboxes → event payload, not R7 (user-facing) |
| INVITE | 1348–1359 | — | — | `CSInPlace`, `OnInvite` (protsupp.cpp:4514 — popup) | EVT `invited(by, ident, room)` |
| JOIN (other) | 1361–1390 | — | — | `LookupDoc`; rules engine `theApp.m_dynaRules.bMatchAndApplyRules` ×2 (1383, 1388); `AddAndExecute(new JoinEntry…)` (histent.cpp) | EVT `user-joined(nick, ident, channel)`; rules = app reaction |
| JOIN (self) | 1391–1425 | via macros: reset topic/mode/limit | enqueue Names/Topic; issue Mode/Who/PropGet(CLIENT) | `bProcessAddChannel` (doc creation); `theApp.m_nMyIdentLength` + `SetMyIdent` (setupdlg.cpp) at 1402–1406 | EVT `self-joined`; ident-length capture is protocol state → keep (R17 for theApp field) |
| KICK | 1430–1440 | — | — | `LookupDoc`, `CSInString`, `OnKick` (protsupp.cpp:1922) | EVT `kicked(kicker, kickee, reason, channel)` |
| MODE (channel) | 1448–1511 | `ParseChannelMode` (ircsock.cpp:299–400) mutates `doc->m_proto->m_dwModes/m_strPassword/m_dwMaxUsers`; static `MODECACH mcLost` (1468) tracks -o/-q loss | dequeue ctSetChannelMode (1466–1492) | inside ParseChannelMode: `ChatChangeAdmin` (protsupp.cpp:2072), `doc->OnViewText()` **UI** (372), `theApp.m_bSaveViewMode` (371), `FixMICChannelName` (379), `UpdateSpectators` (memblst.cpp, 391–394) | EVT `channel-mode-changed(delta)` + `member-status-changed`; OnViewText → app reaction to CM_NOFORMAT |
| MODE (user) | 1513–1572 | — | dequeue ctSetUserMode (1522–1566) | mutates `theApp.m_flags1` F1_USERVISIBLE (1551) | EVT `visibility-changed`; flags1 R17 |
| NICK | 1576–1611 | — | — | `g_docs` iteration + `LookupPui`; `AddAndExecute(new NickEntry…)` (1596); `SetMyNameNick` (setupdlg.cpp, 1603); status format IDS_NOWKNOWNAS (1607) | EVT `nick-changed(old, new, isSelf)` |
| NOTICE / PRIVMSG | 1614–1666 | — | — | `LookupDoc`; "# Appears as" fan-out (1644–1654); `CSInString`; `OnTextMsg` (protsupp.cpp:4358); server notices → status (1663–1664) | EVT `text(nick, ident, target, text, kind)` — THE main event |
| PART (self) | 1668–1676 | — | — | `GotPartChannel` (protsupp.cpp:4720), `theApp.m_pExitingDoc=NULL` | EVT `self-parted(channel)` |
| PART (other) | 1677–1691 | — | — | rules ×2 (1683, 1688), `AddAndExecute(new PartEntry…)` (1687) | EVT `user-parted` |
| PING | 1694–1701 | — | — | none — `sprintf(GetOutBuff(),"PONG :%s\r\n",…); Send(…)` (raw `Send`, not SendMessageText) | pure protocol; lifts verbatim |
| PROP | 1709–1751 | topic branch mutates `doc->m_proto->m_strTopic` + `m_prgdwTopicFormatting` (1741–1745) | dequeue ctPropSet/qpSetClient (1725–1733) | `LookupDoc`; `HandleClientDataChange` (ircproto.cpp:737 → `OnPropertyChange` → backdrop sync, protsupp.cpp:3455) | EVT `room-property-changed(key,val)` / `topic-changed` |
| KILL / QUIT | 1760–1777 | — | — | `g_docs` loop, `LookupPui`, `AddAndExecute(new PartEntry…)` | EVT `user-quit(nick)` |
| TOPIC | 1779–1830 | `doc->m_proto->m_strTopic` + formatting copy (1793–1797) | dequeue ctTopic/qpSetTopic (1802–1817) | `LookupDoc`, `CSInString`, `SzControlLess` (format.cpp), `AddToStatus` w/ formatting (1823) | EVT `topic-changed(channel, topic, fmt)` |
| WHISPER | 1832–1844 | — | — | `LookupDoc`, `GetTalkTos(doc,&talkTos,args[2])` (protsupp.cpp:1086), `OnTextMsg(... MT_WHISPER, &talkTos)` | EVT `whisper(...)`; note talkTos built from *nick list text*, never raw DWORDs on the wire (confirms handoff debt item — wire carries nick names, ircsock.cpp:1839) |

### 3.2 HandleResultCode numerics (ircsock.cpp:1849–3003)

Print-only groups collapsed; every case not listed as print-only is itemized.

| Reply | Lines | (a)/(b) | (c) app/UI calls | Disposition |
|---|---|---|---|---|
| 001 WELCOME | 1873–1895 | (b) enqueue qpInitialLUsersMOTD (1888) | `theApp.CompleteConnection()` (1876); `SetMyNameNick(args[1])` (1879); rules eOnConnect (1885); `GetIrcProto()->SetConnectionStatus(CX_NOCHANNEL)` (seam); `OnLogin()` (1892 → §3.4) | EVT `logged-in(actual-nick)` |
| 002–005, 200-series traces/stats, 221, 256–259, 302, 303, 305/306, 314/369, 364/365, 371/374/351/391, 381/386, 801–810 | various | — | status prints only | EVT status-line |
| 251–255/265/266 LUSER | 1952–1985 | `m_strLUSER` accumulate (1975) | `CSInString` | internal accumulation; surfaces via MOTD event |
| 301 AWAY | 2001–2012 | — | `DecodeNickForScreen`, IDS_AWAYREPORT status | EVT `user-away-notice` |
| 311 WHOISUSER | 2021–2075 | (b) FindQuery ctWhoIs, route by purpose | qpKickDlg → `currentRoom->DoKickDlg` (2044, **dialog**); qpBanDlg → `g_strBan` + send `MODE +b` (2047–2049); qpGetIdent → `ShowIdentity` (protsupp.cpp:3641 → GetInfoEntry in doc history); qpIgnoreIdent → `IgnoreUser` (protsupp.cpp:2134) | EVT `whois-result(nick,user,host,purpose)` — purposes become event tags |
| 312/313/317/319/320 | 2077–2089 | (b) suppress if pending ctWhoIs | — | print/suppress |
| 318 ENDOFWHOIS | 2091–2111 | (b) dequeue ctWhoIs | — | EVT end marker |
| 324 CHANNELMODEIS | 2127–2195 | resets `m_strPassword`/`m_dwModes` then absolute `ParseChannelMode` (2144–2147); (b) dequeue ctGetChannelMode | `theApp.GetRoomInfoFromName`/`RemoveRoomInfo` (2146, 2170); on-create fixups `ChatSetMode`+`ChatSetTopic` (2152–2166, seam commands); `bInitEnterInfo` (2172) | EVT `channel-modes(absolute)`; enterInfo bookkeeping is session state (sibling: chatsrv) |
| 331 NOTOPIC | 2197–2216 | (b) dequeue ctTopic/qpListMembers | `OnUserListAux` (chat.cpp — member-list dialog flow) | R11-adjacent (user-list dialog); EVT `no-topic` |
| 332 TOPIC | 2218–2298 | topic + formatting into `doc->m_proto` (2237–2241); (b) dequeue ctTopic | `LookupDoc`, `CSInString`, `AddToStatus`, `OnUserListAux` (2296) | EVT `topic(channel, topic)` |
| 341 INVITING | 2301–2308 | — | `AcknowledgeInvite` (protsupp.cpp:4993) | EVT `invite-acknowledged` |
| 321/811 LISTSTART | 2311–2338 | (b) FindQuery ctList/ctListX | `StartRoomList` (protsupp.cpp:3567); `g_bCanViewUnrated = bCanViewUnrated()` | EVT `room-list-begin` |
| 322 LIST | 2340–2393 | (b) | qpRoomListDlg → `new CRoom` + `AddToRoomList` (protsupp.cpp:3601); qpOnNewRoomEvent → rules daemon `bAddChannelToCurrentList` | EVT `room-list-item(name, users, topic)` |
| 812 LISTXLIST | 2395–2448 | statics `sRoom`/`sbAddIt` (1851–1852) | as 322 + `SzControlLess`, MIC detection | EVT `room-list-item` (extended) |
| 813 LISTXPICS | 2450–2459 | `sbAddIt = bPassesRatings(lastString)` (protsupp.cpp:4429) | — | ratings gate = app policy; EVT carries PICS string |
| 323/816/817 LISTEND | 2461–2506 | (b) dequeue | `EndRoomList` (protsupp.cpp:3577); rules `bOnEndOfListing` | EVT `room-list-end(truncated?)` |
| 353 NAMEREPLY | 2508–2549 | (b) drop stale qpInitialTopic (2515–2527) | `bForEachWord(lastString, bSingleJoin…)` → `AddAndExecute(new JoinEntry…)` per nick (ircsock.cpp:270–277); rules eOnJoin (2538) | EVT `names(channel, nick-list)` |
| 366 ENDOFNAMES | 2551–2574 | (b) dequeue ctNames | — | EVT end marker |
| 352 WHOREPLY | 2576–2650 | (b) route by purpose | rules/notifs user match (`bIsMatch`, `CreateUserFromWhoReply` protsupp.cpp, daemon add); qpInitialWho → `UpdateIgnoreOnEntry` (protsupp.cpp:2114); qpUserListDlg → `AddToUserList` (protsupp.cpp:3616) | EVT `who-result(...)` w/ purpose tag |
| 315 ENDOFWHO | 2652–2710 | (b) dequeue ctWho | `EndUserList` (protsupp.cpp:3589); rules end-of-listing | EVT end marker |
| 367 BANLIST | 2721–2730 | file-static `g_arrayBans` accumulate (ircsock.cpp:28–29) | `DecodeNick` | accumulate → EVT at 368 |
| 368 ENDOFBANLIST | 2732–2753 | (b) dequeue ctSetChannelMode | `DoBanDlg(args[2], g_strBan, g_arrayBans)` (protsupp.cpp:3797 — **dialog**) | EVT `ban-list(channel, masks)`; dialog is app reaction |
| 372/375/376/377 MOTD | 2755–2808 | `m_strMOTD` accumulate; (b) dequeue ctLUsersMOTD | `ShowMOTD(m_strLUSER, m_strMOTD)` (motd.cpp — window) gated by `theApp.m_flags1 & F1_SHOWMOTD` | EVT `motd(luser, motd)`; show/hide = app policy |
| 800 RPL_IRCX | 2817–2900 | `m_bIrcXServer=TRUE`, `m_bAnonAllowed`, `m_rgszSvrSecuPack`, `m_nMaxMsgLength` grow via `HrInitAlloc` (2875–2882); (b) dequeue ctModeIsIrcX/ctIrcX | `theApp.HrAllocBuffer` (2881, R17) | pure capability negotiation; lifts; EVT `server-caps(ircx, maxlen)` |
| 818 PROPLIST | 2915–2960 | (b) FindQuery ctPropGet | PICS gate `bPassesRatings` → `ChatJoinAux`/`ChatCreateAux` (seam, deferred join); CLIENT → `HandleClientDataChange` | EVT `room-props(PICS/CLIENT)`; ratings gate = app policy hook |
| 819 PROPEND | 2962–3001 | (b) dequeue | `bCanViewUnrated(TRUE)` (**can prompt a dialog**, protsupp.cpp:3518) → deferred join/create | same |

### 3.3 HandleErrorCode (ircsock.cpp:3006–3496)

Common tail 3416–3490: per-code channel-name extraction → abandon pending join
(`theApp.GetRoomInfoFromName`/`RemoveRoomInfo`). Default arm: red status print.

| Error | Lines | Side effects | Disposition |
|---|---|---|---|
| 401 NOSUCHNICK | 3034–3050 | `AfxMessageBox`; `bFreeModeCell` (3499 — mode-query cleanup) | EVT `error(no-such-nick/channel)` |
| 403 NOSUCHCHANNEL | 3052–3094 | `ShowBadChannelName` (protsupp.cpp:4103); **direct RoomList dialog widget pokes** `pRoomList->GetDlgItem(IDC_RESET_LIST)…EnableWindow/GotoDlgCtrl/NextDlgCtrl` (3072–3084); `AfxMessageBox`; `bFreeModeCell` | worst UI offender in the parse layer — R11 the widget block; EVT `error(bad-channel)` |
| 405 / 436 / 438 / 439 / 465 / 466 / 552 / 553 / 556 | 3096–3099, 3138–3154, 3204–3214, 3299–3315 | `AfxMessageBox(IDS_…)` only | EVT typed errors (message-box → event; NOT R7 — user-facing) |
| 422 NOMOTD | 3102–3126 | status print + `ShowMOTD(LUSER only)`; dequeue ctLUsersMOTD | EVT |
| 431/432/433 bad nick | 3128–3136 | `GetIrcProto()->TryNewNick(…)` → **CNicknameDlg modal** (protsupp.cpp:177–212; cancels can force `theApp.OnDisconnect`) | EVT `nick-rejected(kind, badnick)` — retry loop moves to Swift |
| 442 NOTONCHANNEL | 3156–3180 | member-list fallback `OnUserListAux`; `bFreeModeCell` | EVT |
| 451 NOTREGISTERED | 3182–3187 | `HrModeIsIrcXFailure()` (ircsock.cpp:560 — the ISIRCX-probe fallback: dequeue, plain IRC login, `KillTimer`) | pure protocol; lifts |
| 461 / 467 / 472 / 482 / 501 / 502 | 3189–3195, 3216–3237, 3260–3297 | `bFreeModeCell` variants + status print | protocol-state cleanup; lifts |
| 464 PASSWDMISMATCH | 3197–3202 | `HrIrcSetOper(user, NULL)` → **PromptForPassword dialog** (ircsock.cpp:525–557, writes registry) | EVT `auth-password-rejected`; prompt is Swift-side |
| 471/473/474 join blocked | 3224–3251 | `AfxMessageBox(fmt(DecodeChan))` | EVT `join-failed(reason, channel)` |
| 475 BADCHANNELKEY | 3253–3258 | `OnBadChannelPassword(*pEnterInfo)` (protsupp.cpp:4673 — **password dialog + rejoin**) | EVT `join-failed(bad-key)`; re-prompt loop Swift-side |
| 900/902, 904, 905, 907 | 3317–3359 | dual-meaning MIC-vs-IRCX codes: msgbox only when `!m_bIrcXServer` | EVT; keep the `m_bIrcXServer` disambiguation in engine |
| 910 AUTHENTICATIONFAILED | 3361–3370 | `AfxMessageBox(ID_ERR_BADUSERINFO)`; `m_bAuthFailed=TRUE`; retry `HrIrcXLogin(FALSE)` | EVT `auth-failed` |
| 912 UNKNOWNPACKAGE | 3372–3379 | `HrIrcXLogin(TRUE)` next package | protocol; lifts |
| 924 NOSUCHOBJECT | 3381–3412 | PICS-on-missing-room → `ChatJoinAux`/`ChatCreateAux` via `bCanViewUnrated(TRUE)` (may prompt) | EVT + policy hook |

### 3.4 `CIrcProto::OnLogin` (ircsock.cpp:1079–1112) — post-login app hook

Called from RPL_WELCOME. All app-side: `StopIdentD()` (ircproto.cpp:1435);
rules/notifs daemon start (`theApp.m_dynaRules/m_dynaNotifs`, 1085–1089);
`AddToServerList` (1091); `theApp.m_bInSearch=FALSE`;
`SetVisibility(theApp.m_flags1 & F1_USERVISIBLE)` (seam command, 1093);
on-connect action switch → `ChatJoinChannel(g_enterInfo)` or
`theApp.OnChatroomList()` (1103–1111). Disposition: EVT `logged-in` and let
Swift drive the follow-ups; the auto-join belongs to the app layer.

---

## 4. OUTBOUND PATH (commands → bytes)

### 4.1 The socket boundary (what Swift's NWConnection replaces)

Single choke point: **`CIrcProto::SendMessageText(char*)` →
`m_pSock->Send(szMesg, strlen(szMesg))`** (ircproto.cpp:475–478). Every
outbound line goes through it except three raw `Send` sites: PONG
(ircsock.cpp:1698), the identd responder (ircproto.cpp:1455), and nothing
else (`grep 'Send('` confirms). Callers outside ircproto.cpp that use the
seam: PASS/NICK/USER login (ircsock.cpp:640–654), OPER (785), AUTH blobs
(992), ban-followup `MODE +b` (2049). Two output buffers exist: the shared
app-context `GetOutBuff()` (= `cui.GetOutBuffSz()`, ui.h:18; alloc
ircsock.cpp:511) used by most `sprintf` builders, and
`serverConn.m_szOutput2` used only by `bChatSendToTarget`. Bridge shape:
`SendMessageText` becomes the "outbound buffer ready" C callback.

### 4.2 Message send with chunking — `bChatSendToTarget` (ircproto.cpp:481–698)

1. Encode text: private msgs always `ENC_DBCS`; channel msgs
   `EncodingType()` (ircproto.cpp:1261–1265: `%`-channel and not MIC →
   `ENC_UTF8`, else DBCS).
2. **Low-level quote** `\r`/`\n` via `bLowLevelQuoting(g_chLLQuoteCTCP, TRUE,…)`
   (ircproto.cpp:497; algorithm §5).
3. Compute worst-case receive-side length: `12` ("PRIVMSG  :\r\n") + target +
   annotations + text + `":<mynick>!<ident> "` prefix the *receiver* will see
   (nReceivingPrefixLen, 523–534; unknown ident ⇒ `user + 32`).
4. Fits `serverConn.m_nMaxMsgLength` → single shot: IRCX+annotations sends
   `DATA <target> CCUDI1 :<annotations>\r\n` then `PRIVMSG|NOTICE <target>
   :<text>\r\n` (542–549); non-IRCX inlines
   `…:<annotations><text>\r\n` (554–556).
5. Too long → chunk loop (618–691): per-mode prefix/suffix bookkeeping
   (BM_ACTION/BM_SOUND/BM_AWAY keep the 0x01-CTCP wrapper per chunk;
   BM_HERESINFO keeps the `#` prefix; BM_SAY/THINK/WHISPER bare, 576–605);
   `nGetBreakingPoint` (398–472) finds a break ≥80% of max preferring spaces,
   never splitting a formatting escape or a UTF-8/DBCS sequence, and returns
   carry-over formatting state (`wFormatEnd`) so each chunk is re-prefixed
   with `szFormatBegin` — formatting continuity across chunks; SOUND degrades
   to ACTION after chunk 1 (682–689); CP932 DBCS sends only one chunk
   (`bOnlySendOneChunk`, 573).

`bChatSendToChannel` (707) and `bChatSendPrivMesg` (701) are thin wrappers.

### 4.3 Query-correlated commands — `bExecuteQuery` (ircproto.cpp:1034–1219)

Refuses when `CX_DISCONNECTED` (1047); allocates + enqueues `CCQuery` **before
sending** (1050–1061) — this enqueue-then-send discipline is the correlation
contract the reply handlers depend on. Wire strings built (all
`SendMessageText(GetOutBuff())` at 1216):
`WHO [filter|chan]` (1065–1096), `WHOIS <mask>` (1098), `TOPIC <chan>[ :<t>]`
(1103–1115), `LIST[ <chan>]` (1117), `LISTX[ N=<chan>]` (1124),
`LUSERS\r\nMOTD` (1131 — two commands, one buffer), `IRCX` (1135),
`MODE ISIRCX` (1139), `MODE <chan>` (1143), `MODE <chan><modes>` (1147),
`MODE <nick> ±i|<modes>` (1151–1173), `PROP <chan> PICS|CLIENT` (1176–1193),
`PROP <chan> CLIENT :<data>` (1196–1209).

### 4.4 Direct `sprintf` builders (no query cell)

`PART` (ircproto.cpp:772), `JOIN <chan>[ <pwd>]` (810–815), `CREATE <chan>
[modes][max][pwd]` (845), `KICK <chan> <nick> :<reason>` (854), `MODE <chan>
±b <mask>` (883, 1016), `INVITE <nick> <chan>` (895), `NICK <nick>` (906),
`AWAY[ :<msg>]` (924–926), `MODE ±<modechars> [max] [key]` (966–974), plus
login-time `PASS`/`NICK`/`USER` (ircsock.cpp:640–654), `OPER` (785), `AUTH`
(671–998), `PONG` (1698). Arbitrary passthrough: `SlashRaw`
(protsupp.cpp:2849).

### 4.5 Slash-command front end (protsupp.cpp:2886–3003)

`CIrcProto::ProcessSlashCommand` re-uses `ParseIt` (bDoubleQuotes=TRUE) and
`NGetCmd` on the same `g_rgIrcCmd` table, dispatches to the 16 `Slash*`
methods (protsupp.cpp:2208–2735) which use the `g_rgSyntax[24]` argument-type
table (ircsock.h:393–419) + `StrEncodeCommandParam` (ircproto.cpp:1302–1386)
for per-argument-type encoding. UI in this front end: `AfxMessageBox
(IDS_MUSTBE_CONNECTED)` (protsupp.cpp:2906) and the Status-Window
activation block (`GetStatusView()->GetParentFrame()`, `AfxGetMainWnd`,
`ActivateFrame`, `AutoArrangeWindows`, 2986–3001) — R11 the tail block, keep
the dispatch.

---

## 5. LOW-LEVEL QUOTING — exact algorithm

Source: `/Users/timbroddin/Projects/comic-chat/artifacts/core/ccommon.cpp`
(shared core, compiled into this app via `v2.5-beta-1-modern/ccommon.cpp:6`).
Constants: `g_chLLQuoteCTCP = 0x10` (DLE; ccommon.h:41), used for all wire
traffic; `g_chLLQuoteIRCX = '\\'` (ccommon.h:39), used only by histent.cpp's
save-file quoting — NOT wire protocol.

**Encode** `bLowLevelQuoting(Q, bTreatAsByteArray, szSrc, *pszDst, *pbFree,
bRemoveCarriageReturns=FALSE)` (ccommon.cpp:945–1010): exactly three bytes are
escaped — `LF (0x0A) → Q 'n'`, `CR (0x0D) → Q 'r'` (or dropped entirely when
bRemoveCarriageReturns), `Q → Q Q` (997–998). All other bytes pass through
(DBCS-aware via CharNext unless bTreatAsByteArray — the wire path passes TRUE,
ircproto.cpp:497). Zero-copy fast path: if nothing needs quoting, `*pszDst =
szSrc` and `*pbFree = FALSE` (965–969); otherwise heap-allocates and sets
`*pbFree = TRUE` (caller frees — ircproto.cpp:694–695).

**Decode** `bLowLevelUnquoting(Q, bTreatAsByteArray, szSrc, szDst)`
(ccommon.cpp:1026–1115): in-place capable (szDst == szSrc — both call sites do
this, protsupp.cpp:848, 1563). Two-pass tolerant heuristic: pass 1 scans the
whole string; a `Q` followed by anything other than `n`/`r`/`Q` marks the
string "not quoted" (1050–1057) and pass 2 then copies it **verbatim** —
i.e., a stray naked 0x10 makes the decoder treat the entire line as unquoted
rather than fail. `Q n → LF`, `Q r → CR`, `Q Q → Q`. Byte-compare tests must
reproduce the escape set {0x0A, 0x0D, 0x10} and the all-or-nothing decode
heuristic exactly.

---

## 6. THE 17 UI-ISH SITES IN ircproto.cpp — full enumeration

Raw pattern grep (AfxMessageBox|theApp.|LoadString|GetWindowText|DoModal|…)
hits 45 lines; they collapse to these 17 function-level sites. "True UI" =
touches HWND/dialog/message box.

| # | Lines | Function | What | Disposition |
|---|---|---|---|---|
| 1 | 49, 65 | `CommunicationInits`/`Cleanup` | `theApp.m_bDoCB32` + `CNmProto::Initialize/Uninitialize` (NetMeeting/CB32) | already `#ifdef CB32SUPPORT` — do not lift (nmproto excluded) |
| 2 | 52, 70 | same | `cui.m_pvIrcProto` set/clear (ui.h context global) | R17 — engine context owns the protocol instance |
| 3 | 54–57 | `CommunicationInits` | `AfxSocketInit()` + **`AfxMessageBox(IDP_SOCKETS_INIT_FAILED)`** | whole init disappears with Swift sockets; msgbox R7 if kept |
| 4 | 74–83 | `FixMICChannelName` | `doc->SetLegalPath`, `ChatSetChannel` (setupdlg.cpp settings), `doc->GetConnectionStatus`/`SetConnectionStatus` | EVT `channel-renamed(pretty)`; doc/title fixups are app reactions |
| 5 | 101–133 | `ChatFillRoomList` | reads `CRoomList` dialog persistence (`prl->m_persist->m_strQuery`), `g_bCanViewUnrated` | R11 whole function; its core resurfaces as bridge command `request-room-list(query)` → bExecuteQuery LIST/LISTX |
| 6 | 136–184 | `ChatFillUserList` | **`pul->m_user.GetWindowText` (157)**, **`pul->m_ctlRoom.GetWindowText` (176)** — live dialog-control reads — + persist fields | R11 whole function; core = `request-user-list(filter)` → WHO |
| 7 | 187–195 | `GetMyIP` | `serverConn.GetSockName` + `ntohl` (winsock) | callers are filesend.cpp only (DCC) — R11/`CC_NO_UI` now; Swift supplies local addr if DCC ever lifts |
| 8 | 204–248 | `EncodeNick`/`DecodeNick` | `theApp.m_wszBuffer/m_nBufferSize` scratch; Win32 NLS (`MultiByteToWideChar`, `GetACP`) | R17 buffers → engine context; NLS = Plan 3 NLS shim work (handoff's Capitalize/intl.c cluster) |
| 9 | 250–296 | `DecodeNickForScreen` | `GetStringTypeEx`, `GetUserDefaultLCID`, `wsprintf`, `CharNext` | display-only helper (quotes nicks containing blanks/controls); used in status strings — keep w/ NLS shim, or R11 |
| 10 | 300–395 | `DecodeChan`/`EncodeChan`/`DecodeString` | `theApp.m_szBuffer/m_charSet/m_nBufferSize`; `ConvertEncodingIn/Out` (intl.c) | R17 + NLS; note CP-1252 posture (spec §4.5) may collapse the DBCS arms |
| 11 | 527–533 | `bChatSendToTarget` | `GetMyNickName()`/`GetMyUserName()` (chat.h:317 → chatsrv/userinfo) + `theApp.m_nMyIdentLength` | R17 — self-identity via engine session (sibling maps the owner) |
| 12 | 769–783 | `ChatPartChannel` | `theApp.m_dynaRules.bMatchAndApplyRules(eOnLeave…)`, `theApp.m_myNick/m_myIdent`, `doc->m_bStatusView` | rules engine = app; EVT `self-parting` and let app run rules; R17 for identity strings |
| 13 | 912–938 | `ChatSetAway` | iterates `g_docs`, calls each `doc->m_proto->CRoomInfo::ChatSetAway` (echo into every open room) | R17 — session channel list; echo fan-out becomes app reaction to EVT `away-changed` |
| 14 | 980–1001 | `ChatGetIdentity` | `ShowIdentity` (protsupp.cpp:3641 — posts GetInfoEntry into doc history) | EVT `identity(nick, user, host)` |
| 15 | 1228–1252 | `DoIgnoreUser` | `IgnoreUser` (protsupp.cpp:2134 — ignore list + member-list UI refresh) | EVT `ignore-changed`; list mutation app-side |
| 16 | 1302–1386 | `StrEncodeCommandParam` | `LookupDoc(szEncodedChannelName)` (1322) to disambiguate channel-vs-nick args | R17 — needs a "is this a joined channel" predicate from the engine session table |
| 17 | 1406–1457 | `CIdentdSocket` + `StartIdentD`/`StopIdentD` | **an entire embedded identd server** — `CAsyncSocket` listener on port 113, `Accept`, `Receive`, replies `"<req> : USERID : UNIX : <user>\r\n"` (1454, uses `GetOutBuff`/`GetMyUserName`) | spec §4.4 says engine never opens sockets: either drop (modern ircds rarely require identd) or reimplement as Swift NWListener with the reply format lifted; decision needed at plan time |

Only #3, #6 (and #5's dialog coupling) are true UI. The file's real porting
cost is #8–#10 (NLS/codec) and #17 (identd), not message boxes. By contrast
ircsock.cpp holds ~20 `AfxMessageBox` sites (all itemized in §3) plus 2
dialogs (`PromptForPassword` ircsock.cpp:525, `CNicknameDlg` via `TryNewNick`
protsupp.cpp:177) and 1 direct dialog-widget poke (ircsock.cpp:3072–3084).

---

## 7. TIMERS

| Timer | Interval | Armed | Killed | Fires | Purpose |
|---|---|---|---|---|---|
| `ID_ISIRCXTIMEOUT` | 50 000 ms one-shot (`ISIRCXTIMEOUT`, defines.h:202) | `CIrcSocket::OnConnect` after sending `MODE ISIRCX` probe (ircsock.cpp:1050–1052, via `::AfxGetMainWnd()->SetTimer`) | `HrModeIsIrcXFailure` (ircsock.cpp:581); disconnect path chat.cpp:2554 | `CMainFrame::OnTimer` (mainfrm.cpp:329) → `CChatApp::IsIrcXTimeout` (chat.cpp:2596) → `serverConn.HrModeIsIrcXFailure()` | server never answered the IRCX probe → fall back to plain IRC login (also triggered synchronously by ERR_NOTREGISTERED ircsock.cpp:3185 and by `cmdidError` ircsock.cpp:1342) |
| `ID_CONNECT_TRY` | 50 ms | `bChatServerConnect` (protsupp.cpp:4818), `CChatApp::ResumeConnection` (chat.cpp:2581) | chat.cpp:2540/2553, protsupp.cpp:4751 | → `theApp.ContinueConnection()` (mainfrm.cpp:326) | multi-server connect state-machine pump — pre-socket app logic; Swift connection management replaces it wholesale |
| rules/notifs daemons | app-defined | from `CIrcProto::OnLogin` (ircsock.cpp:1085–1089) | app | mainfrm.cpp OnTimer | app-layer automation; out of protocol scope |

**No PING keepalive, no flood timer, no retry/backoff timer inside the
protocol.** Flooding is arrival-time counters (`CUserInfo::IsFlooding`,
protsupp.cpp:3472). `CRoomInfo::OnIdle` is a no-op for IRC (chatprot.h:90;
pumped from `CChatApp::OnIdle` chat.cpp:975 — used only by the CB32 backend).

**What the engine needs from Swift**: a single one-shot timer facility —
`request_timer(id, ms)` / `cancel_timer(id)` outbound, `timer_fired(id)`
inbound — used exactly once (ISIRCX probe timeout → `HrModeIsIrcXFailure`).

---

## 8. WIN32/MFC DENSITY (beyond the UI sites)

Per file, what the shim/`CC_NO_UI` budget must cover:

**ircproto.cpp** — MFC types `CString`/`CDWordArray`/`CPtrList`/`CDocument*`
(all but CPtrList already in `mfc_compat.h`: CString :315, CDWordArray/
CPtrArray/CStringArray :1026–1029, POSITION :1047 — **CPtrList is NOT yet
shimmed**); Win32 NLS: `MultiByteToWideChar`/`WideCharToMultiByte` (210, 239,
313, 352, 384, 1285), `GetACP` (throughout), `GetStringTypeEx`+
`GetUserDefaultLCID` (256), `CharNext` (285, 452), `IsDBCSLeadByte` (271),
`wsprintf` (289); `mbstring.h` (`OurMbsChr` wrapper, 864); winsock
`GetSockName`/`ntohl`/`SOCKADDR_IN` (188–193); `CAsyncSocket` (identd,
1406–1457); `AfxSocketInit` (54); UTF-8 codec externs
`bConvertWideStringToUTF8`/`bConvertUTF8StringToWide` (artifacts core
ccommon.cpp — lifts with the quoting functions); `ConvertEncodingIn/Out`
(intl.c — the handoff's CP-1252 decision point); TRACE/ASSERT/VERIFY.

**ircsock.cpp** — `CAsyncSocket` base (Receive/Send/Create/Listen/Accept/
GetLastError); **SSPI/NTLM auth block** (413–998: `CSSPI.H`, `CredHandle`/
`CtxtHandle`/`PSecurityFunctionTable`, `LoadLibrary` of security DLL
ircsock.cpp:800–830, base64-ish blob exchange in `HrGenerateAndSendAuthMsg`
886–998) — **recommend NOT lifting**; modern servers don't speak IRCX-NTLM;
plan a `cc_auth_unsupported` event instead and keep `authtypeNone`/
`authtypePlainText` (PASS/NICK/USER + OPER) only; registry writes
(`WriteToRegistry`, 547); `gethostname` (649); `::AfxGetMainWnd()->SetTimer/
KillTimer`; `lstrcmpi`/`_tcsicmp`; `AfxMessageBox` ~20 sites (§3);
`CStringArray`, `ZeroMemory`.

**protsupp.cpp (CIrcProto methods only)** — `CNicknameDlg`/`CPersonalPage`
(177–202), status-window frame activation (2986–3001), `CChatDoc` casts.

Existing shim assets that already cover part of this: `shim/mfc_compat.h`
(CString, arrays, POSITION, COLORREF/RGB — RGB is used by every
`CIrcPrint::SetFormat` call), `shim/engine_context.*` (the R17 pattern),
`CharNext` shim member (Plan 2 Task 5, currently `CC_NO_UI`-gated per
handoff).

---

## 9. STATE HELD (connection vs room vs user)

### CIrcProto (per-room protocol object)
| Member | Decl | Class |
|---|---|---|
| `m_pSock` | ircproto.h:14 | wiring (always `&serverConn`) |
| `m_bInRoom` | ircproto.h:15 | **room** — in-channel flag; combined with socket state to synthesize CX_INCHANNEL/CX_NOCHANNEL (protsupp.cpp:244–252) |
| `m_strClientData` | ircproto.h:16 | **room** — IRCX CLIENT prop cache; diffed in `HandleClientDataChange` (ircproto.cpp:737–767) |
| inherited `m_strChannel`/`m_strPrettyChannel`/`m_strPassword`/`m_strTopic`/`m_strCreationModes`/`m_prgdwTopicFormatting`/`m_dwModes`/`m_dwMaxUsers`/`m_bSetMode` | chatprot.h:13–21 | **room** |
| inherited `m_doc` | chatprot.h:22 | app back-pointer (CDocument*) — bridge replaces with opaque session/channel id |

### CIrcSocket (singleton `serverConn` — connection-scoped)
Framing/buffers: `m_szInput`/`m_szOutput2`/`m_szMessage`, `m_nMaxMsgLength`
(ircsock.h:477–481). Connection: `m_iConnected` (490), `m_bRegistered` (475).
Server caps: `m_bIrcXServer` (474), `m_bAnonAllowed` (472),
`m_bJustSentModeIsIrcX` (476), `m_rgszSvrSecuPack` (468). Auth (drop-set):
`m_rgszUsrSecuPack`, `m_pszUserName`/`m_pszPassword`,
`m_nAuthenticationType`, `m_nSecuPackIndex`, `m_bAuthFailed`, SSPI handles
(483–489). Accumulators: `m_strMOTD`/`m_strLUSER` (465–466). Correlation:
`m_queries : CQueryPtrList` (492). `Reset()` (ircsock.cpp:473–484) defines the
reconnect-survivable subset.

### File-statics (hidden protocol state — must move into the lifted object)
`g_strBan`, `g_arrayBans` (ircsock.cpp:28–29 — ban-list accumulation between
367/368); `MODECACH mcLost` (ircsock.cpp:1468 — host/owner-loss echo
suppression); `sRoom`/`sbAddIt` (ircsock.cpp:1851–1852 — LISTX row-in-flight
+ PICS verdict); `bLoopPwd` (ircsock.cpp:675, static local in HrIrcXLogin);
`g_identd`/`g_ident0` (ircproto.cpp:1415).

### Deliberately NOT mapped here (sibling agent: chatsrv.cpp ownership)
Self-identity (`GetMyNickName` chat.h:317, `GetMyName`/`GetMyUserName`/
`GetMyServer`/`GetMyIdent`, `SetMyNameNick`/`SetMyIdent` — setupdlg.cpp);
`theApp.m_myNick`/`m_myIdent`/`m_nMyIdentLength`; the pending-join table
(`theApp.GetRoomInfoFromName`/`RemoveRoomInfo`, `g_enterInfo`/`currentRoom`
and the `strCurrentChannel…` macros chatprot.h:100–104); `g_docs` and the
per-user tables (`LookupPui`, `externalPuis` protsupp.cpp:70). **Ambiguity to
resolve jointly**: `currentRoom` is mutated by parse handlers
(ircsock.cpp:1248–1251, 1396–1399) and read by ircproto.cpp via the
`dwCurrentChannelMode`/`strCurrentChannelKey` macros (ircproto.cpp:944–964) —
whichever side owns it, the bridge needs a single authority for "the room the
next reply refers to".

---

## 10. LIFT-SET CONSEQUENCE (summary for the plan writer)

Protocol lift unit = `ircproto.cpp` (minus §6 items 1, 3, 5–7, 17) +
`ircsock.cpp` lines 36–400 & 1000‑only‑framing‑replaced & 1079–3534 (minus
SSPI 413–998, minus AfxMessageBox/dialog arms) + `protsupp.cpp` CIrcProto
methods & payload layer (846–1544 selected, 2208–3003) + `query.cpp/.h` +
artifacts-core `ccommon.cpp` quoting/UTF-8 block. New work: inbound **event
vocabulary** distilled from §3's ~30 free-function callees (the sibling's
chatsrv map names their consumers); `CPtrList` shim; one one-shot timer;
identd decision; SSPI drop ruling.
