// engine_context.h — replaces `theApp` for lifted code (rule R2).
#ifndef ENGINE_CONTEXT_H
#define ENGINE_CONTEXT_H

#include "mfc_compat.h"

struct cc_canvas;  // comicchat.h; forward-declared to avoid a hard dependency here

struct CCEngineContext {
    CString avatarDir;    // replaces theApp.GetAvatarDir()
    CString backdropDir;  // replaces theApp.GetBackDropDir()
    cc_canvas* metricsCanvas = nullptr;  // Plan 2 Task 2: cc_set_metrics_canvas()
};

CCEngineContext& ccContext();

#endif
