#include "foundation/attempt_identity.h"
#include "kernel/call/call_runtime.h"

namespace ayni::kernel {
namespace {
ayni_call_outcome_t outcome(ProviderMessageKind kind) {
    switch (kind) {
    case ProviderMessageKind::completed:
        return AYNI_CALL_COMPLETED;
    case ProviderMessageKind::failed:
        return AYNI_CALL_FAILED;
    case ProviderMessageKind::cancelled:
        return AYNI_CALL_CANCELLED;
    case ProviderMessageKind::abandoned:
        return AYNI_CALL_ABANDONED;
    case ProviderMessageKind::cannot_cancel:
        return AYNI_CALL_CANNOT_CANCEL;
    case ProviderMessageKind::cancel_unknown:
        return AYNI_CALL_CANCEL_UNKNOWN;
    case ProviderMessageKind::progress:
        return AYNI_CALL_PROGRESS;
    }
    return 0;
}
} // namespace

// Read never retires the delivery. Publication must succeed before acknowledge,
// so journal backpressure cannot free a slot or lose a result body (C-CALL-012).
CallRuntime::Advance CallRuntime::consume(uint64_t now_us) {
    ProviderMessage message{};
    ProviderReceipt receipt{};
    uint32_t required = 0;
    const auto blocked = blocked_receipts();
    const auto read =
        mailbox_.read_next(receipt_, limits_.result_bytes, required, message, receipt, &blocked);
    if (read == AYNI_NOT_FOUND) {
        return Advance::idle;
    }
    if (read != AYNI_OK) {
        return Advance::internal;
    }
    uint32_t index = 0;
    for (; index < limits_.capacity; ++index) {
        if (slots_[index].active && slots_[index].active->delivery &&
            foundation::same_attempt(slots_[index].active->delivery->reservation.attempt,
                                     receipt.reservation.attempt)) {
            break;
        }
    }
    uint32_t task_index = 0;
    if (index == limits_.capacity || !slots_[index].active || !slots_[index].active->delivery ||
        slots_[index].active->delivery->reservation.index != receipt.reservation.index ||
        !work_.tasks.find(work_.calls.identity(index).task_id, task_index)) {
        return Advance::internal;
    }
    const bool query = slots_[index].active->delivery->purpose == Delivery::Purpose::query;
    // Query progress/cancel acknowledgements do not establish execution facts;
    // keep its reserved terminal lane until a conclusive query delivery arrives.
    const bool old_boot = receipt.reservation.attempt.boot_epoch != work_.ids.boot_epoch();
    if ((query || old_boot) && (message.kind == ProviderMessageKind::progress ||
                                message.kind == ProviderMessageKind::cannot_cancel ||
                                message.kind == ProviderMessageKind::cancel_unknown)) {
        return mailbox_.acknowledge(receipt) == ProviderMailbox::Acknowledge::stale
                   ? Advance::internal
                   : Advance::applied;
    }
    const bool terminal_message = message.kind != ProviderMessageKind::progress &&
                                  message.kind != ProviderMessageKind::cannot_cancel &&
                                  message.kind != ProviderMessageKind::cancel_unknown;
    const auto &execution = work_.calls.state(index).execution;
    if (!query && terminal_message && execution && !work::execution_stopped(*execution)) {
        const auto call_id = work_.calls.identity(index).call_id;
        if (!control_.delivery_ended(call_id)) {
            return Advance::internal;
        }
        slots_[index].active->stage = Stage::waiting_stop;
        return control_.request_stop(call_id) ? Advance::applied : Advance::internal;
    }
    bool terminal = false;
    const auto applied =
        apply_receipt(index, task_index, message, work_.transitions.next(), terminal);
    if (applied != AYNI_OK ||
        mailbox_.acknowledge(receipt) == ProviderMailbox::Acknowledge::stale) {
        return Advance::internal;
    }
    if (terminal) {
        release(index, now_us);
    }
    return Advance::applied;
}

// Domain publication shares one B4 transition with the selected Task. Transport
// interpretation is separate from copying/acknowledging the reliable mailbox.
ayni_status_t CallRuntime::apply_receipt(uint32_t index, uint32_t task_index,
                                         const ProviderMessage &message,
                                         foundation::TransitionStamp stamp, bool &terminal) {
    const bool query = slots_[index].active->delivery->purpose == Delivery::Purpose::query;
    const work::ActionStamp action_stamp{work_.ids.boot_epoch(), stamp};
    const bool old_boot =
        slots_[index].active->delivery->reservation.attempt.boot_epoch != work_.ids.boot_epoch();
    // C-CALL-011: retaining an old transport borrow across storage reload does
    // not authorize old evidence to update the new writer's execution result.
    if (old_boot) {
        terminal = true;
        if (query) {
            return work_.calls.end_query(index, AYNI_RESULT_UNKNOWN, AYNI_REASON_NONE,
                                         action_stamp);
        }
        const auto status = work_.calls.finish(index,
                                               {AYNI_CALL_ABANDONED,
                                                AYNI_RESULT_UNKNOWN,
                                                AYNI_REASON_NONE,
                                                work::ActionEffect::unknown,
                                                {}},
                                               action_stamp);
        return status == AYNI_OK && finish_task(index, task_index, stamp) ? AYNI_OK : AYNI_INTERNAL;
    }
    const auto kind = outcome(message.kind);
    terminal = false;
    ayni_status_t applied = AYNI_INVALID_ARGUMENT;
    if (query) {
        terminal = true;
        applied = consume_query(index, task_index, message, stamp) ? AYNI_OK : AYNI_INTERNAL;
    } else if (kind == AYNI_CALL_PROGRESS) {
        applied = work_.calls.progress(index, message.sequence, action_stamp);
    } else if (kind == AYNI_CALL_CANNOT_CANCEL || kind == AYNI_CALL_CANCEL_UNKNOWN) {
        applied = work_.calls.cancel_response(index, kind, action_stamp);
    } else {
        terminal = true;
        work::CallResult result{
            kind,
            message.status,
            message.reason,
            static_cast<work::ActionEffect>(message.effect),
            {reinterpret_cast<const char *>(message.payload.data), message.payload.size}};
        applied = message.query_result == 0U
                      ? work_.calls.finish(index, result, action_stamp,
                                           control_.result_dependencies(index))
                      : AYNI_INVALID_ARGUMENT;
        if (applied == AYNI_INVALID_ARGUMENT || applied == AYNI_CAPACITY_EXCEEDED) {
            // A terminal has ended the local borrow but malformed evidence cannot
            // prove success/failure. Preserve UNKNOWN with an explicit diagnostic.
            applied = work_.calls.finish(index,
                                         {AYNI_CALL_ABANDONED,
                                          AYNI_RESULT_UNKNOWN,
                                          AYNI_REASON_PROVIDER_RESULT,
                                          work::ActionEffect::unknown,
                                          {}},
                                         action_stamp);
        }
        if (applied == AYNI_OK && !finish_task(index, task_index, stamp)) {
            return AYNI_INTERNAL;
        }
    }
    return applied;
}

uint64_t CallRuntime::next_deadline_us() const {
    uint64_t deadline = resources_.next_deadline_us();
    if (control_.next_deadline_us() < deadline) {
        deadline = control_.next_deadline_us();
    }
    for (uint32_t at = 0; at < limits_.capacity; ++at) {
        const auto &active = slots_[at].active;
        if (active && active->stage == Stage::submitted && active->delivery &&
            active->delivery->cancel == Delivery::Cancel::none &&
            active->delivery->deadline_us < deadline) {
            deadline = active->delivery->deadline_us;
        }
    }
    return deadline;
}

// Project only work that can advance now. A pending remote call alone must not
// keep the owner spinning; completion wakeups or the next timer bring it back.
bool CallRuntime::ready(uint64_t now_us, uint64_t durable_seq, Mode mode) const {
    if (mode == Mode::fenced || mode == Mode::closing) {
        return transport_ready(mode);
    }
    const auto blocked = blocked_receipts();
    if (mailbox_.has_pending(&blocked)) {
        return true;
    }
    resources::AdmissionChoice choice{};
    const bool resource_ready = resources_.next(now_us, choice);
    for (uint32_t at = 0; at < limits_.capacity; ++at) {
        if (!work_.calls.occupied(at)) {
            continue;
        }
        const auto &state = work_.calls.state(at);
        const auto &id = work_.calls.identity(at);
        if (control_.stop_measured(id.call_id) && state.execution &&
            !work::execution_stopped(*state.execution)) {
            return true;
        }
        if (slots_[at].active && slots_[at].active->stage == Stage::retiring) {
            continue;
        }
        uint32_t task_index = 0;
        if (!work_.tasks.find(id.task_id, task_index)) {
            return true; // advance reports the invariant failure, instead of hiding it.
        }
        const auto &task = work_.tasks.at(task_index);
        if (state.outcome != 0U) {
            if (task.call.id == id.call_id && task.call.end != work::TaskCall::End::none) {
                return mode != Mode::fenced;
            }
            if (work_.calls.query_pending(at) && query_ready(at, now_us, durable_seq, mode)) {
                return true;
            }
            continue;
        }
        const auto &active = slots_[at].active;
        if (!active) {
            if (task.call.end != work::TaskCall::End::none) {
                return true;
            }
            if (work::TaskTable::is_held(task) || task.lifecycle == AYNI_TASK_PAUSED) {
                continue;
            }
            if (mode != Mode::fenced && resources_.has_room()) {
                return true;
            }
            continue;
        }
        const auto authority = work_.check_task_authority(task_index, now_us, durable_seq);
        const bool stopping = mode != Mode::running || task.call.end != work::TaskCall::End::none ||
                              task.lifecycle == AYNI_TASK_PAUSED ||
                              state.cancel_cause != work::CallCancelCause::none ||
                              (authority.status != AYNI_OK && authority.status != AYNI_BUSY) ||
                              delivery_expired(at, now_us);
        if (stopping) {
            if (submitted(active->stage) ? active->delivery->cancel == Delivery::Cancel::none
                                         : mode != Mode::fenced) {
                return true;
            }
            continue;
        }
        if (active->stage == Stage::queued && resource_ready && choice.call_id == id.call_id) {
            return true;
        }
        if ((active->stage == Stage::prepared ||
             (active->stage == Stage::barrier && control_.issue_ready(at, now_us))) &&
            state.latest.transition.commit_seq <= durable_seq && authority.status == AYNI_OK) {
            return true;
        }
    }
    return false;
}
} // namespace ayni::kernel
