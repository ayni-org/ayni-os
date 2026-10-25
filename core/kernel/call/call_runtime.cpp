#include "kernel/call/call_runtime.h"

#include <new>

namespace ayni::kernel {
namespace {
ayni_string_v1 view(const catalog::CatalogId &id) {
    return {id.data(), id.size()};
}
} // namespace

std::size_t CallRuntime::storage_size(Limits limits) {
    return limits.capacity == 0U
               ? 0U
               : sizeof(Slot) * limits.capacity + limits.input_bytes + limits.result_bytes;
}

CallRuntime::CallRuntime(void *storage, Limits limits, WorkModel &work, CallCatalogs catalogs,
                         resources::ResourceAdmission &resources, resources::PermitTable &permits,
                         const knowledge::ObservationTable &observations, ProviderMailbox &mailbox,
                         AttemptSequence &attempts, const ayni_host_v1 &host)
    : work_(work), catalogs_(catalogs), resources_(resources), permits_(permits),
      observations_(observations), mailbox_(mailbox), attempts_(attempts), host_(host),
      limits_(limits), slots_(static_cast<Slot *>(storage)),
      staging_(limits.capacity == 0U ? nullptr
                                     : reinterpret_cast<uint8_t *>(slots_ + limits.capacity)),
      receipt_(limits.capacity == 0U ? nullptr : staging_ + limits.input_bytes) {
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
    if (permits_.find(active->call_id) != nullptr) {
        active->delivery.reset();
        active->stage = Stage::retiring; // Stop fact must become durable before the exact
                                         // quarantine is released.
        return;
    }
    if (active->claim.call_id != 0U) {
        static_cast<void>(resources_.release(active->claim));
        // Without an issued lease no Provider received a permit. This exact local proof
        // permits cleanup of a pre-dispatch actuator claim (ADR-0050).
        resources_.confirm_all_stopped(active->claim);
    } else {
        static_cast<void>(resources_.remove_waiter(active->call_id, now_us));
    }
    active.reset();
    work_.due_work_changed = true;
}

bool CallRuntime::idle() const {
    if (mailbox_.in_flight() != 0U || !permits_.empty()) {
        return false;
    }
    for (uint32_t at = 0; at < limits_.capacity; ++at) {
        if (slots_[at].active && slots_[at].active->stage != Stage::blocked) {
            return false;
        }
    }
    return true;
}

// Only a successful terminal result commits a completed call step. Other known
// outcomes also reserve their stable step identity in CallTable, so a strategy
// must inspect the result and explicitly choose a new step before trying again.
bool CallRuntime::finish_task(uint32_t call_index, uint32_t task_index,
                              foundation::TransitionStamp stamp) {
    const auto &state = work_.calls.state(call_index);
    const auto &task = work_.tasks.at(task_index);
    if (state.outcome == AYNI_CALL_ABANDONED && task.call.end == work::TaskCall::End::none) {
        work_.tasks.hold_unknown_call(task_index, stamp);
        return true;
    }
    if (task.call.end == work::TaskCall::End::none && state.outcome == AYNI_CALL_COMPLETED &&
        !work_.tasks.add_step(task_index, view(work_.calls.identity(call_index).step_key), stamp)) {
        return false;
    }
    work_.tasks.finish_call(task_index, stamp);
    work_.due_work_changed = true;
    return true;
}

CallRuntime::Advance CallRuntime::expire_wait(uint32_t index, uint32_t task_index,
                                              resources::WaitAction action, uint64_t now_us) {
    if (action == resources::WaitAction::wait) {
        return Advance::internal; // An unbounded waiter cannot be an expired queue choice.
    }
    const auto stamp = work_.transitions.next();
    if (action == resources::WaitAction::skip) {
        if (work_.calls.skip_before_dispatch(index, {work_.ids.boot_epoch(), stamp}) != AYNI_OK) {
            return Advance::internal;
        }
        // The occurrence was already taken when this Task was derived. Its
        // EXPIRED/SKIPPED facts close this run without advancing the next cursor.
        work_.tasks.skip_queued_call(task_index, stamp);
    } else {
        work_.tasks.pause_resource_wait(task_index, stamp);
    }
    release(index, now_us);
    return Advance::applied;
}

CallRuntime::Advance CallRuntime::cancel_local(uint32_t index, uint32_t task_index) {
    auto &active = slots_[index].active;
    if (active && active->delivery && !mailbox_.withdraw(active->delivery->reservation)) {
        return Advance::internal; // No submit has occurred; a receipt here violates ownership.
    }
    const auto stamp = work_.transitions.next();
    if (work_.calls.cancel_before_dispatch(index, {work_.ids.boot_epoch(), stamp}) != AYNI_OK ||
        !finish_task(index, task_index, stamp)) {
        return Advance::internal;
    }
    release(index, host_.monotonic_now_us(host_.ctx));
    return Advance::applied;
}

// Receipt consumption has priority over another dispatch. Every call step is
// one item; no loop waits for a resource, a Provider or a durable acknowledgement.
CallRuntime::Advance CallRuntime::advance(uint64_t now_us, uint64_t durable_seq, Mode mode) {
    if (mode == Mode::fenced || mode == Mode::closing) {
        return drain_transport(mode);
    }
    const auto blocked = blocked_receipts();
    if (mailbox_.has_pending(&blocked)) {
        return consume(now_us);
    }
    for (uint32_t scanned = 0; scanned < limits_.capacity; ++scanned) {
        const uint32_t index = cursor_;
        cursor_ = (cursor_ + 1U) % limits_.capacity;
        if (!work_.calls.occupied(index)) {
            continue;
        }
        const auto result = advance_call(index, now_us, durable_seq, mode);
        if (result != Advance::idle) {
            return result;
        }
    }
    return Advance::idle;
}

// Cancellation intent is distinct from a terminal receipt. The Provider keeps
// its delivery reservation until it reports a terminal, even if cancel refuses.
CallRuntime::Advance CallRuntime::advance_call(uint32_t index, uint64_t now_us,
                                               uint64_t durable_seq, Mode mode) {
    const auto &id = work_.calls.identity(index);
    const auto &state = work_.calls.state(index);
    if (const auto *lease = permits_.find(id.call_id); lease != nullptr && lease->stopped &&
                                                       state.execution &&
                                                       !work::execution_stopped(*state.execution)) {
        return commit_stop(index);
    }
    uint32_t task_index = 0;
    if (!work_.tasks.find(id.task_id, task_index)) {
        return Advance::internal;
    }
    const auto &task = work_.tasks.at(task_index);
    if (slots_[index].active && slots_[index].active->stage == Stage::retiring) {
        // A storage reload may have retained the physical lease after its volatile
        // Provider terminal was drained. First persist stop, then retain UNKNOWN.
        return state.outcome == 0 && state.execution && work::execution_stopped(*state.execution)
                   ? restore_wait(index, task_index, now_us, mode)
                   : Advance::idle;
    }

    if (state.outcome != 0U) {
        if (task.call.id == id.call_id && task.call.end != work::TaskCall::End::none &&
            mode != Mode::fenced) {
            work_.tasks.finish_call(task_index, work_.transitions.next());
            work_.due_work_changed = true;
            return Advance::applied;
        }
        return work_.calls.query_pending(index) ? advance_query(index, now_us, durable_seq, mode)
                                                : Advance::idle;
    }
    if (!slots_[index].active) {
        if (task.call.end != work::TaskCall::End::none) {
            return cancel_local(index, task_index);
        }
        // Expired waits persist through shutdown. User pause is independent and
        // also prevents a resumed call from joining the queue until released.
        if (work::TaskTable::is_held(task) || task.lifecycle == AYNI_TASK_PAUSED) {
            return Advance::idle;
        }
        return restore_wait(index, task_index, now_us, mode);
    }
    auto &active = *slots_[index].active;
    if (active.call_id != id.call_id) {
        return Advance::internal;
    }
    const auto authority = work_.check_task_authority(task_index, now_us, durable_seq);
    const bool user_stop =
        task.call.end != work::TaskCall::End::none || task.lifecycle == AYNI_TASK_PAUSED;
    const bool expired = delivery_expired(index, now_us);
    const bool denied = authority.status != AYNI_OK && authority.status != AYNI_BUSY;
    if (mode != Mode::running || user_stop || expired || denied ||
        state.cancel_cause != work::CallCancelCause::none) {
        const auto cause = state.cancel_cause != work::CallCancelCause::none ? state.cancel_cause
                           : mode != Mode::running ? work::CallCancelCause::shutdown
                           : user_stop             ? work::CallCancelCause::user
                           : expired               ? work::CallCancelCause::deadline
                                                   : work::CallCancelCause::authority;
        return stop(index, task_index, mode, cause);
    }
    if (authority.status != AYNI_OK || mode == Mode::fenced) {
        return Advance::idle;
    }
    return progress_dispatch(index, task_index, now_us, durable_seq);
}

// Resource admission determines order; table scanning never bypasses its fair
// choice. Preparation follows acquisition, and each persisted barrier is separate.
CallRuntime::Advance CallRuntime::progress_dispatch(uint32_t index, uint32_t task_index,
                                                    uint64_t now_us, uint64_t durable_seq) {
    auto &active = *slots_[index].active;
    const auto &state = work_.calls.state(index);
    if (active.stage == Stage::queued) {
        resources::AdmissionChoice choice{};
        if (!resources_.next(now_us, choice) || choice.call_id != active.call_id) {
            return Advance::idle;
        }
        if (choice.expired) {
            return expire_wait(index, task_index, choice.on_wait, now_us);
        }
        if (!resources_.acquire(active.call_id, now_us, active.claim)) {
            return Advance::internal;
        }
        active.stage = Stage::prepared;
        if (work_.calls.action(index) == nullptr &&
            work_.calls.prepare(index, {work_.ids.boot_epoch(), work_.transitions.next()}) !=
                AYNI_OK) {
            return Advance::internal;
        }
        return Advance::applied;
    }
    if (active.stage == Stage::prepared) {
        if (state.latest.transition.commit_seq > durable_seq ||
            mailbox_.in_flight() == limits_.capacity) {
            return Advance::idle;
        }
        ayni_attempt_ref_v1 attempt{};
        ProviderReservation reservation{};
        if (!attempts_.next(work_.ids.boot_epoch(), attempt)) {
            return Advance::attempt_exhausted;
        }
        if (mailbox_.reserve(attempt, reservation) != AYNI_OK) {
            return Advance::internal;
        }
        if (work_.calls.begin_dispatch(index, attempt,
                                       {work_.ids.boot_epoch(), work_.transitions.next()},
                                       durable_seq) != AYNI_OK) {
            static_cast<void>(mailbox_.withdraw(reservation));
            return Advance::internal;
        }
        active.delivery = Delivery{reservation, nullptr, UINT64_MAX, Delivery::Cancel::none};
        active.stage = Stage::barrier;
        return Advance::applied;
    }
    return active.stage == Stage::barrier && state.latest.transition.commit_seq <= durable_seq
               ? dispatch(index, task_index, now_us, durable_seq)
               : Advance::idle;
}
} // namespace ayni::kernel
