// C ABI entry points of the kernel itself: version query, memory
// requirements, create, lifecycle and kernel info queries, start, step,
// shutdown, destroy, completion and event posting. They check arguments and
// struct sizes; lifecycle semantics live in kernel_lifecycle.cpp. The work
// model's entry points are in work_api.cpp; other implemented entry points
// live in the corresponding files in this directory. The remaining Host
// trace draft (include/ayni/draft/host_v1.h) has no symbols yet.

#include "catalog/abi_checks.h"
#include "catalog/catalog_id.h"
#include "kernel/api/api_gate.h"
#include "kernel/runtime/kernel_layout.h"
#include "kernel/runtime/kernel_state.h"
#include "kernel/runtime/storage/storage_budget.h"
#include "kernel/service/persistence/work_journal.h"

#include <ayni/ayni.h>

#include <cstddef>
#include <cstdint>
#include <new>

namespace {

// abi_major and abi_minor keep their place in every layout (C-VER-002), so a
// caller built against another version is told so before its size is judged.
constexpr std::size_t config_v1_version_end =
    offsetof(ayni_config_v1, abi_minor) + sizeof(ayni_config_v1::abi_minor);

using ayni::catalog::check_struct_size;
using ayni::kernel::KernelCapacities;
using ayni::kernel::KernelLayout;
using ayni::kernel::KernelLimits;

struct CapacityRange {
    uint32_t min_value;
    uint32_t max_value;
};

// A capacity field must lie in [min, max]. Where the range allows zero, its
// meaning is the field's own: a default, a disabled table or a real limit.
ayni_status_t read_capacity(const ayni_config_v1 &config, uint32_t ayni_config_v1::*field,
                            const CapacityRange &range, uint32_t *out) {
    const uint32_t value = config.*field;
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

// Closed ABI field mapping. The order preserves the established first-error
// precedence.
struct CapacityField {
    uint32_t ayni_config_v1::*field;    // Public input field.
    CapacityRange range;                // Inclusive bounds in the public field's units.
    uint32_t KernelCapacities::*member; // Matching validated output field, never null.
};

constexpr CapacityField capacity_fields[] = {
    {&ayni_config_v1::ingress_capacity,
     {1U, AYNI_INGRESS_CAPACITY_MAX},
     &KernelCapacities::ingress_capacity},
    {&ayni_config_v1::event_body_max_bytes,
     {1U, AYNI_EVENT_BODY_MAX_BYTES_MAX},
     &KernelCapacities::event_body_max_bytes},
    {&ayni_config_v1::completion_slot_count,
     {1U, AYNI_COMPLETION_SLOT_COUNT_MAX},
     &KernelCapacities::completion_slot_count},
    // Zero stays unresolved until every capacity has been read.
    {&ayni_config_v1::storage_buffer_bytes,
     {0U, AYNI_STORAGE_BUFFER_BYTES_MAX},
     &KernelCapacities::storage_buffer_bytes},
    {&ayni_config_v1::agent_capacity,
     {1U, AYNI_AGENT_CAPACITY_MAX},
     &KernelCapacities::agent_capacity},
    {&ayni_config_v1::goal_type_capacity,
     {1U, AYNI_GOAL_TYPE_CAPACITY_MAX},
     &KernelCapacities::goal_type_capacity},
    {&ayni_config_v1::goal_capacity,
     {1U, AYNI_GOAL_CAPACITY_MAX},
     &KernelCapacities::goal_capacity},
    {&ayni_config_v1::goal_parameter_max_bytes,
     {AYNI_GOAL_PARAMETER_MAX_BYTES_MIN, AYNI_GOAL_PARAMETER_MAX_BYTES_MAX},
     &KernelCapacities::goal_parameter_max_bytes},
    {&ayni_config_v1::session_capacity,
     {1U, AYNI_SESSION_CAPACITY_MAX},
     &KernelCapacities::session_capacity},
    {&ayni_config_v1::task_capacity,
     {1U, AYNI_TASK_CAPACITY_MAX},
     &KernelCapacities::task_capacity},
    {&ayni_config_v1::strategy_capacity,
     {1U, AYNI_STRATEGY_CAPACITY_MAX},
     &KernelCapacities::strategy_capacity},
    {&ayni_config_v1::policy_intent_capacity,
     {1U, AYNI_POLICY_INTENT_CAPACITY_MAX},
     &KernelCapacities::policy_intent_capacity},
    {&ayni_config_v1::task_step_capacity,
     {1U, AYNI_TASK_STEP_CAPACITY_MAX},
     &KernelCapacities::task_step_capacity},
    {&ayni_config_v1::task_checkpoint_max_bytes,
     {AYNI_TASK_CHECKPOINT_MAX_BYTES_MIN, AYNI_TASK_CHECKPOINT_MAX_BYTES_MAX},
     &KernelCapacities::task_checkpoint_max_bytes},
    {&ayni_config_v1::command_capacity,
     {1U, AYNI_COMMAND_CAPACITY_MAX},
     &KernelCapacities::command_capacity},
    {&ayni_config_v1::capability_capacity,
     {1U, AYNI_CAPABILITY_CAPACITY_MAX},
     &KernelCapacities::capability_capacity},
    {&ayni_config_v1::authority_scope_max_bytes,
     {AYNI_AUTHORITY_SCOPE_MAX_BYTES_MIN, AYNI_AUTHORITY_SCOPE_MAX_BYTES_MAX},
     &KernelCapacities::authority_scope_max_bytes},
    {&ayni_config_v1::grant_capacity,
     {1U, AYNI_GRANT_CAPACITY_MAX},
     &KernelCapacities::grant_capacity},
    {&ayni_config_v1::grant_delegation_depth_max,
     {AYNI_GRANT_DELEGATION_DEPTH_MAX_MIN, AYNI_GRANT_DELEGATION_DEPTH_MAX_MAX},
     &KernelCapacities::grant_delegation_depth_max},
    {&ayni_config_v1::goal_requirement_capacity,
     {1U, AYNI_GOAL_REQUIREMENT_CAPACITY_MAX},
     &KernelCapacities::goal_requirement_capacity},
    {&ayni_config_v1::resource_capacity,
     {0U, AYNI_RESOURCE_CAPACITY_MAX},
     &KernelCapacities::resource_capacity},
    {&ayni_config_v1::resource_request_capacity,
     {0U, AYNI_RESOURCE_REQUEST_CAPACITY_MAX},
     &KernelCapacities::resource_request_capacity},
    {&ayni_config_v1::call_capability_capacity,
     {0U, AYNI_CALL_CAPABILITY_CAPACITY_MAX},
     &KernelCapacities::call_capability_capacity},
    {&ayni_config_v1::provider_capacity,
     {0U, AYNI_PROVIDER_CAPACITY_MAX},
     &KernelCapacities::provider_capacity},
    {&ayni_config_v1::binding_capacity,
     {0U, AYNI_BINDING_CAPACITY_MAX},
     &KernelCapacities::binding_capacity},
    {&ayni_config_v1::call_capacity,
     {0U, AYNI_CALL_CAPACITY_MAX},
     &KernelCapacities::call_capacity},
    {&ayni_config_v1::call_input_max_bytes,
     {0U, AYNI_CALL_INPUT_BYTES_MAX},
     &KernelCapacities::call_input_max_bytes},
    {&ayni_config_v1::device_capacity,
     {0U, AYNI_DEVICE_CAPACITY_MAX},
     &KernelCapacities::device_capacity},
    {&ayni_config_v1::component_capacity,
     {0U, AYNI_COMPONENT_CAPACITY_MAX},
     &KernelCapacities::component_capacity},
    {&ayni_config_v1::executor_capacity,
     {0U, AYNI_EXECUTOR_CAPACITY_MAX},
     &KernelCapacities::executor_capacity},
    {&ayni_config_v1::execution_binding_capacity,
     {0U, AYNI_EXECUTION_BINDING_CAPACITY_MAX},
     &KernelCapacities::execution_binding_capacity},
    {&ayni_config_v1::scene_capacity,
     {0U, AYNI_SCENE_CAPACITY_MAX},
     &KernelCapacities::scene_capacity},
    {&ayni_config_v1::observation_ingress_capacity,
     {0U, AYNI_OBSERVATION_INGRESS_CAPACITY_MAX},
     &KernelCapacities::observation_ingress_capacity},
    {&ayni_config_v1::observation_source_capacity,
     {0U, AYNI_OBSERVATION_SOURCE_CAPACITY_MAX},
     &KernelCapacities::observation_source_capacity},
    {&ayni_config_v1::memory_capacity,
     {0U, AYNI_MEMORY_CAPACITY_MAX},
     &KernelCapacities::memory_capacity},
    {&ayni_config_v1::memory_body_max_bytes,
     {0U, AYNI_MEMORY_BODY_BYTES_MAX},
     &KernelCapacities::memory_body_max_bytes},
    {&ayni_config_v1::memory_dependency_capacity,
     {0U, AYNI_MEMORY_DEPENDENCY_CAPACITY_MAX},
     &KernelCapacities::memory_dependency_capacity},
    {&ayni_config_v1::call_result_max_bytes,
     {0U, AYNI_CALL_RESULT_BYTES_MAX},
     &KernelCapacities::call_result_max_bytes},
};

// Read each bounded capacity once, then let validate_config resolve storage
// against the complete set. This list contains no cross-field policy.
ayni_status_t read_capacities(const ayni_config_v1 *config, KernelCapacities &capacities) {
    for (const auto &field : capacity_fields) {
        const auto status =
            read_capacity(*config, field.field, field.range, &(capacities.*field.member));
        if (status != AYNI_OK) {
            return status;
        }
    }
    if (capacities.observation_source_capacity == 0 &&
        capacities.observation_ingress_capacity != 0) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (capacities.observation_source_capacity != 0 &&
        capacities.observation_ingress_capacity == 0) {
        capacities.observation_ingress_capacity = AYNI_OBSERVATION_INGRESS_CAPACITY_DEFAULT;
    }
    if (capacities.memory_body_max_bytes == 0U) {
        capacities.memory_body_max_bytes = AYNI_MEMORY_BODY_BYTES_DEFAULT;
    }
    if (capacities.memory_dependency_capacity == 0U) {
        capacities.memory_dependency_capacity = AYNI_MEMORY_DEPENDENCY_CAPACITY_DEFAULT;
    }
    if (capacities.call_input_max_bytes == 0U) {
        capacities.call_input_max_bytes = AYNI_CALL_INPUT_BYTES_DEFAULT;
    }
    if (capacities.call_result_max_bytes == 0U) {
        capacities.call_result_max_bytes = AYNI_CALL_RESULT_BYTES_DEFAULT;
    }
    return AYNI_OK;
}

ayni_status_t validate_config(const ayni_config_v1 *config, KernelCapacities *out_capacities,
                              KernelLimits *out_limits) {
    if (config == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (config->struct_size < config_v1_version_end) {
        return AYNI_INVALID_ARGUMENT;
    }
    // Major 0 is experimental (C-VER-006): any minor may change published
    // semantics, so only the exact version this library implements is safe.
    if (config->abi_major != AYNI_ABI_MAJOR || config->abi_minor != AYNI_ABI_MINOR) {
        return AYNI_UNSUPPORTED_VERSION;
    }
    // Every struct of major 0 has the one layout of this minor (ADR-0053).
    ayni_status_t status =
        check_struct_size(config->struct_size, sizeof(ayni_config_v1), sizeof(ayni_config_v1));
    if (status != AYNI_OK) {
        return status;
    }
    KernelCapacities capacities{};
    status = read_capacities(config, capacities);
    if (status == AYNI_OK) {
        status = resolve_storage_buffer(capacities);
    }
    KernelLimits limits{};
    if (status == AYNI_OK) {
        status = read_capacity(*config, &ayni_config_v1::goal_depth_max,
                               {AYNI_GOAL_DEPTH_MAX_MIN, AYNI_GOAL_DEPTH_MAX_MAX},
                               &limits.goal_depth_max);
    }
    if (status == AYNI_OK) {
        status = read_capacity(*config, &ayni_config_v1::task_model_call_hard_limit,
                               {0U, AYNI_TASK_MODEL_CALL_HARD_LIMIT_MAX},
                               &limits.task_model_call_hard_limit);
        if (limits.task_model_call_hard_limit == 0U) {
            limits.task_model_call_hard_limit = AYNI_TASK_MODEL_CALL_HARD_LIMIT_DEFAULT;
        }
    }
    if (status == AYNI_OK) {
        status = read_capacity(*config, &ayni_config_v1::goal_children_max,
                               {AYNI_GOAL_CHILDREN_MAX_MIN, AYNI_GOAL_CHILDREN_MAX_MAX},
                               &limits.goal_children_max);
    }
    if (status == AYNI_OK) {
        status = read_capacity(*config, &ayni_config_v1::storage_write_interval_us,
                               {0U, AYNI_STORAGE_WRITE_INTERVAL_US_MAX},
                               &limits.storage_write_interval_us);
        if (limits.storage_write_interval_us == 0U) {
            limits.storage_write_interval_us = AYNI_STORAGE_WRITE_INTERVAL_US_DEFAULT;
        }
    }
    if (status == AYNI_OK) {
        status = read_capacity(*config, &ayni_config_v1::storage_log_max_bytes,
                               {0U, AYNI_STORAGE_LOG_MAX_BYTES_MAX}, &limits.storage_log_max_bytes);
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

ayni_status_t validate_host(const ayni_host_v1 *host) {
    if (host == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const ayni_status_t status =
        check_struct_size(host->struct_size, sizeof(ayni_host_v1), sizeof(ayni_host_v1));
    if (status != AYNI_OK) {
        return status;
    }
    const ayni_host_v1 &table = *host;
    if (table.monotonic_now_us == nullptr || table.wake == nullptr ||
        table.ingress_lock == nullptr || table.ingress_unlock == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    // Storage is optional, but a log that can be written and not read back,
    // or the reverse, is a broken table (ADR-0016).
    if ((table.storage_append == nullptr) != (table.storage_read_prefix == nullptr)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const bool some_maintenance =
        table.storage_read_log != nullptr || table.storage_fence_writers != nullptr ||
        table.storage_write_snapshot != nullptr || table.storage_read_snapshot != nullptr ||
        table.storage_trim_prefix != nullptr || table.storage_free_bytes != nullptr;
    if (some_maintenance &&
        (table.storage_append == nullptr || table.storage_read_log == nullptr ||
         table.storage_fence_writers == nullptr || table.storage_write_snapshot == nullptr ||
         table.storage_read_snapshot == nullptr || table.storage_trim_prefix == nullptr ||
         table.storage_free_bytes == nullptr)) {
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

    const ayni::kernel::KernelStorage storage = ayni::kernel::storage_at(memory, layout);
    *out_kernel = new (memory) ayni_kernel(*host, capacities, limits, storage);
    return AYNI_OK;
}

ayni_status_t ayni_get_lifecycle(const ayni_kernel_t *kernel, ayni_lifecycle_t *out_state) {
    if (kernel == nullptr || out_state == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    *out_state = kernel->state();
    return AYNI_OK;
}

ayni_status_t ayni_get_kernel_info(const ayni_kernel_t *kernel, ayni_kernel_info_v1 *out_info) {
    if (kernel == nullptr || out_info == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const ayni_status_t status =
        check_struct_size(out_info->struct_size, sizeof(*out_info), sizeof(*out_info));
    if (status != AYNI_OK) {
        return status;
    }
    kernel->describe(*out_info);
    return AYNI_OK;
}

ayni_status_t ayni_start(ayni_kernel_t *kernel) {
    if (kernel == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::owner);
        gated != AYNI_OK) {
        return gated;
    }
    return kernel->start();
}

ayni_status_t ayni_step(ayni_kernel_t *kernel, uint32_t max_items, uint32_t max_time_us,
                        ayni_step_report_v1 *out_report) {
    if (kernel == nullptr || max_items == 0U || max_time_us == 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (out_report != nullptr) {
        const ayni_status_t status =
            check_struct_size(out_report->struct_size, sizeof(*out_report), sizeof(*out_report));
        if (status != AYNI_OK) {
            return status;
        }
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::owner);
        gated != AYNI_OK) {
        return gated;
    }
    ayni_step_report_v1 report{};
    const ayni_status_t status = kernel->step(max_items, max_time_us, report);
    if (status == AYNI_OK && out_report != nullptr) {
        report.struct_size = sizeof(report);
        *out_report = report;
    }
    return status;
}

ayni_status_t ayni_begin_shutdown(ayni_kernel_t *kernel) {
    if (kernel == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::owner);
        gated != AYNI_OK) {
        return gated;
    }
    return kernel->begin_shutdown();
}

ayni_status_t ayni_destroy(ayni_kernel_t *kernel) {
    if (kernel == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::destroy);
        gated != AYNI_OK) {
        return gated;
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
        result->struct_size, sizeof(ayni_completion_result_v1), sizeof(ayni_completion_result_v1));
    if (status != AYNI_OK) {
        return status;
    }
    return completion->pool->post(*completion, *result);
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
    return kernel->post_local_input(source);
}

} // extern "C"
