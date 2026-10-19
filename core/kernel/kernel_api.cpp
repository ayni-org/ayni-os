// C ABI entry points implemented so far: version query, memory requirements,
// create, lifecycle and kernel info queries, start, step, shutdown, destroy
// and completion posting. They check arguments and struct sizes; lifecycle
// semantics live in kernel_lifecycle.cpp. Everything else in the planned API
// (include/ayni/draft/) has no symbol yet.

#include "kernel/kernel_layout.h"
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
// New in 0.3 and 0.4, so their first layout is the whole struct.
constexpr std::size_t completion_result_v1_min_size = sizeof(ayni_completion_result_v1);
constexpr std::size_t kernel_info_v1_min_size = sizeof(ayni_kernel_info_v1);
constexpr std::size_t step_report_v1_min_size = sizeof(ayni_step_report_v1);

using ayni::kernel::KernelCapacities;
using ayni::kernel::KernelLayout;

bool covers(uint32_t struct_size, std::size_t field_offset, std::size_t field_size) {
    return struct_size >= field_offset + field_size;
}

struct CapacityRange {
    uint32_t default_value;
    uint32_t min_value;
    uint32_t max_value;
};

// A capacity field that struct_size covers must lie in [min, max]; an
// uncovered one takes the documented default and is never read (C-VER-003).
ayni_status_t read_capacity(const ayni_config_v1 *config, std::size_t field_offset,
                            const CapacityRange &range, uint32_t *out) {
    if (!covers(config->struct_size, field_offset, sizeof(uint32_t))) {
        *out = range.default_value;
        return AYNI_OK;
    }
    uint32_t value = 0;
    std::memcpy(&value, reinterpret_cast<const unsigned char *>(config) + field_offset,
                sizeof(value));
    if (value < range.min_value || value > range.max_value) {
        return AYNI_INVALID_ARGUMENT;
    }
    *out = value;
    return AYNI_OK;
}

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

ayni_status_t validate_config(const ayni_config_v1 *config, KernelCapacities *out_capacities) {
    if (config == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    ayni_status_t status =
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
    KernelCapacities capacities{};
    status = read_capacity(config, offsetof(ayni_config_v1, ingress_capacity),
                           {AYNI_INGRESS_CAPACITY_DEFAULT, 1U, AYNI_INGRESS_CAPACITY_MAX},
                           &capacities.ingress_capacity);
    if (status == AYNI_OK) {
        status =
            read_capacity(config, offsetof(ayni_config_v1, event_body_max_bytes),
                          {AYNI_EVENT_BODY_MAX_BYTES_DEFAULT, 1U, AYNI_EVENT_BODY_MAX_BYTES_MAX},
                          &capacities.event_body_max_bytes);
    }
    if (status == AYNI_OK) {
        status =
            read_capacity(config, offsetof(ayni_config_v1, completion_slot_count),
                          {AYNI_COMPLETION_SLOT_COUNT_DEFAULT, 1U, AYNI_COMPLETION_SLOT_COUNT_MAX},
                          &capacities.completion_slot_count);
    }
    if (status == AYNI_OK) {
        status = read_capacity(config, offsetof(ayni_config_v1, storage_buffer_bytes),
                               {AYNI_STORAGE_BUFFER_BYTES_DEFAULT, AYNI_STORAGE_BUFFER_BYTES_MIN,
                                AYNI_STORAGE_BUFFER_BYTES_MAX},
                               &capacities.storage_buffer_bytes);
    }
    if (status == AYNI_OK) {
        *out_capacities = capacities;
    }
    return status;
}

// Copies each field that the caller's struct_size covers completely; the
// caller has already checked that the first layout is covered. Other fields
// keep their zero default, so a struct_size ending inside a field never
// yields part of a pointer.
ayni_host_v1 copy_host(const ayni_host_v1 *host) {
    ayni_host_v1 copy{};
    copy.struct_size = sizeof(ayni_host_v1);
    copy.ctx = host->ctx;
    copy.monotonic_now_us = host->monotonic_now_us;
    if (covers(host->struct_size, offsetof(ayni_host_v1, wake), sizeof(host->wake))) {
        copy.wake = host->wake;
    }
    if (covers(host->struct_size, offsetof(ayni_host_v1, ingress_lock),
               sizeof(host->ingress_lock))) {
        copy.ingress_lock = host->ingress_lock;
    }
    if (covers(host->struct_size, offsetof(ayni_host_v1, ingress_unlock),
               sizeof(host->ingress_unlock))) {
        copy.ingress_unlock = host->ingress_unlock;
    }
    if (covers(host->struct_size, offsetof(ayni_host_v1, storage_append),
               sizeof(host->storage_append))) {
        copy.storage_append = host->storage_append;
    }
    if (covers(host->struct_size, offsetof(ayni_host_v1, storage_read_prefix),
               sizeof(host->storage_read_prefix))) {
        copy.storage_read_prefix = host->storage_read_prefix;
    }
    return copy;
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
    // Copying first gives every port an older caller lacks its zero default,
    // so a required port missing from a short table is caught here too.
    const ayni_host_v1 copy = copy_host(host);
    if (copy.monotonic_now_us == nullptr || copy.wake == nullptr || copy.ingress_lock == nullptr ||
        copy.ingress_unlock == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    // Storage is optional, but a log that can be written and not read back,
    // or the reverse, is a broken table (ADR-0016).
    if ((copy.storage_append == nullptr) != (copy.storage_read_prefix == nullptr)) {
        return AYNI_INVALID_ARGUMENT;
    }
    return AYNI_OK;
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
    KernelCapacities capacities{};
    const ayni_status_t status = validate_config(config, &capacities);
    if (status != AYNI_OK) {
        return status;
    }
    const KernelLayout layout = ayni::kernel::layout_for(capacities);
    *out_size = layout.size;
    *out_align = layout.alignment;
    return AYNI_OK;
}

ayni_status_t ayni_create(const ayni_config_v1 *config, const ayni_host_v1 *host, void *memory,
                          size_t memory_size, ayni_kernel_t **out_kernel) {
    if (out_kernel == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    *out_kernel = nullptr;

    KernelCapacities capacities{};
    ayni_status_t status = validate_config(config, &capacities);
    if (status != AYNI_OK) {
        return status;
    }
    status = validate_host(host);
    if (status != AYNI_OK) {
        return status;
    }
    const KernelLayout layout = ayni::kernel::layout_for(capacities);
    if (memory == nullptr || !is_aligned(memory, layout.alignment)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (memory_size < layout.size) {
        return AYNI_BUFFER_TOO_SMALL;
    }

    uint8_t *base = static_cast<uint8_t *>(memory);
    const ayni::kernel::KernelStorage storage{base + layout.ingress_offset,
                                              base + layout.completions_offset,
                                              base + layout.storage_buffer_offset};
    *out_kernel = new (memory) ayni_kernel(copy_host(host), capacities, storage);
    return AYNI_OK;
}

ayni_status_t ayni_get_lifecycle(const ayni_kernel_t *kernel, ayni_lifecycle_t *out_state) {
    if (kernel == nullptr || out_state == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    *out_state = kernel->lifecycle;
    return AYNI_OK;
}

ayni_status_t ayni_get_kernel_info(const ayni_kernel_t *kernel, ayni_kernel_info_v1 *out_info) {
    if (kernel == nullptr || out_info == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const ayni_status_t status =
        check_struct_size(out_info->struct_size, kernel_info_v1_min_size, sizeof(*out_info));
    if (status != AYNI_OK) {
        return status;
    }
    ayni_kernel_info_v1 info{};
    info.struct_size = out_info->struct_size;
    info.lifecycle = kernel->lifecycle;
    info.restriction = kernel->restriction();
    info.boot_epoch = kernel->boot.boot_epoch();
    std::memcpy(out_info, &info, out_info->struct_size);
    return AYNI_OK;
}

ayni_status_t ayni_start(ayni_kernel_t *kernel) {
    if (kernel == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    return kernel->start();
}

ayni_status_t ayni_step(ayni_kernel_t *kernel, uint32_t max_items, uint32_t max_time_us,
                        ayni_step_report_v1 *out_report) {
    if (kernel == nullptr || max_items == 0U || max_time_us == 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (out_report != nullptr) {
        const ayni_status_t status = check_struct_size(
            out_report->struct_size, step_report_v1_min_size, sizeof(*out_report));
        if (status != AYNI_OK) {
            return status;
        }
    }
    ayni_step_report_v1 report{};
    const ayni_status_t status = kernel->step(max_items, max_time_us, report);
    if (status == AYNI_OK && out_report != nullptr) {
        report.struct_size = out_report->struct_size;
        std::memcpy(out_report, &report, out_report->struct_size);
    }
    return status;
}

ayni_status_t ayni_begin_shutdown(ayni_kernel_t *kernel) {
    if (kernel == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    return kernel->begin_shutdown();
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

ayni_status_t ayni_completion_post(ayni_completion_t *completion,
                                   const ayni_completion_result_v1 *result) {
    if (completion == nullptr || result == nullptr || completion->pool == nullptr ||
        !completion->pool->owns(completion)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const ayni_status_t status = check_struct_size(
        result->struct_size, completion_result_v1_min_size, sizeof(ayni_completion_result_v1));
    if (status != AYNI_OK) {
        return status;
    }
    ayni_completion_result_v1 copy{};
    std::memcpy(&copy, result, result->struct_size);
    copy.struct_size = sizeof(ayni_completion_result_v1);
    return completion->pool->post(*completion, copy);
}

} // extern "C"
