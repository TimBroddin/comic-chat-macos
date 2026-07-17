// engine_context.h — replaces `theApp` for lifted code (rule R2).
#ifndef ENGINE_CONTEXT_H
#define ENGINE_CONTEXT_H

#include "mfc_compat.h"

struct cc_canvas;  // comicchat.h; forward-declared to avoid a hard dependency here

struct CCEngineContext {
    CString avatarDir;    // replaces theApp.GetAvatarDir()
    CString backdropDir;  // replaces theApp.GetBackDropDir()
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

private:
    CDC* metricsDC_ = nullptr;
};

CCEngineContext& ccContext();

#endif
