#include "mfc_compat.h"
#include <sys/stat.h>

void ccLog(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

// R9: minimal GetFileAttributes() — used by avatario.cpp only to test
// existence of a file before opening it.
DWORD GetFileAttributes(LPCSTR pszPath) {
    struct stat st;
    if (pszPath == nullptr || stat(pszPath, &st) != 0) {
        return INVALID_FILE_ATTRIBUTES;
    }
    return 0;
}
