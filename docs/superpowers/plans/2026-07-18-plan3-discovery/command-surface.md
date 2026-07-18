# Plan 3 Discovery — IRC / IRCX Command Surface (emitted vs parsed)

Read-only discovery for the protocol lift. All paths under
`/Users/timbroddin/Projects/comic-chat/v2.5-beta-1-modern/` (read-only reference).
Build set verified against `chat.mak`. "EMITS" means the client actively writes it to the
socket; "PARSES" means the inbound dispatch has an explicit handler; everything else is
tolerated-and-ignored (the code is deliberately permissive — unknown verbs TRACE+drop,
unknown numerics go to the status window).

---

## 0. HEADLINE FACTS (read this first)

1. **`nmproto.cpp/.h` is DEAD CODE.** Not in the `OBJS` list of `chat.mak` (chat.mak:54-139;
   the header comment chat.mak:11 says so explicitly). The entire file body is inside
   `#ifdef CB32SUPPORT` (nmproto.h:1) and `CB32SUPPORT` is defined nowhere in `CPP_PROJ`
   (chat.mak:42). It is the NetMeeting T.120 data-channel transport (`ICb32Core`) —
   **do not analyze it as live protocol.** (The *live* NetMeeting integration is only the
   `\x01NETMEET\x01` CTCP + `ConferenceConnect` API, see §4.6.)
2. **`irc.txt`, `ircnew.txt`, `ircorig.txt` are NOT specs — they are captured session
   logs** produced by the `#ifdef IRCLOG` debug writer (ircsock.cpp:1120-1130, which
   appends to a file literally named `irc.txt`). `irc.txt` is an MS Exchange Chat
   (IRCX) login (`:chloe1 800 * 0 0 NTLM,ANON 512 *` — irc.txt:1); `ircnew.txt`/
   `ircorig.txt` are plain-IRC logins against `irc.stealth.net` whose **first line is
   `451 * :You have not registered`** — the server's answer to the client's `MODE ISIRCX`
   probe (ircorig.txt:1, ircnew.txt:1). `rtwsupport.txt` is a Microsoft support phone-number
   document — no protocol content.
3. **The client never sends `QUIT`.** Disconnect = `PART` each joined room + raw socket
   close (`ChatServerDisconnect`, protsupp.cpp:4743-4746; `CIrcProto::ChatPartChannel`,
   ircproto.cpp:769-783). The only "QUIT" in the tree is the inbound-parse table entry
   (ircsock.cpp:70).
4. **The client never sends client-initiated `PING` to the server** (only CTCP
   `\x01PING\x01` to users). It answers server `PING` with `PONG` (ircsock.cpp:1694-1701).
5. **`NAMES` and initial `TOPIC` are never requested after JOIN** — the client queues
   expectation cells (`qpInitialNames`/`qpInitialTopic`, ircsock.cpp:1408-1416) and relies
   on the server volunteering `353/366` and (optionally) `332` after JOIN. `bExecuteQuery`
   has **no** `ctNames` emit case (ircproto.cpp:1063-1215).
6. **Comic annotations ride two different vehicles** depending on mode: IRCX →
   out-of-band `DATA <target> CCUDI1 :<blob>` immediately before the `PRIVMSG`
   (ircproto.cpp:540-556); plain IRC → in-band `(#<blob>) ` prefix glued onto the
   `PRIVMSG` text (protsupp.cpp:3077-3097, parenthesis mode selected at
   protsupp.cpp:3242 when `!IsIRCX()`). See §4.1.
7. **Line discipline**: inbound is split on `\n` only (ircsock.cpp:1013-1029);
   `ParseIt` (ircsock.cpp:137-258) is the single parser for both inbound lines and
   outbound slash commands; max message length starts at 512 (`g_nDefaultIOBuff`,
   ircsock.h:12) and is **renegotiated upward from field 5 of the first `800` reply**
   (ircsock.cpp:2874-2882).

Compiled protocol set (from chat.mak OBJS): `ircproto.obj ircsock.obj protsupp.obj
chatsrv.obj query.obj webreq.obj filesend.obj histent.obj …` (chat.mak:82-139).
NOT compiled: `nmproto`, plus the Plan-2 dead files (`wmini`, `semantic`).

---

## 1. WIRE PLUMBING (who touches the socket)

| Component | Location | Role |
|---|---|---|
| `CIrcSocket serverConn` (global) | ircproto.cpp:27, ircsock.h:438-501 | The one TCP socket. Owns login state machine, inbound dispatch, query list |
| `CIrcProto : CRoomInfo` | ircproto.h:10-78 | Per-document protocol object; all point at `serverConn` (`m_pSock`, ircproto.cpp:40) |
| `CIrcProto::SendMessageText` | ircproto.cpp:475-478 | The single outbound write: `m_pSock->Send(sz, strlen(sz))` |
| `CIrcSocket::OnReceive` → `ProcessMessage` | ircsock.cpp:1000-1032 / 1115-1159 | Buffer, split on `\n`, `ParseIt`, route to `HandleCommand` / `HandleResultCode` / `HandleErrorCode` |
| `CChatServiceConnector` | chatsrv.cpp:1386-1996 | Multi-server DNS/connect racing; on success re-attaches winsock handle to `serverConn` (chatsrv.cpp:1816-1828) — **no wire traffic of its own** |
| `CCQuery` / `m_queries` | query.h:80-131 | Request/response correlation: each emitted query pushes a `(purpose, commandType)` cell; numeric handlers pop the oldest matching cell to decide what a reply *means* |
| `GetOutBuff()` | ui.h:18 | Shared formatted-output scratch buffer used by nearly every emit site |
| identd | ircproto.cpp:1406-1457 | Tiny inbound TCP :113 server, replies `<query> : USERID : UNIX : <username>\r\n` (ircproto.cpp:1454) during connect (`StartIdentD`/`StopIdentD`, stopped at login ircproto.cpp:1083) |

Outbound payload quoting: message bodies are DLE-quoted (`g_chLLQuoteCTCP = 0x10`,
../artifacts/inc/ccommon.h:41) for embedded `\r`/`\n` before sending
(ircproto.cpp:497) and unquoted on receipt (protsupp.cpp:848, 1563). Long messages are
split into multiple PRIVMSGs at word boundaries with formatting-state carry-over
(`nGetBreakingPoint`, ircproto.cpp:398-472; chunk loop ircproto.cpp:618-691).

Encoding: channel names starting `%` and IRCX extended nicknames (leading `'`) are UTF-8
encoded/decoded (`EncodeChan`/`DecodeChan` ircproto.cpp:300-365, `EncodeNick`/`DecodeNick`
ircproto.cpp:204-248); `#`/`&` channels and message text stay in the local DBCS/ANSI
codepage unless the room is a `%` room (`EncodingType`, ircproto.cpp:1261-1265).

---

## 2. EMITTED COMMANDS (what the 2.5 client actually sends)

**28 distinct top-level verbs are actively emitted** (25 hard-coded + `ISON`/`KILL`/
`NAMES`/`USERHOST` via the slash syntax table), plus an unrestricted raw passthrough.

### 2.1 Connect / login state machine

| # | Wire format (exact template) | Emit site | Trigger |
|---|---|---|---|
| 1 | `MODE ISIRCX\r\n` | ircproto.cpp:1140 (via `bExecuteQuery ctModeIsIrcX`) | First bytes after TCP connect (`OnConnect`, ircsock.cpp:1050-1052); starts 50 s fallback timer `ID_ISIRCXTIMEOUT` (defines.h:202) |
| 2 | `IRCX\r\n` | ircproto.cpp:1136 (`ctIrcX`) | After first `800` reply with flag `0` — switches server into IRCX mode (ircsock.cpp:2887) |
| 3 | `AUTH <pkg> I :<blob>\r\n` (initial) / `AUTH <pkg> S :<blob>\r\n` (continue) | ircsock.cpp:991 | IRCX SSPI login (`HrIrcXLogin`→`HrAuthenticate`→`HrGenerateAndSendAuthMsg`, ircsock.cpp:671-997). Package list from 800 field 4 (`NTLM`, `ANON`, MSN/DPA looping ircsock.cpp:707) |
| 4 | `PASS <password>\r\n` | ircsock.cpp:640 | `HrIrcLogin` when a password is set (plaintext auth or channel-server pass) |
| 5 | `NICK <nick>\r\n` | ircproto.cpp:906 (`ChatChangeNick`) | Login (ircsock.cpp:644) + user nick change (`ChatSetNick` protsupp.cpp:3965-3987, retry dialog `TryNewNick` protsupp.cpp:177-212). IRCX extended nicks pre-encoded `'<utf8>` (ircproto.cpp:903-904) |
| 6 | `USER <user> <machinename> . :<realname>\r\n` | ircsock.cpp:653 | Registration (`HrIrcLogin`; servername field is literally `.`; spaces stripped from user, ircsock.cpp:612-626) |
| 7 | `OPER <user> <password>\r\n` | ircsock.cpp:784 (`HrIrcSetOper`) | Plain-IRC + `authtypePlainText` (ircsock.cpp:661-665); retried on `464` (ircsock.cpp:3197-3202) |
| 8 | `PONG :<token>\r\n` | ircsock.cpp:1696-1698 | Answer to server `PING` |

### 2.2 Room lifecycle

| # | Wire format | Emit site | Trigger |
|---|---|---|---|
| 9 | `JOIN <chan>\r\n` / `JOIN <chan> <key>\r\n` | ircproto.cpp:810 / 815 (`ChatJoinAux`) | Enter room (after optional `PROP <chan> PICS` ratings pre-check when IRCX, ircproto.cpp:786-793) |
| 10 | `CREATE <chan>[ <modes>][ <maxusers>][ <key>]\r\n` | ircproto.cpp:845 (`ChatCreateAux`) | Create-room flow (`/create`, room-list "Create"). **Emitted even on plain IRC** (the non-IRCX branch of `ChatCreateChannel` ircproto.cpp:796-803 still calls `ChatCreateAux`); a plain ircd answers 421-or-90x, see §5 |
| 11 | `PART <chan>\r\n` | ircproto.cpp:772; also protsupp.cpp:4247 (channel-limit rejection) | Leave room / disconnect |
| 12 | `TOPIC <chan> :<topic>\r\n` (set) / `TOPIC <chan>\r\n` (get, used by room-list "List Members") | ircproto.cpp:1107 / 1110 (`ctTopic`, purposes `qpSetTopic`/`qpListMembers`) | Topic edit dialog; member listing (`ListMembers` protsupp.cpp:2801-2805) |
| 13 | `KICK <chan> <nick> :<reason>\r\n` | ircproto.cpp:854 | Kick dialog (preceded by `WHOIS` for ban pattern, ircproto.cpp:1004-1007) |
| 14 | `INVITE <nick> <chan>\r\n` | ircproto.cpp:895 | Invite dialog (protsupp.cpp:3916) |
| 15 | `MODE <chan>\r\n` | ircproto.cpp:1144 (`ctGetChannelMode`) | Automatically after own JOIN/CREATE echo (ircsock.cpp:1419, 1264) |
| 16 | `MODE <chan> -<modes> <key>\r\n` + `MODE <chan> +<modes> <maxusers> <key>\r\n` | ircproto.cpp:966 / 973 (`ChatSetMode`) | Room-properties dialog; mode chars `psitnmlk` (`GetModeChars`, ircproto.cpp:86-98) |
| 17 | `MODE <chan> +b\r\n` (list bans) | ircproto.cpp:1016; ircsock.cpp:2048 | Ban dialog open (reply consumed by 367/368) |
| 18 | `MODE <chan> +b <mask>\r\n` / `-b <mask>` | ircproto.cpp:883 (`ChatBanUser`) | Ban/unban |
| 19 | `MODE <chan> +o/-o/+v/-v <nick>\r\n` | protsupp.cpp:3678, 3682, 3689, 3694 (`ChatSetOperator`) | Host/participant/spectator promotion UI |
| 20 | `MODE <nick> +i\r\n` / `-i\r\n` | ircproto.cpp:1172 (`ctSetUserMode`, `qpSetInvisible`/`qpSetVisible`) | Visibility option; **sent automatically at every login** (`OnLogin`→`SetVisibility`, ircproto.cpp:1093, 1255-1258) |

### 2.3 Talk path (see §4 for payload grammar)

| # | Wire format | Emit site | Trigger |
|---|---|---|---|
| 21 | `PRIVMSG <target> :<annotations><text>\r\n` | ircproto.cpp:554-556 (short), 648-654 (chunked) | Say/think/whisper/action/sound/CTCP — the workhorse. `<target>` = channel (say) or nick (whisper — **outbound whispers are plain PRIVMSG even on IRCX**; the client never emits `WHISPER`) |
| 22 | `NOTICE <target> :<payload>\r\n` | same sites, `bAsNotice=TRUE` | CTCP *replies* (VERSION/PING/TIME/EMAIL/URL/NETMEET-refuse, protsupp.cpp:1139-1163, 1322-1323, 1803-1804) |
| 23 | `DATA <target> CCUDI1 :<annotations>\r\n` | ircproto.cpp:542, 637 | IRCX only: comic annotation blob sent immediately before its PRIVMSG (`CCUDI1` = "Comic Chat User Display Info v1", ircproto.h:89) |
| 24 | `AWAY :<msg>\r\n` / `AWAY\r\n` | ircproto.cpp:924 / 926 | Away toggle (server-level; additionally broadcast as CTCP `\x01AWAY\x01` to the room, protsupp.cpp:3744-3769) |

### 2.4 Queries

| # | Wire format | Emit site | Trigger |
|---|---|---|---|
| 25 | `WHO\r\n` / `WHO <chan>\r\n` / `WHO <mask>\r\n` | ircproto.cpp:1088-1095 | Auto after JOIN (ircsock.cpp:1420); user-list dialog (`ChatFillUserList`, ircproto.cpp:136-184); rules/notification daemons (`qpOnConnectEvent`…) |
| 26 | `WHOIS <nick>\r\n` | ircproto.cpp:1100 | Get identity, kick/ban dialogs, ignore-by-ident (ircproto.cpp:980-1006, 1245) |
| 27 | `LIST\r\n` / `LIST <chan>\r\n` | ircproto.cpp:1119-1121; protsupp.cpp:2657 | Room list on plain IRC; `/list` |
| 28 | `LISTX\r\n` / `LISTX N=<mask>\r\n` | ircproto.cpp:1126-1128; protsupp.cpp:2659 | Room list on IRCX (ratings-aware via 813) |
| 29 | `LUSERS\r\nMOTD\r\n` (two commands, one send) | ircproto.cpp:1132 | Auto after 001 (`qpInitialLUsersMOTD`, ircsock.cpp:1888-1890) and "MOTD" menu (`bChatShowMOTD`, ircproto.cpp:1222-1225) |
| 30 | `PROP <chan> PICS\r\n` / `PROP <chan> CLIENT\r\n` | ircproto.cpp:1192 (`ctPropGet`) | IRCX: ratings check before join/create (ircproto.cpp:788-799); backdrop/client-data fetch after join (ircsock.cpp:1422-1423) |
| 31 | `PROP <chan> CLIENT :<keystring>\r\n` | ircproto.cpp:1208 (`ctPropSet`) | Room owner sets client data, e.g. backdrop `bk=<name>,<url>` (`ChatSetClientData` ircproto.cpp:721-735; `ChangeProperty` ircproto.cpp:1389-1404) |

### 2.5 Slash commands (user-typed; `ProcessSlashCommand`, protsupp.cpp:2886-3003)

Recognized commands route through dedicated builders; **anything unrecognized is sent
raw** (`SlashRaw`, protsupp.cpp:2849-2876: format `"%s\r\n"` at :2873) — with three
guards: `/quote JOIN|CREATE` blocked (security, :2851-2858), `LIST/LISTX` ratings-gated
(:2859-2864), `MODE` with args registered for reply correlation (:2865-2871).

| Slash | Builder | Wire result |
|---|---|---|
| `/me`, `/action`, `/think` | `SlashMeOrThink` protsupp.cpp:2603 | PRIVMSG with `\x01ACTION`/think annotation |
| `/msg`, `/privmsg` | `SlashPrivMsg` protsupp.cpp:2503 → `bSendPrivMsg` :3834 | PRIVMSG (whisper or external-channel send) |
| `/sound` | `SlashSound` protsupp.cpp:2526 | PRIVMSG `\x01SOUND …\x01` |
| `/join`, `/create`, `/part`, `/nick`, `/away`, `/list`, `/who`, `/mode`, `/prop`, `/server` | protsupp.cpp:2406-2733 | As §2.1-2.4 (`/server` reconnects the socket) |
| `/invite /ison /kick /kill /names /topic /userhost /whois` | `SlashGeneric` protsupp.cpp:2224-2307 + syntax table ircsock.h:394-419 | `<VERB> <encoded args>\r\n` (protsupp.cpp:2297-2298) — this is how `ISON`, `KILL`, `NAMES`, `USERHOST` get emitted |
| anything else (`/knock`, `/access`, `/auth`, `/kline`, …) | `SlashRaw` default (protsupp.cpp:2982) | Verbatim passthrough |

The command table `g_rgIrcCmd` (ircsock.cpp:36-85, 47 entries, must stay sorted — binary
search `NGetCmd` ircsock.cpp:92-134) exists mostly to (a) classify slash input and
(b) name inbound verbs; presence in the table does **not** mean the client emits it.

---

## 3. PARSED INBOUND SURFACE

Dispatch: `ProcessMessage` (ircsock.cpp:1115-1159) → numeric? → `HandleResultCode` /
`HandleErrorCode` (error iff `bIsErrorCode`, ircsock.h:290: 401-502, 503-557, 900-999)
→ else `HandleCommand`.

### 3.1 Verbs (`HandleCommand`, ircsock.cpp:1162-1846)

| Verb | Case at | Feature-level effect |
|---|---|---|
| `AUTH` | ircsock.cpp:1207 | SSPI continuation: `S` → next blob (`HrGenerateAndSendAuthMsg`); `*` → authenticated, finalize with NICK/USER (`AUTH NTLM * REGISB@REDMOND 0` shape, comment :1224) |
| `CLONE` | :1236 | IRCX room-clone notice → status window only |
| `CREATE` | :1242 | Own room-creation echo → open doc, queue `NAMES`/`TOPIC` expectations, emit `MODE`+`WHO` (+`PROP CLIENT` on IRCX) |
| `DATA` | :1275 | MS Chat annotation: requires `lastString[0]=='#'` and `args[2]=="CCUDI1"` (:1281-1286) → `OnDataMsg` (protsupp.cpp:4374): `"# …"` comment vs `#G…` UDI blob; `# Appears as` fan-out to all shared rooms (:1307-1317) |
| `ERROR` | :1325 | Show verbatim; special-cases "No IRC clients" (MIC-only); during ISIRCX probe → treated as probe failure → plain login (:1339-1343) |
| `INVITE` | :1348 | Invitation popup (`OnInvite`) |
| `JOIN` | :1361 | Other user → member add + rules; self → open doc + auto-queries (see CREATE). **Modernization already in tree**: accepts RFC2812 `JOIN #chan` without colon (:1364-1370) |
| `KICK` | :1430 | `OnKick` (protsupp.cpp:1922): comic "action" panel + part; self-kick closes room |
| `KILL`, `QUIT` | :1760 | Part the user from every room (no distinct QUIT UI) |
| `KILLED` | :1753 | Status-window confirmation (MS Chat server verb) |
| `KNOCK` | :1442 | Status window only (IRCX knock) |
| `MODE` | :1448 | Channel: `ParseChannelMode` (ircsock.cpp:299-400; chars `psitnml k q o v f y` — `q`=owner, `f`=IRCX no-format, `y`=MIC) + query-cell bookkeeping; user: visibility `+i/-i` sync (:1513-1572) |
| `NICK` | :1576 | Rename across docs; self-rename status line |
| `NOTICE`, `PRIVMSG` | :1614 | → `OnTextMsg` (protsupp.cpp:4358) → `ProcessComment` (`#`-comments) or `ProcessSay` (CTCP + chat text, §4) ; server notices (no user prefix) → status window (:1663-1664) |
| `PART` | :1668 | Self → close room; other → member remove + rules |
| `PING` | :1694 | **Emits** `PONG :<token>` |
| `PONG` | :1703 | Status window |
| `PROP` | :1709 | IRCX property-change push: `CLIENT` → `HandleClientDataChange` (backdrop sync via `bk` key, protsupp.cpp:3455-3470); `TOPIC` → topic update (:1737-1746) |
| `TOPIC` | :1779 | Topic change (with MS Chat inline-formatting codes stripped via `SzControlLess`) |
| `WHISPER` | :1832 | **IRCX inbound whisper**: `WHISPER <chan> <targetlist> :<text>` → treated as private message with talk-to list (`GetTalkTos`) → `OnTextMsg(MT_PRIVATEMSG|MT_WHISPER)` |
| `REPLY`, `REQUEST` | :1193-1205 | Recognized (IRCX data-request family) but **explicitly untreated** — TRACE only |
| *anything else* | :1179-1191 | TRACE + debug ASSERT, dropped |

Parsed-verb count: **22 handled + 2 recognized-no-op** of the 47 names in `g_rgIrcCmd`.

### 3.2 Numerics — replies (`HandleResultCode`, ircsock.cpp:1849-3003)

~95 explicit cases. The feature-bearing ones:

| Numeric(s) | Case at | Effect |
|---|---|---|
| `001` RPL_WELCOME | ircsock.cpp:1873 | Login complete: adopt server-assigned nick, `CompleteConnection`, queue `LUSERS\r\nMOTD`, run `OnLogin` (visibility MODE, auto-join/room-list) |
| `002-005` | :1897-1909 | Status text |
| `200-206,208,261` (TRACE), `211-219,241-244` (STATS), `256-259` (ADMIN), `221`, `364/365` (LINKS), `371/374` INFO, `351` VERSION, `391` TIME, `381/386` oper | various :1911-2125, 2712-2719, 2810-2815 | Status-window formatting only |
| `251-255,265,266` LUSERS | :1952 | Accumulate `m_strLUSER` for MOTD dialog |
| `301` AWAY, `305/306` | :2001-2019 | Away report lines |
| `302` USERHOST, `303` ISON | :1987-1999 | Status text with prefix |
| `311` WHOISUSER | :2021 | Query-purpose dispatch: ban-dialog pattern build (→ **emits** `MODE <chan> +b`), identity popup, ignore-by-ident |
| `312/313/317/319/320` | :2077 | Swallowed while a WHOIS query cell exists |
| `318` ENDOFWHOIS | :2091 | Pop WHOIS cell |
| `314/369` WHOWAS | :2113 | Status text |
| `321/322/323` LIST + `811/812/813/816/817` LISTX | :2311-2506 | Room-list dialog fill (LISTX adds mode-flags field, registered flag `r`, MIC flag `y`, and PICS rating via 813 gate `bPassesRatings`) or rules-daemon channel enumeration |
| `324` CHANNELMODEIS | :2127 | Absolute mode sync after join; triggers deferred creation-mode/topic set (`m_bSetMode` path :2149-2167) |
| `331` NOTOPIC / `332` TOPIC | :2197 / :2218 | Topic init; `qpListMembers` continuation → member-list dialog (`OnUserListAux`) |
| `341` INVITING | :2301 | "Invitation sent" acknowledgment |
| `352/315` WHO | :2576 / :2652 | Member ident harvesting (ignore-list on entry), user-list dialog rows, rules/notification user enumeration (`CreateUserFromWhoReply` protsupp.cpp:5032) |
| `353/366` NAMES | :2508 / :2551 | Initial member population (`bSingleJoin` per word, ircsock.cpp:270-276) |
| `367/368` BANLIST | :2721 / :2732 | Fill + open ban dialog |
| `375/372/377/376` MOTD, `422` | :2755-2808, 3102 | MOTD accumulation → `ShowMOTD` |
| `800` RPL_IRCX | :2817 | **The IRCX pivot.** Reply shape `800 * <state 0|1> <version> <pkglist> <maxmsglen> *` (comment :2819-2826). First (state 0): set `m_bIrcXServer=TRUE`, parse security packages (`ANON`→`m_bAnonAllowed`), grow buffers to `<maxmsglen>`, emit `IRCX`. Second (state 1): begin `HrIrcXLogin` (AUTH or anonymous NICK/USER) |
| `801-810` ACCESS/EVENT | :2902-2913 | Status-window only (client never emits ACCESS except via raw slash) |
| `818/819` PROPLIST/PROPEND | :2915 / :2962 | PICS ratings gate before join/create; `CLIENT` prop → backdrop/client data |

### 3.3 Numerics — errors (`HandleErrorCode`, ircsock.cpp:3006-3496)

35 explicit cases + default (red status-window line, :3020-3032, 3492-3493). Notable:

| Numeric(s) | Case at | Effect |
|---|---|---|
| `401/403/405/471/473/474/442/443` | :3034-3100, 3156, 3224-3251, 3416-3442 | Message boxes; failed-join cleanup of pending room-info cells |
| `422` NOMOTD | :3102 | Ends the LUSERS/MOTD sequence gracefully |
| `431/432/433` | :3128 | **Nick-retry dialog** (`TryNewNick`); `436/438/439` message boxes (:3138-3154) |
| `451` NOTREGISTERED | :3182-3187 | **The ISIRCX fallback**: treated as "plain IRC server" → `HrModeIsIrcXFailure` → immediate `PASS`/`NICK`/`USER` login (ircsock.cpp:560-584) |
| `464` PASSWDMISMATCH | :3197 | Re-prompt + re-`OPER` |
| `475` BADCHANNELKEY | :3253 | Password prompt + rejoin (`OnBadChannelPassword`) |
| `461/467/472/482/501/502` | :3189-3296 | Pop the pending MODE/TOPIC query cell so correlation stays sane |
| `552/553/556` IRCX join restrictions | :3299-3315 | Message boxes |
| `900-907` | :3317-3359 | **Dual-interpretation**: same codes are MIC-1.0 errors on non-IRCX (join/create/nick restrictions, ircsock.h:274-283) and IRCX `ERR_BAD*` on IRCX — branch on `m_bIrcXServer` |
| `910` AUTHENTICATIONFAILED / `912` UNKNOWNPACKAGE | :3361 / :3372 | Retry same package with prompt / advance to next package |
| `924` NOSUCHOBJECT | :3381 | `PROP` on nonexistent room → proceed to JOIN/CREATE anyway |

Full numeric constant inventory (including ones the client only displays): ircsock.h:57-286.

---

## 4. MS CHAT SPECIFICS — how comic data rides the wire

### 4.1 Annotation blob (UDI — "User Display Info")

Grammar (built in `bInsertAnnotations`, protsupp.cpp:3057-3099; parsed in
`ProcessUDIData` protsupp.cpp:1485-1542 and inline in `ProcessSay` protsupp.cpp:1566-1611):

```
#G<pose><emo><int> E<face><emo><int> [R] M<mode> [T<nick>[,<nick>…]]
   (no spaces; every value is a single byte, digit-encoded via IndexToByte = +'0')
G = torso/gesture index+emotion+intensity      (CGESTUREPREFIX 'G', defines.h:73)
E = face/expression index+emotion+intensity    (CEXPRESSIONPREFIX 'E')
R = pose was user-requested (not inferred)     (CREQUESTEDPREFIX 'R')
M = say mode: 1 say, 2 whisper, 3 think, 4 action (SM_*, defines.h:57-61; SM2BM protsupp.cpp:1035)
T = talk-to nickname list                      (CTALKTOPREFIX 'T')
```

Two transports:

- **IRCX**: `DATA <target> CCUDI1 :#G…E…M…\r\n` sent immediately *before* the
  `PRIVMSG`/`NOTICE` carrying the visible text (ircproto.cpp:540-549; on receive the blob
  is stashed on the sender's `CUserInfo` via `m_bbValidUDI` and consumed by the next text
  message, protsupp.cpp:1541, 1614-1624). Used **regardless of the "send comics data"
  option** when the server is IRCX (protsupp.cpp:827-828).
- **Plain IRC**: the same blob is parenthesized and glued in-band onto the text:
  `PRIVMSG #room :(#G…E…M…Tnick) actual text` (parenthesis form selected when
  `!IsIRCX()`, protsupp.cpp:3242; receive parse of `(#…) ` prefix protsupp.cpp:1566-1611).
  Gated by `g_bSendComicsData` (default TRUE, protsupp.cpp:72; toggled by the
  "Don't send Microsoft Chat specific information" option, protsupp.cpp:87-101).

### 4.2 `#`-comment protocol (in PRIVMSG text or DATA payload)

Handled by `ProcessComment` (protsupp.cpp:846-1020). Prefix constants ircproto.h:83-88
(note each embeds a **leading space**; on the wire the payloads start `"# "`):

| Wire text | Emit site | Receive behavior |
|---|---|---|
| `# Appears as <avname>.<url>` / `# Appears as <avname>` | protsupp.cpp:833/835 (`ChatAnnounceNewAvatar`, chatprot.h:42) | Avatar identity announce. Broadcast to room on self-join (histent.cpp:279) and on avatar change (avatar.cpp:598, protsupp.cpp:91); on receiving one from an unknown comic user, reply *privately* with own announce using deferred-URL `?` (protsupp.cpp:870-877, `DEFERRED_URL_STRING` ircproto.h:125); channel-less arrivals fan out to every shared room (ircsock.cpp:1307-1317, 1643-1654) |
| `# GetInfo` | protsupp.cpp:3419 (`ChatGetInfo`) | Profile request → answered with `# HeresInfo: <profile>` (protsupp.cpp:919-920, sent `BM_HERESINFO` so chunking keeps the prefix, ircproto.cpp:592-595) |
| `# HeresInfo: <text>` | protsupp.cpp:919 | Shown only if we asked (`IsRequestInfo(RF_PROFILE)` anti-spoof, protsupp.cpp:948) |
| `# GetCharInfo` | protsupp.cpp:3427 (`ChatGetAvatarInfo`) | Avatar-download handshake: ask target for name+real URL; reply is a private `# Appears as <name>.<url>` **with** URL (protsupp.cpp:934-935) |
| `# BDrop: <name>` | protsupp.cpp:3447 | Legacy backdrop sync (ops only, protsupp.cpp:972) |
| `# BDrop2: <name>,<url>` | protsupp.cpp:3438 | New backdrop sync with URL (ops only, :996); on IRCX additionally persisted as room `PROP CLIENT` key `bk` (protsupp.cpp:3450-3451, read back at :3459) |

**Avatar download itself is NOT on the IRC wire**: the URL from `# Appears as`/
`# GetCharInfo` is fetched over HTTP/FTP by the WinInet background-thread pool in
webreq.cpp (`InternetOpenUrl`, webreq.cpp:636; entry `SetUserAvatarRealInfo`,
userinfo.cpp:44, called from protsupp.cpp:887).

### 4.3 CTCP set (0x01-framed, inside PRIVMSG; replies inside NOTICE)

IDs declared ircproto.h:91-102. Dispatcher: `ProcessSay` protsupp.cpp:1626-1869
(also accepts the nonstandard `\x01*VERSION…` reply shape, :1816-1866; unknown CTCPs
are silently dropped and count toward flood detection, :1868-1869).

| CTCP | Request emit | Reply emit | Notes |
|---|---|---|---|
| `ACTION` | protsupp.cpp:3114-3120 (non-comic mode; thinks are converted to "thinks:" actions :3105-3112) | — | Inbound renders as comic action (:1630-1648) |
| `SOUND <file> [text]` | protsupp.cpp:3349 (`bChatSendSound`; filename CTCP-quoted, histent.cpp:772) | — | Inbound plays local `.wav/.mid` by name only (:1649-1658) — no transfer |
| `VERSION` | :3704 | NOTICE `\x01VERSION <ver> <comic/text mode>\x01` :1137-1139 | |
| `PING <secs>` | :3714 | NOTICE echo :1143 | RTT display :1183 |
| `TIME` | :3722 | NOTICE `<date>, <time>` :1151 | |
| `EMAIL` | :3730 | NOTICE `<email>` :1156 | MS-specific |
| `URL` | :3738 | NOTICE `<homepage>` :1161 | MS-specific |
| `NETMEET <hostlong-ip>` | :1376 (`ChatStartNetMeeting`; starts `ConferenceListen` thread) | NOTICE `NETMEET REFUSED|NOHAVE` :1322 | Accept → `ConferenceConnect` to caller's IP (:1300-1344) — NetMeeting call setup rides one CTCP, media is out-of-band |
| `AWAY <msg>` / `AWAY` | protsupp.cpp:3751/3753 (broadcast to channel + per-join private copies, histent.cpp:281-286) | — | Peer away display (:1777-1794) — MS Chat's user-to-user away, distinct from server `AWAY` |
| `CLIENTINFO` | — | NOTICE advertising `ACTION AWAY CLIENTINFO DCC EMAIL NETMEET PING SOUND TIME USERINFO URL VERSION` :1803 | Advertises `USERINFO` but has **no USERINFO handler** (only the `# GetInfo` mechanism) |
| `DCC SEND <file> <hostlong-ip> <port> <size>` | filesend.cpp:174-176 | — | Receiver parses (filesend.cpp:350-380), connects raw TCP, ack-counting protocol in `ReceiveFileThread`; `DCC CHAT` explicitly ignored (:353) |
| `X-VCHAT` | — | — | Recognized and dropped (:1811-1815) |

### 4.4 Room properties / client data

IRCX `PROP` is used for exactly three keys: `PICS` (read-only rating check before
join/create, ircproto.cpp:1183), `CLIENT` (read+write; a `;`-separated `key=value`
string — `ChangeKeyString`/`GetValueFromKeyString`/`EnumKeyString`,
protsupp.cpp:5133-5260 — of which only `bk` (backdrop) is interpreted,
protsupp.cpp:3459), and `TOPIC` (inbound push only, ircsock.cpp:1737). Only room
*owners* may write (`ChangeProperty`, ircproto.cpp:1394-1396).

---

## 5. PROTOCOL VARIANTS / MODE DETECTION

State machine (all in ircsock.cpp):

```
TCP connect
  └─ EMIT "MODE ISIRCX" + start 50 s timer            (OnConnect :1050-1052)
      ├─ RECV 800 (state 0) → m_bIrcXServer=TRUE; parse pkgs+maxlen; EMIT "IRCX"   (:2841-2887)
      │     └─ RECV 800 (state 1) → HrIrcXLogin        (:2889-2894)
      │           ├─ ANON allowed & no forced auth → PASS?/NICK/USER   (:679-680, 596-668)
      │           └─ else AUTH <pkg> I/S loop (NTLM/MSN/DPA via SSPI)  (:686-756, 886-997)
      │                 └─ RECV "AUTH <pkg> * <ident> 0" → NICK/USER finalize (:1224-1231)
      ├─ RECV 451 → plain IRC → HrIrcLogin (PASS?/NICK/USER [, OPER])  (:3182-3187, 560-584)
      ├─ RECV ERROR while probing → same fallback                       (:1339-1343)
      └─ 50 s timeout (ID_ISIRCXTIMEOUT) → same fallback                (chat.cpp:2596-2599, mainfrm.cpp:329)
RECV 001 → registered; LUSERS+MOTD; MODE <self> ±i; auto-join or room list (:1873-1894)
```

`IsIRCX()` = `m_pSock->m_bIrcXServer` (ircproto.h:63), reset on every disconnect
(`Reset`, ircsock.cpp:473-484; called from `ChatServerDisconnect` protsupp.cpp:4775).

Per-mode behavioral differences:

| Concern | Plain IRC | IRCX |
|---|---|---|
| Annotations | in-band `(#…) ` PRIVMSG prefix (protsupp.cpp:3242) | out-of-band `DATA … CCUDI1` (ircproto.cpp:540) |
| Room list | `LIST` (ircproto.cpp:1119) | `LISTX` + PICS gate (ircproto.cpp:1126, ircsock.cpp:2450) |
| Join pre-check | none | `PROP <chan> PICS` when ratings on (ircproto.cpp:788) |
| Nicknames | raw | UTF-8 `'`-prefixed extended nicks (ircproto.cpp:903, 1374) |
| Auth | optional `PASS` + `OPER` | SSPI `AUTH` packages (NTLM/MSN/DPA/ANON) |
| Room properties | not available (`ChatSetClientData` returns FALSE, ircproto.cpp:732) | `PROP CLIENT`, owner mode `+q`, whisper verb inbound |
| `+f` channel mode | ignored | no-format mode (`CM_NOFORMAT`, ircsock.cpp:365-375) |
| Errors 900-907 | MIC 1.0 meanings | IRCX `ERR_BAD*` meanings (ircsock.cpp:3317-3359) |
| Max message len | 512 | from 800 field 5 (ircsock.cpp:2874-2882) |

On a plain vanilla IRC server the client is a well-behaved (if chatty) RFC1459 client:
NICK/USER/JOIN/PRIVMSG/PART, comic data hidden inside `(#…)` message prefixes and
`#`-comment PRIVMSGs that other Comic Chat clients interpret and plain clients see as
odd text.

---

## 6. MINIMAL-SERVER CHECKLIST (for the Wine capture rig)

**Verdict: a stock ircd (ngircd / UnrealIRCd / InspIRCd) is SUFFICIENT to capture the
full plain-IRC command surface — which is the surface that matters, since the IRC-mode
annotation path is entirely client-to-client via PRIVMSG.** The IRCX path (`DATA`,
`PROP`, `LISTX`, `AUTH`, `800`) needs an IRCX server (MS Exchange Chat / a shim), or a
scripted fake — see MUST-NOT list.

MUST (client blocks or misbehaves without):

1. **Answer `MODE ISIRCX` sent before registration with `451` (or `ERROR`)** — this is
   the trigger for the plain-IRC login (ircsock.cpp:3182-3187). Every RFC-compliant ircd
   does this (evidence: ircorig.txt:1). If the server instead answers `421` (unknown
   command) the client does NOT fall back — it sits until the **50-second**
   `ID_ISIRCXTIMEOUT` fires (defines.h:202). ngircd answers 451 → fine; verify during
   rig bring-up, else captures start with a 50 s stall.
2. `001` welcome carrying the accepted nick as arg 1 (ircsock.cpp:1873-1894).
3. Echo the client's own `JOIN` back with full `:nick!user@host` prefix — self-detection
   compares prefix nick (ircsock.cpp:1377) and the ident length is learned from it
   (ircsock.cpp:1402-1406).
4. Volunteer `353`+`366` after JOIN (never requested; ircsock.cpp:2508-2574).
5. Reply to auto-emitted post-join `MODE <chan>` (→ `324`) and `WHO <chan>`
   (→ `352…315`) (ircsock.cpp:1419-1420; handlers :2127, :2576).
6. Relay `PRIVMSG`/`NOTICE` verbatim including 0x01, 0x10 (DLE) and the `(#…)`/`#`
   prefixes — annotation fidelity is the whole point of the capture.
7. `PING`/`PONG` keepalive (client answers, never initiates — ircsock.cpp:1694).
8. `LIST` → `321/322/323` for the room-list dialog (ircsock.cpp:2311-2506).
9. `WHOIS` → `311…318` — kick/ban/identity/ignore dialogs are driven off 311
   (ircsock.cpp:2021-2075).
10. `375/372/376` or `422` — the post-login LUSERS/MOTD query cell must be resolved
    (ircsock.cpp:2755-2808, 3102-3126).

SHOULD (exercised by common flows; stock ircds provide all):

- `TOPIC` set/get + `331/332`; `KICK`; `INVITE`+`341`; `MODE` `+o/-o/+v/-b`+`367/368`;
  `AWAY` (server-side) + `301/305/306`; `433` nick-collision (to capture the retry
  dialog); `NICK`/`PART`/`QUIT` relays.

Stock ircd will NOT provide (flag for the capture matrix — needs either MS Chat server,
an IRCX shim, or scripted injection):

- `800` reply → without it the client never sets `m_bIrcXServer`, so **`IRCX`, `AUTH`,
  `DATA`, `PROP`, `LISTX`, `CREATE`-with-modes, WHISPER-inbound and extended nicks are
  unreachable in live capture** (`CREATE` itself *is* still emitted by the create-room
  flow even on plain IRC — ircproto.cpp:796-803 — a stock server answers `421`, which
  lands in the untreated-error status path).
- Inbound `WHISPER`/`KNOCK`/`CLONE`/`KILLED`/`REQUEST`/`REPLY` verbs — can be injected
  by a puppet client/scripted server to exercise handlers ircsock.cpp:1832/1442/1236/1753/1193.
- `900-907` MIC error semantics and `552/553/556` IRCX errors.
- Note ngircd/UnrealIRCd will happily relay everything needed for the *comic* capture:
  `(#G…E…M…T…)` prefixes, `# Appears as`, `# GetInfo`/`# HeresInfo:`, `# BDrop2:`,
  all CTCPs, and DCC (direct TCP, server not involved).
- identd: the client runs its own :113 responder (ircproto.cpp:1417-1433); configure the
  ircd's ident timeout low or let it query — harmless either way.

Suggested capture matrix rows (client action → expected wire artifacts): connect
(MODE ISIRCX→451→NICK/USER), join, say (with/without comics data option), whisper,
think, action, sound, away toggle (AWAY + \x01AWAY\x01), avatar change (# Appears as),
profile request round-trip, backdrop change (# BDrop2 + # BDrop), kick+ban dialog
(WHOIS→MODE +b→KICK), room list, member list (TOPIC probe), nick collision (433),
DCC send, /quote passthrough.
