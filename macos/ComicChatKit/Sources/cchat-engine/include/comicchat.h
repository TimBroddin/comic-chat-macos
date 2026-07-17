#ifndef COMICCHAT_H
#define COMICCHAT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int32_t cc_engine_version(void);
void    cc_set_art_dirs(const char* avatar_dir, const char* backdrop_dir);
int32_t cc_run_selftests(void);

/* Plan 2 Task 7: CBody draw-path selftest. Opens the avatar at avatar_path
 * (a real .avb fixture; the Swift wrapper passes it), builds a body, draws it
 * through a recording canvas, and asserts the image-blit count + dest rects
 * match GetBodyBox. Returns 0 on success (failure count otherwise). Kept out
 * of cc_run_selftests because the C++ selftests take no path argument. */
int32_t cc_run_bodydraw_selftest(const char* avatar_path);

/* Plan 2 Task 8: panel orchestrator + body-placement camera selftest. Opens the
 * avatar at avatar_path (a real .avb fixture) TWICE (two avatars A/B), wires two
 * session users + the talk-to graph, drives LayoutAvatars/AddLine, and
 * characterizes camera facing/order, panel-break rules, and orchestration
 * (panel count + balloon bboxes inside panel unit rects). Returns 0 on success
 * (failure count otherwise). Kept out of cc_run_selftests because it needs the
 * fixture path. */
int32_t cc_run_panel_selftest(const char* avatar_path);

/* Plan 2 Task 1: engine log level. 0=silent, 1=errors (ASSERT/VERIFY
 * failures), 2=trace. Default 2; also readable once via env var
 * CC_LOG_LEVEL (read lazily on first log call). Also resets the lazy env
 * read, so calling this always takes effect immediately. */
void cc_set_log_level(int32_t level);

/* ============================================================================
 * Permanent art API (Task 6). RGBA is always 8-bit/channel, row-major,
 * top-to-bottom, straight (non-premultiplied) alpha. `rgba` is malloc'd by
 * the bridge and must be released with cc_image_free — exactly once, and
 * only via this function (never free() it directly). A zeroed cc_image
 * (width=0, height=0, rgba=NULL) is always safe to pass to cc_image_free. */

typedef struct cc_image {
    int32_t width;
    int32_t height;
    uint8_t* rgba; /* width*height*4 bytes, RGBA8, or NULL if not populated */
} cc_image;

void cc_image_free(cc_image* img);

/* An opened avatar file (.avb). Owns the underlying parsed CAvatarX object
 * for the lifetime between cc_avatar_open and cc_avatar_close. */
typedef struct cc_avatar cc_avatar;

cc_avatar*  cc_avatar_open(const char* path);   /* NULL on failure */
void        cc_avatar_close(cc_avatar* av);     /* no-op if av == NULL */
const char* cc_avatar_name(const cc_avatar* av); /* never NULL; "" if unknown */
int32_t     cc_avatar_pose_count(const cc_avatar* av);
const char* cc_avatar_pose_name(const cc_avatar* av, int32_t idx); /* never NULL */
int32_t     cc_avatar_pose_image(const cc_avatar* av, int32_t idx, cc_image* out); /* 0 = ok */

/* An opened backdrop file (.bgb or .bmp). Owns the underlying parsed
 * CChatBackdrop object for the lifetime between cc_backdrop_open and
 * cc_backdrop_close. */
typedef struct cc_backdrop cc_backdrop;

cc_backdrop* cc_backdrop_open(const char* path); /* NULL on failure */
void         cc_backdrop_close(cc_backdrop* bd); /* no-op if bd == NULL */
const char*  cc_backdrop_name(const cc_backdrop* bd); /* never NULL; "" if unknown */
int32_t      cc_backdrop_image(const cc_backdrop* bd, cc_image* out); /* 0 = ok */

/* CRC32 (zlib polynomial/algorithm) of an arbitrary byte buffer. Used by
 * cc-dumpart / the golden catalog test to fingerprint decoded RGBA buffers
 * without embedding raw pixel data in the golden JSON. */
uint32_t cc_crc32(const uint8_t* data, size_t len);

/* ============================================================================
 * Canvas boundary (Plan 2, spec §4.3). All coordinates are MM_TWIPS logical
 * units (1/1440 inch, y-up) exactly as the original GDI code used them; the
 * implementation maps to device space. Text params are raw bytes (CP-1252 by
 * default). color values are GDI COLORREF (0x00BBGGRR). */

typedef struct cc_font_spec {
    char    face[64];     /* e.g. "Comic Sans MS" */
    int32_t height;       /* LOGFONT lfHeight in twips; negative = char height */
    int32_t weight;       /* 400 normal, 700 bold */
    uint8_t italic, underline, strikeout, charset;
} cc_font_spec;

typedef struct cc_text_metrics { /* the TEXTMETRIC fields the engine reads; twips */
    int32_t height, ascent, descent, internal_leading, external_leading;
    int32_t ave_char_width, max_char_width;
} cc_text_metrics;

enum { CC_PATH_MOVE = 0, CC_PATH_LINE = 1, CC_PATH_CUBIC = 2, CC_PATH_CLOSE = 3 };
/* CC_PATH_CUBIC appears as THREE consecutive entries (control1, control2,
 * endpoint), all with verb CC_PATH_CUBIC. */
typedef struct cc_path_pt { int32_t verb; int32_t x, y; } cc_path_pt;

typedef struct cc_canvas_ops {
    /* measurement — must work with no drawing surface active */
    void (*measure_text)(void* ctx, const cc_font_spec* f, const char* bytes,
                         int32_t len, int32_t* out_w, int32_t* out_h);
    void (*font_metrics)(void* ctx, const cc_font_spec* f, cc_text_metrics* out);
    /* drawing */
    void (*draw_text)(void* ctx, const cc_font_spec* f, int32_t x, int32_t y,
                      uint32_t color, int32_t bk_opaque, uint32_t bk_color,
                      const char* bytes, int32_t len);
    void (*fill_rect)(void* ctx, int32_t l, int32_t t, int32_t r, int32_t b,
                      uint32_t color);
    void (*draw_image)(void* ctx, const cc_image* img,
                       int32_t dl, int32_t dt, int32_t dr, int32_t db,
                       int32_t sl, int32_t st, int32_t sr, int32_t sb);
    void (*path)(void* ctx, const cc_path_pt* pts, int32_t n,
                 int32_t do_fill, uint32_t fill_color,
                 int32_t do_stroke, uint32_t stroke_color, int32_t stroke_width,
                 int32_t dashed);
    void (*clip_push)(void* ctx, int32_t l, int32_t t, int32_t r, int32_t b);
    void (*clip_pop)(void* ctx);
    int32_t (*is_printing)(void* ctx);
} cc_canvas_ops;

typedef struct cc_canvas { const cc_canvas_ops* ops; void* ctx; } cc_canvas;

/* Register the canvas used for LAYOUT-TIME text measurement (the original's
 * shared MM_TWIPS CClientDC). Must outlive all layout calls. */
void cc_set_metrics_canvas(cc_canvas* canvas);

#ifdef __cplusplus
}
#endif
#endif /* COMICCHAT_H */
