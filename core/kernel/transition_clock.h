// Identity of each transition the kernel applies (C-COMMIT-003): the next
// transition_id and commit_seq after the boot batch. Until the log stores
// these transitions (MA-010), the stamps order them in memory and stay above
// durable_seq, so no state they produce is reported as durable, and the next
// boot starts again after its own boot batch and reuses them (C-COMMIT-016).

#ifndef AYNI_CORE_KERNEL_TRANSITION_CLOCK_H
#define AYNI_CORE_KERNEL_TRANSITION_CLOCK_H

#include "foundation/transition_stamp.h"

namespace ayni::kernel {

class TransitionClock {
  public:
    // first is the stamp next() returns first.
    void start(foundation::TransitionStamp first) { next_ = first; }

    // Never wraps: recovery leaves this boot at least 2^63 transition ids
    // (boot_sequence.cpp), and commit_seq counts batches from 1.
    foundation::TransitionStamp next() {
        const foundation::TransitionStamp stamp = next_;
        ++next_.transition_id;
        ++next_.commit_seq;
        return stamp;
    }

  private:
    foundation::TransitionStamp next_{0, 0};
};

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_TRANSITION_CLOCK_H
