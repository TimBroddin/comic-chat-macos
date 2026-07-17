// engine_context.h — replaces `theApp` for lifted code (rule R2).
#ifndef ENGINE_CONTEXT_H
#define ENGINE_CONTEXT_H

#include "mfc_compat.h"

struct CCEngineContext {
    CString avatarDir;    // replaces theApp.GetAvatarDir()
    CString backdropDir;  // replaces theApp.GetBackDropDir()
};

CCEngineContext& ccContext();

#endif
