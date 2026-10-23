// Stable ids the kernel assigns, such as goal and instance ids (C-ID-001,
// C-ID-010). The durable boot epoch fills the high 32 bits and a per-boot
// sequence that starts at 1 fills the low 32 bits. Boot epochs only grow along
// the log (ADR-0016), so an id is never assigned twice, even though no
// counter is stored (ADR-0017). The cost is at most 2^32 - 1 ids per boot and
// boot epochs below 2^32.

#ifndef AYNI_CORE_FOUNDATION_KERNEL_ID_H
#define AYNI_CORE_FOUNDATION_KERNEL_ID_H

#include <cstdint>

namespace ayni::foundation {

// C-ID-010: both components occupy one 32-bit word. Zero in either word
// cannot be an allocated ID; id_boot_epoch() also accepts an allocator prefix.
constexpr uint32_t kernel_id_epoch_shift = 32;
// The largest boot epoch whose IDs fit in the fixed-width representation.
constexpr uint64_t max_boot_epoch = 0xFFFFFFFFU;
// Largest per-boot sequence; batch admission must not wrap this word.
constexpr uint32_t max_id_sequence = 0xFFFFFFFFU;

// Decode the durable epoch without requiring a valid per-boot sequence.
constexpr uint64_t id_boot_epoch(uint64_t id) {
    return id >> kernel_id_epoch_shift;
}

// Shape only: existence, owner and the permitted boot bound are caller rules.
constexpr bool is_kernel_id(uint64_t id) {
    return id_boot_epoch(id) != 0U && static_cast<uint32_t>(id) != 0U;
}

// Owns this boot's sequence; the caller supplies the already durable epoch.
class IdAllocator {
  public:
    // Starts the sequence of a durable boot epoch in [1, max_boot_epoch].
    // The kernel always starts at sequence 1; a test may start near the end
    // to reach exhaustion without allocating 2^32 ids.
    void start(uint64_t boot_epoch, uint32_t first_sequence = 1) {
        prefix_ = boot_epoch << kernel_id_epoch_shift;
        next_ = first_sequence;
    }

    // Whether allocate() would succeed: started, and the sequence not used up.
    bool available(uint32_t count = 1) const {
        return prefix_ != 0U &&
               (count == 0U || (next_ != 0U && count - 1U <= max_id_sequence - next_));
    }

    // Zero before start(); otherwise the epoch, including after exhaustion.
    uint64_t boot_epoch() const { return id_boot_epoch(prefix_); }

    // Assigns the next id, which is never 0. Returns false, assigning
    // nothing, when no id is available.
    bool allocate(uint64_t &out_id) {
        if (!available()) {
            return false;
        }
        out_id = prefix_ | next_;
        ++next_; // wraps to 0 after the last sequence number
        return true;
    }

  private:
    uint64_t prefix_ = 0; // high-word epoch; zero means not started
    uint32_t next_ = 0;   // next per-boot sequence; zero means unavailable
};

} // namespace ayni::foundation

#endif // AYNI_CORE_FOUNDATION_KERNEL_ID_H
