#include "foundation/deadline.h"
#include "kernel/service/call/call_runtime.h"
#include "kernel/service/memory/memory_access.h"
namespace ayni::kernel {
// Both initial dispatch and renewals use exclusive saturating deadlines;
// an unrepresentable finite lease is refused rather than becoming unlimited.
CallRuntime::LeaseRequest CallRuntime::plan_subscription_lease(uint64_t now_us, uint32_t lease_ms,
                                                               uint64_t sequence) {
    const uint64_t duration = uint64_t{lease_ms} * microseconds_per_millisecond;
    constexpr uint64_t half = 2;
    return {sequence, foundation::deadline(now_us, duration),
            foundation::deadline(now_us, duration / half)};
}
// Durable dispatch already selected this operation. The sink stays closed until
// STARTED is applied, even if the Provider posts synchronously within submit.
bool CallRuntime::prepare_subscription_delivery(uint32_t index, uint64_t now_us,
                                                ayni_subscription_open_v1 &out) {
    const auto &id = work_.calls.identity(index);
    const auto &state = work_.calls.state(index);
    const auto &spec = *work_.calls.contract(index).subscription;
    auto &delivery = *slots_[index].active->delivery;
    const auto plan = plan_subscription_lease(now_us, spec.lease_ms, 1);
    uint32_t source = 0;
    if (plan.requested_until_us == UINT64_MAX ||
        !catalogs_.observation_sources.find({spec.source.data(), spec.source.size()}, source)) {
        return false;
    }
    const ayni_subscription_ref_v1 reference{id.call_id, work_.ids.boot_epoch(),
                                             state.subscription->generation};
    auto *sink = subscription_ingress_.prepare(subscription_ingress_.context, index, reference,
                                               source, id.owner_instance_id);
    if (!sink || !mailbox_.expect_subscription_control(
                     delivery.reservation, ProviderMessageKind::subscription_started, 1)) {
        return false;
    }
    observations_.begin_subscription(index, source, id.task_id, id.call_id, reference.generation);
    slots_[index].active->collection =
        CollectionLease{plan.requested_until_us, plan.renew_at_us, {}, plan};
    if (delivery.deadline_us > plan.requested_until_us) {
        delivery.deadline_us = plan.requested_until_us;
    }
    out = {sizeof(out),          spec.source_version,     reference, spec.source.snapshot(),
           id.owner_instance_id, plan.requested_until_us, sink};
    return true;
}
uint64_t CallRuntime::collection_stop_deadline(const CollectionLease &lease) {
    return lease.pending && lease.pending->requested_until_us > lease.valid_until_us
               ? lease.pending->requested_until_us
               : lease.valid_until_us;
}
// Terminal delivery ends the local borrow, but UNKNOWN does not prove that a
// remote collection already stopped. Retain exactly its occupied resource units.
void CallRuntime::await_subscription_stop(uint32_t index) {
    auto &active = *slots_[index].active;
    close_subscription_gate(index);
    active.stage = Stage::lease_stop;
    active.delivery.reset();
    active.collection->inhibition = work::CallCancelCause::authority;
    active.collection->renew_at_us = UINT64_MAX;
}
bool CallRuntime::subscription_expiry_ready(uint64_t now_us) const {
    for (uint32_t index = 0; index < limits_.capacity; ++index) {
        const auto &active = slots_[index].active;
        if (active && active->stage == Stage::lease_stop &&
            now_us >= collection_stop_deadline(*active->collection)) {
            return true;
        }
    }
    return false;
}
CallRuntime::Advance CallRuntime::release_expired_subscriptions(uint64_t now_us) {
    for (uint32_t index = 0; index < limits_.capacity; ++index) {
        const auto &active = slots_[index].active;
        if (active && active->stage == Stage::lease_stop &&
            now_us >= collection_stop_deadline(*active->collection)) {
            release(index, now_us);
            return Advance::applied;
        }
    }
    return Advance::idle;
}
bool CallRuntime::subscription_stop_pending(uint32_t index) const {
    const auto &active = slots_[index].active;
    return work_.calls.occupied(index) && active && active->delivery && active->collection &&
           active->collection->inhibition &&
           work_.calls.state(index).cancel_cause == work::CallCancelCause::none;
}
bool CallRuntime::subscription_cancel_ready() const {
    for (uint32_t index = 0; index < limits_.capacity; ++index) {
        const auto &active = slots_[index].active;
        if (active && active->delivery && active->collection && active->collection->inhibition &&
            active->delivery->cancel == Delivery::Cancel::none) {
            return true;
        }
    }
    return false;
}
bool CallRuntime::subscription_renew_ready(uint32_t index, uint64_t now_us,
                                           uint64_t durable_seq) const {
    const auto &active = slots_[index].active;
    if (!active || !active->delivery || !active->collection || !work_.calls.occupied(index)) {
        return false;
    }
    const auto &lease = *active->collection;
    const auto &fact = work_.calls.state(index).subscription;
    return fact && work_.calls.state(index).latest.transition.commit_seq <= durable_seq &&
           fact->phase == work::SubscriptionPhase::active && !lease.inhibition && !lease.pending &&
           active->delivery->cancel == Delivery::Cancel::none && now_us >= lease.renew_at_us;
}
// Refusal bookkeeping closes all gates separately; here each Provider close is
// one budgeted item. During journal pressure the volatile inhibition survives
// until its one Call row can be persisted before any terminal is acknowledged.
CallRuntime::Advance CallRuntime::advance_subscription_control(uint64_t now_us,
                                                               uint64_t durable_seq, Mode mode,
                                                               JournalAdmission admission) {
    for (uint32_t index = 0; index < limits_.capacity; ++index) {
        auto &active = slots_[index].active;
        if (!active || !active->delivery || !active->collection ||
            !active->collection->inhibition || active->delivery->cancel != Delivery::Cancel::none) {
            continue;
        }
        if (admission == JournalAdmission::available && mode != Mode::fenced &&
            mode != Mode::closing &&
            work_.calls.state(index).cancel_cause == work::CallCancelCause::none) {
            return commit_subscription_stop(index);
        }
        return cancel_transport(*active->delivery);
    }
    if (admission == JournalAdmission::available && mode == Mode::running) {
        const auto blocked = blocked_receipts();
        if (mailbox_.has_pending(&blocked)) {
            return Advance::idle;
        }
        for (uint32_t index = 0; index < limits_.capacity; ++index) {
            if (subscription_renew_ready(index, now_us, durable_seq)) {
                return renew_subscription(index, now_us, durable_seq);
            }
        }
    }
    return Advance::idle;
}
CallRuntime::Advance CallRuntime::commit_subscription_stop(uint32_t index) {
    const auto cause = *slots_[index].active->collection->inhibition;
    uint32_t task_index = 0;
    if (!work_.tasks.find(work_.calls.identity(index).task_id, task_index)) {
        return Advance::internal;
    }
    const auto &task = work_.tasks.at(task_index);
    const bool preserve =
        work::TaskTable::is_live(task.lifecycle) && task.call.end == work::TaskCall::End::none &&
        cause != work::CallCancelCause::deadline && !work_.calls.state(index).content_erased;
    const auto desired =
        preserve ? work::SubscriptionDesired::open : work::SubscriptionDesired::closed;
    return work_.calls.stop_subscription(
               index, cause, desired, {work_.ids.boot_epoch(), work_.transitions.next()}) == AYNI_OK
               ? Advance::applied
               : Advance::internal;
}
// An ACK confirms only the requested absolute deadline. Late or inhibited ACKs
// return their reliable lane without reopening either cached evidence or sink.
CallRuntime::Advance CallRuntime::consume_subscription_control(uint32_t index, uint32_t task_index,
                                                               const ProviderMessage &message,
                                                               const ProviderReceipt &receipt,
                                                               uint64_t now_us, uint64_t) {
    auto &delivery = *slots_[index].active->delivery;
    const auto &state = work_.calls.state(index);
    if (!state.subscription || !slots_[index].active->collection) {
        return Advance::internal;
    }
    auto &lease = *slots_[index].active->collection;
    if (!lease.inhibition && now_us >= delivery.deadline_us) {
        lease.inhibition = work::CallCancelCause::deadline;
        close_subscription_gate(index);
    }
    // Preserve the stop as domain state before returning this independent ACK lane.
    if (subscription_stop_pending(index)) {
        return commit_subscription_stop(index);
    }
    if (!lease.inhibition && state.cancel_cause == work::CallCancelCause::none) {
        const bool valid =
            lease.pending && lease.pending->sequence == message.sequence &&
            message.status == AYNI_OK && message.reason == AYNI_REASON_NONE &&
            message.query_result == 0 &&
            (message.effect == AYNI_CALL_EFFECT_NONE ||
             message.effect == AYNI_CALL_EFFECT_PARTIAL) &&
            (message.kind == ProviderMessageKind::subscription_started) == (message.sequence == 1);
        const auto stamp = work_.transitions.next();
        if (!valid) {
            if (work_.calls.fail_subscription(index, AYNI_RESULT_UNKNOWN,
                                              AYNI_REASON_PROVIDER_RESULT,
                                              {work_.ids.boot_epoch(), stamp}) != AYNI_OK) {
                return Advance::internal;
            }
            lease.inhibition = work::CallCancelCause::authority;
            close_subscription_gate(index);
        } else {
            const auto attachment = state.subscription->attachment;
            const auto plan = *lease.pending;
            if (work_.calls.confirm_subscription(index, message.sequence,
                                                 {work_.ids.boot_epoch(), stamp}) != AYNI_OK) {
                return Advance::internal;
            }
            const auto &id = work_.calls.identity(index);
            const ayni_string_v1 step{id.step_key.data(), id.step_key.size()};
            if (!work_.tasks.has_step(task_index, step) &&
                !work_.tasks.add_step(task_index, step, stamp)) {
                return Advance::internal;
            }
            if (attachment == work::SubscriptionAttachment::foreground) {
                if (work_.tasks.at(task_index).call.id != id.call_id) {
                    return Advance::internal;
                }
                work_.tasks.finish_call(task_index, stamp);
            }
            lease.valid_until_us = plan.requested_until_us;
            lease.renew_at_us = plan.renew_at_us;
            lease.pending.reset();
            delivery.deadline_us = lease.valid_until_us;
            const ayni_subscription_ref_v1 reference{id.call_id, work_.ids.boot_epoch(),
                                                     state.subscription->generation};
            if (!subscription_ingress_.activate(subscription_ingress_.context, index, reference,
                                                lease.valid_until_us)) {
                return Advance::internal;
            }
            observations_.activate_subscription(index, reference.generation, lease.valid_until_us);
        }
    }
    return mailbox_.acknowledge(receipt) == ProviderMailbox::Acknowledge::stale ? Advance::internal
                                                                                : Advance::applied;
}
// Original route/fields and current authority are rechecked before extending I/O.
// A rejected/unknown renewal never silently retries a new lease control sequence.
CallRuntime::Advance CallRuntime::renew_subscription(uint32_t index, uint64_t now_us,
                                                     uint64_t durable_seq) {
    const auto &id = work_.calls.identity(index);
    const auto &contract = work_.calls.contract(index);
    auto &delivery = *slots_[index].active->delivery;
    auto &lease = *slots_[index].active->collection;
    uint32_t task_index = 0, binding_index = 0;
    work::CallInputView input{};
    if (!work_.tasks.find(id.task_id, task_index) ||
        !catalogs_.bindings.find({id.binding_id.data(), id.binding_id.size()}, id.binding_version,
                                 binding_index) ||
        work::read_call_input(work_.calls.domain(index), work_.calls.input(index), contract,
                              input) != AYNI_OK) {
        return Advance::internal;
    }
    const auto authority =
        check_input(task_index, catalogs_.bindings.at(binding_index), work_.calls.domain(index),
                    input, contract, host_.monotonic_now_us(host_.ctx), durable_seq);
    if (authority.status != AYNI_OK || !resources_.executable(slots_[index].active->claim)) {
        lease.inhibition = work::CallCancelCause::authority;
        close_subscription_gate(index);
        return commit_subscription_stop(index);
    }
    const auto sequence = work_.calls.state(index).subscription->lease_sequence;
    if (sequence == UINT64_MAX) {
        lease.inhibition = work::CallCancelCause::deadline;
        close_subscription_gate(index);
        return commit_subscription_stop(index);
    }
    const auto plan =
        plan_subscription_lease(now_us, contract.subscription->lease_ms, sequence + 1);
    if (plan.requested_until_us == UINT64_MAX ||
        !mailbox_.expect_subscription_control(
            delivery.reservation, ProviderMessageKind::subscription_renewed, plan.sequence)) {
        return Advance::internal;
    }
    lease.pending = plan;
    lease.renew_at_us = UINT64_MAX;
    // A pending extension never extends the ACK wait beyond the confirmed lease.
    // Its requested deadline is nevertheless retained for UNKNOWN resource isolation.
    const auto ack_until =
        foundation::deadline(now_us, uint64_t{contract.timeout_ms} * microseconds_per_millisecond);
    delivery.deadline_us = ack_until < lease.valid_until_us ? ack_until : lease.valid_until_us;
    const ayni_subscription_renew_v1 request{
        sizeof(request),
        0,
        {id.call_id, work_.ids.boot_epoch(), work_.calls.state(index).subscription->generation},
        plan.sequence,
        plan.requested_until_us};
    const auto submitted = work_.call_out(
        [&] { return delivery.provider->renew(delivery.provider->context, &request); });
    if (submitted == AYNI_SUBMIT_ACCEPTED || submitted == AYNI_SUBMIT_ACCEPT_UNKNOWN) {
        return Advance::applied;
    }
    lease.inhibition = work::CallCancelCause::authority;
    close_subscription_gate(index);
    const auto status =
        submitted == AYNI_SUBMIT_REJECTED_NOT_RETAINED ? AYNI_NOT_READY : AYNI_RESULT_UNKNOWN;
    const auto reason =
        status == AYNI_RESULT_UNKNOWN ? AYNI_REASON_PROVIDER_RESULT : AYNI_REASON_NONE;
    return work_.calls.fail_subscription(
               index, status, reason, {work_.ids.boot_epoch(), work_.transitions.next()}) == AYNI_OK
               ? Advance::applied
               : Advance::internal;
}
// Only known stopped, preserved intent may reenter admission. UNKNOWN requires
// explicit owner reconciliation and never satisfies this predicate (C-CALL-008).
bool CallRuntime::subscription_reopen_ready(uint32_t index, uint64_t now_us, uint64_t durable_seq,
                                            Mode mode) const {
    const auto &state = work_.calls.state(index);
    if (mode != Mode::running || slots_[index].active || !state.subscription ||
        state.subscription->desired != work::SubscriptionDesired::open ||
        state.subscription->phase != work::SubscriptionPhase::stopped ||
        state.outcome != AYNI_CALL_CANCELLED || state.content_erased || !resources_.has_room() ||
        state.latest.transition.commit_seq > durable_seq) {
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
// Reopening preserves the original intent/input, never an uncertain old command.
// The new operation is journaled before resources can establish a new barrier.
CallRuntime::Advance CallRuntime::reopen_subscription(uint32_t index, uint32_t task_index,
                                                      uint64_t now_us, uint64_t durable_seq,
                                                      Mode mode) {
    if (!subscription_reopen_ready(index, now_us, durable_seq, mode)) {
        return Advance::idle;
    }
    catalog::BindingLinks links{};
    if (!compatible_call(index, links)) {
        work_.tasks.hold_restored(task_index, AYNI_UNSUPPORTED_VERSION,
                                  AYNI_REASON_INTENT_UNSUPPORTED);
        work_.forget_due_work();
        return Advance::applied;
    }
    uint64_t generation = 0;
    if (!work_.ids.allocate(generation)) {
        return Advance::attempt_exhausted;
    }
    const auto stamp = work_.transitions.next();
    const auto &id = work_.calls.identity(index);
    if (work_.calls.restart_subscription(index, generation, {work_.ids.boot_epoch(), stamp}) !=
            AYNI_OK ||
        resources_.enqueue(
            resource_request(id.call_id, id.owner_instance_id, links, work_.calls.contract(index),
                             work_.goals.at(work_.tasks.at(task_index).goal_index).work_profile),
            now_us) != AYNI_OK) {
        return Advance::internal;
    }
    slots_[index].active = Active{id.call_id, Stage::queued, {}, std::nullopt};
    return Advance::applied;
}
} // namespace ayni::kernel
