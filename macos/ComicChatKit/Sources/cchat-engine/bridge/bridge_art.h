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

// Plan 2 Task 7 (R14(i)): decode an image DIB + separate mask DIB into one
// straight-alpha RGBA8 buffer (mask supplies the alpha, exactly as the
// pose-image golden path does). maskBmi/maskBits may be NULL for a fully-opaque
// plane. On success *outRgba is malloc'd; release it via cc_image_free.
// The CDC adapter's DrawPoseImage (mfc_compat.cpp) is the sole caller.
bool bridge_decode_dib_pair_to_rgba(BITMAPINFO* imgBmi, void* imgBits,
                                     BITMAPINFO* maskBmi, void* maskBits,
                                     int32_t* outWidth, int32_t* outHeight,
                                     uint8_t** outRgba);

// Plan 2 Task 7 review fix (R14(v)): decode a 1bpp aura plane DIB into
// straight-alpha RGBA8 using the aura's OWN pixel-exact semantics, not the
// image/mask polarity above. The aura plane's 2-entry palette is {index0 =
// white = background, index1 = black = silhouette footprint}. The original
// draws the aura with MERGEPAINT ALONE (no paired SRCAND, unlike the
// image/mask draw): dest = (NOT src) OR dest. Working the ROP algebra:
//   aura bit = 1 (black, silhouette):  dest = NOT(black) OR dest
//                                           = white OR dest = white
//                                           -> destination FORCED WHITE.
//   aura bit = 0 (white, background):  dest = NOT(white) OR dest
//                                           = black OR dest = dest (no-op)
//                                           -> destination UNTOUCHED.
// So the on-screen effect is a solid white halo silhouette, with the
// background showing through everywhere else -- opposite of naively
// decoding the aura plane through the image/mask path (which would render
// the black silhouette opaque and the white background as a fully-covering
// opaque box). This function reproduces that MERGEPAINT-alone effect as a
// single straight-alpha RGBA sprite: output RGB is always (255,255,255)
// (the only color MERGEPAINT can ever force the destination to), and alpha
// is 255 (opaque white) where the aura bit is 1 (black/silhouette), 0
// (fully transparent, background shows through) where the aura bit is 0
// (white/background). auraBmi/auraBits must be a 1bpp DIB (any other depth
// fails). On success *outRgba is malloc'd; release via cc_image_free.
// Returns false (outputs untouched) on failure.
bool bridge_decode_aura_to_white_alpha(BITMAPINFO* auraBmi, void* auraBits,
                                        int32_t* outWidth, int32_t* outHeight,
                                        uint8_t** outRgba);

#endif // BRIDGE_ART_H
