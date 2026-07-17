// bridge_art.h — declares the one bridge_art.cpp entry point the shim's CDC
// adapter needs (Plan 2 Task 3's StretchDIBits): decoding a raw palettized
// DIB (BITMAPINFO* + bits) into RGBA8. See bridge_art.cpp for the full
// ownership/decode documentation.
#ifndef BRIDGE_ART_H
#define BRIDGE_ART_H

#include "mfc_compat.h"
#include <cstdint>

// Decodes bmi/bits (exactly what Win32's StretchDIBits itself takes) into
// freshly malloc'd top-down RGBA8 (no mask -- StretchDIBits has no separate
// mask-plane concept). On success, *outRgba is malloc'd and must be released
// by the caller (matches cc_image's ownership contract -- pass through
// cc_image_free). Returns false (outputs untouched) on failure.
bool bridge_decode_dib_to_rgba(BITMAPINFO* bmi, void* bits,
                                int32_t* outWidth, int32_t* outHeight,
                                uint8_t** outRgba);

#endif // BRIDGE_ART_H
