// cc_recording_canvas.h — C++ recording implementation of cc_canvas_ops
// (Plan 2 Task 2). Every op appends one line of human-readable text to an
// internal log instead of drawing anything; used by selftests (and later
// tasks' tests) to assert exactly what the layout engine asked the canvas
// to do, without a real drawing surface.
//
// Deterministic fake metrics (no real font engine is consulted):
//   - measure_text: every byte is 120 twips wide, regardless of font or byte
//     value; height is always 240 twips. So measure_text("hello", 5) reports
//     w=600, h=240.
//   - font_metrics: always {height=240, ascent=190, descent=50,
//     internal_leading=40, external_leading=20, ave_char_width=120,
//     max_char_width=240}, regardless of the requested cc_font_spec.
//
// Log line format (STABLE CONTRACT — every later task's selftests assert
// against these exact shapes; do not change without updating every caller):
//   text  "text x,y color=RRGGBB \"bytes\""
//   rect  "rect l,t,r,b fill=RRGGBB"
//   image "image dl,dt,dr,db src=sl,st,sr,sb"
//   path  "path n=N fill=F fillc=RRGGBB stroke=S strokec=RRGGBB w=WIDTH dashed=D [entries]"
//         where entries are space-separated, one of:
//           "M x,y"                     (CC_PATH_MOVE)
//           "L x,y"                     (CC_PATH_LINE)
//           "C x1,y1 x2,y2 x3,y3"       (three consecutive CC_PATH_CUBIC
//                                        entries collapsed into one C triple)
//           "Z"                         (CC_PATH_CLOSE)
//   clip_push  "clip+ l,t,r,b"
//   clip_pop   "clip-"
//
// Formatting rules:
//   - Coordinates are signed decimal integers, comma-joined, no spaces
//     ("x,y" / "l,t,r,b"), exactly as written above.
//   - Colors: COLORREF is 0x00BBGGRR (GDI convention: R in bits 0-7, G in
//     bits 8-15, B in bits 16-23 — see mfc_compat.h's RGB/GetRValue/
//     GetGValue/GetBValue). The log renders colors as 6 uppercase hex
//     digits in RRGGBB *display* order — i.e. GetRValue first, then
//     GetGValue, then GetBValue — NOT the raw little-endian byte order of
//     the COLORREF integer. Example: COLORREF 0x00FFFFFF (white; R=G=B=255)
//     logs as "FFFFFF"; COLORREF 0x00000000 (black) logs as "000000".
//     Every color-formatting call site must use this same conversion.
//   - Text bytes are printed verbatim between double quotes; no escaping is
//     performed (selftest inputs never contain a `"` or control byte, so
//     this is safe for now — revisit if that assumption ever changes).
#ifndef CC_RECORDING_CANVAS_H
#define CC_RECORDING_CANVAS_H

#include "comicchat.h"
#include <string>
#include <vector>

class CCRecordingCanvas {
public:
    CCRecordingCanvas();

    cc_canvas* handle() { return &canvas_; }
    const std::vector<std::string>& log() const { return log_; }

private:
    static const cc_canvas_ops kOps;
    cc_canvas canvas_;
    std::vector<std::string> log_;

    static CCRecordingCanvas* self(void* ctx) {
        return static_cast<CCRecordingCanvas*>(ctx);
    }

    static void measure_text(void* ctx, const cc_font_spec* f, const char* bytes,
                              int32_t len, int32_t* out_w, int32_t* out_h);
    static void font_metrics(void* ctx, const cc_font_spec* f, cc_text_metrics* out);
    static void draw_text(void* ctx, const cc_font_spec* f, int32_t x, int32_t y,
                           uint32_t color, int32_t bk_opaque, uint32_t bk_color,
                           const char* bytes, int32_t len);
    static void fill_rect(void* ctx, int32_t l, int32_t t, int32_t r, int32_t b,
                           uint32_t color);
    static void draw_image(void* ctx, const cc_image* img,
                            int32_t dl, int32_t dt, int32_t dr, int32_t db,
                            int32_t sl, int32_t st, int32_t sr, int32_t sb);
    static void path(void* ctx, const cc_path_pt* pts, int32_t n,
                      int32_t do_fill, uint32_t fill_color,
                      int32_t do_stroke, uint32_t stroke_color,
                      int32_t stroke_width, int32_t dashed);
    static void clip_push(void* ctx, int32_t l, int32_t t, int32_t r, int32_t b);
    static void clip_pop(void* ctx);
    static int32_t is_printing(void* ctx);
};

#endif /* CC_RECORDING_CANVAS_H */
