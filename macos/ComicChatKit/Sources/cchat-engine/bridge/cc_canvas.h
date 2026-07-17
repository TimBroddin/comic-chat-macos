// cc_canvas.h — header-only C++ convenience wrapper around cc_canvas
// (Plan 2 Task 2). Mirrors every cc_canvas_ops entry as an inline forwarding
// method so call sites read like ordinary member calls instead of manual
// vtable dispatch. Purely a convenience layer: it owns nothing and asserts
// the wrapped canvas is well-formed on every call.
#ifndef CC_CANVAS_H
#define CC_CANVAS_H

#include "comicchat.h"
#include "mfc_compat.h"  // ASSERT

class CCanvas {
    cc_canvas* c_;

public:
    explicit CCanvas(cc_canvas* c) : c_(c) {
        ASSERT(c_ && c_->ops);
    }

    void measure_text(const cc_font_spec* f, const char* bytes, int32_t len,
                       int32_t* out_w, int32_t* out_h) const {
        ASSERT(c_ && c_->ops);
        c_->ops->measure_text(c_->ctx, f, bytes, len, out_w, out_h);
    }

    void font_metrics(const cc_font_spec* f, cc_text_metrics* out) const {
        ASSERT(c_ && c_->ops);
        c_->ops->font_metrics(c_->ctx, f, out);
    }

    void draw_text(const cc_font_spec* f, int32_t x, int32_t y, uint32_t color,
                    int32_t bk_opaque, uint32_t bk_color, const char* bytes,
                    int32_t len) const {
        ASSERT(c_ && c_->ops);
        c_->ops->draw_text(c_->ctx, f, x, y, color, bk_opaque, bk_color, bytes, len);
    }

    void fill_rect(int32_t l, int32_t t, int32_t r, int32_t b, uint32_t color) const {
        ASSERT(c_ && c_->ops);
        c_->ops->fill_rect(c_->ctx, l, t, r, b, color);
    }

    void draw_image(const cc_image* img, int32_t dl, int32_t dt, int32_t dr,
                     int32_t db, int32_t sl, int32_t st, int32_t sr,
                     int32_t sb) const {
        ASSERT(c_ && c_->ops);
        c_->ops->draw_image(c_->ctx, img, dl, dt, dr, db, sl, st, sr, sb);
    }

    void path(const cc_path_pt* pts, int32_t n, int32_t do_fill,
               uint32_t fill_color, int32_t do_stroke, uint32_t stroke_color,
               int32_t stroke_width, int32_t dashed) const {
        ASSERT(c_ && c_->ops);
        c_->ops->path(c_->ctx, pts, n, do_fill, fill_color, do_stroke,
                      stroke_color, stroke_width, dashed);
    }

    void clip_push(int32_t l, int32_t t, int32_t r, int32_t b) const {
        ASSERT(c_ && c_->ops);
        c_->ops->clip_push(c_->ctx, l, t, r, b);
    }

    void clip_pop() const {
        ASSERT(c_ && c_->ops);
        c_->ops->clip_pop(c_->ctx);
    }

    int32_t is_printing() const {
        ASSERT(c_ && c_->ops);
        return c_->ops->is_printing(c_->ctx);
    }
};

#endif /* CC_CANVAS_H */
