// What a call waits for, projected from its owners on demand (ayni_get_call);
// nothing here is cached in the Task.

#include "kernel/service/call/call_runtime.h"

namespace ayni::kernel {
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
