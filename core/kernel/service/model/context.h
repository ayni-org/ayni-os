// Borrowed owner-call context; no persistent state or platform implementation.
#ifndef AYNI_CORE_KERNEL_SERVICE_MODEL_CONTEXT_H
#define AYNI_CORE_KERNEL_SERVICE_MODEL_CONTEXT_H
#include "work/authority/authority_facts.h"
#include <cstdint>
namespace ayni::kernel {

// The Host monotonic clock, borrowed for one synchronous owner call. A rule
// takes the sample the kernel made at entry (now_us) for schedules and the
// checks before any caller code runs, and samples this clock again for a
// check after caller code returns (a goal type's export, a strategy's
// decide), so a deadline that passed meanwhile counts (ADR-0051).
class OwnerClock {
  public:
    OwnerClock(void *ctx, uint64_t (*read)(void *ctx)) : ctx_(ctx), now_us_(read) {}
    uint64_t now_us() const { return now_us_(ctx_); }

  private:
    void *ctx_;
    uint64_t (*now_us_)(void *ctx);
};

// The ingress frontier, borrowed for one synchronous admission. Only the
// publication of a new explicit reauthorization samples it, never a retry.
class InputResumeSampler {
  public:
    InputResumeSampler(void *ctx, work::InputResumeBoundary (*read)(void *ctx))
        : ctx_(ctx), sample_(read) {}
    work::InputResumeBoundary sample() const { return sample_(ctx_); } // Taken under its lock.

  private:
    void *ctx_;
    work::InputResumeBoundary (*sample_)(void *ctx);
};

} // namespace ayni::kernel
#endif
