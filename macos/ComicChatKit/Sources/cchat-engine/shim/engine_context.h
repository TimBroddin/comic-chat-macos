// engine_context.h — replaces `theApp` for lifted code (rule R2).
#ifndef ENGINE_CONTEXT_H
#define ENGINE_CONTEXT_H

#include "mfc_compat.h"
#include "defines.h"  // Plan 2 Task 4: chat.h (R2's replaced include) transitively
                       // provided defines.h's BM_* mode constants etc. to every
                       // file that included it; re-provide them here so R2-rerouted
                       // files see the same symbols chat.h gave them.
#include "userinfo.h"  // Plan 2 Task 8 (R17): the session user table holds
                        // CUserInfo objects (the camera's talk-to graph lives in
                        // CUserInfo::m_udi.m_talkTos). userinfo.h is self-contained
                        // (CString/CDWordArray/CUserDisplayInfo come from
                        // mfc_compat.h; CChatDoc is only forward-declared there).

struct cc_canvas;  // comicchat.h; forward-declared to avoid a hard dependency here

// Plan 2 Task 6 (R17): the layout/font code (fonts.cpp, balloon.cpp) reached
// theApp's comics-font and formatting settings for its defaults. Those move
// here as a plain settings struct; the headless engine supplies them (the
// original filled them from the registry/resource chain -- ID_COMIC_FONT_NAME
// "Comic Sans MS", 12pt, m_comicsColor default RGB(0,0,0), m_charSet from the
// active codepage). Every field is exercised by cc_selftest_balloon().
//   theApp field           -> session field       original type (chat.h)
//   m_comicsColor          -> comicsColor          COLORREF   (fonts.cpp:135/137)
//   m_charSet              -> charSet              BYTE       (balloon.cpp:111)
//   m_comicsFont           -> comicsFont           LOGFONT    (fonts.cpp:64/110)
//   m_iFontHeightBalloon   -> iFontHeightBalloon   INT        (fonts.cpp:105/128)
//   m_szGuiFaceName        -> guiFaceName          CHAR[LF_FACESIZE] (fonts.cpp:58)
//   m_flags1               -> flags1               DWORD      (balloon.h:77, F1_RTFCOMIC)
// comicsFontFace/comicsFontPts are the brief's Step-1 seed fields for the
// default comics LOGFONT (face + point size); comicsFont is that LOGFONT
// once SetFonts() has been called (it writes it back, fonts.cpp:64), seeded
// here to the same face/size so pre-SetFonts reads are still self-consistent.
// Plan 2 Task 8 (R17): one entry of the headless session's user table. The
// original kept CUserInfo objects in the doc's member list / g_mapNickToPtr,
// keyed for lookup by nickname; the headless engine keys them by the same
// avatarID the lifted call sites already use (GetAvatar(id)->m_userInfo). The
// camera (panel.cpp EvalPair/AddTalkTos) reads pui->m_udi.m_talkTos (a
// CDWordArray of (DWORD)CUserInfo* pointers) off these -- so the table is where
// the addressee/talk-to graph lives on this port. Stored inline (not by
// pointer) so the CUserInfo's address is stable for the lifetime of the
// session (talkTos entries are raw CUserInfo* addresses into this array).
struct CCSessionUser {
    UINT     id = 0;
    CUserInfo info;
};

#define CC_SESSION_MAX_USERS 32  // MAXBDYPERFRAME(20) headroom; a comic panel
                                 // caps at 5 bodies, so this is generous.

struct CCSessionSettings {
    COLORREF comicsColor = RGB(0, 0, 0);
    char     comicsFontFace[LF_FACESIZE] = "Comic Sans MS";
    int      comicsFontPts = 12;
    int      charSet = 0;
    LOGFONT  comicsFont = {};
    int      iFontHeightBalloon = 240;  // 12pt * 20 twips/pt (MM_TWIPS)
    char     guiFaceName[LF_FACESIZE] = "Comic Sans MS";
    DWORD    flags1 = 0;

    // Plan 2 Task 8 (R17): CPanel::CPanel() read GetChatDoc()->GetBackDropID()
    // (panel.cpp:558) and CUnitPanelPage::UpdateTitle read
    // GetChatDoc()->GetComicsTitle() (panel.cpp:1302) off the doc. Those move
    // here (the backdrop id every new panel inherits, and the comics title the
    // title page shows). backdropID default 0 == "no backdrop" (the original's
    // zero-init doc default); comicsTitle default empty.
    UINT     backdropID = 0;
    char     comicsTitle[128] = "";

    // Plan 2 Task 8 (R17): the headless session user table (see CCSessionUser).
    CCSessionUser users[CC_SESSION_MAX_USERS];
    int          userCount = 0;

    // Add a user with the given avatarID, returning a pointer to its (mutable)
    // CUserInfo so the caller can populate name/talkTos/etc. Returns an existing
    // entry if id is already present (idempotent add). Returns nullptr if the
    // table is full. Mirrors the original's "find-or-create user by identity"
    // shape (the doc member list did the same, keyed by nick).
    CUserInfo* addUser(UINT id) {
        for (int i = 0; i < userCount; i++)
            if (users[i].id == id) return &users[i].info;
        if (userCount >= CC_SESSION_MAX_USERS) return nullptr;
        users[userCount].id = id;
        return &users[userCount++].info;
    }
    // Look up a user's CUserInfo by avatarID; nullptr if not present. This is the
    // headless replacement for the doc/g_mapNickToPtr lookup the original used
    // to resolve a speaker to its CUserInfo.
    CUserInfo* lookupUser(UINT id) {
        for (int i = 0; i < userCount; i++)
            if (users[i].id == id) return &users[i].info;
        return nullptr;
    }
    // Drop every user (test/session reset). Does not touch other settings.
    void clearUsers() { userCount = 0; }

    // Plan 2 Task 8 (R13 + R17): recover the FULL CUserInfo* that a talk-to-graph
    // DWORD key was truncated from. m_udi.m_talkTos stores CUserInfo* as
    // (DWORD)(uintptr_t) low-32 keys (the original assumed 32-bit pointers; on
    // LP64 that truncation is lossy). Where the original ROUND-TRIPPED a pointer
    // out of the array (panel.cpp AddTalkTos:357, to pull an absent addressee's
    // body into the panel), a raw `(CUserInfo*)key` widening would rebuild a
    // garbage-high-bits pointer whose dereference is UB. This instead recovers
    // the real pointer by scanning the user table (which OWNS every CUserInfo a
    // talkTos key can name) for the entry whose &info truncates to `key`.
    // Returns nullptr if no match (the wiring invariant broke -- the call site
    // routes that to the original's ASSERT(FALSE) error branch). A 32-entry
    // linear scan is fine: this fires only on the rare multi-party pull-in path.
    // Selftested (round-trip + unknown-key) in cc_selftest_panel.
    CUserInfo* userFromTalkTo(DWORD key) {
        for (int i = 0; i < userCount; i++) {
            if ((DWORD)(uintptr_t)&users[i].info == key)
                return &users[i].info;
        }
        return nullptr;
    }
};

struct CCEngineContext {
    CString avatarDir;    // replaces theApp.GetAvatarDir()
    CString backdropDir;  // replaces theApp.GetBackDropDir()
    CCSessionSettings session;  // Plan 2 Task 6 (R17): comics font/formatting settings
    cc_canvas* metricsCanvas = nullptr;  // Plan 2 Task 2: cc_set_metrics_canvas()

    // Plan 2 Task 3 (R17): replaces the original's single shared MM_TWIPS
    // desktop CClientDC (pageview.cpp's GetClientDC()) used for layout-time
    // text measurement. Lazily constructs a CDC bound to metricsCanvas on
    // first call and reuses it thereafter (mirrors the original's "created
    // once in OnCreate, fetched everywhere via GetClientDC()" lifetime).
    // ASSERTs metricsCanvas is registered -- calling this before
    // cc_set_metrics_canvas() is a programming error, not a runtime
    // condition to handle gracefully.
    CDC* metricsDC();

    // Plan 2 Task 6: invalidate the cached metricsDC so the next metricsDC()
    // call rebuilds it against the CURRENT metricsCanvas. cc_set_metrics_canvas
    // calls this -- without it, the cached CDC would keep pointing at a
    // previously-registered (possibly destroyed) canvas after a re-register.
    void resetMetricsDC();

private:
    CDC* metricsDC_ = nullptr;
};

CCEngineContext& ccContext();

#endif
