// struct_size rules shared by the C ABI entry points (C-VER-003): a field is
// read only when struct_size covers it, and a struct_size outside the known
// layouts is refused.

#ifndef AYNI_CORE_KERNEL_ABI_CHECKS_H
#define AYNI_CORE_KERNEL_ABI_CHECKS_H

#include <ayni/ayni.h>

#include <cstddef>
#include <cstdint>

namespace ayni::kernel {

inline bool covers(uint32_t struct_size, std::size_t field_offset, std::size_t field_size) {
    return struct_size >= field_offset + field_size;
}

// AYNI_INVALID_ARGUMENT below the first published layout of the ABI major,
// AYNI_UNSUPPORTED_VERSION beyond the layout this library knows. A struct new
// in this minor has one layout, so both sizes are its sizeof.
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

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_ABI_CHECKS_H
