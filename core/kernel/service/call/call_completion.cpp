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
std::optional<CallRuntime::Advance> CallRuntime::consume(uint64_t now_us, uint64_t durable_seq,
                                                         JournalAdmission admission) {
    // The configured byte limit can be nonzero even when there is no receipt arena.
    if (limits_.capacity == 0U) {
        return std::nullopt;
    }
    ProviderMessage message{};
    ProviderReceipt receipt{};
    uint32_t required = 0;
    const auto blocked = blocked_receipts(admission);
    const auto read =
        mailbox_.read_next(receipt_, limits_.result_bytes, required, message, receipt, &blocked);
    if (read == AYNI_NOT_FOUND) {
        return std::nullopt;
    }
    if (read != AYNI_OK) {
        return Advance::internal;
    }
    // read_next writes no payload on refusal or empty. Wipe both lanes after an actual copy.
    const ReceiptScratch scratch{receipt_, limits_.result_bytes};
    const ReceiptScratch normalized_scratch{normalized_, limits_.result_bytes};
    uint32_t index = 0;
    for (; index < limits_.capacity; ++index) {
        if (slots_[index].active && slots_[index].active->delivery &&
            foundation::same_attempt(slots_[index].active->delivery->reservation.attempt,
                                     receipt.reservation.attempt)) {
            break;
        }
    }
    uint32_t task_index = no_task;
    if (index == limits_.capacity || !slots_[index].active || !slots_[index].active->delivery ||
        slots_[index].active->delivery->reservation.index != receipt.reservation.index ||
        !resolve_call_task(index, task_index)) {
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
        if (const auto gated = settle_execution_terminal(index, message)) {
            return *gated;
        }
    }
    bool terminal = false;
    const auto applied =
        apply_delivery(index, task_index, message, durable_seq, terminal, admission);
    if (applied != AYNI_OK ||
        mailbox_.acknowledge(receipt) == ProviderMailbox::Acknowledge::stale) {
        return Advance::internal;
    }
    if (terminal) {
        release(index, now_us);
    }
    return Advance::applied;
}

// Handles physical execution stop confirmation when a terminal Provider receipt arrives.
// Returns std::nullopt iff an EPHEMERAL inline stop proof completed immediately so the caller
// can proceed to apply_delivery in the same step; otherwise returns the step advance result.
std::optional<CallRuntime::Advance>
CallRuntime::settle_execution_terminal(uint32_t index, const ProviderMessage &message) {
    const auto call_id = work_.calls.identity(index).call_id;
    if (!control_.delivery_ended(call_id)) {
        return Advance::internal;
    }
    // Colocated inline stop proof skips redundant STOP + STATUS round-trips (ADR-0110).
    // Any flag, identity or phase mismatch fails closed into normal quarantine polling.
    if (message.execution_status && control_.accept_inline_stop(index, *message.execution_status)) {
        if (work_.calls.state(index).ephemeral) {
            if (control_.commit_stop(index) != Advance::applied) {
                return Advance::internal;
            }
            return std::nullopt;
        }
        slots_[index].active->stage = Stage::waiting_stop;
        return control_.commit_stop(index);
    }
    slots_[index].active->stage = Stage::waiting_stop;
    return control_.request_stop(call_id) ? Advance::applied : Advance::internal;
}

// Validate the original Provider evidence first, including UTF-8 and proposal shape.
// Privacy may discard a valid late body; it must never turn malformed evidence into success.
ayni_status_t CallRuntime::apply_delivery(uint32_t index, uint32_t task_index,
                                          const ProviderMessage &message, uint64_t durable_seq,
                                          bool &terminal, JournalAdmission admission) {
    const bool ephemeral = work_.calls.state(index).ephemeral;
    const foundation::TransitionStamp stamp =
        ephemeral ? foundation::TransitionStamp{0, 0} : work_.transitions.next();
    const auto applied = apply_receipt(index, task_index, message, stamp, terminal, admission);
    if (applied == AYNI_OK && terminal && work_.calls.state(index).outcome != 0U &&
        task_index != no_task &&
        (!task_content_current({work_, host_.monotonic_now_us(host_.ctx), durable_seq},
                               task_index) ||
         !model_context_current(task_index, work_.calls.identity(index).model_context,
                                host_.monotonic_now_us(host_.ctx), durable_seq))) {
        work_.calls.discard_unusable_result(index);
    }
    // RAM-only EPHEMERAL results stay inline or in the body store's volatile cache;
    // skipping intermediate stage/confirm avoids per-tool Flash writes (ADR-0110).
    if (!ephemeral && applied == AYNI_OK && terminal && host_.bodies && host_.bodies->stage &&
        host_.bodies->confirm && (host_.bodies->reserved & AYNI_BODY_STORE_SHARED_RESULTS) != 0U &&
        work_.calls.contract(index).category != catalog::CallClass::model &&
        work_.calls.state(index).outcome == AYNI_CALL_COMPLETED &&
        !work_.calls.state(index).content_erased && !work_.calls.state(index).result_body.id) {
        const auto record = work_.calls.record(index);
        if (record.result_kind == work::CallResultKind::text ||
            record.result_kind == work::CallResultKind::value) {
            ayni_body_ref_v1 shared{};
            const auto copied = host_.bodies->stage(
                host_.bodies->context, record.identity.call_id,
                {reinterpret_cast<const uint8_t *>(record.result.data), record.result.size},
                &shared);
            if (copied == AYNI_OK) {
                const auto requested = host_.bodies->confirm(host_.bodies->context, &shared);
                if (requested == AYNI_OK || requested == AYNI_NOT_READY) {
                    if (work_.calls.externalize_result(index, shared) != AYNI_OK) {
                        return AYNI_INTERNAL;
                    }
                } else {
                    release_body(shared);
                }
            }
            // C-CALL-012 / ADR-0104: sharing may backpressure, but never lose an accepted
            // receipt. Its bounded inline ledger remains authoritative until sharing succeeds;
            // the conversation store reports its capacity/persistence failure before progression.
        }
    }
    if (!ephemeral && applied == AYNI_OK && terminal && host_.bodies && host_.bodies->confirm &&
        message.body_ref.id != 0U &&
        work_.calls.state(index).result_body.id == message.body_ref.id) {
        // Preserve the raw model proposal once; conversation fields share or reference it.
        // The result is already accepted. A save failure remains visible in the body store
        // and blocks its reader/publication; it cannot fabricate another execution outcome.
        static_cast<void>(host_.bodies->confirm(host_.bodies->context, &message.body_ref));
    }
    // A body this transition does not keep was never named by a durable record.
    if (message.body_ref.id != 0U &&
        (applied != AYNI_OK || !work_.calls.occupied(index) ||
         work_.calls.state(index).result_body.id != message.body_ref.id)) {
        release_body(message.body_ref);
    }
    if (applied == AYNI_OK && terminal && work_.calls.occupied(index) &&
        work_.calls.state(index).outcome != 0U) {
        audit_call_terminal(index, host_.monotonic_now_us(host_.ctx));
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
                                         foundation::TransitionStamp stamp, bool &terminal,
                                         JournalAdmission admission) {
    const bool query = slots_[index].active->delivery->purpose == Delivery::Purpose::query;
    const work::ActionStamp action_stamp{work_.ids.boot_epoch(), stamp};
    // C-CALL-011: retaining an old transport borrow across storage reload does
    // not authorize old evidence to update the new writer's execution result.
    if (slots_[index].active->delivery->reservation.attempt.boot_epoch != work_.ids.boot_epoch()) {
        terminal = true;
        if (query) {
            return work_.calls.end_query(index, AYNI_RESULT_UNKNOWN, AYNI_REASON_NONE,
                                         action_stamp);
        }
        const work::CallResult unknown{AYNI_CALL_ABANDONED,
                                       AYNI_RESULT_UNKNOWN,
                                       AYNI_REASON_NONE,
                                       work::ActionEffect::unknown,
                                       {}};
        return work_.calls.finish(work_.workspace, index, unknown, action_stamp) == AYNI_OK &&
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
            const work::CallResult bad{AYNI_CALL_ABANDONED,
                                       AYNI_RESULT_UNKNOWN,
                                       AYNI_REASON_PROVIDER_RESULT,
                                       work::ActionEffect::unknown,
                                       {}};
            applied = work_.calls.finish(work_.workspace, index, bad, action_stamp);
        }
        if (applied == AYNI_OK) {
            if (work_.calls.state(index).ephemeral) {
                applied = task_index != no_task
                              ? finish_ephemeral_task_call(index, task_index, admission)
                              : AYNI_OK;
            } else if (!work::end_task_call(work_.calls, work_.tasks, index, task_index, stamp)) {
                return AYNI_INTERNAL;
            }
        }
    }
    return applied;
}

// Finishes a RAM-only Tier 0 EPHEMERAL call on its Task. Completed calls append a volatile
// step key with zero WAL traffic; ABANDONED persists an explicit UNKNOWN hold without
// setting task.call_unknown so reboot replay never looks up an unpersisted call row.
ayni_status_t CallRuntime::finish_ephemeral_task_call(uint32_t index, uint32_t task_index,
                                                      JournalAdmission admission) {
    const auto &final_state = work_.calls.state(index);
    const auto &task = work_.tasks.at(task_index);
    const auto call_id = work_.calls.identity(index).call_id;
    if (final_state.outcome == AYNI_CALL_ABANDONED) {
        work_.tasks.end_volatile_call(task_index, call_id);
        if (work::TaskTable::is_live(task.lifecycle)) {
            work_.tasks.defer_volatile_unknown(task_index, final_state.reason);
            if (admission == JournalAdmission::available &&
                !work::TaskTable::at_largest_revision(task)) {
                work_.tasks.commit_volatile_unknown(task_index, work_.transitions.next());
            }
        }
        return AYNI_OK;
    }
    if (work::TaskTable::is_live(task.lifecycle) && final_state.outcome == AYNI_CALL_COMPLETED) {
        const auto &step = work_.calls.identity(index).step_key;
        if (!work_.tasks.add_volatile_step(task_index, {step.data(), step.size()})) {
            return AYNI_INTERNAL;
        }
    }
    work_.tasks.end_volatile_call(task_index, call_id);
    return AYNI_OK;
}

// Journal-blocked timeouts wait for the append wake. A submitted physical
// Call still needs its deadline to inhibit output even if its ledger is blocked.
uint64_t CallRuntime::next_deadline_us(uint64_t now_us, JournalAdmission admission) const {
    uint64_t next = UINT64_MAX;
    const auto report = [&](uint64_t deadline) {
        if (deadline > now_us && deadline < next) {
            next = deadline;
        }
    };
    report(resources_.next_deadline_us(resource_admission(admission)));
    report(control_.next_deadline_us());
    if (admission == JournalAdmission::available) {
        for (uint32_t at = 0; at < work_.tasks.size(); ++at) {
            const auto &task = work_.tasks.at(at);
            if (task.lifecycle == AYNI_TASK_WAITING && task.operation_wait &&
                task.operation_wait->timeout_ms &&
                task.operation_wait->boot_epoch == work_.ids.boot_epoch()) {
                report(task.operation_wait->deadline_us);
            }
        }
    }
    for (uint32_t at = 0; at < limits_.capacity; ++at) {
        const auto &active = slots_[at].active;
        if (!active) {
            continue;
        }
        if (active->stage == Stage::lease_stop) {
            report(collection_stop_deadline(*active->collection));
        }
        if (!active->delivery || active->delivery->cancel != Delivery::Cancel::none) {
            continue;
        }
        if (active->stage == Stage::submitted &&
            (admits_call(at, admission) || control_.holds_lease(active->call_id))) {
            report(active->delivery->deadline_us);
        }
        if (admission == JournalAdmission::available && active->collection &&
            !active->collection->inhibition && !active->collection->pending) {
            report(active->collection->renew_at_us);
        }
    }
    return next;
}

// Project only work that can advance now. A pending remote call alone must not
// keep the owner spinning; completion wakeups or the next timer bring it back.
bool CallRuntime::ready(uint64_t now_us, uint64_t durable_seq, Mode mode,
                        JournalAdmission admission) const {
    if (mode == Mode::fenced || mode == Mode::closing) {
        return work_.calls.release_due(durable_seq) || transport_ready(mode);
    }
    return ready_before_calls(now_us, durable_seq, mode, admission) ||
           calls_ready(now_us, durable_seq, mode, admission);
}

bool CallRuntime::ready_before_calls(uint64_t now_us, uint64_t durable_seq, Mode mode,
                                     JournalAdmission admission) const {
    if (work_.calls.release_due(durable_seq)) {
        return true;
    }
    const auto blocked = blocked_receipts(admission);
    uint32_t waiting_task = 0;
    return mailbox_.has_pending(&blocked) ||
           (admission == JournalAdmission::available &&
            (work_.tasks.next_volatile_unknown(waiting_task) ||
             (mode == Mode::running &&
              next_subscription_wakeup(now_us, durable_seq, waiting_task))));
}

// Match the ordinary Call progress lane, including retained background leases.
// A completed transport alone is idle until its Task, query or stop can advance.
bool CallRuntime::calls_ready(uint64_t now_us, uint64_t durable_seq, Mode mode,
                              JournalAdmission admission) const {
    resources::AdmissionChoice choice{};
    const bool resource_ready = resources_.next(now_us, choice, resource_admission(admission));
    for (uint32_t at = 0; at < limits_.capacity; ++at) {
        if (work_.calls.occupied(at) &&
            scheduled_readiness(at, now_us, durable_seq, mode, resource_ready ? &choice : nullptr,
                                admission) != CallReady::idle) {
            return true;
        }
    }
    return false;
}

CallRuntime::CallReady CallRuntime::scheduled_readiness(uint32_t at, uint64_t now_us,
                                                        uint64_t durable_seq, Mode mode,
                                                        const resources::AdmissionChoice *choice,
                                                        JournalAdmission admission) const {
    if (!admits_call(at, admission)) {
        return CallReady::idle;
    }
    const auto ready = call_readiness(at, now_us, durable_seq, mode);
    if (ready != CallReady::queued) {
        return ready;
    }
    if (!choice || choice->call_id != work_.calls.identity(at).call_id) {
        return CallReady::idle;
    }
    return choice->expired ? CallReady::other : CallReady::queued;
}

// One occupied slot projects its next lifecycle transition. PREPARED may enqueue
// DISPATCHING immediately; only the final dispatch needs the durable prefix.
CallRuntime::CallReady CallRuntime::call_readiness(uint32_t at, uint64_t now_us,
                                                   uint64_t durable_seq, Mode mode) const {
    if (model_input_due(at, durable_seq)) {
        return CallReady::other;
    }
    const auto &state = work_.calls.state(at);
    const auto &id = work_.calls.identity(at);
    if (state.execution && !work::execution_stopped(*state.execution) &&
        control_.stop_measured(id.call_id)) {
        return CallReady::other;
    }
    if (slots_[at].active && slots_[at].active->stage == Stage::retiring) {
        return CallReady::idle;
    }
    if (state.reflex) {
        return reflex_call_readiness(at, now_us, durable_seq, mode);
    }
    uint32_t task_index = 0;
    if (!work_.tasks.find(id.task_id, task_index)) {
        return CallReady::other; // advance reports the invariant failure, instead of hiding it.
    }
    const auto &task = work_.tasks.at(task_index);
    if (subscription_stop_pending(at) || subscription_reopen_ready(at, now_us, durable_seq, mode) ||
        subscription_renew_ready(at, now_us, durable_seq)) {
        return CallReady::other;
    }
    if (state.outcome != 0U) {
        if (task.call.id == id.call_id && task.call.end != work::TaskCall::End::none) {
            return mode != Mode::fenced ? CallReady::other : CallReady::idle;
        }
        if (work_.calls.query_pending(at) && query_ready(at, now_us, durable_seq, mode)) {
            return CallReady::other;
        }
        return CallReady::idle;
    }
    const auto &active = slots_[at].active;
    if (!active) {
        // ADR-0086: detached queued work still drains on pause/end, even under a wait hold.
        if (!work::TaskTable::is_live(task.lifecycle) ||
            task.call.end != work::TaskCall::End::none ||
            (state.attachment == work::CallAttachment::background &&
             task.lifecycle == AYNI_TASK_PAUSED)) {
            return CallReady::other;
        }
        if (work::TaskTable::is_held(task) || task.lifecycle == AYNI_TASK_PAUSED ||
            work_.goal_lineage_boot_reset_pending(task.goal_index)) {
            return CallReady::idle;
        }
        if (mode != Mode::fenced && resources_.has_room()) {
            return CallReady::other;
        }
        return CallReady::idle;
    }
    authority::GrantCheck authority{AYNI_OK, AYNI_REASON_NONE};
    if (active_call_stopping(at, task_index, now_us, durable_seq, mode, authority)) {
        const bool actionable = submitted(active->stage)
                                    ? active->delivery->cancel == Delivery::Cancel::none
                                    : mode != Mode::fenced;
        return actionable ? CallReady::other : CallReady::idle;
    }
    // A replacement Grant can be valid but still await its durable binding (C-COMMIT-007).
    if (authority.status != AYNI_OK) {
        return CallReady::idle;
    }
    if (active->stage == Stage::queued) {
        return active->preemption != AYNI_CALL_PREEMPT_NONE &&
                       !resources_.wait_expired(active->call_id, now_us) &&
                       (!control_.allowed(at, now_us, durable_seq) ||
                        preemption_ready(at, now_us, durable_seq))
                   ? CallReady::other
                   : CallReady::queued;
    }
    if (active->stage == Stage::prepared && mailbox_.in_flight() != limits_.capacity) {
        return CallReady::prepared;
    }
    return active->stage == Stage::barrier && control_.issue_ready(at, now_us) &&
                   (state.ephemeral || state.latest.transition.commit_seq <= durable_seq)
               ? CallReady::other
               : CallReady::idle;
}

// Projects scheduling readiness for a Task-less RAM-only reflex Call (ADR-0112).
CallRuntime::CallReady CallRuntime::reflex_call_readiness(uint32_t at, uint64_t now_us,
                                                          uint64_t durable_seq, Mode mode) const {
    const auto &state = work_.calls.state(at);
    if (state.outcome != 0U) {
        return CallReady::idle;
    }
    const auto &active = slots_[at].active;
    if (!active) {
        return CallReady::other;
    }
    const auto authority = work_.check_ephemeral_call_authority(at, now_us, durable_seq);
    const bool stopping = (active->delivery && active->delivery->inhibition) ||
                          mode != Mode::running ||
                          state.cancel_cause != work::CallCancelCause::none ||
                          (authority.status != AYNI_OK && authority.status != AYNI_BUSY) ||
                          delivery_expired(at, now_us);
    if (stopping) {
        const bool actionable = submitted(active->stage)
                                    ? active->delivery->cancel == Delivery::Cancel::none
                                    : mode != Mode::fenced;
        return actionable ? CallReady::other : CallReady::idle;
    }
    if (authority.status != AYNI_OK) {
        return CallReady::idle;
    }
    if (active->stage == Stage::queued) {
        return active->preemption != AYNI_CALL_PREEMPT_NONE &&
                       !resources_.wait_expired(active->call_id, now_us) &&
                       (!control_.allowed(at, now_us, durable_seq) ||
                        preemption_ready(at, now_us, durable_seq))
                   ? CallReady::other
                   : CallReady::queued;
    }
    if (active->stage == Stage::prepared && mailbox_.in_flight() != limits_.capacity) {
        return CallReady::prepared;
    }
    return active->stage == Stage::barrier && control_.issue_ready(at, now_us) ? CallReady::other
                                                                               : CallReady::idle;
}

// Evaluates all authority, lifecycle, content and timeout stop conditions for an active call.
bool CallRuntime::active_call_stopping(uint32_t at, uint32_t task_index, uint64_t now_us,
                                       uint64_t durable_seq, Mode mode,
                                       authority::GrantCheck &out_authority) const {
    const auto &state = work_.calls.state(at);
    const auto &task = work_.tasks.at(task_index);
    const auto &active = slots_[at].active;
    const auto task_authority = work_.check_task_authority(task_index, now_us, durable_seq);
    out_authority = task_authority.status == AYNI_OK
                        ? work_.check_ephemeral_call_authority(at, now_us, durable_seq)
                        : task_authority;
    return (active->delivery && active->delivery->inhibition) ||
           !task_content_current({work_, now_us, durable_seq}, task_index) ||
           mode != Mode::running || task.call.end != work::TaskCall::End::none ||
           task.lifecycle == AYNI_TASK_PAUSED || !work::TaskTable::is_live(task.lifecycle) ||
           state.cancel_cause != work::CallCancelCause::none ||
           (out_authority.status != AYNI_OK && out_authority.status != AYNI_BUSY) ||
           delivery_expired(at, now_us);
}

} // namespace ayni::kernel
