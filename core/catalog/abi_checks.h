// C-VER-003 struct_size rules shared by catalog, authority and Kernel. While
// the ABI major is 0 every struct has the one layout of this minor, so each
// caller passes its sizeof as both sizes (ADR-0053).

#ifndef AYNI_CORE_CATALOG_ABI_CHECKS_H
#define AYNI_CORE_CATALOG_ABI_CHECKS_H

#include <ayni/ayni.h>

#include <cstddef>
#include <cstdint>

namespace ayni::catalog {

// AYNI_INVALID_ARGUMENT below min_size, AYNI_UNSUPPORTED_VERSION beyond the
// layout this library knows (C-VER-004). From major 1 on, min_size is the
// first layout of the major and shorter callers get defaults (C-VER-006).
inline ayni_status_t check_struct_size(uint32_t struct_size, std::size_t min_size,
                                       std::size_t known_size) {
    if (struct_size < min_size) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (struct_size > known_size) {
        return AYNI_UNSUPPORTED_VERSION;
    }
    return AYNI_OK;
}

} // namespace ayni::catalog

#endif // AYNI_CORE_CATALOG_ABI_CHECKS_H
