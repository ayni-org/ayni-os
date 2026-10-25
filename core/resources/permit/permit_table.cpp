#include "resources/permit/permit_table.h"
#include <new>
namespace ayni::resources {
std::size_t PermitTable::storage_size(uint32_t executors) {
    return sizeof(Slot) * executors;
}
PermitTable::PermitTable(void *storage, uint32_t executors, ResourceAdmission &resources)
    : slots_(static_cast<Slot *>(storage)), capacity_(executors), resources_(resources) {
    for (uint32_t at = 0; at < capacity_; ++at) {
        new (slots_ + at) Slot{};
    }
}
bool PermitTable::available(uint32_t executor) const {
    return executor < capacity_ && !slots_[executor];
}
bool PermitTable::install(uint64_t call_id, uint32_t executor, ResourceClaim claim,
                          const ayni_execution_permit_v1 &permit, uint64_t now_us) {
    if (!available(executor) || call_id == 0 || claim.call_id != call_id || claim.generation == 0 ||
        !resources_.executable(claim)) {
        return false;
    }
    slots_[executor] = PermitLease{call_id,
                                   executor,
                                   claim,
                                   permit,
                                   now_us,
                                   PermitLease::Phase::issued,
                                   PermitLease::StopDelivery::pending,
                                   AYNI_OK,
                                   false,
                                   std::nullopt};
    return true;
}
PermitLease *PermitTable::find(uint64_t call_id) {
    for (uint32_t at = 0; at < capacity_; ++at) {
        if (slots_[at] && slots_[at]->call_id == call_id) {
            return &*slots_[at];
        }
    }
    return nullptr;
}
const PermitLease *PermitTable::find(uint64_t call_id) const {
    for (uint32_t at = 0; at < capacity_; ++at) {
        if (slots_[at] && slots_[at]->call_id == call_id) {
            return &*slots_[at];
        }
    }
    return nullptr;
}
bool PermitTable::request_stop(uint64_t call_id) {
    auto *lease = find(call_id);
    if (lease == nullptr) {
        return false;
    }
    if (lease->phase == PermitLease::Phase::issued) {
        if (!resources_.release(lease->claim)) {
            return false;
        }
        lease->phase = PermitLease::Phase::stopping;
        lease->poll_after_us =
            0; // Owner should send the independent stop before its next normal poll.
    }
    return true;
}
// Gate identity and lifecycle are explicit. A local stop request, elapsed lease
// or successful Provider result alone never clears an actuator quarantine.
bool PermitTable::observe(uint64_t call_id, const ayni_executor_status_v1 &status) {
    auto *lease = find(call_id);
    const auto &control = status.control;
    if (lease == nullptr || status.struct_size != sizeof(status) ||
        control.struct_size != sizeof(control) || control.permit_id != lease->permit.permit_id ||
        control.operation_id != lease->permit.operation_id || control.reserved != 0 ||
        status.target > AYNI_TARGET_UNSATISFIED || control.phase < AYNI_CONTROL_ACTIVE ||
        control.phase > AYNI_CONTROL_STOPPED || control.stop_cause > AYNI_CONTROL_STOP_CLOCK ||
        (control.phase == AYNI_CONTROL_ACTIVE ? control.stop_cause != AYNI_CONTROL_STOP_NONE
                                              : control.stop_cause == AYNI_CONTROL_STOP_NONE)) {
        return false;
    }
    if (control.phase != AYNI_CONTROL_ACTIVE && !request_stop(call_id)) {
        return false;
    }
    if (control.phase == AYNI_CONTROL_STOPPED) {
        if (!lease->stopped) {
            lease->stopped = StopFact{AYNI_EXECUTION_STOPPED, control.stop_cause, status.target};
        }
    }
    return true;
}
bool PermitTable::not_started(uint64_t call_id) {
    auto *lease = find(call_id);
    if (lease == nullptr || !lease->terminal_seen || !request_stop(call_id)) {
        return false;
    }
    if (!lease->stopped) {
        lease->stopped =
            StopFact{AYNI_EXECUTION_NOT_STARTED, AYNI_CONTROL_STOP_NONE, AYNI_TARGET_UNVERIFIED};
    }
    return true;
}
bool PermitTable::release_stopped(uint64_t call_id) {
    auto *lease = find(call_id);
    if (lease == nullptr || !lease->stopped || lease->phase != PermitLease::Phase::stopping) {
        return false;
    }
    resources_.confirm_all_stopped(lease->claim);
    slots_[lease->executor_index].reset();
    return true;
}
uint64_t PermitTable::next_deadline_us() const {
    uint64_t next = UINT64_MAX;
    for (uint32_t at = 0; at < capacity_; ++at) {
        if (slots_[at] && !slots_[at]->stopped && slots_[at]->poll_after_us < next) {
            next = slots_[at]->poll_after_us;
        }
    }
    return next;
}
bool PermitTable::empty() const {
    for (uint32_t at = 0; at < capacity_; ++at) {
        if (slots_[at]) {
            return false;
        }
    }
    return true;
}
} // namespace ayni::resources
