// panel.cpp — LIFTED for the macOS headless port (Plan 2 Task 8).
// Source of truth: v2.5-beta-1-modern/panel.cpp (READ-ONLY). Every edit vs the
// original is attributed to an Edit Rule (plan2-edit-rules.md); see
// p2-task-8-report.md for the exhaustive per-hunk map and the R11/R16/R17 lists.
//
// This file is the comic COMPOSITION engine: the body-placement "camera"
// (OrderAvatars/DoGreedyOrdering/EvalPlacement/EvalPair/AddTalkTos), the panel
// orchestrator (CUnitPanelPage::AddLine), and the balloon/avatar layout
// (LayoutAvatars/LayoutBalloons/LayoutBalloon). Those crown-jewel functions are
// lifted VERBATIM — only rule-governed reroutes (R2/R8/R17) touch them.

#include "mfc_compat.h"    // R1: was stdafx.h
#include "engine_context.h" // R2: was chat.h (ccContext() replaces theApp)
#include "userinfo.h"       // stays (lifted): CUserInfo (the talk-to graph)
// R8 (delete): chatprot.h, binddoc.h, chatdoc.h — UI/doc/protocol headers.
#include "traj.h"           // stays (lifted)
#include "spline.h"         // stays (lifted)
#include "bbox.h"           // stays (lifted)
#include "pe.h"             // stays (lifted)
#include "dib.h"            // stays (lifted)
#include "avatar.h"         // stays (lifted)
#include "balloon.h"        // stays (lifted)
#include "backdrop.h"       // stays (lifted)
// R8 (delete): pageview.h — UI view header.
#include "panel.h"          // stays (lifted, Task 6)
// R8 (delete): ui.h — UI header.
#include "vector2d.h"       // stays (lifted): ROUND/PI/LARGESHORT
#include "format.h"         // stays (lifted): SzControlLess/FreeAndNullFormatting
// R8 (delete): protsupp.h — protocol support header (no live symbol needed by
//   the ported subset; its only panel.cpp use, annotation codecs, is not here).
#include <stdlib.h>
#include <math.h>

// R2 (delete): extern CChatApp theApp;
extern BOOL		g_bNewedPanel;			// defined in lifted_singles.cpp (R12a)
// R8 (delete, dead): extern CBody *GetBodyCamBody(); — referenced nowhere.
extern BOOL		printBMP;				// used only in CUnitPanelPage::Draw (R16)
// R17 reroute setter for Establishing() (lifted_singles.cpp): AddLine/AddReaction
// register the page under composition so the camera's establishing-shot test can
// read its live panel count (was GetView()->GetDocument()->m_pages).
extern void		ccSetComposingPage(CUnitPanelPage* p);

IMPLEMENT_DYNAMIC(CDamage, CObject);

#define INFOMARGIN			10
#define MAXINFOTEXTHEIGHT	.5		// as a percentage of pane
#define YOFFSET				300
#define DELTA				320
#define BR_SPEAKER			0
#define BR_IMPORTANT		1
#define BR_GOODIDEA			2
#define BR_OK				3
#define MAXBDYPERFRAME		20
#define ONELINETHRESHOLD	500
#define MINHOOKHEIGHT		100
#define ICONSIZE			500
#define ICONSPACE			100
#define BELOWSTARRING		300
#define ROWHEIGHT			500
#define bZoomIn				TRUE					// FALSE for SIG PIX

COLORREF	clrs[] = { RGB(80, 80, 80), RGB(130, 130, 130), RGB(160, 160, 160), RGB(240, 240, 240), RGB(255, 255, 255)};
CDC*		pnlDC = NULL;		// For debugging purposes only

double	randfloat();
BOOL	bbox_overlap (RECT *bbox1, RECT *bbox2);
BOOL	Establishing();
void	PrintBMP();

int		CUnitPanelPage::m_panelsPerRow		= 2;
int		CUnitPanelPage::m_printPanelsPerRow	= 0;  // set in CPage::PreparePrintDC
int		CUnitPanelPage::m_panelsPerColumn	= -1; // negative value means no limit (page never ends)
int		CUnitPanelPage::m_unitWidth			= MINUNITPANELWIDTH-1;  // triggers a resize if not overridden
int		CUnitPanelPage::m_unitHeight		= MINUNITPANELHEIGHT-1;
int		CUnitPanelPage::m_hInterstice		= 144;
int		CUnitPanelPage::m_vInterstice		= 144;

int		CUnitPanel::m_borderWidth			= 60;	  // I hope these initialize in order!
CPen	CUnitPanel::m_borderPen(PS_SOLID, 2 * CUnitPanel::m_borderWidth, RGB(0,0,0));

class CBodyRecord
{
public:
	CBody*		m_body;
	CPtrList	m_lookAts;
	UCHAR		m_priority;
};


void DrawPoint(CDC *dc, POINT p, int delta)
{
	// Draw a simple plus sign at p
	dc->MoveTo(p.x, p.y - delta);
	dc->LineTo(p.x, p.y + delta);
	dc->MoveTo(p.x - delta, p.y);
	dc->LineTo(p.x + delta, p.y);
}


void DrawSpline(CDC *dc, RECT *rect)
{
	CPoint pt1(rect->left + DELTA, rect->bottom + DELTA);
	CPoint pt2(pt1.x, pt1.y + YOFFSET);
	CPoint pt3(rect->left + 2*DELTA, rect->bottom + 2*DELTA);
	CPoint pt4(pt3.x, pt3.y + YOFFSET);
	CPoint pt5(rect->left + 3*DELTA, rect->bottom + 3*DELTA);
	CPoint pt6(pt5.x, pt5.y + YOFFSET);

 #if 0
	POINT pts[6];
	pts[0] = pt1;
	pts[1] = pt2;
	pts[2] = pt3;
	pts[3] = pt4;
	pts[4] = pt5;
	pts[5] = pt6;

	for (int i = 0; i < 6; i++) {
		 DrawPoint(dc, pts[i]);
	}
	CBeta card(pts, 6);
	dc->PolyBezier(card.bezpts, card.BezierCount());
#endif
}


void DrawRoutes(CDC *dc, CPanel *panel)
{
	int i = 0;
	POSITION pos = panel->m_elements.GetTailPosition();
	while (pos != NULL)
	{
		CBalloon *b = (CBalloon *) panel->m_elements.GetPrev(pos);
		RECT r;
		r.left = b->m_routeRgn.Left;
		r.right = b->m_routeRgn.Right;
		r.top = 0;
		r.bottom = -CUnitPanelPage::m_unitHeight;
		dc->FillSolidRect(&r, clrs[i]);
		i = (i+1) % sizeof(clrs);
	}
}


void RandEmotion(CEmotion &e)
{
	e.m_intensity = (float) randfloat();
	e.m_emotion = (float)((randfloat() * 2.0 - 1.0) * PI);
}


int GetIndex(const CPtrList &list, void *member)
{
	int index = 0;
	POSITION pos = list.GetHeadPosition();
	while (pos)
	{
		const void *foo = list.GetNext(pos);
		if (member == foo)
			return index;
		index++;
	}
	return -1;
}


void ForceFitBalloon(CBalloon *pBalloon, RECT &rcFreeRect, char **pszRest, CDWordArray **pprgdwRestFormatting, char **pszURLStartInRest)
{
	ASSERT(pBalloon);
	ASSERT(pszURLStartInRest);
	ASSERT(pszRest);
	ASSERT(pprgdwRestFormatting);

	pBalloon->SetBBox(rcFreeRect.left, rcFreeRect.bottom, rcFreeRect.right, rcFreeRect.top);
	*pszRest = pBalloon->SplitHeight(rcFreeRect.top - rcFreeRect.bottom, pprgdwRestFormatting, pszURLStartInRest);  // need a fudge factor?
	if (pBalloon->m_bbox.Top > -250)
		pBalloon->DockAtTop(rcFreeRect.top);
}


BOOL GetInterveningBBox(CBalloon *balloons[], int index, RECT &freeRect, RECT &irect) {
	RECT cloudbox;
	int mostLeft, mostRight, leftAllowance, rightAllowance, delta;
	void Dock(RECT&);

	int toPtX = balloons[index]->m_speaker->m_arrowX;
	// find region between all routeRgns, above toPtX
	mostLeft = freeRect.left;
	mostRight = freeRect.right;
	for (int i = 0; i < index; i++) {
		balloons[i]->QueryRouteRgn(toPtX, leftAllowance, rightAllowance);
		mostLeft = max(leftAllowance, mostLeft);
		mostRight = min(rightAllowance, mostRight);
	}
	if (mostLeft > irect.left || mostRight < irect.right) {  // irect can't be placed as is
		int potentialClearance = mostRight - mostLeft;
		if (potentialClearance >= (irect.right - irect.left)) {		// we need only shift it
			if (mostLeft > irect.left)
				delta = mostLeft - irect.left;
			else delta = mostRight - irect.right;
			irect.left += delta;
			irect.right += delta;
		} else {											// grab maximal clearance
			irect.left = mostLeft;
			irect.right = mostRight;
		}
	}

	// irect.top must be no higher than bottom of any balloon to its right, and no higher than
	// bottom of any balloon to its right\and no higher than the top of any balloon
	// to its left.
	irect.top = freeRect.top;
	for (int i = 0; i < index; i++) {
		balloons[i]->GetCloudBBox(&cloudbox);
		if (cloudbox.right < irect.left) {		// cloud is to the right
			irect.top = min(irect.top, cloudbox.top);
		} else {
			Dock(cloudbox); // dock the cloudbox so that irect's top will be higher
			irect.top = min(irect.top, cloudbox.bottom);
		}
	}

	return TRUE;
}


int LowestPreviousBottom(CBalloon *balloons[], int index, int lowY)
{
	for (int i = 0; i < index; i++)
		lowY = min(lowY, balloons[i]->m_bbox.Bottom);
	return lowY;
}


BOOL NoneToLeft(CBalloon *balloons[], int nb, int index)
{
	if (index >= nb-1)
		return TRUE;

	int thisToLeft = balloons[index]->m_speaker->m_bbox.Left;

	for (int i = index+1; i < nb; i++)
	{
		if (balloons[i]->m_speaker->m_bbox.Left < thisToLeft)
			return FALSE;
	}

	return TRUE;
}


void AssignRECTToSRECT(RECT &r, SRECT &s)
{
	s.Left = (short) r.left;
	s.Right = (short) r.right;
	s.Bottom = (short) r.bottom;
	s.Top = (short) r.top;
}


void AdjustRouteRgns(CBalloon *balloons[], int index)
{
	// subtract out index's routeRgn from other routeRgns
	int left = balloons[index]->m_routeRgn.Left;
	int right = balloons[index]->m_routeRgn.Right;
	int toX = balloons[index]->m_speaker->m_arrowX;

	for (int i = 0; i < index; i++)
		balloons[i]->SetRouteRgn(toX, left, right);
}


int ComputeDisplacementPenalty(CPtrArray &bdyArray, int nEntries) {
	int penalty = 0;

	for (int i = 0; i < nEntries; i++) {
		CBodyRecord *r = (CBodyRecord *) bdyArray[i];
		CAvatarX *av = GetAvatar(r->m_body->m_avatarID);
		if (i > 0) {
			int rt = ((CBodyRecord *) bdyArray[i-1])->m_body->m_avatarID;
			if (av->m_lastRight != ((CBodyRecord *) bdyArray[i-1])->m_body->m_avatarID)
				penalty++;
		}
		if (i < nEntries-1) {
			int lt = ((CBodyRecord *) bdyArray[i+1])->m_body->m_avatarID;
			if (av->m_lastLeft != ((CBodyRecord *) bdyArray[i+1])->m_body->m_avatarID)
				penalty++;
		}
	}
	return penalty;
}

int EvalPair(CBodyRecord &b1, CBodyRecord &b2, int deltaPlacement)
{
	int rating = 0, desiredDir;
	if (deltaPlacement > 0)
		desiredDir = FALSE;
	else {
		desiredDir = TRUE;
		deltaPlacement = -deltaPlacement;
	}

	CUserInfo *pui1 = (CUserInfo *)(GetAvatar(b1.m_body->m_avatarID)->m_userInfo);
	ASSERT(pui1);
	// REGISB: 11/13/97 new m_udi in this function
	int nTalkTos = pui1->m_udi.m_talkTos.GetUpperBound() + 1;
	if (nTalkTos == 0) {
		if (b1.m_body->m_flip != desiredDir)  // talking to world, but I'm not facing other's direction
			rating += 4;
		if (b2.m_body->m_flip == desiredDir)  // talking to world, but he's not facing my direction
			rating += 2;
	} else {
		UINT b2ID = b2.m_body->m_avatarID;
		void *pui2 = GetAvatar(b2ID)->m_userInfo;
		for (int i = 0; i < nTalkTos; i++) {
			// R13: original `(DWORD) pui2` assumed 32-bit pointers (Win32);
			// clang rejects the pointer->DWORD truncation on LP64. The via-
			// uintptr_t cast is the minimal standard-conforming rewrite and is
			// behavior-exact for this port: every talkTos entry + compared
			// CUserInfo* points into the session's fixed users[] array (stable
			// base, struct-sized spacing), so the low-32-bit truncation is
			// consistent + collision-free. See p2-task-8-report.md (R13 list).
			if (pui1->m_udi.m_talkTos[i] == (DWORD)(uintptr_t) pui2) {
				if (b1.m_body->m_flip == desiredDir)  // if I'm facing him, then rating gets 2*adjacency
					rating += 4*(deltaPlacement-1);
				else
					rating += 40;						  // if I'm facing away, then heavy penalty
				if (b2.m_body->m_flip == desiredDir)   // if he's facing away, while I'm talking to him, minor penalty
					rating += 4;
			}
		}
	}
	return rating;
}


void AddTalkTos(CBodyRecord bdys[], int &recCount) {
	int initialCount = recCount;
	for (int i = 0; i < initialCount; i++) {
		CAvatarX *av = GetAvatar(bdys[i].m_body->m_avatarID);
		CUserInfo *pui = (CUserInfo *)(av->m_userInfo);
		// REGISB: 11/13/97 new m_udi in this function
		int nTalkTos = pui->m_udi.m_talkTos.GetUpperBound() + 1;
		for (int j = 0; j < nTalkTos; j++) {
			if (recCount >= 5) return;		// don't add more than 5 people to the panel!!!
			int duplicate = FALSE;
			for (int k = 0; k < recCount; k++) {
				CUserInfo *theirPui = (CUserInfo *)(GetAvatar(bdys[k].m_body->m_avatarID)->m_userInfo);
				// R13: pointer->DWORD truncation via uintptr_t (see EvalPair).
				if (pui->m_udi.m_talkTos[j] == (DWORD)(uintptr_t)theirPui) {
					duplicate = TRUE;
					break;
				}
			}
			if (!duplicate) {
				// R13 + R17: the original round-tripped a CUserInfo* OUT of the
				// DWORD array here — `(CUserInfo *)pui->m_udi.m_talkTos[j]` — to
				// pull an absent addressee's body into the panel. On LP64 that
				// widening rebuilds a garbage-high-bits pointer whose deref is
				// UB (the low-32 key can't reconstruct the full pointer). Recover
				// the REAL pointer via the session table (which owns every
				// CUserInfo a talkTos key can name). The original's unused debug
				// local `tst` (assigned, never read — zero uses in original AND
				// lifted) is subsumed by `talked`. A nullptr means the session-
				// wiring invariant broke; route it to the ORIGINAL's existing
				// ASSERT(FALSE) error branch below (the "no NULL path" else, ~15
				// lines down) rather than inventing a new guard — the original
				// cast could not fail, so nullptr IS the "impossible state."
				CUserInfo *talked = ccContext().session.userFromTalkTo(pui->m_udi.m_talkTos[j]);
				CAvatarX *theirAv = talked ? GetAvatar(talked->GetAvatarID()) : NULL;
				if (theirAv)
				{
					CEmotion neutral(0.0, 0.0);
					bdys[recCount].m_body = theirAv->GetBodyFromEmotion(neutral);
					bdys[recCount].m_body->m_requested = FALSE;   // talktos not required
					bdys[recCount++].m_priority = BR_GOODIDEA;
				}
				else
				{
					// Originally, there was no code to check for this NULL condition. But this
					// was causing a crash, and we have not been able to find what conditions
					// this crashes under. So I've put this ASSERT in here, so that if we
					// ASSERT here, we should consider it a bug and check it out.
					ASSERT(FALSE);
				}
			}
//			AddLookAt(av->m_talkTo[j], bdys[i]->m_avatarID, TRUE);
		}
	}
}


int EvalPlacement(CPtrArray &bdyArray, int nPlaced, CBodyRecord &bdy, int index, int &dir)
{
	bdyArray.InsertAt(index, &bdy);

	int penalty = ComputeDisplacementPenalty(bdyArray, nPlaced+1);
    int ratingR = penalty, ratingL = penalty;

	bdy.m_body->m_flip = FALSE;

	for (int i = 0; i <= nPlaced; i++) {
		CBodyRecord *rec1 = (CBodyRecord *) (bdyArray[i]);
		for (int j = i+1; j <= nPlaced; j++) {
			CBodyRecord *rec2 = (CBodyRecord *) bdyArray[j];
			ratingR += EvalPair(*rec1, *rec2, j-i) + EvalPair(*rec2, *rec1, i-j);
		}
	}

	bdy.m_body->m_flip = TRUE;

	for (int i = 0; i <= nPlaced; i++) {
		CBodyRecord *rec1 = (CBodyRecord *) (bdyArray[i]);
		for (int j = i+1; j <= nPlaced; j++) {
			CBodyRecord *rec2 = (CBodyRecord *) bdyArray[j];
			ratingL += EvalPair(*rec1, *rec2, j-i) + EvalPair(*rec2, *rec1, i-j);
		}
	}

	bdyArray.RemoveAt(index);

//	TRACE("EvalPair: ratingR = %d, ratingL = %d, nPlaced = %d, index = %d.\n",
//		  ratingR, ratingL, nPlaced, index);

	if (ratingR < ratingL) {
		dir = FALSE;
		return ratingR;
	} else if (ratingR > ratingL) {
		dir = TRUE;
		return ratingL;
	} else {  // equiv, so return avatar's last dir
		dir = GetAvatar(bdy.m_body->m_avatarID)->m_lastDir;
		return ratingR;
	}
}


void DoGreedyOrdering(CBodyRecord bdys[], int recCount, CPtrArray &bdyArray)
{
	int nPlaced = 0, dir, bestRating, bestPosition, bestDir;
	for (int i = 0; i < recCount; i++) {
		bestRating = 1000;
		for (int j = 0; j <= nPlaced; j++) {
			int rating = EvalPlacement(bdyArray, nPlaced, bdys[i], j, dir);
			if (rating < bestRating) {
				bestRating = rating;
				bestPosition = j;
				bestDir = dir;
			}
		}
		// insert element at best position
		bdys[i].m_body->m_flip = bestDir;
		bdyArray.InsertAt(bestPosition, bdys+i);
		nPlaced++;
	}
}


BOOL OrderAvatars(CBodyRecord bdys[], int &recCount, CPtrArray &placed)
{
	placed.SetSize(0, 5);
	if (recCount < 5)
		AddTalkTos(bdys, recCount);
	DoGreedyOrdering(bdys, recCount, placed);
//	DoForcedOrdering(bdys, recCount, placed);

	return TRUE;
}

void UpdateHistoresis(CPtrArray &placed, int nPlaced)
{
	for (int i = 0; i < nPlaced; i++) {
		CBodyRecord *r = (CBodyRecord *)(placed[i]);
		CAvatarX *av = GetAvatar(r->m_body->m_avatarID);
		av->m_lastDir = r->m_body->m_flip;

		if (i > 0)
			av->m_lastRight = ((CBodyRecord *)(placed[i-1]))->m_body->m_avatarID;

		if (i < nPlaced-1)
			av->m_lastLeft = ((CBodyRecord *)(placed[i+1]))->m_body->m_avatarID;
	}
}


// R11 (whole function): GetRandomTitle picks a random comic title from the
// string resources (CString::LoadString(IDS_TITLE1..)). Resource loading is out
// of scope (R6) and its only callers (chatdoc.cpp/histent.cpp) are non-lifted
// UI/doc files, so nothing in the lifted set calls it -- but it's kept (wrapped)
// for fidelity rather than deleted, consistent with the rest of the deferred
// title subsystem below.
#ifndef CC_NO_UI
char *GetRandomTitle()
{
	CString strTitle;
	static int nTitles = -1;
	if (nTitles == -1) {   // get title count (only need to do this first time)
		UINT titleID = IDS_TITLE1;
		while (TRUE) {
			if (!strTitle.LoadString(titleID) || strTitle.IsEmpty()) break;
			else titleID++;
		}
		nTitles = titleID - IDS_TITLE1;
	}
	if (nTitles < 1) {
		strTitle.LoadString(IDS_NOTITLE);
	} else {
		int chosenTitle = (int)(randfloat() * nTitles);
		chosenTitle = min(chosenTitle, nTitles-1);
		strTitle.LoadString(chosenTitle + IDS_TITLE1);
	}
	return strdup(strTitle);
}
#endif // CC_NO_UI (R11: GetRandomTitle)


// Plan 4a Task 7 (un-R11): AddStarsAux serves the title-page "starring"
// credits. Two reroutes (R17), everything else verbatim:
//   - g_mapNickToPtr (the doc's live member map, keyed by nick) -> the
//     headless session user table (ccContext().session.users[0..userCount)),
//     which is where every registered participant's CUserInfo lives in this
//     port (cc_compose.cpp's cc_strip_add_participant wiring invariant).
//   - g_puiSelf (pointer to the doc's own CUserInfo) -> a same-shape
//     comparison against ccContext().session.selfParticipant (the id
//     cc_strip_set_self records): `pui->GetAvatarID() == selfParticipant`
//     identifies the same "is this the self entry" test g_puiSelf pointer
//     identity performed, since every session user's GetAvatarID() is unique
//     and stable for the session's lifetime.
// The insertion/ordering algorithm itself (self first, then by departed-last +
// m_nSends-descending) is untouched.
void AddStarsAux(CPtrArray &stars, int maxStars)
{
	int inserted, GetAvatarUpperBound();
	CCSessionSettings &sess = ccContext().session;  // R17: was g_mapNickToPtr's owning doc

	for (int u = 0; u < sess.userCount; u++) {  // R17: was g_mapNickToPtr's GetStartPosition/GetNextAssoc walk
		CUserInfo *pui = &sess.users[u].info;
		CAvatarX *newAv = GetAvatar(pui->GetAvatarID());
		if (!newAv->m_icon) continue;
		BOOL usDeparted = pui->IsDeparted();

		if (pui->GetAvatarID() == sess.selfParticipant) {  // R17: was (pui == g_puiSelf)
			stars.InsertAt(0, newAv);
			continue;
		} else {
			int upper = stars.GetUpperBound();
			inserted = FALSE;
			for (int i = 1; i <= upper; i++) {
				CAvatarX *av = (CAvatarX *)(stars[i]);
				CUserInfo *theirPui = (CUserInfo *) av->m_userInfo;
				BOOL themDeparted = theirPui->IsDeparted();
				if ((!usDeparted && themDeparted)||
					((usDeparted == themDeparted) && (newAv->m_nSends > av->m_nSends))) {
					stars.InsertAt(i, GetAvatar(pui->GetAvatarID()));
					inserted = TRUE;
					break;
				}
			}
			// insert at end if there's room
			if (upper <= maxStars - 1 && !inserted) stars.InsertAt(upper+1, GetAvatar(pui->GetAvatarID()));
		}
	}

	int starsSoFar = stars.GetUpperBound() + 1;
	if (starsSoFar >= maxStars) return;
#if 0
	// add duplicates as necessary to fill out panel...  (but for now, only if standalone...)
	if (GetConnectionStatus() != CX_DISCONNECTED) return;
	int avsUpper = GetAvatarUpperBound();
	for (int i = 1; i <= avsUpper; i++) {
		CAvatarX *newAv = GetAvatar(i);
		if (!newAv->m_icon) continue;
		CUserInfo *newPui = (CUserInfo *) newAv->m_userInfo;
		if (newPui && newPui->GetAvatarID() != newAv->m_avatarID && newAv->m_nSends > 0) {  // then we have changed avs ...
			int starsUpper = stars.GetUpperBound();
			inserted = FALSE;
			for (int j = starsSoFar; j <= starsUpper; j++) {
				CAvatarX *av = (CAvatarX *)(stars[j]);
				if (newAv->m_nSends > av->m_nSends) {
					stars.InsertAt(j, newAv);
					inserted = TRUE;
					break;
				}
			}
			// insert at end if there's room
			if (starsUpper <= maxStars - 1 && !inserted) stars.InsertAt(starsUpper+1, newAv);
		}
	}
#endif
}


BOOL CPanelElement::SetBBox(int left, int bottom, int right, int top)
{
	m_bbox.Top = (short) top;
	m_bbox.Left = (short) left;
	m_bbox.Bottom = (short) bottom;
	m_bbox.Right = (short) right;
	return TRUE;
}


CPanel::CPanel()
{
	// choose a set seed so that panel always refreshes the
	// same way.
	m_seed = rand();
	m_hasBorder = TRUE;
	// R17: was GetChatDoc() ? GetChatDoc()->GetBackDropID() : 0; — the doc's
	// backdrop id every new panel inherits now lives in the session settings.
	m_backDrop.m_backID = ccContext().session.backdropID;
}


CPanel::CPanel(const CPanel &p) {
	m_seed = p.m_seed;
	m_hasBorder = p.m_hasBorder;
	m_backDrop.m_backID = p.m_backDrop.m_backID;

	// copy avatars
	POSITION pos = p.m_bodies.GetHeadPosition();
	while (pos) {
		CBody *b = (CBody *) p.m_bodies.GetNext(pos);
		m_bodies.AddTail(b->Clone());
	}

	// copy balloons
	pos = p.m_elements.GetHeadPosition();
	while (pos) {
		CBalloon *oldB = (CBalloon *) p.m_elements.GetNext(pos);
		CBalloon *newB = oldB->Clone();
		int index = GetIndex(p.m_bodies, oldB->m_speaker);  // trick: must sub in new avatar
		POSITION bdyPos = m_bodies.FindIndex(index);
		CBody *matchingBody = (CBody *) m_bodies.GetAt(bdyPos);
		newB->m_speaker = matchingBody;
		m_elements.AddTail(newB);
	}
}


CPanel::~CPanel()
{
	POSITION pos = m_elements.GetHeadPosition();
	while (pos != NULL)
	{	// for each page
		CPanelElement *pe = (CPanelElement *) m_elements.GetNext(pos);
		delete pe;
	}

	pos = m_bodies.GetHeadPosition();
	while (pos != NULL)
	{
		CBody *bdy = (CBody *) m_bodies.GetNext(pos);
		delete bdy;
	}
}


CBody *CPanel::FetchSpeaker(UINT uID)
{
	POSITION pos = m_bodies.GetHeadPosition();

	while (pos != NULL)
	{
		CBody *bdy = (CBody *) m_bodies.GetNext(pos);
		if (bdy->m_avatarID == uID)
			return bdy;
	}

	CAvatarX *av = GetAvatar(uID);
	CBody *bdy = av->m_body->Clone();
	av->RecordBody(bdy);

	m_bodies.AddTail(bdy);
	return bdy;
}


BOOL CPanel::ReplaceBody(UINT id) {
	POSITION pos = m_bodies.GetHeadPosition();
	while (pos) {
		CBody *oldBdy = (CBody *) m_bodies.GetAt(pos);
		if (oldBdy->m_avatarID == id) {
			// Create a clone of the requested body, and substitute it.
			CAvatarX *av = GetAvatar(id);
			CBody *newBdy = av->m_body->Clone();
			newBdy->m_requested = TRUE;
			m_bodies.SetAt(pos, newBdy);
			av->RecordBody(newBdy);

			// Now replace body in all associated balloons
			POSITION p2 = m_elements.GetHeadPosition();
			while (p2) {
				CBalloon *b = (CBalloon *) m_elements.GetNext(p2);
				if (b->m_speaker->m_avatarID == id)
					b->m_speaker = newBdy;
			}
			// must be done *after* the b->m_speaker->m_avatarID test, since it may be b->m_speaker that's deleted.
			delete oldBdy;
			return TRUE;
		}
		m_bodies.GetNext(pos);
	}
	return FALSE;
}


BOOL CPanel::AvatarInPanel(UINT avID) {
	POSITION pos = m_bodies.GetHeadPosition();
	while (pos) {
		CBody *b = (CBody *) m_bodies.GetNext(pos);
		if (b->m_avatarID == avID) return TRUE;
	}
	return FALSE;
}

void CUnitPanel::Draw(CDC *dc, POINT *ul, RECT *dmgRect)
{
	RECT rect, oldClip, itemBox;

	rect.top = rect.left = 0;
	rect.right = CUnitPanelPage::m_unitWidth;
	rect.bottom = - CUnitPanelPage::m_unitHeight;

//	dc->FillSolidRect(&rect, RGB(randfloat()*255, randfloat()*255, randfloat()*255));
//	return;

	dc->GetClipBox(&oldClip);
	dc->IntersectClipRect(&rect);
	dc->IntersectClipRect(dmgRect);

	m_backDrop.Draw(dc, &rect, dmgRect);
//	DrawRoutes(dc, this);

	// Draw each avatar
	POSITION pePos = m_bodies.GetHeadPosition();

	CBody *bdy = NULL;
	while (pePos != NULL)
	{
		bdy = (CBody *) m_bodies.GetNext(pePos);
		bdy->GetBBox(&itemBox);
		if (bbox_overlap(dmgRect, &itemBox))
			bdy->Draw(dc, NULL, dmgRect);
	}

	// Draw each panel element
	pePos = m_elements.GetTailPosition();
	while (pePos != NULL)
	{	// for each panel
		CPanelElement *pe = (CPanelElement *) m_elements.GetPrev(pePos);
		pe->GetBBox(&itemBox);
		if (bbox_overlap(dmgRect, &itemBox))
			pe->Draw(dc, NULL, dmgRect);
	}

	if (m_hasBorder)
		DrawBorder(dc, &rect);

	dc->SelectClipRgn(NULL, RGN_COPY);		// clear current clip
	dc->IntersectClipRect(&oldClip);		// reset old clip
}

void CUnitPanel::DrawBorder(CDC *dc, RECT *rect) {
	CPen *oldPen = dc->SelectObject(&m_borderPen);
	dc->BeginPath();
	dc->MoveTo(rect->left, rect->bottom);
	dc->LineTo(rect->left, rect->top);
	dc->LineTo(rect->right, rect->top);
	dc->LineTo(rect->right, rect->bottom);
	dc->CloseFigure();
	dc->EndPath();
	dc->StrokePath();

	dc->SelectObject(oldPen);
}


void CUnitPanel::LayoutAvatars() {
	BOOL OrderAvatars(CBodyRecord [], int&, CPtrArray&);
	CBodyRecord bRecs[MAXBDYPERFRAME];
	CPtrArray placed;
	short width[MAXBDYPERFRAME], height[MAXBDYPERFRAME], normHeight[MAXBDYPERFRAME], top[MAXBDYPERFRAME], headHeight[MAXBDYPERFRAME], maxHeadHeight = 0;
	double arrowX[MAXBDYPERFRAME];
	short bitArrowX;		// dist from left of bitmap of arrowX
	int bdyCount = 0, bdyWidth = 0, sumWidth = 0, maxNorm = 0;
	int nBodies = m_bodies.GetCount();

	// for now, lay them out in order
	ASSERT(nBodies > 0 && nBodies < MAXBDYPERFRAME);
	int maxBodyHeight = (int)(CUnitPanelPage::m_unitHeight / 1.9);
	int minMargin = 0;
	POSITION pos = m_bodies.GetHeadPosition();
	while (pos) {
		CBody *b = (CBody *) m_bodies.GetNext(pos);
		if (IsSpeaker(b)) { 	// Only grab the speakers
			bRecs[bdyCount].m_body = b;
			bRecs[bdyCount].m_priority = BR_SPEAKER;
			bdyCount++;
		} else
			delete b;					    // Other bodies get reclaimed
	}

	m_bodies.RemoveAll();

	OrderAvatars(bRecs, bdyCount, placed);
	ASSERT(bdyCount > 0);

	for (int i = 0; i < bdyCount; i++) {
		CBody *b = ((CBodyRecord *)(placed[i]))->m_body;
		b->GetDimInfo(width[i], height[i], normHeight[i], headHeight[i], bitArrowX);
		arrowX[i] = ((double) bitArrowX) / width[i];					// initially store arrows as percentage of width from left
		maxNorm = max(maxNorm, normHeight[i]);
	}

	for (int i = 0; i < bdyCount; i++) {
		// scale all of them such that maxHeight == m_unitHeight / 2
		int newHeight  = ROUND(maxBodyHeight * ((float)normHeight[i] / maxNorm));
		float scaleRatio = (float)newHeight / height[i];
		height[i] = newHeight;
		width[i] = ROUND(scaleRatio * width[i]);
		top[i] = -CUnitPanelPage::m_unitHeight + height[i];
		headHeight[i] = ROUND(scaleRatio * headHeight[i]);
		bdyWidth += width[i];
	}

	sumWidth = bdyWidth + (bdyCount+1) * minMargin;

	double zoomFactor = 1.0;
	if (sumWidth > CUnitPanelPage::m_unitWidth) {
		// must reduce the size of the avatars
		float reduction = (float) CUnitPanelPage::m_unitWidth / sumWidth;
		bdyWidth = 0;
		for (int i = 0; i < bdyCount; i++) {
			height[i] = ROUND(height[i] * reduction);
			width[i] = ROUND(width[i] * reduction);
			top[i] = -CUnitPanelPage::m_unitHeight + height[i];
			bdyWidth += width[i];
		}
		AdjustArtToCoord(0, 1.0);
	} else if (bZoomIn && !Establishing()) {
		// increase size of avatars
		zoomFactor = (double) CUnitPanelPage::m_unitWidth / sumWidth;

		for (int i = 0; i < bdyCount; i++)
			maxHeadHeight = max(maxHeadHeight, headHeight[i]);
		double headFactor = (double)maxBodyHeight / (maxHeadHeight * 1.2);  // don't cut at neck
		zoomFactor = min(zoomFactor, headFactor);
		if (zoomFactor < 1.1) zoomFactor = 1.0;

		bdyWidth = 0;
		for (int i = 0; i < bdyCount; i++) {
			height[i] = ROUND(height[i] * zoomFactor);
			width[i] = ROUND(width[i] * zoomFactor);
			bdyWidth += width[i];
		}
	}
	AdjustArtToCoord(-CUnitPanelPage::m_unitHeight + maxBodyHeight, zoomFactor);

	int margin = (CUnitPanelPage::m_unitWidth - bdyWidth) / (bdyCount+1); // margins also between avs and borders
	int xOffset = margin;
	for (int i = 0; i < bdyCount; i++) {
		CBodyRecord *r = (CBodyRecord *) placed[i];
		CBody *b = r->m_body;
		m_bodies.AddTail(b);
		b->SetBBox(xOffset, top[i]-height[i], xOffset+width[i], top[i]);
		b->m_arrowX = b->m_bbox.Left + ROUND(arrowX[i] * (b->m_bbox.Right - b->m_bbox.Left));
		xOffset += width[i] + margin;
	}

	void UpdateHistoresis(CPtrArray &, int);
	UpdateHistoresis(placed, bdyCount);
}

BOOL CUnitPanel::IsSpeaker(CBody *bdy) {
	if (bdy->m_requested) return TRUE;   // sort of makes them a speaker

	UINT avID = bdy->m_avatarID;
	POSITION pos = m_elements.GetHeadPosition();
	while (pos) {
		CPanelElement *e = (CPanelElement *) m_elements.GetNext(pos);
		if (e->GetType() & PE_BALLOON) {
			CBalloon *b = (CBalloon *) e;
			if (b->m_speaker->m_avatarID == avID) return TRUE;
		}
	}
	return FALSE;
}



RECT CUnitPanel::GetBalloonRect() {
	RECT brect;
	brect.left = brect.top = 0;
	brect.right = CUnitPanelPage::m_unitWidth;
	brect.bottom = -CUnitPanelPage::m_unitHeight / 2;
	if (m_hasBorder) {
		int penWidth = m_borderWidth;
		brect.left += penWidth;
		brect.right -= penWidth;
		brect.top -= penWidth;
	}

	return brect;
}


BOOL CUnitPanel::LayoutBalloons(char **pszRest, CDWordArray **pprgdwRestFormatting, char **pszURLStartInRest)
{
	int			nb = 0;
	CBalloon*	balloons[10];

	*pszRest = NULL;
	*pszURLStartInRest = NULL;
	*pprgdwRestFormatting = NULL;

	RECT		rcFreeRect = GetBalloonRect();
	POSITION	pos = m_elements.GetHeadPosition();

	srand(m_seed);	// always layout panel the same random way

	while (pos) {	// stash in an array for easy access
		CBalloon *nextBalloon = (CBalloon *) m_elements.GetNext(pos);
		balloons[nb++] = nextBalloon;
	}

	for (int i = 0; i < nb; i++)
		if (!LayoutBalloon(balloons, nb, i, rcFreeRect)) {  // best guess for layout
			if (i == 0 && nb == 1) {
				ForceFitBalloon(balloons[i], rcFreeRect, pszRest, pprgdwRestFormatting, pszURLStartInRest);
				return TRUE;
			} else return FALSE;
		}

	return TRUE;
}

void CUnitPanel::GetCloudEstimate(CBalloon *balloons[], int nb, int index, RECT& freeRect, RECT& brect) {
	int len, lineHeight, goalWidth;

	CBalloon *balloon = balloons[index];
	int area = balloon->AreaEstimate(&len, &lineHeight);
	int maxWidth = freeRect.right - freeRect.left;

	BOOL canBeTall = TRUE; // NoneToLeft(balloons, nb, index);
	if (len <= ONELINETHRESHOLD) {
		goalWidth = len;
	} else if (canBeTall) {
		int potentialHeight = LowestPreviousBottom(balloons, index, freeRect.top) - freeRect.bottom + MINHOOKHEIGHT;
		int minWidth = area / potentialHeight;
		minWidth = max(minWidth, balloon->WidestWord());
		goalWidth = minWidth + (int)(randfloat() * (maxWidth - minWidth));
	} else {
		// it should be wide, aim for no more than two or three lines
		// pick a random number between 1 and 3 inclusive
		int goalLines = 1 + (int)(randfloat() * 3.0);
		goalLines = min(3, goalLines);
		goalWidth = area / (goalLines * lineHeight);
	}

	// randomly place brect in x, guaranteeing that it overlaps character
	goalWidth = min(goalWidth+200, maxWidth); // the + N is a fudge factor.  FIX!!!
	goalWidth = min(goalWidth, len+200);		// won't be wider than len (FIX FUDGE)
	if (balloon->GetType() & PE_BOX) brect.left = freeRect.left;
	else {
		int toPtX = balloon->m_speaker->m_arrowX;
		int leftLimit = toPtX - goalWidth;
		int rightLimit = toPtX;
		int startX = leftLimit + (int)(randfloat() * (rightLimit - leftLimit));
		if (startX < freeRect.left) startX = freeRect.left;
		if (startX + goalWidth > freeRect.right) startX = freeRect.right - goalWidth;
		brect.left = startX;
	}
	brect.right = brect.left + goalWidth;  // top and bottom of brect computed elsewhere
}


BOOL CUnitPanel::LayoutBalloon(CBalloon *balloons[], int nb, int index, RECT& freeRect)
{
	BOOL itFit;
	RECT brect;

	GetCloudEstimate(balloons, nb, index, freeRect, brect);
	itFit = GetInterveningBBox(balloons, index, freeRect, brect);
	if (!itFit)
		return FALSE;

	CBalloon *balloon = balloons[index];
	if (!balloon->SetBBox(brect.left, brect.bottom, brect.right, brect.top))
		return FALSE;	// couldn't build a balloon with this text and size
	if (balloon->m_bbox.Top > -250)
		balloon->DockAtTop(freeRect.top);
	balloon->GetCloudBBox(&balloon->m_routeRgn);	// y not significant (note: this doesn't exactly provide disjoint routeRgns.  Fix)
	if (balloon->m_routeRgn.Bottom < freeRect.bottom + MINHOOKHEIGHT)
		return FALSE;
	AdjustRouteRgns(balloons, index);
	return TRUE;
}


void CUnitPanel::AdjustArtToCoord(int fixedY, double zoomFactor) {
	if (m_backDrop.m_mode == BF_NOZOOM) zoomFactor = 1.0;

	int logHeight = ROUND(CUnitPanelPage::m_unitHeight / zoomFactor);
	int logWidth = ROUND(CUnitPanelPage::m_unitWidth / zoomFactor);
	int newFixedY = ROUND(fixedY / zoomFactor);
	int delta = fixedY - newFixedY;
	m_backDrop.SetBBox(0, -logHeight + delta, logWidth, delta);
}


void CUnitPanel::RearrangeBalloons(CBalloon *balloons[], int nb, RECT& freeRect) {
}

// R11 (whole function): the "no character" hot-link handler kicks off an
// interactive avatar download (theApp.StartDownloadingAvatar) against the live
// doc — pure interactive UI, never on the headless layout path. The header
// declaration stays (it overrides CPanel::OnClickHotLink); the body is wrapped.
#ifndef CC_NO_UI
void
CUnitPanel::OnClickHotLink(
UINT   nLink,
LPCSTR pszLinkText)
{
	CString strCompare;

	strCompare.LoadString (IDS_NO_CHAR_HOTLINK);
	if (!lstrcmpi (pszLinkText, strCompare)) {
		POSITION pos = m_bodies.GetHeadPosition ();
		if (pos) {
			CAvatarX* pAv = GetAvatar (((CBody *)m_bodies.GetAt (pos))->m_avatarID);
			CUserInfo * pui = pAv ? (CUserInfo*)pAv->m_userInfo : NULL;
			extern BOOL g_bCanViewUnrated;
			if (pui != NULL && !pui->IsAvatarReal () && g_bCanViewUnrated) {
				theApp.StartDownloadingAvatar (pui, GetChatDoc (), TRUE);
			}
		}
	}
}
#else
void CUnitPanel::OnClickHotLink(UINT, LPCSTR) {}  // R11: no interactive download headless
#endif // CC_NO_UI (R11: OnClickHotLink)


CPage::~CPage() {
	POSITION pos = m_panels.GetHeadPosition();
	while (pos != NULL)	{							// for each page
		CPanel *panel = (CPanel *) m_panels.GetNext(pos);
		delete panel;
	}
}


CPanel *CPage::RemoveLastPanel() {
	// following refresh would be elegant, but it causes double repaints. why?
//	RefreshLastPanel();	// so that it redraws
	return ((CPanel *) m_panels.RemoveTail());
}


CSize CUnitPanelPage::GetScrollPage() {
	return CSize(m_unitWidth + m_vInterstice, m_unitHeight + m_hInterstice);
}

// R11 (whole functions): RefreshLastPanel/RefreshPanelN are the view-invalidation
// path — they compute a damage rect and poke the MFC view (m_doc->m_view) via
// UpdateViewsX to repaint. Headless draws once, so there is no live view to
// damage. Both are pure virtuals in CPage, so a #else no-op body keeps the class
// instantiable (R11 vtable-completeness exception). AddPanel/UpdateTitle call
// them; the no-op bodies make those callers safe. Header declarations stay.
#ifndef CC_NO_UI
void CUnitPanelPage::RefreshLastPanel() {
	int nPanels = m_panels.GetCount();
	ASSERT(nPanels > 0);
	nPanels--;
	RefreshPanelN(nPanels);
}

void CUnitPanelPage::RefreshPanelN(int nPanels) {	// nPanels starts at 0
	CDamage d;
	void UpdateViewsX(CView* pSender, LPARAM lHint = 0L, CObject* pHint = NULL, BOOL scrollToView = FALSE);

	// Calculate area that will be damaged
	int panelsToLeft = (nPanels % m_panelsPerRow);
	d.m_g.left = m_leftX + panelsToLeft * m_unitWidth;
	if (panelsToLeft > 0) d.m_g.left += m_vInterstice * panelsToLeft;
	int panelsAbove = nPanels / m_panelsPerRow;
	d.m_g.top = m_topY - (panelsAbove * m_unitHeight);
	if (panelsAbove > 0) d.m_g.top -= m_hInterstice * panelsAbove;
	d.m_g.bottom = d.m_g.top - m_unitHeight;
	d.m_g.right = d.m_g.left + m_unitWidth;

	UpdateViewsX(m_doc->m_view, 0L, &d);
}
#else
void CUnitPanelPage::RefreshLastPanel() {}       // R11: no live view headless
void CUnitPanelPage::RefreshPanelN(int) {}       // R11: no live view headless
#endif // CC_NO_UI (R11: RefreshLastPanel/RefreshPanelN)


BOOL CUnitPanelPage::AddPanel(CPanel *newPanel) {
	m_panels.AddTail(newPanel);
	RefreshLastPanel();
	return TRUE;
}


CBalloon *CUnitPanelPage::MakeBalloon(const char *szMesg, USHORT uModes, CDWordArray *prgdwFormatting, const char *szURLStart)
{
	switch (uModes)
	{
	case BM_SAY:
		return (new CBWoodringNormal(szMesg, prgdwFormatting, szURLStart));
	case BM_WHISPER:
		return (new CBWoodringWhisper(szMesg, prgdwFormatting, szURLStart));
	case BM_THINK:
		return (new CBWoodringThink(szMesg, prgdwFormatting, szURLStart));
	case BM_ACTION:
	case BM_ACTION|BM_SAY:
	case BM_ACTION|BM_THINK:
	case BM_ACTION|BM_WHISPER:
		return (new CBWoodringBox(szMesg, prgdwFormatting, szURLStart, uModes & BM_WHISPER));
	default:
		ASSERT(0);
		return (new CBWoodringNormal(szMesg, prgdwFormatting, szURLStart));	// security only - should never happen!
	}
}


BOOL CUnitPanelPage::AddLine(UINT uID, const char *szWords, USHORT uModes, CDWordArray *prgdwFormatting, const char *szURLStart)
{
	// R17: register the page under composition so Establishing() (the camera's
	// establishing-shot test, lifted_singles.cpp) can read this page's live
	// panel count in place of GetView()->GetDocument()->m_pages.
	ccSetComposingPage(this);

	BOOL	bReplaceLast;
	CPanel	*pNewP, *pOldP;
//	void AddSemantics(CPanel *, const char *), PostSemantics(CPanel *, const char *);

	if (uModes == BM_ACTION)
		StartNewPanel();			// start new panel for boxes

	if (strcmp(szWords, "<Brk>") == 0)
	{	// Force a new panel, given the break char (for debugging)
		StartNewPanel();
		return TRUE;
	}

	if (strcmp(szWords, "<Chr>") == 0)
		return AddReaction(uID);

	g_bNewedPanel = FALSE;

	pOldP = (CUnitPanel*) m_panels.GetTail();
	if (m_newPanel || pOldP->m_elements.GetCount() >= 5 || m_panels.GetCount() < 2 || pOldP->AvatarInPanel(uID))
	{
		pNewP = new CUnitPanel;
		m_newPanel = FALSE;
		bReplaceLast = FALSE;
		g_bNewedPanel = TRUE;
	}
	else
	{
		pNewP = pOldP->Clone();
		bReplaceLast = TRUE;
	}

	// AddSemantics(newP, words);

	// make a new balloon
	CBalloon *newBalloon = MakeBalloon(szWords, uModes, prgdwFormatting, szURLStart);
	if (!newBalloon)
		return FALSE;
	newBalloon->m_speaker = pNewP->FetchSpeaker(uID);

	pNewP->m_elements.AddTail(newBalloon);	// add balloon to panel

	// if char was in panel, for now, sub body
	pNewP->ReplaceBody(uID);

	pNewP->LayoutAvatars();					// make a best guess as to avatars & positioning

	char		*szLeftOverString = NULL, *szURLStartInLeftOver = NULL;
	CDWordArray	*prgdwLeftOverFormatting = NULL;

	if (!pNewP->LayoutBalloons(&szLeftOverString, &prgdwLeftOverFormatting, &szURLStartInLeftOver))
	{
		// if (pNewP->m_elements.GetCount() <= 1) return TRUE;  // Ignore utterance for now -- won't fit in panel
		delete pNewP;
		StartNewPanel();
		AddLine(uID, szWords, uModes, prgdwFormatting, szURLStart);
	}
	else
	{
		if (bReplaceLast)
		{
			RemoveLastPanel();
			delete pOldP;
		}
		//PostSemantics(pNewP, words);
		AddPanel(pNewP);
		void ResetAvatar(int);
		ResetAvatar(uID);   // Set the avatar to a new neutral position
		if (szLeftOverString)
		{
			AddLine(uID, szLeftOverString, uModes, prgdwLeftOverFormatting, szURLStartInLeftOver);
			free(szLeftOverString);
			if (szURLStartInLeftOver)
				delete [] szURLStartInLeftOver;
			FreeAndNullFormatting(&prgdwLeftOverFormatting);
		}
	}

	return TRUE;
}


BOOL CUnitPanelPage::AddReaction(UINT uID)
{
	// R17: register the page under composition (see AddLine) — AddReaction is a
	// second orchestrator entry that also runs LayoutAvatars -> Establishing().
	ccSetComposingPage(this);

	BOOL	bReplaceLast;
	CPanel	*pNewP, *pOldP;

	pOldP = (CUnitPanel *) m_panels.GetTail();
	if (m_newPanel || pOldP->m_bodies.GetCount() >= 5 || m_panels.GetCount() < 2)
	{
		pNewP = new CUnitPanel;
		m_newPanel = FALSE;
		bReplaceLast = FALSE;
	}
	else
	{
		pNewP = pOldP->Clone();
		bReplaceLast = TRUE;
	}

	// if char was in panel, for now, sub body and redraw
	if (!pNewP->ReplaceBody(uID))
		pNewP->FetchSpeaker(uID);

	pNewP->LayoutAvatars();					// make a best guess as to avatars & positioning

	char		*szLeftOverString = NULL, *szURLStartInLeftOver = NULL;
	CDWordArray	*prgdwLeftOverFormatting = NULL;

	if (!pNewP->LayoutBalloons(&szLeftOverString, &prgdwLeftOverFormatting, &szURLStartInLeftOver))
	{
		delete pNewP;
		StartNewPanel();
		AddReaction(uID);
	}
	else
	{
		if (bReplaceLast)
		{
			RemoveLastPanel();
			delete pOldP;
		}
		AddPanel(pNewP);
		void ResetAvatar(int);
		ResetAvatar(uID);   // Set the avatar to a new neutral position
	}

	ASSERT(prgdwLeftOverFormatting == NULL);

	return TRUE;
}


// R16: CUnitPanelPage::Draw is welded to the MFC view layer — retained-DIB
// mem-DC (CreateCompatibleDC), palette realize, CPageView::GetRetSec/
// AccountForScroll, and the SRCCOPY BitBlt scroll machinery. It is NOT ported.
// The whole definition is wrapped in #ifndef CC_NO_UI (kept verbatim for
// fidelity); Task 10's headless compositor (cc_compose.cpp, bridge code)
// replaces it, reusing the panel-origin walk (1232-1255) and GetBBox row/col
// math per R16. Draw is a pure virtual in CPage, so the class still needs a
// definition: the #else provides a no-op (headless never calls Draw — the
// compositor drives CUnitPanel::Draw per-panel directly).
#ifndef CC_NO_UI
void CUnitPanelPage::Draw(CPageView *pView, CDC *dc, POINT *, RECT *damage = NULL)
{
	// First set up rDC for retained dib section
	RECT	panelRect;
	RECT	damageRel;			// damage rectangle in panel coords
	CDC		memDC;
	pnlDC = &memDC;

	VERIFY(memDC.CreateCompatibleDC(dc));

	// Palette Operations definitely needed, RamuM
	CPalette *oldPal;
	CPalette *curPal = dc->GetCurrentPalette();
#ifdef NOGLOBPAL
	if (oldPal = memDC.SelectPalette(GetPalette(pDC), TRUE))
#else NOGLOBAPAL
	if (oldPal = memDC.SelectPalette(curPal, TRUE))
#endif NOGLOBPAL
		memDC.RealizePalette();

	memDC.m_bPrinting = dc->m_bPrinting;	// this determines which set of bitmaps to use
	memDC.SetMapMode(dc->GetMapMode());

	// REGISB: added 09/15/97
	// Update text color
	memDC.SetTextColor(theApp.m_comicsColor);

	POINT point;
	GetBrushOrgEx(memDC.GetSafeHdc(),&point);
	int iOldMode = memDC.SetStretchBltMode(STRETCHMODE); //COLORONCOLOR);
	SetBrushOrgEx(memDC.GetSafeHdc(),point.x,point.y,&point);

	HBITMAP retSec;
	VERIFY(retSec = pView->GetRetSec(dc));
	CBitmap temp;
	CBitmap *retCBit = temp.FromHandle(retSec);
	CBitmap *bmpOld = memDC.SelectObject(retCBit); // must use a CBitmap


	int truePanelsPerRow = dc->IsPrinting() ? m_printPanelsPerRow : m_panelsPerRow;
	SetRect(&panelRect, 0, 0, m_unitWidth, -m_unitHeight);
	int panelCount = 0;
	POINT loc;
	loc.x = loc.y = 0;
	POSITION panelPos = m_panels.GetHeadPosition();
	while (panelPos != NULL) {						// for each panel
		// XXX - add panel intersection test (then must change loc incrementing)
		CPanel *panel = (CPanel *) m_panels.GetNext(panelPos);
		panelCount++;
		SetRect(&damageRel, damage->left-loc.x, damage->top-loc.y, damage->right-loc.x, damage->bottom-loc.y);
		if (bbox_overlap(&damageRel, &panelRect)) {
			panel->Draw(&memDC, &loc, &damageRel);
			if (!dc->m_bPrinting || !printBMP) {
				POINT loc2 = pView->AccountForScroll(&loc, TRUE, TRUE, dc->m_bPrinting);
				VERIFY(dc->BitBlt(loc2.x, loc2.y, m_unitWidth, -m_unitHeight, &memDC, 0, 0, SRCCOPY));
			} else PrintBMP();
		}
		if (panelCount % truePanelsPerRow == 0) {
			loc.x = 0;
			loc.y -= m_unitHeight + m_hInterstice;
		} else
			loc.x += m_unitWidth + m_vInterstice;
	}

	GetBrushOrgEx(memDC.GetSafeHdc(),&point);
	memDC.SetStretchBltMode(iOldMode);
	SetBrushOrgEx(memDC.GetSafeHdc(),point.x,point.y,&point);

	memDC.SelectObject(bmpOld);

	memDC.SelectPalette(oldPal,TRUE);
}
#else
void CUnitPanelPage::Draw(CPageView *, CDC *, POINT *, RECT *) {}  // R16: replaced by Task 10 compositor
#endif // CC_NO_UI (R16: CUnitPanelPage::Draw)


// note: rect guaranteed to be normalized
void CUnitPanelPage::GetBBox(RECT *rect) {
	rect->left = m_leftX;
	rect->top = m_topY;
	int nPanels = m_panels.GetCount();
	int nRows = (nPanels - 1) / m_panelsPerRow + 1;  // note: assumption that nPanels > 0
	int nColumns = min(nPanels, m_panelsPerRow);
	rect->right = m_leftX + nColumns * m_unitWidth + (nColumns - 1) * m_vInterstice;
	rect->bottom = m_topY - (nRows * m_unitHeight + (nRows - 1) * m_hInterstice);
}


// Plan 4a Task 7 (un-R11): AddTitle builds the title/starring credits panel --
// CLabel/CStarLabel title art + AddStars (which reads the session user table,
// also un-R11'd this task). One reroute:
//   - starringStr.LoadString(ID_STARRING) -> a direct R9 constant. ID_STARRING
//     (resource.h:990, value 63017) has no other lifted reader and this port
//     has no resource compiler (R6), so rather than add a single-entry
//     LoadString registration for one never-reused id, the verified resource
//     TEXT itself (chat.rc:2275: `ID_STARRING "STARRING"` -- all caps,
//     verbatim) becomes the constant directly.
void CUnitPanelPage::AddTitle(const char *title) {
	CString starringStr;
	RECT border;

	CLabel *titleL = new CLabel(title, m_fiTitle);
	titleL->SetBBox(0, -m_unitHeight/2, m_unitWidth, -100);
	titleL->GetBBox(&border);
	starringStr = "STARRING";  // R9: was starringStr.LoadString(ID_STARRING) -- chat.rc:2275
	CLabel *starringL = new CLabel(starringStr, m_fiShout);
	starringL->SetBBox(0, -m_unitHeight, m_unitWidth, border.bottom);
	CUnitPanel *newPanel = new CUnitPanel;
	newPanel->m_hasBorder = FALSE;
	newPanel->m_backDrop.m_backID = 0;				// no background
	newPanel->m_elements.AddTail(titleL);
	newPanel->m_elements.AddTail(starringL);
	starringL->GetBBox(&border);
	AddStars(newPanel, border.bottom);
	AddPanel(newPanel);
}


// Plan 4a Task 7 (un-R11): UpdateTitle rebuilds the title/starring panel. One
// reroute (already applied by Plan 2 Task 8, ahead of this task's un-wrap):
// GetChatDoc()->GetComicsTitle() -> ccContext().session.comicsTitle. AddStars
// and RefreshPanelN are both live now (AddStars un-R11'd this task;
// RefreshPanelN was already a live headless no-op, panel.cpp:1094).
void CUnitPanelPage::UpdateTitle() {
	if (m_panels.IsEmpty())
		AddTitle(ccContext().session.comicsTitle);  // R17: was GetChatDoc()->GetComicsTitle()
	else {
		CUnitPanel *firstPanel = (CUnitPanel *) m_panels.GetHead();
		int count = firstPanel->m_elements.GetCount();
		for (int i = 0; i < count-2; i++) 			// -2 leaves in the title and starring labels
			delete ((CPanelElement *)firstPanel->m_elements.RemoveTail());
		CLabel *starring = (CLabel *) firstPanel->m_elements.GetTail();
		RECT border;
		starring->GetBBox(&border);
		AddStars(firstPanel, border.bottom);
		RefreshPanelN(0);
	}
}


// R11 (whole function): ShowInfo builds the per-avatar "info" credit panels
// (hot-link labels, capitalization, formatting-array control-byte processing) —
// interactive info-display UI, not on the layout path. Pure virtual in CPage
// -> #else no-op.
#ifndef CC_NO_UI
void CUnitPanelPage::ShowInfo(USHORT avID, const char *szInfo, char cHotLinkChar)
{
	// szInfo is a control full string

	CString			strFormatted;
	CDWordArray*	prgdwFormatting = new CDWordArray;
	CDWordArray*	prgdwRestFormatting = NULL;
	RECT			rcBorder;

	void Capitalize(char *);

	char* szControlFull = strdup(szInfo);

	Capitalize(szControlFull);

	char* szControlLess = SzControlLess(szControlFull, prgdwFormatting);
	if (cHotLinkChar != 0) {
		extern CDWordArray* MarkHotLinks(CDWordArray*, char *, char);
		prgdwFormatting = MarkHotLinks (prgdwFormatting, szControlLess, cHotLinkChar);
	}

	int iMaxBoxHeight = (int) (m_unitHeight * MAXINFOTEXTHEIGHT);

	while (TRUE)
	{
		CUnitPanel	*newPanel = new CUnitPanel;
		int			iMargin = newPanel->m_borderWidth + INFOMARGIN;
		char		*szRest = NULL;
		CLabel		*box;

		if (cHotLinkChar != 0)
			box = new CHotLinkLabel(szControlLess, m_fiWNormal, prgdwFormatting);
		else
			box = new CLabel(szControlLess, m_fiWNormal, prgdwFormatting);

		box->m_format |= FT_LEFT_JUSTIFY;
		newPanel->m_elements.AddTail(box);

		int			iTop = (int)(-100 * ((float) m_unitHeight / 4860)) - box->m_fontI->m_topOffset;
		int			iBottom = iTop - iMaxBoxHeight;

		box->SetBBox(iMargin, iBottom, m_unitWidth-iMargin, iTop);
		box->GetBBox(&rcBorder);

		if (rcBorder.bottom < iBottom)
		{
			szRest = box->SplitHeight(iMaxBoxHeight, &prgdwRestFormatting);
			box->GetBBox(&rcBorder);
		}
		// center text box (text inside box is ragged-right)
		int iBoxWidth = rcBorder.right - rcBorder.left;
		int iNewLeftX = (m_unitWidth - iBoxWidth) / 2;
		box->SetBBox(iNewLeftX, rcBorder.bottom, iNewLeftX + iBoxWidth, rcBorder.top);

		CBody *bdy = GetAvatar(avID)->GetBodyFromEmotion(CEmotion(0.0, 0.0));
		bdy->SetBBox(iMargin, -m_unitHeight, m_unitWidth-iMargin, rcBorder.bottom);
		newPanel->m_bodies.AddTail(bdy);

		newPanel->m_backDrop.m_backID = 0;			// no background for now
		AddPanel(newPanel);
		StartNewPanel();							// no adding to this one!

		if (szControlLess != szControlFull)
			free(szControlLess);
		szControlLess = szRest;
		FreeAndNullFormatting(&prgdwFormatting);
		if (!szRest) break;
		prgdwFormatting = prgdwRestFormatting;
	}

	free(szControlFull);
}
#else
void CUnitPanelPage::ShowInfo(USHORT, const char *, char) {}  // R11: info panel deferred headless
#endif // CC_NO_UI (R11: ShowInfo)


// Plan 4a Task 7 (un-R11): AddStars builds the starring-credits rows -- reaches
// MyAvatarID()/MyAvatar() (now live, un-R11'd this task: returns the
// set_self participant/its CAvatarX, 0/NULL if unset -- the early return
// below preserves the "not registered yet" guard verbatim) and AddStarsAux
// (also un-R11'd this task), and draws CStarLabel icon+name rows. No reroute
// needed in this body -- verbatim.
void CUnitPanelPage::AddStars(CUnitPanel *panel, int topY) {
	CPtrArray stars;
	CPtrArray sLabels;
	if (MyAvatarID() == 0) return;	// not registered yet
	CAvatarX *myAv = MyAvatar();
	int GetAvatarUpperBound();
	CString avatarCredit;

//	avatarCredit.LoadString(ID_AVATAR_CREDIT);

	int lineHeight = m_fiShout->m_lineHeight;
	int rowHeight = max(ICONSIZE, lineHeight);
	topY = topY - (BELOWSTARRING*m_unitHeight/4860);
	int maxStars = (m_unitHeight + topY) / rowHeight;
	topY -= rowHeight;			// start at bottom of first row

	// first get those that are actually logged in (one per person)
	AddStarsAux(stars, maxStars);
	int nStars = min(maxStars, stars.GetUpperBound()+1);
	int maxWidth = 0;

	for (int i = 0; i < nStars; i++)
	{
		RECT bbox;
		const char *szNickname;

		CAvatarX *star = (CAvatarX *)stars[i];
		star->GetScreenName(&szNickname);

		CStarLabel *label = new CStarLabel(szNickname, m_fiShout);
		label->SetBBox(0, -m_unitHeight, m_unitWidth, 0);
		label->GetBBox(&bbox);
		sLabels.Add(label);
		maxWidth = max(maxWidth, bbox.right - bbox.left);
	}

	maxWidth += ICONSIZE + ICONSPACE;
	int iconOffset = (m_unitWidth - maxWidth) / 2;
	if (iconOffset < 0) iconOffset = 0;
	int textOffset = iconOffset + ICONSIZE + ICONSPACE;
	int iconVdisp = (rowHeight - ICONSIZE)/2;			// center text or icon vertically in row
	int textVdisp = (rowHeight - lineHeight)/2;

	for (int i = 0; i < nStars; i++)
	{
		((CStarLabel*) sLabels[i])->m_format |= FT_LEFT_JUSTIFY;
		CBodyUnary *b = new CBodyUnary(((CAvatarX*) stars[i])->m_avatarID);
		b->m_bodyID = ((CAvatarX*) stars[i])->m_icon;
		int vertOffset =
			b->SetBBox(iconOffset, topY+iconVdisp, iconOffset+ICONSIZE, topY+ICONSIZE+iconVdisp);
		panel->m_elements.AddTail(b);
		((CStarLabel*) sLabels[i])->SetBBox(textOffset, topY+textVdisp, m_unitWidth, topY+lineHeight+textVdisp);
		panel->m_elements.AddTail((CStarLabel*) sLabels[i]);
		topY -= rowHeight;
	}
}


// R11 (whole function): PageSizeInPanels reads CPrintInfo::m_rectDraw (the
// print framework's page rect) — CPrintInfo is an opaque forward-declared type
// headless (R8), and printing is out of scope. Plain member; no lifted code
// outside the (also-R11) print functions calls it.
#ifndef CC_NO_UI
void CUnitPanelPage::PageSizeInPanels(CPrintInfo *pInfo, int &panelsWide, int &panelsHigh) {
	int pageWidth = pInfo->m_rectDraw.right - pInfo->m_rectDraw.left;
	int pageHeight = pInfo->m_rectDraw.top - pInfo->m_rectDraw.bottom;

	// panelsHigh equation below derives from the fact that
	// panelsHigh * m_unitHeight + (panelsHigh - 1) * m_hInterstice < pageHeight
	panelsHigh = (pageHeight + m_hInterstice) / (m_unitHeight + m_hInterstice);
	panelsWide = (pageWidth + m_vInterstice) / (m_unitWidth + m_hInterstice);
}
#endif // CC_NO_UI (R11: PageSizeInPanels)


// R11 (whole function): PreparePrintDC is print-framework glue — CPrintInfo
// (opaque headless), SetPrintOffset (pageview.cpp, not lifted), viewport-origin
// math for the print DC. Pure virtual in CPage -> #else no-op.
#ifndef CC_NO_UI
void CUnitPanelPage::PreparePrintDC(CDC *pDC, CPrintInfo *pInfo, int pageNum) {
	CPoint vpOrigin;
	RECT clipRect;
	int panelsWide, panelsHigh, startPanelRow, mapY;

	int pageWidth = pInfo->m_rectDraw.right - pInfo->m_rectDraw.left;
	int pageHeight = pInfo->m_rectDraw.top - pInfo->m_rectDraw.bottom;

	PageSizeInPanels(pInfo, panelsWide, panelsHigh);
	m_printPanelsPerRow = panelsWide;				// here we cache this value (side effect, oh well)

	vpOrigin.x = (pageWidth - (panelsWide * m_unitWidth + (panelsWide - 1) * m_vInterstice)) / 2;
	vpOrigin.y = -(pageHeight - (panelsHigh * m_unitHeight + (panelsHigh - 1) * m_hInterstice)) / 2;
	// now calculate the y that should be mapped to vpOrigin.y
	startPanelRow = (pageNum-1) * panelsHigh;
	mapY = startPanelRow * (m_unitHeight + m_hInterstice);
	// pDC->SetWindowOrg(-vpOrigin.x, -(mapY - vpOrigin.y));
	SetPrintOffset(-vpOrigin.x, -mapY - vpOrigin.y);

	clipRect.left = vpOrigin.x;
	clipRect.top = vpOrigin.y;
	clipRect.right = clipRect.left + panelsWide * m_unitWidth + (panelsWide-1) * m_vInterstice;
	clipRect.bottom = clipRect.top - panelsHigh * m_unitHeight - (panelsHigh-1) * m_hInterstice;
	pDC->IntersectClipRect(&clipRect);
}
#else
void CUnitPanelPage::PreparePrintDC(CDC *, CPrintInfo *, int) {}  // R11: printing out of scope
#endif // CC_NO_UI (R11: PreparePrintDC)


// R11 (whole function): GetPhysicalPageCount is print pagination — reads
// CPrintInfo (opaque). Pure virtual in CPage -> #else returns 0 (no printed
// pages headless).
#ifndef CC_NO_UI
int CUnitPanelPage::GetPhysicalPageCount(CPrintInfo *pInfo) {
	int panelsWide, panelsHigh, panelCount;

	PageSizeInPanels(pInfo, panelsWide, panelsHigh);
	panelCount = m_panels.GetCount();
	return ((int) ceil((double) panelCount / (panelsWide * panelsHigh)));
}
#else
int CUnitPanelPage::GetPhysicalPageCount(CPrintInfo *) { return 0; }  // R11: printing out of scope
#endif // CC_NO_UI (R11: GetPhysicalPageCount)



/* BEGIN BLOCK FOR SIGGRAPH PIX */
UINT bdyOrder[] = {2, 1};						// FOR SIGGRAPH PIX
BOOL dirOrder[] = {FALSE, TRUE};				// FOR SIGGRAPH PIX

void DoForcedOrdering(CBodyRecord bdys[], int recCount, CPtrArray &bdyArray) {
	TRACE("Entering forced w/ reccount of %d.\n", recCount);
	for (int i = 0; i < sizeof(bdyOrder)/sizeof(UINT); i++) {
		for (int j = 0; j < recCount; j++) {
			if (bdys[j].m_body->m_avatarID == bdyOrder[i]) {
				bdys[j].m_body->m_flip = dirOrder[i];
				bdyArray.Add(bdys+j);
				break;
			}
		}
	}
	ASSERT(recCount == bdyArray.GetUpperBound()+1);   // if not, a body not in bdyOrder!
	TRACE("Leaving forced w/ placed of %d.\n", bdyArray.GetUpperBound()+1);
}
/* END BLOCK FOR SIGGRAPH PIX */


#if 0
void PrintBodyAvID(CPanel *p, const char *str) {
	POSITION pos = p->m_bodies.GetHeadPosition();
	int i = 0;
	while (pos) {
		CBody *bdy = (CBody *) p->m_bodies.GetNext(pos);
		TRACE("(%x) %s: body #%d has av of %d.\n", p, str, i++, bdy->m_avatarID);
	}
	pos = p->m_elements.GetHeadPosition();
	i = 0;
	while (pos) {
		CBalloon *b = (CBalloon *) p->m_bodies.GetNext(pos);
		TRACE("(%x) %s: balloon #%d has av of %d.\n", p, str, i++, b->m_speaker->m_avatarID);
	}
}
#endif

#if 0
/* Explicitly commented by RamuM
CPalette *lastPal;

void InstallGrays(CDC *dc, BOOL realize = FALSE) {
	LOGPALETTE *lp = (LOGPALETTE *) malloc(sizeof(LOGPALETTE) + sizeof(PALETTEENTRY) * 255);

	lp->palVersion = 0x300;
	lp->palNumEntries = 256;
	for (int i = 0; i < 256; i++) {
		lp->palPalEntry[i].peRed = i;
		lp->palPalEntry[i].peGreen = i;
		lp->palPalEntry[i].peBlue = i;
		lp->palPalEntry[i].peFlags = 0;
	}

	CPalette *pal = new CPalette();  // This is not being cleaned up - TEST XXXXXXXXXXXXX
	lastPal = pal;
	VERIFY(pal->CreatePalette(lp));
	VERIFY(dc->SelectPalette(pal, FALSE));    // should save and reinstall old palette?
	if (realize) {
		int n = dc->RealizePalette();
		TRACE("%d colors realized.\n", n);
	}
	free(lp);
}

void DrawLines(CDC *dc) {
	for (int i = 0; i < 256; i++) {
		CPen pen(PS_SOLID, 30, PALETTERGB(i, i, i));
		CPen *oldPen = dc->SelectObject(&pen);
		dc->MoveTo(i*15, 0);
		dc->LineTo(i*15, -8000);
		dc->SelectObject(oldPen);
	}
}

*/
#endif

//#define PAGEHEIGHT ((int)(10.5 * 1440)) // printable page 10.5" high, 1440 units/inch
//#define PAGEWIDTH  ((int)(8.15 * 1440))	// printable page 8.15" wide, 1440 units/inch

// REGISB 09/17/97 not used
//void ForceFitLabel(CLabel *pLabel, RECT &rcFreeRect, char **pszRest, CDWordArray **pprgdwRestFormatting)
//{
//*pszRest = pLabel->SplitHeight(rcFreeRect.top - rcFreeRect.bottom, pprgdwRestFormatting);
//}
