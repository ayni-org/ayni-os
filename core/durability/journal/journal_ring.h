// The batches a kernel has committed in memory and not yet stored, in commit
// order, in its storage buffer (ADR-0026). A transition encodes its batch in
// place, so it needs one contiguous piece of the buffer: the ring keeps the
// queued batches in at most two runs, one after the other in log order, and
// a new batch goes after the later run or, when that has no room, at the
// start of the buffer before the earlier one (a bip buffer). The oldest
// batch leaves once the Host has stored it.

#ifndef AYNI_CORE_DURABILITY_JOURNAL_JOURNAL_RING_H
#define AYNI_CORE_DURABILITY_JOURNAL_JOURNAL_RING_H

#include <cstddef>
#include <cstdint>

namespace ayni::durability {

class JournalRing {
  public:
    // The bytes the ring keeps for each batch beside the batch itself.
    static constexpr uint32_t entry_overhead = 4;

    // buffer holds size bytes and outlives the ring.
    JournalRing(uint8_t *buffer, uint32_t size) : buffer_(buffer), size_(size) {}

    JournalRing(const JournalRing &) = delete;
    JournalRing &operator=(const JournalRing &) = delete;

    // Whether a batch of up to max_size bytes fits in one piece now.
    bool fits(uint32_t max_size) const;
    // Where the next batch, of up to max_size bytes, is encoded; only when
    // fits(max_size). It is not queued until end_batch().
    uint8_t *begin_batch(uint32_t max_size);
    // Queues the batch begin_batch() placed, which took size bytes, at most
    // the max_size it was begun with.
    void end_batch(uint32_t size);

    bool empty() const { return first_start_ == first_end_; }
    // The oldest batch queued; only when not empty().
    const uint8_t *front(uint32_t &out_size) const;
    // Drops and erases the oldest batch; only after its Host borrow ended and not empty().
    void pop_front();
    // Borrow queued batch at ordinal, or null after the retained prefix. No cursor changes.
    const uint8_t *peek(uint32_t ordinal, uint32_t &out_size) const;
    void clear(); // Erases the entire buffer; caller must own it without any I/O borrow.

  private:
    // The free run a new batch takes: after the later run, or before the
    // earlier one once the later run has wrapped to the start.
    uint32_t free_start(uint32_t need) const;

    uint8_t *buffer_;
    uint32_t size_;
    // The earlier run [first_start_, first_end_) and, once wrapped, the
    // later one [0, second_end_), which then ends at or before first_start_.
    uint32_t first_start_ = 0;
    uint32_t first_end_ = 0;
    uint32_t second_end_ = 0;
    bool wrapped_ = false;
    uint32_t begun_at_ = 0;
};

} // namespace ayni::durability

#endif // AYNI_CORE_DURABILITY_JOURNAL_JOURNAL_RING_H
