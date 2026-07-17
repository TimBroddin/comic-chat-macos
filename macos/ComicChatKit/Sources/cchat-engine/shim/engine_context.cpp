#include "engine_context.h"
#include "comicchat.h"

CCEngineContext& ccContext() {
    static CCEngineContext ctx;
    return ctx;
}

extern "C" void cc_set_art_dirs(const char* avatarDir, const char* backdropDir) {
    ccContext().avatarDir = avatarDir ? avatarDir : "";
    ccContext().backdropDir = backdropDir ? backdropDir : "";
}

extern "C" void cc_set_metrics_canvas(cc_canvas* canvas) {
    ccContext().metricsCanvas = canvas;
}
