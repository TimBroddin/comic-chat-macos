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

/* Plan 4a Task 5: panel geometry API selftest. Opens the avatar at avatar_path
 * (one participant) and exercises cc_strip_set_panel_geometry/
 * get_panel_geometry: the create-time default (2300/2300/2, interstices 144),
 * an explicit set-then-get round trip, cc_strip_get_size reflecting the new
 * arithmetic after real lines are added, and the FRESH STRIP ONLY reject once
 * a line has been added. Returns 0 on success (failure count otherwise). Kept
 * out of cc_run_selftests because it needs the fixture path (adding a real
 * line requires a registered avatar; CPanel::FetchSpeaker dereferences
 * GetAvatar(uID)->m_body unconditionally). */
int32_t cc_run_panel_geometry_selftest(const char* avatar_path);

/* Plan 4a Task 6: avatar API selftest (cc_avatar_icon_image +
 * cc_strip_set_participant_avatar). Opens avatar_path as a standalone
 * cc_avatar and checks cc_avatar_icon_image; separately drives a cc_strip with
 * one participant loaded from avatar_path, switches that participant's avatar
 * to other_avatar_path via cc_strip_set_participant_avatar mid-strip (with a
 * line added on each side), and composes to a recording canvas. Returns 0 on
 * success (failure count otherwise). Kept out of cc_run_selftests because it
 * needs two fixture paths. */
int32_t cc_run_avatar_api_selftest(const char* avatar_path, const char* other_avatar_path);

/* Plan 4a Task 7: title/starring lift selftest. Opens avatar_path TWICE (two
 * participants) plus other_avatar_path (a third, added after a title is set --
 * exercises the add_participant/UpdateTitle member-join wiring). Drives
 * set_self, set_title, two lines, and asserts the composed recording-canvas
 * log carries the title text, "STARRING", and every participant's nickname in
 * the starring rows -- see cc_selftest_strip_title_starring's own comment for
 * the full assertion list. Returns 0 on success (failure count otherwise).
 * Kept out of cc_run_selftests because it needs two fixture paths. */
int32_t cc_run_strip_title_starring_selftest(const char* avatar_path, const char* other_avatar_path);

/* Plan 4b Task 2: emotion-wheel engine surface selftest. Opens avatar_path as
 * one participant, calls set_self, and exercises cc_strip_self_pose /
 * cc_strip_set_self_emotion / cc_strip_self_annotations / cc_strip_preview_self_text
 * against it (plus the "no self set" rejection case on a fresh strip with no
 * set_self call, and -- Plan 4b live-fix 4 -- a post-cc_strip_set_participant_avatar
 * switch case using other_avatar_path, proving the self APIs follow the CURRENT
 * avatar id, not the stale participant id). Returns 0 on success (failure count
 * otherwise). Kept out of cc_run_selftests because it needs the fixture paths. */
int32_t cc_run_self_emotion_selftest(const char* avatar_path, const char* other_avatar_path);

/* Plan 4b live-fix 7: cc_strip_self_preview selftest -- the head+torso DrawBody
 * preview regression pin. avatar_path MUST be a COMPLEX (two-part) avatar
 * (anna.avb): its body is a CBodyDouble, and cc_strip_self_preview drives
 * CBody::DrawBody, which emits BOTH the torso and head planes (>= 2 image
 * blits in the recording-canvas log) -- where the retired single-pose-record
 * preview path could emit at most one, rendering a headless body. Also checks
 * the "no self set" and degenerate-bounds rejections. Returns 0 on success
 * (failure count otherwise). Kept out of cc_run_selftests because it needs the
 * fixture path. */
int32_t cc_run_selfpose_preview_selftest(const char* avatar_path);

/* Comic hit-testing selftest (Plan 4b): opens avatarPath TWICE (two
 * participants) plus other_avatar_path (a third, switched onto participant 1
 * mid-strip so the AVATAR-vs-PARTICIPANT id reversal is exercised), builds the
 * fixed 2x4 conversation, composes, then drives cc_strip_hit_test_avatar /
 * cc_strip_hit_test_balloon against points derived from the LIVE panel/body/
 * balloon bboxes (deterministic under fake metrics): a point inside a known
 * body bbox -> that body's PARTICIPANT id; a point in an empty page margin -> 0;
 * a point inside a balloon -> its text bytes; the participant-id reversal after
 * a set_participant_avatar switch. Returns 0 on success (failure count
 * otherwise). Kept out of cc_run_selftests because it needs two fixture paths. */
int32_t cc_run_hit_test_selftest(const char* avatar_path, const char* other_avatar_path);

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

/* The member-list/picker icon pose (excluded from the pose API by design --
 * Plan 1 handover). NULL mask plane decodes opaque. 0 = ok. */
int32_t     cc_avatar_icon_image(const cc_avatar* av, cc_image* out);

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
 * Modes mirror defines.h BM_* values exactly (defines.h:63-67). CC_MODE_SOUND
 * (Plan 4b outbound-sound task, append-only) mirrors BM_SOUND (0x0010) -- it
 * is not a mode cc_session_send_say/send_whisper are meant to be called with
 * (those two never compose the SOUND CTCP themselves, see
 * cc_session_send_sound's doc comment); it exists so callers/tests can name
 * the bit symbolically when inspecting `modes`/wire behavior. */
enum { CC_MODE_SAY = 0x0001, CC_MODE_WHISPER = 0x0002,
       CC_MODE_THINK = 0x0004, CC_MODE_ACTION = 0x0008,
       CC_MODE_SOUND = 0x0010 };

/* ---- Annotation codec (Plan 3 Task 3) --------------------------------------
 * Decoded comic annotation block ("User Display Info"). Values are indices,
 * NOT the +'0' wire bytes. addressees are encoded nick strings (CP-1252).
 * Wire grammar (state-and-codec.md §3.3):
 *   #G<gp><ge><gi>E<ep><ee><ei>[R]M<m>[T<nick>[,<nick>...]]
 * Every <x> byte is IndexToByte(value) = value + '0' (protsupp.cpp:1023).
 * `cooked` is set only when both intensity fields have been written by the
 * decoder (protsupp.cpp:1538-1539). Declared here (ahead of the strip API,
 * moved up from its original spot ahead of the session API in Plan 3 Task 9)
 * because cc_strip_add_line_cooked below also takes a `const cc_annotations*`. */
#define CC_MAX_ADDRESSEES 5
typedef struct cc_annotations {
    int32_t gesture_pose, gesture_emotion, gesture_intensity;   /* G group */
    int32_t face_pose,    face_emotion,    face_intensity;      /* E group */
    int32_t requested;                                          /* R flag (0/1) */
    int32_t mode;                                               /* SM_* say mode 1..5 */
    int32_t addressee_count;
    char    addressees[CC_MAX_ADDRESSEES][64];                  /* encoded nicks */
    int32_t cooked;                                             /* both intensities present */
} cc_annotations;

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

/* Load the .avb at avb_path, register it, re-point participant's session user
 * at it. Existing panels keep the old avatar (original ChangeAvatarEntry
 * behavior); subsequent lines render with the new one. 0 = ok. */
int32_t   cc_strip_set_participant_avatar(cc_strip* s, int32_t participant,
                                          const char* avb_path);

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

/* Ingest one WIRE-RECEIVED line whose pose/emotion is already decided by a
 * decoded cc_annotations block (Plan 3 Task 9), instead of inferring the pose
 * from the text via ChatPreSendText (the original's SayEntry::Execute /
 * histent.cpp:80-108 "cooked" path, ported): when `ann->cooked` is nonzero,
 * this sets the speaker avatar's face/gesture pose directly from
 * `ann->face_pose`/`ann->gesture_pose` (SetIndices, for ordinary avatars) or,
 * for an OTHERMAPPED avatar, the closest-matching emotion/intensity pair
 * (SetEmotions, built from ann->face_emotion/face_intensity and
 * ann->gesture_emotion/gesture_intensity via BytesToEmotion) -- and does NOT
 * call ChatPreSendText, so an explicitly-annotated received pose is never
 * clobbered by a re-inferred one. When `ann` is NULL or `ann->cooked` is 0,
 * this behaves exactly like cc_strip_add_line (falls back to text inference)
 * -- a locally-typed / unannotated line still gets the original heuristic.
 * `modes`/`addressees`/`n_addr` behave exactly as in cc_strip_add_line.
 * Returns 0 on success. */
int32_t   cc_strip_add_line_cooked(cc_strip* s, int32_t speaker,
                                   const char* text_bytes, uint32_t modes,
                                   const int32_t* addressees, int32_t n_addr,
                                   const cc_annotations* ann); /* 0 ok */

/* Number of panels laid out so far. */
int32_t   cc_strip_panel_count(const cc_strip* s);

/* Bounding box of the finished page in twips (width, height >= 0). Matches
 * CUnitPanelPage::GetBBox's row/column arithmetic. */
void      cc_strip_get_size(const cc_strip* s, int32_t* out_w, int32_t* out_h);

/* Plan 4a Task 5: panel geometry API. Thin wrappers over
 * CUnitPanelPage::SetUnitPanelWidth/SetUnitPanelHeight/SetUnitPanelsPerRow
 * (engine/panel.h:156-158) and the interstice statics (engine/panel.cpp:70-76).
 * cc_strip_create's hard-seed (MINUNITPANELWIDTH/HEIGHT, 2/row) remains the
 * default for a freshly-created strip; these let a caller override it.
 *
 * FRESH STRIP ONLY: mirrors the original CPageView::SetPanelsWide, which always
 * reflows via ResetExistingPanels(TRUE) -- FreeRetainedPanelS + DestroyPages +
 * re-add + ExecuteHistory(HM_RELOAD) (pageview.cpp:1110-1125). The headless
 * engine has no history log to replay from, so reflow-after-lines is out of
 * scope for this task (by design, not an oversight): cc_strip_set_panel_geometry
 * REJECTS the call (returns nonzero) once any line has been added -- i.e. once
 * cc_strip_panel_count(s) > 0, since CUnitPanelPage::AddLine always produces at
 * least one panel. Call it before the first cc_strip_add_line/add_line_cooked. */
int32_t   cc_strip_set_panel_geometry(cc_strip* s, int32_t unit_w_twips,
                                      int32_t unit_h_twips, int32_t panels_per_row);
                                      /* 0 ok, nonzero if a line has been added */

/* Current panel geometry: unit width/height (twips), panels per row, and the
 * horizontal/vertical interstice constants (engine/panel.cpp:75-76; not
 * settable -- the original never exposes a setter for them either). */
void      cc_strip_get_panel_geometry(const cc_strip* s, int32_t* unit_w,
                                      int32_t* unit_h, int32_t* per_row,
                                      int32_t* h_interstice, int32_t* v_interstice);

/* Composite the finished page onto `canvas` (the R16 headless replacement for
 * CUnitPanelPage::Draw). Returns 0 on success. */
int32_t   cc_strip_compose(cc_strip* s, cc_canvas* canvas); /* 0 ok */

/* ---- Comic hit-testing (Plan 4b) -------------------------------------------
 * Click-an-avatar-in-the-strip to set your talk-to target, plus balloon-text
 * tooltips. Both transliterate the original's CPageView hit-test loops
 * (v2.5-beta-1-modern/pageview.cpp:663-702 FindAvatarUnderPoint /
 * :704-745 FindLabelUnderPoint) as bridge code walking m_panels -> per-panel
 * grid arithmetic -> element bboxes, WITHOUT the original's view-layer
 * DPtoLP/AddScrollOffset device<->logical machinery (the caller passes page
 * twips directly). Engine-queue only (every cc_* call, threading contract). */

/* cc_strip_hit_test_avatar: x/y in PAGE TWIPS (y-up, the SAME coordinate space
 * as cc_strip_get_size -- x in [0, width], y in [-height, 0]). Walks m_panels,
 * finds the panel whose grid slot contains the point (the original's
 * rowNum/colNum arithmetic, pageview.cpp:681-687), then walks that panel's
 * m_bodies and returns the PARTICIPANT id (>= 1, the same id space
 * cc_strip_add_participant returns and the caller/bridge use) of the first body
 * whose bbox contains the point, or 0 for no avatar there.
 *
 * PARTICIPANT vs AVATAR id (mind the two id spaces): CBody::m_avatarID is an
 * AVATAR-registry id, which equals the participant id at add-participant time
 * but DIVERGES after cc_strip_set_participant_avatar (which loads a fresh
 * avatar under a NEW registry id while the participant's session-user id is
 * unchanged -- the same divergence live-fix 3/4 handle). This function reverses
 * the body's avatar id back to the participant id via the session user table
 * (the entry whose CUserInfo::GetAvatarID() == body->m_avatarID), so the
 * returned id is always the one the bridge's participantIDs map and the UI's
 * talk-to selection key on -- never a stale avatar id. Returns 0 if no session
 * user currently owns that avatar id (a body from a since-replaced avatar). */
int32_t   cc_strip_hit_test_avatar(const cc_strip* s, int32_t x, int32_t y);

/* cc_strip_hit_test_balloon: the tooltip sibling. Same page-twips point + same
 * panel-grid walk, but scans the panel's m_elements for the first CBalloon
 * (GetType() & PE_BALLOON) whose bbox contains the point and copies its text
 * bytes (CBalloon's CLabel::m_str, the balloon's displayed line -- CP-1252,
 * already Capitalize'd at layout time) into `buf` (NUL-terminated, truncated to
 * buflen-1 bytes). Returns the number of bytes written (excluding the NUL, >= 0)
 * on a hit, or -1 for no balloon at that point / a NULL buf / buflen <= 0. On
 * a miss buf[0] is set to '\0' when buf/buflen allow. NOTE: this is the
 * balloon-TEXT tooltip the Plan 4b brief asked for, NOT the original's
 * OnToolHitTest, which showed the AVATAR's screen name (pageview.cpp:638-661) --
 * the balloon text is the strictly richer tooltip and CBalloon carries it
 * cleanly (m_str + a bbox), so the engine half lands here rather than being
 * skipped. */
int32_t   cc_strip_hit_test_balloon(const cc_strip* s, int32_t x, int32_t y,
                                     char* buf, int32_t buflen);

/* Plan 4a Task 7: title/starring panel (un-R11 lift of AddTitle/UpdateTitle/
 * AddStars/AddStarsAux + CStarLabel::Draw).
 *
 * cc_strip_set_title stores `title_bytes` (CP-1252) into
 * ccContext().session.comicsTitle and either builds the title panel fresh
 * (CUnitPanelPage::AddTitle, if no panels exist yet -- the title becomes
 * PANEL 0) or rebuilds the existing title panel's starring rows in place
 * (CUnitPanelPage::UpdateTitle, if panel 0 already exists), per the original
 * UpdateTitle's own branch (panel.cpp AddTitle/UpdateTitle). Calling this
 * BEFORE any cc_strip_add_participant/add_line makes panel 0 the title panel;
 * calling it after lines exist updates panel 0 (which by then is a real
 * speech panel from the FIRST line -- see the note below). Returns 0 on
 * success, non-zero if `s`/`title_bytes` is NULL.
 *
 * cc_strip_set_self records `participant` (a participant id from
 * add_participant) as the strip's own avatar -- the original's
 * GetChatDoc()->m_myAvatarID / MyAvatarID(), which this port has no doc to
 * back (R17: ccContext().session.selfParticipant). AddStars renders NOTHING
 * until a self participant is set (it early-returns, exactly like the
 * original -- panel.cpp AddStars' "not registered yet" guard) -- so
 * set_title BEFORE set_self renders a title-only panel (valid: the title/
 * starring-header labels exist, the starring ROWS don't yet). Once a title
 * already exists (comicsTitle non-empty), set_self also triggers UpdateTitle
 * so the just-registered self's starring row appears immediately. Returns 0
 * on success, non-zero if `s` is NULL or `participant` is not a known id.
 *
 * ORDER NOTE (panel 0 vs. line panels): AddTitle always AddPanel's onto the
 * TAIL of an empty page, so calling set_title on a strip with ZERO panels
 * makes the title panel panel 0. If lines were already added before
 * set_title is first called, panel 0 is whatever the first LINE built --
 * set_title's AddTitle branch only fires on m_panels.IsEmpty(), so in that
 * case it instead falls through and would need panels to already look like a
 * title panel for UpdateTitle's rebuild-in-place branch to be safe; callers
 * in this port always call set_title (and set_self) before the first
 * add_line/add_line_cooked, matching the "title panel is always panel 0"
 * contract Task 11 relies on. */
int32_t   cc_strip_set_title(cc_strip* s, const char* title_bytes); /* 0 ok */
int32_t   cc_strip_set_self(cc_strip* s, int32_t participant); /* 0 ok; starring order: self first */

/* Plan 4b Task 2: emotion-wheel engine surface. All four operate on the SELF
 * participant (cc_strip_set_self must have been called first; nonzero return
 * otherwise, matching this file's existing error-code convention where 0 is
 * the only success value and every failure is a nonzero code -- these use -1
 * uniformly, mirroring cc_strip_set_self's own -1-on-error style rather than
 * the brief sketch's `return 1`, which does not match the file's real
 * convention; see cc_compose.cpp for the exact per-branch codes).
 *
 * cc_strip_set_self_emotion: the wheel drag. Builds a CEmotion from
 * (angle_radians, intensity01) and runs the original CBodyCam::UpdateEmotion
 * chain minus the HWND drawing (bodycam.cpp:436: GetBodyFromEmotion then
 * UpdateBody) directly on the self avatar. intensity01 is clamped to [0,1]
 * here; the 0.2-center detente is the CALLER's job (GetEmotionFromPoint's UI
 * behavior, which stays in Swift -- bodycam.cpp:409-419).
 * DEVIATION from the brief's Step 3 sketch (recorded in the task report):
 * CEmotion's real constructor is CEmotion(double intensity, double emotion)
 * (avatar.h:62) -- intensity FIRST, angle/emotion SECOND -- the OPPOSITE
 * argument order the sketch wrote. This implementation uses the real order.
 *
 * cc_strip_preview_self_text: the typing preview. Runs
 * ChatPreSendText(text, self) (textpose.cpp:120) so the self avatar's pose
 * reflects what the text WOULD infer, without adding a line. Mutates avatar
 * pose state exactly like the original's per-edit preview (saywnd.cpp:975).
 * ChatPreSendText itself no-ops (silently) if ccContext().session.comicView
 * is false; it defaults true and nothing in this API clears it, so this is a
 * live path in practice, not a hidden failure mode of this call.
 *
 * cc_strip_self_pose: the self avatar's CURRENT pose id (1-based, into the
 * avatar's own m_arrPoses -- CreatePose/CreatePoseWithMask assign poseID =
 * array-position + 1, avatar.cpp:434's own comment) -- for driving the
 * wheel's live preview via cc_avatar_pose_image on a STANDALONE cc_avatar
 * handle of the SAME .avb file. DEVIATION/CAVEAT (recorded prominently in the
 * task report): this is NOT directly the same index space as
 * cc_avatar_pose_image's `idx` parameter -- that idx is a 0-based index into
 * a COMPACTED list that skips the icon pose (bridge_art.cpp's cc_avatar::
 * poseIndices), so idx = (poseIndices such that poseIndices[idx] == poseID-1).
 * A caller driving the wheel preview against a standalone cc_avatar must do
 * that translation (or, more simply, treat poseID-1 as the idx directly,
 * which is correct EXCEPT on the rare frame where the current pose happens to
 * BE the icon pose, which the avatar's normal body/emotion machinery never
 * selects in practice). Returns the pose id via out_pose_index; 0 = ok.
 *
 * cc_strip_self_annotations: fills `out` with the outbound annotation block
 * for the CURRENT self pose/emotion state -- face/torso pose indices (the
 * bInsertAnnotations sense: GetIndices' record-array indices, NOT the
 * cc_avatar_pose_image poseID space above -- these are TWO DIFFERENT index
 * spaces the original itself keeps distinct, see the task report's Step 1(c)
 * finding) + RAW emotion/intensity indices for both G (gesture/torso) and E
 * (face) groups -- mirroring bInsertAnnotations's own field reads
 * (protsupp.cpp:364-370: GetIndices + GetEmotions + EmotionToBytes x2), but
 * unwrapped back to the struct's index-space convention via ByteToIndex
 * (EmotionToBytes itself returns +'0' wire bytes, matching bInsertAnnotations's
 * needs; cc_annotations stores plain indices, per this header's own field
 * comment above, so this function inverts that wrapping before storing).
 * cooked=1. mode/addressees are NOT filled (left 0 / empty) -- caller's job. */
int32_t cc_strip_set_self_emotion(cc_strip* s, double angle_radians, double intensity01); /* 0 ok */
int32_t cc_strip_preview_self_text(cc_strip* s, const char* text_bytes); /* 0 ok */
int32_t cc_strip_self_pose(cc_strip* s, int32_t* out_pose_index); /* 0 ok */
int32_t cc_strip_self_annotations(cc_strip* s, cc_annotations* out); /* 0 ok */

/* cc_strip_self_preview: render the SELF participant's CURRENT posed body
 * (head + torso + masks, whatever the last wheel drag / typing preview set)
 * scaled-to-fit and centered into a `width_twips` x `height_twips` canvas --
 * the bodycam pane's own draw path (bodycam.cpp:499's
 * body->DrawBody(&memDC, rect2, FALSE)), the original's live self-pose
 * preview. This REPLACES the old cc_strip_self_pose -> cc_avatar_pose_image
 * single-record preview, which for a COMPLEX (two-part) avatar drew only the
 * torso record -- a headless body -- because such a body is a CBodyDouble
 * composited from a SEPARATE head and torso pose (CBodyDouble::DrawBody).
 * Driving CBody::DrawBody directly emits BOTH planes for a complex avatar
 * (>= 2 image blits) and the single plane for a simple one. Scale/center/
 * aspect are NOT computed by the caller: DrawBody's GetBodyBox aspect-fits and
 * centers the body into the clientRect for both shapes. The canvas is
 * MM_TWIPS y-up (a page of height_twips spans y in [-height_twips, 0]); pass
 * the SAME width/height twips the canvas was created with. Returns 0 on
 * success, -1 when no self is set / the participant no longer resolves / the
 * avatar has no body -- the caller then renders no preview. See cc_compose.cpp
 * for the full derivation. */
int32_t cc_strip_self_preview(cc_strip* s, cc_canvas* canvas,
                              int32_t width_twips, int32_t height_twips); /* 0 ok */

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
    /* Plan 4a Task 2: login identity fields for cc_session_login (the lifted
     * HrIrcLogin's szUserName/szRealName -- ircsock.cpp:596-668, never ported
     * as wire-emitting code; see cc_session_login's doc comment). Both
     * nullable -- cc_session_login falls back to the own_nick resolver's
     * value for either when NULL/empty, matching HrIrcLogin's own
     * `if (szUserName == NULL) szUserName = GetMyUserName()`-style fallback
     * (this port has no GetMyUserName/GetMyRealName equivalent, so the
     * fallback is the nick itself, the same "at least send something valid"
     * posture). Appended after encoding -- append-only, R9-style. */
    const char*         own_user;      /* USER command's <user> field; NULL -> own_nick */
    const char*         own_realname;  /* USER command's <realname> field; NULL -> own_nick */
} cc_session_config;

cc_session* cc_session_create(const cc_session_config* cfg);   /* NULL on bad cfg */
void        cc_session_destroy(cc_session* s);                 /* no-op if NULL */
void        cc_session_feed_bytes(cc_session* s, const uint8_t* data, size_t len); /* no-op if s is NULL */
void        cc_session_fire_timer(cc_session* s, int32_t timer_id);                /* no-op if s is NULL */

/* Test-only hook (Task 1): drives one send("ECHO\r\n") + one on_event. Removed
 * once real parsing lands; kept behind CC_SESSION_TESTHOOK. */
void        cc_session_test_echo(cc_session* s);

/* ---- Inbound event vocabulary (Plan 3 Task 5a) -----------------------------
 * The complete set of events the engine can emit through cc_on_event_fn
 * (see cc_session_config above). Every enum value has a matching union
 * member below -- this is a strict 1:1, completeness-is-the-deliverable
 * contract; later tasks (5b's parser, 6's payload stage) fill these at
 * parse sites and Swift (Task 7) mirrors them 1:1. All `const char*`
 * fields are CP-1252 bytes valid only for the duration of the on_event
 * callback -- copy them if you need to keep them. */
typedef enum cc_proto_event_type {
    CC_EV_NONE = 0,
    /* connection lifecycle */
    CC_EV_LOGGED_IN,          /* 001 -> own actual nick */
    CC_EV_SERVER_CAPS,        /* 800 -> ircx?, max_msg_len */
    CC_EV_DISCONNECTED_HINT,  /* fatal ERROR text */
    /* membership */
    CC_EV_SELF_JOINED, CC_EV_SELF_PARTED,
    CC_EV_USER_JOINED, CC_EV_USER_PARTED, CC_EV_USER_QUIT,
    CC_EV_KICKED, CC_EV_INVITED,
    CC_EV_NAMES, CC_EV_END_OF_NAMES,
    CC_EV_NICK_CHANGED,
    /* messages (the core comic events) */
    CC_EV_TEXT,               /* PRIVMSG/NOTICE: nick, target, text, kind, annotations? */
    CC_EV_DATA,               /* IRCX DATA CCUDI1: annotations */
    CC_EV_WHISPER,
    CC_EV_ACTION, CC_EV_SOUND, CC_EV_AWAY_PEER,   /* CTCP-derived */
    CC_EV_APPEARS_AS,         /* "# Appears as name.url" avatar announce */
    /* room state */
    CC_EV_TOPIC_CHANGED, CC_EV_CHANNEL_MODE, CC_EV_USER_MODE,
    CC_EV_ROOM_PROP,          /* PROP CLIENT (bk backdrop etc.) */
    CC_EV_ROOM_LIST_BEGIN, CC_EV_ROOM_LIST_ITEM, CC_EV_ROOM_LIST_END,
    CC_EV_WHOIS_RESULT, CC_EV_WHO_RESULT,
    CC_EV_MOTD,
    /* errors & prompts (Swift owns retry/prompt) */
    CC_EV_ERROR,              /* typed error + human text (from numerics/AfxMessageBox) */
    CC_EV_NICK_REJECTED,      /* 431/432/433 -> Swift retries */
    CC_EV_AUTH_UNSUPPORTED,   /* SSPI path dropped (R21) */
    CC_EV_STATUS_LINE,        /* catch-all status text (permissive: unknown numerics) */
} cc_proto_event_type;

typedef struct cc_proto_event {
    cc_proto_event_type type;
    uint32_t room_token;          /* 0 if not room-scoped */
    union {
        /* connection lifecycle */
        struct { const char* nick; } logged_in;                        /* CC_EV_LOGGED_IN */
        struct { int32_t ircx; int32_t max_msg_len; } server_caps;     /* CC_EV_SERVER_CAPS */
        struct { const char* text; } disconnected_hint;                /* CC_EV_DISCONNECTED_HINT */
        /* membership */
        struct { const char* channel; } self_joined;                   /* CC_EV_SELF_JOINED */
        struct { const char* channel; } self_parted;                   /* CC_EV_SELF_PARTED */
        struct { const char* nick; const char* ident; } user_joined;   /* CC_EV_USER_JOINED */
        struct { const char* nick; const char* reason; } user_parted;  /* CC_EV_USER_PARTED */
        struct { const char* nick; const char* reason; } user_quit;    /* CC_EV_USER_QUIT */
        struct { const char* kicker; const char* kickee; const char* reason;
                 const char* channel; } kicked;                        /* CC_EV_KICKED */
        struct { const char* by; const char* ident;
                 const char* channel; } invited;                       /* CC_EV_INVITED */
        struct { const char* channel; const char* nicks; } names;      /* CC_EV_NAMES (space-joined) */
        struct { const char* channel; } end_of_names;                  /* CC_EV_END_OF_NAMES */
        struct { const char* old_nick; const char* new_nick;
                 int32_t is_self; } nick_changed;                      /* CC_EV_NICK_CHANGED */
        /* messages (the core comic events) */
        struct { const char* nick; const char* ident; const char* target;
                 const char* text; int32_t kind;      /* MT_* */
                 int32_t has_annotations; cc_annotations annotations; } text;   /* CC_EV_TEXT */
        struct { const char* nick; cc_annotations annotations; } data; /* CC_EV_DATA */
        struct { const char* nick; const char* ident; const char* text;
                 int32_t has_annotations; cc_annotations annotations; } whisper; /* CC_EV_WHISPER */
        struct { const char* nick; const char* text;
                 int32_t has_annotations; cc_annotations annotations; } action;  /* CC_EV_ACTION */
        struct { const char* nick; const char* file; const char* text; } sound;  /* CC_EV_SOUND */
        struct { const char* nick; const char* message; } away_peer;   /* CC_EV_AWAY_PEER */
        struct { const char* nick; const char* avatar_name;
                 const char* url; } appears_as;                        /* CC_EV_APPEARS_AS (url may be "" or "?") */
        /* room state */
        struct { const char* channel; const char* topic; } topic_changed;   /* CC_EV_TOPIC_CHANGED */
        struct { const char* channel; const char* modes;
                 const char* arg; } channel_mode;                      /* CC_EV_CHANNEL_MODE (delta) */
        struct { const char* nick; const char* modes; } user_mode;     /* CC_EV_USER_MODE */
        struct { const char* key; const char* value; } room_prop;      /* CC_EV_ROOM_PROP */
        struct { int32_t truncated; } room_list_begin;                 /* CC_EV_ROOM_LIST_BEGIN */
        struct { const char* name; int32_t users;
                 const char* topic; } room_list_item;                  /* CC_EV_ROOM_LIST_ITEM */
        struct { int32_t truncated; } room_list_end;                   /* CC_EV_ROOM_LIST_END */
        struct { const char* nick; const char* user; const char* host;
                 const char* real; int32_t purpose; } whois_result;    /* CC_EV_WHOIS_RESULT */
        struct { const char* nick; const char* user; const char* host;
                 const char* channel; int32_t purpose; } who_result;   /* CC_EV_WHO_RESULT */
        struct { const char* luser; const char* motd; } motd;          /* CC_EV_MOTD */
        /* errors & prompts (Swift owns retry/prompt) */
        struct { int32_t code; const char* text; } error;              /* CC_EV_ERROR */
        struct { int32_t kind; const char* bad_nick; } nick_rejected;  /* CC_EV_NICK_REJECTED */
        struct { int32_t dummy; } auth_unsupported;                    /* CC_EV_AUTH_UNSUPPORTED */
        struct { const char* text; } status_line;                      /* CC_EV_STATUS_LINE */
    } u;
} cc_proto_event;

/* ---- Outbound command builders (Plan 3 Task 4) -----------------------------
 * Thin wrappers over ircproto.cpp's CIrcProto builders (engine/ircproto.{h,cpp}).
 * Every one of these ultimately calls CIrcProto::SendMessageText, which
 * reaches cfg.send -- the single outbound choke point (see ircproto.cpp's
 * file header). All return 0 on success, non-zero on failure (mirrors the
 * lifted BOOL builders: FALSE -> non-zero here).
 *
 * room_token identifies a joined room: 0 (CC_ROOM_TOKEN_NONE) is never a
 * valid room. cc_session_register_room assigns a fresh token for a channel
 * name (Swift calls this on join-confirm once Task 7 wires the real inbound
 * flow; this task's own selftests call it directly to seed the mapping).
 * See bridge/cc_session.h's header comment for the full token<->channel
 * scheme and its documented single-CIrcProto-per-session simplification. */
#define CC_ROOM_TOKEN_NONE 0u

/* Register (or re-register) a channel name and return its room_token. Token
 * assignment is dense and stable for the session's lifetime (see
 * cc_session.h). channel must be the WIRE-encoded channel name (e.g.
 * "#comicrig") -- callers that have a pretty/decoded name must EncodeChan it
 * first (not this function's job; it does no codec work itself). */
uint32_t cc_session_register_room(cc_session* s, const char* channel);
/* Look up a room_token's channel name; "" if token is unknown/out of range. */
const char* cc_session_room_channel(cc_session* s, uint32_t room_token);
/* Live-fix 1 (Plan 4b): overwrites an ALREADY-registered token's channel
 * string in place (the SAME token, no new registration) -- originally added
 * for the one case `cc_session_register_room` cannot handle: a server that
 * echoes a join confirm using DIFFERENT case than the channel name Swift
 * originally registered (e.g. we `JOIN #crypt`, the server's JOIN echo/every
 * subsequent PRIVMSG names `#Crypt`). IRC channel names are case-insensitive
 * (RFC 1459 Section 2.3.1).
 *
 * Plan 4b live-fix 6, Fix 1 update: `ccSessionRoomTokenForChannel`'s scan
 * (engine/ircsock.cpp) is now ITSELF case-insensitive (`stricmp`, matching
 * the original's `LookupDoc`, v2.5-beta-1-modern/chatdoc.cpp:2021-2031) --
 * so a token registered under one casing NO LONGER fails to resolve inbound
 * wire lines naming that room under a different casing; that half of the
 * original problem this function solved is now handled at the resolution
 * site directly. This function remains necessary for a NARROWER reason:
 * `cc_session_room_channel` (used by every OUTBOUND builder --
 * `announceAvatar`/`say`/`part`/... -- to turn a room_token back into the
 * channel STRING that goes on the wire) returns whatever string is
 * CURRENTLY registered for that token, verbatim, with no case-folding of its
 * own. Left un-updated, outbound wire lines would keep using our ORIGINAL
 * casing forever, even after the server's JOIN confirm revealed a different
 * one -- cosmetically wrong (case-insensitive IRC servers don't care) but a
 * fidelity/consistency gap against the original's own display convention
 * (it always adopted the server's casing). Swift calls this once it learns
 * the server's authoritative casing (the join confirm) so the ENGINE's
 * ANNOUNCED/outbound-facing casing -- not just Swift's own bookkeeping --
 * converges on the server's casing too. No-op (returns non-zero) if
 * room_token is out of range; channel must be the WIRE-encoded channel name,
 * matching cc_session_register_room's own contract. */
int32_t cc_session_update_room_channel(cc_session* s, uint32_t room_token, const char* channel);

int32_t cc_session_join(cc_session* s, const char* channel, const char* key /*nullable*/);
int32_t cc_session_part(cc_session* s, uint32_t room_token, const char* reason /*nullable*/);
int32_t cc_session_create_room(cc_session* s, const char* channel, const char* creation_modes /*nullable*/, uint32_t max_users, const char* key /*nullable*/);
int32_t cc_session_change_nick(cc_session* s, const char* new_nick);
int32_t cc_session_set_topic(cc_session* s, uint32_t room_token, const char* topic);
int32_t cc_session_kick(cc_session* s, uint32_t room_token, const char* nickname, const char* reason /*nullable*/);
int32_t cc_session_invite(cc_session* s, uint32_t room_token, const char* nickname);
int32_t cc_session_ban(cc_session* s, uint32_t room_token, const char* ban_pattern, int32_t ban /*1=ban,0=unban*/);
int32_t cc_session_set_mode(cc_session* s, uint32_t room_token, uint32_t new_mode, uint32_t new_max_users, const char* new_password /*nullable*/);
int32_t cc_session_away(cc_session* s, int32_t is_away, const char* message /*nullable*/);

/* send_say: builds the annotation block from `ann` (see the deviation note in
 * cc_session.cpp -- encodes `ann`'s already-decoded fields directly per the
 * "#G...E...M...[T...]" grammar, rather than reconstructing fake CAvatarX/
 * CUserInfo objects to feed the lifted bInsertAnnotations) and sends it via
 * CIrcProto::bChatSendToChannel. `ann` may be NULL (no annotation block, bare
 * text). `modes` is a bitwise CC_MODE_x / BM_x value (SAY/WHISPER/THINK/etc). */
int32_t cc_session_send_say(cc_session* s, uint32_t room_token, const cc_annotations* ann, const char* text, uint16_t modes);
/* send_whisper: same annotation handling as send_say, sent to each of
 * `nicks` via CIrcProto::bChatSendPrivMesg (one wire send per nick, matching
 * the original's per-addressee bChatSendPrivMesg loop -- see cc_session.cpp). */
int32_t cc_session_send_whisper(cc_session* s, uint32_t room_token, const cc_annotations* ann, const char* text, const char* const* nicks, int32_t nick_count);

int32_t cc_session_who(cc_session* s, const char* mask /*nullable*/);
int32_t cc_session_list(cc_session* s, const char* query /*nullable, "LIST"/"LISTX" + optional channel filter*/);

/* Sends the "MODE ISIRCX" probe (bExecuteQuery's ctModeIsIrcX case) and
 * requests the one-shot 50s timeout timer (CC_TIMER_ISIRCX_PROBE) via
 * cfg.set_timer -- the engine REQUESTS, Swift SCHEDULES (R21-adjacent; the
 * engine never runs its own timers). In the original this probe is sent from
 * CIrcSocket::OnConnect (ircsock.cpp:1050-1052, connection-establishment
 * flow, not lifted this task) which arms the timer itself via
 * ::AfxGetMainWnd()->SetTimer; this wrapper is the Task-4-scope equivalent so
 * the timer-request wiring exists and is tested NOW, ahead of Task 5b/7
 * wiring the real OnConnect call site (which will call this same C function,
 * or fold its two lines directly into the connection-established handler --
 * either is equivalent since bExecuteQuery+set_timer here has no other
 * side effect). Cancel the timer via cfg.cancel_timer(CC_TIMER_ISIRCX_PROBE)
 * on whichever of {IRCX capability reply, ERR_NOTREGISTERED, timer fires}
 * happens first (HrModeIsIrcXFailure's original disambiguation, ircsock.cpp:
 * 560 -- Task 5b's job once the reply handlers exist). */
int32_t cc_session_probe_ircx(cc_session* s);

/* Plan 4a Task 2: the plain-IRC login (NICK/USER), Swift-driven equivalent of
 * the lifted-but-never-wire-emitting HrIrcLogin (v2.5-beta-1-modern/
 * ircsock.cpp:596-668; the port's ircsock.cpp kept HrModeIsIrcXFailure's
 * *trigger* -- dequeue the probe cell, cancel the timer -- but explicitly
 * deferred the PASS/NICK/USER emission itself to this task, see that
 * function's header comment at ircsock.cpp:476-501). Sends "NICK <nick>\r\n"
 * (via CIrcProto::ChatChangeNick, same builder cc_session_change_nick uses)
 * then, if not already registered this connection, "USER <user> <host> . :
 * <realname>\r\n" (cfg.own_user/own_realname, falling back to the own_nick
 * resolver's value for either if NULL/empty -- HrIrcLogin's own fallback
 * posture) -- byte-for-byte the original's sprintf grammar (verified against
 * the real 1998 capture's client c2s line, smoke-2.jsonl: "USER Anonymous
 * Tims-Mac . :Your Full Name\r\n"). No PASS support (cc_session_config has no
 * password field; every login this port drives is anonymous/no-auth, R21).
 * Callers: the probe's 451 fallback, the probe timeout, and the IRCX
 * second-800 pivot (see ProtocolSession's sendLoginIfNeeded). Idempotent
 * within one connection for the USER half (sock.m_bRegistered guards it,
 * exactly like the original); NICK is resent every call (harmless -- a
 * server-side no-op if unchanged, and this function is only ever called
 * once per session by its Swift caller, guarded by a one-shot flag there). */
int32_t cc_session_login(cc_session* s);

/* Plan 4a Task 8: outbound avatar announce, the Swift-driven equivalent of
 * CRoomInfo::ChatAnnounceNewAvatar (v2.5-beta-1-modern/protsupp.cpp:817-843
 * -- READ-ONLY original, not present in the lifted engine/protsupp.cpp at
 * all: that file lifted only the RECEIVE-side "# Appears as" parser
 * (ProcessComment), never this SEND-side builder). Builds:
 *   "# Appears as <name>"       (url NULL or empty)
 *   "# Appears as <name>.<url>" (url non-empty)
 * -- APPEARSPREFIX's exact text (ircproto.h:83, " Appears as "), reproduced
 * as an R16 bridge-side string builder in cc_session.cpp rather than an
 * un-wrap: the original method lives on CRoomInfo, a base this port's
 * CIrcProto does not inherit from (ircproto.cpp's own structural note), so
 * there is no CRoomInfo file to un-wrap the string-builder half out of;
 * cc_session.cpp's ccEncodeAnnotations (Plan 3 Task 4) already established
 * this exact precedent -- a bridge-side reimplementation of an original
 * string-building routine, citing the original's sprintf grammar byte for
 * byte. Sent as the ANNOTATIONS argument with an EMPTY message via
 * CIrcProto::bChatSendToChannel/bChatSendPrivMesg (bChatSendToTarget's own
 * IsIRCX() branch then picks "DATA <target> CCUDI1 :<annotations>\r\n"
 * (IRCX, no PRIVMSG line since the message is empty) vs plain
 * "PRIVMSG <target> :<annotations>\r\n" -- inherited for free, exactly the
 * brief's D4 §4 requirement).
 *
 * `to_nick` NULL sends channel-wide (bChatSendToChannel, protsupp.cpp:839's
 * "initially we send a spurious announcement prior to connection" case);
 * non-NULL sends a private reply-announce to that nick via
 * bChatSendPrivMesg (protsupp.cpp:868-877's ProcessComment rule: on first
 * "# Appears as" from a not-yet-comic-user peer, reply privately with our
 * own avatar). `name` NULL/empty is sent as "NONE" (protsupp.cpp:830-831's
 * fallback -- an empty avatar name is never sent literally empty).
 *
 * DROPPED (R20, each individually noted): (1) the original's
 * `IsIRCX() || g_bSendComicsData || bForce` all-or-nothing send gate --
 * `g_bSendComicsData` is a UI settings toggle never lifted into this
 * engine, and `bForce` doesn't exist in this signature at all; this
 * function always builds and sends (the original's gate decided WHETHER to
 * send at all, not the grammar -- deferring that decision to the caller,
 * same posture as (2) below). (2) `GetConnectionStatus() != CX_INCHANNEL`
 * -- a CRoomInfo virtual this CIrcProto doesn't inherit (same structural gap
 * bExecuteQuery's own DEVIATION comment documents); connection-status
 * gating is Task 7's CCSession's job, done at the C-function layer by the
 * Swift caller's `onQueueGated` (connectionStatus == .connected) exactly as
 * bExecuteQuery's comment prescribes -- this function has no independent
 * connection-status check of its own, mirroring bExecuteQuery.
 *
 * Reentrancy: follows cc_session_login's save/restore g_session pattern
 * (this function CAN be called from inside an active engine frame, e.g. a
 * reply-announce triggered synchronously from the inbound "# Appears as"
 * dispatch once Task 9 wires that up -- unconditionally nulling g_session
 * on exit would clobber a still-active outer frame's session pointer).
 * 0 = ok, non-zero on a NULL session or an unknown/unregistered room_token
 * (`name` NULL/empty is NOT a failure -- see the "NONE" fallback above). */
int32_t cc_session_announce_avatar(cc_session* s, uint32_t room_token,
                                   const char* to_nick, const char* name,
                                   const char* url);

/* Plan 4b outbound-sound task: the Swift-driven equivalent of
 * bChatSendSound (v2.5-beta-1-modern/protsupp.cpp:3292-3409 -- READ-ONLY
 * original; the lifted engine's protsupp.cpp only ever ported the RECEIVE
 * side of SOUND, ccPrepareSound -- there is no bChatSendSound in this
 * port's engine/protsupp.cpp at all, same structural gap
 * cc_session_announce_avatar's own doc comment documents for
 * ChatAnnounceNewAvatar). This is a bridge-side reimplementation, not an
 * un-wrap, following that same precedent.
 *
 * FINDING (recorded per the task brief): cc_session_send_say/send_whisper do
 * NOT compose the SOUND CTCP for a caller that merely passes CC_MODE_SOUND/
 * BM_SOUND as `modes` -- they hand `text` straight through as bChatSendToTarget's
 * `szMesg` parameter unmodified (see cc_session.cpp's own bodies). In the
 * original, the CTCP framing is built by bChatSendSound itself, BEFORE
 * bChatSendToChannel is ever called -- bChatSendToChannel/bChatSendToTarget
 * only chunk/wrap an ALREADY-composed message, they never build the
 * "\x01SOUND ...\x01" bytes from a bare filename. So a dedicated builder is
 * required; this function is that builder.
 *
 * WIRE GRAMMAR (wire-COMPATIBLE quoted-filename form of bChatSendSound's
 * sprintf, protsupp.cpp:3349: `sprintf(GetOutBuff(), "%.*s %s %s%c",
 * g_nSoundLen, soundID, szQuotedSnd, szControlFull ? szControlFull :
 * szMesg, 0x1)` -- NOT byte-identical for plain names: the original's
 * CTCPQuoteString (histent.cpp:772) leaves an unproblematic filename BARE
 * (`\x01SOUND boing.wav \x01`) where this builder always quotes; the
 * original receiver ccPrepareSound accepts BOTH branches, so the forms
 * interoperate -- review sweep I-1 recorded this wording correction):
 *   \x01SOUND "<file>" <text>\x01
 * `file` is wrapped in literal double-quotes here (matching the selftest's
 * VECTOR 17 ground truth, cc_selftest_pv_sound_ctcp: `:Bob!bob@h PRIVMSG
 * #comicrig :\x01SOUND "boing.wav"\x01`) -- the quoted form needs no CTCP
 * low-level escaping (histent.cpp's CTCPQuoteString, out of this lifted
 * engine's scope, same deviation ccPrepareSound's own doc comment records
 * for the receive side) and matches ccPrepareSound's quoted-filename parse
 * branch exactly, so a self-send round-trips through this engine's own
 * inbound parser. `text` may be empty (an empty trailing message is legal --
 * ccPrepareSound's parser only special-cases an EMPTY szSound, i.e. no
 * filename at all, not an empty message).
 *
 * Sent via CIrcProto::bChatSendToChannel with `uModes = BM_SOUND` (so the
 * ALREADY-lifted verbatim multi-chunk fallback in bChatSendToTarget --
 * ircproto.cpp's `case BM_SOUND: nPrefixLen = g_nSoundLen + 1` branch, and
 * the "re-prefix following chunks as ACTION" continuation logic just below
 * it -- applies for free to an oversized sound line, exactly as it does in
 * the original). No whisper form (the original's whisper-sound path,
 * saywnd.cpp's `bWhisperInBox(... BM_WHISPER|BM_SOUND)`, is out of this
 * task's scope -- the brief only asks for "sends to the ACTIVE room").
 *
 * Local playback is NOT this function's job (R20/R21 posture, matching
 * cc_session_announce_avatar's own connection-status-gating precedent) --
 * bFindAndPlaySound is UI-side; Swift's ChatSessionModel.sendSound plays the
 * file locally itself and fires the same onSound callback path the inbound
 * CC_EV_SOUND case uses, per the task brief.
 *
 * 0 = ok, non-zero on a NULL session, NULL/empty `file`, or an unknown/
 * unregistered room_token. `text` NULL is treated as "" (an empty trailing
 * message), matching cc_session_send_say's NULL-annotations posture of
 * tolerating an absent optional field rather than failing. */
int32_t cc_session_send_sound(cc_session* s, uint32_t room_token,
                              const char* file, const char* text /*nullable*/);

#ifdef __cplusplus
}
#endif
#endif /* COMICCHAT_H */
