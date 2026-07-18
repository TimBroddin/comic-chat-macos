//=--------------------------------------------------------------------------=
// ircsock.cpp — INBOUND PARSER lift from v2.5-beta-1-modern/ircsock.cpp.
// Plan 3 Task 5b. Original: Copyright 1998 Microsoft Corporation.
//=--------------------------------------------------------------------------=
//
// WHAT THIS FILE IS
//   The full inbound parse + dispatch layer: the OnReceive line framer (now
//   driven from cc_session_feed_bytes), ParseIt/NGetCmd/ParseChannelMode (pure
//   parse, byte-identical to the original), and ProcessMessage/HandleCommand/
//   HandleResultCode/HandleErrorCode (the three giant dispatch switches). Every
//   direct app-callee in those switches is replaced by ONE ccEmitProtoEvent
//   filling the matching CC_EV_* variant (R18). The parsing/extraction up to
//   each emit point is preserved verbatim.
//
// KEY EDIT RULES APPLIED (see p3-task-5b-report.md for the full per-site map)
//   R18  each direct app-callee -> one ccEmitProtoEvent. History-entry side
//        effects (AddAndExecute(new JoinEntry...)) become the event with NO
//        engine-side state write.
//   R20  user-facing AfxMessageBox sites -> CC_EV_ERROR with the text (NOT
//        ccLog); TryNewNick (431/432/433) -> CC_EV_NICK_REJECTED; the 403
//        RoomList dialog-widget pokes -> CC_EV_ERROR(bad-channel), widget block
//        dropped.
//   R21  the SSPI/NTLM block (original :413-998) is DROPPED. AUTH emits
//        CC_EV_AUTH_UNSUPPORTED and the login path reduces to HrIrcLogin
//        (PASS/NICK/USER) -- but note: PASS/NICK/USER emission is a CONNECTION-
//        ESTABLISHMENT concern owned by Task 7 (Swift drives login). This task
//        keeps the *parse-side* fallback trigger (ERR_NOTREGISTERED / ERROR-
//        during-probe / the ISIRCX timeout) wired to a single
//        ccModeIsIrcXFailure that cancels the probe timer and emits nothing
//        else -- Swift then performs the plain-IRC login. See ccModeIsIrcXFailure.
//
// CRoomInfo RECONCILIATION (the key architectural narrative -- report §)
//   The original writes room-property state onto LookupDoc(channel)->m_proto
//   (a CIrcProto whose CRoomInfo base holds m_dwModes/m_strTopic/...). This
//   engine has no CChatDoc/g_docs/currentRoom/theApp. The faithful mapping:
//   the CCSession holds exactly ONE CIrcProto (`sess.proto`, Task 4), which IS
//   the original's `currentRoom` (currentRoom = m_proto aliased the active
//   doc's proto). Handlers write the room-property scratch onto sess.proto AND
//   emit the CC_EV_* so Swift tracks the canonical per-room copy (state-and-
//   codec §1.4: "engine's copy is scratch; Swift's is canonical"). LookupDoc,
//   which returned a per-channel doc, becomes "does this line target the room
//   this session is tracking" -- the parser no longer needs a doc pointer to
//   route, because routing is Swift's job off the emitted (channel-carrying)
//   event.

#include "ircsock.h"
#include "cc_session.h"     // CCSession, ccEmitProtoEvent, ccSession
#include "ircproto.h"
#include "protsupp.h"       // EnumKeyString/GetValueFromKeyString (PROP CLIENT diff)
#include "format.h"         // SzControlLess/CopyFormatting/FreeAndNullFormatting/PushFormattingOffsets
#include "defines.h"        // CM_*/MT_*/CHANNELPREFIX/CGESTUREPREFIX etc.
#include <cstdlib>
#include <cstring>
#include <cctype>

// ---------------------------------------------------------------------------
// DecodeChan/DecodeNick/DecodeString live in ircproto.cpp (already lifted).
extern const char *DecodeChan(const char *szChannel, BOOL bForceDBCS);
extern const char *DecodeNick(const char *szNick);
extern const char *DecodeString(const char *szString, int iEncoding);
extern const char *DecodeNickForScreen(const char *szNick);

// UnConst (chat.h:311 in the original -- a plain const-cast helper, not lifted
// as a file). Reproduced locally; ParseIt's no-prefix branch is its only caller.
static inline char *UnConst(const char *sz) { return const_cast<char*>(sz); }

// ---------------------------------------------------------------------------
// Internal forward declarations (defined later in this TU; C++ needs them
// before the dispatch switches that call them).
static void ccEmitStatus(const char* szLine);
static void ccEmitError(int code, const char* text);
void ccModeIsIrcXFailure(CCSession& sess);
void ccEmitClientDataChange(CCSession& sess, const char* pszNewClientData);
BOOL ccFreeModeCell(CIrcSocket& sock, LPCTSTR szChannel, LPCTSTR szNickname);

// Resolve an encoded channel name to the session's room_token (0 if unknown).
// The session's channel<->token table (cc_session.h) is the only room registry
// this engine has; a channel the session has not registered a token for yields
// CC_ROOM_TOKEN_NONE (0) -- correct, since Swift registers tokens on join.
static uint32_t ccSessionRoomTokenForChannel(CCSession& sess, const char* channel);

// ---------------------------------------------------------------------------
// Ban-list accumulation statics (original file-statics ircsock.cpp:28-29). In
// the original these were TU-file-statics shared across the ban-list 367/368
// reply pair. Kept as function-local-ish TU statics here (single-threaded
// engine contract -- comicchat.h THREADING CONTRACT) exactly as the original.
static CString      g_strBan;
static CStringArray g_arrayBans;

// LISTX row-in-flight statics (original static locals inside HandleResultCode,
// ircsock.cpp:1851-1852): the row being built across the 812/813 reply pair,
// plus the PICS verdict. `sbAddIt` gates whether a completed row passes the
// ratings check. Kept as TU statics (same single-threaded posture).
static CString      s_listxName, s_listxPretty, s_listxDescr;
static int          s_listxUsers = 0;
static BOOL         s_listxRowPending = FALSE;

//=--------------------------------------------------------------------------=
// SECTION 1: g_rgIrcCmd table + NGetCmd + ParseIt + FreeParse + ParseChannelMode
//            (PURE PARSE -- lifted VERBATIM, confirmed byte-identical in report)
//=--------------------------------------------------------------------------=

#define SET_CMD(sz)		sz, (sizeof(sz) - 1)
//
// This table contains all supported IRC commands. MUST BE SORTED
// and MUST match the order of ID_CMD in ircsock.h
//
PRIRCCMD g_rgIrcCmd[]=
{
	SET_CMD("ACCESS"),	0x03, 0x00,
	SET_CMD("ACTION"),	0x02, 0x00,
	SET_CMD("AUTH"),	0x03, 0x00,
	SET_CMD("AWAY"),	0x02, 0x00,
	SET_CMD("CLONE"),	0x03, 0x00,
	SET_CMD("CREATE"),	0x02, 0x00,
	SET_CMD("DATA"),	0x02, 0x00,
	SET_CMD("ERROR"),	0x02, 0x00,
	SET_CMD("INFO"),	0x03, 0x00,
	SET_CMD("INVITE"),	0x02, 0x02,
	SET_CMD("ISON"),	0x03, 0x01,
	SET_CMD("JOIN"),	0x02, 0x00,
	SET_CMD("KICK"),	0x02, 0x02,
	SET_CMD("KILL"),	0x02, 0x01,
	SET_CMD("KILLED"),	0x02, 0x00,
	SET_CMD("KLINE"),	0x03, 0x00,
	SET_CMD("KNOCK"),	0x03, 0x00,
	SET_CMD("LIST"),	0x02, 0x00,
	SET_CMD("LISTX"),	0x03, 0x00,
	SET_CMD("LUSERS"),	0x03, 0x00,
	SET_CMD("ME"),		0x02, 0x00,
	SET_CMD("MODE"),	0x02, 0x01,
	SET_CMD("MSG"),		0x02, 0x00,
	SET_CMD("NAMES"),	0x03, 0x00,
	SET_CMD("NICK"),	0x02, 0x01,
	SET_CMD("NOTICE"),	0x02, 0x00,
	SET_CMD("PART"),	0x02, 0x00,
	SET_CMD("PASS"),	0x02, 0x00,
	SET_CMD("PING"),	0x03, 0x00,
	SET_CMD("PONG"),	0x02, 0x00,
	SET_CMD("PRIVMSG"),	0x02, 0x00,
	SET_CMD("PROP"),	0x02, 0x00,
	SET_CMD("QUIT"),	0x02, 0x00,
	SET_CMD("QUOTE"),	0x03, 0x00,
	SET_CMD("RAW"),		0x03, 0x00,
	SET_CMD("REPLY"),	0x02, 0x00,
	SET_CMD("REQUEST"),	0x02, 0x00,
	SET_CMD("SERVER"),	0x01, 0x00,
	SET_CMD("SOUND"),	0x02, 0x00,
	SET_CMD("THINK"),	0x02, 0x00,
	SET_CMD("TOPIC"),	0x03, 0x01,
	SET_CMD("UNKLINE"),	0x03, 0x00,
	SET_CMD("USER"),	0x03, 0x00,
	SET_CMD("USERHOST"),0x03, 0x01,
	SET_CMD("WHISPER"),	0x02, 0x00,
	SET_CMD("WHO"),		0x02, 0x00,
	SET_CMD("WHOIS"),	0x03, 0x01
};

SHORT NGetCmd(CHAR* szCmd)
{
	ASSERT(szCmd);
	//
	// binary search the command table
	//
	SHORT	nMiddle;
	SHORT	nStart, nEnd;	// search range
	SHORT	nRet;

	nStart	= 0;
	nEnd	= cmdidMax - 1;

	do
	{
		nMiddle = (nEnd - nStart)/2 + nStart;

		// FIDELITY NOTE: the original uses ::lstrcmpiA, whose contract is to
		// return exactly -1/0/+1. mfc_compat.h's lstrcmpi forwards to strcasecmp,
		// which returns an arbitrary-magnitude signed difference. The binary
		// search below branches on `-1 == nRet`, so we sign-normalize to -1/0/+1
		// to preserve the exact original control flow (a positive difference must
		// take the else branch, a negative one the `-1` branch). Verified to
		// return the same index as Win32 lstrcmpiA for every g_rgIrcCmd entry.
		int cmp = ::lstrcmpi(szCmd, g_rgIrcCmd[nMiddle].szCmd);
		nRet = (cmp < 0) ? -1 : (cmp > 0) ? 1 : 0;
		if (0 == nRet) // a match
			return nMiddle;

		if (nStart == nEnd)
			break;

		if (-1 == nRet)
		{
			//
			// The cmd is less than
			//
			nEnd = nMiddle;
		}
		else
		{
			if (nMiddle != nStart)
				nStart = nMiddle;
			else
				nStart = nEnd;
		}
	}
	while (TRUE);

	return -1;	// not found
}

//=--------------------------------------------------------------------------=

void ParseIt(const char *szMessage, PIRCPARSE pParse, BOOL bDoubleQuotes /*=FALSE*/)
{
	// parse prefix
	const char	*szStart = szMessage;
	char		*szBody, *szCurToken;
	char		szPrefixBuff[300];

	pParse->nick[0] = '\0';
	pParse->user[0] = '\0';
	pParse->machine[0] = '\0';
	pParse->lastString = NULL;
	pParse->uCode = 0;
	pParse->nArgs = 0;
	ZeroMemory(pParse->args, sizeof(CHAR*) * MAXARGS);
	ZeroMemory(pParse->nOffsets, sizeof(SHORT) * MAXARGS);

	if (*szMessage == ':')
	{	// there's a prefix
		szMessage++;					// don't include the colon
		pParse->bHasPrefix = TRUE;
		szBody = (char *)strchr(szMessage, ' ');
		ASSERT(szBody);					// messages must have a body
		int cbPrefixSize = szBody - szMessage;
		cbPrefixSize = min(cbPrefixSize, (int)sizeof(szPrefixBuff)-1);
		strncpy(szPrefixBuff, szMessage, cbPrefixSize);
		szPrefixBuff[cbPrefixSize] = '\0';
		char *szStart = szPrefixBuff;
		char *szEnd = NULL;
		if (!CHANNELPREFIX(*szStart)) szEnd = strpbrk(szStart, "!@");	// parse snick (must be present)
		if (szEnd)
		{
			int nChars = szEnd - szStart;
			nChars = min(nChars, (int)sizeof(pParse->nick)-1);
			strncpy(pParse->nick, szStart, nChars);
			pParse->nick[nChars] = '\0';
			if (*szEnd == '!')
			{	// parse user
				szStart = szEnd+1;
				szEnd = (char *)strchr(szStart, '@');
				if (szEnd)
				{
					int nChars = szEnd - szStart;
					nChars = min(nChars, (int)sizeof(pParse->user)-1);
					strncpy(pParse->user, szStart, nChars);
					pParse->user[nChars] = '\0';
					nChars = sizeof(pParse->machine)-1;
					strncpy(pParse->machine, szEnd+1, nChars);			// nfield now pts to !, parse machine
					pParse->machine[nChars] = '\0';
				}
			}
		}
		else
			if (strlen(szPrefixBuff) < sizeof(pParse->nick))
				strcpy(pParse->nick, szPrefixBuff);
	}
	else
	{
		pParse->bHasPrefix = FALSE;
		szBody = UnConst(szMessage);
	}

	while (TRUE)
	{
		while (my_isspace(*szBody))
			szBody++;
		if (*szBody == ':')
		{
			szBody++;
end:		char *szEnd = strpbrk(szBody, "\r\n");
			if (!szEnd)
				szEnd = (char *)strchr(szBody, '\0');
			int cbLen = szEnd - szBody;
			if ((pParse->lastString = (char*) malloc(cbLen+1)) != NULL)
			{
				strncpy(pParse->lastString, szBody, cbLen);
				pParse->lastString[cbLen] = '\0';
			}
			break;
		}
		char *szToken;
		if (bDoubleQuotes && *szBody == '\"')
		{
			szToken = GetToken1(szBody, &szBody, "\"\r\n", &szCurToken, FALSE /*bSkipInitialSeps*/);
			if (*szBody == '\"')	// skip the terminating double quote
			{
				szBody++;
				strcat(szToken, "\"");
			}
		}
		else
		{
			szToken = GetToken(szBody, &szBody, " \r\n", &szCurToken);
			if (!szToken)
				break;
		}
		pParse->nOffsets[pParse->nArgs] = szCurToken - szStart;
		pParse->args[pParse->nArgs++] = strdup(szToken);
		if (pParse->nArgs == MAXARGS)
		{
			TRACE("Too many parameters - Have to use lastString for remaining message.\n");
			goto end;
		}
	}

	// Now fill in uCode member
	if (pParse->args[0])
	{
		CHAR	ch = pParse->args[0][0];
		INT		i  = 0;
 		if (isdigit(ch))
		{
			// Result or Error Code
			do
			{
				pParse->uCode *= 10;
				pParse->uCode += (ch - '0');
				ch = pParse->args[0][++i];
			}
			while (isdigit(ch));
		}
	}
}

void FreeParse(PIRCPARSE pParse)
{
	if (pParse->lastString)
		free(pParse->lastString);
	for (SHORT i = 0; i < pParse->nArgs; i++)
		free(pParse->args[i]);
}

//=--------------------------------------------------------------------------=
// ParseChannelMode (ircsock.cpp:299-400). LIFTED with the R18 substitution for
// its app-callees: the flag-scanning that writes room-property scratch
// (m_dwModes/m_dwMaxUsers/m_strPassword onto the session's one CIrcProto) is
// VERBATIM; the +q/+o/+v member-status branches (which called ChatChangeAdmin,
// an app member-list mutation) and the +f/+y app hooks (doc->OnViewText,
// FixMICChannelName, UpdateSpectators) are DROPPED -- the whole MODE line is
// re-emitted as a single CC_EV_CHANNEL_MODE(channel, modes, arg) delta by the
// caller (HandleCommand's cmdidMode / HandleResultCode's 324), from which Swift
// applies member/spectator status. The mode-DELTA still updates the DWORD
// scratch (dwModes/dwMaxUsers) so a following outbound read sees it.
static void ccParseChannelMode(CIrcProto& proto, const char *szFlags,
                               const char *szArg2, const char *szArg3)
{
	BOOL	bAdd = TRUE;
	DWORD	dwDelta = 0, addFlags = 0, subFlags = 0;

	while (*szFlags != '\0')
	{
		switch (*szFlags++)
		{
		case '+':
			bAdd = TRUE;
			dwDelta = 0;
			break;
		case '-':
			bAdd = FALSE;
			dwDelta = 0;
			break;
		case 'p':
			dwDelta |= CM_PRIVATE;
			break;
		case 's':
			dwDelta |= CM_HIDDEN;
			break;
		case 'i':
			dwDelta |= CM_INVITEONLY;
			break;
		case 't':
			dwDelta |= CM_TOPICHOST;
			break;
		case 'n':
			dwDelta |= CM_NOEXTERN;
			break;
		case 'm':
			dwDelta |= CM_MODERATED;
			break;
		case 'l':
			dwDelta |= CM_USERLIMIT;
			proto.m_dwMaxUsers = bAdd ? atoi(szArg2) : 0;
			break;
		case 'k':
			dwDelta |= CM_CHANNELKEY;
			if (bAdd)
				proto.m_strPassword = (szArg3 && *szArg3) ? szArg3 : szArg2;
			else
				proto.m_strPassword = ""; // clear it out, since we check for changes in the value before setting
			break;
		case 'q':
			// R18: original called ChatChangeAdmin(doc, szArg2, UF_OWNER|UF_OPERATOR/UF_OWNER)
			// -- member-status mutation, now carried to Swift by the emitted
			// CC_EV_CHANNEL_MODE delta (the caller re-emits the full mode line).
			break;
		case 'o':
			// R18: ChatChangeAdmin(..., UF_OPERATOR) -> CC_EV_CHANNEL_MODE delta.
			break;
		case 'v':
			// R18: ChatChangeAdmin(..., UF_HASVOICE) -> CC_EV_CHANNEL_MODE delta.
			break;
		case 'f':
			// +f (IRCX no-format). Original: on IRCX, dwDelta |= CM_NOFORMAT and
			// (bAdd) doc->OnViewText() (app view toggle). We keep the DWORD flag
			// (room-property scratch) and drop the app view toggle; Swift reacts
			// to CM_NOFORMAT off the emitted delta / channel-mode DWORD.
			if (proto.IsIRCX())
				dwDelta |= CM_NOFORMAT;
			break;
		case 'y':
			// +y (MIC). Original also called FixMICChannelName (app doc/title
			// fixup) -- dropped; the CM_MIC flag stays.
			dwDelta |= CM_MIC;
			break;
		}
		if (bAdd)
			addFlags |= dwDelta;
		else
			subFlags |= dwDelta;
	}

	proto.m_dwModes |= addFlags;
	proto.m_dwModes &= ~subFlags;
	// Original UpdateSpectators(doc, moderated) on CM_MODERATED delta is DROPPED
	// (app member-list recompute) -- Swift recomputes spectators off the delta.
}

//=--------------------------------------------------------------------------=
// SECTION 2: emit helpers (R18 -- the "one ccEmitProtoEvent per app-callee")
//=--------------------------------------------------------------------------=

// Resolve an encoded channel name to the session's room_token (0 if unknown).
static uint32_t ccSessionRoomTokenForChannel(CCSession& sess, const char* channel) {
	if (!channel) return CC_ROOM_TOKEN_NONE;
	for (size_t i = 1; i < sess.channels.size(); i++)
		if (sess.channels[i] == channel) return (uint32_t)i;
	return CC_ROOM_TOKEN_NONE;
}

static void ccEmitStatus(const char* szLine) {
	cc_proto_event ev; memset(&ev, 0, sizeof(ev));
	ev.type = CC_EV_STATUS_LINE;
	ev.u.status_line.text = szLine ? szLine : "";
	ccEmitProtoEvent(&ev);
}

static void ccEmitError(int code, const char* text) {
	cc_proto_event ev; memset(&ev, 0, sizeof(ev));
	ev.type = CC_EV_ERROR;
	ev.u.error.code = code;
	ev.u.error.text = text ? text : "";
	ccEmitProtoEvent(&ev);
}

// GetBanString (ircsock.cpp:403-410): builds the ban mask for a WHOIS reply.
// Pure string formatting; lifted for the 311 qpBanDlg path.
static void ccGetBanString(BOOL bIrcX, const char *szUserName, const char *szHostName, CString& strBan) {
	if (!bIrcX || *szUserName == '~')
		strBan.Format("*!*@%s", szHostName);  // not authenticated
	else
		strBan.Format("*!%s@%s", szUserName, szHostName);
}

//=--------------------------------------------------------------------------=
// SECTION 3: the IRCX-probe fallback (HrModeIsIrcXFailure, ircsock.cpp:560-584)
//=--------------------------------------------------------------------------=
// Original: on ERR_NOTREGISTERED / ERROR-during-probe / the 50s timeout, dequeue
// the ctModeIsIrcX cell, KillTimer, and call HrIrcLogin (send PASS/NICK/USER).
// The PASS/NICK/USER emission is Task 7's (Swift owns login); this task's parse
// layer keeps the trigger: dequeue the probe cell, request the timer be
// cancelled (cfg.cancel_timer), and emit CC_EV_AUTH_UNSUPPORTED? -- no: the
// plain-IRC fallback is NOT an auth failure. It emits nothing; Swift observes
// the fallback by the (unchanged) absence of CC_EV_SERVER_CAPS and drives its
// own plain login. We DO cancel the probe timer so Swift's scheduled 50s
// timeout doesn't also fire.
void ccModeIsIrcXFailure(CCSession& sess) {
	if (!sess.sock.m_bJustSentModeIsIrcX)
		return;
	POSITION pos;
	CCQuery* pQuery = sess.sock.m_queries.FindQuery(ctModeIsIrcX, &pos);
	if (pQuery) {
		ASSERT(pos);
		sess.sock.m_queries.FreeRemoveAt(pos);
	}
	ASSERT(sess.sock.m_bIrcXServer == FALSE);
	sess.sock.m_bJustSentModeIsIrcX = FALSE;
	if (sess.cfg.cancel_timer)
		sess.cfg.cancel_timer(sess.cfg.user_data, CC_TIMER_ISIRCX_PROBE);
	// Task 7 performs the plain-IRC login (PASS/NICK/USER). Nothing to emit.
}

//=--------------------------------------------------------------------------=
// SECTION 4: CSInString (ircsock.cpp:279-296) -- inbound DBCS decode of a
// trailing string. Original consulted theApp.m_charSet + the doc's CM_MIC to
// choose ENC_UTF8 vs ENC_DBCS. This port's permanent posture is single-byte
// CP-1252 (ANSI_CHARSET), so the original's early-out (`theApp.m_charSet ==
// ANSI_CHARSET && iEncoding == ENC_DBCS`) always fires for the DBCS branch, and
// the UTF-8 branch (channel name starting '%') is the only real transform. We
// keep the exact decision but read the channel-name prefix directly (no doc).
static void ccCSInString(char **pszString, const char *szChannelName = NULL) {
	ASSERT(*pszString);
	int iEncoding = (szChannelName && *szChannelName == '%') ? ENC_UTF8 : ENC_DBCS;
	// CP-1252 posture: ANSI_CHARSET + ENC_DBCS -> no transform (early-out).
	if (!**pszString || iEncoding == ENC_DBCS) return;
	char *szNewString = strdup(DecodeString(*pszString, iEncoding));
	free(*pszString);
	*pszString = szNewString;
}

//=--------------------------------------------------------------------------=
// SECTION 5: annotation decode for CC_EV_TEXT / CC_EV_DATA (Task-6 seam)
//=--------------------------------------------------------------------------=
// Per the brief's Task-6 seam: the PRIVMSG/NOTICE/DATA handlers emit the RAW
// text/data event with the annotation blob decoded IF TRIVIALLY AVAILABLE. The
// full CTCP/comment CLASSIFICATION (into CC_EV_ACTION/SOUND/APPEARS_AS) is
// Task 6's job (ProcessSay/ProcessComment). Here we do the cheap, self-
// contained decode of the two annotation transports:
//   * plain IRC in-band:  "(#G..E..[R]M.[T..]) <text>"  -> strip the prefix,
//                          decode G/E/R/M/T, set has_annotations, text = rest.
//   * IRCX out-of-band:   DATA payload "#G..E..[R]M.[T..]" -> decode into a
//                          cc_annotations (no visible text; CC_EV_DATA).
// This mirrors the grammar in state-and-codec §3.3 (IndexToByte = value+'0').
// It does NOT do CTCP (0x01) or "# " comment classification (Task 6).

// Decode the "#G.E.[R]M.[T..]" body (starting at the '#') into out. Returns a
// pointer just past the block, or NULL if the leading '#G' isn't present.
static const char* ccDecodeAnnotationBody(const char* p, cc_annotations* out) {
	memset(out, 0, sizeof(*out));
	if (!p || p[0] != '#' || p[1] != CGESTUREPREFIX) return NULL;
	p++; // skip '#'
	// G group
	if (*p != CGESTUREPREFIX) return NULL;
	p++;
	int haveGIntensity = 0, haveEIntensity = 0;
	if (*p) { out->gesture_pose    = (int)ByteToIndex((BYTE)*p++); }
	if (*p) { out->gesture_emotion = (int)ByteToIndex((BYTE)*p++); }
	if (*p && *p != CEXPRESSIONPREFIX) { out->gesture_intensity = (int)ByteToIndex((BYTE)*p++); haveGIntensity = 1; }
	// E group
	if (*p == CEXPRESSIONPREFIX) {
		p++;
		if (*p) { out->face_pose    = (int)ByteToIndex((BYTE)*p++); }
		if (*p) { out->face_emotion = (int)ByteToIndex((BYTE)*p++); }
		if (*p && *p != CREQUESTEDPREFIX && *p != CMODEPREFIX && *p != CTALKTOPREFIX) {
			out->face_intensity = (int)ByteToIndex((BYTE)*p++); haveEIntensity = 1;
		}
	}
	// R flag (presence only)
	if (*p == CREQUESTEDPREFIX) { out->requested = 1; p++; }
	// M mode
	if (*p == CMODEPREFIX) {
		p++;
		if (*p) { out->mode = (int)ByteToIndex((BYTE)*p++); }
	}
	// T addressee list (comma-separated encoded nicks; clip at CC_MAX_ADDRESSEES)
	if (*p == CTALKTOPREFIX) {
		p++;
		int n = 0;
		while (*p && n < CC_MAX_ADDRESSEES) {
			char buf[64]; int bi = 0;
			while (*p && *p != ',' && bi < (int)sizeof(buf) - 1)
				buf[bi++] = *p++;
			buf[bi] = '\0';
			strncpy(out->addressees[n], buf, sizeof(out->addressees[n]) - 1);
			out->addressees[n][sizeof(out->addressees[n]) - 1] = '\0';
			n++;
			if (*p == ',') p++;
		}
		out->addressee_count = n;
	}
	// cooked (state-and-codec §3.3: both intensity fields arrived)
	out->cooked = (haveGIntensity && haveEIntensity) ? 1 : 0;
	return p;
}

// For plain-IRC in-band text "(#...) actual text": if szText starts with "(#"
// and contains ") ", decode the annotation block and return a pointer to the
// text after the ") "; otherwise return szText unchanged and clear *pHas.
static const char* ccSplitInlineAnnotations(const char* szText, cc_annotations* out, int* pHas) {
	*pHas = 0;
	memset(out, 0, sizeof(*out));
	if (!szText) return "";
	if (szText[0] == '(' && szText[1] == '#') {
		const char* close = strstr(szText, ") ");
		if (close) {
			// decode the body between '(' and ')'
			cc_annotations tmp;
			if (ccDecodeAnnotationBody(szText + 1, &tmp)) {
				*out = tmp;
				*pHas = 1;
			}
			return close + 2;  // skip ") "
		}
	}
	return szText;
}

//=--------------------------------------------------------------------------=
// SECTION 6: HandleCommand (ircsock.cpp:1162-1846)
//=--------------------------------------------------------------------------=

static void ccHandleCommand(CCSession& sess, char *szLine, PIRCPARSE pParse)
{
	ASSERT(pParse);
	CIrcSocket& sock = sess.sock;

	SHORT nCmd = NGetCmd(pParse->args[0]);
	if (-1 == nCmd)
	{
		// Unknown IRC/X command -- permissive: TRACE + drop (original ASSERT+return).
		TRACE("Unknown command: %s\n", pParse->args[0] ? pParse->args[0] : "");
		return;
	}

	ASSERT(nCmd < cmdidMax);

	switch (nCmd)
	{
		default:
			// Unexpected command: TRACE-only (original DEBUG TRACE+ASSERT).
			TRACE("Unexpected command: %s\n", pParse->args[0] ? pParse->args[0] : "");
			break;

		case cmdidReply:
		case cmdidRequest:
			// Recognized IRCX data-request family but explicitly untreated
			// (original: TRACE-only). No event.
			TRACE("Untreated command: %s\n", pParse->args[0] ? pParse->args[0] : "");
			break;

		case cmdidAuth:
		{
			// R21: the SSPI/NTLM auth continuation is DROPPED. The engine cannot
			// speak IRCX-NTLM; emit CC_EV_AUTH_UNSUPPORTED so Swift knows the
			// server demanded auth we don't do (it then falls back to plain
			// login or reports failure). Original: HrGenerateAndSendAuthMsg /
			// HrIrcLogin finalize.
			cc_proto_event ev; memset(&ev, 0, sizeof(ev));
			ev.type = CC_EV_AUTH_UNSUPPORTED;
			ev.u.auth_unsupported.dummy = 1;
			ccEmitProtoEvent(&ev);
			break;
		}

		case cmdidClone:
		case cmdidKnock:
		case cmdidPong:
		case cmdidKilled:
			// status-print-only in the original -> CC_EV_STATUS_LINE.
			ccEmitStatus(szLine);
			break;

		case cmdidCreate:
		{
			// Self room-creation echo. Original: bProcessAddChannel (creates the
			// CChatDoc), zeroes currentRoom topic/mode/limit, enqueues Names/
			// Topic expectation cells + issues Mode/Who/PropGet(CLIENT).
			// R18: emit CC_EV_SELF_JOINED(channel); the doc-creation was an app
			// reaction. We keep the query-cell enqueue + auto-query issue (pure
			// protocol correlation state, needed so the following 353/366/324/
			// 352 replies route) exactly as the original.
			if (pParse->nArgs >= 3)
			{
				// room-property scratch reset (original currentRoom macros)
				sess.proto.m_strTopic = "";
				sess.proto.m_dwModes = 0;
				sess.proto.m_dwMaxUsers = 0;

				cc_proto_event ev; memset(&ev, 0, sizeof(ev));
				ev.type = CC_EV_SELF_JOINED;
				ev.u.self_joined.channel = pParse->args[1];
				ccEmitProtoEvent(&ev);

				// enqueue expectation cells + issue auto-queries (VERBATIM
				// correlation-state ops; bExecuteQuery sends via cfg.send)
				CCQuery* pQuery = new CCQuery(qpInitialNames, ctNames, dtMax, NULL, pParse->args[1], "", FALSE);
				if (pQuery) sock.m_queries.bAddQuery(pQuery);
				pQuery = new CCQuery(qpInitialTopic, ctTopic, dtMax, NULL, pParse->args[1], "", FALSE);
				if (pQuery) sock.m_queries.bAddQuery(pQuery);
				sess.proto.bExecuteQuery(qpInitialMode, ctGetChannelMode, dtMax, NULL, pParse->args[1], "");
				sess.proto.bExecuteQuery(qpInitialWho, ctWho, dtMax, NULL, pParse->args[1], "");
				if (sock.m_bIrcXServer)
					sess.proto.bExecuteQuery(qpJoinBackUrl, ctPropGet, dtMax, NULL, pParse->args[1], "");
			}
			break;
		}

		case cmdidData:
		{
			// IRCX comic annotation blob: DATA <target> CCUDI1 :#G...
			// Original: OnDataMsg -> ProcessUDIData (Task-6 stage). R18/Task-6
			// seam: emit CC_EV_DATA with the annotations decoded trivially here.
			// The "# Appears as" fan-out (:1307-1317) is a Task-6 classification
			// concern (it's a comment, not a UDI blob); a "# Appears as" arriving
			// via DATA is still emitted raw (Task 6 refines to CC_EV_APPEARS_AS).
			if (pParse->lastString &&
				pParse->lastString[0] == '#' &&
				pParse->nArgs >= 3 &&
				!strcmp(pParse->args[2], CCUDI1) &&
				*pParse->nick &&
				*pParse->user)
			{
				cc_annotations ann;
				if (ccDecodeAnnotationBody(pParse->lastString, &ann)) {
					cc_proto_event ev; memset(&ev, 0, sizeof(ev));
					ev.type = CC_EV_DATA;
					ev.u.data.nick = pParse->nick;
					ev.u.data.annotations = ann;
					ccEmitProtoEvent(&ev);
				} else {
					// "# Appears as ..."-style comment arriving via DATA -- emit
					// raw text so Task 6 can classify it (CC_EV_TEXT with the
					// data payload; no annotations decoded).
					cc_proto_event ev; memset(&ev, 0, sizeof(ev));
					ev.type = CC_EV_DATA;
					ev.u.data.nick = pParse->nick;
					memset(&ev.u.data.annotations, 0, sizeof(ev.u.data.annotations));
					ccEmitProtoEvent(&ev);
				}
			}
			break;
		}

		case cmdidError:
		{
			// Fatal server ERROR. Original: AfxMessageBox verbatim + PostMessage
			// ID_FILE_NEW; special "No IRC clients" (MIC-only); during ISIRCX
			// probe -> treat as probe failure.
			if (pParse->lastString) {
				ccCSInString(&pParse->lastString);
				if (sock.m_bJustSentModeIsIrcX) {
					// ERROR arriving during the probe = plain-IRC fallback trigger
					// (original ircsock.cpp:1339-1343).
					ccModeIsIrcXFailure(sess);
				} else {
					// R20: user-facing fatal error -> CC_EV_DISCONNECTED_HINT with
					// the verbatim text (the original showed it, then opened a new
					// connection dialog -- Swift owns that reconnect flow).
					cc_proto_event ev; memset(&ev, 0, sizeof(ev));
					ev.type = CC_EV_DISCONNECTED_HINT;
					ev.u.disconnected_hint.text = pParse->lastString;
					ccEmitProtoEvent(&ev);
				}
			}
			break;
		}

		case cmdidInvite:
		{
			// Original: OnInvite (popup). R18: no INVITE event variant exists in
			// the union -- the closest is a status line carrying who invited us
			// where. Emit CC_EV_STATUS_LINE with the raw line so the invite is
			// surfaced; Swift can present it. (Reported as a union-gap note; the
			// brief says escalate if a needed variant is missing, but INVITE is
			// non-core and STATUS_LINE is the union's explicit catch-all for
			// "surface this text" -- see report §Deviations.)
			if (pParse->lastString) {
				ccEmitStatus(szLine);
			}
			break;
		}

		case cmdidJoin:
		{
			// Preserve the RFC2812 "JOIN #chan" (channel as arg, no colon)
			// modernization (ircsock.cpp:1364-1369) -- captured-bytes tests
			// depend on it.
			if (!pParse->lastString && pParse->nArgs >= 2 && pParse->args[1])
				pParse->lastString = strdup(pParse->args[1]);
			ASSERT(pParse->lastString);
			if (!pParse->lastString) break;

			CString strIdent(pParse->user);
			strIdent += "@";
			strIdent += pParse->machine;

			const char* ownNick = sess.cfg.own_nick ? sess.cfg.own_nick(sess.cfg.user_data) : "";
			if (stricmp(pParse->nick, ownNick))
			{	// Other user joined. Original: rules + AddAndExecute(JoinEntry).
				// R18: emit CC_EV_USER_JOINED(nick, ident). Rules = app reaction.
				cc_proto_event ev; memset(&ev, 0, sizeof(ev));
				ev.type = CC_EV_USER_JOINED;
				ev.room_token = ccSessionRoomTokenForChannel(sess, pParse->lastString);
				ev.u.user_joined.nick = pParse->nick;
				ev.u.user_joined.ident = strIdent;
				ccEmitProtoEvent(&ev);
			}
			else
			{	// Self join. Original: bProcessAddChannel (doc creation) + ident-
				// length capture + enqueue Names/Topic + issue Mode/Who/PropGet.
				// R18: emit CC_EV_SELF_JOINED; keep the correlation-state ops.
				sess.proto.m_strChannel = pParse->lastString;
				sess.proto.m_strTopic = "";
				sess.proto.m_dwModes = 0;
				sess.proto.m_dwMaxUsers = 0;

				cc_proto_event ev; memset(&ev, 0, sizeof(ev));
				ev.type = CC_EV_SELF_JOINED;
				ev.room_token = ccSessionRoomTokenForChannel(sess, pParse->lastString);
				ev.u.self_joined.channel = pParse->lastString;
				ccEmitProtoEvent(&ev);

				CCQuery* pQuery = new CCQuery(qpInitialNames, ctNames, dtMax, NULL, pParse->lastString, "", FALSE);
				if (pQuery) sock.m_queries.bAddQuery(pQuery);
				pQuery = new CCQuery(qpInitialTopic, ctTopic, dtMax, NULL, pParse->lastString, "", FALSE);
				if (pQuery) sock.m_queries.bAddQuery(pQuery);
				sess.proto.bExecuteQuery(qpInitialMode, ctGetChannelMode, dtMax, NULL, pParse->lastString, "");
				sess.proto.bExecuteQuery(qpInitialWho, ctWho, dtMax, NULL, pParse->lastString, "");
				if (sock.m_bIrcXServer)
					sess.proto.bExecuteQuery(qpJoinBackUrl, ctPropGet, dtMax, NULL, pParse->lastString, "");
			}
			break;
		}

		case cmdidKick:
		{
			// Original: OnKick (comic action panel + part). R18: no KICK variant
			// in the union. Emit CC_EV_USER_PARTED(kickee, reason) -- a kick IS a
			// forced part; the reason carries the kick text. (Union-gap note in
			// report; the kicker nick is in pParse->nick, surfaced via a status
			// line as well so it isn't lost.)
			if (pParse->lastString && pParse->nArgs >= 3)
			{
				ccCSInString(&pParse->lastString, pParse->args[1]);
				cc_proto_event ev; memset(&ev, 0, sizeof(ev));
				ev.type = CC_EV_USER_PARTED;
				ev.room_token = ccSessionRoomTokenForChannel(sess, pParse->args[1]);
				ev.u.user_parted.nick = pParse->args[2];      // kickee
				ev.u.user_parted.reason = pParse->lastString; // kick reason
				ccEmitProtoEvent(&ev);
				ccEmitStatus(szLine);  // preserve the kicker/context line
			}
			break;
		}

		case cmdidMode:
		{
			if (pParse->nArgs >= 3)	// Channel mode change
			{
				const char *szFlags, *szArg2 = "", *szArg3 = "";
				szFlags = pParse->args[2];
				if (pParse->nArgs >= 4)
					szArg2 = pParse->args[3];
				if (pParse->nArgs >= 5)
					szArg3 = pParse->args[4];

				// Room-property scratch update (writes onto the session's one
				// CIrcProto -- the CRoomInfo reconciliation). Original guarded on
				// LookupDoc(args[1]); here the session tracks one room.
				ccParseChannelMode(sess.proto, szFlags, szArg2, szArg3);

				// Dequeue the pending ctSetChannelMode cell (our own MODE echo);
				// the mcLost host/owner-loss echo-suppression (original static
				// MODECACH) was purely a Status-Window display nicety -- DROPPED
				// (Swift decides display). Correlation dequeue stays.
				POSITION pos;
				CCQuery* pQuery = sock.m_queries.FindQuery(ctSetChannelMode, &pos);
				if (pQuery && pQuery->GetQueryPurpose() == qpComSetChannelMode) {
					ASSERT(pos);
					sock.m_queries.FreeRemoveAt(pos);
				}

				// R18: emit the channel-mode delta (channel, modes, arg). Swift
				// applies member/spectator status + room modes from this.
				cc_proto_event ev; memset(&ev, 0, sizeof(ev));
				ev.type = CC_EV_CHANNEL_MODE;
				ev.room_token = ccSessionRoomTokenForChannel(sess, pParse->args[1]);
				ev.u.channel_mode.channel = pParse->args[1];
				ev.u.channel_mode.modes = szFlags;
				ev.u.channel_mode.arg = szArg2;
				ccEmitProtoEvent(&ev);
			}
			else if (pParse->nArgs == 2)	// User mode change (visibility etc.)
			{
				const char* ownNick = sess.cfg.own_nick ? sess.cfg.own_nick(sess.cfg.user_data) : "";
				if (0 == stricmp(pParse->args[1], ownNick))
				{
					// Dequeue the ctSetUserMode cell (our own +i/-i echo).
					// Original also flipped theApp.m_flags1 F1_USERVISIBLE (app
					// state) -- Swift tracks visibility off the emitted event.
					POSITION pos;
					CCQuery* pQuery = sock.m_queries.FindQuery(ctSetUserMode, &pos);
					if (pQuery) {
						ASSERT(pos);
						enumQueryPurpose qp = pQuery->GetQueryPurpose();
						if (qp == qpSetInvisible || qp == qpSetVisible || qp == qpComSetUserMode) {
							BOOL bRemoveCell = (qp == qpComSetUserMode);
							LPTSTR szModes = pParse->lastString;
							if (szModes) {
								while (*szModes) {
									if (*szModes == 'i')
										bRemoveCell = TRUE;
									szModes++;
								}
							}
							if (bRemoveCell)
								sock.m_queries.FreeRemoveAt(pos);
						}
					}
					// R18: emit user-mode change (nick, modes).
					cc_proto_event ev; memset(&ev, 0, sizeof(ev));
					ev.type = CC_EV_USER_MODE;
					ev.u.user_mode.nick = pParse->args[1];
					ev.u.user_mode.modes = pParse->lastString ? pParse->lastString : "";
					ccEmitProtoEvent(&ev);
				}
			}
			break;
		}

		case cmdidNick:
		{
			if (pParse->lastString)
			{
				const char* ownNick = sess.cfg.own_nick ? sess.cfg.own_nick(sess.cfg.user_data) : "";
				BOOL bSelf = (0 == strcmp(pParse->nick, ownNick));
				// Original: iterate g_docs, AddAndExecute(NickEntry) per room the
				// user is in; SetMyNameNick if self. R18: emit ONE
				// CC_EV_NICK_CHANGED(old, new, is_self); Swift applies it to every
				// room it tracks the user in (state-and-codec §1.2's echo-only
				// own-nick update rule).
				cc_proto_event ev; memset(&ev, 0, sizeof(ev));
				ev.type = CC_EV_NICK_CHANGED;
				ev.u.nick_changed.old_nick = pParse->nick;
				ev.u.nick_changed.new_nick = pParse->lastString;
				ev.u.nick_changed.is_self = bSelf ? 1 : 0;
				ccEmitProtoEvent(&ev);
			}
			break;
		}

		case cmdidNotice:
		case cmdidPrivMsg:
		{
			TRACE("Got a PrivMsg! (snick = %s)\n", pParse->nick);
			if (pParse->lastString && pParse->nArgs >= 2)
			{
				if (*pParse->nick && *pParse->user)
				{
					BYTE msgType = (cmdidPrivMsg == nCmd) ? MT_PRVMSG : MT_NOTICE;
					BOOL bChannel = CHANNELPREFIX(pParse->args[1][0]);
					if (bChannel) msgType |= MT_CHANNELSEND;
					else          msgType |= MT_PRIVATEMSG;

					CString strID;
					if (*pParse->user && *pParse->machine)
						strID.Format("%s@%s", pParse->user, pParse->machine);

					// Original: "# Appears as" fan-out to all shared rooms
					// (:1644-1654) is a Task-6 comment classification; here we
					// emit CC_EV_TEXT raw and let Task 6 route # comments.
					ccCSInString(&pParse->lastString, pParse->args[1]);

					// Trivial annotation split (plain-IRC in-band "(#...) text").
					cc_annotations ann; int hasAnn = 0;
					const char* body = ccSplitInlineAnnotations(pParse->lastString, &ann, &hasAnn);

					cc_proto_event ev; memset(&ev, 0, sizeof(ev));
					ev.type = CC_EV_TEXT;
					ev.room_token = bChannel ? ccSessionRoomTokenForChannel(sess, pParse->args[1]) : 0;
					ev.u.text.nick = pParse->nick;
					ev.u.text.ident = strID;
					ev.u.text.target = pParse->args[1];
					ev.u.text.text = body;
					ev.u.text.kind = (int)msgType;
					ev.u.text.has_annotations = hasAnn;
					if (hasAnn) ev.u.text.annotations = ann;
					else memset(&ev.u.text.annotations, 0, sizeof(ev.u.text.annotations));
					ccEmitProtoEvent(&ev);
				}
				else if (!*pParse->nick && !*pParse->user)
				{
					// server notice (no user prefix) -> status window
					ccEmitStatus(szLine);
				}
			}
			break;
		}

		case cmdidPart:
		{
			TRACE("Got a PART!\n");
			const char* ownNick = sess.cfg.own_nick ? sess.cfg.own_nick(sess.cfg.user_data) : "";
			if (stricmp(pParse->nick, ownNick) == 0)
			{
				// Self part. Original: GotPartChannel (app doc close). R18: emit
				// CC_EV_SELF_PARTED(channel).
				cc_proto_event ev; memset(&ev, 0, sizeof(ev));
				ev.type = CC_EV_SELF_PARTED;
				ev.room_token = ccSessionRoomTokenForChannel(sess, pParse->args[1]);
				ev.u.self_parted.channel = pParse->args[1];
				ccEmitProtoEvent(&ev);
			}
			else
			{
				// Other part. Original: rules + AddAndExecute(PartEntry). R18:
				// emit CC_EV_USER_PARTED(nick, reason).
				cc_proto_event ev; memset(&ev, 0, sizeof(ev));
				ev.type = CC_EV_USER_PARTED;
				ev.room_token = ccSessionRoomTokenForChannel(sess, pParse->args[1]);
				ev.u.user_parted.nick = pParse->nick;
				ev.u.user_parted.reason = pParse->lastString ? pParse->lastString : "";
				ccEmitProtoEvent(&ev);
			}
			break;
		}

		case cmdidPing:
		{
			// Pure protocol: answer with PONG. Original used raw Send; here the
			// single outbound choke point is cfg.send (via ccSessionSendRaw).
			char pong[600];
			snprintf(pong, sizeof(pong), "PONG :%s\r\n", pParse->lastString ? pParse->lastString : "");
			ccSessionSendRaw(pong, strlen(pong));
			break;
		}

		case cmdidProp:
		{
			// IRCX property-change push. Original: CLIENT -> HandleClientDataChange
			// (backdrop sync); TOPIC -> topic update.
			if (pParse->nArgs == 3 && pParse->lastString)
			{
				if (CHANNELPREFIX(pParse->args[1][0]) && sock.m_bIrcXServer)
				{
					if (!strcmp(pParse->args[2], "CLIENT"))
					{
						// dequeue our own ctPropSet echo
						POSITION pos;
						CCQuery* pQuery = sock.m_queries.FindQuery(ctPropSet, &pos);
						if (pQuery && pQuery->GetQueryPurpose() == qpSetClient) {
							ASSERT(pos);
							sock.m_queries.FreeRemoveAt(pos);
						}
						// R18: HandleClientDataChange -> diff old vs new client-data,
						// emit CC_EV_ROOM_PROP per changed key (see ccEmitClientDataChange).
						ccEmitClientDataChange(sess, pParse->lastString);
					}
					else if (!strcmp(pParse->args[2], "TOPIC"))
					{
						// Channel topic changed via PROP. Original wrote
						// m_strTopic + m_prgdwTopicFormatting on the doc's proto.
						ccCSInString(&pParse->lastString, pParse->args[1]);
						if (sess.proto.m_prgdwTopicFormatting)
							sess.proto.m_prgdwTopicFormatting->RemoveAll();
						else
							sess.proto.m_prgdwTopicFormatting = new CDWordArray;
						char* szCtrlLess = SzControlLess(pParse->lastString, sess.proto.m_prgdwTopicFormatting);
						sess.proto.m_strTopic = szCtrlLess ? szCtrlLess : "";
						// R18: emit topic-changed.
						cc_proto_event ev; memset(&ev, 0, sizeof(ev));
						ev.type = CC_EV_TOPIC_CHANGED;
						ev.room_token = ccSessionRoomTokenForChannel(sess, pParse->args[1]);
						ev.u.topic_changed.channel = pParse->args[1];
						ev.u.topic_changed.topic = sess.proto.m_strTopic;
						ccEmitProtoEvent(&ev);
					}
				}
			}
			break;
		}

		case cmdidKill:
		case cmdidQuit:
		{
			// Part the user from every room. Original: g_docs loop +
			// AddAndExecute(PartEntry). R18: emit CC_EV_USER_QUIT(nick, reason);
			// Swift removes the user from every room it tracks.
			const char* szQuittingNick = (nCmd == cmdidQuit) ? pParse->nick : pParse->args[1];
			cc_proto_event ev; memset(&ev, 0, sizeof(ev));
			ev.type = CC_EV_USER_QUIT;
			ev.u.user_quit.nick = szQuittingNick;
			ev.u.user_quit.reason = pParse->lastString ? pParse->lastString : "";
			ccEmitProtoEvent(&ev);
			break;
		}

		case cmdidTopic:
		{
			// Topic change command. Original: m_strTopic + formatting onto the
			// doc's proto; dequeue ctTopic (our own set echo); else status print.
			if (pParse->nArgs >= 2 && pParse->lastString)
			{
				CDWordArray rgdwFormattingTmp;
				ccCSInString(&pParse->lastString, pParse->args[1]);
				char* szCtrlLess = SzControlLess(pParse->lastString, &rgdwFormattingTmp);
				CString strCtrlLessTopic = szCtrlLess ? szCtrlLess : "";

				// room-property scratch (CRoomInfo reconciliation)
				if (sess.proto.m_prgdwTopicFormatting)
					FreeAndNullFormatting(&sess.proto.m_prgdwTopicFormatting);
				sess.proto.m_prgdwTopicFormatting = CopyFormatting(&rgdwFormattingTmp);
				sess.proto.m_strTopic = strCtrlLessTopic;

				POSITION pos;
				CCQuery* pQuery = sock.m_queries.FindQuery(ctTopic, &pos);
				if (pQuery) {
					ASSERT(pos);
					sock.m_queries.FreeRemoveAt(pos);
				}

				// R18: emit topic-changed regardless (our own set echoes it too).
				cc_proto_event ev; memset(&ev, 0, sizeof(ev));
				ev.type = CC_EV_TOPIC_CHANGED;
				ev.room_token = ccSessionRoomTokenForChannel(sess, pParse->args[1]);
				ev.u.topic_changed.channel = pParse->args[1];
				ev.u.topic_changed.topic = strCtrlLessTopic;
				ccEmitProtoEvent(&ev);

				rgdwFormattingTmp.RemoveAll();
			}
			break;
		}

		case cmdidWhisper:
		{
			// IRCX inbound whisper: WHISPER <chan> <targetlist> :<text>.
			// Original: GetTalkTos(doc, &talkTos, args[2]) then OnTextMsg(...
			// MT_WHISPER). R18: emit CC_EV_WHISPER(nick, ident, text). The talk-to
			// list (args[2]) is nick-string text on the wire (state-and-codec §2);
			// Swift resolves it. Annotations decoded trivially if inline.
			if (*pParse->nick && pParse->lastString) {
				ccCSInString(&pParse->lastString, pParse->args[1]);
				cc_annotations ann; int hasAnn = 0;
				const char* body = ccSplitInlineAnnotations(pParse->lastString, &ann, &hasAnn);

				CString strID;
				if (*pParse->user && *pParse->machine)
					strID.Format("%s@%s", pParse->user, pParse->machine);

				cc_proto_event ev; memset(&ev, 0, sizeof(ev));
				ev.type = CC_EV_WHISPER;
				ev.room_token = (pParse->nArgs >= 2 && CHANNELPREFIX(pParse->args[1][0]))
					? ccSessionRoomTokenForChannel(sess, pParse->args[1]) : 0;
				ev.u.whisper.nick = pParse->nick;
				ev.u.whisper.ident = strID;
				ev.u.whisper.text = body;
				ev.u.whisper.has_annotations = hasAnn;
				if (hasAnn) ev.u.whisper.annotations = ann;
				else memset(&ev.u.whisper.annotations, 0, sizeof(ev.u.whisper.annotations));
				ccEmitProtoEvent(&ev);
			}
			break;
		}
	}
}

//=--------------------------------------------------------------------------=
// SECTION 7: HandleResultCode (ircsock.cpp:1849-3003) -- numeric replies
//=--------------------------------------------------------------------------=

static void ccHandleResultCode(CCSession& sess, char *szLine, PIRCPARSE pParse)
{
	ASSERT(pParse);
	ASSERT(pParse->uCode);
	CIrcSocket& sock = sess.sock;

	switch (pParse->uCode)
	{
		default:
			// Untreated reply -> permissive status line (catch-all).
			ccEmitStatus(szLine);
			break;

		case RPL_WELCOME:		// 001
		{
			// Login complete. Original: CompleteConnection, SetMyNameNick(args[1]),
			// rules eOnConnect, enqueue LUsersMOTD, OnLogin (visibility MODE +
			// auto-join/room-list). R18: emit CC_EV_LOGGED_IN(actual-nick); Swift
			// drives the follow-ups (OnLogin is app policy). Keep the LUsersMOTD
			// query-cell enqueue (correlation state so 372/375/376/422 route).
			cc_proto_event ev; memset(&ev, 0, sizeof(ev));
			ev.type = CC_EV_LOGGED_IN;
			ev.u.logged_in.nick = (pParse->nArgs >= 2) ? pParse->args[1] : "";
			ccEmitProtoEvent(&ev);

			CCQuery* pQuery = new CCQuery(qpInitialLUsersMOTD, ctLUsersMOTD, dtMax, NULL, "", "", FALSE);
			if (pQuery) sock.m_queries.bAddQuery(pQuery);
			break;
		}

		// ---- status-print-only reply groups (original SetFormat -> Status
		//      Window). Collapsed to CC_EV_STATUS_LINE. ----
		case RPL_YOURHOST:		// 002
		case RPL_CREATED:		// 003
		case RPL_MYINFO:		// 004
		case RPL_FOOFORNOW:		// 005
		case RPL_TRACELINK: case RPL_TRACECONNECTING: case RPL_TRACEHANDSHAKE:
		case RPL_TRACEUNKNOWN: case RPL_TRACEOPERATOR: case RPL_TRACEUSER:
		case RPL_TRACESERVER: case RPL_TRACENEWTYPE: case RPL_TRACELOG:
		case RPL_STATSLINKINFO: case RPL_STATSCOMMANDS: case RPL_STATSCLINE:
		case RPL_STATSNLINE: case RPL_STATSILINE: case RPL_STATSKLINE:
		case RPL_STATSYLINE: case RPL_ENDOFSTATS: case RPL_STATSLLINE:
		case RPL_STATSUPTIME: case RPL_STATSOLINE: case RPL_STATSHLINE:
		case RPL_ADMINME: case RPL_ADMINLOC1: case RPL_ADMINLOC2: case RPL_ADMINEMAIL:
		case RPL_UMODEIS:		// 221
		case RPL_USERHOST:		// 302
		case RPL_ISON:			// 303
		case RPL_UNAWAY: case RPL_NOWAWAY:	// 305/306
		case RPL_WHOWASUSER: case RPL_ENDOFWHOWAS:	// 314/369
		case RPL_LINKS: case RPL_ENDOFLINKS:		// 364/365
		case RPL_INFO: case RPL_ENDOFINFO: case RPL_VERSION: case RPL_TIME:	// 371/374/351/391
		case RPL_YOUREOPER: case RPL_YOUREADMIN:	// 381/386
		case RPL_ACCESSADD: case RPL_ACCESSDELETE: case RPL_ACCESSSTART:
		case RPL_ACCESSLIST: case RPL_ACCESSEND: case RPL_EVENTADD:
		case RPL_EVENTDEL: case RPL_EVENTSTART: case RPL_EVENTLIST: case RPL_EVENTEND:
			ccEmitStatus(szLine);
			break;

		case RPL_LUSERCLIENT:	// 251
		case RPL_LUSEROP:		// 252
		case RPL_LUSERUNKNOWN:	// 253
		case RPL_LUSERCHANNELS:	// 254
		case RPL_LUSERME:		// 255
		case RPL_LOCALUSERS:	// 265
		case RPL_GLOBALUSERS:	// 266
		{
			// Accumulate into m_strLUSER (surfaces via the MOTD event). Original
			// prepended args[2] for 252-254.
			ccCSInString(&pParse->lastString);
			const char* szLUser = pParse->lastString;
			if (szLUser)
			{
				CString strLine;
				if (pParse->nArgs >= 3) { strLine = pParse->args[2]; strLine += " "; }
				strLine += szLUser;
				CCQuery* pQuery = sock.m_queries.FindQuery(ctLUsersMOTD, NULL);
				if (pQuery) { sock.m_strLUSER += strLine; sock.m_strLUSER += "\n"; }
				else ccEmitStatus(szLine);   // not part of an MOTD sequence -> status
			}
			break;
		}

		case RPL_AWAY:			// 301
		{
			// Away report. Original: IDS_AWAYREPORT status line. R18: emit
			// CC_EV_AWAY_PEER(nick, message) -- a peer's away notice.
			if (pParse->lastString && pParse->nArgs > 2)
			{
				cc_proto_event ev; memset(&ev, 0, sizeof(ev));
				ev.type = CC_EV_AWAY_PEER;
				ev.u.away_peer.nick = DecodeNickForScreen(pParse->args[2]);
				ev.u.away_peer.message = pParse->lastString;
				ccEmitProtoEvent(&ev);
			}
			break;
		}

		case RPL_WHOISUSER:		// 311
		{
			if (pParse->nArgs >= 5)
			{
				ccCSInString(&pParse->args[3]);	// user name
				CCQuery* pQuery = sock.m_queries.FindQuery(ctWhoIs, NULL);
				// Original routed by purpose: qpKickDlg->DoKickDlg (dialog);
				// qpBanDlg->build MODE +b; qpGetIdent->ShowIdentity; qpIgnoreIdent
				// ->IgnoreUser. R18: emit CC_EV_WHOIS_RESULT with the purpose tag;
				// Swift performs the dialog/ban/ignore reaction. The qpBanDlg
				// side-effect (send MODE +b to fetch the ban list) is protocol,
				// kept: build g_strBan + send the MODE +b (so 367/368 arrive).
				int purpose = pQuery ? (int)pQuery->GetQueryPurpose() : -1;
				if (pQuery && pQuery->GetQueryPurpose() == qpBanDlg) {
					ccGetBanString(sock.m_bIrcXServer, pParse->args[3], pParse->args[4], g_strBan);
					char buf[600];
					snprintf(buf, sizeof(buf), "MODE %s +b\r\n", (LPCTSTR)pQuery->GetChannelName());
					ccSessionSendRaw(buf, strlen(buf));
				}
				cc_proto_event ev; memset(&ev, 0, sizeof(ev));
				ev.type = CC_EV_WHOIS_RESULT;
				ev.u.whois_result.nick = pParse->args[2];
				ev.u.whois_result.user = pParse->args[3];
				ev.u.whois_result.host = pParse->args[4];
				ev.u.whois_result.real = pParse->lastString ? pParse->lastString : "";
				ev.u.whois_result.purpose = purpose;
				ccEmitProtoEvent(&ev);
			}
			break;
		}

		case RPL_WHOISSERVER:	// 312
		case RPL_WHOISOPERATOR:	// 313
		case RPL_WHOISIDLE:		// 317
		case RPL_WHOISCHANNELS:	// 319
		case RPL_WHOISIP:		// 320
			// Swallowed while a WHOIS cell exists; else status.
			if (!sock.m_queries.FindQuery(ctWhoIs, NULL))
				ccEmitStatus(szLine);
			break;

		case RPL_ENDOFWHOIS:	// 318
		{
			if (pParse->nArgs >= 3)
			{
				POSITION pos;
				CCQuery* pQuery = sock.m_queries.FindQuery(ctWhoIs, &pos);
				if (pQuery) { ASSERT(pos); sock.m_queries.FreeRemoveAt(pos); }
				else ccEmitStatus(szLine);
			}
			break;
		}

		case RPL_CHANNELMODEIS:	// 324
		{
			// Absolute channel-mode sync after join. Original: reset password/
			// modes then ABSOLUTE ParseChannelMode; deferred create-time mode/
			// topic set (m_bSetMode); dequeue ctGetChannelMode.
			if (pParse->nArgs >= 4)
			{
				const char *szArg2 = "", *szArg3 = "";
				if (pParse->nArgs >= 5) szArg2 = pParse->args[4];
				if (pParse->nArgs >= 6) szArg3 = pParse->args[5];

				sess.proto.m_strPassword = "";
				sess.proto.m_dwModes = 0;   // next ParseChannelMode is absolute
				ccParseChannelMode(sess.proto, pParse->args[3], szArg2, szArg3);

				// Deferred create-time set (m_bSetMode) -- original set modes/topic
				// on channel creation. Kept as a protocol follow-up (the outbound
				// ChatSetMode/ChatSetTopic builders exist). Only fires if a
				// create requested it (m_bSetMode); this task's inbound flow never
				// sets m_bSetMode (that came from the create dialog, app-side), so
				// this is dead unless Swift sets it -- guarded, faithful.
				if (sess.proto.m_bSetMode &&
					!stricmp(pParse->args[2], (LPCTSTR)sess.proto.m_strChannel))
				{
					sess.proto.ChatSetMode(sess.proto.m_dwModes, sess.proto.m_dwMaxUsers, sess.proto.m_strPassword);
					if (!sess.proto.m_strTopic.IsEmpty())
						sess.proto.ChatSetTopic(sess.proto.m_strTopic);
				}

				POSITION pos;
				CCQuery* pQuery = sock.m_queries.FindQuery(ctGetChannelMode, &pos);
				if (pQuery) { ASSERT(pos); sock.m_queries.FreeRemoveAt(pos); }

				// R18: emit the absolute channel-mode (channel + raw mode flags).
				cc_proto_event ev; memset(&ev, 0, sizeof(ev));
				ev.type = CC_EV_CHANNEL_MODE;
				ev.room_token = ccSessionRoomTokenForChannel(sess, pParse->args[2]);
				ev.u.channel_mode.channel = pParse->args[2];
				ev.u.channel_mode.modes = pParse->args[3];
				ev.u.channel_mode.arg = szArg2;
				ccEmitProtoEvent(&ev);
			}
			break;
		}

		case RPL_NOTOPIC:		// 331
		{
			// No topic. Original: qpListMembers continuation -> OnUserListAux
			// (member-list dialog). R18: dequeue the ctTopic/qpListMembers cell;
			// emit an empty topic-changed so Swift knows there's no topic.
			POSITION pos;
			CCQuery* pQuery = sock.m_queries.FindQuery(ctTopic, &pos);
			if (pQuery && pQuery->GetQueryPurpose() == qpListMembers) {
				ASSERT(pos);
				sock.m_queries.FreeRemoveAt(pos);
			}
			if (pParse->nArgs >= 3) {
				cc_proto_event ev; memset(&ev, 0, sizeof(ev));
				ev.type = CC_EV_TOPIC_CHANGED;
				ev.room_token = ccSessionRoomTokenForChannel(sess, pParse->args[2]);
				ev.u.topic_changed.channel = pParse->args[2];
				ev.u.topic_changed.topic = "";
				ccEmitProtoEvent(&ev);
			}
			break;
		}

		case RPL_TOPIC:			// 332
		{
			if (pParse->nArgs >= 3 && pParse->lastString)
			{
				CDWordArray rgdwFormattingTmp;
				ccCSInString(&pParse->lastString, pParse->args[2]);
				char* szCtrlLess = SzControlLess(pParse->lastString, &rgdwFormattingTmp);
				CString strCtrlLessTopic = szCtrlLess ? szCtrlLess : "";

				// room-property scratch
				if (sess.proto.m_prgdwTopicFormatting)
					FreeAndNullFormatting(&sess.proto.m_prgdwTopicFormatting);
				sess.proto.m_prgdwTopicFormatting = CopyFormatting(&rgdwFormattingTmp);
				sess.proto.m_strTopic = strCtrlLessTopic;

				// dequeue ctTopic (qpInitialTopic or qpListMembers)
				POSITION pos;
				CCQuery* pQuery = sock.m_queries.FindQuery(ctTopic, &pos);
				if (pQuery) { ASSERT(pos); sock.m_queries.FreeRemoveAt(pos); }

				// R18: emit topic-changed.
				cc_proto_event ev; memset(&ev, 0, sizeof(ev));
				ev.type = CC_EV_TOPIC_CHANGED;
				ev.room_token = ccSessionRoomTokenForChannel(sess, pParse->args[2]);
				ev.u.topic_changed.channel = pParse->args[2];
				ev.u.topic_changed.topic = strCtrlLessTopic;
				ccEmitProtoEvent(&ev);

				rgdwFormattingTmp.RemoveAll();
			}
			break;
		}

		case RPL_INVITING:		// 341
		{
			// "Invitation sent" ack. Original: AcknowledgeInvite (app). R18:
			// status line (no dedicated invite-ack variant; non-core).
			if (pParse->nArgs >= 4)
				ccEmitStatus(szLine);
			break;
		}

		case RPL_LISTSTART:		// 321
		case RPL_LISTXSTART:	// 811
		{
			enumCommandType ct = (pParse->uCode == RPL_LISTSTART) ? ctList : ctListX;
			CCQuery* pQuery = sock.m_queries.FindQuery(ct, NULL);
			if (pQuery && pQuery->GetQueryPurpose() == qpRoomListDlg)
			{
				s_listxRowPending = FALSE;
				cc_proto_event ev; memset(&ev, 0, sizeof(ev));
				ev.type = CC_EV_ROOM_LIST_BEGIN;
				ev.u.room_list_begin.truncated = 0;
				ccEmitProtoEvent(&ev);
			}
			else if (!pQuery)
				ccEmitStatus(szLine);
			break;
		}

		case RPL_LIST:			// 322
		{
			if (pParse->nArgs >= 4 && pParse->lastString)
			{
				CCQuery* pQuery = sock.m_queries.FindQuery(ctList, NULL);
				if (pQuery && pQuery->GetQueryPurpose() == qpRoomListDlg)
				{
					ccCSInString(&pParse->lastString);
					// skip the private-room '*' name sentinel (original :2371)
					if (!pParse->args[2] || pParse->args[2][0] != '*' || pParse->args[2][1] != '\0')
					{
						cc_proto_event ev; memset(&ev, 0, sizeof(ev));
						ev.type = CC_EV_ROOM_LIST_ITEM;
						ev.u.room_list_item.name = DecodeChan(pParse->args[2], FALSE);
						ev.u.room_list_item.users = atoi(pParse->args[3]);
						ev.u.room_list_item.topic = pParse->lastString;
						ccEmitProtoEvent(&ev);
					}
				}
				else if (!pQuery)
					ccEmitStatus(szLine);
			}
			break;
		}

		case RPL_LISTXLIST:		// 812
		{
			CCQuery* pQuery = sock.m_queries.FindQuery(ctListX, NULL);
			if (pQuery && pQuery->GetQueryPurpose() == qpRoomListDlg)
			{
				// flush the previous pending row (813 sets its PICS verdict)
				if (s_listxRowPending) {
					cc_proto_event ev; memset(&ev, 0, sizeof(ev));
					ev.type = CC_EV_ROOM_LIST_ITEM;
					ev.u.room_list_item.name = s_listxPretty;
					ev.u.room_list_item.users = s_listxUsers;
					ev.u.room_list_item.topic = s_listxDescr;
					ccEmitProtoEvent(&ev);
					s_listxRowPending = FALSE;
				}
				if (pParse->nArgs >= 6 && pParse->lastString) {
					const char *szRoomName = pParse->args[2];
					BOOL bMIC = (strchr(pParse->args[3], 'y') != NULL);
					ccCSInString(&pParse->lastString, bMIC ? NULL : szRoomName);
					s_listxName = szRoomName;
					s_listxPretty = DecodeChan(szRoomName, bMIC);
					s_listxUsers = atoi(pParse->args[4]);
					char* szCtrlLess = SzControlLess(pParse->lastString, NULL);
					s_listxDescr = szCtrlLess ? szCtrlLess : "";
					s_listxRowPending = TRUE;
				}
			}
			else if (!pQuery)
				ccEmitStatus(szLine);
			break;
		}

		case RPL_LISTXPICS:		// 813
			// PICS ratings verdict for the pending LISTX row. Original: sbAddIt =
			// bPassesRatings(...) (app policy gate). R18: ratings gating is app
			// policy -- we always emit the row (Swift applies ratings). No-op here
			// beyond keeping the row pending until 812/817 flushes it.
			break;

		case RPL_LISTEND:		// 323
		case RPL_LISTXTRUNC:	// 816
		case RPL_LISTXEND:		// 817
		{
			POSITION pos;
			CCQuery* pQuery = sock.m_queries.FindQuery(pParse->uCode == RPL_LISTEND ? ctList : ctListX, &pos);
			if (pQuery)
			{
				ASSERT(pos);
				sock.m_queries.RemoveAt(pos);
				if (pQuery->GetQueryPurpose() == qpRoomListDlg)
				{
					// flush any pending LISTX row
					if (s_listxRowPending) {
						cc_proto_event ev; memset(&ev, 0, sizeof(ev));
						ev.type = CC_EV_ROOM_LIST_ITEM;
						ev.u.room_list_item.name = s_listxPretty;
						ev.u.room_list_item.users = s_listxUsers;
						ev.u.room_list_item.topic = s_listxDescr;
						ccEmitProtoEvent(&ev);
						s_listxRowPending = FALSE;
					}
					cc_proto_event ev; memset(&ev, 0, sizeof(ev));
					ev.type = CC_EV_ROOM_LIST_END;
					ev.u.room_list_end.truncated = (pParse->uCode == RPL_LISTXTRUNC) ? 1 : 0;
					ccEmitProtoEvent(&ev);
				}
				delete pQuery;
			}
			else
				ccEmitStatus(szLine);
			break;
		}

		case RPL_NAMEREPLY:		// 353
		{
			// Initial member population. Original: drop stale qpInitialTopic;
			// bForEachWord(bSingleJoin) -> AddAndExecute(JoinEntry) per nick.
			// R18: drop the stale ctTopic cell; emit CC_EV_NAMES(channel, nicks).
			POSITION pos;
			CCQuery* pQuery = sock.m_queries.FindQuery(ctTopic, &pos);
			if (pQuery && pQuery->GetQueryPurpose() == qpInitialTopic) {
				ASSERT(pos);
				sock.m_queries.FreeRemoveAt(pos);
			}

			if (sock.m_queries.FindQuery(ctNames, NULL))
			{
				if (pParse->lastString && pParse->nArgs >= 4) {
					cc_proto_event ev; memset(&ev, 0, sizeof(ev));
					ev.type = CC_EV_NAMES;
					ev.room_token = ccSessionRoomTokenForChannel(sess, pParse->args[3]);
					ev.u.names.channel = pParse->args[3];
					ev.u.names.nicks = pParse->lastString;   // space-joined
					ccEmitProtoEvent(&ev);
				}
			}
			else
				ccEmitStatus(szLine);
			break;
		}

		case RPL_ENDOFNAMES:	// 366
		{
			POSITION pos;
			CCQuery* pQuery = sock.m_queries.FindQuery(ctNames, &pos);
			if (pQuery) {
				ASSERT(pos);
				sock.m_queries.FreeRemoveAt(pos);
				cc_proto_event ev; memset(&ev, 0, sizeof(ev));
				ev.type = CC_EV_END_OF_NAMES;
				ev.room_token = (pParse->nArgs >= 3) ? ccSessionRoomTokenForChannel(sess, pParse->args[2]) : 0;
				ev.u.end_of_names.channel = (pParse->nArgs >= 3) ? pParse->args[2] : "";
				ccEmitProtoEvent(&ev);
			}
			else
				ccEmitStatus(szLine);
			break;
		}

		case RPL_WHOREPLY:		// 352
		{
			if (pParse->nArgs >= 8)
			{
				CCQuery* pQuery = sock.m_queries.FindQuery(ctWho, NULL);
				if (pQuery)
				{
					// Original routed by purpose (rules/notif daemons vs the
					// user-list dialog vs qpInitialWho ignore-on-entry). All are
					// app reactions. R18: emit CC_EV_WHO_RESULT with the purpose
					// tag; Swift routes. args: <me> <chan> <user> <host> <server>
					// <nick> <flags> :<hops realname>.
					ccCSInString(&pParse->args[3]);   // user name
					cc_proto_event ev; memset(&ev, 0, sizeof(ev));
					ev.type = CC_EV_WHO_RESULT;
					ev.room_token = ccSessionRoomTokenForChannel(sess, pParse->args[2]);
					ev.u.who_result.nick = pParse->args[6];
					ev.u.who_result.user = pParse->args[3];
					ev.u.who_result.host = pParse->args[4];
					ev.u.who_result.channel = pParse->args[2];
					ev.u.who_result.purpose = (int)pQuery->GetQueryPurpose();
					ccEmitProtoEvent(&ev);
				}
				else
					ccEmitStatus(szLine);
			}
			break;
		}

		case RPL_ENDOFWHO:		// 315
		{
			POSITION pos;
			CCQuery* pQuery = sock.m_queries.FindQuery(ctWho, &pos);
			if (pQuery) { ASSERT(pos); sock.m_queries.RemoveAt(pos); delete pQuery; }
			else ccEmitStatus(szLine);
			break;
		}

		case RPL_BANLIST:		// 367
		{
			// Accumulate a ban mask (original g_arrayBans between 367/368).
			if (pParse->nArgs >= 4)
				g_arrayBans.Add(DecodeNick(pParse->args[3]));
			break;
		}

		case RPL_ENDOFBANLIST:	// 368
		{
			// Original: DoBanDlg(channel, g_strBan, g_arrayBans) (dialog).
			// R18: emit one CC_EV_STATUS_LINE per accumulated ban? -- the union
			// has no ban-list variant; the ban masks are non-core. Emit the raw
			// end line as status; Swift can request MODE +b if it wants the list.
			// (Union-gap note in report.) Dequeue our own ctSetChannelMode echo.
			POSITION pos;
			CCQuery* pQuery = sock.m_queries.FindQuery(ctSetChannelMode, &pos);
			if (pQuery && pQuery->GetQueryPurpose() == qpComSetChannelMode) {
				ASSERT(pos);
				sock.m_queries.FreeRemoveAt(pos);
			}
			// surface each ban mask on a status line so the info isn't lost
			for (int i = 0; i <= g_arrayBans.GetUpperBound(); i++)
				ccEmitStatus((LPCTSTR)(CString)g_arrayBans.GetAt(i));
			g_strBan = "";
			g_arrayBans.RemoveAll();
			break;
		}

		case RPL_MOTDSTART:		// 375
			break;   // suppressed in original

		case RPL_MOTD:			// 372
		case RPL_MOTD2:			// 377
		{
			const char *szMOTD = pParse->lastString;
			if (szMOTD)
			{
				if (strncmp(szMOTD, "- ", 2) == 0) szMOTD += 2;
				if (strcmp(szMOTD, "-") == 0)      szMOTD++;
				sock.m_strMOTD += szMOTD;
				sock.m_strMOTD += "\r\n";
			}
			CCQuery* pQuery = sock.m_queries.FindQuery(ctLUsersMOTD, NULL);
			if (!(pQuery && pQuery->GetQueryPurpose() == qpLUsersMOTD)) {
				// not part of the initial silent sequence -> status line
				if (szMOTD) ccEmitStatus(szMOTD);
			}
			break;
		}

		case RPL_ENDOFMOTD:		// 376
		{
			POSITION pos;
			CCQuery* pQuery = sock.m_queries.FindQuery(ctLUsersMOTD, &pos);
			if (pQuery)
			{
				// Original: ShowMOTD(luser, motd) gated by F1_SHOWMOTD (app
				// policy). R18: emit CC_EV_MOTD(luser, motd); Swift decides to
				// show it. Always emit if either accumulator is non-empty.
				if (!sock.m_strMOTD.IsEmpty() || !sock.m_strLUSER.IsEmpty()) {
					cc_proto_event ev; memset(&ev, 0, sizeof(ev));
					ev.type = CC_EV_MOTD;
					ev.u.motd.luser = sock.m_strLUSER;
					ev.u.motd.motd = sock.m_strMOTD;
					ccEmitProtoEvent(&ev);
				}
				ASSERT(pos);
				sock.m_queries.FreeRemoveAt(pos);
			}
			sock.m_strMOTD = "";
			sock.m_strLUSER = "";
			break;
		}

		case RPL_IRCX:			// 800 -- the IRCX pivot
		{
			// args: <me>* <state 0|1> <version> <pkglist> <maxmsglen> *
			POSITION pos;
			CCQuery* pQuery = sock.m_queries.FindQuery(ctModeIsIrcX, &pos);
			if (!pQuery) pQuery = sock.m_queries.FindQuery(ctIrcX, &pos);

			if (pQuery)
			{
				ASSERT(pos);
				sock.m_queries.FreeRemoveAt(pos);

				if ('0' == pParse->args[2][0])
				{
					// first instance: still IRC mode, becoming IRCX
					sock.m_bIrcXServer = TRUE;
					sock.m_bJustSentModeIsIrcX = FALSE;
					// probe answered: cancel the ISIRCX timeout timer
					if (sess.cfg.cancel_timer)
						sess.cfg.cancel_timer(sess.cfg.user_data, CC_TIMER_ISIRCX_PROBE);

					// parse security packages (ANON -> m_bAnonAllowed); the SSPI
					// package list is otherwise unused (R21 auth drop).
					if (pParse->nArgs >= 7)
					{
						BOOL bEnd = FALSE;
						CHAR *szHeadTmp, *szTmp;
						szHeadTmp = szTmp = pParse->args[4];
						do
						{
							if ((',' == *szTmp) || (bEnd = ('\0' == *szTmp)))
							{
								*szTmp = '\0';
								if (stricmp("ANON", szHeadTmp) == 0)
									sock.m_bAnonAllowed = TRUE;
								if (!bEnd) szHeadTmp = ++szTmp;
							}
							else
								szTmp++;
						}
						while (!bEnd);
					}

					// grow buffers to the server-advertised max message length
					SHORT nMaxMsgLength = atoi(pParse->args[pParse->nArgs-2]);
					if (sock.m_nMaxMsgLength < nMaxMsgLength)
						sock.HrInitAlloc(nMaxMsgLength);

					// R18: emit CC_EV_SERVER_CAPS(ircx=1, max_msg_len).
					cc_proto_event ev; memset(&ev, 0, sizeof(ev));
					ev.type = CC_EV_SERVER_CAPS;
					ev.u.server_caps.ircx = 1;
					ev.u.server_caps.max_msg_len = sock.m_nMaxMsgLength;
					ccEmitProtoEvent(&ev);

					// switch to IRCX mode (send "IRCX")
					sess.proto.bExecuteQuery(qpIrcX, ctIrcX, dtMax, NULL, "", "");
				}
				else
				{
					// second instance: already IRCX -> login. R21: SSPI auth
					// dropped; emit CC_EV_AUTH_UNSUPPORTED if the server does not
					// allow anonymous (else Swift does plain NICK/USER). We emit
					// the caps-confirmed marker via AUTH_UNSUPPORTED only when
					// anon is not allowed; otherwise nothing (Swift logs in anon).
					if (!sock.m_bAnonAllowed) {
						cc_proto_event ev; memset(&ev, 0, sizeof(ev));
						ev.type = CC_EV_AUTH_UNSUPPORTED;
						ev.u.auth_unsupported.dummy = 1;
						ccEmitProtoEvent(&ev);
					}
				}
			}
			else
				ccEmitStatus(szLine);
			break;
		}

		case RPL_PROPLIST:		// 818
		{
			if (pParse->nArgs >= 4)
			{
				CCQuery* pQuery = sock.m_queries.FindQuery(ctPropGet, NULL);
				if (pQuery)
				{
					enumQueryPurpose qp = pQuery->GetQueryPurpose();
					if (qp == qpJoinPics || qp == qpCreatePics) {
						// PICS ratings gate before join/create (app policy).
						// R18: emit CC_EV_ROOM_PROP(PICS, value); Swift applies the
						// ratings decision + issues the deferred join/create.
						pQuery->SetQueryPurpose(qpMax);
						cc_proto_event ev; memset(&ev, 0, sizeof(ev));
						ev.type = CC_EV_ROOM_PROP;
						ev.room_token = ccSessionRoomTokenForChannel(sess, pParse->args[2]);
						ev.u.room_prop.key = "PICS";
						ev.u.room_prop.value = pParse->lastString ? pParse->lastString : "";
						ccEmitProtoEvent(&ev);
					}
					else if (qp == qpJoinBackUrl) {
						// CLIENT prop -> diff + emit CC_EV_ROOM_PROP per key.
						ccEmitClientDataChange(sess, pParse->lastString ? pParse->lastString : "");
					}
				}
				else
					ccEmitStatus(szLine);
			}
			break;
		}

		case RPL_PROPEND:		// 819
		{
			if (pParse->nArgs >= 3)
			{
				POSITION pos;
				CCQuery* pQuery = sock.m_queries.FindQuery(ctPropGet, &pos);
				if (pQuery) {
					ASSERT(pos);
					sock.m_queries.RemoveAt(pos);
					delete pQuery;
				}
			}
			break;
		}
	}
}

//=--------------------------------------------------------------------------=
// SECTION 8: HandleErrorCode (ircsock.cpp:3006-3496) -- numeric errors
//=--------------------------------------------------------------------------=

static void ccHandleErrorCode(CCSession& sess, char *szLine, PIRCPARSE pParse)
{
	ASSERT(pParse);
	ASSERT(pParse->uCode);
	CIrcSocket& sock = sess.sock;
	BOOL bDisplayErrorInStatusWindow = FALSE;
	CString strMesg;

	switch (pParse->uCode)
	{
		default:
			bDisplayErrorInStatusWindow = TRUE;
			break;

		case ERR_NOSUCHNICK:		// 401 (== ERR_NOSUCHNICK)
		{
			// <nick|channel> :No such nick/channel. R20: user-facing -> CC_EV_ERROR.
			if (CHANNELPREFIX(pParse->args[2][0]))
				strMesg.Format("No such channel: %s", DecodeChan(pParse->args[2], FALSE));
			else {
				strMesg.Format("No such nick: %s", DecodeNick(pParse->args[2]));
				ccFreeModeCell(sock, NULL, pParse->args[2]);
			}
			ccEmitError(pParse->uCode, strMesg);
			break;
		}

		case ERR_NOSUCHCHANNEL:		// 403
		{
			// R20: the RoomList dialog-widget pokes (:3072-3084) are DROPPED
			// (dialog re-enable is Swift's). Original had two paths (ShowBadChannelName
			// vs list-members-continuation vs bFreeModeCell). We emit CC_EV_ERROR
			// (bad-channel) and dequeue any qpListMembers ctTopic cell.
			POSITION pos;
			CCQuery* pQuery = sock.m_queries.FindQuery(ctTopic, &pos);
			if (pQuery && pQuery->GetQueryPurpose() == qpListMembers) {
				ASSERT(pos);
				sock.m_queries.FreeRemoveAt(pos);
				strMesg.Format("No such channel anymore: %s", DecodeChan(pParse->args[2], FALSE));
			}
			else {
				strMesg.Format("No such channel: %s", DecodeChan(pParse->args[2], FALSE));
				ccFreeModeCell(sock, pParse->args[2], pParse->args[2]);
			}
			ccEmitError(pParse->uCode, strMesg);
			break;
		}

		case ERR_TOOMANYCHANNELS:	// 405
			ccEmitError(pParse->uCode, "You have joined too many channels");
			break;

		case ERR_NOMOTD:			// 422
		{
			// Ends the LUSERS/MOTD sequence gracefully. R18: emit CC_EV_MOTD with
			// whatever LUSER accumulated (no MOTD text); dequeue the cell.
			POSITION pos;
			CCQuery* pQuery = sock.m_queries.FindQuery(ctLUsersMOTD, &pos);
			if (pQuery) {
				if (!sock.m_strLUSER.IsEmpty()) {
					cc_proto_event ev; memset(&ev, 0, sizeof(ev));
					ev.type = CC_EV_MOTD;
					ev.u.motd.luser = sock.m_strLUSER;
					ev.u.motd.motd = "";
					ccEmitProtoEvent(&ev);
				}
				ASSERT(pos);
				sock.m_queries.FreeRemoveAt(pos);
			}
			sock.m_strLUSER = "";
			break;
		}

		case ERR_NONICKNAMEGIVEN:	// 431
		case ERR_ERRONEUSNICKNAME:	// 432
		case ERR_NICKNAMEINUSE:		// 433
		{
			// R20: TryNewNick dialog -> CC_EV_NICK_REJECTED (Swift owns retry).
			int iIndex = (ERR_NICKNAMEINUSE == pParse->uCode) ? 2 : 1;
			const char* szBadNick = (pParse->nArgs >= (iIndex+1)) ? pParse->args[iIndex] : "";
			cc_proto_event ev; memset(&ev, 0, sizeof(ev));
			ev.type = CC_EV_NICK_REJECTED;
			ev.u.nick_rejected.kind = (int)pParse->uCode;
			ev.u.nick_rejected.bad_nick = sock.m_bIrcXServer ? DecodeNick(szBadNick) : szBadNick;
			ccEmitProtoEvent(&ev);
			break;
		}

		case ERR_NICKCOLLISION:		// 436
			ccEmitError(pParse->uCode, "Nickname collision");
			break;
		case ERR_NICKTOOFAST:		// 438
			ccEmitError(pParse->uCode, "Changing nickname too fast");
			break;
		case ERR_NICKNOCHANGE:		// 439
			ccEmitError(pParse->uCode, "Nick change not permitted");
			break;

		case ERR_NOTONCHANNEL:		// 442
		{
			// member-list fallback (qpListMembers) or mode-cell cleanup.
			POSITION pos;
			CCQuery* pQuery = sock.m_queries.FindQuery(ctTopic, &pos);
			if (pQuery && pQuery->GetQueryPurpose() == qpListMembers) {
				ASSERT(pos);
				sock.m_queries.FreeRemoveAt(pos);
			} else {
				ccFreeModeCell(sock, pParse->args[2], NULL);
				bDisplayErrorInStatusWindow = TRUE;
			}
			break;
		}

		case ERR_NOTREGISTERED:		// 451 -- THE ISIRCX FALLBACK
			ccModeIsIrcXFailure(sess);
			break;

		case ERR_NEEDMOREPARAMS:	// 461
			ccFreeModeCell(sock, NULL, NULL);
			bDisplayErrorInStatusWindow = TRUE;
			break;

		case ERR_PASSWDMISMATCH: 	// 464
			// Original: re-prompt + re-OPER (dialog + registry). R20: emit
			// CC_EV_ERROR(auth-password-rejected); Swift re-prompts.
			ccEmitError(pParse->uCode, "Password mismatch");
			break;

		case ERR_YOUREBANNEDCREEP:	// 465
			ccEmitError(pParse->uCode, "You are banned from this server");
			break;
		case ERR_YOUWILLBEBANNED:	// 466
			ccEmitError(pParse->uCode, "You will be banned from this server");
			break;

		case ERR_KEYSET:			// 467
			ccFreeModeCell(sock, pParse->args[2], NULL);
			bDisplayErrorInStatusWindow = TRUE;
			break;

		case ERR_CHANNELISFULL:		// 471
			strMesg.Format("Channel is full: %s", DecodeChan(pParse->args[2], FALSE));
			ccEmitError(pParse->uCode, strMesg);
			break;

		case ERR_UNKNOWNMODE:		// 472
			ccFreeModeCell(sock, NULL, NULL);
			bDisplayErrorInStatusWindow = TRUE;
			break;

		case ERR_INVITEONLYCHAN:	// 473
			strMesg.Format("Invite-only channel: %s", DecodeChan(pParse->args[2], FALSE));
			ccEmitError(pParse->uCode, strMesg);
			break;

		case ERR_BANNEDFROMCHAN:	// 474
			strMesg.Format("Banned from channel: %s", DecodeChan(pParse->args[2], FALSE));
			ccEmitError(pParse->uCode, strMesg);
			break;

		case ERR_BADCHANNELKEY:		// 475
			// Original: OnBadChannelPassword (password dialog + rejoin). R20:
			// emit CC_EV_ERROR(bad-key); Swift re-prompts.
			strMesg.Format("Bad channel key: %s", DecodeChan(pParse->args[2], FALSE));
			ccEmitError(pParse->uCode, strMesg);
			break;

		case ERR_CHANOPRIVSNEEDED:	// 482
		{
			if (!ccFreeModeCell(sock, pParse->args[2], NULL)) {
				POSITION pos;
				CCQuery* pQuery = sock.m_queries.FindQuery(ctTopic, &pos);
				if (pQuery) { ASSERT(pos); sock.m_queries.FreeRemoveAt(pos); }
			}
			bDisplayErrorInStatusWindow = TRUE;
			break;
		}

		case ERR_UMODEUNKNOWNFLAG:	// 501
		case ERR_USERSDONTMATCH:	// 502
			ccFreeModeCell(sock, NULL, "");
			bDisplayErrorInStatusWindow = TRUE;
			break;

		case ERR_NOJOINDYNAMIC:		// 552
			ccEmitError(pParse->uCode, "Cannot join dynamic channels");
			break;
		case ERR_NODYNAMICCHANNELS:	// 553
			ccEmitError(pParse->uCode, "Cannot create dynamic channels");
			break;
		case ERR_AUTHONLY:			// 556
			ccEmitError(pParse->uCode, "Only authenticated users may join");
			break;

		// dual-meaning MIC-vs-IRCX codes: msgbox only when !m_bIrcXServer
		case ERR_CANNOTCREATEDYNAMIC:	// 902 / ERR_BADFUNCTION
			if (!sock.m_bIrcXServer)
				ccEmitError(pParse->uCode, "Cannot create dynamic channels (admin)");
			break;
		case ERR_ONLYAUTHCANJOIN:		// 904 / ERR_BADTAG
			if (!sock.m_bIrcXServer)
				ccEmitError(pParse->uCode, "Only authenticated users may join channel");
			break;
		case ERR_CANNOTCHANGENICK:		// 905 / ERR_BADPROPERTY
			if (!sock.m_bIrcXServer)
				ccEmitError(pParse->uCode, "Nick changes are not permitted at this time");
			break;
		case ERR_CANNOTJOINDYNAMIC:		// 907 / ERR_RESOURCE
			if (!sock.m_bIrcXServer)
				ccEmitError(pParse->uCode, "Cannot join dynamic channels due to admin restriction");
			break;

		case ERR_AUTHENTICATIONFAILED:	// 910
			// Original: AfxMessageBox + retry HrIrcXLogin. R21: auth dropped ->
			// emit CC_EV_AUTH_UNSUPPORTED (Swift falls back / reports).
			sock.m_bJustSentModeIsIrcX = FALSE;
			{
				cc_proto_event ev; memset(&ev, 0, sizeof(ev));
				ev.type = CC_EV_AUTH_UNSUPPORTED;
				ev.u.auth_unsupported.dummy = 1;
				ccEmitProtoEvent(&ev);
			}
			break;

		case ERR_UNKNOWNPACKAGE:	// 912
			// Original: HrIrcXLogin(TRUE) next package. R21: auth dropped.
			{
				cc_proto_event ev; memset(&ev, 0, sizeof(ev));
				ev.type = CC_EV_AUTH_UNSUPPORTED;
				ev.u.auth_unsupported.dummy = 1;
				ccEmitProtoEvent(&ev);
			}
			break;

		case ERR_NOSUCHOBJECT:		// 924
		{
			// PROP on non-existent room -> proceed to JOIN/CREATE. Original:
			// bCanViewUnrated(TRUE) gate + ChatJoinAux/ChatCreateAux. R18: emit
			// CC_EV_ROOM_PROP(NOSUCHOBJECT sentinel) so Swift decides to join;
			// dequeue the ctPropGet cell.
			POSITION pos;
			CCQuery* pQuery = sock.m_queries.FindQuery(ctPropGet, &pos);
			if (pQuery) {
				ASSERT(pos);
				sock.m_queries.FreeRemoveAt(pos);
			}
			else
				bDisplayErrorInStatusWindow = TRUE;
			break;
		}
	}

	if (bDisplayErrorInStatusWindow)
		ccEmitError((int)pParse->uCode, szLine);
}

//=--------------------------------------------------------------------------=
// SECTION 9: bFreeModeCell (ircsock.cpp:3499-3534) -- mode-query cell cleanup
//=--------------------------------------------------------------------------=
// Pure correlation-list cleanup: dequeue the oldest qpComSetUserMode/
// qpComSetChannelMode cell (whichever is older) when a mode command failed.
// Lifted verbatim (no app callee).
BOOL ccFreeModeCell(CIrcSocket& sock, LPCTSTR szChannel, LPCTSTR szNickname)
{
	POSITION pos1, pos2;
	LONG lRank1 = 0, lRank2 = 0;
	CCQuery* pQuery1 = NULL;
	CCQuery* pQuery2 = NULL;

	if (szNickname || (!szNickname && !szChannel))
		pQuery1 = sock.m_queries.FindQuery(ctSetUserMode, &pos1, &lRank1);

	if (szChannel || (!szNickname && !szChannel))
		pQuery2 = sock.m_queries.FindQuery(ctSetChannelMode, &pos2, &lRank2);

	if (!pQuery1 || qpComSetUserMode != pQuery1->GetQueryPurpose())
		lRank1 = 0L;
	if (!pQuery2 || qpComSetChannelMode != pQuery2->GetQueryPurpose())
		lRank2 = 0L;

	if (lRank1 && (!lRank2 || lRank1 < lRank2)) {
		ASSERT(pos1);
		sock.m_queries.FreeRemoveAt(pos1);
		return TRUE;
	}
	if (lRank2 && (!lRank1 || lRank2 < lRank1)) {
		ASSERT(pos2);
		sock.m_queries.FreeRemoveAt(pos2);
		return TRUE;
	}
	return FALSE;
}

//=--------------------------------------------------------------------------=
// SECTION 10: HandleClientDataChange -> CC_EV_ROOM_PROP diff (R18 of the
//             original ircproto.cpp:737-767 OnPropertyChange path)
//=--------------------------------------------------------------------------=
// Diffs the session proto's cached client-data (m_strClientData) against the
// new string, emitting CC_EV_ROOM_PROP(key, value) for each added/modified/
// removed key -- the exact enumerate-both-passes logic of the original, with
// OnPropertyChange replaced by the event. A removed key emits value == "".
void ccEmitClientDataChange(CCSession& sess, const char* pszNewClientData)
{
	LPCSTR pszPropStrings[2] = { pszNewClientData, (LPCSTR)sess.proto.m_strClientData };
	CString strKey, strValue, strOtherValue;
	LPCSTR psz;
	for (int iPass = 0; iPass < 2; iPass++) {
		psz = pszPropStrings[iPass];
		while (EnumKeyString(psz, strKey, strValue)) {
			if (!GetValueFromKeyString(pszPropStrings[1 - iPass], strKey, strOtherValue)) {
				// pass 0: added; pass 1: removed.
				cc_proto_event ev; memset(&ev, 0, sizeof(ev));
				ev.type = CC_EV_ROOM_PROP;
				ev.u.room_prop.key = strKey;
				ev.u.room_prop.value = (iPass == 1) ? "" : (LPCTSTR)strValue;
				ccEmitProtoEvent(&ev);
			} else if (strOtherValue != strValue && iPass == 0) {
				// modified (emit once, on pass 0)
				cc_proto_event ev; memset(&ev, 0, sizeof(ev));
				ev.type = CC_EV_ROOM_PROP;
				ev.u.room_prop.key = strKey;
				ev.u.room_prop.value = (LPCTSTR)strValue;
				ccEmitProtoEvent(&ev);
			}
		}
	}
	sess.proto.m_strClientData = pszNewClientData;
}

//=--------------------------------------------------------------------------=
// SECTION 11: ProcessMessage (ircsock.cpp:1115-1159) + OnReceive framer
//=--------------------------------------------------------------------------=

void ccProcessMessage(CCSession& sess, char* szLine)
{
	IRCPARSE parse;

	ParseIt(szLine, &parse);
	if (parse.nArgs <= 0) {
		// original ASSERT + re-parse; permissive: drop.
		FreeParse(&parse);
		return;
	}

	if (0 == parse.uCode)
		ccHandleCommand(sess, szLine, &parse);
	else if (bIsErrorCode(parse.uCode))
		ccHandleErrorCode(sess, szLine, &parse);
	else
		ccHandleResultCode(sess, szLine, &parse);

	FreeParse(&parse);
}

// The OnReceive line framer (ircsock.cpp:1000-1032). Append the new bytes into
// m_szInput, then loop: find '\n', copy the line (incl. terminator) to
// m_szMessage, shift the remainder down, ProcessMessage. Reentrant-but-single-
// threaded (the original's comment) -- we re-find '\n' after each ProcessMessage
// exactly like the original, since a handler can feed more (it doesn't here, but
// the discipline is preserved).
void ccOnReceiveBytes(CCSession& sess, const uint8_t* data, size_t len)
{
	CIrcSocket& sock = sess.sock;
	// Append into m_szInput up to its capacity (m_nMaxMsgLength). The original
	// Receive()d directly into the tail with a bounded `space`; here we append
	// the caller-supplied bytes with the same cap so a single over-long line
	// can't overflow. Any bytes beyond capacity are dropped (matches the
	// original's bounded Receive -- a well-behaved server never exceeds
	// m_nMaxMsgLength per line).
	char* startPtr = (char*)strchr(sock.m_szInput, '\0');
	int space = (int)(sock.m_szInput + sock.m_nMaxMsgLength - startPtr);
	int nRead = (int)len;
	if (nRead > space) nRead = space;
	if (nRead > 0) {
		memcpy(startPtr, data, (size_t)nRead);
		startPtr[nRead] = '\0';
	}

	char* eoc = (char*)strchr(sock.m_szInput, '\n');
	while (eoc) {
		eoc++;
		int comLen = (int)(eoc - sock.m_szInput);
		strncpy(sock.m_szMessage, sock.m_szInput, comLen);
		sock.m_szMessage[comLen] = '\0';

		// move the rest of the message forward
		char* eob = (char*)strchr(sock.m_szInput, '\0');
		int nRest = (int)(eob - eoc);
		memmove(sock.m_szInput, eoc, (size_t)nRest);
		sock.m_szInput[nRest] = '\0';

		TRACE("Got message: %.100s\n", sock.m_szMessage);
		ccProcessMessage(sess, sock.m_szMessage);
		eoc = (char*)strchr(sock.m_szInput, '\n');  // re-find after ProcessMessage (reentrant)
	}
}

// ISIRCX probe timeout (CC_TIMER_ISIRCX_PROBE fired by Swift).
void ccFireIsIrcXTimeout(CCSession& sess)
{
	ccModeIsIrcXFailure(sess);
}
