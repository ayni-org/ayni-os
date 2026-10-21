// The journal ring in the storage buffer (ADR-0026): the batches a kernel
// has committed and not yet stored wait there in commit order, each in one
// contiguous piece after its size prefix, in at most two runs (a bip
// buffer); a new batch never overwrites one still queued, and the oldest
// leaves only once the Host has stored it.

#include "durability/journal/journal_ring.h"
#include "foundation/little_endian.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

namespace {

using ayni::durability::JournalRing;
using ayni::foundation::load_le32;

constexpr uint32_t overhead = JournalRing::entry_overhead;
constexpr uint32_t guard_size = 16;
constexpr uint8_t guard_byte = 0xA5;

// A ring over a buffer with guard bytes on both sides, which no batch may
// touch.
class Ring {
  public:
    explicit Ring(uint32_t size)
        : bytes_(size + 2U * guard_size, guard_byte), size_(size),
          ring_(bytes_.data() + guard_size, size) {}

    JournalRing *operator->() { return &ring_; }

    uint8_t *buffer() { return bytes_.data() + guard_size; }

    // Begins a batch of up to max_size bytes, writes all of them with fill,
    // as an encoder may, and queues the first size. Returns where the batch's
    // size prefix lies in the buffer; no_offset, queueing nothing, when the
    // batch does not fit.
    static constexpr uint32_t no_offset = UINT32_MAX;
    uint32_t queue(uint32_t size, uint8_t fill, uint32_t max_size) {
        if (!ring_.fits(max_size)) {
            ADD_FAILURE() << "a batch of " << max_size << " bytes does not fit";
            return no_offset;
        }
        uint8_t *at = ring_.begin_batch(max_size);
        const auto offset = static_cast<uint32_t>(at - buffer()) - overhead;
        std::memset(at, fill, max_size);
        ring_.end_batch(size);
        return offset;
    }
    uint32_t queue(uint32_t size, uint8_t fill) { return queue(size, fill, size); }

    // Checks that the oldest batch is size bytes of fill, then drops it.
    void pop(uint32_t size, uint8_t fill) {
        ASSERT_FALSE(ring_.empty());
        uint32_t queued = 0;
        const uint8_t *at = ring_.front(queued);
        ASSERT_EQ(queued, size);
        for (uint32_t index = 0; index < size; ++index) {
            ASSERT_EQ(at[index], fill) << index;
        }
        ring_.pop_front();
    }

    bool guards_intact() const {
        for (uint32_t index = 0; index < guard_size; ++index) {
            if (bytes_[index] != guard_byte || bytes_[guard_size + size_ + index] != guard_byte) {
                return false;
            }
        }
        return true;
    }

  private:
    std::vector<uint8_t> bytes_;
    uint32_t size_;
    JournalRing ring_;
};

// The storage buffer holds the batch and its size prefix in one piece, so
// the largest batch is the buffer less the prefix; a size that would wrap
// with the prefix added does not fit either.
TEST(JournalRing, empty_ring_fits_a_batch_of_its_size_less_the_prefix) {
    Ring ring(64);
    EXPECT_TRUE(ring->empty());
    EXPECT_TRUE(ring->fits(0));
    EXPECT_TRUE(ring->fits(64 - overhead));
    EXPECT_FALSE(ring->fits(64 - overhead + 1U));
    EXPECT_FALSE(ring->fits(UINT32_MAX));
    EXPECT_FALSE(ring->fits(UINT32_MAX - overhead + 1U));
}

// ADR-0026: a transition encodes its batch in place after the size prefix,
// and the batch is queued only once end_batch() gives the size it took.
TEST(JournalRing, batch_is_encoded_after_its_size_prefix_and_queued_at_end_batch) {
    Ring ring(64);
    uint8_t *at = ring->begin_batch(10);
    EXPECT_EQ(at, ring.buffer() + overhead);
    EXPECT_TRUE(ring->empty());
    std::memset(at, 7, 10);
    ring->end_batch(6);
    EXPECT_FALSE(ring->empty());
    EXPECT_EQ(load_le32(ring.buffer()), 6U);
    uint32_t size = 0;
    EXPECT_EQ(ring->front(size), ring.buffer() + overhead);
    EXPECT_EQ(size, 6U);
    // The next batch follows the 6 bytes queued, not the 10 begun.
    EXPECT_EQ(ring.queue(3, 8), overhead + 6U);
    ring.pop(6, 7);
    ring.pop(3, 8);
    EXPECT_TRUE(ring->empty());
    EXPECT_TRUE(ring.guards_intact());
}

// The Host stores batches in commit order, so they leave oldest first; an
// empty ring starts again at the beginning of the buffer.
TEST(JournalRing, batches_leave_oldest_first) {
    Ring ring(64);
    EXPECT_EQ(ring.queue(5, 1), 0U);
    EXPECT_EQ(ring.queue(0, 2), 9U);
    EXPECT_EQ(ring.queue(7, 3), 13U);
    ring.pop(5, 1);
    ring.pop(0, 2);
    ring.pop(7, 3);
    EXPECT_TRUE(ring->empty());
    EXPECT_EQ(ring.queue(4, 4), 0U);
    EXPECT_TRUE(ring.guards_intact());
}

// A batch may end on the buffer's last byte; then nothing fits until the
// oldest batch leaves.
TEST(JournalRing, batch_may_fill_the_end_of_the_buffer_to_the_last_byte) {
    Ring ring(32);
    EXPECT_EQ(ring.queue(12, 1), 0U);
    EXPECT_TRUE(ring->fits(12));
    EXPECT_FALSE(ring->fits(13));
    EXPECT_EQ(ring.queue(12, 2), 16U);
    EXPECT_FALSE(ring->fits(0));
    ring.pop(12, 1);
    ring.pop(12, 2);
    EXPECT_TRUE(ring.guards_intact());
}

// A batch that does not fit after the newest goes to the start of the
// buffer, up to the oldest batch exactly, and leaves after the batches
// queued before it. While the newer run lies at the start, the free bytes at
// the end stay unused: a batch there would be read before the newer run.
TEST(JournalRing, batch_that_does_not_fit_at_the_end_wraps_to_the_start) {
    Ring ring(40);
    ring.queue(12, 1);
    ring.queue(12, 2);
    EXPECT_FALSE(ring->fits(12));
    ring.pop(12, 1);
    EXPECT_TRUE(ring->fits(12));
    EXPECT_FALSE(ring->fits(13));
    EXPECT_EQ(ring.queue(12, 3), 0U);
    EXPECT_FALSE(ring->fits(0));
    ring.pop(12, 2);
    ring.pop(12, 3);
    EXPECT_TRUE(ring->empty());
    EXPECT_TRUE(ring.guards_intact());
}

// Once wrapped, new batches follow the newer run and may grow it up to the
// oldest batch exactly.
TEST(JournalRing, wrapped_run_grows_up_to_the_oldest_batch) {
    Ring ring(48);
    ring.queue(8, 1);
    ring.queue(8, 2);
    EXPECT_EQ(ring.queue(16, 3), 24U);
    ring.pop(8, 1);
    ring.pop(8, 2);
    EXPECT_EQ(ring.queue(8, 4), 0U);
    EXPECT_TRUE(ring->fits(8));
    EXPECT_FALSE(ring->fits(9));
    EXPECT_EQ(ring.queue(8, 5), 12U);
    EXPECT_FALSE(ring->fits(0));
    ring.pop(16, 3);
    ring.pop(8, 4);
    ring.pop(8, 5);
    EXPECT_TRUE(ring.guards_intact());
}

// When the older run has left, the newer run is the only one: new batches
// follow it and may use the end of the buffer again.
TEST(JournalRing, ring_unwraps_once_the_older_run_has_left) {
    Ring ring(48);
    ring.queue(20, 1);
    ring.queue(16, 2);
    ring.pop(20, 1);
    EXPECT_EQ(ring.queue(8, 3), 0U);
    ring.pop(16, 2);
    // The batch at the start is now the oldest, and the rest of the buffer
    // follows it.
    EXPECT_TRUE(ring->fits(48 - 12 - overhead));
    EXPECT_FALSE(ring->fits(48 - 12 - overhead + 1U));
    EXPECT_EQ(ring.queue(48 - 12 - overhead, 4), 12U);
    ring.pop(8, 3);
    ring.pop(48 - 12 - overhead, 4);
    EXPECT_TRUE(ring->empty());
    EXPECT_TRUE(ring.guards_intact());
}

// A failed log drops what it had not stored, wrapped or not.
TEST(JournalRing, clear_drops_every_batch) {
    Ring ring(40);
    ring.queue(12, 1);
    ring.queue(12, 2);
    ring.pop(12, 1);
    ring.queue(12, 3);
    ring->clear();
    EXPECT_TRUE(ring->empty());
    EXPECT_TRUE(ring->fits(40 - overhead));
    EXPECT_EQ(ring.queue(4, 4), 0U);
    ring->clear();
    EXPECT_TRUE(ring->empty());
    EXPECT_EQ(ring.queue(40 - overhead, 5), 0U);
    ring.pop(40 - overhead, 5);
    EXPECT_TRUE(ring.guards_intact());
}

// A queued batch is the Host's to read until it is stored. Whatever batches
// come and go, and however much of what a batch was begun with it writes,
// every queued batch keeps its bytes, and the batches leave in the order
// they came; an empty ring always fits the largest batch.
TEST(JournalRing, queued_batches_keep_their_bytes_and_order_through_any_sequence) {
    constexpr uint32_t size = 101;
    Ring ring(size);
    struct Queued {
        uint32_t offset; // of the size prefix
        uint32_t size;
        uint32_t seq;
    };
    std::deque<Queued> queued;
    const auto pattern = [](uint32_t seq, uint32_t index) {
        return static_cast<uint8_t>(seq * 131U + index * 7U + 1U);
    };
    uint32_t state = 12345;
    const auto next = [&state](uint32_t bound) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state % bound;
    };
    uint32_t seq = 0;
    uint32_t wraps = 0;
    for (int round = 0; round < 20000; ++round) {
        if (queued.empty()) {
            ASSERT_TRUE(ring->fits(size - overhead));
        }
        if (next(2) == 0U) {
            const uint32_t max_size = next(40);
            if (!ring->fits(max_size)) {
                continue;
            }
            const uint32_t batch_size = next(max_size + 1U);
            uint8_t *at = ring->begin_batch(max_size);
            ASSERT_GE(at, ring.buffer() + overhead);
            ASSERT_LE(at + max_size, ring.buffer() + size);
            const auto offset = static_cast<uint32_t>(at - ring.buffer()) - overhead;
            if (!queued.empty() && offset < queued.back().offset) {
                ++wraps;
            }
            ++seq;
            for (uint32_t index = 0; index < max_size; ++index) {
                at[index] = pattern(seq, index);
            }
            ring->end_batch(batch_size);
            queued.push_back({offset, batch_size, seq});
            for (const Queued &batch : queued) {
                ASSERT_EQ(load_le32(ring.buffer() + batch.offset), batch.size) << round;
                for (uint32_t index = 0; index < batch.size; ++index) {
                    ASSERT_EQ(ring.buffer()[batch.offset + overhead + index],
                              pattern(batch.seq, index))
                        << round;
                }
            }
        } else if (!queued.empty()) {
            const Queued oldest = queued.front();
            uint32_t front_size = 0;
            const uint8_t *front = ring->front(front_size);
            ASSERT_EQ(front, ring.buffer() + oldest.offset + overhead) << round;
            ASSERT_EQ(front_size, oldest.size) << round;
            ring->pop_front();
            queued.pop_front();
        }
        ASSERT_EQ(ring->empty(), queued.empty()) << round;
    }
    EXPECT_GT(wraps, 100U);
    EXPECT_TRUE(ring.guards_intact());
}

} // namespace
