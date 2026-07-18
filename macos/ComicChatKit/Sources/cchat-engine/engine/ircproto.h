// ircproto.h — PARTIAL LIFT from v2.5-beta-1-modern/ircproto.h. Plan 3
// Task 4: OUTBOUND command builders only (bytes in, events out; no inbound
// parsing yet -- that is Task 5b). See ircproto.cpp for the full provenance/
// Edit Rules note and the 17-UI-ish-sites disposition table (mirrored from
// docs/superpowers/plans/2026-07-18-plan3-discovery/ircproto-map.md §6).
//
// STRUCTURAL DEVIATION (documented up front because it reshapes every
// signature below): the original's CIrcProto derives from CRoomInfo
// (chatprot.h), an abstract seam with ~45 virtuals covering inbound CTCP
// replies, UI dialogs (DoChannelDialog/DoKickDlg/ChatInvite), NetMeeting/
// file-send, and app-policy hooks (OnLogin, SetConnectionStatus). Per the
// task brief rule 5 ("if a lifted builder references something non-outbound
// ... do NOT copy inbound/UI code"), NONE of that virtual dispatch is lifted
// this task -- only ~20 of CRoomInfo's ~45 virtuals are outbound protocol
// commands (discovery doc §1.1's "Seam quality verdict"), and every one of
// them is already a concrete CIrcProto method below with no virtual dispatch
// needed (nothing in this task calls through a CRoomInfo* base pointer; every
// call site is a concrete CIrcProto instance). CIrcProto is therefore a
// concrete, non-virtual class here, holding directly the four CRoomInfo data
// members its outbound bodies actually read (m_strChannel/m_strPassword/
// m_strTopic/m_dwModes/m_dwMaxUsers) instead of inheriting them. Re-deriving
// from a lifted CRoomInfo (with its UI virtuals) is explicitly NOT this
// task's job -- Task 5b/6/7 add whatever of that seam inbound handling needs.
//
// Likewise CIrcSocket here is NOT the full v2.5-beta-1-modern/ircsock.h class
// (CAsyncSocket-derived, SSPI auth blocks, login state, MOTD/LUSER
// accumulators, framing buffers) -- that whole class is ircsock.cpp/Task 5b
// territory (R21: SSPI drop ruling) and Swift owns the actual socket (spec
// §4.1). This CIrcSocket carries ONLY the fields ircproto.cpp's outbound
// builders read: m_bIrcXServer, m_nMaxMsgLength, m_szOutput2 (the sprintf
// scratch buffer bChatSendToTarget uses), and m_queries (the CCQuery
// correlation list, query.h). Task 5b grows this into (or replaces it with)
// the real connection-state class alongside the inbound parser.
#ifndef __IRCPROTO_H__
#define __IRCPROTO_H__

#include "mfc_compat.h"
#include "query.h"

// --- ENC_* text-encoding constants (verbatim values, ircsock.h:14-16) ------
#define ENC_CHANNEL	0
#define ENC_DBCS	1
#define ENC_UTF8	2

// --- AT_* argument-type flags (verbatim values, ircsock.h:24-45) -----------
// StrEncodeCommandParam's dwAt parameter is built from these bits by
// ProcessSlashCommand/the g_rgSyntax table (protsupp.cpp, not lifted this
// task) -- only the subset StrEncodeCommandParam's own switch tests is
// reproduced here (the full ircsock.h table also has AT_MAXMEMBER/
// AT_CHANNELFLAGS/AT_SERVER/AT_NETWORK/AT_SOUND/AT_USERFLAGS/AT_PROPNAME/
// AT_OPTIONAL/AT_SPACEMULTIPLE/AT_COMMAMULTIPLE/AT_SHOWCOLON/AT_COLON, none
// of which StrEncodeCommandParam's body reads).
#define AT_NICKNAME		0x00000001
#define AT_NICKMASK		0x00000002
#define AT_CHANNEL		0x00000004
#define AT_TOPIC		0x00000008
#define AT_REASON		0x00000010
#define AT_MESSAGE		0x00000020
#define AT_PASSWORD		0x00000100
#define AT_PROPVALUE	0x00004000

// --- resolver typedefs (R19 hoists; see ircproto.cpp per-function notes) ---
// bChatSendToTarget needs the caller's own nick + ident length (originally
// GetMyNickName()/GetMyUserName()/theApp.m_nMyIdentLength -- session identity
// state, sibling territory per discovery §9's "deliberately not mapped here").
// Threaded through as an explicit function-pointer parameter, same pattern as
// Task 3's PFNRESOLVETALKTO/PFNLOOKUPPUI (protsupp.h).
typedef struct {
	const char *own_nick;      // never NULL; "" if unknown
	const char *own_user;      // never NULL; "" if unknown
	int         ident_length;  // 0 if unknown (matches theApp.m_nMyIdentLength's "unset" sentinel)
} cc_own_identity;
typedef cc_own_identity (*PFNGETOWNIDENTITY)(void);

// StrEncodeCommandParam's channel-vs-nickname disambiguation
// (LookupDoc(szEncodedChannelName) in the original -- "is there a doc/room
// joined under this encoded channel name", discovery §6 site #16). Hoisted to
// an explicit predicate parameter: return TRUE if szEncodedChannel names a
// room the session currently considers joined.
typedef BOOL (*PFNISJOINEDCHANNEL)(const char *szEncodedChannel);

// --- CIrcSocket (outbound-scoped subset; see header note above) ------------
class CIrcSocket {
public:
	CIrcSocket() {
		m_bIrcXServer = FALSE;
		m_nMaxMsgLength = 512;   // g_nDefaultIOBuff, ircsock.h:12
		m_szOutput2 = new CHAR[m_nMaxMsgLength + 1];
		m_szOutput2[0] = '\0';
	}
	~CIrcSocket() {
		delete [] m_szOutput2;
	}

	BOOL			m_bIrcXServer;
	SHORT			m_nMaxMsgLength;
	CHAR			*m_szOutput2;
	CQueryPtrList	m_queries;
};

// --- CIrcProto (outbound-scoped subset; see header note above) -------------
class CIrcProto {
public:
	CIrcProto() { m_bInRoom = FALSE; m_dwModes = m_dwMaxUsers = 0L; }

	CIrcSocket*		m_pSock;
	BOOL			m_bInRoom;
	CString			m_strClientData;

	// Room state (CRoomInfo subset this task's outbound bodies read; see
	// header note above for why this isn't an inherited CRoomInfo).
	CString			m_strChannel;			// Encoded channel name
	CString			m_strPassword;
	CString			m_strTopic;
	DWORD			m_dwModes;
	DWORD			m_dwMaxUsers;

	void	SendMessageText(char *szMesg);
	BOOL	bChatSendToChannel(const char *szAnnotations, const char *szMesg, char *szNMText, USHORT uModes, PFNGETOWNIDENTITY pfnGetOwnIdentity);
	BOOL	bChatSendPrivMesg(const char *szAddressee, const char *szAnnotations, const char *szMesg, char *szNMText, BOOL bAsNotice, USHORT uModes, PFNGETOWNIDENTITY pfnGetOwnIdentity);
	BOOL	ChatSetTopic(const char *szTopic);
	BOOL	ChatKickUser(const char *szNickname, const char *szReason);
	BOOL	ChatBanUser(const char *szBanPattern, BOOL bBan, const char *szEncodedChannel = NULL);
	BOOL	ChatSendInvitation(const char *szNickname);
	BOOL	ChatChangeNick(const char *szNewNickname);
	// ChatSetAway: the original also fans this out to every OTHER open room's
	// CIrcProto (g_docs iteration, ircproto.cpp:930-937) -- app/session-doc
	// state out of this task's scope (R20; see .cpp). This method sends the
	// wire command only; per-room echo is Task 6/7's job once the session
	// owns a real room table.
	void	ChatSetAway(BOOL bAway, const char *szMesg);
	void	ChatPartChannel();
	void	ChatJoinAux(const char *szChannel, const char *szPassword);
	void	ChatCreateAux(const char *szChannel, const char *szCreationModes, DWORD dwMaxUsers, const char *szPassword);
	BOOL	bRegisterMode(char* szMesg);
	BOOL	IsIRCX() { return m_pSock->m_bIrcXServer; }
	BOOL	ChatSetMode(DWORD newMode, DWORD newMaxUsers, const char *szNewPasswd);
	BOOL	bExecuteQuery(enumQueryPurpose qp,
						  enumCommandType ct,
						  enumDataType dt,
						  PVOID pvData,
						  CString strChannelName,
						  CString strNicknameMask);
	BOOL	ChatSetClientData(const char *szClientData);
	int		EncodingType();
	const char*	EncodeString(const char *szString, int iEncoding = ENC_CHANNEL);

	// bChatSendToTarget/StrEncodeCommandParam need a resolver for a couple of
	// R19 hoists (own-nick/ident-length; "is this a joined channel"), threaded
	// through as explicit parameters -- same pattern Task 3's protsupp.cpp
	// already established for PFNRESOLVETALKTO/PFNLOOKUPPUI (resolver-as-
	// parameter, not a new global). See ircproto.cpp for the exact call sites
	// and the resolver typedefs below.
	BOOL	bChatSendToTarget(const char *szAddressee, const char *szAnnotations, const char *szMesg, USHORT uModes, BOOL bAsNotice, PFNGETOWNIDENTITY pfnGetOwnIdentity);
	CString	StrEncodeCommandParam(DWORD dwAt, INT *piEncoding, CHAR *szParam, PFNISJOINEDCHANNEL pfnIsJoinedChannel);
};

extern CIrcSocket serverConn;


#define APPEARSPREFIX		" Appears as "
#define GETINFOPREFIX		" GetInfo"
#define HERESINFOPREFIX		" HeresInfo: "
#define BACKGRNDPREFIX		" BDrop: "
#define NEWBACKGRNDPREFIX	" BDrop2: "
#define REQUESTCHARPREFIX	" GetCharInfo"
#define CCUDI1				"CCUDI1"

// Verbatim from v2.5-beta-1-modern/ircproto.h:91-121 -- header-embedded
// initialized const arrays (internal linkage per-TU, exactly as the original
// declared them; no separate .cpp definition needed or wanted).
const char actionID[]		= {0x01, 'A', 'C', 'T', 'I', 'O', 'N'};
const char soundID[]		= {0x01, 'S', 'O', 'U', 'N', 'D'};
const char versionID[]		= {0x01, 'V', 'E', 'R', 'S', 'I', 'O', 'N'};
const char pingID[]			= {0x01, 'P', 'I', 'N', 'G'};
const char timeID[]			= {0x01, 'T', 'I', 'M', 'E'};
const char emailID[]		= {0x01, 'E', 'M', 'A', 'I', 'L'};
const char urlID[]			= {0x01, 'U', 'R', 'L'};
const char netMeetingID[]	= {0x01, 'N', 'E', 'T', 'M', 'E', 'E', 'T'};
const char awayID[]			= {0x01, 'A', 'W', 'A', 'Y'};
const char clientInfoID[]	= {0x01, 'C', 'L', 'I', 'E', 'N', 'T', 'I', 'N', 'F', 'O'};
const char fileDCCID[]		= {0x01, 'D', 'C', 'C'};
const char xvchatID[]		= {0x01, 'X', '-', 'V', 'C', 'H', 'A', 'T'};

const short	g_nActionLen	= 7;
const short g_nSoundLen		= 6;
const short g_nVersionLen	= 8;
const short g_nPingLen		= 5;
const short g_nTimeLen		= 5;
const short g_nEmailLen		= 6;
const short g_nUrlLen		= 4;
const short g_nNetMeetLen	= 8;
const short g_nAwayLen		= 5;
const short g_nClientInfoLen= 11;
const short g_nFileDCCLen	= 4;
const short g_nHeresInfoLen	= 12;
const short g_nAppearsAsLen	= 12;
const short g_nXVChatLen	= 8;
const short g_nGetCharLen   = 12;

const WORD	g_wIgnoreIdent		= 0x0001;
const WORD	g_wAutoIgnoreIdent	= 0x0002;

// Character used to send a deferred URL, telling the receiver to query us
// for the real URL when needed.
#define DEFERRED_URL_STRING "?"
#define DEFERRED_URL_CHAR   '?'

extern const char *DecodeString(const char *szString, int iEncoding);
extern const char *EncodeChan(const char *szChannel);
extern const char *DecodeChan(const char *szChannel, BOOL bForceDBCS = FALSE);
extern const char *EncodeNick(const char *szNick, BOOL bEscapeWildcards = FALSE);
extern const char *DecodeNick(const char *szNick);
extern const char *DecodeNickForScreen(const char *szNick);
extern void GetModeChars(DWORD dwFlags, char *szBuff);
extern short nGetBreakingPoint(int iEncodingType, const char *szBody, short nBodyLen, short nMaxLength, WORD wFormatBegin, char *szFormatBegin, WORD *pwFormatEnd);

// --- outbound wire choke point (R19) ----------------------------------------
// CIrcProto::SendMessageText reaches this instead of a CAsyncSocket -- Swift
// owns the actual socket (spec §4.1). Defined in bridge/cc_session.cpp
// (reaches ccSession()->cfg.send); declared here so ircproto.cpp can call it
// without pulling in cc_session.h (which would create an engine->bridge
// header dependency the rest of this engine/ directory doesn't have -- only
// this one free-function declaration crosses that line, matching the "single
// outbound choke point" discipline the original itself uses).
extern void ccSessionSendRaw(const char *data, size_t len);

// TrimQuotes (actions.cpp:116-123): strips one layer of surrounding double
// quotes off a CString, used by StrEncodeCommandParam's nickname-mask
// handling. Pure CString helper with no session/app dependency; lifted
// directly here (not into a separate file) since ircproto.cpp is its only
// caller in this task's lift set (actions.cpp itself, the original owner, is
// slash-command/rules territory not lifted this task).
extern void TrimQuotesLocal(CString &strIn);

#endif // __IRCPROTO_H__
