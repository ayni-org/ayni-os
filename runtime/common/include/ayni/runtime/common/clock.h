#ifndef AYNI_RUNTIME_CLOCK_H
#define AYNI_RUNTIME_CLOCK_H
#include <ayni/ayni.h>
namespace ayni::runtime {
// Bounded read-only clock ports, safe from owner and writer threads, borrowed
// through node release. Values use the Kernel Host's clock domain.
struct Clock {
    void *context = nullptr;
    uint64_t (*now_us)(void *) = nullptr;
    void (*utc_now)(void *, ayni_utc_sample_v1 *) = nullptr;
};
} // namespace ayni::runtime
#endif
