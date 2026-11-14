#include "catalog/capability/business_encoding.h"
#include "catalog/capability/value_validation.h"
#include "foundation/attempt_identity.h"
#include "foundation/workspace.h"
#include "kernel/service/call/call_runtime.h"
#include "kernel/service/memory/memory_access.h"
#include "work/call_propagation.h"
#include <cstring>

namespace ayni::kernel {
namespace {
// The reliable mailbox retains its own copy until acknowledge; this temporary receipt copy
// must be erased on every return, including query notices and waiting for physical stop.
struct ReceiptScratch {
    uint8_t *data; // Borrowed CallRuntime scratch, never the mailbox's retained delivery.
    uint32_t size; // Full allocated bytes, including unused tail; zero iff disabled.
    ~ReceiptScratch() {
        if (data != nullptr) {
            std::memset(data, 0, size);
        }
    }
};
ayni_call_outcome_t outcome(ProviderMessageKind kind) {
    switch (kind) {
    case ProviderMessageKind::subscription_started:
        return AYNI_CALL_SUBSCRIPTION_STARTED;
    case ProviderMessageKind::subscription_renewed:
        return AYNI_CALL_SUBSCRIPTION_RENEWED;
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
CallRuntime::Advance CallRuntime::consume(uint64_t now_us, uint64_t durable_seq) {
    const ReceiptScratch scratch{receipt_, limits_.result_bytes};
    const ReceiptScratch normalized_scratch{normalized_, limits_.result_bytes};
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
                                message.kind == ProviderMessageKind::cancel_unknown ||
                                provider_subscription_control(message.kind))) {
        return mailbox_.acknowledge(receipt) == ProviderMailbox::Acknowledge::stale
                   ? Advance::internal
                   : Advance::applied;
    }
    if (!query && !old_boot && provider_subscription_control(message.kind)) {
        return consume_subscription_control(index, task_index, message, receipt,
                                            host_.monotonic_now_us(host_.ctx), durable_seq);
    }
    if (!query && !old_boot && subscription_stop_pending(index)) {
        return commit_subscription_stop(index);
    }
    const bool terminal_message = provider_terminal(message.kind);
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
    const auto applied = apply_delivery(index, task_index, message, durable_seq, terminal);
    if (applied != AYNI_OK ||
        mailbox_.acknowledge(receipt) == ProviderMailbox::Acknowledge::stale) {
        return Advance::internal;
    }
    if (terminal) {
        release(index, now_us);
    }
    return Advance::applied;
}

// Validate the original Provider evidence first, including UTF-8 and proposal shape.
// Privacy may discard a valid late body; it must never turn malformed evidence into success.
ayni_status_t CallRuntime::apply_delivery(uint32_t index, uint32_t task_index,
                                          const ProviderMessage &message, uint64_t durable_seq,
                                          bool &terminal) {
    const auto applied =
        apply_receipt(index, task_index, message, work_.transitions.next(), terminal);
    if (applied == AYNI_OK && terminal && work_.calls.state(index).outcome != 0U &&
        (!task_content_current({work_, host_.monotonic_now_us(host_.ctx), durable_seq},
                               task_index) ||
         !model_context_current(task_index, work_.calls.identity(index).model_context,
                                host_.monotonic_now_us(host_.ctx), durable_seq))) {
        work_.calls.discard_unusable_result(index);
    }
    // A body this transition does not keep was never named by a durable record.
    if (message.body_ref.id != 0U &&
        (applied != AYNI_OK || !work_.calls.occupied(index) ||
         work_.calls.state(index).result_body.id != message.body_ref.id)) {
        release_body(message.body_ref);
    }
    return applied;
}

// A successful post released the producer borrow; schema errors are consumed
// here as invalid evidence. Default expansion uses reserved owner result space.
ayni_status_t CallRuntime::normalize_result(uint32_t index, ayni_bytes_v1 body,
                                            work::CallResult &result) {
    const auto &contract = work_.calls.contract(index);
    if (result.kind == work::CallResultKind::text) {
        if (contract.business_contract.size == 0U ||
            (result.outcome != AYNI_CALL_COMPLETED && result.text.size == 0U)) {
            return AYNI_OK;
        }
        auto proof_lease = work_.workspace.make<catalog::DecodedBusiness>();
        if (!proof_lease) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        auto &proof = *proof_lease;
        const auto status =
            catalog::read_admitted_business(work_.workspace, contract.business_contract, proof);
        if (status != AYNI_OK) {
            return status;
        }
        ayni_value_node_v1 text{};
        text.kind = AYNI_VALUE_TEXT;
        text.subtree_end = 1;
        text.value.text = result.text;
        return catalog::scalar_matches(proof.result.nodes[0], text) ? AYNI_OK
                                                                    : AYNI_INVALID_ARGUMENT;
    }
    if (contract.result_format != catalog::call_value_result_format) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto proof_lease = work_.workspace.make<catalog::DecodedBusiness>();
    if (!proof_lease) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    auto &proof = *proof_lease;
    auto status =
        catalog::read_admitted_business(work_.workspace, contract.business_contract, proof);
    auto raw_lease = work_.workspace.make<catalog::DecodedValue>();
    if (!raw_lease) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    auto &raw = *raw_lease;
    auto normalized_lease = work_.workspace.make<catalog::DecodedValue>();
    if (!normalized_lease) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    auto &normalized = *normalized_lease;
    if (status == AYNI_OK) {
        status = catalog::decode_value(work_.workspace, body, raw);
    }
    if (status == AYNI_OK) {
        status = catalog::normalize_borrowed_value(work_.workspace, proof.result.view(), raw.view(),
                                                   normalized);
    }
    uint32_t size = 0;
    if (status == AYNI_OK) {
        status = catalog::encode_value(normalized.view(), normalized_, limits_.result_bytes, size);
    }
    if (status == AYNI_OK) {
        result.text = {reinterpret_cast<const char *>(normalized_), size};
    }
    return status;
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
        const auto status = work_.calls.finish(work_.workspace, index,
                                               {AYNI_CALL_ABANDONED,
                                                AYNI_RESULT_UNKNOWN,
                                                AYNI_REASON_NONE,
                                                work::ActionEffect::unknown,
                                                {}},
                                               action_stamp);
        return status == AYNI_OK &&
                       work::end_task_call(work_.calls, work_.tasks, index, task_index, stamp)
                   ? AYNI_OK
                   : AYNI_INTERNAL;
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
            {reinterpret_cast<const char *>(message.payload.data), message.payload.size},
            message.body,
            message.model_usage,
            message.body_ref,
            message.body_calls};
        // The producer admitted the proposal shape. Names and arguments can be
        // handled as ordinary tool errors; they are not business outcome evidence.
        // A body by reference is verified by its reader, never read by Core.
        const bool value_valid = message.body == work::CallResultKind::tool_calls ||
                                 message.body_ref.id != 0U ||
                                 normalize_result(index, message.payload, result) == AYNI_OK;
        applied = message.query_result == 0U && value_valid
                      ? work_.calls.finish(work_.workspace, index, result, action_stamp,
                                           control_.result_dependencies(index))
                      : AYNI_INVALID_ARGUMENT;
        if (applied == AYNI_INVALID_ARGUMENT || applied == AYNI_CAPACITY_EXCEEDED) {
            // A terminal has ended the local borrow but malformed evidence cannot
            // prove success/failure. Preserve UNKNOWN with an explicit diagnostic.
            applied = work_.calls.finish(work_.workspace, index,
                                         {AYNI_CALL_ABANDONED,
                                          AYNI_RESULT_UNKNOWN,
                                          AYNI_REASON_PROVIDER_RESULT,
                                          work::ActionEffect::unknown,
                                          {}},
                                         action_stamp);
        }
        if (applied == AYNI_OK &&
            !work::end_task_call(work_.calls, work_.tasks, index, task_index, stamp)) {
            return AYNI_INTERNAL;
        }
    }
    return applied;
}

uint64_t CallRuntime::next_deadline_us() const {
    uint64_t deadline = resources_.next_deadline_us();
    for (uint32_t at = 0; at < work_.tasks.size(); ++at) {
        const auto &task = work_.tasks.at(at);
        if (task.lifecycle == AYNI_TASK_WAITING && task.operation_wait &&
            task.operation_wait->timeout_ms) {
            const auto &wait = *task.operation_wait;
            const auto until = wait.boot_epoch == work_.ids.boot_epoch() ? wait.deadline_us : 0;
            if (until < deadline) {
                deadline = until;
            }
        }
    }
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
    for (uint32_t at = 0; at < limits_.capacity; ++at) {
        const auto &active = slots_[at].active;
        if (active && active->stage == Stage::lease_stop) {
            const auto stop = collection_stop_deadline(*active->collection);
            if (stop < deadline) {
                deadline = stop;
            }
        } else if (active && active->delivery && active->collection) {
            const auto &lease = *active->collection;
            if (!lease.inhibition && !lease.pending &&
                active->delivery->cancel == Delivery::Cancel::none &&
                lease.renew_at_us < deadline) {
                deadline = lease.renew_at_us;
            }
        }
    }
    return deadline;
}

// Project only work that can advance now. A pending remote call alone must not
// keep the owner spinning; completion wakeups or the next timer bring it back.
bool CallRuntime::ready(uint64_t now_us, uint64_t durable_seq, Mode mode) const {
    if (work_.calls.release_due(durable_seq)) {
        return true;
    }
    if (mode == Mode::fenced || mode == Mode::closing) {
        return transport_ready(mode);
    }
    const auto blocked = blocked_receipts();
    if (mailbox_.has_pending(&blocked)) {
        return true;
    }
    uint32_t waiting_task = 0;
    if (mode == Mode::running && next_subscription_wakeup(now_us, durable_seq, waiting_task)) {
        return true;
    }
    return calls_ready(now_us, durable_seq, mode);
}

// Match the ordinary Call progress lane, including retained background leases.
// A completed transport alone is idle until its Task, query or stop can advance.
bool CallRuntime::calls_ready(uint64_t now_us, uint64_t durable_seq, Mode mode) const {
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
        if (subscription_stop_pending(at) ||
            subscription_reopen_ready(at, now_us, durable_seq, mode) ||
            subscription_renew_ready(at, now_us, durable_seq)) {
            return true;
        }
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
            // ADR-0086: detached queued work still drains on pause/end, even under a wait hold.
            if (!work::TaskTable::is_live(task.lifecycle) ||
                task.call.end != work::TaskCall::End::none ||
                (state.attachment == work::CallAttachment::background &&
                 task.lifecycle == AYNI_TASK_PAUSED)) {
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
        const bool stopping = (active->delivery && active->delivery->inhibition) ||
                              !task_content_current({work_, now_us, durable_seq}, task_index) ||
                              mode != Mode::running || task.call.end != work::TaskCall::End::none ||
                              task.lifecycle == AYNI_TASK_PAUSED ||
                              !work::TaskTable::is_live(task.lifecycle) ||
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
