// Finite monotonic deadlines. UINT64_MAX denotes no deadline; overflow must
// never accidentally turn a finite wait into an unbounded one.
#ifndef AYNI_CORE_FOUNDATION_DEADLINE_H
#define AYNI_CORE_FOUNDATION_DEADLINE_H
#include <cstdint>
namespace ayni::foundation {
constexpr uint64_t last_finite_time = UINT64_MAX - 1U;
// Saturate at the last finite instant, including a clock already at its limit.
constexpr uint64_t deadline(uint64_t now_us, uint64_t duration_us) {
    return now_us >= last_finite_time || duration_us > last_finite_time - now_us
               ? last_finite_time
               : now_us + duration_us;
}
} // namespace ayni::foundation
#endif
