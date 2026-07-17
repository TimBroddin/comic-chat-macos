// bridge_crc32.cpp -- cc_crc32, kept in its own translation unit deliberately.
//
// avbfile.h declares its own `extern "C" int compress/compress2/uncompress`
// (inside `namespace ZLIB`, but extern "C" linkage is flat, so it still
// collides with zlib.h's declarations of the same names if both headers
// land in one TU) -- see bridge_art.cpp, which never includes <zlib.h> for
// exactly this reason. Isolating the zlib.h include here avoids the clash
// entirely while still linking against the same libz (see Package.swift's
// cchat-engine linkerSettings).

#include "comicchat.h"
#include <zlib.h>

extern "C" uint32_t cc_crc32(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) {
        return (uint32_t)crc32(0L, Z_NULL, 0);
    }
    return (uint32_t)crc32(crc32(0L, Z_NULL, 0), data, (uInt)len);
}
