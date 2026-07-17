#include "engine_context.h"
#include "comicchat.h"

// Plan 2 Task 9 (R17 + R9): seeds CString::LoadString's registry (mfc_compat.h/
// .cpp) with the eleven ID_RULE_* rule-table strings, verbatim from
// chat.rc:2290-2304. RC "" is an escaped literal '"'; every value below
// reproduces that STRINGTABLE content exactly (quotes, \n separators, and the
// three genuinely-empty ANGRY/SCARED/BORED entries -- LoadCompositeRule's
// LoadSingleRule loop exits immediately on an empty rule string, registering
// no rules for those three emotions, exactly as the original shipped).
void ccSeedEmotionRuleStrings() {
    RegisterStringResource(ID_RULE_SHOUT,
        "AllCaps(\"\");9\nFindString(\"!!!\");9");
    RegisterStringResource(ID_RULE_LAUGH,
        "CheckWord*(\"ROTFL\");11\nCheckWord*(\"LOL\");11\nFindString*(\"HEHE\");11");
    RegisterStringResource(ID_RULE_HAPPY,
        "FindString(\":)\");10\nFindString(\":-)\");10");
    RegisterStringResource(ID_RULE_SAD,
        "FindString(\":(\");10\nFindString(\":-(\");10");
    RegisterStringResource(ID_RULE_POINTOTHER,
        "CheckStart*(\"You\");4\nCheckWord*(\"are you\");8\nCheckWord*(\"will you\");8"
        "\nCheckWord*(\"did you\");8\nCheckWord*(\"aren't you\");8\nCheckWord*(\"don't you\");8");
    RegisterStringResource(ID_RULE_POINTSELF,
        "CheckStart*(\"I\");3\nCheckWord*(\"i'm\");7\nCheckWord*(\"i will\");7"
        "\nCheckWord*(\"i'll\");7\nCheckWord*(\"i am\");7");
    RegisterStringResource(ID_RULE_WAVE,
        "CheckStart*(\"Hi\");2\nCheckStart*(\"Bye\");3\nCheckStart*(\"Hello\");5"
        "\nCheckStart*(\"Welcome\");5\nCheckStart*(\"Howdy\");5");
    RegisterStringResource(ID_RULE_COY,
        "FindString(\";-)\");10\nFindString(\";)\");10");
    RegisterStringResource(ID_RULE_ANGRY, "");
    RegisterStringResource(ID_RULE_SCARED, "");
    RegisterStringResource(ID_RULE_BORED, "");
}

CCEngineContext& ccContext() {
    static CCEngineContext ctx;
    static bool seeded = (ccSeedEmotionRuleStrings(), true);
    (void)seeded;
    return ctx;
}

extern "C" void cc_set_art_dirs(const char* avatarDir, const char* backdropDir) {
    ccContext().avatarDir = avatarDir ? avatarDir : "";
    ccContext().backdropDir = backdropDir ? backdropDir : "";
}

extern "C" void cc_set_metrics_canvas(cc_canvas* canvas) {
    ccContext().metricsCanvas = canvas;
    // Invalidate any CDC cached against the previous canvas (Task 6): the next
    // metricsDC() must bind to the newly-registered canvas, not the old one.
    ccContext().resetMetricsDC();
}

// Plan 2 Task 3 (R17): see engine_context.h for the lazy-construct contract.
CDC* CCEngineContext::metricsDC() {
    ASSERT(metricsCanvas != nullptr);
    if (metricsDC_ == nullptr) {
        metricsDC_ = new CDC(metricsCanvas);
    }
    return metricsDC_;
}

void CCEngineContext::resetMetricsDC() {
    delete metricsDC_;
    metricsDC_ = nullptr;
}
