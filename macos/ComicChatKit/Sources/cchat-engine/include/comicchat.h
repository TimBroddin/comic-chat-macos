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

/* Plan 2 Task 10: strip session API + headless compositor selftest. Opens the
 * avatar at avatar_path (loaded twice -> two participants) and the backdrop at
 * backdrop_path (a real .bgb fixture), drives cc_strip_create/add_participant/
 * set_backdrop/add_line over a fixed 2x4 conversation, and asserts panel_count,
 * get_size==GetBBox, and (Step 3) the FULL recording-canvas compose log against
 * a frozen snapshot. Returns 0 on success (failure count otherwise). Kept out of
 * cc_run_selftests because it needs fixture paths. */
int32_t cc_run_strip_selftest(const char* avatar_path, const char* backdrop_path);

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
    /* draw_text's `y` is the GDI TA_TOP box-TOP -- the top of the text cell
     * in y-up space, matching the original's default GDI text-alignment mode
     * (TA_TOP, never changed to TA_BASELINE) -- NOT a CoreText/baseline-style
     * origin. Implementations whose text API positions by baseline must
     * convert: baseline = y - ascent (ascent from font_metrics/
     * cc_text_metrics, in the same twips space as `y`). Getting this
     * backwards shifts every glyph down by roughly one ascent's worth of
     * twips (final review; see also cc_text_metrics.ascent above). */
    void (*draw_text)(void* ctx, const cc_font_spec* f, int32_t x, int32_t y,
                      uint32_t color, int32_t bk_opaque, uint32_t bk_color,
                      const char* bytes, int32_t len);
    void (*fill_rect)(void* ctx, int32_t l, int32_t t, int32_t r, int32_t b,
                      uint32_t color);
    /* draw_image's dest rect edges may arrive in reversed order -- reversed
     * HORIZONTAL order (dr < dl) means mirror the image; this is how the
     * engine draws an avatar body facing the other way (FlipBodyBox,
     * engine/bodycam.cpp mirrors the dest rect the same way the original's
     * GDI StretchDIBits negative-width blit did). The vertical order (dt/db)
     * has never been observed reversed. The source rect is always
     * normal-order (the CDC adapter always passes 0,0,w,h -- see
     * shim/mfc_compat.cpp's DrawPoseImage/DrawAuraImage). Canvas
     * implementations that only draw upright must special-case dr < dl to
     * flip; naively normalizing with abs()/min() silently drops the mirror
     * (final review, Plan 2 Task 13 follow-on: this exact bug shipped once). */
    void (*draw_image)(void* ctx, const cc_image* img,
                       int32_t dl, int32_t dt, int32_t dr, int32_t db,
                       int32_t sl, int32_t st, int32_t sr, int32_t sb);
    void (*path)(void* ctx, const cc_path_pt* pts, int32_t n,
                 int32_t do_fill, uint32_t fill_color,
                 int32_t do_stroke, uint32_t stroke_color, int32_t stroke_width,
                 int32_t dashed);
    void (*clip_push)(void* ctx, int32_t l, int32_t t, int32_t r, int32_t b);
    /* clip_pop RESETS the clip to the unclipped base state — it is NOT a
     * balanced one-level pop of the most recent clip_push. This mirrors the
     * engine's only clip-reset path, the CDC adapter's
     * `SelectClipRgn(NULL, RGN_COPY)`, which collapses the ENTIRE clip stack
     * in one call; the engine never emits a call that undoes just one
     * clip_push. A canvas implementation that pops only one level per
     * clip_pop will leave earlier panels' clip rects active forever, which
     * collapses every later panel's drawing into panel 1's clip region. */
    void (*clip_pop)(void* ctx);
    int32_t (*is_printing)(void* ctx);
} cc_canvas_ops;

typedef struct cc_canvas { const cc_canvas_ops* ops; void* ctx; } cc_canvas;

/* Register the canvas used for LAYOUT-TIME text measurement (the original's
 * shared MM_TWIPS CClientDC). Must outlive all layout calls. */
void cc_set_metrics_canvas(cc_canvas* canvas);

/* ============================================================================
 * Scripted-strip session (Plan 2 Task 10). Drives the full lifted layout
 * engine end-to-end: opens participant avatars, wires the session user table
 * (the talk-to/camera graph), ingests scripted lines through the panel
 * orchestrator (CUnitPanelPage::AddLine), and composites the finished page onto
 * a cc_canvas headlessly (the R16 replacement for CUnitPanelPage::Draw).
 *
 * THREADING CONTRACT (Task 8 review amendment, binding): the engine uses
 * process-global mutable state -- the avatar registry (avatar.cpp `avatars[]`),
 * the session/settings (ccContext().session), the font statics
 * (CUnitPanelPage's CFontInfo/CFont statics), the backdrop registries
 * (backdrop.cpp backRecS/backMapS), and the composing-page back-pointer
 * (s_composingPage). There is NO internal locking. ALL cc_* calls (strip API
 * and every other cc_* entry point) must originate from ONE thread at a time;
 * concurrent calls are undefined behavior. The Swift Strip wrapper (Task 11)
 * documents and enforces the same single-threaded contract.
 *
 * Modes mirror defines.h BM_* values exactly (defines.h:63-66). */
enum { CC_MODE_SAY = 0x0001, CC_MODE_WHISPER = 0x0002,
       CC_MODE_THINK = 0x0004, CC_MODE_ACTION = 0x0008 };

typedef struct cc_strip cc_strip;

/* Create an empty strip session. Initializes the avatar registry, resets the
 * session user table, installs the comics fonts from the session face/size
 * defaults, and sets sane unit-panel geometry.
 *
 * ONE STRIP AT A TIME (final review, binding): cc_strip_create resets the
 * process-global registries a strip drives -- the avatar registry
 * (avatar.cpp `avatars[]`), the backdrop registries (backdrop.cpp
 * backRecS/backMapS), the session user table (ccContext().session), and the
 * emotion rule tables (textpose.cpp) -- and cc_strip_destroy frees all of
 * them. These tables are shared process-wide state, not per-handle state: a
 * second cc_strip_create before the first strip's cc_strip_destroy re-inits
 * the tables out from under the first handle's avatars/users, and destroying
 * either handle afterward frees state the other handle's `page` still
 * references. Two coexisting cc_strip handles are therefore a use-after-free,
 * not merely a data race -- create exactly one strip, drive it to
 * completion, and destroy it before creating the next. This is a stricter
 * requirement than the THREADING CONTRACT below (which governs concurrent
 * threads); it applies even to sequential code on a single thread.
 *
 * DETERMINISM (Task 8 review amendment, binding): the live layout path consumes
 * the global rand() stream -- CPanel::CPanel() seeds each panel with
 * m_seed = rand() (panel.cpp:604), and CUnitPanel::LayoutBalloons reseeds via
 * srand(m_seed) so per-panel balloon shift/fit is reproducible FROM that seed.
 * Because the seed itself is drawn from the global stream, an unseeded stream
 * would make identical scripts yield different strips depending on prior rand()
 * consumers. cc_strip_create() therefore seeds the global stream
 * deterministically (srand(0x5EED)) so identical scripts yield byte-identical
 * strips. */
cc_strip* cc_strip_create(void);
void      cc_strip_destroy(cc_strip* s);            /* no-op if NULL */

/* Open the avatar at avb_path, register it (GetAvatar(id) then finds it), create
 * its session user entry, and wire m_userInfo -> that entry (the Task 8 wiring
 * invariant). nick names the participant. Returns the assigned participant id
 * (>= 1) on success, -1 on failure. */
int32_t   cc_strip_add_participant(cc_strip* s, const char* nick,
                                   const char* avb_path);   /* >=0 id, -1 fail */

/* Load the .bgb at bgb_path and register it so every subsequently-created panel
 * inherits it (via ccContext().session.backdropID, read by CPanel::CPanel).
 * Returns 0 on success, non-zero on failure. Call before add_line for the
 * backdrop to appear in composed panels. */
int32_t   cc_strip_set_backdrop(cc_strip* s, const char* bgb_path); /* 0 ok */

/* Ingest one scripted line spoken by `speaker` (a participant id from
 * add_participant). `modes` is a bitwise-OR of CC_MODE_* (mirroring BM_*).
 * `addressees`/`n_addr` name who the speaker is talking to (participant ids);
 * they populate the speaker's talk-to graph (the camera's facing/order input).
 * `addressees` may be NULL only if `n_addr` is 0 (no addressees) -- NULL with
 * a nonzero `n_addr` is rejected (-1), not treated as zero addressees (final
 * review). Runs the original ingestion chain (talk-to wiring, ChatPreSendText
 * emotion inference, CUnitPanelPage::AddLine). Returns 0 on success. */
int32_t   cc_strip_add_line(cc_strip* s, int32_t speaker,
                            const char* text_bytes, uint32_t modes,
                            const int32_t* addressees, int32_t n_addr); /* 0 ok */

/* Number of panels laid out so far. */
int32_t   cc_strip_panel_count(const cc_strip* s);

/* Bounding box of the finished page in twips (width, height >= 0). Matches
 * CUnitPanelPage::GetBBox's row/column arithmetic. */
void      cc_strip_get_size(const cc_strip* s, int32_t* out_w, int32_t* out_h);

/* Composite the finished page onto `canvas` (the R16 headless replacement for
 * CUnitPanelPage::Draw). Returns 0 on success. */
int32_t   cc_strip_compose(cc_strip* s, cc_canvas* canvas); /* 0 ok */

/* ---- Protocol session (Plan 3): bytes in, events out ---------------------
 * The engine never opens a socket. Swift owns NWConnection and the event loop.
 * All calls are single-threaded (see the engine threading contract above).
 * Text buffers are wire bytes (CP-1252 by default); the engine does not
 * transcode — Swift converts for display. */

typedef struct cc_session cc_session;
typedef uint32_t cc_user_ref;     /* opaque per-user handle from resolve_user; 0 = unknown */
#define CC_USER_REF_NONE 0u

/* One-shot timer ids the engine may request (Swift schedules, then calls
 * cc_session_fire_timer). Exactly one is used today. */
#define CC_TIMER_ISIRCX_PROBE 1

typedef void        (*cc_send_fn)(void* user_data, const uint8_t* data, size_t len);
typedef void        (*cc_set_timer_fn)(void* user_data, int32_t timer_id, int32_t ms);
typedef void        (*cc_cancel_timer_fn)(void* user_data, int32_t timer_id);
struct cc_proto_event;  /* defined below in Task 5's block */
typedef void        (*cc_on_event_fn)(void* user_data, const struct cc_proto_event* ev);
typedef const char* (*cc_own_nick_fn)(void* user_data);   /* never NULL; "" if unknown */
typedef cc_user_ref (*cc_resolve_user_fn)(void* user_data, const char* nick, uint32_t room_token);

typedef struct cc_session_config {
    void*               user_data;
    cc_send_fn          send;
    cc_set_timer_fn     set_timer;
    cc_cancel_timer_fn  cancel_timer;
    cc_on_event_fn      on_event;
    cc_own_nick_fn      own_nick;
    cc_resolve_user_fn  resolve_user;
    const char*         local_host;    /* value gethostname would have returned; may be NULL */
    int32_t             encoding;      /* 0 = CP-1252 (default), 1 = UTF-8 (per spec §4.5) */
} cc_session_config;

cc_session* cc_session_create(const cc_session_config* cfg);   /* NULL on bad cfg */
void        cc_session_destroy(cc_session* s);                 /* no-op if NULL */
void        cc_session_feed_bytes(cc_session* s, const uint8_t* data, size_t len); /* no-op if s is NULL */
void        cc_session_fire_timer(cc_session* s, int32_t timer_id);                /* no-op if s is NULL */

/* Test-only hook (Task 1): drives one send("ECHO\r\n") + one on_event. Removed
 * once real parsing lands; kept behind CC_SESSION_TESTHOOK. */
void        cc_session_test_echo(cc_session* s);

#ifdef __cplusplus
}
#endif
#endif /* COMICCHAT_H */
