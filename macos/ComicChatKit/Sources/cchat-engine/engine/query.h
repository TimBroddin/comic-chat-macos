// query.h — lifted from v2.5-beta-1-modern/query.h. Plan 3 Task 4.
//
// CCQuery / CQueryPtrList: the request/response correlation list. Every
// outbound query-style command (WHO, WHOIS, LIST, MODE, PROP, ...) enqueues a
// CCQuery BEFORE sending (the discipline ircproto.cpp:1050-1061 establishes
// and this port's cc_session.cpp preserves exactly, see cc_session.cpp's
// bExecuteQuery-derived helper); inbound reply handlers (Task 5b) dequeue by
// command-type to decide routing. CQueryPtrList is CPtrList-derived (Task 2's
// shim, mfc_compat.h).
//
// Edit Rules applied:
//   R1  - #include "ccomp.h" (was implicit via chat.h) replaces the original's
//         implicit MFC/app include chain; mfc_compat.h supplies CPtrList/
//         CString/POSITION/BOOL/PVOID.
//   R8  - CCRule*/CCNotif* stay FORWARD-DECLARED ONLY (never defined in this
//         port -- the rules/notification daemon is app automation, entirely
//         out of the protocol lift's scope). See query.cpp's CCQuery ctor/dtor
//         for the resulting deviation (documented there): the dtRule/dtNotif
//         AddRef()/Release() branches cannot be compiled against an
//         incomplete type, so they become ASSERT(0)-guarded dead branches.
//         No lifted caller in this task ever constructs a CCQuery with
//         dt==dtRule or dt==dtNotif (ircproto.cpp's bExecuteQuery call sites
//         only ever pass dtMax/dtFlags/dtUser) -- confirmed by grep across
//         every ircproto.cpp CCQuery-adjacent call site.
#ifndef __QUERY_H__
#define __QUERY_H__

#include "mfc_compat.h"  // R1: CPtrList/CString/POSITION/BOOL/PVOID
#include "ccomp.h"       // R1: PPRUSERMATCH (Plan 3 Task 4 addition, see ccomp.h)

// Forward declarations only (R8): the rules/notification daemon (CCRule,
// CCNotif) is app automation, not protocol -- never defined in this port. See
// the header note above and query.cpp's ctor/dtor for the resulting handling.
class CCRule;
class CCNotif;

typedef enum
{
	qpBanDlg,
	qpComSetChannelMode,
	qpComSetUserMode,
	qpCreatePics,
	qpGetIdent,
	qpIgnoreIdent,
	qpInitialLUsersMOTD,
	qpInitialMode,
	qpInitialNames,
	qpInitialTopic,
	qpInitialWho,
	qpIrcX,
	qpIsIrcX,
	qpJoinBackUrl,
	qpJoinPics,
	qpKickDlg,
	qpListMembers,
	qpLUsersMOTD,
	qpOnConnectEvent,
	qpOnDisconnectEvent,
	qpOnNewRoomEvent,
	qpOnNotification,
	qpRoomListDlg,
	qpSetClient,
	qpSetInvisible,
	qpSetTopic,
	qpSetVisible,
	qpUserListDlg,
	qpMax
} enumQueryPurpose;


typedef enum
{
	ctGetChannelMode,
	ctIrcX,
	ctList,
	ctListX,
	ctLUsersMOTD,
	ctModeIsIrcX,
	ctNames,
	ctPropGet,
	ctPropSet,
	ctSetChannelMode,
	ctSetUserMode,
	ctTopic,
	ctWho,
	ctWhoIs,
	ctMax
} enumCommandType;


typedef enum
{
	dtFlags,
	dtNotif,
	dtRule,
	dtUser,
	dtMax
} enumDataType;


class CQueryPtrList;


class CCQuery
{
friend class CQueryPtrList;

public:
	CCQuery();
	CCQuery(enumQueryPurpose qp,
			enumCommandType ct,
			enumDataType dt,
			PVOID pvData,
			CString strChannelName,
			CString strNicknameMask,
			BOOL bCreatePrUserMatch);
	virtual ~CCQuery();

	void				SetQueryPurpose(enumQueryPurpose qp)		{ m_qp = qp; }
	void				SetCommandType(enumCommandType ct)			{ m_ct = ct; }
	void				SetDataType(enumDataType dt)				{ m_dt = dt; }
	void				SetData(PVOID pvData)						{ m_pvData = pvData; }
	void				SetChannelName(CString& strChannelName)		{ m_strChannelName = strChannelName; }
	void				SetNicknameMask(CString& strNicknameMask)	{ m_strNicknameMask = strNicknameMask; }

	enumQueryPurpose	GetQueryPurpose()	{ return m_qp; }
	enumCommandType		GetCommandType()	{ return m_ct; }
	enumDataType		GetDataType()		{ return m_dt; }
	PVOID				GetData()			{ return m_pvData; }
	CString				GetChannelName()	{ return m_strChannelName; }
	CString				GetNicknameMask()	{ return m_strNicknameMask; }
	PPRUSERMATCH		GetPrUserMatch()	{ return m_pPrUserMatch; }

protected:
	enumQueryPurpose	m_qp;
	enumCommandType		m_ct;
	enumDataType		m_dt;
	PVOID				m_pvData;
	CString				m_strChannelName;
	CString				m_strNicknameMask;
	PPRUSERMATCH		m_pPrUserMatch;
};


class CQueryPtrList : public CPtrList
{
public:
	CQueryPtrList() {};
	virtual ~CQueryPtrList();

	BOOL		bAddQuery(CCQuery* pQuery);
	void		FreeRemoveAll();
	void		FreeRemoveAt(POSITION pos);
	CCQuery*	FindQuery(enumCommandType ct, POSITION *pPos = NULL, LONG *plRank = NULL);
};

#endif // __QUERY_H__
