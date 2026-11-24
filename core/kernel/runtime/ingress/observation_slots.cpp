#include "kernel/runtime/ingress/observation_slots.h"
#include <new>

namespace ayni::kernel {

std::size_t ObservationSlots::storage_size(uint32_t sources) {
    return static_cast<std::size_t>(sources) * sizeof(Slot);
}

ObservationSlots::ObservationSlots(void *storage, uint32_t sources,
                                   const catalog::ObservationSources &registry)
    : registry_(registry), slots_(sources == 0U ? nullptr : static_cast<Slot *>(storage)),
      capacity_(sources) {
    static_assert(alignof(Slot) <= storage_alignment, "observation slot arena alignment");
    for (uint32_t at = 0; at < capacity_; ++at) {
        new (slots_ + at) Slot{};
    }
}

void ObservationSlots::begin_boot(uint64_t boot_epoch) {
    if (capacity_ != 0U) {
        knowledge::ObservationInput dropped{};
        for (uint32_t at = 0; at < capacity_; ++at) {
            static_cast<void>(drain(at, dropped));
        }
        boot_.store(boot_epoch, std::memory_order_release);
    }
}

void ObservationSlots::close() {
    boot_.store(0U, std::memory_order_release);
    knowledge::ObservationInput dropped{};
    for (uint32_t at = 0; at < capacity_; ++at) {
        if (drain(at, dropped)) {
            record_discarded();
        }
    }
}

// Publishes one validated Host sample into a guaranteed-disjoint triple buffer without mutexes.
// Only one writer holds writing_bit per source; owner only pins latest, so choosing W != latest
// and W != reading guarantees exclusive ownership of buffers[W] (ADR-0112).
ayni_status_t ObservationSlots::publish_latest(uint64_t boot_epoch,
                                               const knowledge::ObservationInput &input,
                                               bool &out_wake) {
    out_wake = false;
    if (capacity_ == 0U) {
        return AYNI_UNSUPPORTED;
    }
    const uint64_t active_boot = boot_.load(std::memory_order_acquire);
    if (active_boot == 0U) {
        return AYNI_NOT_READY;
    }
    if (boot_epoch != active_boot || (input.media && input.media->boot_epoch != active_boot)) {
        return AYNI_STALE;
    }
    uint32_t index = 0;
    if (!registry_.find({input.source.data(), input.source.size()}, index) || index >= capacity_) {
        return AYNI_NOT_FOUND;
    }
    if (const auto status = knowledge::check_observation_source(input, registry_.at(index));
        status != AYNI_OK) {
        return status;
    }
    Slot &slot = slots_[index];
    uint32_t cur = slot.state.load(std::memory_order_acquire);
    bool claimed = false;
    for (uint32_t spin = 0; spin < writer_spin_limit; ++spin) {
        if ((cur & writing_bit) != 0U) {
            cur = slot.state.load(std::memory_order_acquire);
            continue;
        }
        if (slot.state.compare_exchange_weak(cur, cur | writing_bit, std::memory_order_acquire,
                                             std::memory_order_relaxed)) {
            claimed = true;
            break;
        }
    }
    if (!claimed) {
        return AYNI_BUSY;
    }
    const uint32_t latest = cur & slot_mask;
    const uint32_t reading = (cur >> reading_shift) & slot_mask;
    uint32_t write_idx = 0;
    while (write_idx == latest || write_idx == reading) {
        ++write_idx;
    }
    slot.buffers[write_idx] = input;
    bool overwrote = false;
    uint32_t s = slot.state.load(std::memory_order_relaxed);
    for (;;) {
        overwrote = (s & dirty_bit) != 0U;
        const uint32_t next = (s & ~slot_mask & ~writing_bit) | write_idx | dirty_bit;
        if (slot.state.compare_exchange_weak(s, next, std::memory_order_release,
                                             std::memory_order_relaxed)) {
            break;
        }
    }
    published_.fetch_add(1U, std::memory_order_relaxed);
    if (overwrote) {
        overwritten_.fetch_add(1U, std::memory_order_relaxed);
    }
    // Wake owner only on clean->dirty transition to avoid wake storms at 200 Hz sensor rates.
    out_wake = !overwrote;
    return AYNI_OK;
}

bool ObservationSlots::drain(uint32_t source_index, knowledge::ObservationInput &out) {
    if (source_index >= capacity_) {
        return false;
    }
    Slot &slot = slots_[source_index];
    uint32_t s = slot.state.load(std::memory_order_acquire);
    if ((s & dirty_bit) == 0U) {
        return false;
    }
    uint32_t pinned = 0;
    for (;;) {
        if ((s & dirty_bit) == 0U) {
            return false;
        }
        pinned = s & slot_mask;
        const uint32_t next =
            (s & ~(slot_mask << reading_shift) & ~dirty_bit) | (pinned << reading_shift);
        if (slot.state.compare_exchange_weak(s, next, std::memory_order_acquire,
                                             std::memory_order_relaxed)) {
            break;
        }
    }
    out = slot.buffers[pinned];
    s = slot.state.load(std::memory_order_relaxed);
    for (;;) {
        const uint32_t next = (s & ~(slot_mask << reading_shift)) | (none_slot << reading_shift);
        if (slot.state.compare_exchange_weak(s, next, std::memory_order_release,
                                             std::memory_order_relaxed)) {
            break;
        }
    }
    return true;
}

bool ObservationSlots::any_dirty() const {
    for (uint32_t at = 0; at < capacity_; ++at) {
        if ((slots_[at].state.load(std::memory_order_acquire) & dirty_bit) != 0U) {
            return true;
        }
    }
    return false;
}

ayni_observation_slot_info_v1 ObservationSlots::info() const {
    return {
        sizeof(ayni_observation_slot_info_v1),      registry_.size(),
        published_.load(std::memory_order_relaxed), overwritten_.load(std::memory_order_relaxed),
        applied_.load(std::memory_order_relaxed),   discarded_.load(std::memory_order_relaxed)};
}

} // namespace ayni::kernel
