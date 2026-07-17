// bridge_art.cpp — permanent C art API (Task 6). Wraps the lifted
// CAvatarX/CPose/CChatBackdrop/CAvatarDIB object graph (avatar.h, avbfile.h,
// backdrop.h) and decodes indexed DIBs into straight-alpha RGBA8 buffers.
//
// Ownership summary (see the memory-ownership audit in task-6-report.md for
// the exhaustive per-path walkthrough):
//   - cc_avatar_open() heap-allocates a cc_avatar that owns exactly one
//     CAvatarX* (which itself owns its CAvatarStream* once SetStream() is
//     called, and its CPose array, transitively -- see avatar.cpp's
//     ~CAvatarX / ~CPose). cc_avatar_close() deletes the wrapper's CAvatarX*
//     exactly once, which cascades to the stream and every pose's DIBs.
//   - cc_backdrop_open() mirrors this for CChatBackdrop* (owns its m_pDIB).
//   - cc_image.rgba is malloc'd fresh by decodeDibToRgba(); the caller must
//     release it via cc_image_free() (and only that function -- never a
//     bare free()). On any failure we return non-zero and leave *out
//     zeroed, so cc_image_free() is always safe to call on it.
//   - No C++ exception ever crosses these functions: every throwing engine
//     call (CAvatarX::LoadAvatar, CChatBackdrop::LoadBackdrop, CPose::Load
//     all use MFC-style TRY/CATCH_ALL internally already) is additionally
//     wrapped here in try/catch(...) so an unexpected std::bad_alloc etc.
//     still degrades to NULL/nonzero instead of unwinding into Swift.

#include "comicchat.h"
#include "mfc_compat.h"
#include "bbox.h"
#include "pe.h"
#include "dib.h"
#include "avbfile.h"
#include "avatar.h"
#include "avatario.h"
#include "backdrop.h"
#include "bridge_art.h"
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ============================================================================
// RGBA decode
//
// Transparency rule provenance (do not invent a different one -- this is
// read directly off the original 1998 code, not guessed):
//
//   CPose::ConvertMasksCommon (avbfile.cpp, ~line 1642-1652) splits a 2bpp
//   AIP_MASKEDMONO/AIP_DUALMASK source pixel pair `v` into three 1bpp planes,
//   all sharing the same 2-entry MonochromePalette (index 0 = white
//   RGB(255,255,255), index 1 = black RGB(0,0,0); avbfile.cpp line 15):
//     m_pdibs[0] (image) bit = v & 1
//     m_pdibs[1] (mask)  bit = (v >> 1) & 1
//     m_pdibs[2] (aura)  bit = v != 0
//   Immediately after, avbfile.cpp forces
//     image_byte &= mask_byte
//   with the comment "Bug in drawing code does not allow the image's pixels
//   to be black in the area where the aura is [sic -- refers to the mask]."
//   i.e. wherever the mask bit is 0 (white), the image bit is forced to 0
//   (white) too.
//
//   The actual draw sequence that consumes this (bodycam.cpp:545-552,
//   CBodyDouble::DrawBody) is:
//     mask->Draw(...,    MERGEPAINT);   // dest = (NOT mask)   OR dest
//     drawing->Draw(..., SRCAND);       // dest = drawing      AND dest
//   Working the GDI ROP algebra through both mask-bit cases:
//     mask bit = 1 (black, RGB 0,0,0):
//       MERGEPAINT: dest = NOT(black) OR dest = white OR dest = white
//       SRCAND:     dest = drawing AND white = drawing   -> avatar pixel
//                   shows through: this is the OPAQUE region.
//     mask bit = 0 (white, RGB 255,255,255):
//       MERGEPAINT: dest = NOT(white) OR dest = black OR dest = dest
//                   (no-op, background untouched)
//       SRCAND:     dest = drawing AND dest; drawing is forced white here
//                   by the mask&=image fixup above only in the *aura*
//                   3-plane case, but for the mask plane itself the
//                   region is simply never touched by MERGEPAINT, so the
//                   background shows through unmodified: this is the
//                   TRANSPARENT region.
//   Conclusion: mask bit = 1 (black/index 1) => OPAQUE (alpha 255);
//               mask bit = 0 (white/index 0) => TRANSPARENT (alpha 0).
//   This is the standard Win32/MFC "AND-mask then SRCAND sprite" idiom
//   (white = transparent, black = opaque), and it is this exact polarity
//   that is reused below, unchanged, per-pixel, against CPose::GetMask()'s
//   DIB when a pose has one. Poses with no mask DIB (e.g. the icon pose,
//   which CAvatarX::CreatePose() creates via the single-image path with no
//   mask/aura offsets, and which the original UI draws plain via
//   CreateDIBitmap+SRCCOPY at protsupp.cpp:592-605 -- opaque by original
//   design, not by omission here) decode as fully opaque (alpha 255).
//
// Backdrops (CChatBackdrop) have no mask/aura concept at all (backdrop.h has
// only m_pDIB) -- backdrops always decode fully opaque.

namespace {

// Reads one palette-indexed pixel from a DWORD-aligned, bottom-up-or-top-down
// DIB row buffer. bitCount is 1, 4, or 8 (the only depths this dataset's
// CAvatarDIB instances use -- CDIB::Convert{4,8}ToNonRLE only handle these).
inline int readIndexedPixel(const uint8_t* row, int x, int bitCount) {
    switch (bitCount) {
        case 1: {
            int byteIdx = x / 8;
            int bitIdx = 7 - (x % 8);
            return (row[byteIdx] >> bitIdx) & 0x1;
        }
        case 4: {
            int byteIdx = x / 2;
            bool highNibble = (x % 2) == 0;
            return highNibble ? (row[byteIdx] >> 4) & 0xF : row[byteIdx] & 0xF;
        }
        case 8:
        default:
            return row[x];
    }
}

// Decodes an indexed CDIB into freshly malloc'd top-down RGBA8. Runs
// ConvertToNonRLE() first (a no-op if already BI_RGB). `maskDib`, if
// non-NULL, must have the same width/height; its bit (1bpp, same row
// layout) supplies the alpha channel per the transparency rule documented
// above. Returns false (leaving *outRgba untouched) on any decode failure.
// Takes CDIB* (not CAvatarDIB*) so callers with a plain in-memory DIB (no
// avatar file backing it -- e.g. the CDC::StretchDIBits adapter, Plan 2
// Task 3) can reuse this without a CAvatarDIB. CAvatarDIB IS-A CDIB, so
// every existing call site below still passes unchanged.
bool decodeDibToRgba(CDIB* dib, CDIB* maskDib,
                      int32_t* outWidth, int32_t* outHeight, uint8_t** outRgba) {
    if (dib == nullptr) return false;

    dib->ConvertToNonRLE();
    if (maskDib != nullptr) {
        maskDib->ConvertToNonRLE();
    }

    BITMAPINFOHEADER* pHeader = &dib->GetBitmapInfoAddress()->bmiHeader;
    int width = pHeader->biWidth;
    int height = pHeader->biHeight;
    bool bottomUp = height > 0;
    int absHeight = bottomUp ? height : -height;
    int bitCount = (int)pHeader->biBitCount;

    if (width <= 0 || absHeight <= 0) return false;
    if (bitCount != 1 && bitCount != 4 && bitCount != 8) return false;

    const uint8_t* bits = (const uint8_t*)dib->GetBitsAddress();
    if (bits == nullptr) return false;
    int storageWidth = dib->StorageWidth();

    const uint8_t* maskBits = nullptr;
    int maskStorageWidth = 0;
    bool maskBottomUp = true;
    if (maskDib != nullptr) {
        BITMAPINFOHEADER* pMaskHeader = &maskDib->GetBitmapInfoAddress()->bmiHeader;
        if (pMaskHeader->biWidth == width &&
            (pMaskHeader->biHeight == height || -pMaskHeader->biHeight == height)) {
            maskBits = (const uint8_t*)maskDib->GetBitsAddress();
            maskStorageWidth = maskDib->StorageWidth();
            maskBottomUp = pMaskHeader->biHeight > 0;
        }
        // A mismatched mask (shouldn't happen for any real pose) is simply
        // ignored -- the image still decodes, just fully opaque.
    }

    RGBQUAD* clrTab = dib->GetClrTabAddress();
    int numClrEntries = dib->GetNumClrEntries();

    uint8_t* rgba = (uint8_t*)malloc((size_t)width * (size_t)absHeight * 4);
    if (rgba == nullptr) return false;

    for (int y = 0; y < absHeight; y++) {
        // Destination row y is "top of image, moving down". Source storage
        // is bottom-up when biHeight > 0 (dib.cpp's GetPixelAddress:
        // "m_pBits + (DibHeight()-y-1) * iWidth + x"), top-down otherwise.
        int srcRow = bottomUp ? (absHeight - 1 - y) : y;
        const uint8_t* rowPtr = bits + (size_t)srcRow * storageWidth;

        const uint8_t* maskRowPtr = nullptr;
        if (maskBits != nullptr) {
            int maskSrcRow = maskBottomUp ? (absHeight - 1 - y) : y;
            maskRowPtr = maskBits + (size_t)maskSrcRow * maskStorageWidth;
        }

        uint8_t* outRow = rgba + (size_t)y * width * 4;
        for (int x = 0; x < width; x++) {
            int idx = readIndexedPixel(rowPtr, x, bitCount);
            uint8_t r = 0, g = 0, b = 0;
            if (idx >= 0 && idx < numClrEntries) {
                RGBQUAD& q = clrTab[idx];
                r = q.rgbRed; g = q.rgbGreen; b = q.rgbBlue;
            }
            uint8_t alpha = 255;
            if (maskRowPtr != nullptr) {
                int maskBit = readIndexedPixel(maskRowPtr, x, 1);
                alpha = (maskBit == 1) ? 255 : 0; // see transparency rule above
            }
            uint8_t* px = outRow + (size_t)x * 4;
            px[0] = r; px[1] = g; px[2] = b; px[3] = alpha;
        }
    }

    *outWidth = width;
    *outHeight = absHeight;
    *outRgba = rgba;
    return true;
}

} // namespace

// Callable entry point for code outside this translation unit (the CDC
// adapter's StretchDIBits, Plan 2 Task 3): decodes a raw palettized DIB
// (BITMAPINFO* + bits, exactly what Win32's StretchDIBits itself takes) into
// freshly malloc'd top-down RGBA8, with no mask (StretchDIBits callers pass
// exactly one DIB -- no separate mask plane in that API). Builds a transient
// CDIB wrapping the caller's memory (CDIB::Create does not take ownership of
// pBits -- see dib.cpp) and reuses decodeDibToRgba() above. Returns false
// (leaving outputs untouched) on failure.
bool bridge_decode_dib_to_rgba(BITMAPINFO* bmi, void* bits,
                                int32_t* outWidth, int32_t* outHeight,
                                uint8_t** outRgba) {
    if (bmi == nullptr || bits == nullptr) return false;
    CDIB dib;
    if (!dib.Create(bmi, (BYTE*)bits)) return false;
    return decodeDibToRgba(&dib, nullptr, outWidth, outHeight, outRgba);
}

// Plan 2 Task 7 (R14(i)): the CBody draw path (bodycam.cpp DrawBody) composites
// a pose plane's image DIB with its separate mask DIB in a single alpha-aware
// blit -- the RGBA collapse of the original's MERGEPAINT-mask + SRCAND-drawing
// ROP pair (see the transparency-rule block at the top of this file for the
// per-mask-bit ROP algebra; mask bit 1 => opaque, mask bit 0 => transparent).
// This is the SAME decode the pose-image golden path (cc_avatar_pose_image)
// already uses -- decodeDibToRgba(drawing, mask) -- exposed here for CDIB*
// callers (the CDC adapter's DrawPoseImage). `maskBmi`/`maskBits` may be NULL
// (fully-opaque plane: no separate mask, e.g. the aura decoded as its own
// self-opaque sprite, or a mask-less pose plane). Builds transient CDIBs over
// the caller's memory (CDIB::Create copies the header, borrows the bits) and
// reuses decodeDibToRgba(). Returns false (outputs untouched) on failure.
bool bridge_decode_dib_pair_to_rgba(BITMAPINFO* imgBmi, void* imgBits,
                                     BITMAPINFO* maskBmi, void* maskBits,
                                     int32_t* outWidth, int32_t* outHeight,
                                     uint8_t** outRgba) {
    if (imgBmi == nullptr || imgBits == nullptr) return false;
    CDIB image;
    if (!image.Create(imgBmi, (BYTE*)imgBits)) return false;
    CDIB mask;
    CDIB* pMask = nullptr;
    if (maskBmi != nullptr && maskBits != nullptr) {
        if (!mask.Create(maskBmi, (BYTE*)maskBits)) return false;
        pMask = &mask;
    }
    return decodeDibToRgba(&image, pMask, outWidth, outHeight, outRgba);
}

extern "C" void cc_image_free(cc_image* img) {
    if (img == nullptr) return;
    if (img->rgba != nullptr) {
        free(img->rgba);
        img->rgba = nullptr;
    }
    img->width = 0;
    img->height = 0;
}

// ============================================================================
// cc_avatar

struct cc_avatar {
    CAvatarX* avatar = nullptr;
    // Poses, indexed as this API exposes them: every pose in m_arrPoses
    // EXCEPT the icon pose (poseID == avatar->m_icon). The icon is loaded
    // and drawn completely differently by the original code (a plain opaque
    // CreateDIBitmap+SRCCOPY blit for a member list, protsupp.cpp:592-605 --
    // never composited with a mask, never iterated as part of "the avatar's
    // poses" anywhere in the original source) so it is not a pose in the
    // sense this API means (a displayable gesture/expression with the usual
    // image+mask+aura triple). See task-6-report.md's pose-model notes.
    std::vector<int> poseIndices; // indices into avatar->m_arrPoses
    std::vector<std::string> poseNames; // synthesized; see pose-model notes
};

extern "C" cc_avatar* cc_avatar_open(const char* path) {
    if (path == nullptr) return nullptr;
    try {
        CAvatarFileStream* pStream = new CAvatarFileStream(path);
        CAvatarX* pAvatar = CAvatarX::LoadAvatar(pStream);
        if (pAvatar == nullptr) {
            delete pStream;
            return nullptr;
        }
        pAvatar->SetStream(pStream); // avatar now owns the stream

        cc_avatar* av = new cc_avatar();
        av->avatar = pAvatar;
        int count = pAvatar->GetPoseCount();
        for (int i = 0; i < count; i++) {
            // CreatePoseWithMask()/CreatePose() return "array position + 1"
            // as the poseID (avbfile.cpp), so m_arrPoses[i]'s poseID is i+1.
            if ((i + 1) == pAvatar->m_icon) continue;
            av->poseIndices.push_back(i);
            av->poseNames.push_back("pose " + std::to_string(av->poseIndices.size() - 1));
        }
        return av;
    } catch (...) {
        return nullptr;
    }
}

extern "C" void cc_avatar_close(cc_avatar* av) {
    if (av == nullptr) return;
    try {
        delete av->avatar; // cascades: stream, poses, and their DIBs
    } catch (...) {
        // never throw across the bridge
    }
    delete av;
}

extern "C" const char* cc_avatar_name(const cc_avatar* av) {
    if (av == nullptr || av->avatar == nullptr || av->avatar->m_name == nullptr) return "";
    return av->avatar->m_name;
}

extern "C" int32_t cc_avatar_pose_count(const cc_avatar* av) {
    if (av == nullptr) return 0;
    return (int32_t)av->poseIndices.size();
}

extern "C" const char* cc_avatar_pose_name(const cc_avatar* av, int32_t idx) {
    if (av == nullptr || idx < 0 || (size_t)idx >= av->poseNames.size()) return "";
    return av->poseNames[(size_t)idx].c_str();
}

extern "C" int32_t cc_avatar_pose_image(const cc_avatar* av, int32_t idx, cc_image* out) {
    if (out == nullptr) return 1;
    out->width = 0; out->height = 0; out->rgba = nullptr;
    if (av == nullptr || av->avatar == nullptr) return 1;
    if (idx < 0 || (size_t)idx >= av->poseIndices.size()) return 2;

    try {
        CPose* pose = av->avatar->m_arrPoses[av->poseIndices[(size_t)idx]];
        if (pose == nullptr) return 3;
        if (!pose->Load(av->avatar->m_pStream, &av->avatar->m_palette)) return 4;

        CAvatarDIB* drawing = pose->GetDrawing();
        CAvatarDIB* mask = pose->GetMask();
        int32_t w = 0, h = 0;
        uint8_t* rgba = nullptr;
        if (!decodeDibToRgba(drawing, mask, &w, &h, &rgba)) return 5;

        out->width = w; out->height = h; out->rgba = rgba;
        return 0;
    } catch (...) {
        return 6;
    }
}

// ============================================================================
// cc_backdrop

struct cc_backdrop {
    CChatBackdrop* backdrop = nullptr;
};

extern "C" cc_backdrop* cc_backdrop_open(const char* path) {
    if (path == nullptr) return nullptr;
    try {
        CAvatarFileStream* pStream = new CAvatarFileStream(path);
        CChatBackdrop* pBackdrop = CChatBackdrop::LoadBackdrop(pStream);
        delete pStream; // LoadBackdrop does not take ownership of the stream
        if (pBackdrop == nullptr) return nullptr;

        cc_backdrop* bd = new cc_backdrop();
        bd->backdrop = pBackdrop;
        return bd;
    } catch (...) {
        return nullptr;
    }
}

extern "C" void cc_backdrop_close(cc_backdrop* bd) {
    if (bd == nullptr) return;
    try {
        delete bd->backdrop; // cascades: m_pDIB, URL/copyright strings
    } catch (...) {
        // never throw across the bridge
    }
    delete bd;
}

extern "C" const char* cc_backdrop_name(const cc_backdrop* bd) {
    if (bd == nullptr || bd->backdrop == nullptr) return "";
    const char* url = bd->backdrop->Url();
    return url != nullptr ? url : "";
}

extern "C" int32_t cc_backdrop_image(const cc_backdrop* bd, cc_image* out) {
    if (out == nullptr) return 1;
    out->width = 0; out->height = 0; out->rgba = nullptr;
    if (bd == nullptr || bd->backdrop == nullptr) return 1;

    try {
        CAvatarDIB* drawing = bd->backdrop->GetDrawing();
        int32_t w = 0, h = 0;
        uint8_t* rgba = nullptr;
        // Backdrops have no mask/aura concept (backdrop.h: CChatBackdrop
        // only ever holds m_pDIB) -- always fully opaque.
        if (!decodeDibToRgba(drawing, nullptr, &w, &h, &rgba)) return 2;

        out->width = w; out->height = h; out->rgba = rgba;
        return 0;
    } catch (...) {
        return 3;
    }
}
