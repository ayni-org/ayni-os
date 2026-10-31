// Optional Host clock adapter. This source does not select a platform or infer trust.
#ifndef AYNI_HOST_CLOCK_HPP
#define AYNI_HOST_CLOCK_HPP
#include <ayni/ayni.h>
namespace ayni::hosts {
// Since 0.53: source-level adapter, independent of the C ABI. The Host borrows
// context through its destruction. Core calls sample on its owner thread only;
// it must be bounded, nonblocking, nonthrowing and must initialize the complete
// sample, including explicit trust. A null function selects the Host default.
struct UtcSource {
    void *context = nullptr; // Borrowed state, null is permitted by the callback.
    void (*sample)(void *, ayni_utc_sample_v1 *) = nullptr; // Null means no override.
};
} // namespace ayni::hosts
#endif
