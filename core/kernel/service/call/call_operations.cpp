// ADR-0086: operation handles select original Task-owned facts, never Provider callbacks.
#include "foundation/deadline.h"
#include "kernel/service/call/call_runtime.h"
#include "kernel/service/memory/memory_access.h"
#include <cstring>
namespace ayni::kernel {
// A finite wait registers exactly a past observed frontier, never a fabricated future.
ayni_status_t CallRuntime::check_wait_control(const work::OperationWait &basis,
                                              const ayni_operation_control_v1 &control,
                                              uint64_t now_us, work::OperationWait &wait) {
    if (!control.timeout_ms || control.timeout_ms > AYNI_OPERATION_WAIT_MS_MAX) {
        return AYNI_INVALID_ARGUMENT;
    }
    // A token is evidence of an observed past, never permission to invent a future.
    // The shared representation validator also enforces the tagged phase domain.
    auto observed = basis;
    observed.boot_epoch = control.after.boot_epoch;
    observed.sequence = control.after.sequence;
    observed.gaps = control.after.gaps;
    observed.phase = control.after.phase;
    observed.cancel_requested = control.after.cancel_requested;
    observed.cancel_result = control.after.cancel_result;
    observed.outcome = control.after.outcome;
    observed.timeout_ms = control.timeout_ms;
    if (!observed.valid() || observed.boot_epoch > basis.boot_epoch ||
        observed.sequence > basis.sequence || observed.gaps > basis.gaps ||
        (observed.boot_epoch == basis.boot_epoch &&
         (observed.cancel_requested > basis.cancel_requested ||
          (observed.outcome && !basis.outcome)))) {
        return AYNI_INVALID_ARGUMENT;
    }
    const uint64_t duration_us = uint64_t{control.timeout_ms} * microseconds_per_millisecond;
    const auto until_us = foundation::deadline(now_us, duration_us);
    if (until_us <= now_us || until_us - now_us != duration_us) {
        return AYNI_INVALID_ARGUMENT; // Refuse a truncated finite wait near the clock limit.
    }
    const auto current = basis.frontier();
    if (!ayni_operation_frontier_equal(&current, &control.after) || basis.outcome) {
        return AYNI_STALE; // A change before wait is returned immediately, never swallowed.
    }
    wait = basis;
    wait.timeout_ms = control.timeout_ms;
    return AYNI_OK;
}

ayni_status_t CallRuntime::operation_index(const ayni_operation_ref_v1 &reference,
                                           uint32_t &index) const {
    if (!decision_ || !work_.calling_out()) {
        return AYNI_INVALID_STATE;
    }
    const auto &task = work_.tasks.at(decision_->task_index);
    if (!foundation::is_kernel_id(reference.task_id) ||
        !foundation::is_kernel_id(reference.logical_call_id) ||
        !foundation::is_kernel_id(reference.operation_id)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (reference.task_id != task.task_id || !work_.calls.find(reference.logical_call_id, index) ||
        work_.calls.identity(index).task_id != task.task_id) {
        return AYNI_NOT_FOUND;
    }
    const auto &contract = work_.calls.contract(index);
    if (contract.category != catalog::CallClass::tool ||
        (contract.form != catalog::CallForm::action &&
         contract.form != catalog::CallForm::subscription) ||
        (contract.form == catalog::CallForm::action &&
         work_.calls.identity(index).mode != AYNI_CALL_MODE_MANAGED)) {
        return AYNI_UNSUPPORTED;
    }
    const auto now = host_.monotonic_now_us(host_.ctx);
    const auto authority =
        work_.check_task_authority(decision_->task_index, now, decision_->durable_seq);
    if (authority.status != AYNI_OK) {
        return authority.status;
    }
    return task_content_current({work_, now, decision_->durable_seq}, decision_->task_index)
               ? AYNI_OK
               : AYNI_EXPIRED;
}

// Progress/sample identity is projected from its owner once; same-state renew ACKs are excluded.
work::OperationWait CallRuntime::operation_basis(uint32_t index) const {
    const auto &id = work_.calls.identity(index);
    const auto &state = work_.calls.state(index);
    work::OperationWait basis{};
    basis.call_id = id.call_id;
    basis.generation = state.subscription ? state.subscription->generation : id.operation_id;
    basis.form = static_cast<uint32_t>(work_.calls.contract(index).form);
    basis.boot_epoch = work_.ids.boot_epoch();
    basis.cancel_requested = state.cancel_cause == work::CallCancelCause::none ? 0U : 1U;
    basis.cancel_result = state.cancel_result;
    basis.outcome = state.outcome;
    if (state.subscription) {
        basis.phase = static_cast<uint32_t>(state.subscription->phase);
        ayni_observation_info_v1 sample{};
        sample.struct_size = sizeof(sample);
        if (observations_.subscription_info(id.call_id, host_.monotonic_now_us(host_.ctx),
                                            sample) == AYNI_OK &&
            sample.reference.generation == basis.generation) {
            basis.sequence = sample.reference.sequence;
            basis.gaps = sample.sequence_gaps;
        }
    } else {
        ayni_call_info_v1 call{};
        describe_call(index, host_.monotonic_now_us(host_.ctx), UINT64_MAX, call);
        basis.phase = call.phase;
        basis.sequence = state.progress_sequence;
    }
    return basis;
}

// Known execution facts survive unusable sample bodies. Reading never performs remote I/O.
ayni_status_t CallRuntime::read_operation(const void *context,
                                          const ayni_operation_ref_v1 *reference,
                                          ayni_operation_info_v1 *out) {
    if (!context || !reference || !out || out->struct_size != sizeof(*out)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto &runtime = *static_cast<const CallRuntime *>(context);
    uint32_t index = 0;
    const auto status = runtime.operation_index(*reference, index);
    if (status != AYNI_OK) {
        return status;
    }
    const auto &id = runtime.work_.calls.identity(index);
    const auto basis = runtime.operation_basis(index);
    ayni_operation_info_v1 result{};
    result.struct_size = sizeof(result);
    result.form = basis.form;
    result.reference = {id.task_id, id.call_id, basis.generation};
    result.frontier = basis.frontier();
    result.step_key = id.step_key.snapshot();
    const auto now = runtime.host_.monotonic_now_us(runtime.host_.ctx);
    runtime.describe_call(index, now, runtime.decision_->durable_seq, result.call);
    if (runtime.work_.calls.state(index).subscription) {
        result.subscription.struct_size = sizeof(result.subscription);
        runtime.subscription_info(index, now, runtime.decision_->durable_seq, result.subscription);
        ayni_observation_info_v1 sample{};
        sample.struct_size = sizeof(sample);
        if (runtime.observations_.subscription_info(id.call_id, now, sample) == AYNI_OK) {
            if (!sample.usable ||
                !runtime.subscription_usable(index, now, runtime.decision_->durable_seq)) {
                // Retain diagnostics, never expose an expired/closed value as usable data.
                sample.usable = 0;
                sample.value = 0;
            }
            sample.media = {}; // AP-05 does not grant media access.
            if (sample.usable &&
                !runtime.retain_sample(runtime.decision_->task_index, sample.reference, now,
                                       runtime.decision_->durable_seq)) {
                return AYNI_CAPACITY_EXCEEDED;
            }
            result.sample = sample;
        }
    }
    *out = result;
    return AYNI_OK;
}

// Validate/copy a sole control before any domain state or output is consumed.
ayni_status_t CallRuntime::stage_operation_control(void *context,
                                                   const ayni_operation_control_v1 *control) {
    if (!context || !control || control->struct_size != sizeof(*control) || control->reserved ||
        control->kind < AYNI_OPERATION_WAIT || control->kind > AYNI_OPERATION_CLOSE) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto &runtime = *static_cast<CallRuntime *>(context);
    uint32_t index = 0;
    auto status = runtime.operation_index(control->reference, index);
    if (status != AYNI_OK) {
        return status;
    }
    if (runtime.proposal_) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    const auto basis = runtime.operation_basis(index);
    if (basis.generation != control->reference.operation_id) {
        return AYNI_STALE;
    }
    Proposal proposal{};
    if (!proposal.step.assign(control->step_key)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (control->kind == AYNI_OPERATION_WAIT) {
        work::OperationWait wait{};
        status = check_wait_control(basis, *control,
                                    runtime.host_.monotonic_now_us(runtime.host_.ctx), wait);
        if (status != AYNI_OK) {
            return status;
        }
        proposal.wait_basis = wait;
    } else {
        const ayni_operation_frontier_v1 empty{};
        if (control->timeout_ms || !ayni_operation_frontier_equal(&empty, &control->after)) {
            return AYNI_INVALID_ARGUMENT;
        }
        if ((control->kind == AYNI_OPERATION_CLOSE) != (basis.form == AYNI_CALL_SUBSCRIPTION)) {
            return AYNI_UNSUPPORTED;
        }
        if (control->kind == AYNI_OPERATION_CANCEL &&
            runtime.work_.calls.contract(index).cancel == catalog::CancelMode::none) {
            return AYNI_UNSUPPORTED;
        }
        const auto &subscription = runtime.work_.calls.state(index).subscription;
        const bool closed =
            subscription && subscription->desired == work::SubscriptionDesired::closed;
        if (control->kind == AYNI_OPERATION_CLOSE ? closed
                                                  : basis.outcome || basis.cancel_requested) {
            return AYNI_STALE; // Read-back is idempotent; sealed Call rows remain immutable.
        }
    }
    proposal.task_id = control->reference.task_id;
    proposal.operation_index = index;
    proposal.control_kind = control->kind;
    proposal.kind = AYNI_POLICY_CONTROL_OPERATION;
    runtime.stage_intent(proposal);
    return AYNI_OK;
}
} // namespace ayni::kernel
