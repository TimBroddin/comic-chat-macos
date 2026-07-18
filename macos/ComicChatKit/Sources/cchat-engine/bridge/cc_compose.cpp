// cc_compose.cpp — Plan 2 Task 10: the scripted-strip session C API and the
// headless compositor (the R16 replacement for CUnitPanelPage::Draw).
//
// This is the FIRST end-to-end driver of the lifted layout engine: it opens
// participant avatars, wires the session user table (the camera's talk-to
// graph), ingests scripted lines through the panel orchestrator
// (CUnitPanelPage::AddLine), and composites the finished page onto a cc_canvas
// with none of the original's MFC view-layer machinery (retained mem-DC,
// palette realize, CPageView::GetRetSec/AccountForScroll, SRCCOPY BitBlt
// scroll -- the [CM §4] scroll machinery that R16 explicitly does NOT port).
//
// ALL borrowed arithmetic cites file:line in the LIFTED tree
// (Sources/cchat-engine/engine/panel.cpp) per R16. NB the plan block cited the
// ORIGINAL panel.cpp line numbers (1232-1255 / 1268); the lifted copy shifted
// them (Task 8 added the R16/R11 wrap comment blocks above Draw/GetBBox), so
// every citation below is the LIFTED line, verified by reading the file.
//
// This file is BRIDGE code -- it modifies no lifted file (R16 requires the
// replacement live in bridge code, never a lifted file). It only CALLS lifted
// entry points, exactly as the panel selftest (cc_selftest.cpp) already does.

#include "comicchat.h"
#include "mfc_compat.h"
#include "engine_context.h"
// Geometry headers first: balloon.h/panel.h are header-only lifts that rely on
// the includer pulling in the types they name (CSpline, CTraj, CFormatInfo,
// SRECT, CPanelElement) before them -- same include order the panel selftest
// uses (cc_selftest.cpp:7-18).
#include "vector2d.h"
#include "traj.h"
#include "spline.h"
#include "format.h"
#include "bbox.h"
#include "pe.h"
#include "dib.h"
#include "avbfile.h"     // CAvatarFileStream (avatar/backdrop open path, Plan 1)
#include "avatar.h"      // CAvatarX / GetAvatar / InitializeAvatars
#include "avatario.h"    // InitializeAvatars / DestroyAvatars declarations
#include "balloon.h"     // CFontInfo (SetFonts needs the balloon header pulled in)
#include "backdrop.h"    // CChatBackdrop / CBackDropArt registries
#include "panel.h"       // CUnitPanelPage (orchestrator + statics)
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// --- lifted free functions this compositor calls, forward-declared here (they
//     live in the engine's .cpp files, not in any header -- the panel selftest
//     forward-declares the same family the same way). ------------------------
void InitializeAvatars();                 // avatar.cpp
void DestroyAvatars();                    // avatar.cpp
void InitializeEmotionRules();            // textpose.cpp
void DestroyEmotionRules();               // textpose.cpp
void ChatPreSendText(CString &str, int avID);  // textpose.cpp:120
int  SetBackDropAux(const char *backname, const char *pszRealName);  // backdrop.cpp:83
void InitializeBackDrops();               // backdrop.cpp:168
void DestroyBackDropArt();                // backdrop.cpp:318
void BytesToEmotion(CEmotion &em, BYTE emIndex, BYTE inIndex);  // avatario.cpp:88

// ============================================================================
// cc_strip
//
// Owns the composed page and the avatars it opened. The session user table
// (ccContext().session.users) holds the CUserInfo objects the camera reads; the
// avatar registry (avatar.cpp `avatars[]`) holds the registered CAvatarX*. A
// strip drives both process-global tables, so exactly one strip is meaningful
// at a time (the single-threaded contract in comicchat.h). destroy tears both
// down so a subsequent strip starts clean.

struct cc_strip {
    CUnitPanelPage* page = nullptr;
    std::vector<CAvatarX*> avatars;   // opened + IndexAvatar'd (registry owns them)
    bool emotionRulesInited = false;
};

// Test hook (declared in cc_selftest.cpp with C++ linkage): expose the live page
// so the selftest can compare cc_strip_get_size against CUnitPanelPage::GetBBox
// directly. Not part of the public C API in comicchat.h -- it's a C++-only view
// into the opaque handle, used only by the in-tree selftest.
CUnitPanelPage* cc_strip_page(cc_strip* s) {
    return s ? s->page : nullptr;
}

// Build the comics LOGFONT from the session face/size defaults. fonts.cpp's own
// arithmetic (fonts.cpp:82 `abs(logFont.lfHeight)/180.0`; :55 lfCharSet gate)
// shows the LOGFONT shape it consumes: lfFaceName, a NEGATIVE lfHeight (char
// height, GDI convention) in twips, lfWeight, lfCharSet. Twips = points * 20
// (MM_TWIPS, 20 twips/point) -- the same 12pt*20=240 the panel selftest uses
// (cc_selftest.cpp:1586 `lf.lfHeight = -240`).
static LOGFONT stripLogFontFromSession() {
    const CCSessionSettings& sess = ccContext().session;
    LOGFONT lf;
    memset(&lf, 0, sizeof(lf));
    strncpy(lf.lfFaceName, sess.comicsFontFace, LF_FACESIZE - 1);
    lf.lfFaceName[LF_FACESIZE - 1] = '\0';
    lf.lfHeight = -(sess.comicsFontPts * 20);  // negative = char height, 20 twips/pt
    lf.lfWeight = 400;                          // FW_NORMAL
    lf.lfCharSet = (BYTE)sess.charSet;
    return lf;
}

extern "C" cc_strip* cc_strip_create(void) {
    // DETERMINISM (Task 8 review amendment): seed the global rand() stream so
    // identical scripts yield byte-identical strips. CPanel::CPanel() draws its
    // per-panel m_seed from this stream (panel.cpp:604 `m_seed = rand()`), and
    // CUnitPanel::LayoutBalloons reseeds via srand(m_seed); without a fixed
    // starting point the whole balloon fit -- hence panel breaks -- depends on
    // whatever consumed rand() before. Documented next to the declaration in
    // comicchat.h.
    srand(0x5EED);

    // Fresh process-global tables: the avatar registry and the session user
    // table. (A prior strip's destroy already cleared these; re-init is
    // idempotent and also covers the very first strip.)
    InitializeAvatars();
    ccContext().session.clearUsers();

    cc_strip* s = new cc_strip();

    // Emotion rules: ChatPreSendText (add_line) infers a pose from the line text
    // via GetEmotionsFromString, which reads the process-global rule tables
    // InitializeEmotionRules() builds (textpose.cpp). Without this the tables are
    // empty -- lines still ingest, just always in the neutral pose. Init here so
    // the full text->pose pipeline runs; destroy tears it down.
    InitializeEmotionRules();
    s->emotionRulesInited = true;

    // Fonts: MakeBalloon's CFontInfo comes from CUnitPanelPage::m_fiWNormal,
    // built by SetFonts from the comics LOGFONT (fonts.cpp:40). Must be set
    // before any AddLine measures/lays out a balloon.
    LOGFONT lf = stripLogFontFromSession();
    if (!CUnitPanelPage::SetFonts(lf, ccContext().session.comicsColor)) {
        ccLog("cc_strip_create: SetFonts failed");
        DestroyEmotionRules();
        delete s;
        return nullptr;
    }

    // Sane unit-panel geometry: the statics default to MINUNITPANEL*-1 sentinels
    // (panel.cpp:73-74, "triggers a resize if not overridden"). The headless
    // engine has no view to drive that resize, so seed the minimum unit size,
    // exactly as the panel selftest does (cc_selftest.cpp:1589-1590).
    CUnitPanelPage::SetUnitPanelWidth(MINUNITPANELWIDTH);
    CUnitPanelPage::SetUnitPanelHeight(MINUNITPANELHEIGHT);
    // Plan 4a Task 5 fix: panels-per-row is ALSO process-global static state
    // (panel.cpp:70, default 2), and cc_strip_set_panel_geometry can now change
    // it. Re-seed it here too so a fresh strip always starts from the
    // documented default (2/row) regardless of what a PRIOR strip's geometry
    // call left the static at -- otherwise cc_strip_create's "fresh strip"
    // guarantee would hold for width/height but silently leak panels-per-row
    // across strips (caught by cross-test contamination: a geometry test
    // running before the strip snapshot test broke its golden log, which
    // assumes 2/row).
    CUnitPanelPage::SetUnitPanelsPerRow(2);

    // The page. m_doc == nullptr is safe headless -- it is only dereferenced in
    // the R11-wrapped RefreshPanelN (a no-op under CC_NO_UI); see the panel
    // selftest's makePage() note (cc_selftest.cpp:1698).
    s->page = new CUnitPanelPage(nullptr);
    s->page->m_topY = s->page->m_leftX = 0;
    return s;
}

extern "C" void cc_strip_destroy(cc_strip* s) {
    if (!s) return;
    delete s->page;   // cascades: panels -> elements/bodies (CPage::~CPage).
                      // Deleted FIRST so no live CBackDrop references a backID
                      // before DestroyBackDropArt frees the registry below, and
                      // so no live CPanelElement text draw touches the
                      // CFontInfo statics DestroyFonts frees below.
    // Tear down the process-global tables this strip drove so the next strip
    // (or another global-state selftest) starts clean.
    DestroyAvatars();
    ccContext().session.clearUsers();
    ccContext().session.backdropID = 0;    // clear the inherited backdrop id
    DestroyBackDropArt();                  // frees BDFileRecs + backdrop art cache
    if (s->emotionRulesInited) DestroyEmotionRules();
    // Fonts (final review): SetFonts (cc_strip_create) leaks CFont/CFontInfo
    // statics -- fonts.cpp:144 DestroyFonts frees the m_fonts/m_fontInfos
    // lists and nulls the statics. Placed last, alongside the other global
    // teardowns, since page (already deleted above) is the only consumer of
    // the CFontInfo statics -- nothing after this point may touch them.
    CUnitPanelPage::DestroyFonts();
    delete s;
}

// Open the avatar, register it, create + wire its session user. Mirrors the
// panel selftest's panelLoadAvatar (cc_selftest.cpp:1495) + panelWireUser
// (:1506): open via the Plan 1 stream/LoadAvatar path, IndexAvatar to register
// into avatars[] (so GetAvatar(id) finds it), addUser to create the session
// CUserInfo, and set av->m_userInfo -> that entry (the Task 8 WIRING INVARIANT:
// every participant's m_userInfo points at its session-table CUserInfo, which
// is what makes the camera's talk-to comparisons + userFromTalkTo lookup exact).
extern "C" int32_t cc_strip_add_participant(cc_strip* s, const char* nick,
                                            const char* avb_path) {
    if (!s || !avb_path) return -1;
    try {
        CAvatarFileStream* pStream = new CAvatarFileStream(avb_path);
        CAvatarX* av = CAvatarX::LoadAvatar(pStream);
        if (!av) { delete pStream; return -1; }
        av->SetStream(pStream);        // avatar now owns the stream (Plan 1 pattern)
        av->IndexAvatar();             // assigns m_avatarID + registers into avatars[]

        UINT id = av->m_avatarID;
        // Optional nick: the camera does not need it (it reads the talk-to graph
        // off CUserInfo, not the name), but GetAvatar(const char*) name lookup +
        // any future title/starring use it, so honor it when provided.
        if (nick && nick[0]) {
            free(av->m_name);
            av->m_name = strdup(nick);
        }

        CUserInfo* pui = ccContext().session.addUser(id);
        if (!pui) { return -1; }       // table full
        pui->SetAvatarID((USHORT)id);  // CUserInfo::GetAvatarID() -> this id
        av->m_userInfo = pui;          // WIRING INVARIANT

        s->avatars.push_back(av);
        return (int32_t)id;
    } catch (...) {
        return -1;
    }
}

// Plan 4a Task 6: switch an EXISTING participant to a different avatar,
// mid-strip. Mirrors cc_strip_add_participant's load half above verbatim
// (open via CAvatarFileStream -> LoadAvatar -> IndexAvatar, registering the
// new avatar into avatars[] under its own fresh id so GetAvatar(newID) finds
// it) but then, instead of addUser'ing a NEW session user, re-points the
// PARTICIPANT'S EXISTING CUserInfo at the new avatar -- exactly the original
// SetUserAvatarID two-liner (userinfo.cpp:38-41):
//   pui->SetAvatarID(avID); GetAvatar(avID)->m_userInfo = pui;
// i.e. the session user (and therefore the participant id the caller already
// has) is unchanged; only which CAvatarX it points at moves.
//
// NO RETRO-RECOMPOSE (binding, matches the original): ChangeAvatarEntry::
// Execute (histent.cpp:368-413) calls this exact SetUserAvatarID pairing and
// nothing else -- it does NOT walk back through history and re-render
// existing panels/bodies with the new avatar. Panels already laid out (their
// CBody snapshots were built from the OLD CAvatarX at AddLine time) keep
// rendering the old avatar; only lines added AFTER this call (whose
// CPanel::FetchSpeaker does a fresh GetAvatar(uID) lookup, panel.cpp:667-668)
// pick up the new one. This function only rewires the registry/session
// pointers -- it deliberately does not touch s->page or any existing panel.
extern "C" int32_t cc_strip_set_participant_avatar(cc_strip* s, int32_t participant,
                                                   const char* avb_path) {
    if (!s || !avb_path || participant <= 0) return -1;

    CUserInfo* pui = ccContext().session.lookupUser((UINT)participant);
    if (!pui) return -1;   // unknown participant id

    try {
        CAvatarFileStream* pStream = new CAvatarFileStream(avb_path);
        CAvatarX* av = CAvatarX::LoadAvatar(pStream);
        if (!av) { delete pStream; return -1; }
        av->SetStream(pStream);        // avatar now owns the stream (Plan 1 pattern)
        av->IndexAvatar();             // assigns m_avatarID + registers into avatars[]

        UINT newID = av->m_avatarID;
        pui->SetAvatarID((USHORT)newID);   // SetUserAvatarID's first line
        av->m_userInfo = pui;              // SetUserAvatarID's second line

        s->avatars.push_back(av);
        return 0;
    } catch (...) {
        return -1;
    }
}

// Register the backdrop so every subsequently-created panel inherits it.
// Resolution story (all lifted, live code -- no un-lifted registry, so no
// R12/R17 escalation): CPanel::CPanel() reads ccContext().session.backdropID
// into m_backDrop.m_backID (panel.cpp:608, Task 8 R17). At compose, CBackDrop::
// Draw calls GetBackDropArtFromID(m_backID) -> BackDropArtFromBackID, which
// builds "ccContext().backdropDir/rec->filename" and loads it via
// CChatBackdrop::LoadBackdrop (backdrop.cpp:254-257). So we must (a) point
// backdropDir at the file's directory, (b) register a BDFileRec whose filename
// is the basename, and (c) store the returned id in session.backdropID.
//   InitializeBackDrops() (backdrop.cpp:168, LIVE) seeds backRecS[0]=NULL so
//   real ids start at 1 (id 0 == "no backdrop", GetBackDropArtFromID:276).
//   SetBackDropAux(filename, NULL) (backdrop.cpp:83, LIVE) adds the BDFileRec
//   and returns its index. NB SetBackDrop (the doc/download variant,
//   backdrop.cpp:102) is #ifndef CC_NO_UI (compiled OUT) -- SetBackDropAux is
//   the correct headless registration entry.
extern "C" int32_t cc_strip_set_backdrop(cc_strip* s, const char* bgb_path) {
    if (!s || !bgb_path) return -1;

    // Split bgb_path into directory + basename.
    std::string path(bgb_path);
    std::string dir, filename;
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        dir = ".";
        filename = path;
    } else {
        dir = path.substr(0, slash);
        filename = path.substr(slash + 1);
    }
    if (filename.empty()) return -1;

    ccContext().backdropDir = dir.c_str();  // R2 reroute target read by BackDropArtFromBackID

    InitializeBackDrops();                  // ensure backRecS[0]==NULL (id 0 reserved)
    int backID = SetBackDropAux(filename.c_str(), NULL);
    if (backID <= 0) return -1;             // 0 == "no backdrop"; failed registration

    ccContext().session.backdropID = (UINT)backID;  // every new CPanel inherits it
    return 0;
}

// Ingest one scripted line. Replicates the original ingestion chain [CM §1]:
//   1. set the speaker's m_udi.m_talkTos from `addressees` (the camera's
//      facing/order input). Truncated-key writes per Task 8's convention
//      ((DWORD)(uintptr_t)&users[i].info -- the same low-32 key EvalPair
//      compares on and userFromTalkTo reverses). Mirrors panelSetTalksTo
//      (cc_selftest.cpp:1514).
//   2. ChatPreSendText(text, speaker) (textpose.cpp:120): infer the emotion
//      pose from the line and UpdateBody the speaker's avatar.
//   3. page->AddLine(speaker, text, modes, NULL /*formatting*/, NULL /*url*/)
//      -- the exact lifted AddLine signature (panel.h:138). CC_MODE_* mirror
//      BM_* bit-for-bit, so modes passes straight through as the USHORT uModes.
extern "C" int32_t cc_strip_add_line(cc_strip* s, int32_t speaker,
                                     const char* text_bytes, uint32_t modes,
                                     const int32_t* addressees, int32_t n_addr) {
    // NULL addressees with nonzero n_addr is malformed input (final review):
    // rather than dereference addressees[i] below, reject it outright,
    // consistent with this function's existing error style (return -1 for
    // any invalid argument, e.g. the !text_bytes check on this same line).
    if (!s || !s->page || !text_bytes || speaker <= 0) return -1;
    if (!addressees && n_addr != 0) return -1;

    CUserInfo* spk = ccContext().session.lookupUser((UINT)speaker);
    if (!spk) return -1;   // unknown speaker id

    // (1) rebuild the speaker's talk-to graph from the addressees.
    spk->m_udi.m_talkTos.RemoveAll();
    for (int32_t i = 0; i < n_addr; i++) {
        CUserInfo* to = ccContext().session.lookupUser((UINT)addressees[i]);
        if (to) spk->m_udi.m_talkTos.Add((DWORD)(uintptr_t)to);
    }

    // (2) emotion inference (mutates the speaker avatar's neutral body).
    CString text(text_bytes);
    ChatPreSendText(text, speaker);

    // (3) orchestrator ingestion. AddLine returns BOOL (TRUE == success).
    BOOL ok = s->page->AddLine((UINT)speaker, (const char*)text, (USHORT)modes, NULL, NULL);
    return ok ? 0 : -1;
}

// Plan 3 Task 9: the wire-received counterpart of cc_strip_add_line -- ports
// the original's SayEntry::Execute "cooked" branch (histent.cpp:80-108) rather
// than ChatPreSendText's text-inference heuristic (textpose.cpp:120), so an
// explicitly-decoded annotation block drives the speaker's rendered pose
// directly. histent.cpp's own gate:
//   if (m_udi.m_bbCooked) {
//       if (!(av->m_flags & OTHERMAPPED)) av->SetIndices(m_chExpr, m_chGest, m_bbReq);
//       else { BytesToEmotion(expr, ...); BytesToEmotion(gest, ...); av->SetEmotions(expr, gest); }
//   }
//   ProcessLine(...) -> if (!bbCooked) ChatPreSendText(...);   // chatdoc.cpp:451-452
// i.e. inference (ChatPreSendText) runs ONLY when NOT cooked -- ported here as
// an if/else between the pose-from-annotations branch and the existing
// pose-from-text-inference branch, so uncooked/no-annotation callers still get
// exactly cc_strip_add_line's original behavior.
extern "C" int32_t cc_strip_add_line_cooked(cc_strip* s, int32_t speaker,
                                            const char* text_bytes, uint32_t modes,
                                            const int32_t* addressees, int32_t n_addr,
                                            const cc_annotations* ann) {
    if (!s || !s->page || !text_bytes || speaker <= 0) return -1;
    if (!addressees && n_addr != 0) return -1;

    CUserInfo* spk = ccContext().session.lookupUser((UINT)speaker);
    if (!spk) return -1;   // unknown speaker id

    // (1) rebuild the speaker's talk-to graph from the addressees (identical
    // to cc_strip_add_line).
    spk->m_udi.m_talkTos.RemoveAll();
    for (int32_t i = 0; i < n_addr; i++) {
        CUserInfo* to = ccContext().session.lookupUser((UINT)addressees[i]);
        if (to) spk->m_udi.m_talkTos.Add((DWORD)(uintptr_t)to);
    }

    // (2) pose: explicit annotations (cooked) win over text inference.
    CAvatarX* av = GetAvatar((USHORT)speaker);
    if (ann && ann->cooked && av) {
        if (!(av->m_flags & OTHERMAPPED)) {
            // Ordinary avatar: the decoded pose indices map directly onto
            // SetIndices' (face, torso/gesture, requested) triple -- exactly
            // histent.cpp:97's av->SetIndices(m_chExpr, m_chGest, m_bbReq).
            av->SetIndices((CHAR)ann->face_pose, (CHAR)ann->gesture_pose,
                           (BYTE)ann->requested);
        } else {
            // OTHERMAPPED avatar: closest-matching emotion/intensity pair,
            // mirroring histent.cpp:99-104's BytesToEmotion+SetEmotions pair
            // (also the exact pattern cc_selftest.cpp:2934-2936 already uses
            // for the encoder-side test avatar).
            CEmotion faceEm, gestEm;
            BytesToEmotion(faceEm, (BYTE)ann->face_emotion, (BYTE)ann->face_intensity);
            BytesToEmotion(gestEm, (BYTE)ann->gesture_emotion, (BYTE)ann->gesture_intensity);
            av->SetEmotions(faceEm, gestEm);
        }
    } else {
        // Not cooked (or no annotations at all): fall back to the original
        // text-inference path, unchanged from cc_strip_add_line.
        CString text(text_bytes);
        ChatPreSendText(text, speaker);
    }

    // (3) orchestrator ingestion -- identical to cc_strip_add_line.
    BOOL ok = s->page->AddLine((UINT)speaker, text_bytes, (USHORT)modes, NULL, NULL);
    return ok ? 0 : -1;
}

extern "C" int32_t cc_strip_panel_count(const cc_strip* s) {
    if (!s || !s->page) return 0;
    return (int32_t)s->page->m_panels.GetCount();
}

// Page bounding box in twips. Transliterated from CUnitPanelPage::GetBBox
// (panel.cpp:1359-1367): width = right-left, height = top-bottom (y-up, so top
// > bottom for a non-empty page). Reuses the lifted GetBBox directly rather than
// re-deriving the row/column arithmetic -- it is live code (not the R16'd Draw),
// so calling it is exact.
extern "C" void cc_strip_get_size(const cc_strip* s, int32_t* out_w, int32_t* out_h) {
    int32_t w = 0, h = 0;
    if (s && s->page && s->page->m_panels.GetCount() > 0) {
        RECT bbox;
        s->page->GetBBox(&bbox);        // panel.cpp:1359
        w = bbox.right - bbox.left;
        h = bbox.top - bbox.bottom;     // y-up: content grows toward -y
    }
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

// Plan 4a Task 5: panel geometry API. Thin wrappers over
// CUnitPanelPage::SetUnitPanelWidth/SetUnitPanelHeight/SetUnitPanelsPerRow
// (engine/panel.h:156-158 -- note SetUnitPanelWidth cascades UpdateTitleFonts,
// exactly as the original CPageView::SetPanelsWide's caller-visible behavior;
// that's expected, not worked around) and the interstice statics (engine/
// panel.cpp:70-76, read-only here -- the original exposes no setter for them
// either). FRESH STRIP ONLY: rejects once a line has been added (panel count
// > 0), mirroring the original's always-reflow-via-ResetExistingPanels design
// (pageview.cpp:1110-1125) without a headless equivalent of that reflow.
extern "C" int32_t cc_strip_set_panel_geometry(cc_strip* s, int32_t unit_w_twips,
                                               int32_t unit_h_twips,
                                               int32_t panels_per_row) {
    if (!s || !s->page) return -1;
    if (s->page->m_panels.GetCount() > 0) return 1;  // lines already added -- reject
    CUnitPanelPage::SetUnitPanelWidth(unit_w_twips);   // cascades UpdateTitleFonts
    CUnitPanelPage::SetUnitPanelHeight(unit_h_twips);
    CUnitPanelPage::SetUnitPanelsPerRow(panels_per_row);
    return 0;
}

extern "C" void cc_strip_get_panel_geometry(const cc_strip* s, int32_t* unit_w,
                                            int32_t* unit_h, int32_t* per_row,
                                            int32_t* h_interstice,
                                            int32_t* v_interstice) {
    // Consistent with cc_strip_get_size: a NULL/destroyed handle reads as all
    // zeros rather than silently exposing whatever the process-global statics
    // (there is only ever one live strip, per the ONE STRIP AT A TIME contract,
    // but a caller passing NULL/a stale handle shouldn't see live state anyway).
    int32_t w = 0, h = 0, pr = 0, hi = 0, vi = 0;
    if (s && s->page) {
        w = CUnitPanelPage::m_unitWidth;
        h = CUnitPanelPage::m_unitHeight;
        pr = CUnitPanelPage::m_panelsPerRow;
        hi = CUnitPanelPage::m_hInterstice;
        vi = CUnitPanelPage::m_vInterstice;
    }
    if (unit_w) *unit_w = w;
    if (unit_h) *unit_h = h;
    if (per_row) *per_row = pr;
    if (h_interstice) *h_interstice = hi;
    if (v_interstice) *v_interstice = vi;
}

// ============================================================================
// Headless compositor — R16 replacement for CUnitPanelPage::Draw (panel.cpp:
// 1281, wrapped #ifndef CC_NO_UI). Transliterates the panel-origin WALK of that
// function (panel.cpp:1320-1343) WITHOUT the memDC/BitBlt/palette/scroll
// machinery [CM §4]: no CreateCompatibleDC, no SelectPalette/RealizePalette, no
// GetRetSec/AccountForScroll, no SRCCOPY BitBlt. Each panel draws directly onto
// the target canvas at its row/column origin.
//
// Origin walk (every borrowed expression cited to the LIFTED line):
//   - truePanelsPerRow = m_panelsPerRow (screen path; the print branch of
//     panel.cpp:1320 `dc->IsPrinting() ? m_printPanelsPerRow : m_panelsPerRow`
//     is dropped -- headless composition is always the screen path).
//   - panelRect = (0, 0, m_unitWidth, -m_unitHeight)         panel.cpp:1321
//   - loc starts at (0,0)                                    panel.cpp:1323-1324
//   - per panel: SetWindowOrg(-loc) maps panel-local (0,0) -> loc, then
//     panel->Draw(&dc, &loc, &dmg). The original computed a damageRel in panel
//     coords (panel.cpp:1330) and bbox_overlap-culled (1331); headless we damage
//     the WHOLE panel unconditionally (dmg = panelRect) so every panel composes
//     in full -- there is no scroll viewport to cull against.
//   - row/column advance                                     panel.cpp:1338-1342
//
// SetWindowOrg sign: Task 3's selftest LOCKS org = -loc (mapping logical loc to
// output means the window origin is its negation; cc_selftest_dc case (c),
// cc_selftest.cpp:501-503). Verified against that selftest, not re-derived.
//
// Adapter binding: the plan block wrote `CDC dc; dc.Attach(canvas);`. The lifted
// reality has no default CDC ctor and no Attach -- the adapter exposes
// `explicit CDC(cc_canvas*)` (mfc_compat.h:635), which every existing call site
// uses (e.g. cc_selftest_dc's `CDC dc(rec.handle())`). So this binds via the
// constructor directly; no Attach is added (cleaner + adapter-consistent, and
// the plan explicitly allows adjusting where the lifted reality differs).
extern "C" int32_t cc_strip_compose(cc_strip* s, cc_canvas* canvas) {
    if (!s || !s->page || !canvas) return -1;
    CDC dc(canvas);                       // adapter bound to the target canvas
    CUnitPanelPage* page = s->page;

    int truePanelsPerRow = CUnitPanelPage::m_panelsPerRow;   // panel.cpp:1320 (screen path)
    RECT panelRect;
    SetRect(&panelRect, 0, 0,
            CUnitPanelPage::m_unitWidth, -CUnitPanelPage::m_unitHeight);  // panel.cpp:1321

    int panelCount = 0;
    POINT loc; loc.x = loc.y = 0;                            // panel.cpp:1323-1324
    POSITION pos = page->m_panels.GetHeadPosition();
    while (pos != NULL) {                                    // panel.cpp:1326
        CPanel* panel = (CPanel*)page->m_panels.GetNext(pos);
        panelCount++;
        dc.SetWindowOrg(-loc.x, -loc.y);   // panel-local (0,0) -> loc (Task 3 sign)
        RECT dmg = panelRect;              // full per-panel damage (no scroll cull)
        panel->Draw(&dc, &loc, &dmg);      // panel.cpp:1332 (panel draws at local 0,0)
        dc.SetWindowOrg(0, 0);
        if (panelCount % truePanelsPerRow == 0) {            // panel.cpp:1338-1342
            loc.x = 0;
            loc.y -= CUnitPanelPage::m_unitHeight + CUnitPanelPage::m_hInterstice;
        } else {
            loc.x += CUnitPanelPage::m_unitWidth + CUnitPanelPage::m_vInterstice;
        }
    }
    return 0;
}
