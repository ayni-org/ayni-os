// Experiment-local bounded latest-value lane, with disjoint payload ownership (ADR-0114).
#pragma once
#include <array>
#include <atomic>
#include <cstdint>

namespace wifichat {
// Multiple non-ISR producers, one non-reentrant consumer. Contention returns false;
// no waiting on a preempted writer. Only the 32-bit state word is atomic on Xtensa.
// A reader pins latest before copying; writers only touch neither-latest-nor-pinned.
template <class T> class LatestSlot {
  public:
    template <class Prepare, class Published> bool write(Prepare prepare, Published published) {
        const auto state = state_.fetch_or(writing, std::memory_order_acquire);
        if (state & writing) {
            return false;
        }
        const auto previous = state & index_mask;
        const auto pinned = (state >> reader_shift) & index_mask;
        uint32_t slot = 0;
        while (slot == previous || slot == pinned) {
            ++slot;
        }
        if (!prepare(buffers_[slot])) {
            state_.fetch_and(~writing, std::memory_order_release);
            return false;
        }
        auto current = state_.load(std::memory_order_relaxed);
        for (;;) {
            const auto next = (current & ~index_mask) | slot | dirty;
            if (state_.compare_exchange_weak(current, next, std::memory_order_release,
                                             std::memory_order_relaxed)) {
                // Keep producer ownership through receipt publication; another producer
                // cannot publish an older receipt after a newer accepted target.
                published(buffers_[previous], (current & dirty) != 0, buffers_[slot]);
                state_.fetch_and(~writing, std::memory_order_release);
                return true;
            }
        }
    }
    bool publish(const T &value) {
        return write(
            [&](T &out) {
                out = value;
                return true;
            },
            [](const T &, bool, const T &) {});
    }
    // consume=false reads the latest snapshot even when clean; consume=true drains once.
    bool read(T &out, bool consume = true) {
        auto state = state_.load(std::memory_order_acquire);
        for (;;) {
            if ((consume && !(state & dirty)) ||
                ((state >> reader_shift) & index_mask) != no_reader) {
                return false;
            }
            const auto pinned = state & index_mask;
            auto next = (state & ~(index_mask << reader_shift)) | (pinned << reader_shift);
            if (consume) {
                next &= ~dirty;
            }
            if (state_.compare_exchange_weak(state, next, std::memory_order_acquire,
                                             std::memory_order_relaxed)) {
                out = buffers_[pinned];
                state_.fetch_or(no_reader << reader_shift, std::memory_order_release);
                return true;
            }
        }
    }

  private:
    static constexpr uint32_t count = 3, index_mask = 3, no_reader = 3, reader_shift = 2;
    static constexpr uint32_t writing = 1U << 4U, dirty = 1U << 5U;
    // IDF's PSRAM workaround reports is_always_lock_free=false even for internal memory.
    // The composition places ControlMailbox in internal RAM so its 32-bit primitives use
    // the IDF native S32C1I path; no 64-bit atomic or software payload seqlock is required.
    std::atomic<uint32_t> state_{no_reader << reader_shift};
    std::array<T, count> buffers_{};
};
} // namespace wifichat
