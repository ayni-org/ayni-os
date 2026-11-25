// ADR-0115: original Provider proof, dedicated bounded transport, scalar conclusion only.
#include "foundation/attempt_identity.h"
#include "foundation/deadline.h"
#include "kernel/service/call/call_runtime.h"
namespace ayni::kernel {
namespace {
ayni_string_v1 view(const catalog::CatalogId &id) {
    return {id.data(), id.size()};
}
} // namespace
CallRuntime::Advance CallRuntime::end_operation_query(uint32_t index, ayni_status_t status,
                                                      ayni_reason_t reason) {
    return work_.unresolved_ops.end_query(
               index, status, reason, {work_.ids.boot_epoch(), work_.transitions.next()}) == AYNI_OK
               ? Advance::applied
               : Advance::internal;
}
// Preserve the complete original contract proof before reserving I/O; no Task, body or lease
// is needed for this read-only query, and no replacement Binding is selected.
CallRuntime::Advance CallRuntime::prepare_operation_query(uint32_t index) {
    const auto &id = work_.unresolved_ops.identity(index);
    uint32_t provider_index = 0;
    if (!catalogs_.providers.find(view(id.provider_id), id.provider_version, provider_index)) {
        return end_operation_query(index, AYNI_UNSUPPORTED_VERSION, AYNI_REASON_NONE);
    }
    const auto &provider = catalogs_.providers.at(provider_index);
    if (!provider.query ||
        !catalog::ProviderRegistry::matches_saved_call(
            provider, view(id.capability_id), id.capability_version,
            work_.unresolved_ops.contract(index), view(id.endpoint), view(id.destination))) {
        return end_operation_query(index, AYNI_UNSUPPORTED_VERSION, AYNI_REASON_NONE);
    }
    ayni_attempt_ref_v1 attempt{};
    if (!attempts_.next(work_.ids.boot_epoch(), attempt)) {
        return Advance::attempt_exhausted;
    }
    ProviderReservation reservation{};
    // Active Calls have at most call_capacity reservations; enabled ledger adds exactly one.
    if (mailbox_.reserve(attempt, reservation) != AYNI_OK) {
        return Advance::internal;
    }
    if (work_.unresolved_ops.begin_query(
            index, attempt, {work_.ids.boot_epoch(), work_.transitions.next()}) != AYNI_OK) {
        static_cast<void>(mailbox_.withdraw(reservation));
        return Advance::internal;
    }
    ledger_query_ = LedgerQuery{id.call_id, Stage::barrier,
                                Delivery{reservation, &provider, UINT64_MAX, Delivery::Cancel::none,
                                         Delivery::Purpose::query}};
    return Advance::applied;
}
CallRuntime::Advance CallRuntime::dispatch_operation_query(uint32_t index, uint64_t now_us) {
    auto &query = *ledger_query_;
    query.stage = Stage::submitted;
    auto &delivery = query.delivery;
    delivery.deadline_us =
        foundation::deadline(now_us, uint64_t{work_.unresolved_ops.contract(index).timeout_ms} *
                                         microseconds_per_millisecond);
    const auto result = work_.call_out([&] {
        return delivery.provider->query(
            delivery.provider->context, work_.unresolved_ops.identity(index).operation_id,
            delivery.reservation.attempt, mailbox_.completion(delivery.reservation));
    });
    if (result == AYNI_SUBMIT_REJECTED_NOT_RETAINED && mailbox_.withdraw(delivery.reservation)) {
        ledger_query_.reset();
        return end_operation_query(index, AYNI_NOT_READY, AYNI_REASON_NONE);
    }
    return result == AYNI_SUBMIT_ACCEPTED || result == AYNI_SUBMIT_ACCEPT_UNKNOWN ||
                   result == AYNI_SUBMIT_REJECTED_NOT_RETAINED
               ? Advance::applied
               : Advance::internal;
}
// A restored dispatching query is never repeated. Only an explicit new owner command may read
// again.
CallRuntime::Advance CallRuntime::advance_operation_query(uint64_t now_us, uint64_t durable_seq,
                                                          Mode mode) {
    auto &ledger = work_.unresolved_ops;
    if (ledger_query_) {
        uint32_t index = 0;
        if (!ledger.find(ledger_query_->call_id, index)) {
            return Advance::internal;
        }
        auto &query = *ledger_query_;
        if (mode != Mode::running || now_us >= query.delivery.deadline_us) {
            if (query.stage == Stage::submitted) {
                return cancel_transport(query.delivery);
            }
            if (!mailbox_.withdraw(query.delivery.reservation)) {
                return Advance::internal;
            }
            ledger_query_.reset();
            return end_operation_query(index, AYNI_RESULT_UNKNOWN, AYNI_REASON_NONE);
        }
        return query.stage == Stage::barrier &&
                       ledger.state(index).latest.transition.commit_seq <= durable_seq
                   ? dispatch_operation_query(index, now_us)
                   : Advance::idle;
    }
    uint32_t selected = work::OperationLedger::no_index;
    for (uint32_t at = 0; at < ledger.capacity(); ++at) {
        if (ledger.occupied(at) && ledger.query_pending(at) &&
            (selected == work::OperationLedger::no_index ||
             ledger.state(at).query->requested.transition.commit_seq <
                 ledger.state(selected).query->requested.transition.commit_seq)) {
            selected = at;
        }
    }
    if (selected == work::OperationLedger::no_index) {
        return Advance::idle;
    }
    if (mode != Mode::running ||
        ledger.state(selected).query->phase == work::CallQuery::Phase::dispatching) {
        return end_operation_query(selected, AYNI_RESULT_UNKNOWN, AYNI_REASON_NONE);
    }
    return ledger.state(selected).latest.transition.commit_seq <= durable_seq
               ? prepare_operation_query(selected)
               : Advance::idle;
}
// Submitted queries wake from their mailbox or finite deadline; pending remote I/O alone
// must not keep a bounded owner loop spinning.
bool CallRuntime::operation_query_ready(uint64_t now_us, uint64_t durable_seq, Mode mode) const {
    const auto &ledger = work_.unresolved_ops;
    if (ledger_query_) {
        uint32_t index = 0;
        if (!ledger.find(ledger_query_->call_id, index)) {
            return true;
        }
        const auto &query = *ledger_query_;
        if (mode != Mode::running || now_us >= query.delivery.deadline_us) {
            return query.stage != Stage::submitted ||
                   query.delivery.cancel == Delivery::Cancel::none;
        }
        return query.stage == Stage::barrier &&
               ledger.state(index).latest.transition.commit_seq <= durable_seq;
    }
    for (uint32_t at = 0; at < ledger.capacity(); ++at) {
        if (ledger.occupied(at) && ledger.query_pending(at) &&
            (mode != Mode::running ||
             ledger.state(at).query->phase == work::CallQuery::Phase::dispatching ||
             ledger.state(at).latest.transition.commit_seq <= durable_seq)) {
            return true;
        }
    }
    return false;
}
bool CallRuntime::matches_operation_receipt(const ProviderReceipt &receipt) const {
    return ledger_query_ &&
           ledger_query_->delivery.reservation.index == receipt.reservation.index &&
           foundation::same_attempt(ledger_query_->delivery.reservation.attempt,
                                    receipt.reservation.attempt);
}
// Only terminal scalar evidence can close an operation. Body references are discarded via their
// owner.
CallRuntime::Advance CallRuntime::consume_operation_query(const ProviderMessage &message,
                                                          const ProviderReceipt &receipt) {
    if (!provider_terminal(message.kind)) {
        return mailbox_.acknowledge(receipt) == ProviderMailbox::Acknowledge::stale
                   ? Advance::internal
                   : Advance::applied;
    }
    uint32_t index = 0;
    auto &ledger = work_.unresolved_ops;
    if (!ledger_query_ || !ledger.find(ledger_query_->call_id, index)) {
        return Advance::internal;
    }
    const auto stamp = work::ActionStamp{work_.ids.boot_epoch(), work_.transitions.next()};
    const auto outcome =
        message.kind == ProviderMessageKind::completed ? AYNI_CALL_COMPLETED : AYNI_CALL_FAILED;
    const bool valid = message.body != work::CallResultKind::tool_calls && !message.body_ref.id &&
                       receipt.reservation.attempt.boot_epoch == work_.ids.boot_epoch() &&
                       (message.kind == ProviderMessageKind::completed ||
                        message.kind == ProviderMessageKind::failed);
    auto status =
        valid ? ledger.reconcile_and_remove(index, message.query_result,
                                            {outcome,
                                             message.status,
                                             message.reason,
                                             static_cast<work::ActionEffect>(message.effect),
                                             {}},
                                            stamp)
              : AYNI_INVALID_ARGUMENT;
    if (status == AYNI_INVALID_ARGUMENT) {
        status = ledger.end_query(index, AYNI_RESULT_UNKNOWN, AYNI_REASON_PROVIDER_RESULT, stamp);
    }
    if (status != AYNI_OK || mailbox_.acknowledge(receipt) == ProviderMailbox::Acknowledge::stale) {
        return Advance::internal;
    }
    if (message.body_ref.id) {
        release_body(message.body_ref);
    }
    ledger_query_.reset();
    work_.forget_due_work();
    return Advance::applied;
}
} // namespace ayni::kernel
