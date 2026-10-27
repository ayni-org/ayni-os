#include "kernel/ports/provider_mailbox.h"
#include "foundation/attempt_identity.h"
#include "kernel/ports/ingress_lock.h"

#include <cstring>
#include <new>

namespace ayni::kernel {
namespace {
constexpr uint32_t payload_lanes = 2; // Independent replaceable progress and immutable terminal.
constexpr uint32_t progress_lane = 0;
constexpr uint32_t terminal_lane = 1;
} // namespace

std::size_t ProviderMailbox::storage_size(Limits limits) {
    return sizeof(Slot) * limits.capacity +
           static_cast<std::size_t>(limits.capacity) * payload_lanes * limits.payload_bytes;
}

ProviderMailbox::ProviderMailbox(void *storage, Limits limits, const ayni_host_v1 &host)
    : host_(host), slots_(static_cast<Slot *>(storage)),
      payloads_(limits.capacity == 0U ? nullptr
                                      : reinterpret_cast<uint8_t *>(slots_ + limits.capacity)),
      capacity_(limits.capacity), payload_bytes_(limits.payload_bytes) {
    static_assert(alignof(Slot) <= storage_alignment, "caller arena aligns slots");
    for (uint32_t index = 0; index < capacity_; ++index) {
        new (&slots_[index]) Slot{{this, index}, {}};
    }
}

ayni_status_t ProviderMailbox::check_message(const ProviderMessage &message) const {
    const bool progress = message.kind == ProviderMessageKind::progress;
    const bool written = message.writer != nullptr;
    const bool lease = provider_subscription_control(message.kind);
    if ((!progress && !lease && !provider_cancel_control(message.kind) &&
         !provider_terminal(message.kind)) ||
        ((progress || lease) ? message.sequence == 0U : message.sequence != 0U) ||
        (written ? message.payload.data != nullptr || message.writer->write == nullptr ||
                       !provider_terminal(message.kind)
                 : message.payload.size != 0U && message.payload.data == nullptr) ||
        ((provider_cancel_control(message.kind) || lease) && message.payload.size != 0U)) {
        return AYNI_INVALID_ARGUMENT;
    }
    return message.payload.size > payload_bytes_ ? AYNI_CAPACITY_EXCEEDED : AYNI_OK;
}

ProviderMailbox::Attempt *ProviderMailbox::find(ProviderReservation reservation) {
    if (reservation.index >= capacity_) {
        return nullptr;
    }
    auto &slot = slots_[reservation.index];
    return slot.attempt && foundation::same_attempt(slot.attempt->identity, reservation.attempt)
               ? &*slot.attempt
               : nullptr;
}

uint8_t *ProviderMailbox::payload_of(uint32_t index, ProviderMessageKind kind) const {
    const auto lane = kind == ProviderMessageKind::progress ? progress_lane : terminal_lane;
    return payloads_ + (static_cast<std::size_t>(index) * payload_lanes + lane) * payload_bytes_;
}

// Reserve all delivery capacity before any Provider is allowed to keep work.
// Failed admission leaves both identity high-water mark and caller output intact.
ayni_status_t ProviderMailbox::reserve(ayni_attempt_ref_v1 identity, ProviderReservation &out,
                                       uint32_t written_limit) {
    const IngressGuard guard(host_);
    if (identity.boot_epoch == 0U || identity.request_id <= last_request_id_) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (in_flight_ == capacity_) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    for (uint32_t index = 0; index < capacity_; ++index) {
        if (slots_[index].attempt) {
            continue;
        }
        slots_[index].attempt = Attempt{identity, 0, {}, {}, {}, written_limit};
        ++in_flight_;
        last_request_id_ = identity.request_id;
        out = {index, identity};
        return AYNI_OK;
    }
    return AYNI_INTERNAL; // in_flight_ and optional slot occupancy must agree.
}

// Copy under the ingress lock, then wake outside it. No queue capacity is
// consulted: reliable delivery was reserved with this exact attempt (C-PROV-006).
ayni_status_t ProviderMailbox::post(ProviderReservation reservation,
                                    const ProviderMessage &message) {
    const auto status = check_message(message);
    if (status != AYNI_OK) {
        return status;
    }
    {
        const IngressGuard guard(host_);
        auto *attempt = find(reservation);
        if (attempt == nullptr || attempt->terminal) {
            return AYNI_INVALID_STATE;
        }
        // A written list is bounded by its call's own result limit, not only by the lane.
        if (message.writer != nullptr && message.payload.size > attempt->written_limit) {
            return attempt->written_limit == 0U ? AYNI_INVALID_ARGUMENT : AYNI_CAPACITY_EXCEEDED;
        }
        const Result result{message.kind,         message.status, message.reason,
                            message.payload.size, message.effect, message.query_result,
                            message.body};
        if (provider_subscription_control(message.kind)) {
            auto &control = attempt->lease_control;
            if (!control || control->result || control->sequence != message.sequence ||
                control->kind != message.kind) {
                return AYNI_INVALID_STATE;
            }
            control->result = result;
        } else if (provider_cancel_control(message.kind)) {
            if (attempt->cancel) {
                return AYNI_INVALID_STATE;
            }
            attempt->cancel = Cancel{result, Cancel::Stage::pending};
        } else {
            if (message.kind == ProviderMessageKind::progress) {
                if (message.sequence <= attempt->last_progress_sequence) {
                    return AYNI_OK;
                }
                attempt->last_progress_sequence = message.sequence;
                attempt->progress = result;
            } else {
                attempt->terminal = result;
                // Terminal supersedes optional progress, never accepted cancel control.
                attempt->progress.reset();
            }
            auto *lane = payload_of(reservation.index, message.kind);
            if (message.writer != nullptr) {
                message.writer->write(message.writer->context, lane, message.payload.size);
            } else if (message.payload.size != 0U) {
                std::memcpy(lane, message.payload.data, message.payload.size);
            }
        }
    }
    host_.wake(host_.ctx);
    return AYNI_OK;
}

const ProviderMailbox::Result *ProviderMailbox::pending(const Attempt &attempt) {
    if (attempt.cancel && attempt.cancel->stage == Cancel::Stage::pending) {
        return &attempt.cancel->result;
    }
    if (attempt.lease_control && attempt.lease_control->result) {
        return &*attempt.lease_control->result;
    }
    if (attempt.terminal) {
        return &*attempt.terminal;
    }
    return attempt.progress ? &*attempt.progress : nullptr;
}

bool ProviderMailbox::expect_subscription_control(ProviderReservation reservation,
                                                  ProviderMessageKind kind, uint64_t sequence) {
    const IngressGuard guard(host_);
    auto *attempt = find(reservation);
    if (!attempt || attempt->terminal || attempt->lease_control ||
        !provider_subscription_control(kind) || attempt->last_lease_sequence == UINT64_MAX ||
        sequence != attempt->last_lease_sequence + 1 ||
        (kind == ProviderMessageKind::subscription_started) != (sequence == 1)) {
        return false;
    }
    attempt->lease_control = LeaseControl{kind, sequence, {}};
    return true;
}

ayni_provider_completion_t *ProviderMailbox::completion(ProviderReservation reservation) {
    const IngressGuard guard(host_);
    return find(reservation) == nullptr ? nullptr : &slots_[reservation.index].target;
}

bool ProviderMailbox::owns(const ayni_provider_completion_t *target) const {
    return target != nullptr && target->pool == this && target->index < capacity_ &&
           &slots_[target->index].target == target;
}

// Reading owns a bounded copy, not a destructive dequeue. The caller may
// wait for storage indefinitely without losing either reliable receipt.
ayni_status_t ProviderMailbox::read_next(uint8_t *payload, uint32_t capacity, uint32_t &required,
                                         ProviderMessage &out, ProviderReceipt &receipt,
                                         const BlockedSlots *blocked) {
    if (payload == nullptr && capacity != 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    const IngressGuard guard(host_);
    for (uint32_t visited = 0; visited < capacity_; ++visited) {
        const uint32_t index = (cursor_ + visited) % capacity_;
        const auto &slot = slots_[index];
        const auto *result = slot.attempt ? pending(*slot.attempt) : nullptr;
        if (result == nullptr || (blocked != nullptr && (*blocked)[index])) {
            continue;
        }
        if (result->size > capacity) {
            required = result->size;
            return AYNI_BUFFER_TOO_SMALL;
        }
        if (result->size != 0U) {
            std::memcpy(payload, payload_of(index, result->kind), result->size);
        }
        const auto sequence =
            result->kind == ProviderMessageKind::progress ? slot.attempt->last_progress_sequence
            : provider_subscription_control(result->kind) ? slot.attempt->lease_control->sequence
                                                          : 0U;
        out = {result->kind,
               sequence,
               result->status,
               result->reason,
               {payload, result->size},
               result->effect,
               result->query_result,
               result->body};
        receipt = {{index, slot.attempt->identity}, result->kind, sequence};
        required = result->size;
        cursor_ = (index + 1U) % capacity_;
        return AYNI_OK;
    }
    return AYNI_NOT_FOUND;
}

// Acknowledgement follows the consumer's commit admission; no producer can
// clear reliable data. A newer progress generation is never erased by an old copy.
ProviderMailbox::Acknowledge ProviderMailbox::acknowledge(const ProviderReceipt &receipt) {
    const IngressGuard guard(host_);
    auto *attempt = find(receipt.reservation);
    if (attempt == nullptr) {
        return Acknowledge::stale;
    }
    if (receipt.kind == ProviderMessageKind::progress) {
        if (receipt.sequence == 0U || receipt.sequence > attempt->last_progress_sequence) {
            return Acknowledge::stale;
        }
        if (receipt.sequence < attempt->last_progress_sequence || attempt->terminal) {
            return Acknowledge::superseded;
        }
        if (!attempt->progress) {
            return Acknowledge::stale;
        }
        attempt->progress.reset();
        return Acknowledge::consumed;
    }
    if (provider_subscription_control(receipt.kind)) {
        const auto &control = attempt->lease_control;
        if (!control || !control->result || control->kind != receipt.kind ||
            control->sequence != receipt.sequence) {
            return Acknowledge::stale;
        }
        attempt->last_lease_sequence = receipt.sequence;
        attempt->lease_control.reset();
        return Acknowledge::consumed;
    }
    if (receipt.sequence != 0U) {
        return Acknowledge::stale;
    }
    if (provider_cancel_control(receipt.kind)) {
        if (!attempt->cancel || attempt->cancel->stage != Cancel::Stage::pending ||
            attempt->cancel->result.kind != receipt.kind) {
            return Acknowledge::stale;
        }
        attempt->cancel->stage = Cancel::Stage::acknowledged;
        return Acknowledge::consumed;
    }
    if (!attempt->terminal || attempt->terminal->kind != receipt.kind ||
        (attempt->cancel && attempt->cancel->stage == Cancel::Stage::pending) ||
        (attempt->lease_control && attempt->lease_control->result)) {
        return Acknowledge::stale;
    }
    release(receipt.reservation.index);
    return Acknowledge::consumed;
}

bool ProviderMailbox::withdraw(ProviderReservation reservation) {
    const IngressGuard guard(host_);
    const auto *attempt = find(reservation);
    if (attempt == nullptr || attempt->terminal || attempt->cancel ||
        attempt->last_progress_sequence != 0U || attempt->last_lease_sequence != 0U ||
        (attempt->lease_control && attempt->lease_control->result)) {
        return false;
    }
    release(reservation.index);
    return true;
}

bool ProviderMailbox::has_pending(const BlockedSlots *blocked) const {
    const IngressGuard guard(host_);
    for (uint32_t index = 0; index < capacity_; ++index) {
        if ((blocked == nullptr || !(*blocked)[index]) && slots_[index].attempt &&
            pending(*slots_[index].attempt)) {
            return true;
        }
    }
    return false;
}

void ProviderMailbox::release(uint32_t index) {
    // Clear bounded payload storage before reuse; this is not durable secure erasure.
    std::memset(payload_of(index, ProviderMessageKind::progress), 0,
                static_cast<std::size_t>(payload_lanes) * payload_bytes_);
    slots_[index].attempt.reset();
    --in_flight_;
}
} // namespace ayni::kernel
