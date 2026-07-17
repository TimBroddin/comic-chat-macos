// cc_recording_canvas.cpp — see cc_recording_canvas.h for the log-format
// contract these functions must produce exactly.

#include "cc_recording_canvas.h"
#include "mfc_compat.h"  // GetRValue/GetGValue/GetBValue
#include <cstdio>

namespace {

// COLORREF (0x00BBGGRR) -> "RRGGBB" display-order hex, 6 uppercase digits.
// Keep every color-formatting call site in this file going through here so
// the conversion stays consistent (see header comment for the rationale).
std::string formatColor(uint32_t color) {
    char buf[7];
    std::snprintf(buf, sizeof(buf), "%02X%02X%02X",
                  GetRValue(color), GetGValue(color), GetBValue(color));
    return std::string(buf);
}

std::string formatXY(int32_t x, int32_t y) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%d,%d", x, y);
    return std::string(buf);
}

} // namespace

const cc_canvas_ops CCRecordingCanvas::kOps = {
    &CCRecordingCanvas::measure_text,
    &CCRecordingCanvas::font_metrics,
    &CCRecordingCanvas::draw_text,
    &CCRecordingCanvas::fill_rect,
    &CCRecordingCanvas::draw_image,
    &CCRecordingCanvas::path,
    &CCRecordingCanvas::clip_push,
    &CCRecordingCanvas::clip_pop,
    &CCRecordingCanvas::is_printing,
};

CCRecordingCanvas::CCRecordingCanvas() {
    canvas_.ops = &kOps;
    canvas_.ctx = this;
}

void CCRecordingCanvas::measure_text(void* ctx, const cc_font_spec* /*f*/,
                                      const char* /*bytes*/, int32_t len,
                                      int32_t* out_w, int32_t* out_h) {
    (void)ctx;
    if (out_w) *out_w = len * 120;
    if (out_h) *out_h = 240;
}

void CCRecordingCanvas::font_metrics(void* ctx, const cc_font_spec* /*f*/,
                                      cc_text_metrics* out) {
    (void)ctx;
    if (!out) return;
    out->height = 240;
    out->ascent = 190;
    out->descent = 50;
    out->internal_leading = 40;
    out->external_leading = 20;
    out->ave_char_width = 120;
    out->max_char_width = 240;
}

void CCRecordingCanvas::draw_text(void* ctx, const cc_font_spec* /*f*/,
                                   int32_t x, int32_t y, uint32_t color,
                                   int32_t /*bk_opaque*/, uint32_t /*bk_color*/,
                                   const char* bytes, int32_t len) {
    CCRecordingCanvas* self = CCRecordingCanvas::self(ctx);
    std::string line = "text " + formatXY(x, y) + " color=" + formatColor(color) +
                        " \"" + std::string(bytes, bytes + len) + "\"";
    self->log_.push_back(line);
}

void CCRecordingCanvas::fill_rect(void* ctx, int32_t l, int32_t t, int32_t r,
                                   int32_t b, uint32_t color) {
    CCRecordingCanvas* self = CCRecordingCanvas::self(ctx);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "rect %d,%d,%d,%d fill=%s", l, t, r, b,
                  formatColor(color).c_str());
    self->log_.push_back(buf);
}

void CCRecordingCanvas::draw_image(void* ctx, const cc_image* /*img*/,
                                    int32_t dl, int32_t dt, int32_t dr, int32_t db,
                                    int32_t sl, int32_t st, int32_t sr, int32_t sb) {
    CCRecordingCanvas* self = CCRecordingCanvas::self(ctx);
    char buf[96];
    std::snprintf(buf, sizeof(buf), "image %d,%d,%d,%d src=%d,%d,%d,%d",
                  dl, dt, dr, db, sl, st, sr, sb);
    self->log_.push_back(buf);
}

void CCRecordingCanvas::path(void* ctx, const cc_path_pt* pts, int32_t n,
                              int32_t do_fill, uint32_t fill_color,
                              int32_t do_stroke, uint32_t stroke_color,
                              int32_t stroke_width, int32_t dashed) {
    CCRecordingCanvas* self = CCRecordingCanvas::self(ctx);
    (void)fill_color;
    (void)stroke_color;

    std::string entries;
    int32_t i = 0;
    while (i < n) {
        if (!entries.empty()) entries += " ";
        switch (pts[i].verb) {
            case CC_PATH_MOVE:
                entries += "M " + formatXY(pts[i].x, pts[i].y);
                i++;
                break;
            case CC_PATH_LINE:
                entries += "L " + formatXY(pts[i].x, pts[i].y);
                i++;
                break;
            case CC_PATH_CUBIC:
                // Three consecutive CC_PATH_CUBIC entries render as one C triple.
                entries += "C " + formatXY(pts[i].x, pts[i].y) + " " +
                           formatXY(pts[i + 1].x, pts[i + 1].y) + " " +
                           formatXY(pts[i + 2].x, pts[i + 2].y);
                i += 3;
                break;
            case CC_PATH_CLOSE:
                entries += "Z";
                i++;
                break;
            default:
                i++;
                break;
        }
    }

    char head[80];
    std::snprintf(head, sizeof(head), "path n=%d fill=%d stroke=%d w=%d dashed=%d ",
                  n, do_fill, do_stroke, stroke_width, dashed);
    self->log_.push_back(std::string(head) + "[" + entries + "]");
}

void CCRecordingCanvas::clip_push(void* ctx, int32_t l, int32_t t, int32_t r, int32_t b) {
    CCRecordingCanvas* self = CCRecordingCanvas::self(ctx);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "clip+ %d,%d,%d,%d", l, t, r, b);
    self->log_.push_back(buf);
}

void CCRecordingCanvas::clip_pop(void* ctx) {
    CCRecordingCanvas* self = CCRecordingCanvas::self(ctx);
    self->log_.push_back("clip-");
}

int32_t CCRecordingCanvas::is_printing(void* /*ctx*/) {
    return 0;
}
