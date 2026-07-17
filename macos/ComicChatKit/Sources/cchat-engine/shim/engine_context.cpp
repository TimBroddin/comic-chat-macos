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

// Plan 2 Task 3 (R17): see engine_context.h for the lazy-construct contract.
CDC* CCEngineContext::metricsDC() {
    ASSERT(metricsCanvas != nullptr);
    if (metricsDC_ == nullptr) {
        metricsDC_ = new CDC(metricsCanvas);
    }
    return metricsDC_;
}
