#include "comicchat.h"
#include "mfc_compat.h"
#include "bbox.h"
#include "pe.h"
#include "dib.h"
#include "avbfile.h"
#include "avatar.h"
#include "avatario.h"
#include <cstring>

extern "C" int32_t cc_engine_version(void) {
    return 1;
}

// Temporary smoke hook (Task 4) — mirrors avatario.cpp's LoadAvatarInfo()
// minus the dir+name path formatting, since this hook receives a full path.
extern "C" int32_t cc_smoke_load_avatar(const char* path, char* name_out, size_t name_cap,
                                        int32_t* pose_count_out) {
    if (path == nullptr || name_out == nullptr || name_cap == 0 || pose_count_out == nullptr) {
        return 1;
    }

    CAvatarFileStream* pStream = new CAvatarFileStream(path);
    CAvatarX* pAvatar = CAvatarX::LoadAvatar(pStream);
    if (pAvatar == nullptr) {
        delete pStream;
        return 2;
    }
    pAvatar->SetStream(pStream);  // avatar now owns the stream (matches LoadAvatarInfo)

    const char* name = pAvatar->m_name ? pAvatar->m_name : "";
    strncpy(name_out, name, name_cap - 1);
    name_out[name_cap - 1] = '\0';
    *pose_count_out = (int32_t)pAvatar->GetPoseCount();

    delete pAvatar;
    return 0;
}
