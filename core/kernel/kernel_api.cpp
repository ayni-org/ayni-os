// C ABI entry points of the kernel itself: version query, memory
// requirements, create, lifecycle and kernel info queries, start, step,
// shutdown, destroy, completion and event posting. They check arguments and
// struct sizes; lifecycle semantics live in kernel_lifecycle.cpp. The work
// model's entry points are in work_api.cpp. Everything else in the planned
// API (include/ayni/draft/) has no symbol yet.

#include "catalog/abi_checks.h"
#include "catalog/catalog_id.h"
#include "kernel/kernel_layout.h"
#include "kernel/kernel_state.h"
#include "kernel/storage/storage_budget.h"
#include "kernel/work_journal.h"

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
// New in 0.3 and 0.4, so their first layout is the whole 0.4 struct.
constexpr std::size_t completion_result_v1_min_size =
    offsetof(ayni_completion_result_v1, next_cursor);
constexpr std::size_t kernel_info_v1_min_size =
    offsetof(ayni_kernel_info_v1, boot_epoch) + sizeof(ayni_kernel_info_v1::boot_epoch);
// Where each field after the first layout ends: durable_seq since 0.5, the
// event counters since 0.9.
constexpr std::size_t kernel_info_v1_field_ends[] = {
    offsetof(ayni_kernel_info_v1, durable_seq) + sizeof(ayni_kernel_info_v1::durable_seq),
    offsetof(ayni_kernel_info_v1, events_unmatched) + sizeof(ayni_kernel_info_v1::events_unmatched),
    offsetof(ayni_kernel_info_v1, events_discarded) + sizeof(ayni_kernel_info_v1::events_discarded),
};
constexpr std::size_t step_report_v1_min_size =
    offsetof(ayni_step_report_v1, next_deadline_us) + sizeof(ayni_step_report_v1::next_deadline_us);
// durable_seq since 0.14.
constexpr std::size_t step_report_v1_durable_end =
    offsetof(ayni_step_report_v1, durable_seq) + sizeof(ayni_step_report_v1::durable_seq);

using ayni::catalog::check_struct_size;
using ayni::catalog::covers;
using ayni::kernel::KernelCapacities;
using ayni::kernel::KernelLayout;
using ayni::kernel::KernelLimits;

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

// The storage buffer holds the largest batch a transition writes (ADR-0026);
// 0 takes three times that, avoiding fragmentation with one batch in flight.
ayni_status_t resolve_storage_buffer(KernelCapacities &capacities) {
    const uint32_t journal_needed = ayni::kernel::storage_buffer_min(capacities);
    const uint32_t snapshot_needed = ayni::kernel::snapshot_record_max(capacities);
    const uint32_t needed = journal_needed > snapshot_needed ? journal_needed : snapshot_needed;
    static_assert(AYNI_STORAGE_BUFFER_BYTES_DEFAULT == 0U, "0 is the default");
    if (capacities.storage_buffer_bytes == 0U) {
        capacities.storage_buffer_bytes =
            3U * (needed > AYNI_STORAGE_BUFFER_BYTES_MIN ? needed : AYNI_STORAGE_BUFFER_BYTES_MIN);
        return AYNI_OK;
    }
    return capacities.storage_buffer_bytes < AYNI_STORAGE_BUFFER_BYTES_MIN ||
                   capacities.storage_buffer_bytes < needed
               ? AYNI_INVALID_ARGUMENT
               : AYNI_OK;
}

ayni_status_t validate_config(const ayni_config_v1 *config, KernelCapacities *out_capacities,
                              KernelLimits *out_limits) {
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
        // 0, the default, is resolved once the other capacities are known.
        status =
            read_capacity(config, offsetof(ayni_config_v1, storage_buffer_bytes),
                          {AYNI_STORAGE_BUFFER_BYTES_DEFAULT, 0U, AYNI_STORAGE_BUFFER_BYTES_MAX},
                          &capacities.storage_buffer_bytes);
    }
    if (status == AYNI_OK) {
        status = read_capacity(config, offsetof(ayni_config_v1, agent_capacity),
                               {AYNI_AGENT_CAPACITY_DEFAULT, 1U, AYNI_AGENT_CAPACITY_MAX},
                               &capacities.agent_capacity);
    }
    if (status == AYNI_OK) {
        status = read_capacity(config, offsetof(ayni_config_v1, goal_type_capacity),
                               {AYNI_GOAL_TYPE_CAPACITY_DEFAULT, 1U, AYNI_GOAL_TYPE_CAPACITY_MAX},
                               &capacities.goal_type_capacity);
    }
    if (status == AYNI_OK) {
        status = read_capacity(config, offsetof(ayni_config_v1, goal_capacity),
                               {AYNI_GOAL_CAPACITY_DEFAULT, 1U, AYNI_GOAL_CAPACITY_MAX},
                               &capacities.goal_capacity);
    }
    if (status == AYNI_OK) {
        status =
            read_capacity(config, offsetof(ayni_config_v1, goal_parameter_max_bytes),
                          {AYNI_GOAL_PARAMETER_MAX_BYTES_DEFAULT, AYNI_GOAL_PARAMETER_MAX_BYTES_MIN,
                           AYNI_GOAL_PARAMETER_MAX_BYTES_MAX},
                          &capacities.goal_parameter_max_bytes);
    }
    if (status == AYNI_OK) {
        status = read_capacity(config, offsetof(ayni_config_v1, session_capacity),
                               {AYNI_SESSION_CAPACITY_DEFAULT, 1U, AYNI_SESSION_CAPACITY_MAX},
                               &capacities.session_capacity);
    }
    if (status == AYNI_OK) {
        status = read_capacity(config, offsetof(ayni_config_v1, task_capacity),
                               {AYNI_TASK_CAPACITY_DEFAULT, 1U, AYNI_TASK_CAPACITY_MAX},
                               &capacities.task_capacity);
    }
    if (status == AYNI_OK) {
        status = read_capacity(config, offsetof(ayni_config_v1, strategy_capacity),
                               {AYNI_STRATEGY_CAPACITY_DEFAULT, 1U, AYNI_STRATEGY_CAPACITY_MAX},
                               &capacities.strategy_capacity);
    }
    if (status == AYNI_OK) {
        status = read_capacity(
            config, offsetof(ayni_config_v1, policy_intent_capacity),
            {AYNI_POLICY_INTENT_CAPACITY_DEFAULT, 1U, AYNI_POLICY_INTENT_CAPACITY_MAX},
            &capacities.policy_intent_capacity);
    }
    if (status == AYNI_OK) {
        status = read_capacity(config, offsetof(ayni_config_v1, task_step_capacity),
                               {AYNI_TASK_STEP_CAPACITY_DEFAULT, 1U, AYNI_TASK_STEP_CAPACITY_MAX},
                               &capacities.task_step_capacity);
    }
    if (status == AYNI_OK) {
        status =
            read_capacity(config, offsetof(ayni_config_v1, task_checkpoint_max_bytes),
                          {AYNI_TASK_CHECKPOINT_MAX_BYTES_DEFAULT,
                           AYNI_TASK_CHECKPOINT_MAX_BYTES_MIN, AYNI_TASK_CHECKPOINT_MAX_BYTES_MAX},
                          &capacities.task_checkpoint_max_bytes);
    }
    if (status == AYNI_OK) {
        status = read_capacity(config, offsetof(ayni_config_v1, command_capacity),
                               {AYNI_COMMAND_CAPACITY_DEFAULT, 1U, AYNI_COMMAND_CAPACITY_MAX},
                               &capacities.command_capacity);
    }
    if (status == AYNI_OK) {
        status = read_capacity(config, offsetof(ayni_config_v1, capability_capacity),
                               {AYNI_CAPABILITY_CAPACITY_DEFAULT, 1U, AYNI_CAPABILITY_CAPACITY_MAX},
                               &capacities.capability_capacity);
    }
    if (status == AYNI_OK) {
        status =
            read_capacity(config, offsetof(ayni_config_v1, authority_scope_max_bytes),
                          {AYNI_AUTHORITY_SCOPE_MAX_BYTES_DEFAULT,
                           AYNI_AUTHORITY_SCOPE_MAX_BYTES_MIN, AYNI_AUTHORITY_SCOPE_MAX_BYTES_MAX},
                          &capacities.authority_scope_max_bytes);
    }
    if (status == AYNI_OK) {
        status = read_capacity(config, offsetof(ayni_config_v1, grant_capacity),
                               {AYNI_GRANT_CAPACITY_DEFAULT, 1U, AYNI_GRANT_CAPACITY_MAX},
                               &capacities.grant_capacity);
    }
    if (status == AYNI_OK) {
        status = read_capacity(config, offsetof(ayni_config_v1, grant_delegation_depth_max),
                               {AYNI_GRANT_DELEGATION_DEPTH_MAX_DEFAULT,
                                AYNI_GRANT_DELEGATION_DEPTH_MAX_MIN,
                                AYNI_GRANT_DELEGATION_DEPTH_MAX_MAX},
                               &capacities.grant_delegation_depth_max);
    }
    if (status == AYNI_OK) {
        status = resolve_storage_buffer(capacities);
    }
    KernelLimits limits{};
    if (status == AYNI_OK) {
        status = read_capacity(
            config, offsetof(ayni_config_v1, goal_depth_max),
            {AYNI_GOAL_DEPTH_MAX_DEFAULT, AYNI_GOAL_DEPTH_MAX_MIN, AYNI_GOAL_DEPTH_MAX_MAX},
            &limits.goal_depth_max);
    }
    if (status == AYNI_OK) {
        status = read_capacity(config, offsetof(ayni_config_v1, goal_children_max),
                               {AYNI_GOAL_CHILDREN_MAX_DEFAULT, AYNI_GOAL_CHILDREN_MAX_MIN,
                                AYNI_GOAL_CHILDREN_MAX_MAX},
                               &limits.goal_children_max);
    }
    if (status == AYNI_OK) {
        status = read_capacity(
            config, offsetof(ayni_config_v1, storage_write_interval_us),
            {AYNI_STORAGE_WRITE_INTERVAL_US_DEFAULT, 0U, AYNI_STORAGE_WRITE_INTERVAL_US_MAX},
            &limits.storage_write_interval_us);
        if (limits.storage_write_interval_us == 0U) {
            limits.storage_write_interval_us = AYNI_STORAGE_WRITE_INTERVAL_US_DEFAULT;
        }
    }
    if (status == AYNI_OK) {
        status =
            read_capacity(config, offsetof(ayni_config_v1, storage_log_max_bytes),
                          {AYNI_STORAGE_LOG_MAX_BYTES_DEFAULT, 0U, AYNI_STORAGE_LOG_MAX_BYTES_MAX},
                          &limits.storage_log_max_bytes);
        // A smaller limit would leave no room to compact the log
        // (StorageBudget).
        if (limits.storage_log_max_bytes == 0U) {
            limits.storage_log_max_bytes =
                ayni::kernel::StorageBudget::log_max_default(capacities.storage_buffer_bytes);
        } else if (limits.storage_log_max_bytes <
                   ayni::kernel::StorageBudget::log_max_min(capacities)) {
            status = AYNI_INVALID_ARGUMENT;
        }
    }
    if (status == AYNI_OK) {
        *out_capacities = capacities;
        *out_limits = limits;
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
    if (covers(host->struct_size, offsetof(ayni_host_v1, storage_read_log),
               sizeof(host->storage_read_log))) {
        copy.storage_read_log = host->storage_read_log;
    }
    if (covers(host->struct_size, offsetof(ayni_host_v1, storage_fence_writers),
               sizeof(host->storage_fence_writers))) {
        copy.storage_fence_writers = host->storage_fence_writers;
    }
    if (covers(host->struct_size, offsetof(ayni_host_v1, storage_write_snapshot),
               sizeof(host->storage_write_snapshot))) {
        copy.storage_write_snapshot = host->storage_write_snapshot;
    }
    if (covers(host->struct_size, offsetof(ayni_host_v1, storage_read_snapshot),
               sizeof(host->storage_read_snapshot))) {
        copy.storage_read_snapshot = host->storage_read_snapshot;
    }
    if (covers(host->struct_size, offsetof(ayni_host_v1, storage_trim_prefix),
               sizeof(host->storage_trim_prefix))) {
        copy.storage_trim_prefix = host->storage_trim_prefix;
    }
    if (covers(host->struct_size, offsetof(ayni_host_v1, storage_free_bytes),
               sizeof(host->storage_free_bytes))) {
        copy.storage_free_bytes = host->storage_free_bytes;
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
    const bool some_maintenance =
        copy.storage_read_log != nullptr || copy.storage_fence_writers != nullptr ||
        copy.storage_write_snapshot != nullptr || copy.storage_read_snapshot != nullptr ||
        copy.storage_trim_prefix != nullptr || copy.storage_free_bytes != nullptr;
    if (some_maintenance &&
        (copy.storage_append == nullptr || copy.storage_read_log == nullptr ||
         copy.storage_fence_writers == nullptr || copy.storage_write_snapshot == nullptr ||
         copy.storage_read_snapshot == nullptr || copy.storage_trim_prefix == nullptr ||
         copy.storage_free_bytes == nullptr)) {
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
    KernelLimits limits{};
    const ayni_status_t status = validate_config(config, &capacities, &limits);
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
    KernelLimits limits{};
    ayni_status_t status = validate_config(config, &capacities, &limits);
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
    const ayni::kernel::KernelStorage storage{
        base + layout.ingress_offset,        base + layout.completions_offset,
        base + layout.storage_buffer_offset, base + layout.agents_offset,
        base + layout.goal_types_offset,     base + layout.goals_offset,
        base + layout.sessions_offset,       base + layout.tasks_offset,
        base + layout.strategies_offset,     base + layout.decisions_offset,
        base + layout.commands_offset,       base + layout.capabilities_offset,
        base + layout.grants_offset};
    *out_kernel = new (memory) ayni_kernel(copy_host(host), capacities, limits, storage);
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
    info.durable_seq = kernel->durable_seq();
    info.events_unmatched = kernel->events_unmatched;
    info.events_discarded = kernel->events_discarded;
    // Whole fields only: one that struct_size covers in part is left as it
    // was (ayni.h).
    std::size_t written = kernel_info_v1_min_size;
    for (const std::size_t field_end : kernel_info_v1_field_ends) {
        if (out_info->struct_size >= field_end) {
            written = field_end;
        }
    }
    std::memcpy(out_info, &info, written);
    return AYNI_OK;
}

ayni_status_t ayni_start(ayni_kernel_t *kernel) {
    if (kernel == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (kernel->in_callback()) {
        return AYNI_INVALID_STATE;
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
    if (kernel->in_callback()) {
        return AYNI_INVALID_STATE;
    }
    ayni_step_report_v1 report{};
    const ayni_status_t status = kernel->step(max_items, max_time_us, report);
    if (status == AYNI_OK && out_report != nullptr) {
        report.struct_size = out_report->struct_size;
        // Whole fields only, as for ayni_kernel_info_v1.
        const std::size_t written = out_report->struct_size >= step_report_v1_durable_end
                                        ? step_report_v1_durable_end
                                        : step_report_v1_min_size;
        std::memcpy(out_report, &report, written);
    }
    return status;
}

ayni_status_t ayni_begin_shutdown(ayni_kernel_t *kernel) {
    if (kernel == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (kernel->in_callback()) {
        return AYNI_INVALID_STATE;
    }
    return kernel->begin_shutdown();
}

ayni_status_t ayni_destroy(ayni_kernel_t *kernel) {
    if (kernel == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (kernel->in_callback() || (kernel->lifecycle != AYNI_LIFECYCLE_CONFIGURING &&
                                  kernel->lifecycle != AYNI_LIFECYCLE_STOPPED)) {
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
    if (result->struct_size != completion_result_v1_min_size &&
        result->struct_size < sizeof(ayni_completion_result_v1)) {
        return AYNI_INVALID_ARGUMENT;
    }
    ayni_completion_result_v1 copy{};
    std::memcpy(&copy, result, result->struct_size);
    copy.struct_size = sizeof(ayni_completion_result_v1);
    return completion->pool->post(*completion, copy);
}

// Reads nothing the owner thread changes outside the ingress lock: the host
// table is fixed at ayni_create, and the gate stands in for the lifecycle
// (ADR-0021). That is also why it cannot refuse a call from a function the
// kernel calls, as owner-thread functions do.
ayni_status_t ayni_post_event(ayni_kernel_t *kernel, const ayni_event_v1 *event) {
    if (kernel == nullptr || event == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    // New in 0.9, so it has one layout.
    const ayni_status_t size_status =
        check_struct_size(event->struct_size, sizeof(ayni_event_v1), sizeof(ayni_event_v1));
    if (size_status != AYNI_OK) {
        return size_status;
    }
    if (event->kind == 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (event->kind != AYNI_EVENT_LOCAL_INPUT) {
        return AYNI_UNSUPPORTED;
    }
    ayni::catalog::CatalogId source;
    if (!source.assign(event->source)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const ayni_status_t status = kernel->ingress.post_local_input(source);
    if (status == AYNI_OK) {
        kernel->host.wake(kernel->host.ctx);
    }
    return status;
}

} // extern "C"
