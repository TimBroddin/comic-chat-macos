// protsupp.cpp — PARTIAL LIFT — codec only in Task 3; session/pipeline in
// Task 7. Lifted from v2.5-beta-1-modern/protsupp.cpp (5260 lines); this file
// carries over ONLY the annotation-codec subset named in the Plan 3 Task 3
// brief: the byte packers (IndexToByte/ByteToIndex/SM2BM/BM2SM, :1023-1063),
// the tokenizers (GetToken/GetToken1/GetToken2/bForEachWord, :257-422), the
// annotation encoder/decoder (bInsertAnnotations :3057, ProcessUDIData
// :1485), the addressee-string builders (GetAddressees/GetWhisperedAddressees
// :3006), the talk-to decoder (GetTalkTos x2 :1066), and the PROP CLIENT
// key-string codec (FindInKeyString/ChangeKeyString/GetValueFromKeyString/
// EnumKeyString, :5073-5260).
//
// Everything else in the original protsupp.cpp -- session-state management
// (LookupPui/CIUserJoin/CIUserPart/ReinstallPui/...), the inbound message
// pipeline (ProcessSay/ProcessComment/OnKick/...), the outbound pipeline
// (bChatSendText/bSendWhispers/...), the slash-command layer, connection
// orchestration, and pure UI dialogs -- is NOT copied here. Task 7 lifts the
// remaining pieces this plan's later tasks need, at the point they need them.
//
// Edit Rules applied: R1 (stdafx.h -> mfc_compat.h), R2 (chat.h/theApp ->
// engine_context.h/ccContext(), not needed by this subset -- no function
// lifted here reads theApp/GetChatDoc directly, see the per-function notes
// below), R8 (drop UI/doc includes; forward-declare CChatDoc/CUserInfo/
// CAvatarX in protsupp.h), R13 (LP64 pointer-truncation rewrites, matching
// the convention panel.cpp already established in Plan 2 Task 8). Four
// deviations beyond mechanical rule application, each individually
// attributed:
//   1. bInsertAnnotations: `MyAvatar()` (a CC_NO_UI-stubbed session accessor
//      that always returns NULL in this build -- see avatar.cpp:556-575)
//      hoisted to an explicit `CAvatarX *av` parameter. Calling the global
//      here would make the encoder permanently dead code in this build
//      configuration; hoisting it is the "not codec -> parameter" case the
//      task brief calls for, not an R20 UI-wrap (MyAvatar itself is not UI,
//      it is session/doc state).
//   2. ProcessUDIData: `theApp.m_bVIPMode` (a session policy flag) hoisted to
//      an explicit `BOOL bVIPMode` parameter (discovery doc §3.2 flags this
//      exact line as "policy -- hoist out").
//   3. GetTalkTos (both overloads) and ProcessUDIData: `LookupPui` (the
//      nick -> CUserInfo* session-table resolver) hoisted to an explicit
//      `PFNLOOKUPPUI pfnLookupPui` function-pointer parameter, per R19 ("stub
//      LookupPui behind R19 for now"; Task 7 wires the real
//      ccSessionResolveUser() resolver). No LookupPui symbol is declared or
//      defined in this file at all -- there is nothing to accidentally link
//      against before Task 7 does the real wiring.
//   4. GetAddressees (+ bInsertAnnotations, which calls it): the original's
//      `(CUserInfo *)(pui->m_udi.m_talkTos[i])` direct DWORD->pointer
//      widening is sound only when DWORD and pointers are the same width
//      (true on the original's 32-bit Win32 target). On this LP64 port it
//      zero-extends a truncated 32-bit value into a garbage 64-bit pointer,
//      and DEREFERENCING it (pui2->GetName()) is undefined behavior --
//      verified by reproduction (a real stack CUserInfo* crashed the
//      selftest with exactly this cast during Task 3 development). This is
//      the SAME hazard panel.cpp's R13 note + engine_context.h's
//      CCSessionSettings::userFromTalkTo already document and solve for the
//      camera/panel code (Plan 2 Task 8) -- GetAddressees needs the identical
//      reconstruction, hoisted to an explicit `PFNRESOLVETALKTO pfnResolve`
//      parameter (R19-style: no new session-table wiring invented in this
//      codec-only task). GetWhisperedAddressees does NOT need this fix --
//      g_rgpuiWhisperees is a CPtrArray (full-width void* elements, never
//      DWORD-truncated), so its original direct cast is already 64-bit-safe.
// All other lines are copied verbatim from the original (whitespace/brace
// style aside).

#include "mfc_compat.h"  // R1 (was stdafx.h)
#include "defines.h"     // SM_*/BM_*/C*PREFIX/MAX_TOKEN/my_isspace (R2's chat.h carried this transitively)
#include "protsupp.h"
#include "avatar.h"      // CAvatarX::GetIndices/GetEmotions, CEmotion (bInsertAnnotations)
#include "userinfo.h"    // CUserInfo, CUserDisplayInfo (already lifted, Plan 2 Task 8)
#include "ccommon_str.h" // Plan 3 Task 6: bLowLevelUnquoting/g_chLLQuoteCTCP (ProcessSay/ProcessComment's first line)
#include "ircproto.h"    // Plan 3 Task 6: CTCP ID tables (actionID/soundID/...) + comment prefixes (APPEARSPREFIX/...)
#include "format.h"      // Plan 3 Task 6: nResettingSequence (PrepareSound)
#include "cc_session.h"  // Plan 3 Task 6 (R19): ccSessionOwnNick/ccSessionResolveUser + ccSession()

// UnConst (chat.h:311 in the original -- a plain const-cast helper, not lifted
// on its own; ircsock.cpp already carries an identical file-local copy for
// the same reason -- R1/R8, not worth a shared header for one line).
static inline char *UnConst(const char *sz) { return const_cast<char*>(sz); }

// --- GetMyNickName (R19; DECISION recorded here, task report cites this) ----
// GetMyNickName (originally setupdlg.cpp's UI-layer accessor for
// theApp.m_myNick) had a Plan-2 R12(b) trap stub in cc_link_stubs.cpp
// (`return "";`) that existed ONLY to satisfy the linker for
// CUserInfo::IsSelf() (userinfo.h:113-116, an INLINE virtual, untouched,
// byte-identical to the original: `const char *GetMyNickName(); return
// (strcmp(GetName(), GetMyNickName()) == 0);` -- note this is a local
// FUNCTION DECLARATION inside the method body, an archaic but valid C++
// construct, that then calls the free function of the same name). This task
// (R19) REPLACES that stub with a real forwarder to ccSessionOwnNick() (the
// resolver Task 1 defined for exactly this "own nick" datum, wired by
// bridge/cc_session.cpp Step 4) -- GetMyNickName now answers with the
// SESSION's actual own nick instead of a permanent "no self" placeholder.
// DECISION on IsSelf(): no change needed to userinfo.h at all. IsSelf() was
// ALREADY correctly wired to call the free function GetMyNickName() (not a
// member) by the original's own code shape; giving that free function a real
// body is sufficient -- IsSelf() now correctly compares against the live
// session nick for any CUserInfo it's called on (session users AND the
// scratch CUserInfo objects this file's payload stage creates), with no
// further wiring. This function lives HERE (protsupp.cpp) rather than back
// in cc_link_stubs.cpp because protsupp.cpp is this port's "session-state
// glue" home for the payload stage (R19's own resolvers are implemented
// alongside it) and because GetMyNickName's real original home
// (setupdlg.cpp) is UI, never slated for a lift -- protsupp.cpp is the
// closest non-UI file that already reroutes session-identity questions
// through ccSession()'s resolvers.
const char* GetMyNickName() { return ccSessionOwnNick(); }

// EmotionToBytes is defined in avatario.cpp (now live, Task 3 un-gates
// CC_NO_PROTOCOL); declared here exactly as the original does at its call
// site (protsupp.cpp:3059 declares it locally inside bInsertAnnotations).
void EmotionToBytes(CEmotion &em, BYTE &emotion, BYTE &intensity);

// --- byte packers (verbatim, protsupp.cpp:1023-1063) ------------------------

BYTE IndexToByte(BYTE byteIn)
{
	return byteIn + '0';
}


BYTE ByteToIndex(BYTE byteIn)
{
	return byteIn - '0';
}


USHORT SM2BM(BYTE byteMode)
{
	switch (byteMode)
	{
	case SM_WHISPER:
		return BM_WHISPER;
	case SM_THINK:
		return BM_THINK;
	case SM_ACTION:
		return BM_ACTION;
	default:
		return BM_SAY;
	}
}


BYTE BM2SM(USHORT uModes)
{
	if ((uModes & BM_ACTION) || (uModes & BM_SOUND))
		return SM_ACTION;

	if (uModes & BM_WHISPER)
		return SM_WHISPER;

	if (uModes & BM_THINK)
		return SM_THINK;

	return SM_SAY;
}


// --- tokenizers (verbatim, protsupp.cpp:257-422) ----------------------------

// GetToken, the new version, now allows different opening and closing separators.
char *GetToken2(char *szStart, char **pszNextStart, const char *szSepsBegin, const char *szSepsEnd, char **pszCurStart /* = NULL */)
{
	static char sszBuff[MAX_TOKEN];

	while (*szStart && (my_isspace(*szStart) || strchr(szSepsBegin, *szStart)))
		szStart++;

	if (pszCurStart)
		*pszCurStart = szStart;

	if (!*szStart)
		return NULL;

	char *szEndPtr = szStart;

	while (*szEndPtr && !my_isspace(*szEndPtr) && !strchr(szSepsEnd, *szEndPtr))
		szEndPtr++;

	int nChars = szEndPtr - szStart;

	nChars = min(nChars, sizeof(sszBuff)-1); // don't overrun buff!
	ASSERT(nChars);
	strncpy(sszBuff, szStart, nChars);
	sszBuff[nChars] = '\0';
	*pszNextStart = szEndPtr;
	return sszBuff;
}


char *GetToken1(char *szStart, char **pszNextStart, const char *szSeps, char **pszCurStart /* = NULL */, BOOL bSkipInitialSeps /* = TRUE */)
{
	static char sszBuff[MAX_TOKEN];

	while (*szStart && (my_isspace(*szStart) || (bSkipInitialSeps && strchr(szSeps, *szStart))))
		szStart++;

	if (pszCurStart)
		*pszCurStart = szStart;

	if (!*szStart)
		return NULL;

	char *szEndPtr = szStart;

	if (!bSkipInitialSeps && strchr(szSeps, *szEndPtr))
		szEndPtr++;

	while (*szEndPtr && !strchr(szSeps, *szEndPtr))
		szEndPtr++;

	int nChars = szEndPtr - szStart;

	nChars = min(nChars, sizeof(sszBuff)-1); // don't overrun buff!
	ASSERT(nChars);
	strncpy(sszBuff, szStart, nChars);
	sszBuff[nChars] = '\0';
	*pszNextStart = szEndPtr;
	return sszBuff;
}

char* GetToken(char *szStart, char **pszNextStart, const char *szSeps /* = ",.)" */, char **pszCurStart /* = NULL */)
{
	return GetToken2(szStart, pszNextStart, szSeps, szSeps, pszCurStart);
}


BOOL bForEachWord(char *szLine, BOOL (*pfn)(char *, void *, DWORD), void *pvClientData, DWORD dwClientData, char *szSep, BOOL bDoubleQuotes /*=FALSE*/)
{
	BOOL	bRet = FALSE;
	char	*szWord;
	char	szSepTmp[32];	// should be big enough

	if (bDoubleQuotes)
	{
		strcpy(szSepTmp, szSep);
		strcat(szSepTmp, "\"");
	}

	while (TRUE)
	{
		if (bDoubleQuotes && *szLine == '\"')
		{
			szWord = GetToken1(szLine, &szLine, szSepTmp, NULL, FALSE /*bSkipInitialSeps*/);
			if (*szLine == '\"')	// skip the terminating double quote
			{
				szLine++;
				strcat(szWord, "\"");
			}
		}
		else
			szWord = GetToken(szLine, &szLine, szSep);
		if (!szWord)
			break;
		bRet |= (*pfn)(szWord, pvClientData, dwClientData);
	}

	return bRet;
}


// --- talk-to decoder (protsupp.cpp:1066-1101; R19 LookupPui hoist) ----------

void GetTalkTos(CChatDoc *doc, CUserInfo *talkerPui, char *str, PFNLOOKUPPUI pfnLookupPui)
{
	// REGISB: 11/13/97 new m_udi in this function
	talkerPui->m_udi.m_talkTos.RemoveAll();
	while (TRUE)
	{
		while (isspace(*str))
			str++;
		if (*str == ')' || *str == '\0')
			return;
		char *szName = GetToken(str, &str);
		if (!szName)
			return;
		CUserInfo *pui = pfnLookupPui(szName, doc);
		// R13 (see panel.cpp:316-322 for the full rationale): original
		// `(DWORD) pui` assumed 32-bit pointers; the via-uintptr_t cast is
		// the minimal standard-conforming rewrite clang requires on LP64.
		if (pui)
			talkerPui->m_udi.m_talkTos.Add((DWORD)(uintptr_t) pui);
	}
}


void GetTalkTos(CChatDoc *doc, CDWordArray *talkTos, char *str, PFNLOOKUPPUI pfnLookupPui)
{
	while (TRUE)
	{
		while (isspace(*str))
			str++;
		if (*str == '\0')
			return;
		char *szName = GetToken(str, &str, ",");
		if (!szName)
			return;
		CUserInfo *pui = pfnLookupPui(szName, doc);
		if (pui)  // R13 (see above): via-uintptr_t cast for LP64
			talkTos->Add((DWORD)(uintptr_t) pui);
	}
}


// --- addressee-string builders (verbatim, protsupp.cpp:3006-3034) ----------
// g_rgpuiWhisperees (original: protsupp.cpp:69, file-scope global) is owned
// here -- same scope/ownership as the original, just re-homed to this
// partial-lift file since the session-management functions that populate it
// (bSendWhispers, slash-command /whisper, ...) are not lifted this task. It
// stays empty until Task 7 wires real whisper-target tracking; the codec
// functions that read it (GetWhisperedAddressees, and bInsertAnnotations's
// BM_WHISPER branch) are correct against an empty array (0 addressees).
CPtrArray g_rgpuiWhisperees;

// R13/R19 deviation (see PFNRESOLVETALKTO in protsupp.h): the original's
// `(CUserInfo *)(pui->m_udi.m_talkTos[i])` direct DWORD->pointer widening
// (verbatim below in the ULONG `p` local, kept for fidelity but no longer
// used to build the pointer) is replaced with pfnResolve(key) — the original
// cast is only sound when DWORD and pointers are the same width (true on the
// original's 32-bit Win32 target, false on this LP64 port).
void GetAddressees(CUserInfo *pui, const char *szSeparator, CString &str, BOOL bUseNick, PFNRESOLVETALKTO pfnResolve)
{
	// REGISB: 11/13/97 new m_udi in this function
	int iUpperBound = min(pui->m_udi.m_talkTos.GetUpperBound(), 4); // clip at first 4 (so don't overrun output buff)

	for (int i = 0; i <= iUpperBound; i++)
	{
		ULONG		p = pui->m_udi.m_talkTos[i];
		CUserInfo*	pui2 = pfnResolve(p);
		const char*	szNickname = bUseNick ? pui2->GetName() : pui2->GetScreenName();
		str += szNickname;
		if (i != iUpperBound)
			str += szSeparator;
	}
}


void GetWhisperedAddressees(const char *szSeparator, CString &str)
{
	int iUpperBound = min(g_rgpuiWhisperees.GetUpperBound(), 4); // clip at first 4 (so don't overrun output buff)

	for (int i = 0; i <= iUpperBound; i++)
	{
		const char *szNickname = ((CUserInfo *) g_rgpuiWhisperees[i])->GetName();
		str+= szNickname;
		if (i != iUpperBound)
			str+= szSeparator;
	}
}


// --- annotation encoder (protsupp.cpp:3057-3099) ----------------------------
// Deviation 1 (see file header): `av` was `CAvatarX* av = MyAvatar();` in the
// original; hoisted to an explicit parameter. Note the original declared
// this `static` (internal linkage, file-private helper) -- not `static` here
// since this partial-lift file is the ONLY definition and later tasks
// (Task 4's outbound builder) call it from other translation units.
BOOL bInsertAnnotations(CAvatarX *av, CUserInfo *puiSelf, char *szBuff, USHORT uModes, BOOL bIncludeParenthesis, PFNRESOLVETALKTO pfnResolve)
{
	CHAR		faceIndex, torsoIndex;
	BYTE		faceEmotion, faceIntensity, torsoEmotion, torsoIntensity;
	BYTE		bbRequested;
	CEmotion	face, torso;

	if (av && puiSelf)
	{
		av->GetIndices(faceIndex, torsoIndex, bbRequested);
		av->GetEmotions(face, torso);
		BYTE faceIndexByte = IndexToByte(faceIndex);
		BYTE torsoIndexByte = IndexToByte(torsoIndex);
		BYTE modeByte = IndexToByte(BM2SM(uModes));
		EmotionToBytes(face, faceEmotion, faceIntensity);
		EmotionToBytes(torso, torsoEmotion, torsoIntensity);

		sprintf(szBuff, "%s#%c%c%c%c%c%c%c%c%s%c%c",
				bIncludeParenthesis ? "(" : "",
				CGESTUREPREFIX, torsoIndexByte, torsoEmotion, torsoIntensity,
				CEXPRESSIONPREFIX, faceIndexByte, faceEmotion, faceIntensity,
				bbRequested ? "R" : "",
				CMODEPREFIX, modeByte);
		// REGISB: 11/13/97 new m_udi in this function
		if ((uModes != BM_WHISPER && puiSelf->m_udi.m_talkTos.GetUpperBound() >= 0) ||
			(uModes == BM_WHISPER && g_rgpuiWhisperees.GetUpperBound() >= 0))
		{
			CString str = "T";
			if (uModes == BM_WHISPER)
				GetWhisperedAddressees(",", str);
			else
				GetAddressees(puiSelf, ",", str, TRUE, pfnResolve);
			strcat(szBuff, str);
		}

		if (bIncludeParenthesis)
			strcat(szBuff, ") ");
	}
	return TRUE;
}


// --- annotation decoder (protsupp.cpp:1485-1542) ----------------------------
// Deviation 2 (see file header): `theApp.m_bVIPMode` hoisted to `bVIPMode`.
// Deviation 3: LookupPui hoisted to `pfnLookupPui`, threaded through to
// GetTalkTos.
void ProcessUDIData(CChatDoc *pDoc, CUserInfo *pui, char *szData, BOOL bVIPMode, PFNLOOKUPPUI pfnLookupPui)
{
	ASSERT(szData && *szData);
	ASSERT(pui);

	char *szTmp = szData;

	pui->m_udi.Reset();

	pui->m_bbValidUDI = 0;

	if (bVIPMode && !pui->IsOperator())
		return;	// VIP's ignore messages from riffraff

	ASSERT(*szTmp == '#');

	szTmp++;

	if (*szTmp == CGESTUREPREFIX)
	{
		szTmp++;
		if (*szTmp) pui->m_udi.m_chGest  = ByteToIndex(*szTmp++);
		if (*szTmp) pui->m_udi.m_chGestE = ByteToIndex(*szTmp++);
		if (*szTmp) pui->m_udi.m_chGestI = ByteToIndex(*szTmp++);
	}

	if (*szTmp == CEXPRESSIONPREFIX)
	{
		szTmp++;
		if (*szTmp) pui->m_udi.m_chExpr  = ByteToIndex(*szTmp++);
		if (*szTmp) pui->m_udi.m_chExprE = ByteToIndex(*szTmp++);
		if (*szTmp) pui->m_udi.m_chExprI = ByteToIndex(*szTmp++);
	}

	if (*szTmp == CREQUESTEDPREFIX)
	{
		szTmp++;
		pui->m_udi.m_bbReq = 1;
	}

	if (*szTmp == CMODEPREFIX)
	{
		szTmp++;
		if (*szTmp)
			pui->m_udi.m_uModes = SM2BM(ByteToIndex(*szTmp++));
	}

	if (*szTmp == CTALKTOPREFIX)
	{
		szTmp++;
		GetTalkTos(pDoc, &(pui->m_udi.m_talkTos), szTmp, pfnLookupPui);
	}

	if (pui->m_udi.m_chGestI != -1 && pui->m_udi.m_chExprI != -1)
		pui->m_udi.m_bbCooked = 1;

	pui->m_bbValidUDI = 1;	// ready for next text message
}


// --- key-string codec (verbatim, protsupp.cpp:5073-5260) --------------------
// The following functions deal with key strings. These strings take the form
// 			key1=value1;key2=value2;...
// A value can optionally be in quotes - these quotes are stripped out.
// A value itself can't have any quotes in it.
// Examples of key strings:
//				msg=hello;id=200
//				msg="hello; what is your name";id=200

// Find entries in a key string (see above for a description). If pszKey is NULL,
// just returns the first key-value pair (like an enumeration).
static BOOL
FindInKeyString(
LPCSTR pszKeyString,
LPCSTR pszKey,
int	*  pnKeyPos,
int *  pnValPos,
int *  pnNextValPos)
{
	int nKeyLength = pszKey != NULL ? lstrlen (pszKey) : 0;

	int nPos = 0;
	BOOL bFound = FALSE;
	int nIncr;

	while (!bFound && *pszKeyString != '\0') {
		if (pszKey == NULL || (!strncmp (pszKeyString, pszKey, nKeyLength) &&
				pszKeyString[nKeyLength] == '='))
		{
			if (pszKey == NULL) {
				LPCSTR psz = OurMbsChr (pszKeyString, '=');
				if (psz == NULL) {
					return FALSE;
				}
				nKeyLength = (int)(psz - pszKeyString);
			}
			*pnKeyPos = nPos;
			*pnValPos = *pnKeyPos + nKeyLength + 1;
			bFound = TRUE;
		}
		// Skip to next one.
		LPCSTR psz = OurMbsChr (pszKeyString, '=');
		if (psz != NULL) {
			psz++;
			if (*psz == '\"') {
				psz = OurMbsChr (psz + 1, '\"');
				if (psz != NULL) {
					psz++;
				}
			}
			if (psz != NULL && (psz = OurMbsChr (psz, ';')) != NULL) {
				nPos += (int)((psz + 1) - pszKeyString);
				pszKeyString = psz + 1;
				continue;
			}
		}

		int nLen = lstrlen (pszKeyString);
		nPos += nLen;
		pszKeyString += nLen;
	}

	// DISCOVERED PRE-EXISTING BUG (not introduced by this lift): the original
	// wrote `pszKeyString[nPos]` here. `pszKeyString` is a LOCAL copy of the
	// parameter that the "skip to next" logic above already advances by the
	// SAME cumulative delta accumulated into `nPos` (every `continue` moves
	// both together, in lockstep, from the original string's start) -- so by
	// the time this line runs, `pszKeyString` already points at the absolute
	// offset `nPos` names. Indexing `pszKeyString[nPos]` therefore double-
	// counts that offset, reading `nPos` bytes PAST where `pszKeyString`
	// already sits: verified with a standalone reproduction + AddressSanitizer
	// (a real out-of-bounds global-buffer-overflow read, not merely UB in
	// theory) for the exact case "msg=hello;id=200" / key "msg" (reads at
	// local offset 10+10=20 into a 17-byte string). Likely silently harmless
	// on the original's 32-bit Win32 build, where CString buffers reserved
	// extra capacity beyond nMaxSize (ChangeKeyString's own caller-supplied
	// cap) that this over-read almost always landed inside; this port's
	// tightly-sized CString backing store has no such headroom, so the exact
	// same bytes crash. Fix: check `*pszKeyString` (the already-advanced
	// pointer) instead of re-indexing it by `nPos` -- this is the evident
	// original intent ("is there anything left after what we just found"),
	// confirmed to reproduce byte-identical *pnNextValPos values to the
	// original in every non-crashing case (hand-verified: found-first-of-N,
	// found-last-of-N, found-only-entry, quoted-value-containing-';', single
	// standalone repro program, cross-checked against GetValueFromKeyString's
	// trailing-';' trim step which depends on this value being exactly
	// right). This is the one exception to "verbatim" in this file --
	// flagged prominently in the task report as a discovered defect
	// requiring reviewer attention, not silently patched over.
	if (bFound) {
		*pnNextValPos = (*pszKeyString == '\0') ? -1 : nPos;
	}
	return bFound;
}

// Makes changes to a key string (see above for a description)
// If pszValue is NULL or empty, the key is deleted
// If the change would make the string longer than nMaxSize, this function fails.

BOOL
ChangeKeyString(
CString &strKeyString,
LPCSTR pszKey,
LPCSTR pszValue,
int nMaxSize)
{
	ASSERT (pszKey != NULL && *pszKey != '\0');
	ASSERT (OurMbsPbrk (pszKey, "=;\"") == NULL);
	CString strNew = strKeyString;

	int nKeyPos, nValPos, nNextValPos;
	if (FindInKeyString (strNew, pszKey, &nKeyPos, &nValPos, &nNextValPos)) {
		if (nNextValPos == -1) {
			strNew = strNew.Left (nKeyPos - 1); // Take out trailing ; if there is one
		}
		else {
			strNew = strNew.Left (nKeyPos) + strNew.Mid (nNextValPos);
		}
	}

	if (pszValue == NULL || *pszValue == '\0') {
		// Deletion has been done.
		return TRUE;
	}

	if (OurMbsChr (pszValue, '\"') != NULL) {
		return FALSE;
	}

	int nOrigLength = strNew.GetLength ();
	BOOL bNeedQuoting = OurMbsPbrk (pszValue, "=;\"") != NULL;
	int nAddedLength = lstrlen (pszKey) + lstrlen (pszValue) +
							(bNeedQuoting ? 2 : 0) +
							(nOrigLength > 0 ? 1 : 0); // 1 for the semicolon
	if (nOrigLength + nAddedLength > nMaxSize) {
		return FALSE;
	}

	CString strPair;
	if (bNeedQuoting) {
		strPair.Format ("%s=\"%s\"", pszKey, pszValue);
	} else {
		strPair.Format ("%s=%s", pszKey, pszValue);
	}
	// R9 (Plan 3 Task 3): the shim's CString provides only operator+(const
	// CString&, const CString&), not MFC's extra operator+(char, const
	// CString&) overload; the original's `strPair = ';' + strPair;` becomes
	// an explicit CString(";") + strPair, same resulting value.
	if (nOrigLength > 0) {
		strPair = CString(";") + strPair;
	}

	strKeyString = strNew + strPair;
	return TRUE;
}

// Gets a value from a key string (see above for a description)
// If the key doesn't exist, returns FALSE.

BOOL
GetValueFromKeyString(
LPCSTR pszKeyString,
LPCSTR pszKey,
CString &strValueOut)
{
	if (pszKeyString == NULL) {
		return FALSE;
	}

	ASSERT (pszKey != NULL && *pszKey != '\0');
	ASSERT (OurMbsPbrk (pszKey, "=;\"") == NULL);

	int nKeyPos, nValPos, nNextValPos;
	if (!FindInKeyString (pszKeyString, pszKey, &nKeyPos, &nValPos, &nNextValPos)) {
		return FALSE;
	}

	if (nNextValPos == -1) {
		strValueOut = CString (pszKeyString + nValPos);
	}
	else {
		strValueOut = CString (pszKeyString + nValPos, nNextValPos - nValPos);
	}

	// Note: For multi-byte character support, we check the last character of a
	// string by using _mbsrchr.

	int nLeftTrim, nRightTrim;
	nLeftTrim  = 0;
	nRightTrim = strValueOut.GetLength ();

	// Trim trailing separators.
	while (nRightTrim > 0 && (LPCSTR)_mbsrchr ((const UCHAR *)(LPCSTR)strValueOut, ';') ==
		   ((LPCSTR)strValueOut) + nRightTrim - 1) {
		nRightTrim--;
	}

	// Trim quotes if they exist.
	// R9 (Plan 3 Task 3): CString has no operator[] in this shim; the
	// original's `strValueOut[0]` (MFC provides operator[]) becomes GetAt(0),
	// which is CString's documented equivalent (both mfc_compat.h and real
	// MFC define operator[] as `return GetAt(idx)`).
	if (nRightTrim >= 2 && strValueOut.GetAt(0) == '\"' &&
			OurMbsRChr (strValueOut, '\"') == ((LPCSTR)strValueOut) + nRightTrim - 1) {
		nLeftTrim++;
		nRightTrim--;
	}

	strValueOut = strValueOut.Mid (nLeftTrim, nRightTrim - nLeftTrim);
	return TRUE;
}

// Enumerates entries in a key string.

BOOL
EnumKeyString(
LPCSTR &pszKeyString,
CString &strKey,
CString &strValue)
{
	if (pszKeyString == NULL) {
		return FALSE;
	}

	int nKeyPos, nValPos, nNextValPos;
	if (!FindInKeyString (pszKeyString, NULL, &nKeyPos, &nValPos, &nNextValPos)) {
		return FALSE;
	}
	strKey = CString (pszKeyString, nValPos - nKeyPos - 1);
	if (!GetValueFromKeyString (pszKeyString, strKey, strValue)) {
		return FALSE;
	}
	pszKeyString = (nNextValPos == -1) ? NULL : pszKeyString + nNextValPos;
	return TRUE;
}


// =============================================================================
// Plan 3 Task 6: the payload SECOND STAGE.
//
// PROVENANCE / FIDELITY DIFF (report cites this comment): lifts OnTextMsg
// (original :4358-4371), OnDataMsg (:4374-4395), ProcessSay's codec+CTCP-
// dispatch core (:1545-1919, MINUS the R20-dropped ignore/flood/rules/history
// pieces enumerated below), ProcessComment's "#"-grammar core (:846-1020,
// MINUS the R20-dropped policy/avatar-download-action pieces), IdentifyWhispers
// (:1448-1467), and the CTCP-dispatch table's ACTION/SOUND/AWAY branches
// (:1626-1869, the only three of the ten CTCP verbs that map to an event in
// the Task-5a union -- see the per-verb table in the task report).
//
// UNIFICATION (state-and-codec.md §3.2, the brief's Step 3 requirement): the
// original had the "#G..E..[R]M.[T..]" grammar hand-inlined a SECOND time
// inside ProcessSay (:1566-1612), byte-for-byte identical to ProcessUDIData's
// body (:1485-1542) except for writing into `pui->m_udi` via a local `szStart`
// instead of `szTmp`, and a private-message anti-spoof mask ProcessUDIData's
// call site never needed. This port collapses BOTH call sites onto the ONE
// decoder already lifted in Task 3 -- ProcessUDIData itself: OnDataMsg's UDI
// branch calls it directly (as the original did); ccOnTextMsgInline (below)
// -- the plain-IRC "(#...) text" trigger -- calls the SAME ProcessUDIData
// against a scratch CUserInfo, then applies the anti-spoof mask as a
// POST-step (identical net effect to the original's inline mask, applied
// where the original applied it: right after the M-group parse, before T).
// There is now exactly ONE annotation-body decoder in this codebase used by
// BOTH the IRCX DATA path and the plain-IRC inline path: ProcessUDIData.
// (ircsock.cpp's OWN pre-Task-6 standalone decoder, ccDecodeAnnotationBody/
// ccSplitInlineAnnotations, added as a Task-5b placeholder specifically
// because Task 6 hadn't landed yet -- see ircsock.cpp's SECTION 5 comment,
// "This mirrors the grammar... Task 6's job" -- is RETIRED by this task; its
// two callers in HandleCommand are rewired to call OnTextMsg/OnDataMsg
// instead, per Step 5 below. That retires the THIRD copy the brief's "3
// original locations" refers to: ProcessUDIData (Task 3), the ProcessSay
// inline block (original, never separately lifted), and ccDecodeAnnotationBody
// (Task 5b's stand-in for the not-yet-lifted Task 6 code) all collapse onto
// this ONE call: ProcessUDIData.
//
// WHY A SCRATCH CUserInfo, NOT A SESSION USER TABLE: Task 3's discovery
// (state-and-codec.md §1.4) already established the engine does not own a
// live per-nick CUserInfo table -- Swift does. ProcessUDIData/GetTalkTos are
// PARAMETERIZED (Task 3, R19-stubbed) to take any CUserInfo* target and any
// PFNLOOKUPPUI resolver; a stack-local CUserInfo used purely as "the codec's
// scratch destination for this one message" (exactly what cc_test_decode_udi
// already does in cc_selftest.cpp) is the correct, minimal instantiation --
// it never leaks past this call, never gets a nick assigned, and is not a new
// "session user table" of any kind. This is the same reasoning Task 3's own
// test rig already used; Task 6 just makes it the REAL (non-test) call site.
//
// R19 RESOLVER WIRING: PFNRESOLVENICKREF (protsupp.h) is the payload stage's
// own resolver shape (nick,room_token)->cc_user_ref, matching
// ccSessionResolveUser exactly (bridge/cc_session.h). Internally it is
// adapted to Task 3's PFNLOOKUPPUI (nick,CChatDoc*)->CUserInfo* shape via
// ccPayloadLookupAdapter below: ccSessionResolveUser's cc_user_ref return
// value (an opaque uint32_t, never a real pointer -- see comicchat.h's
// cc_user_ref doc comment) is round-tripped through the SAME "test registry"
// technique cc_selftest.cpp's cc_test_resolve_talkto already established
// (Task 3): a small process-wide table mapping resolved cc_user_ref keys to
// scratch CUserInfo* stand-ins, so GetTalkTos's `talkerPui->m_udi.m_talkTos.
// Add((DWORD)(uintptr_t)pui)` line (verbatim, Task 3) stores something
// GetAddressees-shaped callers can round-trip -- but Task 6's OWN callers
// (ccPayloadFromUdi below) never re-resolve those DWORDs back into pointers;
// they read pui->m_udi.m_talkTos as opaque keys and hand them to the SAME
// ccSessionResolveUser call a second time is unnecessary -- see
// ccPayloadFromUdi's comment for exactly how the decoded talkTos array
// becomes the event's addressee strings without ever dereferencing a
// reconstructed pointer (only ccPayloadLookupAdapter's OWN table, populated
// by itself, is ever dereferenced -- never a bare cc_user_ref widened
// directly, preserving the LP64 safety Task 3's GetAddressees deviation-4
// already established).
// =============================================================================

// --- R19 resolver adapter: PFNRESOLVENICKREF -> PFNLOOKUPPUI ---------------
// GetTalkTos/ProcessUDIData (Task 3) call their resolver as
// CUserInfo*(*)(const char*, CChatDoc*) and store the result via
// (DWORD)(uintptr_t)pui in talkTos. Task 6's own resolver is
// cc_user_ref(*)(const char*, uint32_t room_token) (ccSessionResolveUser's
// shape). ccPayloadLookupAdapter bridges the two: it calls the Task-6
// resolver, and if it returns a non-NONE ref, vends a STABLE scratch
// CUserInfo* for that ref (creating one the first time that ref is seen this
// call, freed by the caller after the decode -- see ccPayloadTalkToPool
// below), so the DWORD GetTalkTos stores really is a dereferenceable pointer
// for the lifetime of this one message's processing, exactly like Task 3's
// cc_test_resolve_talkto/g_talkToRegistry test rig, just wired to the REAL
// resolver instead of a test table.
namespace {
struct ccPayloadTalkToPool {
    static const int CAP = 8;
    CUserInfo* entries[CAP] = {};
    cc_user_ref refs[CAP] = {};
    int count = 0;
    CUserInfo* vend(cc_user_ref ref) {
        for (int i = 0; i < count; i++) if (refs[i] == ref) return entries[i];
        if (count >= CAP) return nullptr;
        CUserInfo* pui = new CUserInfo();
        refs[count] = ref;
        entries[count] = pui;
        count++;
        return pui;
    }
    void clear() {
        for (int i = 0; i < count; i++) delete entries[i];
        count = 0;
    }
};
// Thread-local-free (single-threaded engine contract, matching ccSession()'s
// own file-static g_session pattern) -- reset at the start of every
// OnTextMsg/OnDataMsg call, so no state survives across messages.
ccPayloadTalkToPool g_payloadTalkToPool;
PFNRESOLVENICKREF g_payloadResolve = nullptr;
uint32_t g_payloadRoomToken = 0;

CUserInfo* ccPayloadLookupAdapter(const char* szNickname, CChatDoc* /*doc*/) {
    if (!g_payloadResolve || !szNickname) return nullptr;
    cc_user_ref ref = g_payloadResolve(szNickname, g_payloadRoomToken);
    if (ref == CC_USER_REF_NONE) return nullptr;
    return g_payloadTalkToPool.vend(ref);
}

// Reverse direction: given a talkTos DWORD key (as GetTalkTos stored it --
// (DWORD)(uintptr_t) of a ccPayloadLookupAdapter-vended CUserInfo*), find
// which of THIS pool's slots it is, and hand back the cc_user_ref that
// produced it (NOT the nick -- Task 6 has no nick string cached against a
// ref; the event's addressee list is populated from the SAME wire nick
// tokens ProcessUDIData's GetTalkTos already tokenized, read straight back
// off the source string by ccPayloadFromUdi -- see there).
cc_user_ref ccPayloadPoolRefForKey(DWORD key) {
    for (int i = 0; i < g_payloadTalkToPool.count; i++)
        if ((DWORD)(uintptr_t)g_payloadTalkToPool.entries[i] == key)
            return g_payloadTalkToPool.refs[i];
    return CC_USER_REF_NONE;
}
} // namespace

// Convert a fully-decoded CUserDisplayInfo (pui->m_udi, after ProcessUDIData
// or the inline-block equivalent has run) into the wire-facing cc_annotations
// value (comicchat.h) the event union carries. addresseeSource, if non-NULL,
// is the ORIGINAL wire text of the T-group (already consumed by GetTalkTos --
// re-tokenized here, verbatim GetToken() calls, purely to recover the nick
// STRINGS for the event; cc_annotations.addressees[] are encoded nick
// strings by design, see comicchat.h -- Task 5a's C-boundary deliberately
// does not carry resolved refs across the wire, Swift re-resolves display
// names itself). Every ref ccPayloadLookupAdapter vended during this decode
// is still reachable via ccPayloadPoolRefForKey for a test to verify
// end-to-end resolution (see cc_selftest.cpp's Task 6 vectors) -- production
// callers don't need it since the string form is what the event carries.
static void ccPayloadUdiToAnnotations(const CUserDisplayInfo &udi, const char* addresseeSource, cc_annotations* out) {
    memset(out, 0, sizeof(*out));
    out->gesture_pose = udi.m_chGest;
    out->gesture_emotion = udi.m_chGestE;
    out->gesture_intensity = udi.m_chGestI;
    out->face_pose = udi.m_chExpr;
    out->face_emotion = udi.m_chExprE;
    out->face_intensity = udi.m_chExprI;
    out->requested = udi.m_bbReq;
    out->mode = BM2SM(udi.m_uModes);
    out->cooked = udi.m_bbCooked;
    out->addressee_count = 0;
    if (addresseeSource) {
        char buf[256];
        strncpy(buf, addresseeSource, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char *next, *tok = buf;
        while (out->addressee_count < CC_MAX_ADDRESSEES) {
            char* name = GetToken(tok, &next, ",");
            if (!name) break;
            strncpy(out->addressees[out->addressee_count], name, sizeof(out->addressees[0]) - 1);
            out->addressees[out->addressee_count][sizeof(out->addressees[0]) - 1] = '\0';
            out->addressee_count++;
            tok = next;
        }
    }
}

// Forward declarations (definitions follow ccProcessSay below, matching the
// original file's own ordering -- PrepareTextAction/PrepareComicsAction/
// PrepareSound/IdentifyWhispers all precede ProcessSay at :1104-1467/1545 in
// the original; here ccProcessSay is presented first for readability and
// these four are declared ahead of it).
static char* ccPrepareTextAction(const char* szNickname, char *szMesg, CString &strNewMesg, USHORT &uModes);
static char* ccPrepareComicsAction(const char* szNickname, char *szMesg, CString &strNewMesg);
static char* ccPrepareSound(const char* szNickname, char *szMesg, CString &strNewMesg, USHORT &uModes, CString &outFile);
static void ccIdentifyWhispers(CUserInfo* pui, BYTE msgType, USHORT &uModes, CDWordArray *talkTos, PFNRESOLVENICKREF pfnResolve, uint32_t room_token);

// --- ProcessComment's "#"-grammar core (protsupp.cpp:846-1020 original) ----
// R20 boundary (each listed individually in the task report): the original
// interleaves EVERY branch with `!pui->Ignored() && !pui->IsFlooding()`
// gates, `pui->IsOperator()` checks (background-drop authorization),
// avatar-download state machine (`NeedsDownload`/`SetUserAvatarRealInfo`),
// and history/UI side effects (`AddAndExecute(new ChangeAvatarEntry(...))`,
// `AddAndExecute(new GetInfoEntry(...))`). NONE of that state exists in the
// engine (no live CUserInfo, no CChatDoc, no history list) -- every one of
// those checks is DROPPED (R20), not merely stubbed true/false, because
// there is no engine-side flag to evaluate them against; Swift owns the real
// ignore/flood/operator state and may suppress the emitted event itself.
// Grammar branches with NO event in the Task-5a union (HeresInfo probe-reply
// echo, BDrop/BDrop2 backdrop-change announcements) are R20-dropped outright
// (ccPayloadHandledNoEvent) -- listed individually in the task report; "#
// Appears as" maps to an event (CC_EV_APPEARS_AS), and Plan 4b Batch C added
// a second: "# GetInfo" now maps to CC_EV_INFO_REQUEST (Tim opted back in --
// see that branch's own doc comment below for the un-suppression detail).
static ccPayloadResult ccProcessComment(char *szMesg) {
    ccPayloadResult r; memset(&r, 0, sizeof(r));
    // Default to ccPayloadSuppressed (NOT MATCHED, "return FALSE" in the
    // original -> OnTextMsg falls through to ProcessSay, protsupp.cpp:4368).
    // Every recognized prefix branch below sets ccPayloadHandledNoEvent
    // (MATCHED, "return TRUE") instead before returning -- this is the fix
    // for the fidelity bug caught by cc_selftest_pv_comment_grammar_
    // suppressed during Task 6 development: the original's `return TRUE`
    // STOPS dispatch (ProcessSay never runs for a matched-but-silent
    // comment), so a matched prefix must not merely emit no event, it must
    // also prevent the fallthrough that would otherwise re-emit the raw
    // "#..." text as a CC_EV_TEXT say.
    r.cls = ccPayloadSuppressed;

    // Low Level Unquoting for \r \n (protsupp.cpp:848, verbatim first line).
    bLowLevelUnquoting(g_chLLQuoteCTCP, TRUE /*bTreatAsByteArray*/, szMesg, szMesg);

    ASSERT(*szMesg == '#');
    szMesg++;  // nuke the crosshatch (protsupp.cpp:851)

    if (!strncmp(szMesg, APPEARSPREFIX, g_nAppearsAsLen)) {
        // "# Appears as <name>.<url>" (protsupp.cpp:854-900). R20-dropped:
        // the ComicUser()-flag flip, the NeedsDownload()/SetUserAvatarRealInfo
        // interactive-download branch, and the AddAndExecute(ChangeAvatarEntry)
        // history entry -- all live-CUserInfo/CChatDoc state this engine
        // doesn't hold. The grammar itself (name/url tokenization) is
        // verbatim: GetToken (whitespace-terminated) then GetToken2(".,)",",)")
        // exactly as the original.
        char *szVar = szMesg + g_nAppearsAsLen;
        char *szCharName = GetToken(szVar, &szVar);
        // djk - BETA1 Fix (verbatim guard). The original returns FALSE here
        // (protsupp.cpp:861: `if (!szCharName) return FALSE;`), which sends
        // the caller (OnTextMsg's `!ProcessComment(...)` check) on to
        // ProcessSay -- the raw "# Appears as" text still renders as a say
        // when the avatar name token is missing. R18/R20 control-flow
        // correction (review fix): this port previously returned
        // ccPayloadHandledNoEvent here (MATCHED, swallows the text with no
        // event), which is the wrong return class -- ccPayloadSuppressed is
        // this port's equivalent of the original's FALSE/fall-through-to-say.
        if (!szCharName) { r.cls = ccPayloadSuppressed; return r; }
        char nameBuf[128];
        strncpy(nameBuf, szCharName, sizeof(nameBuf) - 1);
        nameBuf[sizeof(nameBuf) - 1] = '\0';
        char *szCharURL = GetToken2(szVar, &szVar, ".,)", ",)");
        r.cls = ccPayloadAppearsAs;
        r.avatarName = nameBuf;
        r.avatarUrl = szCharURL ? szCharURL : "";
        return r;
    }

    if (!strncmp(szMesg, GETINFOPREFIX, strlen(GETINFOPREFIX))) {
        // "# GetInfo" (protsupp.cpp:902-924): original replies with
        // "# HeresInfo: <profile>" over the wire (bChatSendPrivMesg).
        // Plan 4b Batch C (R18-style emit addition, opted in by Tim):
        // un-suppressed from ccPayloadHandledNoEvent -- CC_EV_INFO_REQUEST
        // now carries the probe through to Swift, which replies via
        // cc_session_send_info_reply (comicchat.h) with the user's own
        // profile text. Still MATCHED (original returns TRUE) -> no
        // fallthrough to ProcessSay; only the "no event" half of the old
        // comment is no longer true.
        r.cls = ccPayloadInfoRequest;
        return r;
    }

    if (!strncmp(szMesg, REQUESTCHARPREFIX, g_nGetCharLen)) {
        // "# GetCharInfo" (protsupp.cpp:926-939): original replies by
        // re-announcing our avatar with the URL. Same R20 reasoning as
        // GetInfo above (outbound reply, no matching inbound event).
        r.cls = ccPayloadHandledNoEvent;
        return r;
    }

    if (!strncmp(szMesg, HERESINFOPREFIX, g_nHeresInfoLen)) {
        // "# HeresInfo: <profile>" (protsupp.cpp:941-962): original only
        // acts `if (pui->IsRequestInfo(RF_PROFILE))` (a live-pui request
        // counter this engine doesn't hold) then AddAndExecute's a history
        // entry. R20: no engine-side request-counter to gate on, no history
        // list to write to. MATCHED either way (original returns TRUE
        // unconditionally after the if/else) -> no fallthrough.
        r.cls = ccPayloadHandledNoEvent;
        return r;
    }

    if (!strncmp(szMesg, BACKGRNDPREFIX, strlen(BACKGRNDPREFIX))) {
        // "# BDrop: <name>" (protsupp.cpp:964-983): original gates on
        // `pui->IsOperator()` (a live-pui flag) then AddAndExecute's a
        // ChangeBackDropEntry. R20: no operator flag to check, no backdrop
        // history to write. (CC_EV_ROOM_PROP already carries PROP CLIENT
        // bk= changes via ccEmitClientDataChange, ircsock.cpp -- the
        // IRCX-native path for backdrop sync; this "# " form is the legacy
        // plain-IRC announcement of the same fact and is not re-plumbed to
        // an event this task, per R20). MATCHED -> no fallthrough.
        r.cls = ccPayloadHandledNoEvent;
        return r;
    }

    if (!strncmp(szMesg, NEWBACKGRNDPREFIX, strlen(NEWBACKGRNDPREFIX))) {
        // "# BDrop2: <name>[,<url>]" (protsupp.cpp:988-1017): same R20
        // reasoning as BDrop above (operator-gated backdrop announcement).
        // MATCHED -> no fallthrough.
        r.cls = ccPayloadHandledNoEvent;
        return r;
    }

    return r;  // no comment prefix matched -> FALSE in the original
}

// --- CTCP dispatch core, extracted from ProcessSay (protsupp.cpp:1545-1919) -
// R20 boundary (each listed individually in the task report):
//   * `AcceptWhispers()`/`theApp.m_bVIPMode` gates (:1553-1560) -- app-policy
//     flags this engine doesn't hold. DROPPED: classification always
//     proceeds; Swift may suppress after the fact.
//   * Every `!pui->Ignored() && !pui->IsFlooding()` gate around ACTION/SOUND
//     (:1628, 1641, 1651) -- same reasoning, DROPPED.
//   * PING/TIME/EMAIL/URL/NETMEET/CLIENTINFO CTCP branches
//     (:1677-1810, 1858-1865) -- these are OUTBOUND-REPLY-SENDING or
//     UI-launching (ReplyPing/ReplyTime/ReplyEmail/ReplyHomePage send a wire
//     reply via GetOutBuff()/theApp session-identity globals this engine's
//     Task 1 config never modeled; ShowTime/ShowEmail/ShowHomePage/
//     DoNetMeetingCX gate on live pui request-info counters then
//     AddAndExecute a history entry or FLaunchBrowser/AfxMessageBox a UI
//     action) -- NONE of the four in-scope events (ACTION/SOUND/AWAY_PEER/
//     APPEARS_AS) cover these; R20-dropped (ccPayloadSuppressed) rather than
//     invented new event types (brief: "if you need an event shape not in
//     the union, STOP and return NEEDS_CONTEXT" -- these six CTCP verbs have
//     no in-scope event, so they are dropped, not escalated, per R20's own
//     "drop with note" option). VERSION (:1659-1667) is no longer in this
//     dropped set -- Plan 4b Batch C added CC_EV_VERSION_REQUEST to the
//     union and un-suppressed the bare-query branch specifically (Tim opted
//     in for VERSION + GetInfo only); see ccProcessSay's own VERSION-branch
//     comment below for the exact split. The original's `else` half of that
//     same branch (an argument follows the CTCP verb -- ShowVersion, a
//     UI/history action) has no in-scope event and stays R20-dropped exactly
//     as before.
//   * `fileDCCID`/`xvchatID` (:1718-1815) -- DCC file transfer (filesend.cpp,
//     explicitly out of scope this plan per the roadmap) and X-VCHAT
//     (ignored by the original itself, "ignore X-VCHAT CTCPs"). DROPPED.
//   * The "until NOTICE'ed" `\x01*` reply-collection branch (:1816-1866) --
//     these are the SAME four ShowVersion/ShowTime/ShowEmail/ShowHomePage/
//     DoNetMeetingCX UI actions reached via a different CTCP framing
//     (NOTICE'd replies to OUR OWN earlier CTCP requests) -- same R20
//     reasoning, DROPPED.
//   * The final say path (:1874-1913): `bAddToWhisperBox` (UI display
//     routing), `theApp.m_dynaRules.bMatchAndApplyRules` (rules engine,
//     twice, plus `SetCachRecipients`), `AddAndExecute(new SayEntry(...))`
//     (history). ALL DROPPED -- Swift's rules/history/display live entirely
//     outside the engine; the emitted CC_EV_TEXT/CC_EV_ACTION event IS the
//     "here's a say, you decide what to do with it" replacement for this
//     whole block.
// The three verbs that DO map to an in-scope event (ACTION at :1626-1648,
// SOUND at :1649-1658, AWAY at :1777-1794) are lifted; PrepareTextAction/
// PrepareComicsAction/PrepareSound (:1104-1123, 1382-1440) are lifted
// verbatim as codec-shaped string transforms (they don't touch pui flags,
// only pui->GetScreenName() -- read via the nick string parameter here,
// since there is no live pui) modulo the CTCPUnQuoteString deviation noted
// at PrepareSound's call site below.
static ccPayloadResult ccProcessSay(const char* szNickname, CUserInfo* pui, char *szMesg, BYTE msgType, uint32_t room_token, PFNRESOLVENICKREF pfnResolve, CDWordArray *explicitTalkTos) {
    ccPayloadResult r; memset(&r, 0, sizeof(r));
    r.cls = ccPayloadSay;
    CString strActionMesg;
    // Raw T-group wire text (comma-separated encoded nicks), captured from the
    // inline annotation block if one was present -- threaded to
    // ccPayloadUdiToAnnotations below so the event's addressees[] carries the
    // actual talk-to nick strings (cc_annotations' wire-facing string form,
    // see comicchat.h/ccPayloadUdiToAnnotations's doc comment), not an empty
    // list. Empty ("") if no inline block, or the block had no T-group.
    char talkToSrc[256]; talkToSrc[0] = '\0';

    // Low Level Unquoting for \r \n (protsupp.cpp:1563, verbatim first codec line
    // reached from ProcessSay -- the R20-dropped AcceptWhispers/VIP gates above
    // it never touch szMesg, so skipping them changes no codec behavior).
    bLowLevelUnquoting(g_chLLQuoteCTCP, TRUE /*bTreatAsByteArray*/, szMesg, szMesg);

    // --- unified inline-annotation parse (state-and-codec.md §3.2 unification;
    // see the file-header comment above for the full "3 locations -> 1"
    // account). Original :1566-1612 hand-inlined the SAME grammar
    // ProcessUDIData already implements; here we call ProcessUDIData itself
    // against `pui` (the caller's scratch CUserInfo), on the "(#...) text"
    // substring, then advance szMesg past the parenthetical exactly as the
    // original did.
    BOOL bHadInlineAnnotations = FALSE;
    if (!strncmp(szMesg, "(#", 2) && strstr(szMesg + 2, ") ")) {
        char* close = strstr(szMesg + 2, ") ");
        // ProcessUDIData ASSERTs *szTmp=='#' and wants a NUL-terminated
        // buffer -- carve the parenthetical body (between '(' and ')') into a
        // scratch buffer, matching the original's in-place szStart walk
        // (which relied on the buffer having the ") " terminator to stop at;
        // here we materialize that same substring explicitly).
        char body[256];
        size_t bodyLen = (size_t)(close - (szMesg + 1));
        if (bodyLen >= sizeof(body)) bodyLen = sizeof(body) - 1;
        strncpy(body, szMesg + 1, bodyLen);
        body[bodyLen] = '\0';

        // Capture the T-group's raw wire text (if any) BEFORE ProcessUDIData
        // consumes it via GetTalkTos's tokenizer -- ccPayloadUdiToAnnotations
        // re-derives the event's addressee STRINGS from this substring (see
        // that function's doc comment for why: cc_annotations carries
        // strings, not resolved refs).
        {
            const char* t = strchr(body, CTALKTOPREFIX);
            if (t) strncpy(talkToSrc, t + 1, sizeof(talkToSrc) - 1);
        }

        g_payloadResolve = pfnResolve;
        g_payloadRoomToken = room_token;
        // ProcessUDIData's own VIP-mode gate (bVIPMode) is R20-dropped here
        // (FALSE -- no session policy flag to read), matching this file's
        // other ProcessUDIData call sites (OnDataMsg below).
        ProcessUDIData(nullptr, pui, body, FALSE /*bVIPMode*/, ccPayloadLookupAdapter);

        // anti-spoof quirk (PRESERVE VERBATIM, protsupp.cpp:1588-1592,
        // "anti-hacker line"): on a private message, force-mask SAY/THINK to
        // WHISPER. Applied as a post-step here (ProcessUDIData already wrote
        // m_uModes); original applied it inline right after parsing the
        // M-group, before the T-group -- same net m_uModes value either way
        // since T-group parsing never touches m_uModes.
        if (msgType & MT_PRIVATEMSG) {
            pui->m_udi.m_uModes &= ~(BM_SAY | BM_THINK);  // anti-hacker line
            pui->m_udi.m_uModes |= BM_WHISPER;
        }

        szMesg = close + 2;  // advance string to end of parenthetical annotation (:1609)
        bHadInlineAnnotations = TRUE;
    } else if (!pui->m_bbValidUDI) {
        // (protsupp.cpp:1613-1622) no embedded annotation AND no pending UDI
        // from a preceding out-of-band DATA line -- reset, apply the same
        // anti-spoof default-to-WHISPER for private messages.
        pui->m_udi.Reset();
        if (msgType & MT_PRIVATEMSG) {
            pui->m_udi.m_uModes &= ~BM_SAY;
            pui->m_udi.m_uModes |= BM_WHISPER;
        }
    }
    // else: the original's comment here read "pui->m_bbValidUDI was set by a
    // preceding OnDataMsg call on this same scratch pui" -- that is
    // COUNTERFACTUAL for this port and has been corrected (review fix). Both
    // OnTextMsg and OnDataMsg (below) each construct their OWN fresh
    // stack-local `CUserInfo pui;` per call (CUserInfo's ctor zero-inits
    // m_bbValidUDI, lifted_singles.cpp:161) -- there is no shared, persistent
    // CUserInfo a preceding DATA line could have marked. So pui->m_bbValidUDI
    // is ALWAYS 0 by the time ccProcessSay reaches this branch, the `else if
    // (!pui->m_bbValidUDI)` reset above ALWAYS fires (for every plain PRIVMSG
    // with no inline "(#...)" block), and this `else` branch is dead code in
    // this port -- which is the CORRECT stateless contract, not a bug: the
    // original's IRCX DATA-then-PRIVMSG annotation pairing (stash on the
    // sender's persistent CUserInfo, consume on their next plain PRIVMSG) is
    // NOT reproduced in-engine. That pairing is Task 7's (Swift's)
    // responsibility -- Swift owns the real per-nick user table and re-pairs
    // a DATA CCUDI1 blob with the next plain PRIVMSG from the same nick by
    // nick lookup, once both have crossed the C boundary as separate
    // CC_EV_DATA / CC_EV_TEXT(has_annotations=0) events (plan amendment,
    // recorded against this task's review). No code-logic change here: the
    // always-reset behavior below is unchanged and correct.

    pui->m_bbValidUDI = 0;  // pui->m_udi no more valid for next incoming text (:1624)

    // --- CTCP dispatch (the three in-scope verbs; R20 table above covers
    // the rest). msgType/pui->m_udi.m_uModes drive the SAME branch structure
    // as the original (:1626-1869), minus the ignore/flood gates.
    if ((pui->m_udi.m_uModes & BM_ACTION) || !strncmp(szMesg, actionID, g_nActionLen)) {
        // ACTION (:1626-1648): either the CTCP \x01ACTION..\x01 form, or an
        // already-BM_ACTION-flagged comics action (m_udi.m_uModes set via a
        // preceding annotation block with M=5/SM_ACTION). PrepareTextAction
        // strips the CTCP framing; PrepareComicsAction just prefixes the
        // screen name -- both lifted verbatim (protsupp.cpp:1104-1123),
        // using szNickname in place of pui->GetScreenName() (no live
        // CUserInfo screen-name field on this engine's scratch object --
        // the wire nick IS the display name at this layer; Swift owns the
        // real screen-name mapping).
        char* szText;
        if (!strncmp(szMesg, actionID, g_nActionLen))
            szText = ccPrepareTextAction(szNickname, szMesg, strActionMesg, pui->m_udi.m_uModes);
        else
            szText = ccPrepareComicsAction(szNickname, szMesg, strActionMesg);
        r.cls = ccPayloadAction;
        r.text = szText;
        cc_annotations ann;
        ccPayloadUdiToAnnotations(pui->m_udi, talkToSrc[0] ? talkToSrc : nullptr, &ann);
        r.annotations = ann;
        r.hasAnnotations = bHadInlineAnnotations ? 1 : 0;
        return r;
    }

    if (!strncmp(szMesg, soundID, g_nSoundLen)) {
        // SOUND (:1649-1658, PrepareSound :1382-1440). DEVIATION (documented,
        // task report): PrepareSound's unquoted-filename branch called
        // histent.cpp's CTCPUnQuoteString (CTCP backslash-quote unescaping)
        // -- histent.cpp is out of this task's (and this plan's) scope, so
        // ccPrepareSound below skips that unescape step for the unquoted
        // form (the quoted `"..."` form, which needs no unquoting, is
        // unaffected). Rare edge case: an unquoted sound filename containing
        // embedded g_chLLQuoteIRCX-escaped bytes will carry the escape
        // sequence literally instead of being unescaped. bFindAndPlaySound
        // (actual audio playback) is R20-dropped -- Swift decides whether/how
        // to play sounds; the event just names the file.
        CString file;
        char* szText = ccPrepareSound(szNickname, szMesg, strActionMesg, pui->m_udi.m_uModes, file);
        r.cls = ccPayloadSound;
        r.text = szText;
        r.file = file;
        return r;
    }

    if (!strnicmp(szMesg, awayID, g_nAwayLen)) {
        // AWAY (:1777-1794, ShowAway :1240-1266). R20: the original's
        // `!pui->Ignored() && !pui->IsFlooding()` gate around ShowAway, and
        // ShowAway's OWN `DoUserAway(doc, pui, bAway)` (live-pui flag write)
        // + `AddAndExecute(new GetInfoEntry(...))` (history) are all
        // DROPPED -- no engine-side pui flags or history list. The grammar
        // (strip the trailing 0x01, treat empty-vs-nonempty as back/away) is
        // reproduced: an empty message after the AWAY prefix means "back"
        // (message text = ""); CC_EV_AWAY_PEER carries whichever text arrived
        // (Swift's UI decides how to phrase back-vs-away, matching the
        // original's IDS_BACKREPORT/IDS_AWAYREPORT string-resource choice --
        // a UI concern, R20).
        const char* szOffset = szMesg + g_nAwayLen + 1;
        CString strAwayMsg = szOffset;
        int iEnd = strAwayMsg.Find((char)0x01);
        if (iEnd >= 0) strAwayMsg = strAwayMsg.Left(iEnd);
        r.cls = ccPayloadAwayPeer;
        r.text = strAwayMsg;
        return r;
    }

    if (strnicmp(szMesg, versionID, g_nVersionLen) == 0) {
        // Plan 4b Batch C (R18-style emit addition, opted in by Tim):
        // un-suppressed out of the former combined VERSION/PING/TIME/.../
        // X-VCHAT ccPayloadSuppressed grouping below (this branch used to be
        // the first disjunct of that `if`; split out here so VERSION alone
        // gets an event while the other eight verbs stay exactly as
        // suppressed as before -- see this file's own R20 table comment
        // above ProcessSay, and the still-suppressed `if` immediately below).
        // Provenance note (Batch C review M-2): that combined 9-verb `||`
        // grouping was itself a Plan-3 flattening of the original's
        // else-if chain (protsupp.cpp:1659-1811, one branch per verb,
        // READ-ONLY reference) -- this split reverts toward that original
        // per-verb structure rather than introducing a new departure from it.
        // Grammar (original protsupp.cpp:1659-1667): only the BARE
        // `\x01VERSION\x01` query form (no argument text, i.e. the byte
        // right after "VERSION" is the closing 0x01) triggers a reply --
        // the original's `else` branch (an argument follows: either our own
        // earlier VERSION request's reply text arriving via ShowVersion, or
        // a malformed probe) is a UI-launching/history-writing path with no
        // in-scope event, so it stays suppressed here exactly as the
        // R20 table already documented for this whole verb before this
        // batch (that `else` half was never part of the "answer" ask).
        const char *szOffset = szMesg + g_nVersionLen;
        if (*szOffset == 0x01) {
            r.cls = ccPayloadVersionRequest;
            return r;
        }
        r.cls = ccPayloadSuppressed;
        return r;
    }

    if (strnicmp(szMesg, pingID, g_nPingLen) == 0 ||
        strnicmp(szMesg, timeID, g_nTimeLen) == 0 ||
        strnicmp(szMesg, fileDCCID, g_nFileDCCLen) == 0 ||
        strnicmp(szMesg, emailID, g_nEmailLen) == 0 ||
        strnicmp(szMesg, urlID, g_nUrlLen) == 0 ||
        strnicmp(szMesg, netMeetingID, g_nNetMeetLen) == 0 ||
        strnicmp(szMesg, clientInfoID, g_nClientInfoLen) == 0 ||
        strnicmp(szMesg, xvchatID, g_nXVChatLen) == 0 ||
        (*szMesg == 0x01 && szMesg[1] == '*')) {
        // PING/TIME/DCC/EMAIL/URL/NETMEET/CLIENTINFO/X-VCHAT + the
        // "until NOTICE'ed" reply-collection framing -- R20-dropped per the
        // function header comment (outbound-reply-sending or UI-launching,
        // no in-scope event). Also covers the original's bare
        // `*szMesg==0x01 -> goto exitCheckFlood` catch-all (any other CTCP
        // verb): same net effect, suppressed. VERSION was split out above
        // (Plan 4b Batch C) -- everything else here is untouched, still
        // documented-silent per the task brief's explicit scope fence.
        r.cls = ccPayloadSuppressed;
        return r;
    }
    if (*szMesg == 0x01) {
        // any other/unrecognized CTCP verb (:1868-1869 catch-all). Suppressed.
        r.cls = ccPayloadSuppressed;
        return r;
    }

    // --- plain say path (:1874-1913, minus rules/history/UI -- R20) --------
    if (!pui->m_udi.m_bbCooked || pui->m_udi.m_talkTos.GetUpperBound() < 0) {
        // IdentifyWhispers (protsupp.cpp:1448-1467, lifted verbatim below as
        // ccIdentifyWhispers) -- private messages with no already-decoded
        // talk-to list implicitly address ME, UNLESS the caller supplied an
        // explicit talk-to list (the WHISPER command's own target-list field,
        // ircsock.cpp:1832-1841 -- see protsupp.h's OnTextMsg doc comment).
        ccIdentifyWhispers(pui, msgType, pui->m_udi.m_uModes, explicitTalkTos, pfnResolve, room_token);
    }

    r.cls = ccPayloadSay;
    r.text = szMesg;
    cc_annotations ann;
    // talkToSrc (see declaration comment above) carries the inline block's
    // own T-group text when one was present; IdentifyWhispers's own
    // talk-to writes (explicit-list or "just me" fallback, just above) have
    // no corresponding wire-text form to recover here -- Swift already has
    // the WHISPER command's raw target-list field directly (ircsock.cpp
    // passes it as szExplicitTalkTos) if it needs the string form for that
    // case.
    ccPayloadUdiToAnnotations(pui->m_udi, talkToSrc[0] ? talkToSrc : nullptr, &ann);
    r.annotations = ann;
    r.hasAnnotations = bHadInlineAnnotations ? 1 : 0;
    return r;
}

// PrepareTextAction (protsupp.cpp:1104-1114, verbatim grammar; pui->GetScreenName()
// -> szNickname, no live CUserInfo screen name at this layer).
static char* ccPrepareTextAction(const char* szNickname, char *szMesg, CString &strNewMesg, USHORT &uModes) {
    strNewMesg = szNickname;
    strNewMesg += (szMesg + g_nActionLen);
    int iEndIndex = strNewMesg.Find((char)0x01);
    if (iEndIndex >= 0)
        strNewMesg = strNewMesg.Left(iEndIndex);
    uModes &= ~BM_SAY;
    uModes |= BM_ACTION;
    return UnConst(strNewMesg);
}

// PrepareComicsAction (protsupp.cpp:1117-1123, verbatim).
static char* ccPrepareComicsAction(const char* szNickname, char *szMesg, CString &strNewMesg) {
    strNewMesg = szNickname;
    strNewMesg += " ";
    strNewMesg += szMesg;
    return UnConst(strNewMesg);
}

// PrepareSound (protsupp.cpp:1382-1440). Deviation: the unquoted-filename
// branch's CTCPUnQuoteString call is dropped (histent.cpp out of scope --
// see ccProcessSay's SOUND-branch comment); bFindAndPlaySound (actual
// playback) is R20-dropped (Swift's decision). `outFile` receives the parsed
// filename (event payload); return value is the display text.
static char* ccPrepareSound(const char* szNickname, char *szMesg, CString &strNewMesg, USHORT &uModes, CString &outFile) {
    char *szSound = szMesg + g_nSoundLen, *szEnd;

    while (my_isspace(*szSound))
        szSound++;

    if (!*szSound) { outFile = ""; return UnConst(CString("")); }  // empty string cancels display

    BOOL bQuoted = (*szSound == '"');
    if (bQuoted) {
        szEnd = strchr(++szSound, '"');
        if (!szEnd) { outFile = ""; return UnConst(CString("")); }  // no matching quote
    } else {
        szEnd = strchr(szSound + 1, ' ');
        if (!szEnd) szEnd = strchr(szSound, 0x01);
        if (!szEnd) szEnd = strchr(szSound, '\0');
    }

    CString strFile(szSound, (int)(szEnd - szSound));
    // (deviation: CTCPUnQuoteString skipped for the unquoted form -- see
    // function header comment)
    outFile = strFile;

    if (*szEnd == '"')
        szEnd++;  // now end must be at space or end
    strNewMesg = szNickname;
    strNewMesg += szEnd;
    int iEndIndex = strNewMesg.Find((char)0x01);
    if (iEndIndex >= 0)
        strNewMesg = strNewMesg.Left(iEndIndex);

    char szResetSeq[MAX_FORMATTINGPERBYTE];
    if (nResettingSequence(szEnd, szResetSeq))
        strNewMesg += CString(szResetSeq);

    strNewMesg += " (";
    strNewMesg += (const char*)strFile;
    strNewMesg += ")";

    uModes &= ~BM_SAY;
    uModes |= BM_ACTION;

    return UnConst(strNewMesg);
}

// IdentifyWhispers (protsupp.cpp:1448-1467, lifted verbatim modulo the R19
// resolver reroute + the "doc ? doc->m_puiSelf : ExternalPui(...)" session-
// table fallback, which has no engine-side equivalent -- see below).
static void ccIdentifyWhispers(CUserInfo* pui, BYTE msgType, USHORT &uModes, CDWordArray *talkTos, PFNRESOLVENICKREF pfnResolve, uint32_t room_token) {
    if (msgType & MT_PRIVATEMSG) {
        uModes &= ~BM_SAY;
        uModes |= BM_WHISPER;
        pui->m_udi.m_talkTos.RemoveAll();
        if (talkTos) {
            int upper = talkTos->GetUpperBound();
            for (int i = 0; i <= upper; i++)
                pui->m_udi.m_talkTos.Add(talkTos->GetAt(i));
        } else {
            // Original: `doc ? doc->m_puiSelf : ExternalPui(GetMyNickName(), "", TRUE)`
            // -- both branches are session-table lookups this engine doesn't
            // own. R19: resolve OUR OWN nick (ccSessionOwnNick, via the
            // caller-supplied pfnResolve against room_token) instead -- same
            // semantic ("the implicit addressee of a private message is
            // me"), sourced from the resolver rather than a doc pointer.
            g_payloadResolve = pfnResolve;
            g_payloadRoomToken = room_token;
            CUserInfo* pUIMe = ccPayloadLookupAdapter(ccSessionOwnNick(), nullptr);
            if (pUIMe)
                pui->m_udi.m_talkTos.Add((DWORD)(uintptr_t)pUIMe);  // R13: via-uintptr_t, LP64-safe
        }
    }
}

// --- OnTextMsg / OnDataMsg (protsupp.cpp:4358-4395) -------------------------
// Both original entry points resolved a live CUserInfo* via
// PuiFromDocNickIdent (session-table lookup, not lifted -- R19 territory)
// then dispatched. Here there is no session table to resolve INTO; the
// scratch CUserInfo `pui` exists ONLY to give ProcessUDIData/GetTalkTos/
// ccProcessSay a m_udi to decode into for the duration of this one call,
// exactly like the codec's own test rig (cc_test_decode_udi). The
// CHANNELPREFIX(*szNickname) guard (both originals) is preserved verbatim --
// wire messages FROM an entire channel (rare, some servers' broadcast
// pseudo-senders) are not processed.
ccPayloadResult OnTextMsg(const char *szNickname, char *szMesg, BYTE msgType, uint32_t room_token, PFNRESOLVENICKREF pfnResolve, char *szExplicitTalkTos) {
    ccPayloadResult r; memset(&r, 0, sizeof(r));
    r.cls = ccPayloadSuppressed;
    if (!szNickname || !*szNickname || CHANNELPREFIX(*szNickname)) return r;
    if (!szMesg) return r;

    g_payloadTalkToPool.clear();
    CUserInfo pui;  // scratch decode target (see file header comment)

    if (*szMesg == '#') {
        ccPayloadResult c = ccProcessComment(szMesg);
        if (c.cls != ccPayloadSuppressed) { g_payloadTalkToPool.clear(); return c; }
        // ProcessComment returned FALSE (no "# " prefix matched a known
        // grammar) -> fall through to ProcessSay, exactly like the original's
        // `if (*szMesg != '#' || !ProcessComment(...)) ProcessSay(...)`.
    }

    // WHISPER's explicit target-list (ircsock.cpp:1832-1841 original;
    // protsupp.h's OnTextMsg doc comment) -- decode via GetTalkTos (Task 3,
    // R19-resolver-parameterized) exactly like the original's pre-computed
    // CDWordArray, using the SAME resolver adapter this file's other codec
    // call sites use.
    CDWordArray explicitTalkTos;
    CDWordArray* pExplicitTalkTos = nullptr;
    if (szExplicitTalkTos) {
        g_payloadResolve = pfnResolve;
        g_payloadRoomToken = room_token;
        GetTalkTos(nullptr, &explicitTalkTos, szExplicitTalkTos, ccPayloadLookupAdapter);
        pExplicitTalkTos = &explicitTalkTos;
    }

    ccPayloadResult say = ccProcessSay(szNickname, &pui, szMesg, msgType, room_token, pfnResolve, pExplicitTalkTos);
    g_payloadTalkToPool.clear();
    return say;
}

ccPayloadResult OnDataMsg(const char *szNickname, char *szData, BYTE msgType, uint32_t room_token, PFNRESOLVENICKREF pfnResolve) {
    ccPayloadResult r; memset(&r, 0, sizeof(r));
    r.cls = ccPayloadSuppressed;
    if (!szNickname || !*szNickname || CHANNELPREFIX(*szNickname)) return r;
    if (!szData) return r;

    g_payloadTalkToPool.clear();
    if (*(szData + 1) == ' ') {
        // "# " comment arriving via DATA instead of PRIVMSG (:4390, verbatim
        // dispatch condition) -- e.g. an IRCX "# Appears as" sent out-of-band.
        ccPayloadResult c = ccProcessComment(szData);
        g_payloadTalkToPool.clear();
        return c;
    }

    // Real UDI annotation blob (:4394, ProcessUDIData dispatch). Capture the
    // T-group's raw wire text (if any) BEFORE ProcessUDIData consumes it via
    // GetTalkTos's tokenizer, same as ccProcessSay's inline-block handling
    // above -- so the event's addressees[] carries the actual nick strings,
    // not an empty list.
    char talkToSrc[256]; talkToSrc[0] = '\0';
    {
        const char* t = strchr(szData, CTALKTOPREFIX);
        if (t) strncpy(talkToSrc, t + 1, sizeof(talkToSrc) - 1);
    }

    CUserInfo pui;
    g_payloadResolve = pfnResolve;
    g_payloadRoomToken = room_token;
    ProcessUDIData(nullptr, &pui, szData, FALSE /*bVIPMode, R20-dropped*/, ccPayloadLookupAdapter);
    r.cls = ccPayloadData;
    ccPayloadUdiToAnnotations(pui.m_udi, talkToSrc[0] ? talkToSrc : nullptr, &r.annotations);
    r.hasAnnotations = 1;
    (void)msgType;
    g_payloadTalkToPool.clear();
    return r;
}
