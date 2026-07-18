// ccomp.h — lifted from artifacts/inc/ccomp.h. Plan 3 Task 4.
//
// Pure user-match/mask-comparison data type + free functions ("nickname!
// username@ipaddress" mask parsing and '*'/'?' wildcard compare). Needed by
// query.h's CCQuery::m_pPrUserMatch member (PPRUSERMATCH) and query.cpp's
// bAddQuery-adjacent construction path (bGetUserMatchFromMask). Zero MFC/UI
// dependency in the original (only <windows.h>/<tchar.h> for the base
// scalar typedefs, all already provided by mfc_compat.h) -- same tier as
// Task 2's ccommon_str.cpp lift from the same artifacts/core sibling.
//
// Edit Rules applied: R1 (drop <windows.h>/<tchar.h>, mfc_compat.h supplies
// LPTSTR/UINT/BOOL/etc.), R8 (no doc/UI types referenced at all).
#ifndef CCOMP_H
#define CCOMP_H

#include "mfc_compat.h"

// Break down of a nickname!username@ipaddress mask for user comparing.
// Verbatim from artifacts/inc/ccomp.h:15-24.
typedef struct tagPRUSERMATCH
{
	LPTSTR	szTheMask;		// "?usti?!regisb@*.microsoft.com"
    LPTSTR	szNickname;		// "?usti?"
    UINT	cbNickname;		// 6
    LPTSTR	szUserName;		// "regisb"
    UINT	cbUserName;		// 6
    LPTSTR	szIPAddress;	// "*.microsoft.com"
    UINT	cbIPAddress;	// 15
} PRUSERMATCH, *PPRUSERMATCH;

// External routines, verbatim from artifacts/inc/ccomp.h:27-30.
extern BOOL	bMatchAll(LPTSTR szString, UINT cbLen);
extern BOOL bGetUserMatchFromMask(LPTSTR szIdentMask, PPRUSERMATCH pPrUserMatch);
extern BOOL bIsMaskCompare(LPCTSTR szMask, UINT cbMask, LPCTSTR szString, UINT cbString);
extern BOOL bIsMatch(PPRUSERMATCH pPrUserMatch, LPCTSTR szNickname, LPCTSTR szUserName, LPCTSTR szIPAddress);

#endif // CCOMP_H
