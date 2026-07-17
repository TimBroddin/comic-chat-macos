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

/* Temporary smoke-test hook for the avatar loading chain (Task 4). Loads the
 * .avb file at `path` via the original CAvatarFileStream/CAvatarX::LoadAvatar
 * code, copies its name into name_out (best-effort truncated to name_cap),
 * and writes its pose count into pose_count_out. Returns 0 on success,
 * non-zero on any failure. Replaced by the real cc_avatar_* API in Task 6. */
int32_t cc_smoke_load_avatar(const char* path, char* name_out, size_t name_cap,
                             int32_t* pose_count_out);

/* Temporary smoke-test hook for the backdrop loading chain (Task 5). Loads the
 * .bgb (or .bmp) file at `path` via the original
 * CAvatarFileStream/CChatBackdrop::LoadBackdrop code, copies the backdrop's
 * URL (best-effort truncated to name_cap; empty string if none) into
 * name_out, and writes the loaded DIB's width/height into width_out/
 * height_out. Returns 0 on success, non-zero on any failure. Replaced by the
 * real cc_backdrop_* API in Task 6. */
int32_t cc_smoke_load_backdrop(const char* path, char* name_out, size_t name_cap,
                               int32_t* width_out, int32_t* height_out);

#ifdef __cplusplus
}
#endif
#endif /* COMICCHAT_H */
