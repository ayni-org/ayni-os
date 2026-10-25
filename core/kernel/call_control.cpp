#include "foundation/deadline.h"
#include "kernel/call_runtime.h"

namespace ayni::kernel {
// Urgent output inhibition precedes journal admission. Routine polling alternates
// with business work; an idle work turn yields to the Kernel's other work owners,
// including when max_items=1. No hidden second callback bypasses the step budget.
CallRuntime::Advance CallRuntime::advance_managed(uint64_t now_us, uint64_t durable_seq, Mode mode,
                                                  JournalAdmission admission) {
    const auto urgent = advance_urgent_stop(now_us, durable_seq, mode);
    if (urgent != Advance::idle) {
        return urgent;
    }
    if (next_lane_ == NextLane::control) {
        next_lane_ = NextLane::work;
        const auto control = advance_control(now_us, durable_seq, mode);
        if (control != Advance::idle) {
            return control;
        }
    } else {
        next_lane_ = NextLane::control;
    }
    return admission == JournalAdmission::available || mode == Mode::fenced || mode == Mode::closing
               ? advance(now_us, durable_seq, mode)
               : Advance::idle;
}

// Lease slots outlive work-model reload. Inhibition never trusts a partially
// restored CallTable index or waits for a new writer to finish recovery.
CallRuntime::Advance CallRuntime::advance_urgent_stop(uint64_t now_us, uint64_t durable_seq,
                                                      Mode mode) {
    for (uint32_t at = 0; at < permits_.capacity(); ++at) {
        const auto *lease = permits_.at(at);
        if (lease == nullptr || lease->stopped) {
            continue;
        }
        const bool issue_stop =
            lease->phase == resources::PermitLease::Phase::issued
                ? should_stop(*lease, now_us, durable_seq, mode)
                : lease->stop_delivery == resources::PermitLease::StopDelivery::pending;
        if (issue_stop) {
            return poll_executor(at, now_us, durable_seq, mode);
        }
    }
    return Advance::idle;
}

// Independent ports never mutate persistent facts. In particular, journal
// backpressure cannot delay disabling output or maintaining actuator quarantine.
CallRuntime::Advance CallRuntime::poll_executor(uint32_t index, uint64_t now_us,
                                                uint64_t durable_seq, Mode mode) {
    auto *lease = permits_.at(index);
    if (lease == nullptr) {
        return Advance::idle;
    }
    if (lease->stopped) {
        if (stopped_releasable(*lease, durable_seq, mode)) {
            const auto call_id = lease->call_id;
            if (!permits_.release_stopped(call_id)) {
                return Advance::internal;
            }
            uint32_t active = 0;
            if (active_index(call_id, active) && slots_[active].active->stage == Stage::retiring) {
                release(active, now_us);
            }
            return Advance::applied;
        }
        return Advance::idle;
    }
    const bool allowed = !should_stop(*lease, now_us, durable_seq, mode);
    if (!allowed && !permits_.request_stop(lease->call_id)) {
        return Advance::internal;
    }
    const auto &ports = work_.execution_catalog.executor(lease->executor_index).ports;
    if (lease->phase == resources::PermitLease::Phase::stopping &&
        lease->stop_delivery == resources::PermitLease::StopDelivery::pending) {
        work_.calling_out = true;
        const auto status = ports.stop(ports.ctx, lease->permit.permit_id);
        work_.calling_out = false;
        // A not-installed permit remains quarantined until terminal + authoritative absence.
        // Port failure leaves the gate to expire; no return status is physical stop evidence.
        lease->port_status = status;
        lease->stop_delivery = resources::PermitLease::StopDelivery::sent;
        lease->poll_after_us = now_us;
        return Advance::applied;
    }
    if (now_us < lease->poll_after_us) {
        return Advance::idle;
    }
    lease->poll_after_us = foundation::deadline(now_us, ports.poll_period_us);
    ayni_executor_status_v1 status{sizeof(status), AYNI_TARGET_UNVERIFIED, {}};
    status.control.struct_size = sizeof(status.control);
    work_.calling_out = true;
    const auto read = ports.status(ports.ctx, lease->permit.permit_id, &status);
    work_.calling_out = false;
    lease->port_status = read;
    if (read == AYNI_NOT_FOUND && lease->terminal_seen) {
        return permits_.not_started(lease->call_id) ? Advance::applied : Advance::internal;
    }
    if (read == AYNI_OK && !permits_.observe(lease->call_id, status)) {
        lease->port_status = AYNI_INVALID_ARGUMENT;
    }
    if (lease->port_status != AYNI_OK) {
        return permits_.request_stop(lease->call_id) ? Advance::applied : Advance::internal;
    }
    if (allowed && status.control.phase == AYNI_CONTROL_ACTIVE &&
        lease->permit.form == AYNI_FORM_SESSION) {
        return renew_permit(*lease);
    }
    return Advance::applied;
}

// Normal operation requires durable stopped evidence. Explicit closing of an
// unwritable Kernel may release only volatile ownership after all borrowers end;
// the saved possible dispatch is left unchanged for later UNKNOWN reconciliation.
bool CallRuntime::stopped_releasable(const resources::PermitLease &lease, uint64_t durable_seq,
                                     Mode mode) const {
    uint32_t index = 0;
    if (mode == Mode::closing) {
        return active_index(lease.call_id, index) && slots_[index].active->stage == Stage::retiring;
    }
    if (mode == Mode::fenced || !work_.calls.find(lease.call_id, index)) {
        return false;
    }
    const auto &state = work_.calls.state(index);
    return state.execution && work::execution_stopped(*state.execution) &&
           state.latest.transition.commit_seq <= durable_seq;
}

bool CallRuntime::active_index(uint64_t call_id, uint32_t &index) const {
    for (uint32_t at = 0; at < limits_.capacity; ++at) {
        if (slots_[at].active && slots_[at].active->call_id == call_id) {
            index = at;
            return true;
        }
    }
    return false;
}

bool CallRuntime::delivery_expired(uint32_t index, uint64_t now_us) const {
    const auto &active = slots_[index].active;
    return active && active->delivery && now_us >= active->delivery->deadline_us;
}

// All execution gates share the same decision for renewal and immediate stopping.
bool CallRuntime::should_stop(const resources::PermitLease &lease, uint64_t now_us,
                              uint64_t durable_seq, Mode mode) const {
    if (mode != Mode::running || lease.terminal_seen) {
        return true;
    }
    uint32_t active = 0;
    uint32_t index = 0;
    uint32_t task_index = 0;
    if (!active_index(lease.call_id, active) || !work_.calls.find(lease.call_id, index) ||
        !work_.tasks.find(work_.calls.identity(index).task_id, task_index)) {
        return true;
    }
    const auto &task = work_.tasks.at(task_index);
    return delivery_expired(active, now_us) || task.call.end != work::TaskCall::End::none ||
           task.lifecycle == AYNI_TASK_PAUSED ||
           work_.calls.state(index).cancel_cause != work::CallCancelCause::none ||
           work_.check_task_authority(task_index, now_us, durable_seq).status != AYNI_OK ||
           !execution_allowed(index, now_us, durable_seq) || !resources_.executable(lease.claim);
}

// Renewal uses the original complete generations and a new executor-local nonce.
CallRuntime::Advance CallRuntime::renew_permit(resources::PermitLease &lease) {
    const auto &ports = work_.execution_catalog.executor(lease.executor_index).ports;
    ayni_executor_nonce_v1 nonce{};
    work_.calling_out = true;
    const auto challenged = ports.challenge(ports.ctx, &lease.permit.epochs, &nonce);
    ayni_status_t renewed = challenged == AYNI_OK ? AYNI_INVALID_ARGUMENT : challenged;
    if (challenged == AYNI_OK && nonce.control_domain_id == lease.permit.control_domain_id &&
        nonce.nonce != 0) {
        const ayni_permit_renewal_v1 renewal{sizeof(renewal), 0, lease.permit.permit_id,
                                             nonce.nonce, lease.permit.epochs};
        renewed = ports.renew(ports.ctx, &renewal);
    }
    work_.calling_out = false;
    lease.port_status = renewed;
    if (renewed != AYNI_OK && !permits_.request_stop(lease.call_id)) {
        return Advance::internal;
    }
    return Advance::applied;
}

CallRuntime::Advance CallRuntime::advance_control(uint64_t now_us, uint64_t durable_seq,
                                                  Mode mode) {
    for (uint32_t scanned = 0; scanned < permits_.capacity(); ++scanned) {
        const uint32_t index = control_cursor_;
        control_cursor_ = (control_cursor_ + 1U) % permits_.capacity();
        const auto result = poll_executor(index, now_us, durable_seq, mode);
        if (result != Advance::idle) {
            return result;
        }
    }
    return Advance::idle;
}

// Match independent readiness without consulting Provider queues. Evaluating
// authorization here is read-only; callbacks are confined to advance_control.
bool CallRuntime::control_ready(uint64_t now_us, uint64_t durable_seq, Mode mode) const {
    for (uint32_t at = 0; at < permits_.capacity(); ++at) {
        const auto *lease = permits_.at(at);
        if (lease == nullptr) {
            continue;
        }
        if (lease->stopped) {
            if (stopped_releasable(*lease, durable_seq, mode)) {
                return true;
            }
            continue;
        }
        if (now_us >= lease->poll_after_us) {
            return true;
        }
        if (lease->phase == resources::PermitLease::Phase::stopping) {
            continue;
        }
        if (should_stop(*lease, now_us, durable_seq, mode)) {
            return true;
        }
    }
    return false;
}
} // namespace ayni::kernel
