// Per-source triple-buffered lock-free latest-wins observation slots (ADR-0112).
// Any non-ISR Host thread writes without blocking or mutex contention; the single Kernel owner
// drains dirty slots at the start of ayni_step() before refreshing gates or evaluating reflexes.
// Disjoint buffer ownership guarantees zero C++17 data races (TSan clean).
#ifndef AYNI_CORE_KERNEL_INGRESS_OBSERVATION_SLOTS_H
#define AYNI_CORE_KERNEL_INGRESS_OBSERVATION_SLOTS_H
#include "catalog/observation/source_registry.h"
#include "knowledge/observation/observation_input.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ayni::kernel {

class ObservationSlots {
  public:
    // Fixed arena placement constants shared by kernel_layout and construction.
    static constexpr std::size_t storage_alignment = alignof(uint64_t);
    static std::size_t storage_size(uint32_t sources);

    // Caller arena and immutable source registry outlive all producers and this object.
    ObservationSlots(void *storage, uint32_t sources, const catalog::ObservationSources &registry);
    ObservationSlots(const ObservationSlots &) = delete;
    ObservationSlots &operator=(const ObservationSlots &) = delete;

    // Owner: opens admission for boot_epoch once RUNNING starts.
    void begin_boot(uint64_t boot_epoch);
    // Owner: closes admission and discards unread slots without touching producer threads.
    void close();
    // Any non-ISR thread: validates immutable source rules and publishes into a disjoint buffer.
    // Sets out_wake = true when transitioning clean->dirty so caller wakes owner outside locks.
    ayni_status_t publish_latest(uint64_t boot_epoch, const knowledge::ObservationInput &input,
                                 bool &out_wake);
    // Owner: drains one dirty source slot race-free; returns false if clean.
    bool drain(uint32_t source_index, knowledge::ObservationInput &out);
    // Owner: returns true iff any source slot currently holds an unread sample.
    bool any_dirty() const;
    // Owner: records owner-side acceptance or clock/sequence rejection of drained samples.
    void record_applied() { applied_.fetch_add(1U, std::memory_order_relaxed); }
    void record_discarded() { discarded_.fetch_add(1U, std::memory_order_relaxed); }
    // Any non-ISR thread: lock-free diagnostic snapshot.
    ayni_observation_slot_info_v1 info() const;

  private:
    static constexpr uint32_t buffer_count = 3; // Guaranteed disjoint: latest, reading, writing.
    static constexpr uint32_t slot_mask = 0x3U;
    static constexpr uint32_t reading_shift = 2U;
    static constexpr uint32_t none_slot = 3U; // Sentinel in reading bits when owner is idle.
    static constexpr uint32_t writing_bit = 1U << 4U;
    static constexpr uint32_t dirty_bit = 1U << 5U;
    // Bounded CAS spin prevents FreeRTOS priority-inversion livelock on same-source contention.
    static constexpr uint32_t writer_spin_limit = 64U;

    // Fixed per-source triple buffer; state_ synchronizes disjoint writer/reader indices.
    struct Slot {
        std::atomic<uint32_t> state{none_slot << reading_shift};
        std::array<knowledge::ObservationInput, buffer_count> buffers{};
    };

    const catalog::ObservationSources &registry_; // Immutable after ayni_start().
    Slot *slots_;                                 // Arena array of capacity_ entries, or null.
    const uint32_t capacity_;                     // Configured observation_source_capacity.
    std::atomic<uint64_t> boot_{0};               // 0 when closed; positive boot_epoch in RUNNING.
    std::atomic<uint64_t> published_{0};          // Total successful latest-slot writes.
    std::atomic<uint64_t> overwritten_{0};        // Writes that superseded an unread sample.
    std::atomic<uint64_t> applied_{0};            // Drained samples accepted by ObservationTable.
    std::atomic<uint64_t> discarded_{0};          // Drained samples rejected by owner checks.
};

} // namespace ayni::kernel
#endif
