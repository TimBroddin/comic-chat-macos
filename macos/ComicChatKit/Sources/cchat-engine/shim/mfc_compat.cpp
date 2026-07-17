#include "mfc_compat.h"
#include "comicchat.h"
#include "engine_context.h"
#include "dib.h"        // full CDIB definition for CDC::DrawPoseImage (Task 7)
#include "bridge_art.h" // bridge_decode_dib_pair_to_rgba (Task 7)
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

// R14(i) (Plan 2 Task 7): CDC::DrawPoseImage -- the CBody draw path's
// MERGEPAINT-mask + SRCAND-drawing ROP pair collapsed into ONE alpha-composited
// draw_image. Defined here (not inline in the header) because it needs the full
// CDIB definition (GetBitmapInfoAddress/GetBitsAddress) and bridge_art.h, which
// mfc_compat.h keeps pointer-opaque. Reuses bridge_decode_dib_pair_to_rgba (the
// same decodeDibToRgba the pose-image golden path regression-locks) so mask
// polarity has one source of truth.
void CDC::DrawPoseImage(CDIB* image, CDIB* mask,
                        int destX, int destY, int destW, int destH) {
    if (image == nullptr) return;
    BITMAPINFO* imgBmi = image->GetBitmapInfoAddress();
    void* imgBits = image->GetBitsAddress();
    BITMAPINFO* maskBmi = mask != nullptr ? mask->GetBitmapInfoAddress() : nullptr;
    void* maskBits = mask != nullptr ? mask->GetBitsAddress() : nullptr;

    int32_t w = 0, h = 0;
    uint8_t* rgba = nullptr;
    if (!bridge_decode_dib_pair_to_rgba(imgBmi, imgBits, maskBmi, maskBits,
                                        &w, &h, &rgba)) {
        return;
    }
    cc_image img; img.width = w; img.height = h; img.rgba = rgba;
    int32_t dl, dt, dr, db;
    toLogical(destX, destY, dl, dt);
    toLogical(destX + destW, destY + destH, dr, db);
    // Source rect is the full decoded image (the CBody draw path always blits
    // the whole plane, scaled into the dest rect -- see bodycam.cpp DrawBody).
    canvasWrap().draw_image(&img, dl, dt, dr, db, 0, 0, w, h);
    cc_image_free(&img);
}

// R14(v) (Plan 2 Task 7 review fix): CDC::DrawAuraImage -- the aura
// (whisper-nimbus) plane's MERGEPAINT-ALONE draw (bodycam.cpp DrawBody's
// torso/head/single nimbus blits), collapsed into ONE alpha-composited
// draw_image using the aura's own white-forcing polarity (NOT the image/mask
// polarity DrawPoseImage uses -- see bridge_decode_aura_to_white_alpha's
// derivation in bridge_art.cpp for why they differ). Defined here (not inline
// in the header) for the same reason as DrawPoseImage: needs the full CDIB
// definition and bridge_art.h, which mfc_compat.h keeps pointer-opaque.
void CDC::DrawAuraImage(CDIB* aura, int destX, int destY, int destW, int destH) {
    if (aura == nullptr) return;
    BITMAPINFO* auraBmi = aura->GetBitmapInfoAddress();
    void* auraBits = aura->GetBitsAddress();

    int32_t w = 0, h = 0;
    uint8_t* rgba = nullptr;
    if (!bridge_decode_aura_to_white_alpha(auraBmi, auraBits, &w, &h, &rgba)) {
        return;
    }
    cc_image img; img.width = w; img.height = h; img.rgba = rgba;
    int32_t dl, dt, dr, db;
    toLogical(destX, destY, dl, dt);
    toLogical(destX + destW, destY + destH, dr, db);
    // Source rect is the full decoded image, same convention as DrawPoseImage.
    canvasWrap().draw_image(&img, dl, dt, dr, db, 0, 0, w, h);
    cc_image_free(&img);
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
