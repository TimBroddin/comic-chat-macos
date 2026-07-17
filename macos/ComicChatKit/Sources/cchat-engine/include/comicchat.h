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

#ifdef __cplusplus
}
#endif
#endif /* COMICCHAT_H */
