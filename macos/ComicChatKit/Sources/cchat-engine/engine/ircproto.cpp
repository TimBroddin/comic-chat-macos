// ircproto.cpp — PARTIAL LIFT from v2.5-beta-1-modern/ircproto.cpp (1458
// lines). Plan 3 Task 4: OUTBOUND command builders ONLY. See ircproto.h for
// the CIrcProto/CIrcSocket structural deviation note (no CRoomInfo base, no
// full ircsock.h CIrcSocket).
//
// FUNCTIONS LIFTED (outbound builders + their direct helpers), each with its
// disposition/deviation noted at the definition below:
//   SendMessageText, bChatSendToTarget, bChatSendPrivMesg, bChatSendToChannel,
//   ChatSetTopic, ChatSetClientData, ChatPartChannel, ChatJoinAux,
//   ChatCreateAux, ChatKickUser(nickname,reason), ChatBanUser(pattern,bBan),
//   ChatSendInvitation, ChatChangeNick, ChatSetAway (fan-out dropped, see
//   below), ChatSetMode, bRegisterMode, bExecuteQuery, EncodingType,
//   EncodeString, StrEncodeCommandParam, EncodeNick, DecodeNick,
//   DecodeNickForScreen, DecodeChan, EncodeChan, DecodeString, GetModeChars,
//   nGetBreakingPoint.
//
// FUNCTIONS/SITES NOT LIFTED (the 17 UI-ish sites from
// docs/superpowers/plans/2026-07-18-plan3-discovery/ircproto-map.md §6, EVERY
// one listed with its disposition; line numbers below refer to the ORIGINAL
// v2.5-beta-1-modern/ircproto.cpp):
//   1. CommunicationInits/Cleanup (:47-60,63-71), CB32/NetMeeting block
//      (`#ifdef CB32SUPPORT`) -- not lifted: nmproto.h excluded per the
//      original's own #ifdef; this port has no CB32 backend at all.
//   2. CommunicationInits/Cleanup, cui.m_pvIrcProto set/clear -- R17: the
//      "current default CIrcProto" singleton is engine-context/session
//      territory (Task 7's CCSession owns proto directly instead).
//   3. CommunicationInits, AfxSocketInit()+AfxMessageBox(IDP_SOCKETS_INIT_FAILED)
//      -- not lifted: Swift owns the socket (spec §4.1), there is no
//      WinSock-style global init step to port at all.
//   4. FixMICChannelName (:74-83) -- not lifted: doc->SetLegalPath/
//      ChatSetChannel/SetConnectionStatus are app/doc reactions to a
//      successful join (EVT `channel-renamed` per discovery disposition),
//      not an outbound command.
//   5. ChatFillRoomList (:101-133) -- R11/R20 whole-function drop: reads
//      `prl->m_persist->m_strQuery` (a live dialog's persisted state). Its
//      core (decide ctList vs ctListX, call bExecuteQuery) resurfaces as
//      cc_session_list() in cc_session.cpp, taking the query string as a
//      plain parameter instead of reading it off a dialog object.
//   6. ChatFillUserList (:136-184) -- R11/R20 whole-function drop: the two
//      live GetWindowText reads (:157 pul->m_user, :176 pul->m_ctlRoom) are
//      genuine UI. Core resurfaces as cc_session_who() (mask parameter
//      instead of a dialog read).
//   7. GetMyIP (:187-195) -- R20/CC_NO_UI: DCC-only caller (filesend.cpp),
//      deferred; not reachable from any outbound builder this task lifts.
//   8. EncodeNick/DecodeNick (:204-248) -- LIFTED (theApp.m_wszBuffer/
//      m_nBufferSize scratch -> local stack buffers + ccommon_str.cpp's
//      already-lifted bConvertWideStringToUTF8/bConvertUTF8StringToWide, see
//      below for the exact R19 rewrite).
//   9. DecodeNickForScreen (:250-296) -- LIFTED (display-only helper; no
//      session dependency once GetStringTypeEx/CharNext resolve through
//      mfc_compat.h, same as the original's own `#if 0`/`#else` split --
//      preserved verbatim, dead `#if 0` arm included per R4).
//   10. DecodeChan/EncodeChan/DecodeString (:300-395) -- LIFTED
//      (theApp.m_szBuffer/m_charSet/m_nBufferSize -> session-held scratch,
//      see CIrcProto-external static scratch buffer below; CP-1252 posture
//      per Task 2 makes the DBCS `ConvertEncodingIn/Out` arms live but
//      trivial no-ops on this port, kept per R4).
//   11. bChatSendToTarget's GetMyNickName()/GetMyUserName()/
//      theApp.m_nMyIdentLength (:527-533) -- R19: hoisted to an explicit
//      PFNGETOWNIDENTITY resolver parameter (own nick/ident are session
//      identity state, discovery §9's "deliberately not mapped here" list).
//   12. ChatPartChannel's rules-engine fan-out
//      (theApp.m_dynaRules.bMatchAndApplyRules(eOnLeave...), :781) --
//      dropped: the rules/notification daemon is app automation entirely out
//      of the protocol lift's scope (same posture as query.h's CCRule/
//      CCNotif). The PART wire command itself is lifted; the rules
//      side-effect is not.
//   13. ChatSetAway's g_docs fan-out (:930-937) -- dropped (R20): iterating
//      every OTHER open room's CIrcProto and echoing the away state into it
//      is app/session-doc-table state this task's CIrcProto (one instance,
//      no session-wide room registry) cannot reach. The AWAY wire command
//      itself is lifted (this room's CIrcProto only); Task 6/7 (once the
//      session owns a real room table) can restore the fan-out if needed.
//   14. ChatGetIdentity (:980-1001) -- not lifted: its non-cached branch is
//      the outbound WHOIS-for-identity query (arguably a builder), but its
//      cached branch calls ShowIdentity (protsupp.cpp:3641, an INBOUND
//      history-log reaction) and the whole function is invoked exclusively
//      from inbound-triggered UI (kick/ban dialogs), never from any of this
//      task's outbound C entry points. Deferred to Task 6/7 alongside the
//      rest of the CTCP-reply layer.
//   15. DoIgnoreUser (:1228-1252) -- not lifted: IgnoreUser (protsupp.cpp:2134)
//      is an inbound-triggered ignore-list + member-list-UI mutation, not an
//      outbound wire command in its own right (its ONE outbound path,
//      bExecuteQuery(qpIgnoreIdent, ctWhoIs, ...), is reachable but the
//      function's other branch is not outbound at all -- deferred whole,
//      Task 6/7).
//   16. StrEncodeCommandParam's LookupDoc(szEncodedChannelName) (:1322) --
//      R19: hoisted to an explicit PFNISJOINEDCHANNEL predicate parameter
//      ("is there a room joined under this encoded channel name" -- session
//      room-table state, not codec logic).
//   17. CIdentdSocket + StartIdentD/StopIdentD (:1406-1457) -- R21 DROP (per
//      the brief's explicit instruction): the identd server is a whole
//      embedded CAsyncSocket LISTENER on port 113, entirely orthogonal to
//      the outbound-command surface this task lifts, and the engine never
//      opens sockets (spec §4.1). Not ported; StopIdentD's call site
//      (CIrcProto::OnLogin, ircsock.cpp:1079, itself not lifted this task)
//      becomes a no-op by simply not existing. No `#ifndef CC_NO_UI` guard
//      needed since the whole class/functions are simply absent from this
//      file (a stronger drop than a compile-time gate).
//
// ADDITIONAL NOT-LIFTED (outbound-adjacent, but their only caller is
// inbound/app-policy, so lifting the callee would leave dead code):
//   - HandleClientDataChange (:737-767) -- called ONLY from the inbound PROP
//     handler (ircsock.cpp, Task 5b), never from any outbound builder.
//   - ChangeProperty (:1389-1404) -- its only caller (ChatSyncBackDrop,
//     protsupp.cpp:3451) is an app-side reaction to an INBOUND backdrop-sync
//     event, not an outbound entry point. ChatSetClientData underneath it
//     (the actual PROP-CLIENT wire builder) IS lifted since it is otherwise
//     a clean, self-contained bExecuteQuery call.
//   - ChatKickUser(CUserInfo*)/ChatBanUser(CUserInfo*) two-pointer overloads
//     (:1004-1019) and ChatGetIdentity -- these dispatch off a CUserInfo*
//     (session member-table lookup), not a plain string; the string-based
//     overloads (ChatKickUser(nick,reason), ChatBanUser(pattern,bBan)) that
//     do the actual wire building ARE lifted.
//
// Edit Rules applied (mechanical, across every lifted hunk):
//   R1  - #include "stdafx.h" + afxsock.h/resource.h/ui.h/chat.h/etc ->
//         mfc_compat.h/ircproto.h/query.h/ccommon_str.h/protsupp.h/format.h.
//   R2  - theApp.* reads for pure identity/scratch state -> R19 resolver
//         parameters or local scratch (see per-function notes below).
//   R8  - CDocument*/CChatDoc*/CUserInfo* dependent overloads dropped (see
//         "ADDITIONAL NOT-LIFTED" above); no UI/doc header included.
//   R13 - ASSERT(cond, "msg") -> ASSERT(cond) (same rewrite as every prior
//         Task 2/3 lift).
//   R19 - GetMyNickName/GetMyUserName/theApp.m_nMyIdentLength (own identity),
//         theApp.m_wszBuffer/m_szBuffer/m_nBufferSize/m_charSet (scratch) ->
//         explicit parameters or file-local scratch (see below).
//   R20 - ChatFillRoomList/ChatFillUserList/GetMyIP: whole-function drop,
//         core logic resurfaces as cc_session_list/cc_session_who
//         (cc_session.cpp), calling bExecuteQuery directly.
//   R21 - CIdentdSocket/StartIdentD/StopIdentD: whole drop (see site #17).
#include "mfc_compat.h"      // R1 (was stdafx.h)
#include "defines.h"         // SM_*/BM_*/CHANNELPREFIX/CM_*/MAX_TOKEN/my_isspace
#include "ircproto.h"
#include "protsupp.h"        // GetToken (StrEncodeCommandParam not lifted, but shares protsupp.h)
#include "ccommon_str.h"     // bLowLevelQuoting, bExtendedNickname, UTF-8 codec (Task 2/4)
#include "format.h"          // chCtl*/SzSkipOneFormat/nFillFormatting (formatting-aware chunk breaks)

CIrcSocket serverConn;

// --- R19 scratch (was theApp.m_szBuffer/m_wszBuffer/m_nBufferSize) ----------
// EncodeNick/DecodeNick/EncodeChan/DecodeChan/DecodeString/EncodeString all
// return a pointer into a scratch buffer the ORIGINAL owned on theApp (shared
// process-wide, single-threaded by MFC message-pump convention). This port's
// engine has the same single-threaded-per-session contract (comicchat.h's
// THREADING CONTRACT note), so a file-local static scratch buffer is the
// direct, behavior-preserving equivalent -- same "valid until next call"
// contract the original had, just not routed through theApp. Sized well
// above any wire-legal channel/nick/message length (MAX_INPUTLEN=350,
// MAX_TOKEN=201) with generous headroom for UTF-8 expansion.
#define CC_SCRATCH_BUFFER_SIZE 4096
static char   g_szScratchBuffer[CC_SCRATCH_BUFFER_SIZE];
static WCHAR  g_wszScratchBuffer[CC_SCRATCH_BUFFER_SIZE];
static const int g_nScratchBufferSize = CC_SCRATCH_BUFFER_SIZE;

// theApp.m_charSet: this port's fixed CP-1252/ANSI_CHARSET posture (Task 2's
// permanent decision, mfc_compat.h's CharUpperBuff note). DecodeChan/
// EncodeChan/DecodeString/EncodeString all branch on
// `theApp.m_charSet == ANSI_CHARSET` purely to fast-path the DBCS arm; that
// branch is always taken on this port, so the constant stands in directly
// rather than inventing a new session field for a value that never varies.
static const int g_iCharSet = ANSI_CHARSET;

// theApp.m_nMyIdentLength / GetMyNickName() / GetMyUserName(): own identity
// state (discovery §9's "deliberately not mapped here" list -- sibling
// territory). bChatSendToTarget needs these purely to compute the RECEIVING
// side's prefix length so it doesn't cut a message mid-prefix; hoisted to an
// explicit PFNGETOWNIDENTITY resolver (R19) rather than invented as new
// session state in this task.

// ConvertEncodingIn/ConvertEncodingOut (intl.c, not lifted): the DBCS arms of
// DecodeChan/EncodeChan/DecodeString/EncodeString that call these are ALWAYS
// preceded by `if (g_iCharSet == ANSI_CHARSET) return ...;`, and g_iCharSet
// is permanently ANSI_CHARSET on this port (Task 2's fixed CP-1252 posture) --
// so every call site reaching these two functions is provably dead at
// runtime, matching the original's own DBCS-vs-ANSI branch structure (these
// were live only under a non-ANSI/DBCS codepage there too). They still need
// to LINK (the call sites remain in the compiled object even though
// unreachable), so trivial ASSERT(0) stubs stand in -- same posture as
// mfc_compat.h's LCMapString stub for the same reason (Capitalize's dead
// GREEK/RUSSIAN/TURKISH branches). A real Far-East/DBCS build would need to
// implement these for real, at which point this comment is stale.
static BOOL ConvertEncodingIn(LPSTR *) { ASSERT(0); return FALSE; }
static BOOL ConvertEncodingOut(LPSTR *) { ASSERT(0); return FALSE; }


// --- charset codec (verbatim except R19 scratch, ircproto.cpp:204-395) -----

const char *EncodeNick(const char *szNick, BOOL bEscapeWildcards)
{
	static char	szEncoded[128];
	char*		szUtf8 = NULL;
	int			pCChOut, a;

	if (!(a = MultiByteToWideChar(GetACP(), MB_PRECOMPOSED, szNick, -1, g_wszScratchBuffer, g_nScratchBufferSize)))
		goto error;
	if (!bConvertWideStringToUTF8(g_wszScratchBuffer, 0, &szUtf8, &pCChOut, TRUE, FALSE, TRUE, bEscapeWildcards))
		goto error;
	if (!szUtf8)
		goto error;
	strcpy(szEncoded, szUtf8);
	delete [] szUtf8;
	return szEncoded;

error:
	ASSERT(0);
	strcpy(szEncoded, szNick);
	return szEncoded;
}


const char *DecodeNick(const char *szNick)
{
	static char	szEncoded[128];
	LPWSTR		wszBuff;
	int			pCChOut, a;

	if (*szNick != '\'')
		return szNick;
	if (!bConvertUTF8StringToWide(szNick, 0, &wszBuff, &pCChOut, TRUE, FALSE, TRUE))
		goto error;
	if (!wszBuff)
		goto error;
	if (!(a = WideCharToMultiByte(GetACP(), 0, wszBuff, -1, szEncoded, sizeof(szEncoded), NULL, NULL)))
		goto error;
	delete [] wszBuff;
	return szEncoded;

error:
	ASSERT(0);
	strcpy(szEncoded, szNick);
	return szEncoded;
}

const char *DecodeNickForScreen(const char *szNick)
{
	LPCSTR pszDecodedNick = DecodeNick (szNick);
	ASSERT(lstrlen (pszDecodedNick) < 128);
	WORD wTypeInfo[128];
	static char szBufOut[130];
	if (!GetStringTypeEx (GetUserDefaultLCID (), CT_CTYPE1, pszDecodedNick, lstrlen (pszDecodedNick), wTypeInfo))
		return pszDecodedNick;
   #if 0
	LPCSTR pszSrc = pszDecodedNick;
	LPSTR pszDest = szBufOut;
	LPWORD pwTypeInfo = wTypeInfo;
	while (*pszSrc)
	{
		if (*pwTypeInfo & (C1_CNTRL | C1_BLANK))
		{
			*pszDest = '_';
		}
		else
		{
			*pszDest = *pszSrc;
			if (IsDBCSLeadByte (*pszSrc))
				*(++pszDest) = *(++pszSrc);
		}
		pszSrc++;
		pszDest++;
		pwTypeInfo++;
	}
	*pszDest = '\0';
	return szBufOut;
   #else
   	LPCSTR pszSrc;
	LPWORD pwTypeInfo;
	for (pszSrc = pszDecodedNick, pwTypeInfo = wTypeInfo;
		 *pszSrc;
		 pszSrc = CharNext (pszSrc), pwTypeInfo++)
	{
		if (*pwTypeInfo & (C1_CNTRL | C1_BLANK))
		{
			wsprintf (szBufOut, "\"%s\"", pszDecodedNick);
			return szBufOut;
		}
	}
	return pszDecodedNick;
   #endif

}

#define US_CODEPAGE	1252

const char *DecodeChan(const char *szChannel, BOOL bForceDBCS) {
	int iPrefix = 0;
	char firstChar = *szChannel;

	if (!CHANNELPREFIX(firstChar))
		return szChannel;

	if (firstChar == '%') {
		LPWSTR wszBuff;
		int pCChOut, a;
		if (!bConvertUTF8StringToWide(szChannel, 0, &wszBuff, &pCChOut, FALSE, TRUE, TRUE)) goto error;
		if (!wszBuff) goto error;
		int codepage = bForceDBCS ? US_CODEPAGE : GetACP();
		if (!(a = WideCharToMultiByte(codepage, 0, wszBuff, -1, g_szScratchBuffer, g_nScratchBufferSize, NULL, NULL))) goto error;
		delete [] wszBuff;
		if (g_szScratchBuffer[0] == '%' && g_szScratchBuffer[1] != '\0') iPrefix = 2;
		szChannel = g_szScratchBuffer;
	}
	if (firstChar == '#' || firstChar == '&' || bForceDBCS) {
		if (g_iCharSet == ANSI_CHARSET) return szChannel + iPrefix; // fast out
		char *szDup = strdup(szChannel);
		char *szInterChan = szDup;
		BOOL ConvertEncodingIn(LPSTR *);
		BOOL bNeedFree = ConvertEncodingIn(&szInterChan);
		strcpy(g_szScratchBuffer, szInterChan);
		if (bNeedFree) delete [] szInterChan;
		free(szDup);
	}
	return g_szScratchBuffer + iPrefix;

error:
//	ASSERT(0);
	strcpy(g_szScratchBuffer, szChannel);
	return g_szScratchBuffer;
}


const char *EncodeChan(const char *szChannel) {
	if (*szChannel == '#' || *szChannel == '&') {
		if (g_iCharSet == ANSI_CHARSET) return szChannel;
		char *szDup = strdup(szChannel);
		char *szInterChan = szDup;
		BOOL ConvertEncodingOut(LPSTR *);
		BOOL bNeedFree = ConvertEncodingOut(&szInterChan);
		strcpy(g_szScratchBuffer, szInterChan);
		if (bNeedFree) delete [] szInterChan;
		free(szDup);
		return g_szScratchBuffer;
	}
	else if (*szChannel) {
		int pCChOut, a;
		char *szUtf8 = NULL;
		if (!(a = MultiByteToWideChar(GetACP(), MB_PRECOMPOSED, szChannel, -1, g_wszScratchBuffer, g_nScratchBufferSize))) goto error;

		if (!bConvertWideStringToUTF8(g_wszScratchBuffer, 0, &szUtf8, &pCChOut, FALSE, TRUE, TRUE, FALSE)) goto error;
		if (!szUtf8) goto error;
		strcpy(g_szScratchBuffer, szUtf8);
		delete [] szUtf8;
		return g_szScratchBuffer;
	}

error:
	// ASSERT(0);
	strcpy(g_szScratchBuffer, szChannel);
	return g_szScratchBuffer;
}


const char *DecodeString(const char *szString, int iEncoding) {
	if (iEncoding == ENC_DBCS) {
		if (g_iCharSet == ANSI_CHARSET) return szString;
		char *szDup = strdup(szString);
		char *szInterString = szDup;
		BOOL ConvertEncodingIn(LPSTR *);
		BOOL bNeedFree = ConvertEncodingIn(&szInterString);
		strcpy(g_szScratchBuffer, szInterString);
		if (bNeedFree) delete [] szInterString;
		free(szDup);
		return g_szScratchBuffer;
	} else {
		LPWSTR wszBuff;
		int pCChOut, a;
		if (!bConvertUTF8StringToWide(szString, 0, &wszBuff, &pCChOut, FALSE, FALSE, FALSE)) goto error;
		if (!wszBuff) goto error;
		if (!(a = WideCharToMultiByte(GetACP(), 0, wszBuff, -1, g_szScratchBuffer, g_nScratchBufferSize, NULL, NULL))) goto error;
		delete [] wszBuff;
		return g_szScratchBuffer;
	}

error:
	ASSERT(0);
	strcpy(g_szScratchBuffer, szString);
	return g_szScratchBuffer;
}


// TrimQuotesLocal: verbatim body of actions.cpp:116-123's TrimQuotes, lifted
// directly here under a distinct name (see ircproto.h's declaration note --
// actions.cpp itself, the original owner, is slash-command/rules territory
// not lifted this task).
void TrimQuotesLocal(CString &strIn)
{
	INT cbLen = strIn.GetLength();

	if (cbLen >= 2 && strIn.GetAt(0) == '\"' &&
		OurMbsRChr(((LPCTSTR)strIn)+1, '\"') == ((LPCSTR) strIn)+cbLen-1)
		strIn = strIn.Mid(1, cbLen-2);
}


void GetModeChars(DWORD dwFlags, char *szBuff) {
	// assumption: szBuff is large enough to hold mode string
	char *bptr = szBuff;
	if (dwFlags & CM_PRIVATE) *bptr++ = 'p';
	if (dwFlags & CM_HIDDEN) *bptr++ = 's';
	if (dwFlags & CM_INVITEONLY) *bptr++ = 'i';
	if (dwFlags & CM_TOPICHOST) *bptr++ = 't';
	if (dwFlags & CM_NOEXTERN) *bptr++ = 'n';
	if (dwFlags & CM_MODERATED) *bptr++ = 'm';
	if (dwFlags & CM_USERLIMIT) *bptr++ = 'l';
	if (dwFlags & CM_CHANNELKEY) *bptr++ = 'k';
	*bptr = '\0';
}


// --- chunk-break helper (verbatim, ircproto.cpp:398-472) --------------------

short nGetBreakingPoint(int iEncodingType, const char *szBody, short nBodyLen, short nMaxLength, WORD wFormatBegin, char *szFormatBegin, WORD *pwFormatEnd)
{
	ASSERT(pwFormatEnd);
	ASSERT(szBody);

	short	nFormatBeginLen = nFillFormatting(szFormatBegin, 0, wFormatBegin, *szBody);

	nMaxLength -= nFormatBeginLen;

	*pwFormatEnd = 0;

	if (nBodyLen <= nMaxLength)
		return nBodyLen;
	else
	{
		// here comes the tough one!

		const char	*szTmp = szBody;
		const char	*szFurthestSpaceStart = NULL, *szFurthestFormattingStart = NULL;
		const char	*szValidSpaceStart = szBody + (UINT) (nMaxLength * 0.8);
		BOOL		bInSpaces = FALSE;
		WORD		wLastFullFormat;

		// szTmp can point to a regular character or the starting point of a formatting sequence

		do
		{
			switch (*szTmp)
			{
				case chCtlColor:
				case chCtlBold:
				case chCtlItalic:
				case chCtlFixedPitchFont:
				case chCtlUnderline:
				case chCtlSymbol:
					if (!szFurthestFormattingStart)
						szFurthestFormattingStart = szTmp;
					szTmp = SzSkipOneFormat(szTmp, &wFormatBegin);
					break;

				default:
					szFurthestFormattingStart = NULL;
					wLastFullFormat = wFormatBegin;
					if (my_isspace(*szTmp))
					{
						if (!bInSpaces)
						{
							*pwFormatEnd = wFormatBegin;
							szFurthestSpaceStart = szTmp;
							bInSpaces = TRUE;
						}
					}
					else
						bInSpaces = FALSE;
					szTmp = (ENC_DBCS == iEncodingType) ? CharNext(szTmp) : SzNextUTF8Char(szTmp);
			}
		}
		while (szTmp < szBody + nMaxLength - 2);

		if (szFurthestSpaceStart && szFurthestSpaceStart >= szValidSpaceStart)
			// we found a space character close enough to the end and will break there - last formatting is already set
			return szFurthestSpaceStart - szBody;
		else
		{
			*pwFormatEnd = wLastFullFormat;
			// no space at all in the big string
			if (szFurthestFormattingStart)
				// we are in the middle of a formatting sequence
				return szFurthestFormattingStart - szBody - 1;	// we don't want to include the last formatting part
			else
				// we cut wherever we stopped in the middle of the string because there is no space or formatting
				return szTmp - szBody;
		}
	}
}


// --- SendMessageText: the single outbound choke point (R19) -----------------
// Original body: TRACE + m_pSock->Send(szMesg, strlen(szMesg)) (ircproto.cpp:
// 475-478). R19 rewrite: reaches ccSession()->cfg.send(...) instead of a
// CAsyncSocket -- Swift owns the actual socket (spec §4.1). This is the ONE
// place in the whole outbound surface that touches the session; every other
// builder in this file only ever calls SendMessageText, never cfg.send
// directly (mirrors the original's own single-choke-point discipline,
// discovery §4.1).
void CIrcProto::SendMessageText(char *szMesg) {
	TRACE("Sending message: %s\n", szMesg);
	ccSessionSendRaw(szMesg, strlen(szMesg));
}


// --- bChatSendToTarget (verbatim chunking algorithm, ircproto.cpp:481-698) --
// Deviations: (1) GetMyNickName()/GetMyUserName()/theApp.m_nMyIdentLength
// (:527-533) -> explicit PFNGETOWNIDENTITY resolver parameter (R19, see
// header note); (2) serverConn is now `*m_pSock` throughout instead of a
// second global -- both the original's `serverConn.m_nMaxMsgLength` and this
// object's own `m_pSock` always name the SAME instance in this task's tests
// (there is exactly one CIrcSocket, matching the original's one-shared-socket
// design, discovery §1), so this is a spelling change only, not a behavior
// change.
BOOL CIrcProto::bChatSendToTarget(const char *szAddressee, const char *szAnnotations, const char *szMesg, USHORT uModes, BOOL bAsNotice, PFNGETOWNIDENTITY pfnGetOwnIdentity)
{
	BOOL		bFreeTmp = FALSE;
	LPTSTR		szTmp = NULL;
	SHORT		nAnnotationsLen, nMesgLen, nTargetLen, nReceivingPrefixLen, nLen = 12; // 12 for IRC command PRIVMSG
	int			iEncodingType = 0;
	const char*	szTarget = szAddressee ? szAddressee : (const char*) m_strChannel;

	// szAddressee is NULL for channel messages

	if (szMesg)
	{
		iEncodingType = szAddressee ? ENC_DBCS : EncodingType();
		szMesg = EncodeString(szMesg, iEncodingType);

		// Might have to quote \r and \n
		bLowLevelQuoting(g_chLLQuoteCTCP, TRUE /*bTreatAsByteArray*/, szMesg, &szTmp, &bFreeTmp);
		szMesg = szTmp;
	}

	if (szAnnotations)
	{
		nAnnotationsLen = strlen(szAnnotations);
	}
	else
	{
		szAnnotations = "";
		nAnnotationsLen = 0;
	}

	if (szMesg)
	{
		nMesgLen = strlen(szMesg);
	}
	else
	{
		szMesg = "";
		nMesgLen = 0;
	}

	nTargetLen = strlen(szTarget);

	// on the receiving side, the prefix ":<nickname>!<username>@<hostname> " gets added, so we don't want the server
	// to cut the message to get room for this prefix, therefore:

	ASSERT(pfnGetOwnIdentity);
	cc_own_identity myId = pfnGetOwnIdentity();

	// 2 is for starting : and trailing space
	nReceivingPrefixLen = 2 + strlen(myId.own_nick);

	// for private messages: when hostname length is still unknown we use 32 by default
	if (myId.ident_length)
		nReceivingPrefixLen += myId.ident_length;
	else
		nReceivingPrefixLen += strlen(myId.own_user) + 32;

	nLen += nTargetLen + nAnnotationsLen + nMesgLen + nReceivingPrefixLen;	// final length of message on the receiving side

	if (nLen <= m_pSock->m_nMaxMsgLength)
	{
		// message is short enough to be sent in one shot
		if (*szAnnotations && IsIRCX())
		{
			sprintf(m_pSock->m_szOutput2, "DATA %s %s :%s\r\n", szTarget, CCUDI1, szAnnotations);
			SendMessageText(m_pSock->m_szOutput2);

			if (*szMesg)
			{
				sprintf(m_pSock->m_szOutput2, "%s %s :%s\r\n", (bAsNotice ? "NOTICE" : "PRIVMSG"),
																 szTarget, szMesg);
				SendMessageText(m_pSock->m_szOutput2);
			}
		}
		else
		{
			sprintf(m_pSock->m_szOutput2, "%s %s :%s%s\r\n", (bAsNotice ? "NOTICE" : "PRIVMSG"),
																szTarget, szAnnotations, szMesg);
			SendMessageText(m_pSock->m_szOutput2);
		}
	}
	else
	{
		ASSERT(nMesgLen > 0);
		ASSERT(iEncodingType > 0);

		char		szPrefix[16];
		const char	*szBody;
		short		nBodyLen, nPrefixLen = 0, nSuffixLen = 1;	// 1 is for the terminating 0x01 by default
		short		nBreakingPoint;
		char		chBreakingChar, chSuffixChar;
		char		szFormatBegin[11];				// max length would be for ^kWX,YZ^b^u^f for example
		WORD		wFormatBegin = 0, wFormatEnd;
		BOOL		bOnlySendOneChunk;

		bOnlySendOneChunk = (iEncodingType == ENC_DBCS) && (GetACP() == 932);

		// multiple chunks case
		switch (uModes)
		{
		case BM_ACTION:
			// szMesg = 0x01ACTION <data>0x01

			// REGISB: could include verb thinks into prefix for think button.
			nPrefixLen = g_nActionLen + 1;
			break;
		case BM_SOUND:
			// szMesg = 0x01SOUND <filename> <data>0x01
			nPrefixLen = g_nSoundLen + 1;
			break;
		case BM_AWAY:
			// szMesg = 0x01AWAY <data>0x01
			nPrefixLen = g_nAwayLen + 1;
			break;
		case BM_HERESINFO:
			// szMesg = # HeresInfo: <data>
			nPrefixLen = g_nHeresInfoLen + 1;	// +1 for # sign
			nSuffixLen = 0;						// no terminating 0x01
			break;
		case BM_SAY:
		case BM_THINK:
		case BM_WHISPER:
			// szMesg = <data>   ==> nPrefixLen = 0
			nSuffixLen = 0;		// no terminating 0x01
			break;
		default:
			ASSERT(0);
		}

		szBody = szMesg + nPrefixLen;
		nBodyLen = nMesgLen - nPrefixLen;

		ASSERT(szBody);
		ASSERT(nBodyLen == (short) strlen(szBody));

		// first prepare prefix
		if (nPrefixLen)
			strncpy(szPrefix, szMesg, nPrefixLen);
		szPrefix[nPrefixLen] = '\0';

		do
		{
			// send another chunk and update szBody

			// bytes allowed in szBody term = m_pSock->m_nMaxMsgLength - 12 - nChannelLen - nAnnotationsLen - nPrefixLen - nSuffixLen
			// 12 = "PRIVMSG  :\r\n"
			nBreakingPoint = nGetBreakingPoint(iEncodingType, szBody, nBodyLen, m_pSock->m_nMaxMsgLength - nReceivingPrefixLen - 12 - nTargetLen - nAnnotationsLen - nPrefixLen - nSuffixLen, wFormatBegin, szFormatBegin, &wFormatEnd);

			chBreakingChar = szBody[nBreakingPoint+nSuffixLen];
			((char*)szBody)[nBreakingPoint+nSuffixLen] = '\0';

			if (nSuffixLen)
			{
				chSuffixChar = szBody[nBreakingPoint];
				((char*)szBody)[nBreakingPoint] = '\001';
			}

			if (*szAnnotations && IsIRCX())
			{
				sprintf(m_pSock->m_szOutput2, "DATA %s %s :%s\r\n", szTarget, CCUDI1, szAnnotations);
				SendMessageText(m_pSock->m_szOutput2);

				sprintf(m_pSock->m_szOutput2, "%s %s :%s%s%s\r\n",
						bAsNotice ? "NOTICE" : "PRIVMSG",
						szTarget,
						szPrefix,
						szFormatBegin,
						szBody);
			}
			else
				sprintf(m_pSock->m_szOutput2, "%s %s :%s%s%s%s\r\n",
						bAsNotice ? "NOTICE" : "PRIVMSG",
						szTarget,
						szAnnotations,
						szPrefix,
						szFormatBegin,
						szBody);

			#ifdef DEBUG
				short nOutputLen = strlen(m_pSock->m_szOutput2);
				ASSERT(nOutputLen <= m_pSock->m_nMaxMsgLength);
			#endif // DEBUG

			SendMessageText(m_pSock->m_szOutput2);

			((char*)szBody)[nBreakingPoint+nSuffixLen] = chBreakingChar;

			if (nSuffixLen)
				((char*)szBody)[nBreakingPoint] = chSuffixChar;

			// goto the beginning of the next chunk
			szBody += nBreakingPoint;
			nBodyLen -= nBreakingPoint;

			while (my_isspace(*szBody))	// we skip all spaces and tabs...
			{
				szBody++;				// all space type chars are single byte characters
				nBodyLen--;
			}

			wFormatBegin = wFormatEnd;	// assure formatting continuation

			ASSERT(nBodyLen == (short) strlen(szBody));

			if (BM_SOUND == uModes)
			{
				uModes = BM_ACTION;
				nPrefixLen = g_nActionLen + 1;
				strncpy(szPrefix, actionID, g_nActionLen);
				szPrefix[g_nActionLen] = ' ';
				szPrefix[g_nActionLen+1] = '\0';
			}
		}
		while (szBody && *szBody && !bOnlySendOneChunk);
	}

	if (bFreeTmp)
		delete [] szTmp;

	return TRUE;
}


BOOL CIrcProto::bChatSendPrivMesg(const char *szAddressee, const char *szAnnotations, const char *szMesg, char *szNMText, BOOL bAsNotice, USHORT uModes, PFNGETOWNIDENTITY pfnGetOwnIdentity)
{
	return bChatSendToTarget(szAddressee, szAnnotations, szMesg, uModes, bAsNotice, pfnGetOwnIdentity);
}


BOOL CIrcProto::bChatSendToChannel(const char *szAnnotations, const char *szMesg, char *szNMText /*=NULL*/, USHORT uModes /*=0*/, PFNGETOWNIDENTITY pfnGetOwnIdentity)
{
	return bChatSendToTarget(NULL, szAnnotations, szMesg, uModes, FALSE, pfnGetOwnIdentity);
}


BOOL CIrcProto::ChatSetTopic(const char *szTopic)
{
	if (*szTopic)
		szTopic = EncodeString(szTopic);
	return bExecuteQuery(qpSetTopic, ctTopic, dtMax, (PVOID) szTopic, m_strChannel, "");
}


BOOL CIrcProto::ChatSetClientData(const char *szClientData) {
	if (IsIRCX ()) {
		ASSERT(szClientData);
		if (*szClientData) szClientData = EncodeString(szClientData);	// REGIS: why converting string??

		// REGISB 04/03/98 need to Q the PROP setting because of the Status Window display
		return bExecuteQuery(qpSetClient, ctPropSet, dtMax, (PVOID) szClientData, m_strChannel, "");
	}
	else {
		return FALSE;
	}
}

// HandleClientDataChange NOT LIFTED this task -- see file-header note
// ("ADDITIONAL NOT-LIFTED"): its only caller is the inbound PROP handler
// (ircsock.cpp, Task 5b), not any outbound builder.

void CIrcProto::ChatPartChannel() {
	if (m_bInRoom) {
		sprintf(m_pSock->m_szOutput2, "PART %s\r\n", (LPCTSTR) m_strChannel);  // exit gracefully
		SendMessageText(m_pSock->m_szOutput2);
	}

	// Rules-engine fan-out (theApp.m_dynaRules.bMatchAndApplyRules(eOnLeave,
	// ...), original ircproto.cpp:776-782) NOT LIFTED -- app automation, out
	// of scope (see file-header site #12). GetConnectionStatus()/
	// SetConnectionStatus() bookkeeping around it is likewise app/session
	// state (Task 7's CCSession tracks per-room status directly instead of
	// through this seam).
}


void CIrcProto::ChatJoinAux(const char *szChannel, const char *szPassword)
{
	ASSERT(szChannel && *szChannel);
	if (!szPassword || !*szPassword)
		sprintf(m_pSock->m_szOutput2, "JOIN %s\r\n", szChannel);
	else
	{
		int iEncoding = (szChannel[0] == '#' || szChannel[0] == '&') ? ENC_DBCS : ENC_UTF8;
		CString strPwd = EncodeString(szPassword, iEncoding);
		sprintf(m_pSock->m_szOutput2, "JOIN %s %s\r\n", szChannel, (LPCTSTR) strPwd);
	}

	SendMessageText(m_pSock->m_szOutput2);
}


void CIrcProto::ChatCreateAux(const char *szChannel, const char *szCreationModes, DWORD dwMaxUsers, const char *szPassword)
{
	ASSERT(szChannel && *szChannel);

	CString strParams;

	if (szCreationModes && *szCreationModes)
		strParams += CString(" ") + szCreationModes;

	if (dwMaxUsers)
	{
		char szMaxUsers[16];
		sprintf(szMaxUsers, "%d", (int) dwMaxUsers);
		strParams += CString(" ") + CString(szMaxUsers);
	}

	if (szPassword && *szPassword)
	{
		int iEncoding = (szChannel[0] == '#' || szChannel[0] == '&') ? ENC_DBCS : ENC_UTF8;
		CString strPwd = EncodeString(szPassword, iEncoding);
		strParams += CString(" ") + strPwd;
	}

	sprintf(m_pSock->m_szOutput2, "CREATE %s%s\r\n", szChannel, (LPCTSTR) strParams);
	SendMessageText(m_pSock->m_szOutput2);
}


BOOL CIrcProto::ChatKickUser(const char *szNickname, const char *szReason)
{
	if (szReason && *szReason)
		szReason = EncodeString(szReason);
	sprintf (m_pSock->m_szOutput2, "KICK %s %s :%s\r\n", (LPCTSTR) m_strChannel, szNickname, szReason ? szReason : "");
	SendMessageText(m_pSock->m_szOutput2);
	return TRUE;
}


BOOL CIrcProto::ChatBanUser(const char *szBanPattern, BOOL bBan, const char *szEncodedChannel /* = NULL */)
{
	const char *szFlag = bBan ? "+b" : "-b";

	LPCSTR szNickEnd = OurMbsChr(szBanPattern, '!');
	LPTSTR szNickname;

	if (szNickEnd)
	{
		LPCSTR szNickBegin = szBanPattern;

		while (*szNickBegin == '?' || *szNickBegin == '*')
			szNickBegin++;
		if (!(szNickname = new TCHAR[szNickEnd-szNickBegin+1]))
			return FALSE;
		strncpy(szNickname, szNickBegin, szNickEnd-szNickBegin);
		szNickname[szNickEnd-szNickBegin] = '\0';
	}
	else
		szNickname = (LPTSTR) szBanPattern;

	if (IsIRCX() && bExtendedNickname(szNickname))
		szBanPattern = EncodeNick(szBanPattern);
	sprintf(m_pSock->m_szOutput2, "MODE %s %s %s\r\n", szEncodedChannel ? szEncodedChannel : (LPCTSTR) m_strChannel, szFlag, szBanPattern);
	SendMessageText(m_pSock->m_szOutput2);

	if (szNickname != szBanPattern)
		delete [] szNickname;

	return TRUE;
}


BOOL CIrcProto::ChatSendInvitation(const char *szNickname)
{
	sprintf(m_pSock->m_szOutput2, "INVITE %s %s\r\n", szNickname, (LPCTSTR) m_strChannel);
	SendMessageText(m_pSock->m_szOutput2);
	return TRUE;
}


BOOL CIrcProto::ChatChangeNick(const char *szNewNick)
{
	if (IsIRCX() && bExtendedNickname(szNewNick))
		szNewNick = EncodeNick(szNewNick);

	sprintf(m_pSock->m_szOutput2, "NICK %s\r\n", szNewNick);
	SendMessageText(m_pSock->m_szOutput2);
	return TRUE;
}


// ChatSetAway: fan-out to every OTHER open room's CIrcProto (original
// ircproto.cpp:930-937, `POSITION pos = g_docs.GetHeadPosition(); ...`) NOT
// LIFTED this task (R20 -- see ircproto.h's declaration comment and
// file-header site #13: g_docs is a session-wide room table this task's
// single-CIrcProto scope has no access to). The bProtoNotify==FALSE
// "simple notification" early-return branch (CRoomInfo::ChatSetAway) is also
// not lifted -- it's a CRoomInfo virtual this port's concrete CIrcProto
// doesn't inherit (see ircproto.h's structural deviation note); every call
// site this task's outbound C functions expose always wants the wire command
// (bProtoNotify==TRUE path), so that branch would be dead code here.
void CIrcProto::ChatSetAway(BOOL bAway, const char *szMesg)
{
	// szMesg is a control full string

	if (bAway)
		sprintf(m_pSock->m_szOutput2, "AWAY :%s\r\n", szMesg);		// !REGISB! 10/14/97 don't we need to encode the szMesg??
	else
		sprintf(m_pSock->m_szOutput2, "AWAY\r\n");

	SendMessageText(m_pSock->m_szOutput2);
}


BOOL CIrcProto::ChatSetMode(DWORD newMode, DWORD newMaxUsers, const char *szNewPasswd) {
	char szMaxUsers[7] = "";
	const char *szKey;
	// DEVIATION (documented): the original compares against the GLOBAL
	// `currentRoom` (dwCurrentChannelMode/strCurrentChannelKey/
	// dwCurrentUserLimit macros, chatprot.h:100-104) -- "the room currently
	// being entered/created", a separate object from `this` used only during
	// the join/create handshake sequencing (discovery §9 flags this exact
	// ambiguity: "whichever side owns it, the bridge needs a single
	// authority"). That handshake is inbound-flow territory (Task 5b/6/7),
	// out of this task's scope. This lift instead compares against THIS
	// room's own current state (m_dwModes/m_strPassword/m_dwMaxUsers) -- the
	// room the room_token resolves to -- which is the only state this task's
	// CIrcProto has. Escalate/revisit once Task 7 wires the real join
	// handshake if `currentRoom`'s distinct pre-join semantics turn out to
	// matter for ChatSetMode specifically (it is only reachable post-join in
	// every call site the original exposes: DoChannelDialog's mode-change UI).
	DWORD newSets = newMode & ~m_dwModes;
	DWORD newUnSets = m_dwModes & ~newMode;

	if ((newMode & CM_USERLIMIT) && newMaxUsers != m_dwMaxUsers) {
		newSets |= CM_USERLIMIT;
		sprintf(szMaxUsers, "%d", (int) newMaxUsers);
	} else newSets &= ~CM_USERLIMIT;

	if (szNewPasswd && *szNewPasswd)
		szNewPasswd = EncodeString(szNewPasswd);

	if ((newMode & CM_CHANNELKEY) && strcmp(szNewPasswd, m_strPassword)) {
		newSets |= CM_CHANNELKEY;
		if (!m_strPassword.IsEmpty())
			newUnSets |= CM_CHANNELKEY;   // necessary to unset before we can set!
	} else newSets &= ~CM_CHANNELKEY;

	char modeBuff[20];
	GetModeChars(newUnSets, modeBuff);
	if (*modeBuff) {
		if (newUnSets & CM_CHANNELKEY) szKey = m_strPassword;
		else szKey = "";
		sprintf(m_pSock->m_szOutput2, "MODE %s -%s %s\r\n", (LPCTSTR) m_strChannel, modeBuff, szKey);
		SendMessageText(m_pSock->m_szOutput2);
	}
	GetModeChars(newSets, modeBuff);
	if (*modeBuff) {
		if (!(newSets & CM_CHANNELKEY)) szKey = "";
		else szKey = szNewPasswd;
		sprintf(m_pSock->m_szOutput2, "MODE %s +%s %s %s\r\n", (LPCTSTR) m_strChannel, modeBuff, szMaxUsers, szKey);
		SendMessageText(m_pSock->m_szOutput2);
	}
	return TRUE;
}


// ChatGetIdentity NOT LIFTED this task -- see file-header site #14.


BOOL CIrcProto::bRegisterMode(char* szMesg)
{
	char	*szTmp;
	char	*szTarget = GetToken(szMesg, &szTmp, " ");

	if (CHANNELPREFIX(*szTarget))
		return bExecuteQuery(qpComSetChannelMode, ctSetChannelMode, dtMax, (PVOID) szTmp, szTarget, "");
	else
		return bExecuteQuery(qpComSetUserMode, ctSetUserMode, dtMax, (PVOID) szTmp, "", szTarget);
}


// --- bExecuteQuery (verbatim, ircproto.cpp:1034-1219) -----------------------
// Deviation: the MODE ISIRCX case's caller-side timer arming
// (`::AfxGetMainWnd()->SetTimer(ID_ISIRCXTIMEOUT, ISIRCXTIMEOUT, NULL)`) is
// NOT part of this function in the original -- that SetTimer call lives in
// CIrcSocket::OnConnect (ircsock.cpp:1050-1052), which sends the MODE ISIRCX
// probe itself directly, NOT through bExecuteQuery. This task's OnConnect
// equivalent is not lifted (that's the connection-establishment flow, Task
// 5b/7); the brief instead asks for the timer REQUEST to travel with THIS
// query builder (the one other MODE-ISIRCX-shaped call site this task
// exposes), via cfg.set_timer(CC_TIMER_ISIRCX_PROBE, 50000) -- see
// cc_session.cpp's bExecuteQuery wrapper for the actual cfg.set_timer call
// (kept in the bridge layer, not this engine function, so this function
// stays a pure CCQuery-list mutation + wire-string builder like the
// original).
BOOL CIrcProto::bExecuteQuery(enumQueryPurpose qp,
							  enumCommandType ct,
							  enumDataType dt,
							  PVOID pvData,
							  CString strChannelName,
							  CString strNicknameMask)
{
	PPRUSERMATCH	pPrUserMatch;
	LPTSTR			szFilter = NULL;
	UINT			cbFilter = 0;
	CCQuery*		pQuery;

	// Make sure we are connected...
	// DEVIATION: the original checks GetConnectionStatus() == CX_DISCONNECTED
	// (a CRoomInfo virtual this concrete CIrcProto doesn't inherit -- see
	// ircproto.h's structural note). Connection-status tracking is Task 7's
	// CCSession's job; this task's bExecuteQuery is exercised directly by
	// tests/outbound C functions without a connection-status gate. Task 7
	// should re-add an equivalent guard at the C-function layer
	// (cc_session_*) once the session tracks connection state.

	pQuery = new CCQuery(qp, ct, dt, pvData, strChannelName, strNicknameMask, !strNicknameMask.IsEmpty() && ctWho == ct /*bCreatePrUserMatch*/);
	if (!pQuery)
	{
		ASSERT(FALSE);
		return FALSE;
	}

	if (!m_pSock->m_queries.bAddQuery(pQuery))
	{
		ASSERT(FALSE);
		return FALSE;
	}

	switch (ct)
	{
	case ctWho:
		if (pPrUserMatch = pQuery->GetPrUserMatch())
		{
			if (pPrUserMatch->cbNickname)
			{
				szFilter = pPrUserMatch->szNickname;
				cbFilter = pPrUserMatch->cbNickname;
			}
			if (pPrUserMatch->cbUserName > cbFilter)
			{
				szFilter = pPrUserMatch->szUserName;
				cbFilter = pPrUserMatch->cbUserName;
			}
			if (pPrUserMatch->cbIPAddress > cbFilter)
			{
				szFilter = pPrUserMatch->szIPAddress;
				cbFilter = pPrUserMatch->cbIPAddress;
			}
		}
		if (szFilter)
		{
			LPTSTR szFilterTmp = strdup(szFilter);
			szFilterTmp[cbFilter] = g_chEOS;
			sprintf(m_pSock->m_szOutput2, "WHO %s\r\n", szFilterTmp);
			free(szFilterTmp);
		}
		else
			if (strChannelName.IsEmpty())
				strcpy(m_pSock->m_szOutput2, "WHO\r\n");
			else
				sprintf(m_pSock->m_szOutput2, "WHO %s\r\n", (LPCTSTR) strChannelName);
		break;

	case ctWhoIs:
		ASSERT(!strNicknameMask.IsEmpty());
		sprintf(m_pSock->m_szOutput2, "WHOIS %s\r\n", (LPCTSTR) strNicknameMask);
		break;

	case ctTopic:
		switch (qp)
		{
		case qpSetTopic:
			sprintf(m_pSock->m_szOutput2, "TOPIC %s :%s\r\n", (LPCTSTR) strChannelName, (char*) pvData);
			break;
		case qpListMembers:
			sprintf(m_pSock->m_szOutput2, "TOPIC %s\r\n", (LPCTSTR) strChannelName);
			break;
		default:
			ASSERT(FALSE);
		}
		break;

	case ctList:
		if (strChannelName.IsEmpty())
			strcpy(m_pSock->m_szOutput2, "LIST\r\n");
		else
			sprintf(m_pSock->m_szOutput2, "LIST %s\r\n", (LPCTSTR) strChannelName);
		break;

	case ctListX:
		if (strChannelName.IsEmpty())
			strcpy(m_pSock->m_szOutput2, "LISTX\r\n");
		else
			sprintf(m_pSock->m_szOutput2, "LISTX N=%s\r\n", (LPCTSTR) strChannelName);
		break;

	case ctLUsersMOTD:
		strcpy(m_pSock->m_szOutput2, "LUSERS\r\nMOTD\r\n");
		break;

	case ctIrcX:
		strcpy(m_pSock->m_szOutput2, "IRCX\r\n");
		break;

	case ctModeIsIrcX:
		strcpy(m_pSock->m_szOutput2, "MODE ISIRCX\r\n");
		break;

	case ctGetChannelMode:
		sprintf(m_pSock->m_szOutput2, "MODE %s\r\n", (LPCTSTR) strChannelName);
		break;

	case ctSetChannelMode:
		sprintf(m_pSock->m_szOutput2, "MODE %s%s\r\n", (LPCTSTR) strChannelName, (char*) pvData);
		break;

	case ctSetUserMode:
	{
		const char* szModes;

		switch (qp)
		{
		case qpSetVisible:
			ASSERT(!strNicknameMask.IsEmpty());
			szModes = "-i";
			break;
		case qpSetInvisible:
			ASSERT(!strNicknameMask.IsEmpty());
			szModes = "+i";
			break;
		case qpComSetUserMode:
			szModes = (const char*) pvData;
			break;
		default:
			ASSERT(FALSE);
			szModes = "";
		}

		sprintf(m_pSock->m_szOutput2, "MODE %s %s\r\n", (LPCTSTR) strNicknameMask, szModes);
		break;
	}

	case ctPropGet:
	{
		LPCTSTR szPropName;
		switch (qp)
		{
		case qpJoinPics:
		case qpCreatePics:
			szPropName = "PICS";
			break;
		case qpJoinBackUrl:
			szPropName = "CLIENT";
			break;
		default:
			ASSERT(FALSE);
			szPropName = "";
		}
		ASSERT(!strChannelName.IsEmpty());
		sprintf(m_pSock->m_szOutput2, "PROP %s %s\r\n", (LPCTSTR) strChannelName, szPropName);
		break;
	}

	case ctPropSet:
	{
		LPCTSTR szPropName;
		switch (qp)
		{
		case qpSetClient:
			szPropName = "CLIENT";
			break;
		default:
			ASSERT(FALSE);
			szPropName = "";
		}
		ASSERT(!strChannelName.IsEmpty());
		sprintf(m_pSock->m_szOutput2, "PROP %s %s :%s\r\n", (LPCTSTR) strChannelName, szPropName, (char*) pvData);
		break;
	}

	default:
		ASSERT(FALSE);
		return FALSE;
	}
	SendMessageText(m_pSock->m_szOutput2);

	return TRUE;
}


// bChatShowMOTD NOT LIFTED this task: a CRoomInfo virtual override
// (ircproto.cpp:1222-1225) with no distinct body beyond a single
// bExecuteQuery(qpLUsersMOTD, ctLUsersMOTD, ...) call -- trivially
// reconstructable as a one-line C function (cc_session_show_motd) directly
// against bExecuteQuery if a later task needs it; not in this task's named
// Produces list.


// DoIgnoreUser NOT LIFTED this task -- see file-header site #15.


inline int CIrcProto::EncodingType()
{
	const char *szChannel = (LPCTSTR) m_strChannel;
	return (szChannel[0] == '%' && !(m_dwModes & CM_MIC)) ? ENC_UTF8 : ENC_DBCS;
}


const char *CIrcProto::EncodeString(const char *szString, int iEncoding) {
	if (iEncoding == ENC_CHANNEL)
		iEncoding = EncodingType();

	if (iEncoding == ENC_DBCS) {
		if (g_iCharSet == ANSI_CHARSET) return szString;
		char *szDup = strdup(szString);
		char *szInterString = szDup;
		BOOL ConvertEncodingOut(LPSTR *);
		BOOL bNeedFree = ConvertEncodingOut(&szInterString);
		strcpy(g_szScratchBuffer, szInterString);
		if (bNeedFree) delete [] szInterString;
		free(szDup);
		return g_szScratchBuffer;
	} else {
		int pCChOut, a;
		char *szUtf8 = NULL;
		if (!(a = MultiByteToWideChar(GetACP(), MB_PRECOMPOSED, szString, -1, g_wszScratchBuffer, g_nScratchBufferSize))) goto error;

		if (!bConvertWideStringToUTF8(g_wszScratchBuffer, 0, &szUtf8, &pCChOut, FALSE, FALSE, FALSE, FALSE)) goto error;
		if (!szUtf8) goto error;
		ASSERT(strlen(szUtf8) < (size_t) g_nScratchBufferSize);
		strcpy(g_szScratchBuffer, szUtf8);
		delete [] szUtf8;
		return g_szScratchBuffer;
	}

error:
	ASSERT(0);
	strcpy(g_szScratchBuffer, szString);
	return g_szScratchBuffer;
}


// --- StrEncodeCommandParam (verbatim, ircproto.cpp:1302-1386) --------------
// Deviation: LookupDoc(szEncodedChannelName) (:1322, "is there a room joined
// under this name") -> explicit PFNISJOINEDCHANNEL predicate parameter (R19,
// discovery §6 site #16).
CString CIrcProto::StrEncodeCommandParam(DWORD dwAt, INT *piEncoding, CHAR *szParam, PFNISJOINEDCHANNEL pfnIsJoinedChannel)
{
	ASSERT(szParam);
	ASSERT(piEncoding);

	DWORD		dwAtTmp = dwAt & (AT_CHANNEL|AT_NICKNAME|AT_NICKMASK|AT_REASON|AT_PASSWORD|AT_TOPIC|AT_MESSAGE|AT_PROPVALUE);
	const char*	szEncodedChannelName = NULL;

	if (!dwAtTmp)
		return szParam;

	if ((dwAtTmp & AT_CHANNEL) && (dwAtTmp & AT_NICKNAME))
	{
		if (szParam[0] == '%' || szParam[0] == '#' || szParam[0] == '&')
			// definitely a channel name
			dwAtTmp &= ~AT_NICKNAME;
		else
		{
			// don't know yet if it's a channel name or nickname
			szEncodedChannelName = EncodeChan(szParam);
			BOOL bIsJoined = pfnIsJoinedChannel ? pfnIsJoinedChannel(szEncodedChannelName) : FALSE;
			if (bIsJoined)
				// it must be a channel since we are in a channel with that name
				dwAtTmp &= ~AT_NICKNAME;
			else
			{
				// let consider this is a nickname and not a channel name - we don't check if this nick exists on purpose
				dwAtTmp &= ~AT_CHANNEL;
				szEncodedChannelName = NULL;
			}
		}
	}

	if (szEncodedChannelName)
	{
		// we already have our encoded answer
		ASSERT(dwAtTmp == AT_CHANNEL);
		*piEncoding = ENC_UTF8;
		return szEncodedChannelName;
	}

	switch (dwAtTmp)
	{
	case AT_CHANNEL:
		*piEncoding = (szParam[0] == '&' || szParam[0] == '#') ? ENC_DBCS : ENC_UTF8;
		return EncodeChan(szParam);

	case AT_NICKNAME|AT_NICKMASK:
	case AT_NICKNAME:
	case AT_NICKMASK:
	{
		LPSTR	szNickPortion = szParam;
		CString strNick = szParam;
		TrimQuotesLocal(strNick);

		if (dwAtTmp & AT_NICKMASK)
		{
			LPCSTR szNickEnd = OurMbsChr(szParam, '!');

			if (szNickEnd)
			{
				LPCSTR szNickBegin = szParam;

				while (*szNickBegin == '?' || *szNickBegin == '*')
					szNickBegin++;
				if (!(szNickPortion = new CHAR[szNickEnd-szNickBegin+1]))
					return strNick;
				strncpy(szNickPortion, szNickBegin, szNickEnd-szNickBegin);
				szNickPortion[szNickEnd-szNickBegin] = '\0';
			}
		}

		if (IsIRCX() && bExtendedNickname(szNickPortion))
			strNick = EncodeNick(strNick);

		if (szNickPortion != szParam)
			delete [] szNickPortion;

		return strNick;
	}
	default:
		// AT_REASON | AT_TOPIC | AT_PASSWORD | AT_MESSAGE | AT_PROPVALUE
		return EncodeString(szParam, *piEncoding);
	}
}

// ChangeProperty NOT LIFTED this task -- see file-header note
// ("ADDITIONAL NOT-LIFTED").

// CIdentdSocket / StartIdentD / StopIdentD: R21 DROP -- see file-header
// site #17. Not present in this file at all (stronger than a compile-time
// guard).
