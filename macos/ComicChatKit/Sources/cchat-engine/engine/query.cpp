// query.cpp — lifted from v2.5-beta-1-modern/query.cpp. Plan 3 Task 4.
// See query.h for the CCQuery/CQueryPtrList provenance/Edit Rules note.
//
// Edit Rules applied, hunk by hunk:
//   R1  - #include "stdafx.h"/"chat.h"/"cdebug.h" -> #include "query.h"
//         (mfc_compat.h transitively). SZTHISFILE dropped (R8: debug-heap
//         diagnostic aid, not correlation-list logic).
//   R13 - CDebug.H's two-arg ASSERT(cond, "msg") -> mfc_compat.h's one-arg
//         ASSERT(cond) (same rewrite as ccommon_str.cpp/ccomp.cpp).
//   R8  - CCQuery's two-arg constructor's dtRule/dtNotif branches
//         (CCRule::AddRef/CCNotif::AddRef, dtor's matching Release calls):
//         CCRule/CCNotif are forward-declared-only in this port (query.h),
//         so a real AddRef()/Release() call would not compile. These
//         branches are DEAD in every call path reachable from this task's
//         lifted callers (ircproto.cpp's bExecuteQuery only ever constructs
//         a CCQuery with dt==dtMax, matching enumDataType's dtFlags/dtUser/
//         dtMax cases -- never dtRule/dtNotif, which are exclusively used by
//         the rules/notification daemon's OWN query bookkeeping, out of this
//         plan's scope). Rewritten as ASSERT(0) placeholders that fire only
//         if a future caller ever does pass dtRule/dtNotif -- an honest "not
//         supported yet" signal rather than a silent behavior change, and
//         exactly parallel to the R20/R21 "drop with a loud guard" posture
//         used elsewhere in this plan for out-of-scope call paths.
//   R1  - bGetUserMatchFromMask (was declared via chat.h's transitive
//         ccomp.h/actions.h chain) now reaches its declaration through the
//         directly-included ccomp.h (Plan 3 Task 4 addition, see ccomp.h/.cpp).
// All other lines are copied verbatim from the original (whitespace/brace
// style aside).

#include "query.h"


/////////////////////////////////////////////////////////////////////////////
// CCQuery::CCQuery - Constructor
CCQuery::CCQuery()
{
	m_qp			= qpMax;
	m_ct			= ctMax;
	m_dt			= dtMax;
	m_pvData		= NULL;
	m_pPrUserMatch	= NULL;
}


CCQuery::CCQuery(enumQueryPurpose qp,
				 enumCommandType ct,
				 enumDataType dt,
				 PVOID pvData,
				 CString strChannelName,
				 CString strNicknameMask,
				 BOOL bCreatePrUserMatch)
{
	m_qp				= qp;
	m_ct				= ct;
	m_dt				= dt;
	m_pvData			= pvData;
	m_strChannelName	= strChannelName;
	m_strNicknameMask	= strNicknameMask;

	if (dtRule == dt)
	{
		// R8: CCRule is forward-declared-only in this port (query.h) -- the
		// rules daemon is app automation, out of the protocol lift's scope.
		// No lifted caller in this task ever constructs a CCQuery with
		// dt==dtRule (see query.h's header note); this is a loud guard, not
		// a silent behavior change.
		ASSERT(0);
	}
	else
		if (dtNotif == dt)
		{
			// R8: same reasoning as dtRule above, for CCNotif/the
			// notification daemon.
			ASSERT(0);
		}

	if (bCreatePrUserMatch)
	{
		ASSERT(!m_strNicknameMask.IsEmpty());
		m_pPrUserMatch = new PRUSERMATCH;
		if (m_pPrUserMatch)
			// since the strNicknameMask is not going to change we don't make a copy to store into pPrUserMatch,
			// we use the CString string instead.
			bGetUserMatchFromMask((LPTSTR) (LPCTSTR) m_strNicknameMask, m_pPrUserMatch);
	}
	else
		m_pPrUserMatch = NULL;
}


CCQuery::~CCQuery()
{
	if (m_pPrUserMatch)
		// szTheMask is actually the m_strNicknameMask pointer, we don't need to free it
		delete m_pPrUserMatch;

	if (dtRule == m_dt)
	{
		// R8: see the constructor's dtRule branch above -- unreachable in
		// this port's call set, loud guard rather than a silent no-op.
		ASSERT(0);
	}
	else
		if (dtNotif == m_dt)
		{
			// R8: see the constructor's dtNotif branch above.
			ASSERT(0);
		}
}


CQueryPtrList::~CQueryPtrList()
{
	FreeRemoveAll();
}


BOOL CQueryPtrList::bAddQuery(CCQuery* pQuery)
{
	ASSERT(pQuery);

	AddTail((PVOID) pQuery);
	return TRUE;
}


void CQueryPtrList::FreeRemoveAll()
{
	POSITION	pos;
	CCQuery*	pQuery;

    for (pos = GetHeadPosition(); pos != NULL; )
    {
		pQuery = (CCQuery*) GetNext(pos);
		delete pQuery;
	}

	CPtrList::RemoveAll();
}


void CQueryPtrList::FreeRemoveAt(POSITION pos)
{
	ASSERT(pos);

	CCQuery*	pQuery = (CCQuery*) GetAt(pos);

	ASSERT(pQuery);

	delete pQuery;

	CPtrList::RemoveAt(pos);
}


// Find oldest queued query of type ct
CCQuery* CQueryPtrList::FindQuery(enumCommandType ct, POSITION *pPos, LONG *plRank)
{
	POSITION	pos, posPrev = NULL;
	CCQuery*	pQuery = NULL;
	LONG		lRank = 1L;

	if (pPos)
		*pPos = NULL;

	if (plRank)
		*plRank = 0L;

    for (pos = GetHeadPosition(); pos != NULL; )
    {
		posPrev = pos;
		pQuery = (CCQuery*) GetNext(pos);
		if (pQuery->m_ct == ct)
		{
			if (pPos)
				*pPos = posPrev;
			if (plRank)
				*plRank = lRank;
			return pQuery;
		}
		lRank++;
	}
	return NULL;
}
