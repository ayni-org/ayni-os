// What a call waits for, projected from its owners on demand (ayni_get_call);
// nothing here is cached in the Task.

#include "kernel/service/call/call_runtime.h"

namespace ayni::kernel {
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

// C-CALL-003/009: project original execution facts without starting Provider I/O.
void CallRuntime::describe_call(uint32_t index, uint64_t now_us, uint64_t durable_seq,
                                ayni_call_info_v1 &out) const {
    const auto &calls = work_.calls;
    const auto &id = calls.identity(index);
    const auto &state = calls.state(index);
    const auto *action = calls.action(index);
    uint64_t waited = 0;
    const auto wait_reason = waiting(index, now_us, durable_seq, waited);
    const auto query = state.query.value_or(ayni::work::CallQuery{});
    out = {sizeof(out),
           phase(state, action),
           id.call_id,
           id.task_id,
           state.subscription ? state.subscription->generation : id.operation_id,
           id.owner_instance_id,
           state.attempt,
           state.progress_sequence,
           state.latest.transition.commit_seq,
           state.outcome,
           state.status,
           state.reason,
           static_cast<ayni_call_effect_t>(calls.effect(index)),
           state.cancel_cause == ayni::work::CallCancelCause::none ? 0U : 1U,
           state.cancel_result,
           calls.input(index).size,
           calls.result(index).size + calls.value_result(index).size,
           wait_reason,
           ayni::work::tool_call_count(calls.tool_calls(index)),
           waited,
           calls.query_revision(index),
           static_cast<ayni_call_query_phase_t>(query.phase),
           query.status,
           query.reason,
           query.conclusion,
           0,
           query.attempt,
           static_cast<uint32_t>(calls.record(index).result_kind),
           0,
           state.model_usage};
}

// Resource owner diagnoses its own queue. Durability and Provider ownership
// are independent waits; none is stored as a second Task lifecycle flag.
ayni_call_wait_t CallRuntime::waiting(uint32_t index, uint64_t now_us, uint64_t durable_seq,
                                      uint64_t &waited_us) const {
    waited_us = 0;
    const auto &state = work_.calls.state(index);
    if (slots_[index].active && slots_[index].active->stage == Stage::lease_stop) {
        return AYNI_CALL_WAIT_QUARANTINED;
    }
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
