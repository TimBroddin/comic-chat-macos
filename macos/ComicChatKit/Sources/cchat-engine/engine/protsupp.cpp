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
