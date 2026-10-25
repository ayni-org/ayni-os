// Identity of each transition the kernel applies (C-COMMIT-003): the next
// transition_id and commit_seq after the boot batch. Each transition's batch
// takes its stamp in the log (ADR-0026), so a stamp is durable once
// durable_seq reaches its commit_seq; one above durable_seq may be lost, and
// the next boot then assigns it again (C-COMMIT-016).

#ifndef AYNI_CORE_KERNEL_INTENT_TRANSITION_CLOCK_H
#define AYNI_CORE_KERNEL_INTENT_TRANSITION_CLOCK_H

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

    // The commit_seq next() gives next: every transition applied so far has
    // a smaller one.
    uint64_t next_commit_seq() const { return next_.commit_seq; }
    // The stamp next() gave last; only after it gave one.
    foundation::TransitionStamp latest() const {
        return {next_.transition_id - 1U, next_.commit_seq - 1U};
    }

  private:
    foundation::TransitionStamp next_{0, 0};
};

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_INTENT_TRANSITION_CLOCK_H
