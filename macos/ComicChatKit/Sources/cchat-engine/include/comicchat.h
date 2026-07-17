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

#ifdef __cplusplus
}
#endif
#endif /* COMICCHAT_H */
