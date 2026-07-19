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
void EmotionToBytes(CEmotion &em, BYTE &emotion, BYTE &intensity);  // avatario.cpp:74
BYTE ByteToIndex(BYTE);  // protsupp.cpp -- inverse of IndexToByte(v) = v + '0'

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
    // Plan 4a Task 7 fix (belt-and-braces, same shape as Task 5's
    // SetUnitPanelsPerRow(2) reseed above): comicsTitle/selfParticipant are
    // ALSO process-global session state a strip can set (set_title/set_self).
    // cc_strip_destroy already clears both; re-clearing here too means a
    // fresh strip's "no title, no self" guarantee holds even if a caller
    // somehow reaches this without a matching prior destroy.
    ccContext().session.comicsTitle[0] = '\0';
    ccContext().session.selfParticipant = 0;

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
    // Plan 4a Task 7 fix (golden-preservation leak): comicsTitle/selfParticipant
    // are session-global state too (like backdropID above) -- left un-reset, a
    // strip that called set_title/set_self (e.g. this task's own selftest)
    // would leak a non-empty title / non-zero self into the NEXT strip created
    // in the same process, corrupting every later cc_strip_add_participant's
    // conditional UpdateTitle gate (comicsTitle[0] != '\0') and turning panel 0
    // of an UNRELATED strip into a surprise title panel -- exactly the failure
    // caught by strip-golden.txt going non-byte-identical when this reset was
    // missing (verified: without this line, running this task's new selftest
    // before StripTests in the same process broke the frozen snapshot).
    ccContext().session.comicsTitle[0] = '\0';
    ccContext().session.selfParticipant = 0;
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
//
// Plan 4a Task 7: the original's member-join site calls UpdateTitle so a new
// member's starring row appears immediately (protsupp.cpp:461,
// AddToMembersList's `if (doc->m_bComicView) UpdateTitle(doc);`). Ported here
// GATED ON A TITLE ALREADY BEING SET (comicsTitle non-empty), NOT on
// comicView alone: the original ALSO unconditionally builds a title panel at
// document-creation time, before any participant/line ever exists
// (chatdoc.cpp:225's InitMyDocument -- `firstPage->AddTitle(GetComicsTitle())`
// -- with GetComicsTitle() falling back to GetRandomTitle() if never set).
// This port has no "document creation" moment to anchor that unconditional
// first build to, and the brief's OWN hard gate is that every strip which
// never calls cc_strip_set_title (every strip predating this task, incl. the
// frozen strip-golden.txt snapshot) must render BYTE-IDENTICALLY -- panel 0
// must stay whatever the first LINE built, not an uninvited title panel.
// Gating on "title already set" reproduces the member-join refresh exactly
// for every caller that opted in via set_title, while leaving every caller
// that never does untouched.
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
        // off CUserInfo, not the name), but GetAvatar(const char*) name lookup
        // needs it on the AVATAR (av->m_name).
        if (nick && nick[0]) {
            free(av->m_name);
            av->m_name = strdup(nick);
        }

        CUserInfo* pui = ccContext().session.addUser(id);
        if (!pui) { return -1; }       // table full
        pui->SetAvatarID((USHORT)id);  // CUserInfo::GetAvatarID() -> this id
        // Plan 4a Task 7 fix: the starring credits (AddStars/CStarLabel) read
        // the SPEAKER's nickname via CAvatarX::GetScreenName ->
        // pui->GetScreenName() -- i.e. off the CUserInfo, not av->m_name above
        // (a completely separate field on a completely separate object). This
        // was never wired before this task because nothing lifted read it
        // (AddStars was R11-deferred); without it every starring row renders
        // an EMPTY nickname (CUserInfo::GetScreenName() falls back to
        // m_strName, which defaults to "" -- userinfo.h's CUserInfo ctor never
        // sets it either). SetName also covers the UF_SCREENNAME/
        // DecodeNickForScreen quoted-nick convention (userinfo.h:127-131),
        // consistent with how a wire-received nick would arrive.
        if (nick && nick[0]) pui->SetName(nick);
        av->m_userInfo = pui;          // WIRING INVARIANT

        s->avatars.push_back(av);

        if (s->page && ccContext().session.comicsTitle[0] != '\0')
            s->page->UpdateTitle();    // R17 (see header comment): member-join refresh, title-opt-in only

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

        // Live-fix (Plan 4b live-fix 3): refresh the title/starring credits so
        // the starring row's avatar icon follows the switch -- the original's
        // ChangeAvatarEntry::Execute does exactly this on its HM_LIVE path
        // (histent.cpp:402, `UpdateTitle()`), and AddStars reads
        // `pui->GetAvatarID()` (panel.cpp:538) so it picks up the new avatar.
        // Gated on a title having been set (title-opt-in), matching
        // cc_strip_add_participant's own member-join UpdateTitle guard just
        // above -- without a title there is no starring panel to refresh.
        if (s->page && ccContext().session.comicsTitle[0] != '\0')
            s->page->UpdateTitle();

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

    // Live-fix (Plan 4b live-fix 3): resolve the participant's CURRENT avatar
    // id and drive every avatar-keyed step from it, NOT from the raw `speaker`
    // participant id -- this is the EXACT original contract. The original's
    // SayEntry::Execute passes `m_pui->GetAvatarID()` (not a user id) to
    // ProcessLine -> AddLine (histent.cpp:108), and poses via
    // `GetAvatar(m_pui->GetAvatarID())` (histent.cpp:90). The two are equal at
    // add-participant time (`addUser(id)` seeds them so), but
    // `cc_strip_set_participant_avatar` (SetUserAvatarID) re-points
    // `pui->SetAvatarID(newID)` at a freshly-loaded avatar under a NEW id while
    // the caller's participant id is unchanged -- so a post-switch `speaker`
    // still names the OLD avatar. The pre-fix bridge passed `speaker` straight
    // into GetAvatar/AddLine, so post-switch lines posed + cloned the OLD
    // avatar's body -- rendering the pre-switch character forever (Tim's live
    // "still renders Anna" report). `CPanel::FetchSpeaker`/`AvatarInPanel` key
    // on `m_avatarID`, so the current avatar id is exactly the value the panel
    // machinery expects.
    UINT avId = spk->GetAvatarID();

    // (1) rebuild the speaker's talk-to graph from the addressees.
    spk->m_udi.m_talkTos.RemoveAll();
    for (int32_t i = 0; i < n_addr; i++) {
        CUserInfo* to = ccContext().session.lookupUser((UINT)addressees[i]);
        if (to) spk->m_udi.m_talkTos.Add((DWORD)(uintptr_t)to);
    }

    // (2) emotion inference (mutates the speaker avatar's neutral body).
    // ChatPreSendText takes an AVATAR id (GetAvatar(avID), textpose.cpp:125).
    CString text(text_bytes);
    ChatPreSendText(text, (int)avId);

    // Plan 4a Task 7 (R17): the original's CChatDoc::ProcessLine tallies every
    // real utterance onto the speaker's avatar (chatdoc.cpp:463-471 --
    // TallySpeech(uID) -> GetAvatar(id)->m_nSends++), gated on the SAME
    // "<Brk>" sentinel AddLine itself checks (panel.cpp:1141, a panel-break
    // debug marker, not a spoken line) so a break doesn't inflate the starring
    // order. AddStarsAux (panel.cpp) reads m_nSends to rank the starring
    // credits by who spoke most; nothing populated it before this task since
    // the title/starring path was entirely R11-deferred.
    if (strcmp(text_bytes, "<Brk>") != 0) {
        CAvatarX* spkAv = GetAvatar((USHORT)avId);
        if (spkAv) spkAv->m_nSends++;
    }

    // (3) orchestrator ingestion. AddLine's uID is the AVATAR id every CPanel
    // step keys on -- pass the current avatar id (matches histent.cpp:108).
    BOOL ok = s->page->AddLine((UINT)avId, (const char*)text, (USHORT)modes, NULL, NULL);
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

    // Live-fix (Plan 4b live-fix 3): drive every avatar-keyed step off the
    // participant's CURRENT avatar id -- see cc_strip_add_line's identical
    // `avId` note for the full rationale (the original's SayEntry::Execute
    // passes `m_pui->GetAvatarID()` to AddLine, histent.cpp:108, not a fixed
    // user id; post-`cc_strip_set_participant_avatar` the two diverge, and the
    // pre-fix bridge kept rendering the OLD avatar -- Tim's "still renders
    // Anna" report).
    UINT avId = spk->GetAvatarID();

    // (1) rebuild the speaker's talk-to graph from the addressees (identical
    // to cc_strip_add_line).
    spk->m_udi.m_talkTos.RemoveAll();
    for (int32_t i = 0; i < n_addr; i++) {
        CUserInfo* to = ccContext().session.lookupUser((UINT)addressees[i]);
        if (to) spk->m_udi.m_talkTos.Add((DWORD)(uintptr_t)to);
    }

    // (2) pose: explicit annotations (cooked) win over text inference.
    CAvatarX* av = GetAvatar((USHORT)avId);
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
        // text-inference path. ChatPreSendText takes an AVATAR id.
        CString text(text_bytes);
        ChatPreSendText(text, (int)avId);
    }

    // Plan 4a Task 7 (R17): TallySpeech's m_nSends++ (see cc_strip_add_line's
    // identical comment/site) -- `av` is already resolved above for the pose
    // branch, so reuse it rather than a second GetAvatar lookup.
    if (strcmp(text_bytes, "<Brk>") != 0 && av) av->m_nSends++;

    // (3) orchestrator ingestion. AddLine's uID is the AVATAR id -- pass the
    // current avatar id (matches histent.cpp:108).
    BOOL ok = s->page->AddLine((UINT)avId, text_bytes, (USHORT)modes, NULL, NULL);
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

// ============================================================================
// Plan 4a Task 7: title/starring panel (un-R11 lift of AddTitle/UpdateTitle/
// AddStars/AddStarsAux + CStarLabel::Draw -- see comicchat.h's doc comment for
// the full contract).

// cc_strip_set_title: store the title, then call the ALREADY-LIFTED
// UpdateTitle -- its own body IS the "AddTitle if no panels yet, else rebuild
// the existing title panel in place" decision the brief describes
// (panel.cpp: `if (m_panels.IsEmpty()) AddTitle(...); else { rebuild; }`), so
// this bridge entry doesn't re-implement that branch itself.
extern "C" int32_t cc_strip_set_title(cc_strip* s, const char* title_bytes) {
    if (!s || !s->page || !title_bytes) return -1;
    CCSessionSettings& sess = ccContext().session;
    strncpy(sess.comicsTitle, title_bytes, sizeof(sess.comicsTitle) - 1);
    sess.comicsTitle[sizeof(sess.comicsTitle) - 1] = '\0';
    s->page->UpdateTitle();
    return 0;
}

// cc_strip_set_self: record the participant as the strip's own avatar (R17:
// ccContext().session.selfParticipant, replacing the original's
// GetChatDoc()->m_myAvatarID / SetMyAvatar this port has no doc to back), then
// -- ONLY if a title has already been set (comicsTitle non-empty) -- call
// UpdateTitle so the newly-registered self's starring row appears
// immediately. Mirrors the original member-join site (protsupp.cpp:461,
// `if (doc->m_bComicView) UpdateTitle(doc);`), generalized to "self changed"
// as well as "a member joined" since this port's UpdateTitle rebuild is the
// same call either way (AddStars/AddStarsAux re-walk the whole session user
// table from scratch every time).
extern "C" int32_t cc_strip_set_self(cc_strip* s, int32_t participant) {
    if (!s || !s->page || participant <= 0) return -1;
    CCSessionSettings& sess = ccContext().session;
    if (!sess.lookupUser((UINT)participant)) return -1;   // unknown participant id
    sess.selfParticipant = (UINT)participant;
    if (sess.comicsTitle[0] != '\0') s->page->UpdateTitle();
    return 0;
}

// ============================================================================
// Plan 4b Task 2: emotion-wheel engine surface. All four resolve the SELF
// avatar the same way: ccContext().session.selfParticipant (R17, set by
// cc_strip_set_self above) -> GetAvatar(id). A strip with no self set yet
// (selfParticipant == 0, the CCSessionSettings default) or a self id that no
// longer resolves to a registered avatar rejects every one of these with -1,
// matching cc_strip_set_self's own error-code convention.

// cc_strip_set_self_emotion: the wheel drag -- the original CBodyCam::
// UpdateEmotion chain (bodycam.cpp:436) minus the HWND cursor-drawing/status-
// bar side effects (m_mouseDown/m_cursorPos/DrawCursor -- all BodyCam-view
// state this headless engine has no window to back): GetBodyFromEmotion(emo)
// then UpdateBody(that body) directly on the self avatar.
//
// DEVIATION (Step 1 finding, recorded in the task report): the brief's Step 3
// sketch wrote `CEmotion emo((float)angle, (float)intensity01)`, but
// CEmotion's real constructor is `CEmotion(double intensity, double emotion)`
// (avatar.h:62) -- intensity FIRST, angle/emotion SECOND, the OPPOSITE order
// the sketch used. This implementation uses the real (intensity, emotion)
// order; using the sketch's order would silently swap the two fields (an
// intensity of 0.0..2*PI radians and an "emotion" of 0.0..1.0) with no
// compiler error, since both parameters are `double`.
extern "C" int32_t cc_strip_set_self_emotion(cc_strip* s, double angle_radians, double intensity01) {
    if (!s) return -1;
    UINT self = ccContext().session.selfParticipant;   // R17
    if (self == 0) return -1;
    CAvatarX* av = GetAvatar((USHORT)self);
    if (!av) return -1;
    if (intensity01 < 0.0) intensity01 = 0.0;
    if (intensity01 > 1.0) intensity01 = 1.0;
    CEmotion emo(intensity01, angle_radians);   // avatar.h:62: (intensity, emotion) order
    av->UpdateBody(av->GetBodyFromEmotion(emo));  // bodycam.cpp:436's exact chain, minus drawing
    return 0;
}

// cc_strip_preview_self_text: the typing preview. Builds a CString shim from
// text_bytes and runs ChatPreSendText(str, self) (textpose.cpp:120, already
// forward-declared above for cc_strip_add_line's use) so the self avatar's
// pose reflects what the text WOULD infer -- without adding a line (no
// AddLine/page mutation at all, matching the original's per-edit preview,
// saywnd.cpp:975, which calls the same function on every keystroke, not just
// on send). ChatPreSendText itself already guards on !ccContext().session.
// comicView and !av/frozen (textpose.cpp:123-126); those are silent no-ops in
// the original too, not errors, so this wrapper does not duplicate them as
// failure returns -- only "no self set" is an error here.
extern "C" int32_t cc_strip_preview_self_text(cc_strip* s, const char* text_bytes) {
    if (!s || !text_bytes) return -1;
    UINT self = ccContext().session.selfParticipant;   // R17
    if (self == 0) return -1;
    if (!GetAvatar((USHORT)self)) return -1;
    CString text(text_bytes);
    ChatPreSendText(text, (int)self);
    return 0;
}

// cc_strip_self_pose: the self avatar's CURRENT pose id, for driving the
// wheel's live preview via cc_avatar_pose_image on a standalone cc_avatar
// handle of the same .avb (see comicchat.h's doc comment for the exact index-
// space caveat -- this is a poseID, 1-based into m_arrPoses, NOT the same
// numbering as cc_avatar_pose_image's compacted/icon-skipping `idx`).
//
// Step 1(d) finding: CBody::GetPoseID() is NOT a virtual on the CBody base
// class (avatar.h:83-98) -- it exists ONLY on CBodySingle/CBodyUnary
// (avatar.h:166,177). CBodyDouble (CAvatarComplex's body, face+torso) has no
// GetPoseID() at all. So `m_body->GetPoseID()` cannot be called generically
// through the CBody* the avatar stores -- it would fail to compile (no such
// virtual on the base) if written naively, and there is no safe generic
// dispatch for it. GetClass() (avatar.h:93, pure virtual, safe on the base
// pointer) reliably discriminates BC_BODYSINGLE from BC_BODYDOUBLE; this
// function downcasts on that discriminant, mirroring the SAME pattern
// bInsertAnnotations/GetIndices/GetEmotions already use for the two avatar
// shapes. For BC_BODYDOUBLE the TORSO pose id is reported (m_torsoRec->
// poseID) -- consistent with cc_strip_self_annotations below, which also
// treats the torso/gesture group as the "G" (gesture) half of the pair, and
// with GetIndices' own torso-first convention.
extern "C" int32_t cc_strip_self_pose(cc_strip* s, int32_t* out_pose_index) {
    if (out_pose_index) *out_pose_index = -1;
    if (!s) return -1;
    UINT self = ccContext().session.selfParticipant;   // R17
    if (self == 0) return -1;
    CAvatarX* av = GetAvatar((USHORT)self);
    if (!av || !av->m_body) return -1;

    short poseID;
    if (av->m_body->GetClass() == BC_BODYSINGLE) {
        poseID = ((CBodySingle*)av->m_body)->GetPoseID();
    } else {
        // BC_BODYDOUBLE: report the torso pose id (see doc comment above).
        poseID = ((CBodyDouble*)av->m_body)->m_torsoRec->poseID;
    }
    if (out_pose_index) *out_pose_index = (int32_t)poseID;
    return 0;
}

// cc_strip_self_annotations: fills `out` with the outbound annotation block
// for the CURRENT self pose/emotion state, reading the same fields
// bInsertAnnotations reads (protsupp.cpp:364-370) -- GetIndices for the face/
// torso RECORD indices (a DIFFERENT index space from cc_strip_self_pose's
// poseID above -- see comicchat.h's doc comment; this mirrors
// bInsertAnnotations's own field, which is exactly this record index, not a
// poseID) + GetEmotions for the (angle, intensity) pair per group, each
// packed through EmotionToBytes (avatario.cpp:74) exactly like
// bInsertAnnotations's own two calls. cooked=1 always (both intensities are
// always populated by this path, matching cc_annotations' own "cooked"
// contract -- both intensity fields written). mode/addressees are left at
// their zeroed defaults -- caller's job (Task 3), per the brief.
extern "C" int32_t cc_strip_self_annotations(cc_strip* s, cc_annotations* out) {
    if (!s || !out) return -1;
    UINT self = ccContext().session.selfParticipant;   // R17
    if (self == 0) return -1;
    CAvatarX* av = GetAvatar((USHORT)self);
    if (!av || !av->m_body) return -1;

    memset(out, 0, sizeof(*out));

    CHAR faceIndex, torsoIndex;
    BYTE bbRequested;
    av->GetIndices(faceIndex, torsoIndex, bbRequested);   // protsupp.cpp:364

    CEmotion face, torso;
    av->GetEmotions(face, torso);                          // protsupp.cpp:365

    // EmotionToBytes returns WIRE bytes (IndexToByte(v) = v + '0'), matching
    // bInsertAnnotations's own two calls (protsupp.cpp:369-370) exactly -- but
    // cc_annotations stores RAW indices, not wire bytes (comicchat.h's own
    // field comment: "Values are indices, NOT the +'0' wire bytes"), same as
    // gesture_pose/face_pose below. ccEncodeAnnotations (cc_session.cpp:
    // 436-441) re-wraps these fields through IndexToByte again before the
    // wire sprintf, and the decoder (protsupp.cpp:422-432) stores
    // ByteToIndex(wireByte) -- so this function must unwrap EmotionToBytes'
    // output with ByteToIndex before storing, or the send path double-wraps
    // (e.g. struct 57 -> wire 'i' = 105) and a peer decodes garbage.
    BYTE faceEmotion, faceIntensity, torsoEmotion, torsoIntensity;
    EmotionToBytes(face, faceEmotion, faceIntensity);       // protsupp.cpp:369
    EmotionToBytes(torso, torsoEmotion, torsoIntensity);    // protsupp.cpp:370

    out->gesture_pose = torsoIndex;
    out->gesture_emotion = ByteToIndex(torsoEmotion);
    out->gesture_intensity = ByteToIndex(torsoIntensity);
    out->face_pose = faceIndex;
    out->face_emotion = ByteToIndex(faceEmotion);
    out->face_intensity = ByteToIndex(faceIntensity);
    out->requested = bbRequested ? 1 : 0;
    out->mode = 0;              // caller's job (Task 3)
    out->addressee_count = 0;   // caller's job (Task 3)
    out->cooked = 1;            // both intensities always populated by this path
    return 0;
}
