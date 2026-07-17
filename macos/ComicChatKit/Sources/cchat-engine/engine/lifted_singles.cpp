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
//   Task 8). CUnitPanelPage::m_unitWidth is declared in panel.h (lifted
//   header-only) but defined in panel.cpp; balloon.cpp (GetFormatInfoCommon)
//   and fonts.cpp (UpdateTitleFonts) read it. Lifted verbatim with its
//   original initializer per the Task 6 amendment. Task 8 deletes it.
// --------------------------------------------------------------------------

#include "mfc_compat.h"
#include "bbox.h"
#include "vector2d.h"   // LARGESHORT (make_empty's SRECT sentinel)
#include "traj.h"       // CTraj/CSpline fwd chain used by balloon.h
#include "spline.h"     // CSpline (balloon.h members)
#include "pe.h"         // CPanelElement (balloon.h base class)
#include "dib.h"        // CDIB (avatar.h chain)
#include "avatar.h"     // CBody (balloon.h + panel.h)
#include "balloon.h"    // CFontInfo/CBalloon (panel.h's CUnitPanelPage decl needs them)
#include "backdrop.h"   // CBackDrop (panel.h's CPanel decl)
#include "panel.h"      // CUnitPanelPage (for m_unitWidth definition)

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

// --- Entry 2: panel.cpp static DATA member (verbatim, with initializer) -----

// lifted verbatim from panel.cpp:59 — full file comes in Plan 2 Task 8
int		CUnitPanelPage::m_unitWidth			= MINUNITPANELWIDTH-1;  // triggers a resize if not overridden
