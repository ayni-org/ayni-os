#include "foundation/attempt_identity.h"
#include "kernel/call_runtime.h"

#include <utility>

namespace ayni::kernel {
// These ports/leases are volatile owners independent of replayed Task slots.
// Fencing cancels borrowers but keeps every pending result for the new writer.
CallRuntime::Advance CallRuntime::drain_transport(Mode mode) {
    if (mode == Mode::closing && mailbox_.has_pending()) {
        ProviderMessage message{};
        ProviderReceipt receipt{};
        uint32_t size = 0;
        if (mailbox_.read_next(receipt_, limits_.result_bytes, size, message, receipt) != AYNI_OK) {
            return Advance::internal;
        }
        const bool terminal = message.kind != ProviderMessageKind::progress &&
                              message.kind != ProviderMessageKind::cannot_cancel &&
                              message.kind != ProviderMessageKind::cancel_unknown;
        if (mailbox_.acknowledge(receipt) == ProviderMailbox::Acknowledge::stale) {
            return Advance::internal;
        }
        if (terminal) {
            for (uint32_t at = 0; at < limits_.capacity; ++at) {
                const auto &active = slots_[at].active;
                if (active && active->delivery &&
                    active->delivery->reservation.attempt.request_id ==
                        receipt.reservation.attempt.request_id) {
                    release(at, host_.monotonic_now_us(host_.ctx));
                    break;
                }
            }
        }
        return Advance::applied;
    }
    for (uint32_t at = 0; at < limits_.capacity; ++at) {
        auto &active = slots_[at].active;
        if (!active) {
            continue;
        }
        if (active->stage != Stage::submitted) {
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
        work_.calling_out = true;
        const auto result =
            delivery.provider->cancel(delivery.provider->context, delivery.reservation.attempt);
        work_.calling_out = false;
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
    for (uint32_t at = 0; at < limits_.capacity; ++at) {
        const auto &active = slots_[at].active;
        if (active && (active->stage != Stage::submitted ||
                       active->delivery->cancel == Delivery::Cancel::none)) {
            return true;
        }
    }
    return false;
}

// A submitted attempt crossed a durable barrier, so its exact call and attempt
// must survive a validated reload. Other runtime rows can be discarded safely.
// In-place swaps remap stable identities without another metadata arena.
bool CallRuntime::rebind_restored() {
    for (uint32_t at = 0; at < limits_.capacity; ++at) {
        const auto &active = slots_[at].active;
        if (!active) {
            continue;
        }
        if (active->stage != Stage::submitted) {
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
