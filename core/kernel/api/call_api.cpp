#include "kernel/service/memory/memory_access.h"
// C ABI queries of one logical call: its phase and pending wait, and a
// copy of its result once it finished (ayni/calls.h).

#include "kernel/api/api_gate.h"
#include "kernel/runtime/kernel_state.h"

#include <cstring>

extern "C" {
ayni_status_t ayni_get_call(const ayni_kernel_t *kernel, uint64_t logical_call_id,
                            ayni_call_info_v1 *out_info) {
    if (kernel == nullptr || out_info == nullptr || out_info->struct_size != sizeof(*out_info) ||
        logical_call_id == 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::owner);
        gated != AYNI_OK) {
        return gated;
    }
    const auto &calls = kernel->model().calls;
    uint32_t index = 0;
    if (!calls.find(logical_call_id, index)) {
        return AYNI_NOT_FOUND;
    }
    kernel->call_view().describe_call(index, kernel->now_us(), kernel->durable_seq(), *out_info);
    return AYNI_OK;
}

// A diagnostic state can be visible before durability, but its business text
// cannot be externally presented until the original result batch is durable.
ayni_status_t ayni_copy_call_result(const ayni_kernel_t *kernel, uint64_t logical_call_id,
                                    char *buffer, uint32_t capacity, uint32_t *out_size) {
    if (kernel == nullptr || out_size == nullptr || (capacity != 0U && buffer == nullptr) ||
        logical_call_id == 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::owner);
        gated != AYNI_OK) {
        return gated;
    }
    const auto &calls = kernel->model().calls;
    uint32_t index = 0;
    if (!calls.find(logical_call_id, index)) {
        return AYNI_NOT_FOUND;
    }
    const auto &state = calls.state(index);
    // Use the current writer's prefix, never the monotonic floor during fencing.
    // Completed RAM-only EPHEMERAL calls are readable immediately without durable_seq.
    if (kernel->state() == AYNI_LIFECYCLE_FENCED || kernel->state() == AYNI_LIFECYCLE_RECOVERING ||
        state.outcome == 0U ||
        (!state.ephemeral && state.latest.transition.commit_seq > kernel->writer_durable_seq())) {
        return AYNI_BUSY;
    }
    if (state.outcome == AYNI_CALL_ABANDONED) {
        return AYNI_RESULT_UNKNOWN;
    }
    uint32_t task_index = 0;
    if (calls.record(index).result_kind == ayni::work::CallResultKind::unavailable ||
        state.content_erased ||
        !kernel->model().tasks.find(calls.identity(index).task_id, task_index) ||
        !ayni::kernel::task_content_current(
            {kernel->model(), kernel->now_us(), kernel->writer_durable_seq()}, task_index)) {
        return AYNI_EXPIRED;
    }
    const auto saved = calls.record(index);
    const auto result = saved.result_kind == ayni::work::CallResultKind::tool_calls
                            ? ayni_string_v1{}
                            : saved.result;
    if (capacity < result.size) {
        *out_size = result.size;
        return AYNI_BUFFER_TOO_SMALL;
    }
    if (result.size != 0U) {
        std::memcpy(buffer, result.data, result.size);
    }
    *out_size = result.size;
    return AYNI_OK;
}
} // extern "C"
