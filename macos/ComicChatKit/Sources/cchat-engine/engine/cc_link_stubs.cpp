// cc_link_stubs.cpp — rule R12(b): trap stubs for lifted classes' virtual
// methods whose real bodies live in files scheduled for a later plan. These
// are declared (pure or non-pure virtual) in avatar.h/pe.h (byte-identical,
// untouched), but the ten files lifted for Task 4 don't define them — the
// real bodies are in bodycam.cpp (Plan 2) and panel.cpp/wmini.cpp/balloon.cpp
// (Plan 2/UI). Without SOME definition, the linker can't emit vtables for
// CBody/CBodySingle/CBodyDouble/CBodyUnary/CPanelElement, which the avatar
// loading chain's code (GetBodyFromEmotion, SetNeutral, DifferentTorso, etc.)
// instantiates even though CC_NO_RENDER means none of these ever actually
// draw during the load-path smoke test.
//
// Each stub traps via ASSERT(0) (fires only in debug builds when actually
// called) and returns a safe failure/default value. None of these should
// ever execute during CAvatarX::LoadAvatar / cc_smoke_load_avatar — if one
// does, promote it to an R12(a) verbatim single-function lift in
// lifted_singles.cpp instead of leaving it stubbed.
//
// Later plans (Plan 2 lifting bodycam.cpp/panel.cpp/wmini.cpp/balloon.cpp)
// delete these stubs as they lift the owning files.

#include "mfc_compat.h"
#include "bbox.h"
#include "pe.h"
#include "dib.h"
#include "avatar.h"

// --- CPanelElement (pe.h) — real bodies in panel.cpp / wmini.cpp / balloon.cpp ---

// owning file: wmini.cpp:881 / balloon.cpp:641 (defined twice in the original tree)
CPanelElement::CPanelElement(const CPanelElement& /*p*/) {
    ASSERT(0);
}

// owning file: panel.cpp:542
BOOL CPanelElement::SetBBox(int /*left*/, int /*bottom*/, int /*right*/, int /*top*/) {
    ASSERT(0);
    return FALSE;
}

// owning file: wmini.cpp:703 / balloon.cpp:647 (defined twice in the original tree)
void CPanelElement::GetBBox(RECT* /*r*/) {
    ASSERT(0);
}

// --- CBodySingle (avatar.h) — real bodies in bodycam.cpp ---

// owning file: bodycam.cpp:691
BOOL CBodySingle::IsSame(CBody* /*other*/) {
    ASSERT(0);
    return FALSE;
}

// owning file: bodycam.cpp:589
RECT CBodySingle::DrawBody(CDC* /*dc*/, RECT& /*clientArea*/, BOOL /*drawNimbus*/) {
    ASSERT(0);
    RECT r; r.left = r.top = r.right = r.bottom = 0;
    return r;
}

// owning file: bodycam.cpp:614
void CBodySingle::Draw(CDC* /*dc*/, POINT* /*ul*/, RECT* /*dmgRect*/) {
    ASSERT(0);
}

// owning file: bodycam.cpp:660
void CBodySingle::GetBodyBox(CPose* /*boy*/, RECT& /*clientRect*/, RECT& /*fullRect*/) {
    ASSERT(0);
}

// owning file: bodycam.cpp:583
void CBodySingle::FlipBodyBox(RECT& /*fullBox*/) {
    ASSERT(0);
}

// --- CBodyDouble (avatar.h) — real bodies in bodycam.cpp ---

// owning file: bodycam.cpp:685
BOOL CBodyDouble::IsSame(CBody* /*other*/) {
    ASSERT(0);
    return FALSE;
}

// owning file: bodycam.cpp:516
RECT CBodyDouble::DrawBody(CDC* /*dc*/, RECT& /*clientArea*/, BOOL /*drawNimbus*/) {
    ASSERT(0);
    RECT r; r.left = r.top = r.right = r.bottom = 0;
    return r;
}

// owning file: bodycam.cpp:578
void CBodyDouble::Draw(CDC* /*dc*/, POINT* /*ul*/, RECT* /*dmgRect*/) {
    ASSERT(0);
}

// owning file: bodycam.cpp:619
void CBodyDouble::GetBodyBox(CPose* /*head*/, CPose* /*body*/, RECT& /*clientRect*/, RECT& /*fullRect*/, RECT& /*headRect*/, RECT& /*torsoRect*/) {
    ASSERT(0);
}

// owning file: bodycam.cpp:447
void CBodyDouble::FlipBodyBox(RECT& /*fullBox*/, RECT& /*headBox*/, RECT& /*torsoBox*/) {
    ASSERT(0);
}
