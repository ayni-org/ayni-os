#include "kernel/kernel_state.h"

#include <cstring>

namespace ayni::kernel {
// Resource owner diagnoses its own queue. Durability and Provider ownership
// are independent waits; none is stored as a second Task lifecycle flag.
ayni_call_wait_t CallRuntime::waiting(uint32_t index, uint64_t now_us, uint64_t durable_seq,
                                      uint64_t &waited_us) const {
    waited_us = 0;
    const auto &state = work_.calls.state(index);
    if (work_.calls.query_pending(index) && slots_[index].active &&
        slots_[index].active->stage == Stage::submitted) {
        return AYNI_CALL_WAIT_PROVIDER;
    }
    if (state.outcome != 0U) {
        return state.latest.transition.commit_seq > durable_seq ? AYNI_CALL_WAIT_DURABILITY
                                                                : AYNI_CALL_WAIT_NONE;
    }
    const auto &active = slots_[index].active;
    if (active && active->stage == Stage::blocked) {
        return AYNI_CALL_WAIT_COMPATIBILITY;
    }
    resources::WaitReason reason{};
    uint32_t resource = 0;
    if (resources_.waiting(work_.calls.identity(index).call_id, now_us, reason, resource,
                           waited_us)) {
        switch (reason) {
        case resources::WaitReason::available:
            return AYNI_CALL_WAIT_NONE;
        case resources::WaitReason::fifo:
            return AYNI_CALL_WAIT_FIFO;
        case resources::WaitReason::occupied:
            return AYNI_CALL_WAIT_OCCUPIED;
        case resources::WaitReason::offline:
            return AYNI_CALL_WAIT_OFFLINE;
        case resources::WaitReason::faulted:
            return AYNI_CALL_WAIT_FAULTED;
        case resources::WaitReason::conflict:
            return AYNI_CALL_WAIT_CONFLICT;
        case resources::WaitReason::quarantined:
            return AYNI_CALL_WAIT_QUARANTINED;
        }
    }
    if (active && active->stage == Stage::submitted) {
        return AYNI_CALL_WAIT_PROVIDER;
    }
    return state.latest.transition.commit_seq > durable_seq ? AYNI_CALL_WAIT_DURABILITY
                                                            : AYNI_CALL_WAIT_NONE;
}
} // namespace ayni::kernel

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
    if (kernel->in_callback()) {
        return AYNI_INVALID_STATE;
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
                 0,
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
    if (kernel->in_callback()) {
        return AYNI_INVALID_STATE;
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
