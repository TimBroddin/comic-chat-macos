// engine_context.h — replaces `theApp` for lifted code (rule R2).
#ifndef ENGINE_CONTEXT_H
#define ENGINE_CONTEXT_H

#include "mfc_compat.h"
#include "defines.h"  // Plan 2 Task 4: chat.h (R2's replaced include) transitively
                       // provided defines.h's BM_* mode constants etc. to every
                       // file that included it; re-provide them here so R2-rerouted
                       // files see the same symbols chat.h gave them.

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
struct CCSessionSettings {
    COLORREF comicsColor = RGB(0, 0, 0);
    char     comicsFontFace[LF_FACESIZE] = "Comic Sans MS";
    int      comicsFontPts = 12;
    int      charSet = 0;
    LOGFONT  comicsFont = {};
    int      iFontHeightBalloon = 240;  // 12pt * 20 twips/pt (MM_TWIPS)
    char     guiFaceName[LF_FACESIZE] = "Comic Sans MS";
    DWORD    flags1 = 0;
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
