// Public owner queries reuse the same Task-scoped directory as decision leases.
#include "catalog/abi_checks.h"
#include "kernel/api/api_gate.h"
#include <ayni/directory.h>

extern "C" {
ayni_status_t ayni_register_capability_usage(ayni_kernel_t *kernel, ayni_string_v1 summary,
                                             const ayni_tool_usage_v1 *usage) {
    if (!kernel || !usage) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (const auto shape =
            ayni::catalog::check_struct_size(usage->struct_size, sizeof(*usage), sizeof(*usage));
        shape != AYNI_OK) {
        return shape;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::registration);
        gated != AYNI_OK) {
        return gated;
    }
    return kernel->configure_usage(summary, *usage);
}
ayni_status_t ayni_list_task_capabilities(const ayni_kernel_t *kernel, uint64_t task_id,
                                          const ayni_capability_cursor_v1 *cursor,
                                          ayni_capability_info_v1 *items, uint32_t capacity,
                                          uint32_t *out_count,
                                          ayni_capability_cursor_v1 *out_next) {
    if (!kernel || !task_id || !cursor || !out_count || !out_next) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::running);
        gated != AYNI_OK) {
        return gated;
    }
    uint32_t index = 0;
    if (!kernel->model().tasks.find(task_id, index)) {
        return AYNI_NOT_FOUND;
    }
    return kernel->call_view().list_capabilities(index, kernel->now_us(), kernel->durable_seq(),
                                                 *cursor, items, capacity, *out_count, *out_next);
}
ayni_status_t ayni_inspect_task_capability(const ayni_kernel_t *kernel, uint64_t task_id,
                                           ayni_string_v1 binding_id, uint32_t binding_version,
                                           ayni_capability_detail_v1 *out) {
    if (!kernel || !task_id || !out) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::running);
        gated != AYNI_OK) {
        return gated;
    }
    uint32_t index = 0;
    if (!kernel->model().tasks.find(task_id, index)) {
        return AYNI_NOT_FOUND;
    }
    return kernel->call_view().inspect_capability(index, kernel->now_us(), kernel->durable_seq(),
                                                  binding_id, binding_version, *out);
}
ayni_status_t ayni_get_task_tool_authority(const ayni_kernel_t *kernel, uint64_t task_id,
                                           uint32_t index, ayni_task_tool_authority_v1 *out) {
    if (!kernel || !task_id || !out) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (const auto shape =
            ayni::catalog::check_struct_size(out->struct_size, sizeof(*out), sizeof(*out));
        shape != AYNI_OK) {
        return shape;
    }
    if (out->reserved || out->reserved_tail) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::owner);
        gated != AYNI_OK) {
        return gated;
    }
    const auto &model = kernel->model();
    uint32_t task_index = 0;
    if (!model.tasks.find(task_id, task_index)) {
        return AYNI_NOT_FOUND;
    }
    const auto &task = model.tasks.at(task_index);
    if (!model.task_authority.contains(task.slot, task_id)) {
        return AYNI_NOT_FOUND;
    }
    const auto record = model.task_authority.at(task.slot);
    if (index >= record.tool_count) {
        return AYNI_NOT_FOUND;
    }
    const auto &reference = record.tools[index];
    uint32_t call_index = 0;
    if (!model.calls.find(reference.source_call_id, call_index)) {
        return AYNI_INTERNAL;
    }
    *out = {sizeof(*out),
            0,
            task_id,
            reference.source_call_id,
            model.calls.identity(call_index).authorization.grant_id,
            reference.selected.grant_id,
            reference.selected.revision,
            0,
            record.stamp.commit_seq};
    return AYNI_OK;
}
} // extern "C"
