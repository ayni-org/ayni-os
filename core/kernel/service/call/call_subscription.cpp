#include "catalog/abi_checks.h"
#include "kernel/service/call/call_runtime.h"
#include "kernel/service/memory/memory_access.h"
namespace ayni::kernel {
// Transport gates are only a locked projection. Current authority and cached
// evidence are always decided from their owner before business consumption.
bool CallRuntime::subscription_usable(uint32_t index, uint64_t now_us, uint64_t durable_seq) const {
    if (!work_.calls.occupied(index)) {
        return false;
    }
    const auto &state = work_.calls.state(index);
    const auto &active = slots_[index].active;
    if (!state.subscription || state.outcome != 0 ||
        state.subscription->phase != work::SubscriptionPhase::active ||
        state.subscription->desired != work::SubscriptionDesired::open ||
        state.cancel_cause != work::CallCancelCause::none || !active || !active->delivery ||
        !active->collection || active->collection->inhibition ||
        active->delivery->cancel != Delivery::Cancel::none ||
        now_us >= active->collection->valid_until_us ||
        state.attempt.boot_epoch != work_.ids.boot_epoch()) {
        return false;
    }
    uint32_t task_index = 0;
    if (!work_.tasks.find(work_.calls.identity(index).task_id, task_index)) {
        return false;
    }
    const auto &task = work_.tasks.at(task_index);
    return work::TaskTable::is_live(task.lifecycle) && task.lifecycle != AYNI_TASK_PAUSED &&
           task.call.end == work::TaskCall::End::none && !work::TaskTable::is_held(task) &&
           work_.check_task_authority(task_index, now_us, durable_seq).status == AYNI_OK &&
           task_content_current({work_, now_us, durable_seq}, task_index);
}
// The durable fact still belongs to CallTable; these two projections exist
// because worker admission and cached evidence have independent readers.
void CallRuntime::close_subscription_gate(uint32_t index) {
    if (!work_.calls.occupied(index) ||
        (slots_[index].active &&
         slots_[index].active->call_id != work_.calls.identity(index).call_id)) {
        return;
    }
    const auto &state = work_.calls.state(index);
    if (!state.subscription) {
        return;
    }
    const ayni_subscription_ref_v1 reference{
        work_.calls.identity(index).call_id,
        state.attempt.boot_epoch == 0 ? work_.ids.boot_epoch() : state.attempt.boot_epoch,
        state.subscription->generation};
    subscription_ingress_.close(subscription_ingress_.context, index, reference);
    observations_.close_subscription(index);
}
// Every affected scope closes before any physical dispatch, regardless of Call
// scan order. Provider close is a separate budgeted item; inhibition cannot be
// undone by a late ACK, temporary authority recovery or journal backpressure.
bool CallRuntime::refresh_live_gates(uint64_t now_us, uint64_t durable_seq, Mode mode) {
    for (uint32_t index = 0; index < limits_.capacity; ++index) {
        auto &active = slots_[index].active;
        if (!active || !active->delivery || !work_.calls.occupied(index)) {
            continue;
        }
        uint32_t task_index = 0;
        if (!work_.tasks.find(work_.calls.identity(index).task_id, task_index)) {
            continue;
        }
        const auto &task = work_.tasks.at(task_index);
        if (!active->collection && submitted(active->stage) &&
            active->delivery->purpose == Delivery::Purpose::execution &&
            (task.lifecycle == AYNI_TASK_PAUSED || task.call.end != work::TaskCall::End::none)) {
            active->delivery->inhibition = work::CallCancelCause::user;
        }
        if (!active->collection) {
            continue;
        }
        auto &collection = *active->collection;
        if (collection.inhibition) {
            continue;
        }
        const auto &state = work_.calls.state(index);
        const auto authority = work_.check_task_authority(task_index, now_us, durable_seq);
        if (mode != Mode::running) {
            collection.inhibition = work::CallCancelCause::shutdown;
        } else if (!work::TaskTable::is_live(task.lifecycle) ||
                   task.lifecycle == AYNI_TASK_PAUSED ||
                   task.call.end != work::TaskCall::End::none) {
            collection.inhibition = work::CallCancelCause::user;
        } else if (state.cancel_cause != work::CallCancelCause::none) {
            collection.inhibition = state.cancel_cause;
        } else if (now_us >= active->delivery->deadline_us) {
            collection.inhibition = work::CallCancelCause::deadline;
        } else if (work::TaskTable::is_held(task) || authority.status != AYNI_OK ||
                   !resources_.executable(active->claim) ||
                   !task_content_current({work_, now_us, durable_seq}, task_index)) {
            collection.inhibition = work::CallCancelCause::authority;
        }
        if (collection.inhibition) {
            collection.renew_at_us = UINT64_MAX;
            close_subscription_gate(index);
        }
    }
    return control_.latch_closed_gates(now_us, durable_seq, mode);
}
ayni_status_t CallRuntime::subscription_info(uint32_t index, uint64_t now_us, uint64_t durable_seq,
                                             ayni_subscription_info_v1 &out) const {
    const auto &state = work_.calls.state(index);
    if (!state.subscription) {
        return AYNI_NOT_FOUND;
    }
    const auto &id = work_.calls.identity(index);
    const auto &fact = *state.subscription;
    auto phase = static_cast<ayni_subscription_phase_t>(fact.phase);
    const auto &active = slots_[index].active;
    const auto until =
        active && active->collection
            ? (active->stage == Stage::lease_stop ? collection_stop_deadline(*active->collection)
                                                  : active->collection->valid_until_us)
            : 0;
    if (phase == AYNI_SUBSCRIPTION_ACTIVE && !subscription_usable(index, now_us, durable_seq)) {
        phase =
            until != 0 && now_us >= until ? AYNI_SUBSCRIPTION_EXPIRED : AYNI_SUBSCRIPTION_STOPPING;
    }
    out = {sizeof(out),
           phase,
           {id.call_id,
            state.attempt.boot_epoch == 0 ? work_.ids.boot_epoch() : state.attempt.boot_epoch,
            fact.generation},
           id.task_id,
           id.owner_instance_id,
           fact.lease_sequence,
           until,
           fact.control_status != AYNI_OK       ? fact.control_status
           : phase == AYNI_SUBSCRIPTION_EXPIRED ? AYNI_EXPIRED
                                                : state.status,
           fact.control_status != AYNI_OK ? fact.control_reason : state.reason};
    return AYNI_OK;
}
ayni_status_t CallRuntime::subscription_step(ayni_string_v1 step, uint32_t &index) const {
    if (!decision_ || !work_.calling_out()) {
        return AYNI_INVALID_STATE;
    }
    const auto now = host_.monotonic_now_us(host_.ctx);
    if (!task_content_current({work_, now, decision_->durable_seq}, decision_->task_index)) {
        return AYNI_EXPIRED;
    }
    return work_.calls.find_step(work_.tasks.at(decision_->task_index).task_id, step, index) &&
                   work_.calls.state(index).subscription
               ? AYNI_OK
               : AYNI_NOT_FOUND;
}
ayni_status_t CallRuntime::read_subscription(const void *context, ayni_string_v1 step,
                                             ayni_subscription_info_v1 *out) {
    if (!context || !out) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto shape = catalog::check_struct_size(out->struct_size, sizeof(*out), sizeof(*out));
    if (shape != AYNI_OK) {
        return shape;
    }
    const auto &runtime = *static_cast<const CallRuntime *>(context);
    uint32_t index = 0;
    const auto status = runtime.subscription_step(step, index);
    return status == AYNI_OK
               ? runtime.subscription_info(index, runtime.host_.monotonic_now_us(runtime.host_.ctx),
                                           runtime.decision_->durable_seq, *out)
               : status;
}
ayni_status_t CallRuntime::read_subscription_observation(const void *context, ayni_string_v1 step,
                                                         ayni_observation_info_v1 *out) {
    if (!context || !out) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto shape = catalog::check_struct_size(out->struct_size, sizeof(*out), sizeof(*out));
    if (shape != AYNI_OK) {
        return shape;
    }
    const auto &runtime = *static_cast<const CallRuntime *>(context);
    uint32_t index = 0;
    const auto status = runtime.subscription_step(step, index);
    if (status != AYNI_OK) {
        return status;
    }
    const auto now = runtime.host_.monotonic_now_us(runtime.host_.ctx);
    const auto read = runtime.observations_.subscription_info(
        runtime.work_.calls.identity(index).call_id, now, *out);
    if (read == AYNI_OK &&
        !runtime.subscription_usable(index, now, runtime.decision_->durable_seq)) {
        out->usable = 0;
    }
    return read;
}
// The sequence survives cache expiry, so expiry alone never fabricates new input.
// A new generation with no sample uses zero; its identity already changes the frontier.
work::SubscriptionWait CallRuntime::subscription_wait_basis(uint32_t index) const {
    const auto &fact = *work_.calls.state(index).subscription;
    work::SubscriptionWait basis{work_.calls.identity(index).call_id, fact.generation, 0,
                                 fact.phase};
    ayni_observation_info_v1 info{};
    info.struct_size = sizeof(info);
    if (observations_.subscription_info(basis.subscription_id, host_.monotonic_now_us(host_.ctx),
                                        info) == AYNI_OK &&
        info.reference.generation == basis.generation) {
        basis.sequence = info.reference.sequence;
    }
    return basis;
}
// WAITING keeps authority/privacy holds independent. Renewals do not count as
// new input; a busy consumer sees exactly the latest sample when it next decides.
bool CallRuntime::next_subscription_wakeup(uint64_t now_us, uint64_t durable_seq,
                                           uint32_t &task_index) const {
    for (uint32_t at = 0; at < work_.tasks.size(); ++at) {
        const auto &task = work_.tasks.at(at);
        if (task.lifecycle != AYNI_TASK_WAITING || !task.subscription_wait ||
            work::TaskTable::is_held(task) ||
            work_.check_task_authority(at, now_us, durable_seq).status != AYNI_OK ||
            !task_content_current({work_, now_us, durable_seq}, at)) {
            continue;
        }
        uint32_t call_index = 0;
        if (work_.calls.find(task.subscription_wait->subscription_id, call_index) &&
            subscription_wait_basis(call_index) != *task.subscription_wait) {
            task_index = at;
            return true;
        }
    }
    return false;
}
} // namespace ayni::kernel
