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

// --- CPanelElement (pe.h) — real bodies in panel.cpp / balloon.cpp ---
// Task 6 lifted balloon.cpp, which defines the copy-ctor (balloon.cpp:641)
// and GetBBox (balloon.cpp:647) that were stubbed here (Task 4). Those two
// stubs are gone. SetBBox's owner is panel.cpp:542 (Task 8, still stubbed).

// owning file: panel.cpp:542
BOOL CPanelElement::SetBBox(int /*left*/, int /*bottom*/, int /*right*/, int /*top*/) {
    ASSERT(0);
    return FALSE;
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

// --- CArc free-function helpers (DrawArc2/DashArc2, arc.cpp): the R12(b)
//     trap stubs that lived here (Task 4) are gone -- Task 6 lifted arc.cpp
//     (R1 only), which now defines both symbols. --------------------------

// --- INTL/MIME free functions (intl.c) — Plan 2 Task 6, R12(b) variant.
//     balloon.cpp's word-wrap path (ForceLineBreak / FindFurthestLineBreak /
//     BreakIntoLines) calls GetMime()/iBytesofChar()/FindSubStringForINTL-
//     ThatFits(). Their real bodies live in intl.c, which is NOT scheduled
//     for any lift in this roadmap (it is the East-Asian MIME/DBCS layer,
//     out of the engine-port scope, spec §"CP-1252 by default").
//
//     This port runs a single-byte CP-1252 codepage, so GetMime() returns
//     NULL exactly as intl.c's `void *GetMime() { return g_pMime; }` does
//     when no Far-East codepage is active (g_pMime stays NULL). That is the
//     SAME already-reviewed "no DBCS/MIME on this port" posture the shim
//     codified for IsDBCSLeadByte()->FALSE / CharNext() (Task 5). So these
//     are R12(b) stubs, but two return their CP-1252-correct live values
//     rather than trapping, because balloon.cpp genuinely calls them on the
//     load/layout path and NULL/1 is the honest single-byte answer:
//       * GetMime()      -> NULL : reproduces g_pMime==NULL (no MIME active).
//       * iBytesofChar() -> 1    : intl.c's own `if (!g_pMime) return 1;`
//                                  branch value under the NULL-MIME state.
//     FindSubStringForINTLThatFits() is only ever reached when GetMime()!=NULL
//     (guarded at balloon.cpp:289 `if (GetMime())`), so on this port it is
//     genuinely unreachable -> a true ASSERT(0) trap. If a future Far-East
//     build lands, intl.c must be lifted and these three stubs deleted.
//     NOTE (reviewer): the non-trapping return values are a documented
//     judgment call at the R12(a)/R12(b) boundary -- lifting GetMime's
//     verbatim body would drag in the whole MIME machinery (g_pMime/SetMime/
//     SCRIPTINFO/codepage detection), so a faithful NULL-returning stub is
//     used instead. See p2-task-6-report.md for the full analysis.

extern "C" void* GetMime() { return nullptr; }  // intl.c:2682 (CP-1252: no MIME)

extern "C" int iBytesofChar(BYTE /*ch*/) { return 1; }  // intl.c:2699 (NULL-MIME branch)

// intl.c:506 — unreachable on this port (only called when GetMime() != NULL).
extern "C" BOOL FindSubStringForINTLThatFits(
    void* /*vMime*/, HDC /*hdc*/, LPCTSTR /*szString*/, int /*cbString*/,
    DWORD* /*prgdwFormatting*/, int /*cFormats*/, LPCTSTR /*szFixedPitchName*/,
    LPCTSTR /*szSymbolName*/, int* /*pcbFit*/, BOOL* /*pbHasBlankOrAlike*/,
    LPSIZE /*lpSize*/, int /*nMaxExtent*/) {
    ASSERT(0);
    return FALSE;
}
