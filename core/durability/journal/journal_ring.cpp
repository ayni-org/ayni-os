#include "durability/journal/journal_ring.h"

#include "foundation/little_endian.h"

namespace ayni::durability {
namespace {

constexpr uint32_t no_place = 0xFFFFFFFFU;

} // namespace

uint32_t JournalRing::free_start(uint32_t need) const {
    if (wrapped_) {
        return first_start_ - second_end_ >= need ? second_end_ : no_place;
    }
    if (size_ - first_end_ >= need) {
        return first_end_;
    }
    return first_start_ >= need ? 0U : no_place;
}

bool JournalRing::fits(uint32_t max_size) const {
    return max_size <= size_ - entry_overhead && free_start(max_size + entry_overhead) != no_place;
}

uint8_t *JournalRing::begin_batch(uint32_t max_size) {
    begun_at_ = free_start(max_size + entry_overhead);
    return buffer_ + begun_at_ + entry_overhead;
}

void JournalRing::end_batch(uint32_t size) {
    foundation::store_le32(buffer_ + begun_at_, size);
    const uint32_t end = begun_at_ + entry_overhead + size;
    if (wrapped_) {
        second_end_ = end;
    } else if (begun_at_ == first_end_) {
        first_end_ = end;
    } else {
        // It went to the start, before the earlier run.
        wrapped_ = true;
        second_end_ = end;
    }
}

const uint8_t *JournalRing::front(uint32_t &out_size) const {
    out_size = foundation::load_le32(buffer_ + first_start_);
    return buffer_ + first_start_ + entry_overhead;
}

void JournalRing::pop_front() {
    first_start_ += entry_overhead + foundation::load_le32(buffer_ + first_start_);
    if (first_start_ != first_end_) {
        return;
    }
    if (wrapped_) {
        first_start_ = 0;
        first_end_ = second_end_;
        second_end_ = 0;
        wrapped_ = false;
    } else {
        first_start_ = 0;
        first_end_ = 0;
    }
}

void JournalRing::clear() {
    first_start_ = 0;
    first_end_ = 0;
    second_end_ = 0;
    wrapped_ = false;
}

} // namespace ayni::durability
