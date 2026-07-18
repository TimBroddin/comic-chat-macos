// protsupp.h — PARTIAL LIFT: codec-subset declarations only in Task 3;
// session/pipeline declarations arrive in Task 7 (see protsupp.cpp for the
// full provenance note). Original protsupp.h (v2.5-beta-1-modern/protsupp.h)
// declares ~140 functions; this header carries over ONLY the ones the codec
// subset lifted this task needs to be callable from cc_selftest.cpp /
// Task 4's outbound builders / Task 6's payload stage. Everything else
// (session state, ignore lists, dialogs, connection orchestration, ...) is
// NOT declared here -- adding it back is each later task's job, at the file
// scope that actually lifts the corresponding definitions.
#ifndef __PROTSUPP_H__
#define __PROTSUPP_H__

#include "mfc_compat.h"  // R1 (was stdafx.h): CString/CDWordArray/CPtrArray/BOOL/BYTE/USHORT

// Forward declaration only (R8): CChatDoc is a UI/document class, never
// lifted. The codec subset's signatures name it purely as an opaque pointer
// threaded through to a caller-supplied resolver -- none of these functions
// dereference it themselves (matches the original's own usage: protsupp.cpp
// passes `doc` straight through to LookupPui without touching it).
class CChatDoc;

// Forward declaration only (R8): CUserInfo is fully declared by userinfo.h
// (already lifted, Plan 2 Task 8); avoid a hard include-order dependency here
// by forward-declaring, exactly like the original's cross-file convention.
class CUserInfo;

// Forward declaration only (R8): CAvatarX is fully declared by avatar.h
// (already lifted, Plan 1); forward-declared here for the same reason.
class CAvatarX;

// --- byte packers (protsupp.cpp:1023-1063) ----------------------------------
// IndexToByte(v) = v + '0'; ByteToIndex(b) = b - '0'. These pack/unpack every
// value byte of the "#G...E...M..." annotation wire grammar
// (state-and-codec.md §3.3).
BYTE IndexToByte(BYTE byteIn);
BYTE ByteToIndex(BYTE byteIn);

// SM_* (1..5, "say mode" wire value) <-> BM_* (bitmask, in-memory m_udi.m_uModes).
USHORT SM2BM(BYTE byteMode);
BYTE BM2SM(USHORT uModes);

// --- tokenizers (protsupp.cpp:257-422) --------------------------------------
char *GetToken(char *szStart, char **pszNextStart, const char *szSeps = ",.)", char **pszCurStart = NULL);
char *GetToken1(char *szStart, char **pszNextStart, const char *szSepsEnd, char **pszCurStart = NULL, BOOL bSkipInitialSeps = TRUE);
char *GetToken2(char *szStart, char **pszNextStart, const char *szSepsBegin, const char *szSepsEnd, char **pszCurStart = NULL);
BOOL bForEachWord(char *szLine, BOOL (*pfn)(char *, void *, DWORD), void *pvClientData, DWORD dwClientData, char *szSep, BOOL bDoubleQuotes = FALSE);

// --- R19 resolver stub -------------------------------------------------------
// LookupPui (protsupp.cpp:2400s in the original -- the session-state nick
// resolver) is NOT wired here. Task 7 replaces this seam with the real
// ccSessionResolveUser() resolver (see the Plan 3 R19 rule). For this task's
// codec-only scope, GetTalkTos takes the resolver as an explicit function
// parameter instead of calling a global LookupPui -- callers (Task 6's
// ProcessUDIData caller, or this task's test wrapper) supply one. This keeps
// the codec subset link-clean with no dangling reference to session state.
typedef CUserInfo* (*PFNLOOKUPPUI)(const char *szNickname, CChatDoc *doc);

// --- talk-to decode (protsupp.cpp:1066-1101) --------------------------------
// Original signature: GetTalkTos(CChatDoc*, CUserInfo*, char*) / (CChatDoc*,
// CDWordArray*, char*), both calling the global LookupPui(szName, doc)
// directly. R19: LookupPui is hoisted to an explicit resolver parameter
// (pfnLookupPui) rather than a global -- see PFNLOOKUPPUI above.
void GetTalkTos(CChatDoc *doc, CUserInfo *talkerPui, char *str, PFNLOOKUPPUI pfnLookupPui);
void GetTalkTos(CChatDoc *doc, CDWordArray *talkTos, char *str, PFNLOOKUPPUI pfnLookupPui);

// --- annotation codec proper -------------------------------------------------
// Resolver typedef for reconstructing a CUserInfo* from an m_udi.m_talkTos
// DWORD key. Deviation (documented in the task report): the original
// (protsupp.cpp:3006-3020, Win32/32-bit) directly widened the stored DWORD
// back to a CUserInfo* via a C-style cast -- safe when DWORD and pointers are
// both 32 bits, but on this LP64 port that cast zero-extends a truncated
// 32-bit value into a garbage 64-bit pointer whose dereference is UB (the
// exact hazard panel.cpp's R13 note + engine_context.h's
// CCSessionSettings::userFromTalkTo already document and solve for the
// camera/panel code, Plan 2 Task 8). GetAddressees (which reads
// CUserInfo::m_udi.m_talkTos, a CDWordArray of truncated keys) needs the
// identical reconstruction but must not invent new session-table wiring in
// this codec-only task (R19's "hoist to a resolver parameter" applies here
// too, exactly as it does for LookupPui above) -- so the pointer recovery is
// an explicit resolver parameter instead of a hardwired cast. The caller
// (Task 4/6/7, or this task's test wrapper) supplies one; a natural real
// implementation is `ccContext().session.userFromTalkTo`. NOTE:
// GetWhisperedAddressees does NOT need this -- g_rgpuiWhisperees is a
// CPtrArray (full-width void* elements, never DWORD-truncated), so its
// original direct cast is already 64-bit-safe as-is.
typedef CUserInfo* (*PFNRESOLVETALKTO)(DWORD key);

// GetAddressees/GetWhisperedAddressees (protsupp.cpp:3006-3034): clip at the
// first 5 addressees (min(GetUpperBound(), 4)).
void GetAddressees(CUserInfo *pui, const char *szSeparator, CString &str, BOOL bUseNick, PFNRESOLVETALKTO pfnResolve);
void GetWhisperedAddressees(const char *szSeparator, CString &str);

// bInsertAnnotations (encoder, protsupp.cpp:3057-3099). Deviations from the
// original (documented in the task report): (1) the original reads the
// global MyAvatar() directly; MyAvatar() is a CC_NO_UI-stubbed session
// accessor that always returns NULL in this build configuration (avatar.cpp's
// real body is R11-wrapped under #ifndef CC_NO_UI, which this port never
// defines-away), so calling it here would make the encoder permanently dead
// code. `av` is therefore hoisted to an explicit parameter -- the caller
// (Task 4's outbound builder, or this task's test wrapper) supplies the
// CAvatarX* to read indices/emotions from. (2) `pfnResolve` is threaded
// through to the internal GetAddressees/GetWhisperedAddressees calls per the
// PFNRESOLVETALKTO note above. Everything else (the sprintf grammar, the
// GetAddressees/GetWhisperedAddressees dispatch on uModes) is verbatim.
BOOL bInsertAnnotations(CAvatarX *av, CUserInfo *puiSelf, char *szBuff, USHORT uModes, BOOL bIncludeParenthesis, PFNRESOLVETALKTO pfnResolve);

// ProcessUDIData (decoder, protsupp.cpp:1485-1542). Deviations from the
// original (documented in the task report): (1) `theApp.m_bVIPMode` (a
// session policy flag, not codec) is hoisted to an explicit `bVIPMode`
// parameter; (2) LookupPui is hoisted to `pfnLookupPui` per R19 above.
void ProcessUDIData(CChatDoc *pDoc, CUserInfo *pui, char *szData, BOOL bVIPMode, PFNLOOKUPPUI pfnLookupPui);

// --- key-string codec (protsupp.cpp:5073-5260) ------------------------------
// PROP CLIENT keystring codec ("key1=value1;key2=value2;..."). Pure
// CString/LPCSTR functions, no session dependency.
BOOL ChangeKeyString(CString &strKeyString, LPCSTR pszKey, LPCSTR pszValue, int nMaxSize);
BOOL GetValueFromKeyString(LPCSTR pszKeyString, LPCSTR pszKey, CString &strValueOut);
BOOL EnumKeyString(LPCSTR &pszKeyString, CString &strKey, CString &strValue);

#endif // __PROTSUPP_H__
