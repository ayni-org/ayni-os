#include "foundation/attempt_identity.h"
#include "kernel/service/call/call_runtime.h"

#include <utility>

namespace ayni::kernel {
// These ports/leases are volatile owners independent of replayed Task slots.
// Fencing cancels borrowers and drains RAM-only ephemeral receipts while keeping
// durable receipts intact for the new writer after reload.
// A closing/fenced read consumes one reliable notice; terminal acknowledgement alone returns
// the exact Provider borrow. Scalar query results are deliberately not applied on this path.
CallRuntime::Advance
CallRuntime::drain_transport_receipt(const ProviderMailbox::BlockedSlots *filter) {
    ProviderMessage message{};
    ProviderReceipt receipt{};
    uint32_t size = 0;
    if (mailbox_.read_next(receipt_, limits_.result_bytes, size, message, receipt, filter) !=
        AYNI_OK) {
        return Advance::internal;
    }
    const bool terminal = provider_terminal(message.kind);
    if (mailbox_.acknowledge(receipt) == ProviderMailbox::Acknowledge::stale) {
        return Advance::internal;
    }
    if (message.body_ref.id != 0U) {
        release_body(message.body_ref); // Closing/fenced discard never persists this result.
    }
    if (matches_operation_receipt(receipt)) {
        if (terminal) {
            ledger_query_.reset();
        }
        return Advance::applied;
    }
    if (terminal) {
        for (uint32_t at = 0; at < limits_.capacity; ++at) {
            const auto &active = slots_[at].active;
            if (active && active->delivery &&
                active->delivery->reservation.attempt.request_id ==
                    receipt.reservation.attempt.request_id) {
                // Exact ended borrow also permits authoritative absence.
                static_cast<void>(control_.delivery_ended(active->call_id));
                if (active->ephemeral && message.execution_status &&
                    control_.accept_inline_stop(at, *message.execution_status)) {
                    static_cast<void>(control_.commit_stop(at));
                }
                // An unwritable close records no conclusive subscription fact.
                // Keep its units until the longest requested finite lease ends.
                if (active->collection) {
                    await_subscription_stop(at);
                }
                release(at, host_.monotonic_now_us(host_.ctx));
                break;
            }
        }
    }
    return Advance::applied;
}

CallRuntime::Advance CallRuntime::drain_transport(Mode mode) {
    ProviderMailbox::BlockedSlots fenced_blocked{};
    const ProviderMailbox::BlockedSlots *filter = nullptr;
    if (mode == Mode::fenced) {
        fenced_blocked.fill(true);
        for (uint32_t at = 0; at < limits_.capacity; ++at) {
            const auto &active = slots_[at].active;
            if (active && active->ephemeral && active->delivery) {
                fenced_blocked[active->delivery->reservation.index] = false;
            }
        }
        if (ledger_query_) {
            fenced_blocked[ledger_query_->delivery.reservation.index] = false;
        }
        filter = &fenced_blocked;
    }
    if ((mode == Mode::closing || mode == Mode::fenced) && mailbox_.has_pending(filter)) {
        return drain_transport_receipt(filter);
    }
    if (ledger_query_) {
        if (ledger_query_->stage == Stage::submitted) {
            const auto result = cancel_transport(ledger_query_->delivery);
            if (result != Advance::idle) {
                return result;
            }
        } else {
            if (!mailbox_.withdraw(ledger_query_->delivery.reservation)) {
                return Advance::internal;
            }
            ledger_query_.reset();
            return Advance::applied;
        }
    }
    for (uint32_t at = 0; at < limits_.capacity; ++at) {
        auto &active = slots_[at].active;
        if (!active || active->stage == Stage::retiring || active->stage == Stage::lease_stop) {
            continue;
        }
        if (!submitted(active->stage)) {
            if (active->delivery && !mailbox_.withdraw(active->delivery->reservation)) {
                return Advance::internal;
            }
            release(at, host_.monotonic_now_us(host_.ctx));
            return Advance::applied;
        }
        const auto result = cancel_transport(*active->delivery);
        if (result != Advance::idle) {
            return result;
        }
    }
    return Advance::idle;
}

// Cancelling only ends a request to stop; a terminal receipt still owns the
// borrow. Shared by execution, reconciliation, fencing and normal shutdown.
CallRuntime::Advance CallRuntime::cancel_transport(Delivery &delivery) {
    if (delivery.cancel == Delivery::Cancel::sent) {
        return Advance::idle;
    }
    if (delivery.provider == nullptr) {
        return Advance::internal;
    }
    delivery.cancel = Delivery::Cancel::sent;
    if (delivery.provider->cancel != nullptr) {
        const auto result = work_.call_out([&] {
            return delivery.provider->cancel(delivery.provider->context,
                                             delivery.reservation.attempt);
        });
        if (result != AYNI_SUBMIT_ACCEPTED && result != AYNI_SUBMIT_ACCEPT_UNKNOWN &&
            result != AYNI_SUBMIT_REJECTED_NOT_RETAINED) {
            return Advance::internal;
        }
    }
    return Advance::applied;
}

bool CallRuntime::transport_ready(Mode mode) const {
    if (mode == Mode::closing && mailbox_.has_pending()) {
        return true;
    }
    if (mode == Mode::fenced) {
        ProviderMailbox::BlockedSlots fenced_blocked{};
        fenced_blocked.fill(true);
        for (uint32_t at = 0; at < limits_.capacity; ++at) {
            const auto &active = slots_[at].active;
            if (active && active->ephemeral && active->delivery) {
                fenced_blocked[active->delivery->reservation.index] = false;
            }
        }
        if (ledger_query_) {
            fenced_blocked[ledger_query_->delivery.reservation.index] = false;
        }
        if (mailbox_.has_pending(&fenced_blocked)) {
            return true;
        }
    }
    if (ledger_query_ && (ledger_query_->stage != Stage::submitted ||
                          ledger_query_->delivery.cancel == Delivery::Cancel::none)) {
        return true;
    }
    for (uint32_t at = 0; at < limits_.capacity; ++at) {
        const auto &active = slots_[at].active;
        if (active && active->stage != Stage::retiring && active->stage != Stage::lease_stop &&
            (!submitted(active->stage) || active->delivery->cancel == Delivery::Cancel::none)) {
            return true;
        }
    }
    return false;
}

// A submitted attempt crossed a durable barrier, so its exact call and attempt
// must survive a validated reload. Other runtime rows can be discarded safely.
// In-place swaps remap stable identities without another metadata arena.
bool CallRuntime::rebind_restored() {
    if (ledger_query_) {
        if (ledger_query_->stage != Stage::submitted) {
            if (!mailbox_.withdraw(ledger_query_->delivery.reservation)) {
                return false;
            }
            ledger_query_.reset();
        } else {
            uint32_t index = 0;
            if (!work_.unresolved_ops.find(ledger_query_->call_id, index)) {
                return false;
            }
            const auto &query = work_.unresolved_ops.state(index).query;
            if (!query || query->phase != work::CallQuery::Phase::dispatching ||
                query->attempt.boot_epoch !=
                    ledger_query_->delivery.reservation.attempt.boot_epoch ||
                query->attempt.request_id !=
                    ledger_query_->delivery.reservation.attempt.request_id) {
                return false;
            }
        }
    }

    for (uint32_t at = 0; at < limits_.capacity; ++at) {
        const auto &active = slots_[at].active;
        if (!active) {
            continue;
        }
        if (active->stage == Stage::lease_stop) {
            uint32_t restored = 0;
            if (!work_.calls.find(active->call_id, restored)) {
                return false;
            }
            continue;
        }
        if (active->stage == Stage::retiring) {
            uint32_t restored = 0;
            if (!work_.calls.find(active->call_id, restored) ||
                !control_.holds_lease(active->call_id)) {
                return false;
            }
            continue; // No transport borrow survives, but the exact local lease still pins this
                      // work.
        }
        if (!submitted(active->stage)) {
            if (active->delivery && !mailbox_.withdraw(active->delivery->reservation)) {
                return false;
            }
            release(at, host_.monotonic_now_us(host_.ctx));
            continue;
        }
        uint32_t restored = 0;
        if (!work_.calls.find(active->call_id, restored) || !active->delivery) {
            return false;
        }
        const auto &state = work_.calls.state(restored);
        const bool query = active->delivery->purpose == Delivery::Purpose::query;
        if (query ? !work_.calls.query_pending(restored) : state.outcome != 0U) {
            return false;
        }
        const auto attempt = query ? state.query->attempt : state.attempt;
        if (!foundation::same_attempt(attempt, active->delivery->reservation.attempt)) {
            return false;
        }
    }
    for (uint32_t at = 0; at < limits_.capacity; ++at) {
        while (slots_[at].active) {
            uint32_t restored = 0;
            // The first pass proved this exact stable identity remains present.
            static_cast<void>(work_.calls.find(slots_[at].active->call_id, restored));
            if (restored == at) {
                break;
            }
            std::swap(slots_[at].active, slots_[restored].active);
        }
    }
    return true;
}
} // namespace ayni::kernel
