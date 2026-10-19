// C ABI entry points implemented in M0: version query, memory requirements,
// create, lifecycle query and destroy. Everything else in the planned API
// (include/ayni/draft/) has no symbol yet.

#include "kernel/kernel_state.h"

#include <ayni/ayni.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

namespace {

// Smallest struct_size accepted for each ABI struct: the end of the last field
// in the first published layout of ABI major 0. Later fields are appended
// beyond it and read only when struct_size covers them.
constexpr std::size_t config_v1_min_size =
    offsetof(ayni_config_v1, abi_major) + sizeof(ayni_config_v1::abi_major);
// Callers whose config ends before abi_minor were built against ABI 0.1.
constexpr std::size_t config_v1_minor_end =
    offsetof(ayni_config_v1, abi_minor) + sizeof(ayni_config_v1::abi_minor);
constexpr uint32_t first_minor_of_major_0 = 1;
constexpr std::size_t host_v1_min_size =
    offsetof(ayni_host_v1, monotonic_now_us) + sizeof(ayni_host_v1::monotonic_now_us);

ayni_status_t check_struct_size(uint32_t struct_size, std::size_t min_size,
                                std::size_t known_size) {
    if (struct_size < min_size) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (struct_size > known_size) {
        return AYNI_UNSUPPORTED_VERSION;
    }
    return AYNI_OK;
}

ayni_status_t validate_config(const ayni_config_v1 *config) {
    if (config == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const ayni_status_t status =
        check_struct_size(config->struct_size, config_v1_min_size, sizeof(ayni_config_v1));
    if (status != AYNI_OK) {
        return status;
    }
    const uint32_t caller_minor =
        config->struct_size >= config_v1_minor_end ? config->abi_minor : first_minor_of_major_0;
    // Major 0 is experimental (C-VER-006): any minor may change published
    // semantics, so only the exact version this library implements is safe.
    if (config->abi_major != AYNI_ABI_MAJOR || caller_minor != AYNI_ABI_MINOR) {
        return AYNI_UNSUPPORTED_VERSION;
    }
    return AYNI_OK;
}

ayni_status_t validate_host(const ayni_host_v1 *host) {
    if (host == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const ayni_status_t status =
        check_struct_size(host->struct_size, host_v1_min_size, sizeof(ayni_host_v1));
    if (status != AYNI_OK) {
        return status;
    }
    if (host->monotonic_now_us == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    return AYNI_OK;
}

// Copies the part of the caller's table that its struct_size covers; the
// rest keeps the defaults of a zero-initialised table.
ayni_host_v1 copy_host(const ayni_host_v1 *host) {
    ayni_host_v1 copy{};
    std::memcpy(&copy, host, host->struct_size);
    copy.struct_size = sizeof(ayni_host_v1);
    return copy;
}

bool is_aligned(const void *memory, std::size_t alignment) {
    return reinterpret_cast<std::uintptr_t>(memory) % alignment == 0U;
}

} // namespace

extern "C" {

ayni_status_t ayni_get_abi_version(uint32_t *out_major, uint32_t *out_minor) {
    if (out_major == nullptr || out_minor == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    *out_major = AYNI_ABI_MAJOR;
    *out_minor = AYNI_ABI_MINOR;
    return AYNI_OK;
}

ayni_status_t ayni_kernel_memory_requirements(const ayni_config_v1 *config, size_t *out_size,
                                              size_t *out_align) {
    if (out_size == nullptr || out_align == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const ayni_status_t status = validate_config(config);
    if (status != AYNI_OK) {
        return status;
    }
    *out_size = sizeof(ayni_kernel);
    *out_align = alignof(ayni_kernel);
    return AYNI_OK;
}

ayni_status_t ayni_create(const ayni_config_v1 *config, const ayni_host_v1 *host, void *memory,
                          size_t memory_size, ayni_kernel_t **out_kernel) {
    if (out_kernel == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    *out_kernel = nullptr;

    ayni_status_t status = validate_config(config);
    if (status != AYNI_OK) {
        return status;
    }
    status = validate_host(host);
    if (status != AYNI_OK) {
        return status;
    }
    if (memory == nullptr || !is_aligned(memory, alignof(ayni_kernel))) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (memory_size < sizeof(ayni_kernel)) {
        return AYNI_BUFFER_TOO_SMALL;
    }

    *out_kernel = new (memory) ayni_kernel{copy_host(host), AYNI_LIFECYCLE_CONFIGURING};
    return AYNI_OK;
}

ayni_status_t ayni_get_lifecycle(const ayni_kernel_t *kernel, ayni_lifecycle_t *out_state) {
    if (kernel == nullptr || out_state == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    *out_state = kernel->lifecycle;
    return AYNI_OK;
}

ayni_status_t ayni_destroy(ayni_kernel_t *kernel) {
    if (kernel == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (kernel->lifecycle != AYNI_LIFECYCLE_CONFIGURING &&
        kernel->lifecycle != AYNI_LIFECYCLE_STOPPED) {
        return AYNI_INVALID_STATE;
    }
    kernel->~ayni_kernel();
    return AYNI_OK;
}

} // extern "C"
