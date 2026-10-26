// C ABI queries of one logical call: its phase and pending wait, and a
// copy of its result once it finished (ayni/calls.h).

#include "kernel/api/api_gate.h"
#include "kernel/kernel_state.h"

#include <cstring>

namespace {
ayni_call_phase_t phase(const ayni::work::CallState &state, const ayni::work::ActionFact *action) {
    using Phase = ayni::work::ActionPhase;
    if (state.outcome == AYNI_CALL_ABANDONED) {
        return AYNI_CALL_RESULT_UNKNOWN;
    }
    if (state.outcome != 0U) {
        return AYNI_CALL_FINISHED;
    }
    return action == nullptr                     ? AYNI_CALL_QUEUED
           : action->phase == Phase::prepared    ? AYNI_CALL_PREPARED
           : action->phase == Phase::in_progress ? AYNI_CALL_IN_PROGRESS
                                                 : AYNI_CALL_DISPATCHING;
}
} // namespace

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
    const auto &calls = kernel->work.calls;
    uint32_t index = 0;
    if (!calls.find(logical_call_id, index)) {
        return AYNI_NOT_FOUND;
    }
    const auto &id = calls.identity(index);
    const auto &state = calls.state(index);
    const auto *action = calls.action(index);
    uint64_t waited = 0;
    const auto waiting = kernel->calls.waiting(
        index, kernel->host.monotonic_now_us(kernel->host.ctx), kernel->durable_seq(), waited);
    const auto query = state.query.value_or(ayni::work::CallQuery{});
    *out_info = {sizeof(*out_info),
                 phase(state, action),
                 id.call_id,
                 id.task_id,
                 id.operation_id,
                 id.owner_instance_id,
                 state.attempt,
                 state.progress_sequence,
                 state.latest.transition.commit_seq,
                 state.outcome,
                 state.status,
                 state.reason,
                 action == nullptr ? AYNI_CALL_EFFECT_NONE
                                   : static_cast<ayni_call_effect_t>(action->effect),
                 state.cancel_cause == ayni::work::CallCancelCause::none ? 0U : 1U,
                 state.cancel_result,
                 calls.input(index).size,
                 calls.result(index).size,
                 waiting,
                 ayni::work::tool_call_count(calls.tool_calls(index)),
                 waited,
                 calls.query_revision(index),
                 static_cast<ayni_call_query_phase_t>(query.phase),
                 query.status,
                 query.reason,
                 query.conclusion,
                 0,
                 query.attempt};
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
    const auto &calls = kernel->work.calls;
    uint32_t index = 0;
    if (!calls.find(logical_call_id, index)) {
        return AYNI_NOT_FOUND;
    }
    const auto &state = calls.state(index);
    // Use the current writer's prefix, never the monotonic floor during fencing.
    if (kernel->lifecycle == AYNI_LIFECYCLE_FENCED ||
        kernel->lifecycle == AYNI_LIFECYCLE_RECOVERING || state.outcome == 0U ||
        state.latest.transition.commit_seq > kernel->log.durable_seq()) {
        return AYNI_BUSY;
    }
    if (state.outcome == AYNI_CALL_ABANDONED) {
        return AYNI_RESULT_UNKNOWN;
    }
    const auto result = calls.result(index);
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
