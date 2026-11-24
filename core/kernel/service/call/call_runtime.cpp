#include "kernel/service/call/call_runtime.h"

#include "kernel/service/memory/memory_access.h"
#include "work/call_propagation.h"

#include <new>

namespace ayni::kernel {

std::size_t CallRuntime::storage_size(Limits limits) {
    return limits.capacity == 0U
               ? 0U
               : sizeof(Slot) * limits.capacity + limits.input_bytes + limits.result_bytes * 2U;
}

CallRuntime::CallRuntime(void *storage, Limits limits, WorkModel &work, const Catalogs &catalogs,
                         resources::ResourceAdmission &resources, resources::PermitTable &permits,
                         knowledge::ObservationTable &observations,
                         SubscriptionIngressPort subscription_ingress, ProviderMailbox &mailbox,
                         ExecutorMailbox &executors, AttemptSequence &attempts,
                         const ayni_host_v1 &host, AuditRing &audit)
    : observations_(observations), subscription_ingress_(subscription_ingress), work_(work),
      catalogs_(catalogs), resources_(resources), mailbox_(mailbox), attempts_(attempts),
      host_(host), audit_(audit), limits_(limits), slots_(static_cast<Slot *>(storage)),
      staging_(limits.capacity == 0U ? nullptr
                                     : reinterpret_cast<uint8_t *>(slots_ + limits.capacity)),
      receipt_(limits.capacity == 0U ? nullptr : staging_ + limits.input_bytes),
      normalized_(limits.capacity == 0U ? nullptr : receipt_ + limits.result_bytes),
      control_(work, resources, permits, observations, executors, attempts, host, audit,
               {this, transport_holds, transport_retiring, transport_retire}) {
    work_.calls.set_runtime_retention(this, runtime_retains);
    static_assert(alignof(Slot) <= storage_alignment, "arena aligns call runtime slots");
    for (uint32_t at = 0; at < limits.capacity; ++at) {
        new (slots_ + at) Slot{};
    }
}

// Release only this acquisition/waiter. Non-actuator calls need no independent
// physical stop proof; issued actuator permits retain their exact quarantine.
void CallRuntime::release(uint32_t index, uint64_t now_us) {
    auto &active = slots_[index].active;
    if (!active) {
        return;
    }
    if (active->stage != Stage::lease_stop) {
        close_subscription_gate(index);
    }
    const bool uncertain =
        active->collection &&
        (active->stage == Stage::lease_stop ||
         (work_.calls.occupied(index) && work_.calls.state(index).outcome == AYNI_CALL_ABANDONED));
    if (uncertain && now_us < collection_stop_deadline(*active->collection)) {
        await_subscription_stop(index);
        return;
    }
    if (control_.holds_lease(active->call_id)) {
        if (work_.calls.occupied(index) && work_.calls.state(index).ephemeral &&
            control_.release_ephemeral_stopped(active->call_id)) {
            // Confirmed ephemeral physical stop lifts RAM quarantine immediately (ADR-0110).
            active.reset();
            if (work_.calls.state(index).reflex && work_.calls.state(index).outcome != 0U) {
                static_cast<void>(work_.calls.retire_ephemeral(index));
            }
            work_.forget_due_work();
            return;
        }
        active->delivery.reset();
        active->stage = Stage::retiring; // Stop fact must become durable before the exact
                                         // quarantine is released.
        return;
    }
    control_.abandon_issue(active->call_id);
    if (active->claim.call_id != 0U) {
        static_cast<void>(resources_.release(active->claim));
        // Without an issued lease no Provider received a permit. This exact local proof
        // permits cleanup of a pre-dispatch actuator claim (ADR-0050).
        resources_.confirm_all_stopped(active->claim);
    } else {
        static_cast<void>(resources_.remove_waiter(active->call_id, now_us));
    }
    active.reset();
    if (work_.calls.occupied(index) && work_.calls.state(index).reflex &&
        work_.calls.state(index).outcome != 0U) {
        static_cast<void>(work_.calls.retire_ephemeral(index));
    }
    // The task table reads this slot (runtime_retains) without a transition.
    work_.forget_due_work();
}

bool CallRuntime::idle() const {
    if (mailbox_.in_flight() != 0U || !control_.idle()) {
        return false;
    }
    for (uint32_t at = 0; at < limits_.capacity; ++at) {
        if (slots_[at].active && slots_[at].active->stage != Stage::blocked) {
            return false;
        }
    }
    return true;
}

bool CallRuntime::ephemeral_idle() const {
    for (uint32_t at = 0; at < limits_.capacity; ++at) {
        if (slots_[at].active && slots_[at].active->ephemeral) {
            return false;
        }
    }
    return true;
}

bool CallRuntime::privacy_released() const {
    for (uint32_t index = 0; index < limits_.capacity; ++index) {
        if (work_.calls.occupied(index) && work_.calls.state(index).content_erased &&
            slots_[index].active && slots_[index].active->stage != Stage::blocked) {
            return false;
        }
    }
    return true;
}

bool CallRuntime::resolve_call_task(uint32_t call_index, uint32_t &out_task_index) const {
    if (work_.calls.state(call_index).reflex) {
        out_task_index = no_task;
        return true;
    }
    return work_.tasks.find(work_.calls.identity(call_index).task_id, out_task_index);
}

bool CallRuntime::reflex_call_active(uint64_t call_id) const {
    uint32_t index = 0;
    return call_id != 0U && (control_.holds_lease(call_id) || active_index(call_id, index));
}

bool CallRuntime::has_ephemeral_holder(uint32_t resource_index) const {
    std::array<uint64_t, AYNI_RESOURCE_REQUEST_CAPACITY_MAX> holders{};
    const uint32_t total = resources_.holders_of_resource(resource_index, holders.data(),
                                                          static_cast<uint32_t>(holders.size()));
    const uint32_t limit = total < holders.size() ? total : static_cast<uint32_t>(holders.size());
    for (uint32_t at = 0; at < limit; ++at) {
        uint32_t index = 0;
        if (!work_.calls.find(holders[at], index)) {
            continue;
        }
        const auto &state = work_.calls.state(index);
        if (state.ephemeral && state.outcome == 0U &&
            state.cancel_cause == work::CallCancelCause::none &&
            work_.calls.contract(index).cancel != catalog::CancelMode::none) {
            return true;
        }
    }
    return false;
}

// Emits one Call-terminal audit record covering both EPHEMERAL and DURABLE outcomes (ADR-0113).
void CallRuntime::audit_call_terminal(uint32_t index, uint64_t now_us) const {
    if (!work_.calls.occupied(index)) {
        return;
    }
    const auto &id = work_.calls.identity(index);
    const auto &state = work_.calls.state(index);
    uint32_t flags = 0;
    if (state.ephemeral) {
        flags |= AYNI_AUDIT_FLAG_EPHEMERAL;
    }
    if (state.execution.has_value()) {
        flags |= AYNI_AUDIT_FLAG_PHYSICAL;
    }
    if (state.cancel_cause == work::CallCancelCause::preempted ||
        state.cancel_cause == work::CallCancelCause::reflex) {
        flags |= AYNI_AUDIT_FLAG_PREEMPTED;
    }
    AuditEntry entry{};
    entry.kind = AYNI_AUDIT_CALL_TERMINAL;
    entry.boot_epoch = work_.ids.boot_epoch();
    entry.at_us = now_us;
    entry.subject_id = id.call_id;
    entry.auxiliary_id = id.task_id;
    entry.status = state.status;
    entry.reason = state.reason;
    entry.detail_code = state.outcome;
    entry.flags = flags;
    entry.label = {id.capability_id.data(), id.capability_id.size()};
    audit_.emit(entry);
}

void CallRuntime::audit_authority_denied(uint64_t subject_id, uint64_t auxiliary_id,
                                         ayni_status_t status, ayni_reason_t reason,
                                         const catalog::Binding &binding,
                                         const catalog::CallContract &contract,
                                         uint64_t now_us) const {
    uint32_t flags = AYNI_AUDIT_FLAG_DENIED;
    if (contract.is_ephemeral()) {
        flags |= AYNI_AUDIT_FLAG_EPHEMERAL;
    }
    if ((contract.effects & catalog::call_effect_physical) != 0U) {
        flags |= AYNI_AUDIT_FLAG_PHYSICAL;
    }
    AuditEntry entry{};
    entry.kind = AYNI_AUDIT_AUTHORITY_DENIED;
    entry.boot_epoch = work_.ids.boot_epoch();
    entry.at_us = now_us;
    entry.subject_id = subject_id;
    entry.auxiliary_id = auxiliary_id;
    entry.status = status;
    entry.reason = reason;
    entry.flags = flags;
    entry.label = {binding.capability.data(), binding.capability.size()};
    audit_.emit(entry);
}

authority::GrantCheck CallRuntime::audit_proposal_denial(uint32_t task_index,
                                                         const Proposal &proposal,
                                                         authority::GrantCheck check,
                                                         uint64_t now_us) const {
    if (check.status == AYNI_DENIED) {
        const auto &task = work_.tasks.at(task_index);
        audit_authority_denied(task.task_id, task.goal_id, check.status, check.reason,
                               catalogs_.bindings.at(proposal.binding_index),
                               proposal.links.contract, now_us);
    }
    return check;
}

void CallRuntime::audit_preempted(uint64_t victim_call_id, uint64_t preemptor_call_id,
                                  ayni_reason_t reason, bool physical, ayni_string_v1 label,
                                  uint64_t now_us) const {
    AuditEntry entry{};
    entry.kind = AYNI_AUDIT_RESOURCE_PREEMPTED;
    entry.boot_epoch = work_.ids.boot_epoch();
    entry.at_us = now_us;
    entry.subject_id = victim_call_id;
    entry.auxiliary_id = preemptor_call_id;
    entry.status = AYNI_OK;
    entry.reason = reason;
    entry.flags = AYNI_AUDIT_FLAG_EPHEMERAL | AYNI_AUDIT_FLAG_PREEMPTED |
                  (physical ? AYNI_AUDIT_FLAG_PHYSICAL : 0U);
    entry.label = label;
    audit_.emit(entry);
}

// Soft-stops and cancels every eligible uncancelled EPHEMERAL holder on resource_index.
// Tier 2 DURABLE and non-cancellable holders are never preempted (ADR-0111/0112).
ayni_status_t CallRuntime::preempt_resource(uint32_t resource_index, uint64_t now_us,
                                            work::CallCancelCause cause, uint64_t &out_affected) {
    out_affected = 0;
    std::array<uint64_t, AYNI_RESOURCE_REQUEST_CAPACITY_MAX> holders{};
    const uint32_t total = resources_.holders_of_resource(resource_index, holders.data(),
                                                          static_cast<uint32_t>(holders.size()));
    if (total == 0U) {
        return AYNI_NOT_FOUND;
    }
    const uint32_t limit = total < holders.size() ? total : static_cast<uint32_t>(holders.size());
    uint32_t stopped = 0;
    bool saw_protected = false;
    bool saw_stopping = false;
    const ayni_reason_t reason =
        cause == work::CallCancelCause::reflex ? AYNI_REASON_REFLEX : AYNI_REASON_PREEMPTED;
    const auto &res_id = catalogs_.resources.at(resource_index).id;
    const ayni_string_v1 res_label{res_id.data(), res_id.size()};
    for (uint32_t at = 0; at < limit; ++at) {
        uint32_t index = 0;
        uint32_t task_index = no_task;
        if (!work_.calls.find(holders[at], index) || !slots_[index].active ||
            !resolve_call_task(index, task_index)) {
            continue;
        }
        const auto &state = work_.calls.state(index);
        if (!state.ephemeral || work_.calls.contract(index).cancel == catalog::CancelMode::none) {
            saw_protected = true;
            continue;
        }
        if (state.outcome != 0U || state.cancel_cause != work::CallCancelCause::none) {
            saw_stopping = true;
            continue;
        }
        const bool physical = state.execution.has_value();
        auto &active = *slots_[index].active;
        if (!submitted(active.stage)) {
            if (cancel_local(index, task_index, reason) != Advance::applied) {
                return AYNI_INTERNAL;
            }
        } else {
            if (work_.calls.request_cancel(index, cause, {work_.ids.boot_epoch(), {0, 0}}) !=
                AYNI_OK) {
                return AYNI_INTERNAL;
            }
            if (control_.holds_lease(holders[at]) && !control_.request_stop(holders[at])) {
                return AYNI_INTERNAL;
            }
            // The ordinary Call lane sends Provider cancellation as its own budgeted item.
        }
        audit_preempted(holders[at], 0U, reason, physical, res_label, now_us);
        if (out_affected == 0U) {
            out_affected = holders[at];
        }
        ++stopped;
    }
    if (stopped > 0U) {
        return AYNI_OK;
    }
    return saw_protected && !saw_stopping ? AYNI_DENIED : AYNI_NOT_FOUND;
}

CallRuntime::Advance CallRuntime::expire_wait(uint32_t index, uint32_t task_index,
                                              resources::WaitAction action, uint64_t now_us) {
    if (action == resources::WaitAction::wait) {
        return Advance::internal; // An unbounded waiter cannot be an expired queue choice.
    }
    if (work_.calls.state(index).ephemeral) {
        const uint64_t call_id = work_.calls.identity(index).call_id;
        if (action == resources::WaitAction::skip && task_index != no_task) {
            if (work_.calls.skip_before_dispatch(index, {work_.ids.boot_epoch(), {0, 0}}) !=
                AYNI_OK) {
                return Advance::internal;
            }
            work_.tasks.end_volatile_call(task_index, call_id);
            work_.tasks.skip_queued_call(task_index, work_.transitions.next());
        } else {
            // Never set durable waiting_for_resource() for a RAM-only call; report failure
            // in RAM so the strategy or reflex rule observes EXPIRED/RESOURCE_WAIT without drift.
            if (work_.calls.fail_before_dispatch_ephemeral(index, AYNI_EXPIRED,
                                                           AYNI_REASON_RESOURCE_WAIT,
                                                           work_.ids.boot_epoch()) != AYNI_OK) {
                return Advance::internal;
            }
            if (task_index != no_task) {
                work_.tasks.end_volatile_call(task_index, call_id);
            }
        }
        audit_call_terminal(index, now_us);
        release(index, now_us);
        return Advance::applied;
    }
    if (action == resources::WaitAction::pause &&
        work::TaskTable::waiting_for_resource(work_.tasks.at(task_index))) {
        release(index,
                now_us); // One Task overlay covers all queued Calls, without repeat revisions.
        return Advance::applied;
    }
    const auto stamp = work_.transitions.next();
    if (action == resources::WaitAction::skip) {
        if (work::skip_queued_call(work_.calls, work_.tasks, index, task_index,
                                   {work_.ids.boot_epoch(), stamp}) != AYNI_OK) {
            return Advance::internal;
        }
        audit_call_terminal(index, now_us);
    } else {
        work_.tasks.pause_resource_wait(task_index, stamp);
    }
    release(index, now_us);
    return Advance::applied;
}

CallRuntime::Advance CallRuntime::cancel_local(uint32_t index, uint32_t task_index,
                                               ayni_reason_t reason) {
    auto &active = slots_[index].active;
    if (active && active->delivery && !mailbox_.withdraw(active->delivery->reservation)) {
        return Advance::internal; // No submit has occurred; a receipt here violates ownership.
    }
    const uint64_t now_us = host_.monotonic_now_us(host_.ctx);
    if (work_.calls.state(index).ephemeral) {
        if (work_.calls.cancel_before_dispatch(index, {work_.ids.boot_epoch(), {0, 0}}, reason) !=
            AYNI_OK) {
            return Advance::internal;
        }
        if (task_index != no_task) {
            work_.tasks.end_volatile_call(task_index, work_.calls.identity(index).call_id);
        }
        audit_call_terminal(index, now_us);
        release(index, now_us);
        return Advance::applied;
    }
    const auto stamp = work_.transitions.next();
    if (work_.calls.cancel_before_dispatch(index, {work_.ids.boot_epoch(), stamp}, reason) !=
            AYNI_OK ||
        !work::end_task_call(work_.calls, work_.tasks, index, task_index, stamp)) {
        return Advance::internal;
    }
    audit_call_terminal(index, now_us);
    release(index, now_us);
    return Advance::applied;
}

// Evaluates all-or-none preemption eligibility for a queued preempting Call (ADR-0111).
// Returns true iff execution preflight holds, every blocking holder is an eligible EPHEMERAL
// cancellable Call, and at least one of them has not yet been signalled to stop.
bool CallRuntime::collect_preempt_victims(uint32_t waiter_index, uint64_t now_us,
                                          uint64_t durable_seq, uint32_t *out_victims,
                                          uint32_t max_victims, uint32_t &out_count) const {
    out_count = 0;
    const auto &active = slots_[waiter_index].active;
    if (!active || active->stage != Stage::queued || active->preemption == AYNI_CALL_PREEMPT_NONE ||
        !control_.allowed(waiter_index, now_us, durable_seq)) {
        return false;
    }
    struct Check {
        const CallRuntime *self;
        ayni_call_preemption_t preemption;
        uint32_t *out_victims;
        uint32_t max_victims;
        uint32_t *out_count;
        uint64_t waiter_ticket = 0;
        resources::Priority waiter_priority = resources::Priority::background;
        bool eligible = true;
    } check{this, active->preemption, out_victims, max_victims, &out_count};
    bool unavailable = false;
    const uint32_t total = resources_.visit_conflicting_holders(
        active->call_id, unavailable, check.waiter_ticket, check.waiter_priority, &check,
        [](void *ctx, const resources::HeldConflict &holder) {
            auto &c = *static_cast<Check *>(ctx);
            // Priority enum orders foreground < normal < background; strictly greater means lower.
            const bool priority_ok =
                c.preemption == AYNI_CALL_PREEMPT_LOWER
                    ? holder.priority > c.waiter_priority
                    : (holder.priority >= c.waiter_priority && holder.ticket < c.waiter_ticket);
            uint32_t victim_index = 0;
            uint32_t victim_task = no_task;
            if (!priority_ok || !c.self->work_.calls.find(holder.call_id, victim_index) ||
                !c.self->slots_[victim_index].active ||
                !c.self->resolve_call_task(victim_index, victim_task)) {
                c.eligible = false;
                return false;
            }
            const auto &victim_state = c.self->work_.calls.state(victim_index);
            const auto &victim_contract = c.self->work_.calls.contract(victim_index);
            if (!victim_state.ephemeral || victim_state.outcome != 0U ||
                victim_contract.cancel == catalog::CancelMode::none) {
                // Never preempt Tier 2 DURABLE or non-cancellable holders (ADR-0111).
                c.eligible = false;
                return false;
            }
            if (victim_state.cancel_cause == work::CallCancelCause::none) {
                if (c.out_victims != nullptr) {
                    if (*c.out_count >= c.max_victims) {
                        c.eligible = false;
                        return false;
                    }
                    c.out_victims[*c.out_count] = victim_index;
                }
                ++(*c.out_count);
            }
            return true;
        });
    return !unavailable && total > 0U && check.eligible && out_count > 0U;
}

bool CallRuntime::preemption_ready(uint32_t waiter_index, uint64_t now_us,
                                   uint64_t durable_seq) const {
    uint32_t count = 0;
    return collect_preempt_victims(waiter_index, now_us, durable_seq, nullptr, 0U, count);
}

// Marks every uncancelled eligible EPHEMERAL holder blocking waiter_index for stop/cancel.
// Physical holders also enter PermitLease::Phase::stopping so ExecutionControl::advance_urgent()
// dispatches AYNI_EXECUTOR_STOP immediately on the urgent lane (ADR-0111).
CallRuntime::Advance CallRuntime::trigger_preemption(uint32_t waiter_index, uint64_t now_us,
                                                     uint64_t durable_seq) {
    std::array<uint32_t, AYNI_RESOURCE_REQUEST_CAPACITY_MAX> victims{};
    uint32_t count = 0;
    if (!collect_preempt_victims(waiter_index, now_us, durable_seq, victims.data(),
                                 static_cast<uint32_t>(victims.size()), count)) {
        return Advance::idle;
    }
    for (uint32_t at = 0; at < count; ++at) {
        const uint32_t victim_index = victims[at];
        uint32_t victim_task = no_task;
        if (!slots_[victim_index].active || !resolve_call_task(victim_index, victim_task)) {
            return Advance::internal;
        }
        auto &victim_active = *slots_[victim_index].active;
        const bool by_reflex = work_.calls.state(waiter_index).reflex;
        const auto cancel_cause =
            by_reflex ? work::CallCancelCause::reflex : work::CallCancelCause::preempted;
        const auto cancel_reason = by_reflex ? AYNI_REASON_REFLEX : AYNI_REASON_PREEMPTED;
        const uint64_t victim_call_id = work_.calls.identity(victim_index).call_id;
        const uint64_t waiter_call_id = work_.calls.identity(waiter_index).call_id;
        const auto victim_cap = work_.calls.identity(victim_index).capability_id;
        const bool physical = work_.calls.state(victim_index).execution.has_value();
        if (!submitted(victim_active.stage)) {
            if (cancel_local(victim_index, victim_task, cancel_reason) != Advance::applied) {
                return Advance::internal;
            }
            audit_preempted(victim_call_id, waiter_call_id, cancel_reason, physical,
                            {victim_cap.data(), victim_cap.size()}, now_us);
            continue;
        }
        const auto status = work_.calls.request_cancel(victim_index, cancel_cause,
                                                       {work_.ids.boot_epoch(), {0, 0}});
        if (status != AYNI_OK) {
            return Advance::internal;
        }
        if (control_.holds_lease(victim_call_id) && !control_.request_stop(victim_call_id)) {
            return Advance::internal;
        }
        // The ordinary Call lane sends Provider cancellation as its own budgeted item.
        audit_preempted(victim_call_id, waiter_call_id, cancel_reason, physical,
                        {victim_cap.data(), victim_cap.size()}, now_us);
    }
    return Advance::applied;
}

// Receipt consumption has priority over another dispatch. Every call step is
// one item; no loop waits for a resource, a Provider or a durable acknowledgement.
void CallRuntime::release_body(const ayni_body_ref_v1 &ref) const {
    if (host_.bodies != nullptr && host_.bodies->release != nullptr) {
        host_.bodies->release(host_.bodies->context, &ref);
    }
}

bool CallRuntime::model_input_due(uint32_t index, uint64_t durable_seq) const {
    const auto &state = work_.calls.state(index);
    return host_.bodies && host_.bodies->release_temporary && state.outcome != 0U &&
           work_.calls.contract(index).category == catalog::CallClass::model &&
           state.latest.transition.commit_seq <= durable_seq &&
           slots_[index].expired_model_input != work_.calls.identity(index).call_id;
}
void CallRuntime::release_model_input(uint32_t index) {
    std::array<ayni_body_ref_v1, AYNI_VALUE_BODY_COUNT_MAX> refs{};
    const auto count =
        catalog::value_bodies(work_.calls.input(index), refs.data(), AYNI_VALUE_BODY_COUNT_MAX);
    for (uint32_t at = 0; at < count; ++at) {
        host_.bodies->release_temporary(host_.bodies->context, &refs[at]);
    }
    slots_[index].expired_model_input = work_.calls.identity(index).call_id;
}
bool CallRuntime::admits_call(uint32_t index, JournalAdmission admission) const {
    return admission == JournalAdmission::available || work_.calls.state(index).ephemeral;
}

resources::ResourceAdmission::Admission
CallRuntime::resource_admission(JournalAdmission admission) {
    using Admission = resources::ResourceAdmission::Admission;
    return admission == JournalAdmission::available ? Admission::all : Admission::ram_only;
}

CallRuntime::Advance CallRuntime::advance(uint64_t now_us, uint64_t durable_seq, Mode mode,
                                          JournalAdmission admission) {
    // Bodies whose last durable record is gone; the queue is not a transition (ADR-0096).
    // Without a store the composition root owns them, and the queue still drains.
    ayni_body_ref_v1 released{};
    if (work_.calls.next_release(durable_seq, released)) {
        release_body(released);
        return Advance::applied;
    }
    if (mode == Mode::fenced || mode == Mode::closing) {
        return drain_transport(mode);
    }
    if (const auto receipt = consume(now_us, durable_seq, admission)) {
        return *receipt;
    }
    uint32_t waiting_task = 0;
    if (admission == JournalAdmission::available &&
        work_.tasks.next_volatile_unknown(waiting_task)) {
        work_.tasks.commit_volatile_unknown(waiting_task, work_.transitions.next());
        work_.forget_due_work();
        return Advance::applied;
    }
    if (admission == JournalAdmission::available && mode == Mode::running &&
        next_subscription_wakeup(now_us, durable_seq, waiting_task)) {
        if (!work_.tasks.wake_operation(waiting_task, work_.transitions.next())) {
            return Advance::internal;
        }
        work_.forget_due_work();
        return Advance::applied;
    }
    for (uint32_t scanned = 0; scanned < limits_.capacity; ++scanned) {
        const uint32_t index = cursor_;
        cursor_ = (cursor_ + 1U) % limits_.capacity;
        if (!work_.calls.occupied(index) || !admits_call(index, admission)) {
            continue;
        }
        if (model_input_due(index, durable_seq)) {
            release_model_input(index);
            return Advance::applied; // Volatile lease cleanup; no new journal fact.
        }
        const auto result = advance_call(index, now_us, durable_seq, mode, admission);
        if (result != Advance::idle) {
            return result;
        }
    }
    return Advance::idle;
}

// C-COMMIT-006: this pass may write PREPARED or DISPATCHING. Control and ordinary
// Call work keep their own lanes, including the only path that can dispatch.
CallRuntime::Advance CallRuntime::prepare_accepted(const AcceptedCalls &accepted,
                                                   uint64_t deadline_us, uint64_t durable_seq,
                                                   bool ephemeral_only,
                                                   JournalAdmission admission) {
    const auto now_us = host_.monotonic_now_us(host_.ctx);
    if (now_us >= deadline_us || mailbox_.in_flight() == limits_.capacity ||
        control_ready(now_us, durable_seq, Mode::running) ||
        ready_before_calls(now_us, durable_seq, Mode::running, admission)) {
        return Advance::idle;
    }
    const auto selection_us = host_.monotonic_now_us(host_.ctx);
    if (selection_us >= deadline_us) {
        return Advance::idle;
    }
    const auto candidate =
        next_accepted_preparation(accepted, selection_us, durable_seq, ephemeral_only, admission);
    if (!candidate) {
        return Advance::idle;
    }

    // Control and ingress can become ready during the scan. The final sample
    // gives both precedence over a journal-only transition (ADR-0052).
    const auto prepare_us = host_.monotonic_now_us(host_.ctx);
    if (prepare_us >= deadline_us || control_ready(prepare_us, durable_seq, Mode::running) ||
        ready_before_calls(prepare_us, durable_seq, Mode::running, admission)) {
        return Advance::idle;
    }
    const auto ready = call_readiness(*candidate, prepare_us, durable_seq, Mode::running);
    if (ready == CallReady::queued) {
        auto &active = *slots_[*candidate].active;
        auto on_wait = resources::WaitAction::wait;
        // One arbitration at prepare_us. Selection changes and expired waits
        // both yield to ordinary scheduling (C-RES-005).
        const auto acquired = resources_.acquire(active.call_id, prepare_us, active.claim, on_wait,
                                                 resource_admission(admission));
        return acquired == resources::ResourceAdmission::Acquisition::acquired
                   ? record_preparation(*candidate)
                   : Advance::idle;
    }
    // A Call that already has its claim still yields to any newly overdue waiter.
    return ready == CallReady::prepared &&
                   prepare_us < resources_.next_deadline_us(resource_admission(admission))
               ? prepare_barrier(*candidate)
               : Advance::idle;
}

// Reuse ordinary readiness so stop, expire, dispatch and older work retain
// precedence. Among fresh Calls, finish a prepared Call before taking a new claim.
std::optional<uint32_t> CallRuntime::next_accepted_preparation(const AcceptedCalls &accepted,
                                                               uint64_t now_us,
                                                               uint64_t durable_seq,
                                                               bool ephemeral_only,
                                                               JournalAdmission admission) const {
    resources::AdmissionChoice choice{};
    const bool chosen = resources_.next(now_us, choice, resource_admission(admission));
    std::optional<uint32_t> candidate;
    for (uint32_t at = 0; at < limits_.capacity; ++at) {
        if (!work_.calls.occupied(at)) {
            continue;
        }
        const auto ready = scheduled_readiness(at, now_us, durable_seq, Mode::running,
                                               chosen ? &choice : nullptr, admission);
        if (ready == CallReady::idle) {
            continue;
        }
        if (ready == CallReady::other || !accepted.contains(work_.calls.identity(at).call_id)) {
            return std::nullopt;
        }
        if (ephemeral_only && !work_.calls.state(at).ephemeral) {
            continue;
        }
        if (!candidate || ready == CallReady::prepared) {
            candidate = at;
        }
    }
    return candidate;
}

// Cancellation intent is distinct from a terminal receipt. The Provider keeps
// its delivery reservation until it reports a terminal, even if cancel refuses.
CallRuntime::Advance CallRuntime::advance_call(uint32_t index, uint64_t now_us,
                                               uint64_t durable_seq, Mode mode,
                                               JournalAdmission admission) {
    const auto &id = work_.calls.identity(index);
    const auto &state = work_.calls.state(index);
    if (state.execution && !work::execution_stopped(*state.execution) &&
        control_.stop_measured(id.call_id)) {
        return control_.commit_stop(index);
    }
    if (state.reflex) {
        return advance_reflex_call(index, now_us, durable_seq, mode, admission);
    }
    uint32_t task_index = 0;
    if (!work_.tasks.find(id.task_id, task_index)) {
        return Advance::internal;
    }
    const auto &task = work_.tasks.at(task_index);
    if (slots_[index].active && slots_[index].active->stage == Stage::lease_stop) {
        return state.outcome == 0 && mode != Mode::fenced
                   ? restore_wait(index, task_index, now_us, durable_seq, mode)
                   : Advance::idle;
    }
    if (slots_[index].active && slots_[index].active->stage == Stage::retiring) {
        // A storage reload may have retained the physical lease after its volatile
        // Provider terminal was drained. First persist stop, then retain UNKNOWN.
        return state.outcome == 0 && state.execution && work::execution_stopped(*state.execution)
                   ? restore_wait(index, task_index, now_us, durable_seq, mode)
                   : Advance::idle;
    }

    if (subscription_reopen_ready(index, now_us, durable_seq, mode)) {
        return reopen_subscription(index, task_index, now_us, durable_seq, mode);
    }
    if (subscription_stop_pending(index)) {
        return commit_subscription_stop(index);
    }
    if (state.outcome != 0U) {
        if (task.call.id == id.call_id && task.call.end != work::TaskCall::End::none &&
            mode != Mode::fenced) {
            work_.tasks.finish_call(task_index, work_.transitions.next());
            return Advance::applied;
        }
        return work_.calls.query_pending(index) ? advance_query(index, now_us, durable_seq, mode)
                                                : Advance::idle;
    }
    if (!slots_[index].active) {
        return restore_wait(index, task_index, now_us, durable_seq, mode);
    }
    const bool content_current = task_content_current({work_, now_us, durable_seq}, task_index);
    auto &active = *slots_[index].active;
    if (active.call_id != id.call_id) {
        return Advance::internal;
    }
    const auto task_authority = work_.check_task_authority(task_index, now_us, durable_seq);
    const auto authority = task_authority.status == AYNI_OK
                               ? work_.check_ephemeral_call_authority(index, now_us, durable_seq)
                               : task_authority;
    const bool user_stop = (active.delivery && active.delivery->inhibition) ||
                           task.call.end != work::TaskCall::End::none ||
                           task.lifecycle == AYNI_TASK_PAUSED ||
                           !work::TaskTable::is_live(task.lifecycle);
    const bool expired = delivery_expired(index, now_us);
    const bool denied =
        !content_current || (authority.status != AYNI_OK && authority.status != AYNI_BUSY);
    if (mode != Mode::running || user_stop || expired || denied ||
        state.cancel_cause != work::CallCancelCause::none) {
        const auto cause = state.cancel_cause != work::CallCancelCause::none ? state.cancel_cause
                           : active.delivery && active.delivery->inhibition
                               ? *active.delivery->inhibition
                           : mode != Mode::running ? work::CallCancelCause::shutdown
                           : user_stop             ? work::CallCancelCause::user
                           : expired               ? work::CallCancelCause::deadline
                                                   : work::CallCancelCause::authority;
        return stop(index, task_index, mode, cause);
    }
    if (authority.status != AYNI_OK || mode == Mode::fenced) {
        return Advance::idle;
    }
    return progress_dispatch(index, task_index, now_us, durable_seq, admission);
}

// Advances a Task-less RAM-only ReflexRule Call through preemption, admission and dispatch
// while re-verifying live Agent + Grant authority on every step (ADR-0112).
CallRuntime::Advance CallRuntime::advance_reflex_call(uint32_t index, uint64_t now_us,
                                                      uint64_t durable_seq, Mode mode,
                                                      JournalAdmission admission) {
    const auto &id = work_.calls.identity(index);
    const auto &state = work_.calls.state(index);
    if (slots_[index].active && slots_[index].active->stage == Stage::retiring) {
        return Advance::idle;
    }
    if (state.outcome != 0U) {
        if (!slots_[index].active) {
            static_cast<void>(work_.calls.retire_ephemeral(index));
        }
        return Advance::idle;
    }
    if (!slots_[index].active || slots_[index].active->call_id != id.call_id) {
        return Advance::internal;
    }
    const auto &active = *slots_[index].active;
    const auto authority = work_.check_ephemeral_call_authority(index, now_us, durable_seq);
    const bool expired = delivery_expired(index, now_us);
    const bool denied = authority.status != AYNI_OK && authority.status != AYNI_BUSY;
    if (mode != Mode::running || expired || denied ||
        state.cancel_cause != work::CallCancelCause::none ||
        (active.delivery && active.delivery->inhibition)) {
        const auto cause = state.cancel_cause != work::CallCancelCause::none ? state.cancel_cause
                           : active.delivery && active.delivery->inhibition
                               ? *active.delivery->inhibition
                           : mode != Mode::running ? work::CallCancelCause::shutdown
                           : expired               ? work::CallCancelCause::deadline
                                                   : work::CallCancelCause::authority;
        return stop(index, no_task, mode, cause);
    }
    if (authority.status != AYNI_OK || mode == Mode::fenced) {
        return Advance::idle;
    }
    return progress_dispatch(index, no_task, now_us, durable_seq, admission);
}

// Resource admission determines order; table scanning never bypasses its fair
// choice. Preparation follows acquisition; DISPATCHING waits for their durable prefix.
CallRuntime::Advance CallRuntime::progress_dispatch(uint32_t index, uint32_t task_index,
                                                    uint64_t now_us, uint64_t durable_seq,
                                                    JournalAdmission admission) {
    auto &active = *slots_[index].active;
    if (active.stage == Stage::queued) {
        auto on_wait = resources::WaitAction::wait;
        const auto acquired = resources_.acquire(active.call_id, now_us, active.claim, on_wait,
                                                 resource_admission(admission));
        using Acquisition = resources::ResourceAdmission::Acquisition;
        if (acquired == Acquisition::not_selected) {
            if (active.preemption == AYNI_CALL_PREEMPT_NONE ||
                resources_.wait_expired(active.call_id, now_us)) {
                return Advance::idle;
            }
            // Scene/observation preflight must pass before preemption can touch any holder
            // (ADR-0111). A disallowed preemptor cancels locally without disturbing victims.
            if (!control_.allowed(index, now_us, durable_seq)) {
                return cancel_local(index, task_index);
            }
            return trigger_preemption(index, now_us, durable_seq);
        }
        if (acquired == Acquisition::expired) {
            return expire_wait(index, task_index, on_wait, now_us);
        }
        return record_preparation(index);
    }
    if (active.stage == Stage::prepared) {
        return prepare_barrier(index);
    }
    const auto &state = work_.calls.state(index);
    return active.stage == Stage::barrier &&
                   (state.ephemeral || state.latest.transition.commit_seq <= durable_seq)
               ? dispatch(index, task_index, now_us, durable_seq)
               : Advance::idle;
}

CallRuntime::Advance CallRuntime::record_preparation(uint32_t index) {
    slots_[index].active->stage = Stage::prepared;
    // A readmitted Call can retain its existing PREPARED fact (ADR-0045).
    const foundation::TransitionStamp stamp = work_.calls.state(index).ephemeral
                                                  ? foundation::TransitionStamp{0, 0}
                                                  : work_.transitions.next();
    if (work_.calls.action(index) == nullptr &&
        work_.calls.prepare(index, {work_.ids.boot_epoch(), stamp}) != AYNI_OK) {
        return Advance::internal;
    }
    return Advance::applied;
}

// Reserve the exact delivery before journaling DISPATCHING. If publication
// fails, withdraw the reservation before the Kernel enters restricted mode.
CallRuntime::Advance CallRuntime::prepare_barrier(uint32_t index) {
    if (mailbox_.in_flight() == limits_.capacity) {
        return Advance::idle;
    }
    ayni_attempt_ref_v1 attempt{};
    ProviderReservation reservation{};
    if (!attempts_.next(work_.ids.boot_epoch(), attempt)) {
        return Advance::attempt_exhausted;
    }
    // Only a TOOL_CALLS model attempt may post a list, within its own result bound.
    const auto &contract = work_.calls.contract(index);
    const uint32_t written_limit =
        (contract.result_format == catalog::call_tool_calls_result_format ||
         contract.result_format == catalog::call_value_result_format)
            ? contract.result_text_limit
            : 0U;
    if (mailbox_.reserve(attempt, reservation, written_limit) != AYNI_OK) {
        return Advance::internal;
    }
    const foundation::TransitionStamp stamp = work_.calls.state(index).ephemeral
                                                  ? foundation::TransitionStamp{0, 0}
                                                  : work_.transitions.next();
    if (work_.calls.begin_dispatch(index, attempt, {work_.ids.boot_epoch(), stamp}) != AYNI_OK) {
        static_cast<void>(mailbox_.withdraw(reservation));
        return Advance::internal;
    }
    auto &active = *slots_[index].active;
    active.delivery = Delivery{reservation, nullptr, UINT64_MAX, Delivery::Cancel::none};
    active.stage = Stage::barrier;
    return Advance::applied;
}
// Urgent output inhibition precedes journal admission. Routine polling alternates
// with business work; an idle work turn yields to the Kernel's other work owners,
// including when max_items=1. No hidden second callback bypasses the step budget.
CallRuntime::Advance CallRuntime::advance_managed(uint64_t now_us, uint64_t durable_seq, Mode mode,
                                                  JournalAdmission admission) {
    const auto expired = release_expired_subscriptions(now_us);
    if (expired != Advance::idle) {
        return expired;
    }
    const auto urgent = control_.advance_urgent(now_us, durable_seq, mode);
    if (urgent != Advance::idle) {
        return urgent;
    }
    if (subscription_cancel_ready()) {
        return advance_subscription_control(now_us, durable_seq, mode, admission);
    }
    if (next_lane_ == NextLane::control) {
        next_lane_ = NextLane::work;
        const auto control = control_.advance(now_us, durable_seq, mode);
        if (control != Advance::idle) {
            return control;
        }
        const auto subscription =
            advance_subscription_control(now_us, durable_seq, mode, admission);
        if (subscription != Advance::idle) {
            return subscription;
        }

    } else {
        next_lane_ = NextLane::control;
    }
    return advance(now_us, durable_seq, mode, admission);
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

bool CallRuntime::transport_holds(const void *context, uint64_t call_id, uint64_t now_us,
                                  bool &out_expired) {
    const auto &runtime = *static_cast<const CallRuntime *>(context);
    uint32_t index = 0;
    if (!runtime.active_index(call_id, index)) {
        return false;
    }
    out_expired = runtime.delivery_expired(index, now_us);
    return true;
}

bool CallRuntime::retiring_index(uint64_t call_id, uint32_t &index) const {
    return active_index(call_id, index) && slots_[index].active->stage == Stage::retiring;
}

bool CallRuntime::transport_retiring(const void *context, uint64_t call_id) {
    uint32_t index = 0;
    return static_cast<const CallRuntime *>(context)->retiring_index(call_id, index);
}

void CallRuntime::transport_retire(void *context, uint64_t call_id, uint64_t now_us) {
    auto &runtime = *static_cast<CallRuntime *>(context);
    uint32_t index = 0;
    if (runtime.retiring_index(call_id, index)) {
        runtime.release(index, now_us);
    }
}

// The ledger cannot retire while either a transport borrow or a local lease survives.
bool CallRuntime::runtime_retains(const void *context, uint64_t call_id) {
    const auto &runtime = *static_cast<const CallRuntime *>(context);
    uint32_t index = 0;
    return runtime.control_.holds_lease(call_id) || runtime.active_index(call_id, index);
}

ProviderMailbox::BlockedSlots CallRuntime::blocked_receipts(JournalAdmission admission) const {
    ProviderMailbox::BlockedSlots blocked{};
    for (uint32_t at = 0; at < limits_.capacity; ++at) {
        const auto &active = slots_[at].active;
        if (active && active->delivery &&
            (!admits_call(at, admission) ||
             (active->stage == Stage::waiting_stop && work_.calls.state(at).execution &&
              !work::execution_stopped(*work_.calls.state(at).execution)))) {
            blocked[active->delivery->reservation.index] = true;
        }
    }
    return blocked;
}
} // namespace ayni::kernel
