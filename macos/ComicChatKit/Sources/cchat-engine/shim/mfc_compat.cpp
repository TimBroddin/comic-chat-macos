#include "mfc_compat.h"
#include "comicchat.h"
#include "engine_context.h"
#include <sys/stat.h>

// CClientDC (Plan 2 Task 3): a CDC auto-bound to the registered metrics
// canvas. Defined here (not inline in the header) because it needs
// engine_context.h's full CCEngineContext definition, and mfc_compat.h
// cannot #include engine_context.h without a circular include (engine_
// context.h itself #includes mfc_compat.h for CString) -- see the
// ccContext() forward declaration in mfc_compat.h for the full rationale.
CClientDC::CClientDC() : CDC(ccContext().metricsCanvas) {
    ASSERT(ccContext().metricsCanvas != nullptr);
}

// Plan 2 Task 1: ccLog level gate. -1 = not yet initialized; lazily seeded
// from CC_LOG_LEVEL on first use (default 2 if unset/unparsed). 0=silent,
// 1=errors (ASSERT/VERIFY), 2=trace.
static int g_ccLogLevel = -1;

static void ccLogLevelEnsureInit() {
    if (g_ccLogLevel != -1) return;
    const char* env = getenv("CC_LOG_LEVEL");
    if (env != nullptr && env[0] != '\0') {
        char* endptr;
        long val = strtol(env, &endptr, 10);
        // Only accept if entire string parsed (endptr points to NUL)
        if (*endptr == '\0') {
            g_ccLogLevel = (val < 0) ? 0 : (val > 2) ? 2 : (int)val;
        } else {
            g_ccLogLevel = 2;  // invalid parse -> default
        }
    } else {
        g_ccLogLevel = 2;  // unset or empty -> default
    }
}

int ccLogWouldEmit(int level) {
    ccLogLevelEnsureInit();
    return g_ccLogLevel >= level;
}

void cc_set_log_level(int32_t level) {
    // Clamp to valid range [0, 2]
    g_ccLogLevel = (level < 0) ? 0 : (level > 2) ? 2 : level;
}

void ccLog(const char* fmt, ...) {
    ccLogLevelEnsureInit();
    if (g_ccLogLevel < 2) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void ccLogError(const char* fmt, ...) {
    ccLogLevelEnsureInit();
    if (g_ccLogLevel < 1) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

// R9: minimal GetFileAttributes() — used by avatario.cpp only to test
// existence of a file before opening it.
DWORD GetFileAttributes(LPCSTR pszPath) {
    struct stat st;
    if (pszPath == nullptr || stat(pszPath, &st) != 0) {
        return INVALID_FILE_ATTRIBUTES;
    }
    return 0;
}
