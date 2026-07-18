// cc_link_stubs.cpp — rule R12(b): trap stubs for lifted classes' virtual
// methods whose real bodies live in files scheduled for a later plan. These
// are declared (pure or non-pure virtual) in avatar.h/pe.h (byte-identical,
// untouched), but the files lifted so far don't define them — the real bodies
// are in panel.cpp (Plan 2 Task 8). Without SOME definition, the linker can't
// emit the vtable for CPanelElement, which the avatar loading chain's code
// instantiates even though none of these ever actually draw during the
// load-path smoke test.
//
// Each stub traps via ASSERT(0) (fires only in debug builds when actually
// called) and returns a safe failure/default value. None of these should
// ever execute during CAvatarX::LoadAvatar / cc_smoke_load_avatar — if one
// does, promote it to an R12(a) verbatim single-function lift in
// lifted_singles.cpp instead of leaving it stubbed.
//
// Task 7 lifted bodycam.cpp: the ten CBodySingle/CBodyDouble draw+bbox stubs
// that lived here are gone, replaced by their real (now LIVE) bodies. Task 8
// lifted panel.cpp: CPanelElement::SetBBox (the last original Plan-1 stub) is
// gone too — all thirteen Plan-1 link stubs are now retired. What remains here
// is: the intl.c CP-1252/MIME stubs, now the PERMANENT implementation of that
// seam (Plan 3 Task 2 Step 1 decision — CP-1252 is this port's permanent
// posture, not a placeholder awaiting an intl.c lift; see the comment at
// their definition below) and, added by Task 8, the
// CUserInfo::GetQualifiedName vtable-completeness trap (Plan 3, paired with
// the CUserInfo ctor/GetScreenName R12a singles in
// lifted_singles.cpp — the R17 session user table made CUserInfo linkable).

#include "mfc_compat.h"
#include "bbox.h"
#include "pe.h"
#include "dib.h"
#include "avatar.h"
#include "userinfo.h"  // Task 8: CUserInfo::GetQualifiedName R12(b) trap stub

// --- CPanelElement (pe.h) — real body now LIVE ------------------------------
// Task 8 lifted panel.cpp, which defines CPanelElement::SetBBox (panel.cpp:542,
// the real body). That R12(b) trap stub — the LAST original Plan-1 link stub —
// is gone. All thirteen of Plan 1's link stubs are now retired. (The remaining
// stubs in this file are Plan-2/Plan-3 debt registered by later tasks, not the
// original Plan-1 set.)

// --- CUserInfo (userinfo.h) — R12(b) vtable-completeness trap (Task 8) -------
// The R17 session user table (engine_context.h CCSessionUser) embeds a
// CUserInfo by value; lifted_singles.cpp lifts its default ctor + GetScreenName
// (R12a) to make it constructible + anchor the vtable. The one remaining vtable
// slot, GetQualifiedName, touches theApp.m_bShowIdentity (UI) in the original
// (userinfo.cpp:179) and is never called on a session user by any lifted code,
// so it is a trap stub here rather than a verbatim lift. Owning file
// userinfo.cpp (Plan 3, which lifts the real body and deletes this stub).
const char* CUserInfo::GetQualifiedName() {
    ASSERT(0);  // never called on session users (UI-only path)
    return GetScreenName();  // safe non-null fallback if ever reached in release
}

// GetMyNickName: MOVED to protsupp.cpp (Plan 3 Task 6, R19). CUserInfo::IsSelf()
// is an INLINE virtual in userinfo.h (strcmp(GetName(), GetMyNickName())).
// Being virtual it sits in the CUserInfo vtable, so emitting that vtable
// (anchored by the GetScreenName R12a single in lifted_singles.cpp) forces
// GetMyNickName to link even though no lifted payload-stage code calls
// IsSelf() on a session user. Task 6 replaces this R12(b) UI-stub ("" always)
// with a real forwarder to ccSessionOwnNick() (R19) in protsupp.cpp -- see
// the "GetMyNickName / IsSelf" note there for the full decision + why IsSelf()
// itself needed no change (it is byte-identical, untouched userinfo.h; only
// the free function it calls gained a real, non-UI body).

// --- CBodySingle / CBodyDouble (avatar.h) — real bodies now LIVE ------------
// Task 7 lifted bodycam.cpp, which defines all ten CBodySingle/CBodyDouble
// draw+bbox methods (IsSame/DrawBody/Draw/GetBodyBox/FlipBodyBox) that were
// R12(b)-stubbed here for Task 4. Those ten stubs are gone; their real bodies
// are in engine/bodycam.cpp now.

// --- CArc free-function helpers (DrawArc2/DashArc2, arc.cpp): the R12(b)
//     trap stubs that lived here (Task 4) are gone -- Task 6 lifted arc.cpp
//     (R1 only), which now defines both symbols. --------------------------

// --- INTL/MIME free functions (intl.c) — permanent CP-1252 implementation
//     (decision recorded Plan 3 Task 2 Step 1; originally landed Plan 2 Task 6
//     as an R12(b) stub pending an intl.c lift that this decision retires).
//     balloon.cpp's word-wrap path (ForceLineBreak / FindFurthestLineBreak /
//     BreakIntoLines) calls GetMime()/iBytesofChar()/FindSubStringForINTL-
//     ThatFits(). Their real bodies live in intl.c, which is NOT scheduled
//     for any lift in this roadmap (it is the East-Asian MIME/DBCS layer,
//     out of the engine-port scope, spec §4.5 "CP-1252 by default").
//
//     Plan 3 Task 2 makes CP-1252 the PERMANENT posture for this port (spec
//     §4.5; wire text stays bytes end-to-end, the engine never transcodes) --
//     these three are no longer "delete when intl.c lifts" placeholders, they
//     are the permanent CP-1252 implementation of this seam. No code changed
//     below, only this comment (the task brief's Step 1(b) is comment-only).
//
//     This port runs a single-byte CP-1252 codepage, so GetMime() returns
//     NULL exactly as intl.c's `void *GetMime() { return g_pMime; }` does
//     when no Far-East codepage is active (g_pMime stays NULL). That is the
//     SAME already-reviewed "no DBCS/MIME on this port" posture the shim
//     codified for IsDBCSLeadByte()->FALSE / CharNext() (Task 5). Two of the
//     three return their CP-1252-correct live values rather than trapping,
//     because balloon.cpp genuinely calls them on the load/layout path and
//     NULL/1 is the honest single-byte answer:
//       * GetMime()      -> NULL : reproduces g_pMime==NULL (no MIME active).
//       * iBytesofChar() -> 1    : intl.c's own `if (!g_pMime) return 1;`
//                                  branch value under the NULL-MIME state.
//     FindSubStringForINTLThatFits() is only ever reached when GetMime()!=NULL
//     (guarded at balloon.cpp:289 `if (GetMime())`), so on this port it is
//     genuinely unreachable -> a true ASSERT(0) trap; that stays permanent
//     too (no Far-East build is in scope for this port).
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
