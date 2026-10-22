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

// The largest boot epoch whose ids fit the high 32 bits.
constexpr uint64_t max_boot_epoch = 0xFFFFFFFFU;

class IdAllocator {
  public:
    // Starts the sequence of a durable boot epoch in [1, max_boot_epoch].
    // The kernel always starts at sequence 1; a test may start near the end
    // to reach exhaustion without allocating 2^32 ids.
    void start(uint64_t boot_epoch, uint32_t first_sequence = 1) {
        prefix_ = boot_epoch << 32U;
        next_ = first_sequence;
    }

    // Whether allocate() would succeed: started, and the sequence not used up.
    bool available() const { return prefix_ != 0U && next_ != 0U; }

    uint64_t boot_epoch() const { return prefix_ >> 32U; }

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
    uint64_t prefix_ = 0;
    uint32_t next_ = 0;
};

} // namespace ayni::foundation

#endif // AYNI_CORE_FOUNDATION_KERNEL_ID_H
