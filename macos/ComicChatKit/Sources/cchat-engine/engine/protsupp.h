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
#include "comicchat.h"   // Plan 3 Task 6: cc_annotations/cc_user_ref (payload-stage API)

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

// =============================================================================
// Plan 3 Task 6: the payload SECOND STAGE -- OnTextMsg/OnDataMsg/ProcessSay/
// ProcessComment, extracted from the original's UI/policy entanglement (R20)
// per the task brief. See protsupp.cpp's Task 6 provenance comment for the
// full fidelity diff; this header declares only the resulting stateless
// classification API.
//
// WHY STATELESS, NOT A LIFT OF THE ORIGINAL SIGNATURES: the original
// ProcessSay/ProcessComment/OnTextMsg/OnDataMsg all take a live CUserInfo*
// (ignore/flood flags, request-info counters, avatar-download state) and a
// CChatDoc* (history/rules/UI). Neither exists in this headless engine --
// Task 3/5b's discovery already established that the session-side user table
// (CUserInfo objects, LookupPui) is NOT lifted; Swift owns per-user state
// (state-and-codec.md §1.4) and the engine only ever sees wire-parsed nick
// STRINGS plus an opaque cc_user_ref from ccSessionResolveUser. So the payload
// stage here is reshaped into a pure classifier: (nick, wire text, msgType,
// resolver) in, "which ONE event to emit + its decoded fields" out. Every
// ignore-list/flood-counter/rules-matching/history-entry/avatar-download-state
// check the original made against pui's live flags is R20-dropped (there is
// no engine-side object to check those flags against); Swift, which DOES own
// the real per-user ignore/flood state, is free to filter/suppress after
// receiving the classified event. This is the payload stage's R20 boundary,
// listed exhaustively in the task report.

// Which single event OnTextMsg's/OnDataMsg's classification maps to. Mirrors
// the cc_proto_event variants this task populates (CC_EV_TEXT/ACTION/SOUND/
// AWAY_PEER/APPEARS_AS/DATA), plus ccPayloadSuppressed for wire content the
// original silently ate (untreated CTCP verbs, X-VCHAT, DCC, mid-negotiation
// probe replies with no display-worthy text -- see protsupp.cpp for the list).
typedef enum ccPayloadClass {
    ccPayloadSay,           // -> CC_EV_TEXT (plain say/think/whisper-anti-spoof, decoded annotations)
    ccPayloadAction,        // -> CC_EV_ACTION (\x01ACTION..\x01 CTCP, or m_uModes BM_ACTION comics-action)
    ccPayloadSound,         // -> CC_EV_SOUND (\x01SOUND "file" text\x01 CTCP)
    ccPayloadAwayPeer,      // -> CC_EV_AWAY_PEER (\x01AWAY message\x01 CTCP)
    ccPayloadAppearsAs,     // -> CC_EV_APPEARS_AS ("# Appears as name.url" comment)
    ccPayloadData,          // -> CC_EV_DATA (IRCX UDI blob with no visible text; OnDataMsg only)
    ccPayloadSuppressed,    // no event -- a CTCP verb inside ProcessSay with no
                            // in-scope event (R20); falls out of OnTextMsg with
                            // no event, but is NEVER produced by ProcessComment
                            // (see ccPayloadHandledNoEvent for that case)
    ccPayloadHandledNoEvent // ProcessComment's "#" grammar MATCHED a known
                            // prefix (return TRUE in the original) but that
                            // branch's original body is pure R20-dropped
                            // policy/reply-sending with no in-scope event
                            // (GetInfo/HeresInfo/BDrop/BDrop2). Distinct from
                            // ccPayloadSuppressed: this means "stop, do NOT
                            // fall through to ProcessSay" (matching the
                            // original's `!ProcessComment(...)` being FALSE
                            // when ProcessComment returns TRUE) -- whereas an
                            // UNMATCHED "#" comment (no prefix recognized,
                            // ProcessComment returns FALSE in the original)
                            // must fall through to ProcessSay's plain-say
                            // path, matching the original's OWN dispatch
                            // exactly (protsupp.cpp:4368's `if (*szMesg != '#'
                            // || !ProcessComment(...)) ProcessSay(...)`).
} ccPayloadClass;

// Resolver typedef for the payload stage's own nick->ref lookups (R19):
// GetTalkTos/IdentifyWhispers/ProcessSay's PuiFromDocNickIdent all resolved a
// nick string to a session-table pointer in the original; here that resolves
// to an opaque cc_user_ref via ccSessionResolveUser (bridge/cc_session.h).
// room_token disambiguates same-nick-different-room per R19's own contract.
typedef cc_user_ref (*PFNRESOLVENICKREF)(const char *szNickname, uint32_t room_token);

// The classification result. `text` holds the display text for
// Say/Action/Sound/AwayPeer; `file` is the sound filename (Sound only);
// `avatarName`/`avatarUrl` are AppearsAs only; `annotations`/`hasAnnotations`
// carry the decoded udi block for Say/Action (never set for the other
// classes -- CTCP/comment payloads don't carry a udi block of their own,
// matching the original: PrepareTextAction/PrepareSound only ever *mask*
// pui->m_udi.m_uModes into BM_ACTION, they don't touch the G/E groups).
typedef struct ccPayloadResult {
    ccPayloadClass  cls;
    CString         text;
    CString         file;
    CString         avatarName;
    CString         avatarUrl;
    cc_annotations  annotations;
    BOOL            hasAnnotations;
} ccPayloadResult;

// OnTextMsg (protsupp.cpp:4358 original). szMesg is the CTCP-unquoted (caller
// already ran bLowLevelUnquoting via ccCSInString upstream, ircsock.cpp) wire
// text, mutable (the codec advances/rewrites through it exactly like the
// original). msgType is the MT_* bitmask (PRVMSG/NOTICE/WHISPER |
// CHANNELSEND/PRIVATEMSG). pfnResolve resolves talk-to nick tokens (R19);
// room_token scopes those lookups. szExplicitTalkTos is nullable: the
// original's WHISPER handler (ircsock.cpp:1832-1841) pre-computes a
// CDWordArray of talk-to targets from the WHISPER command's OWN target-list
// wire field (args[2], distinct from any inline annotation "T" group) via
// GetTalkTos(doc,&talkTos,args[2]) and passes it through to OnTextMsg, which
// IdentifyWhispers then uses INSTEAD of defaulting to "just me" -- pass that
// same raw wire target-list string here (NULL for PRIVMSG/NOTICE, which have
// no such field) and OnTextMsg decodes it internally via the same GetTalkTos.
// Dispatches internally to ProcessComment (szMesg[0]=='#') or ProcessSay (the
// CTCP/plain-say path), exactly like the original's `if (*szMesg != '#' ||
// !ProcessComment(...)) ProcessSay(...)` -- see protsupp.cpp for why this
// port folds both into one entry point rather than two free functions plus a
// live CUserInfo* to carry state between them.
ccPayloadResult OnTextMsg(const char *szNickname, char *szMesg, BYTE msgType,
                          uint32_t room_token, PFNRESOLVENICKREF pfnResolve,
                          char *szExplicitTalkTos = NULL);

// OnDataMsg (protsupp.cpp:4374 original). szData is the IRCX DATA payload
// (already known to start with '#' by the caller's CCUDI1 dispatch gate,
// ircsock.cpp). Original dispatch: `*(szData+1)==' '` -> ProcessComment (a
// "# " comment arrived via DATA instead of PRIVMSG -- e.g. "# Appears as"),
// else ProcessUDIData (a real UDI annotation blob) -- verbatim, reproduced
// here.
ccPayloadResult OnDataMsg(const char *szNickname, char *szData, BYTE msgType,
                          uint32_t room_token, PFNRESOLVENICKREF pfnResolve);

#endif // __PROTSUPP_H__
