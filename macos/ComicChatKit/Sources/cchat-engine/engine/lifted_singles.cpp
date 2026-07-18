// lifted_singles.cpp — rule R12(a): single symbol definitions lifted verbatim
// from files scheduled for a LATER plan, but referenced by the load/layout
// path of files lifted NOW. Each is a byte-for-byte copy of its original body
// (only the surrounding #include lines are the lift's own), tagged with the
// owning file:line. Later plans DELETE each entry as they lift the owning file.
//
// (Added 2026-07-17, Plan 2 Task 6 amendment: the panel.cpp static-DATA-member
//  clause + the R12(a) load-path-executed-function clause together.)
//
// --------------------------------------------------------------------------
// Entry 1: bbox.cpp geometry helpers (owning file: bbox.cpp — NOT scheduled
//   for a lift in this task's file list; it is trivial pure geometry, sibling
//   to the already-lifted vector2d/spline/traj). balloon.cpp's layout path
//   executes these:
//     * make_empty(SRECT*)            — CBalloon::ComputeCloudBBox (bbox.cpp:87)
//     * include_pt_in_bbox(POINT*,SRECT*) — CBalloon::ComputeCloudBBox (bbox.cpp:32)
//     * bbox_overlap(RECT*,RECT*)     — CBalloon::Overlap (bbox.cpp:60)
//     * bbox_around_pt(RECT*,POINT*,int) — CBWoodringThink::Draw (bbox.cpp:12)
//     * adjust_bbox(RECT*,int)        — dependency of bbox_around_pt (bbox.cpp:5)
//     * inside_bbox(POINT*,RECT*)     — CLabel::bURLHit (bbox.cpp:39)
//     * SRECTToRECT(SRECT&)           — CBodyDouble/CBodySingle::Draw
//                                        (bodycam.cpp:578/614, LIVE in Task 7)
//   Each body below is verbatim from v2.5-beta-1-modern/bbox.cpp; full file
//   comes when a later plan lifts bbox.cpp (delete these then).
//
// Entry 2: panel.cpp static DATA member (owning file: panel.cpp:59, Plan 2
//   Task 8). CUnitPanelPage::m_unitWidth was lifted here header-only in Task 6
//   (balloon.cpp/fonts.cpp read it before panel.cpp existed). Task 8 lifts
//   panel.cpp, which defines m_unitWidth at file scope with the same verbatim
//   initializer -- so this entry is DELETED here (ODR: exactly one definition).
//
// Entry 3: CUserInfo constructibility (owning file: userinfo.cpp, Plan 3 --
//   NOT scheduled for a lift in this plan; the intl.c/MIME userinfo debt is
//   already registered as Plan 3 in cc_link_stubs.cpp). Plan 2 Task 8's R17
//   session user table (engine_context.h CCSessionUser) embeds a CUserInfo BY
//   VALUE (the brief's mandated {UINT id; CUserInfo info} shape), because the
//   camera (panel.cpp EvalPair/AddTalkTos) reads the addressee graph off
//   CUserInfo::m_udi.m_talkTos. That by-value member needs CUserInfo's default
//   ctor + vtable anchor to link. Lifted verbatim (R12a):
//     * CUserInfo::CUserInfo()   -- userinfo.cpp:111 (trivial field zeroing)
//     * CUserInfo::GetScreenName -- userinfo.cpp:172 (first non-inline virtual,
//                                   emits the vtable; self-contained, no theApp)
//     * CUserInfo::SetScreenName -- userinfo.cpp:163 (Plan 4a Task 7: needed by
//                                   the inline CUserInfo::SetName, userinfo.h:
//                                   127-131, which cc_strip_add_participant now
//                                   calls so AddStars' starring rows have a
//                                   real nickname to render -- self-contained,
//                                   no theApp)
//   The one remaining vtable slot, GetQualifiedName (userinfo.cpp:179, touches
//   theApp.m_bShowIdentity -- UI), is an R12(b) trap stub in cc_link_stubs.cpp
//   (never called on session users). Plan 3 lifts userinfo.cpp and deletes
//   both this entry and that stub.
//
// Entry 4: pageview.cpp cross-file singles referenced by panel.cpp's lifted
//   layout path (owning file: pageview.cpp -- NOT lifted; the Task-10 headless
//   compositor replaces its Draw/scroll machinery per R16). The parent-agent
//   ruling (Task 8) placed these here rather than in panel.cpp file scope to
//   keep panel.cpp's fidelity diff ~= original:
//     * g_bNewedPanel (pageview.cpp:830) -- R12(a): a plain BOOL data global.
//       Written ONLY by the lifted CUnitPanelPage::AddLine (panel.cpp:1076/
//       :1084 -- verified the exhaustive writer set: pageview.cpp defines it,
//       panel.cpp writes it, nothing else); read by Establishing() below.
//     * Establishing() (pageview.cpp:832) -- R12(a) body + R17 reroute. Called
//       by the CROWN-JEWEL CUnitPanel::LayoutAvatars (panel.cpp:788,
//       `bZoomIn && !Establishing()`) to suppress zoom-in on the first 1-2
//       (establishing) panels. The original read the live first-page panel
//       count via GetView()->GetDocument()->m_pages; the R17 reroute reads it
//       off s_composingPage (the CUnitPanelPage currently being composed, set
//       by AddLine/AddReaction). In the headless single-page model (discovery
//       §7: pagination deferred, m_panelsPerColumn=-1) the first page IS the
//       page being composed, so this is exact. Null composing-page -> count 0
//       -> TRUE (the conservative "early composition, don't zoom" default,
//       identical to the original's count<=1 branch). Arithmetic otherwise
//       verbatim.
//     * ccSetComposingPage() -- the R17 reroute setter (non-original glue;
//       lives here with the code it serves).
// --------------------------------------------------------------------------

#include "mfc_compat.h"
#include "engine_context.h"  // Entry 3: CCSessionUser embeds CUserInfo (userinfo.h
                             // via engine_context.h); Entry 4 unused but harmless
#include "userinfo.h"   // Entry 3: CUserInfo ctor + GetScreenName singles
#include "bbox.h"
#include "vector2d.h"   // LARGESHORT (make_empty's SRECT sentinel)
#include "traj.h"       // CTraj/CSpline fwd chain used by balloon.h
#include "spline.h"     // CSpline (balloon.h members)
#include "pe.h"         // CPanelElement (balloon.h base class)
#include "dib.h"        // CDIB (avatar.h chain)
#include "avatar.h"     // CBody (balloon.h + panel.h)
#include "balloon.h"    // CFontInfo/CBalloon (panel.h's CUnitPanelPage decl needs them)
#include "backdrop.h"   // CBackDrop (panel.h's CPanel decl)
#include "panel.h"      // CUnitPanelPage (Entry 4 Establishing reads m_panels)

// --- Entry 1: bbox.cpp geometry helpers (verbatim; provenance per line) -----

// lifted verbatim from bbox.cpp:5 — full file comes in a later plan
void adjust_bbox(RECT *bbox, int delta) {
	bbox->left -= delta;
	bbox->bottom -= delta;
	bbox->right += delta;
	bbox->top += delta;
}

// lifted verbatim from bbox.cpp:12 — full file comes in a later plan
void bbox_around_pt(RECT *bbox, POINT *pt, int delta) {
	bbox->left = bbox->right = pt->x;
	bbox->top = bbox->bottom = pt->y;
	if (delta) adjust_bbox(bbox, delta);
}

// lifted verbatim from bbox.cpp:32 — full file comes in a later plan
void include_pt_in_bbox(POINT *pt, SRECT *bbox) {
	bbox->Left = (short) min(pt->x, bbox->Left);
	bbox->Bottom = (short) min(pt->y, bbox->Bottom);
	bbox->Right = (short) max(pt->x, bbox->Right);
	bbox->Top = (short) max(pt->y, bbox->Top);
}

// lifted verbatim from bbox.cpp:39 — full file comes in a later plan
BOOL inside_bbox (POINT *pt, RECT *bbox) {
	return ((pt->x >= bbox->left) &&
			(pt->x <= bbox->right) &&
			(pt->y >= bbox->bottom) &&
			(pt->y <= bbox->top));
}

// lifted verbatim from bbox.cpp:60 — full file comes in a later plan
BOOL bbox_overlap (RECT *bbox1, RECT *bbox2) {
	return (!((bbox1->left > bbox2->right) ||
			  (bbox2->left > bbox1->right) ||
			  (bbox1->bottom > bbox2->top) ||
			  (bbox2->bottom > bbox1->top)));
}

// lifted verbatim from bbox.cpp:87 — full file comes in a later plan
void make_empty (SRECT *bbox) {
	bbox->Left = bbox->Bottom = LARGESHORT;
	bbox->Right = bbox->Top = -LARGESHORT;
}

// lifted verbatim from bbox.cpp:100 — full file comes in a later plan.
// Task 7: now referenced by the LIVE CBodyDouble/CBodySingle::Draw methods
// (bodycam.cpp), which map the body's SRECT bbox to a RECT for DrawBody.
RECT SRECTToRECT(SRECT &s) {
	RECT r;
	r.left = s.Left;
	r.top = s.Top;
	r.right = s.Right;
	r.bottom = s.Bottom;
	return r;
}

// --- Entry 2: panel.cpp static DATA member -- DELETED in Task 8 -------------
// CUnitPanelPage::m_unitWidth is now defined by the lifted panel.cpp at file
// scope (verbatim, same initializer). Defining it here too would violate ODR.

// --- Entry 3: CUserInfo constructibility (verbatim; R12a) --------------------

// lifted verbatim from userinfo.cpp:111 — full file comes in Plan 3
CUserInfo::CUserInfo()
{
	m_uRequests		= 0;
	m_flags			= 0;
	m_avatarID		= 0;
	m_uMsgCount		= 0;
	m_uIntervalStart= 0;
	m_bbValidUDI	= 0;
}

// lifted verbatim from userinfo.cpp:172 — full file comes in Plan 3.
// First non-inline virtual of CUserInfo: anchors the vtable. Self-contained
// (reads only members), so it is faithful to lift as-is.
CString & CUserInfo::GetScreenName() {
	if (m_flags & UF_SCREENNAME)
		return m_strScreenName;
	else
		return m_strName;
}

// lifted verbatim from userinfo.cpp:163 (Plan 4a Task 7 addition — see Entry 3
// comment above). Self-contained (reads/writes only members), so it is
// faithful to lift as-is.
void CUserInfo::SetScreenName(const char *name) {
	if (name) {
		m_strScreenName = name;
		m_flags |= UF_SCREENNAME;
	}
	else
		m_flags &= ~UF_SCREENNAME;
}

// --- Entry 4: pageview.cpp cross-file singles (R12a + R17) -------------------

// lifted verbatim from pageview.cpp:830 — full file NOT lifted (R16 compositor
// replaces pageview.cpp's Draw/scroll). R12(a): plain BOOL data global written
// only by the lifted panel.cpp AddLine.
BOOL g_bNewedPanel = FALSE;

// R17 reroute machinery (non-original glue): the CUnitPanelPage currently being
// composed, set by panel.cpp's AddLine/AddReaction at entry. Stands in for the
// original's GetView()->GetDocument()->m_pages.GetHead() (first page == the
// page being composed, single-page headless model — discovery §7).
static CUnitPanelPage* s_composingPage = NULL;
void ccSetComposingPage(CUnitPanelPage* p) { s_composingPage = p; }

// lifted from pageview.cpp:832 — arithmetic verbatim; ONLY the page-source read
// rerouted (R17) from GetView()->GetDocument()->m_pages to s_composingPage.
// Null composing-page -> count 0 -> TRUE (conservative "establishing, don't
// zoom" default, identical to the original's count<=1 branch).
BOOL Establishing() {
	int count = s_composingPage ? s_composingPage->m_panels.GetCount() : 0;
	if (count <= 1 || (!g_bNewedPanel && count <= 2)) return TRUE;
	else return FALSE;
}
