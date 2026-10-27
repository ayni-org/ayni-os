// Bounded queue of events posted by Host threads (C-LIMIT-008, ADR-0015).
//
// Entries live in caller memory sized by storage_size(); the queue never
// grows. Order is a ring of entry indices, so evicting an entry from the
// middle moves only indices, not bodies. Every member function must be called
// with the ingress lock held: producers push from any thread, the owner pops.
// The longest call, a push into a full queue, reads each queued header once
// and then moves fewer than capacity indices.

#ifndef AYNI_CORE_KERNEL_INGRESS_EVENT_QUEUE_H
#define AYNI_CORE_KERNEL_INGRESS_EVENT_QUEUE_H

#include <ayni/ayni.h>

#include <cstddef>
#include <cstdint>

namespace ayni::kernel {

// One queued event. The kind values and which kinds are droppable are decided
// by the caller; the queue only applies the drop policy.
struct EventHeader {
    uint64_t subscription_id;
    uint64_t sequence;
    uint64_t captured_at_us;
    uint32_t kind;
    uint32_t generation;
    // Once the queue is full, a droppable event replaces the newest queued
    // one with the same subscription and nonzero key; 0 never replaces.
    uint32_t coalesce_key;
    // Set by the queue: how many earlier values this entry overwrote. Ignored
    // on push.
    uint32_t replaced;
    uint32_t body_size;
    // Telemetry-like events that may be replaced or evicted when the queue is
    // full. Others are never dropped: a full queue rejects them instead.
    bool droppable;
};

class EventQueue {
  public:
    struct Counters {
        uint64_t replaced;      // queued values overwritten by a newer one
        uint64_t evicted;       // droppable entries removed to make room
        uint64_t rejected_busy; // pushes refused with AYNI_BUSY
    };

    static constexpr std::size_t storage_alignment = alignof(EventHeader);
    static std::size_t storage_size(uint32_t capacity, uint32_t body_max_bytes);

    // storage holds storage_size(capacity, body_max_bytes) bytes aligned to
    // storage_alignment and outlives the queue. 1 <= capacity <= 65535.
    EventQueue(void *storage, uint32_t capacity, uint32_t body_max_bytes);

    EventQueue(const EventQueue &) = delete;
    EventQueue &operator=(const EventQueue &) = delete;

    // Copies the event in. AYNI_OK when queued, including by replacing or
    // evicting a droppable entry; AYNI_BUSY when full and nothing may be
    // dropped; AYNI_INVALID_ARGUMENT when body_size exceeds body_max_bytes.
    // body may be null only when body_size is 0.
    ayni_status_t push(const EventHeader &header, const uint8_t *body);

    // Moves the oldest event out; body receives body_size bytes and must hold
    // body_max_bytes. Returns false when empty.
    bool pop(EventHeader &out_header, uint8_t *body);

    // Drops one oldest entry without copying its body; owner closes admission first.
    bool discard();

    uint32_t size() const { return count_; }
    const Counters &counters() const { return counters_; }

  private:
    unsigned char *entry(uint16_t index) const { return entries_ + stride_ * index; }
    uint16_t &at_position(uint32_t position) const {
        return order_[(head_ + position) % capacity_];
    }
    // Where a droppable event goes when the queue is full.
    struct Room {
        enum class Kind { none, replace, evict } kind;
        uint32_t position;
        uint32_t replaced; // for replace: the count already on that entry
    };

    uint16_t take_head(); // Releases exactly one occupied slot, only while count_ > 0.
    void write(uint16_t index, const EventHeader &header, uint32_t replaced, const uint8_t *body);
    EventHeader header_of(uint16_t index) const;
    Room find_room(const EventHeader &header) const;
    void remove_at(uint32_t position);

    unsigned char *entries_;
    std::size_t stride_;
    uint16_t *order_; // queue order: entry indices, oldest at head_
    uint16_t *free_;  // stack of unused entry indices
    uint32_t capacity_;
    uint32_t body_max_bytes_;
    uint32_t head_ = 0;
    uint32_t count_ = 0;
    uint32_t free_count_;
    Counters counters_{};
};

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_INGRESS_EVENT_QUEUE_H
