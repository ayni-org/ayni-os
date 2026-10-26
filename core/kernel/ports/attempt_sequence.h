// One owner-thread request sequence for Host and Provider attempts (C-ID-006).
#ifndef AYNI_CORE_KERNEL_COMPLETION_ATTEMPT_SEQUENCE_H
#define AYNI_CORE_KERNEL_COMPLETION_ATTEMPT_SEQUENCE_H

#include <ayni/ayni.h>
#include <cstdint>

namespace ayni::kernel {
// Kernel lifetime identity owner. Automatic storage reload does not reset it;
// a new durable boot still cannot alias a slot incarnation from this object.
class AttemptSequence {
  public:
    // Zero starts a fresh lifetime. A nonzero seed resumes an already consumed
    // prefix, useful for bounded exhaustion checks; it never makes IDs reusable.
    explicit AttemptSequence(uint64_t consumed = 0) : consumed_(consumed) {}
    AttemptSequence(const AttemptSequence &) = delete;
    AttemptSequence &operator=(const AttemptSequence &) = delete;
    // Boot zero is reserved for Host recovery requests. Refusal at exhaustion
    // preserves out, never wraps to the reserved zero request ID.
    bool next(uint64_t boot_epoch, ayni_attempt_ref_v1 &out) {
        if (consumed_ == UINT64_MAX) {
            return false;
        }
        out = {boot_epoch, ++consumed_};
        return true;
    }

  private:
    uint64_t consumed_; // Largest issued request ID; zero only before the first.
};
} // namespace ayni::kernel
#endif
