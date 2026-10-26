// The rule each C ABI entry applies after checking its arguments (ayni.h,
// contracts/domain/lifecycles.md, ADR-0051): whether caller code running
// inside the kernel may call it (C-POLICY-001), and in which lifecycle
// states it runs. Every entry with such a rule names one gate here, so a
// rule is written once and an entry's rule is read from its name. The
// version and memory queries, ayni_create, ayni_get_lifecycle,
// ayni_get_kernel_info, and ayni_post_event and ayni_completion_post, which
// any thread may call, have none.

#ifndef AYNI_CORE_KERNEL_API_API_GATE_H
#define AYNI_CORE_KERNEL_API_API_GATE_H

#include "kernel/runtime/kernel_state.h"

#include <ayni/ayni.h>

#include <cstdint>

namespace ayni::kernel {

struct ApiGate {
    bool from_callback;    // Caller code inside the kernel may call the entry.
    uint32_t lifecycles;   // One lifecycle_bit per state the entry runs in.
    ayni_status_t outside; // What the entry returns in any other state.
};

constexpr uint32_t lifecycle_bit(ayni_lifecycle_t lifecycle) {
    return uint32_t{1} << lifecycle;
}

namespace gate {

constexpr uint32_t every_lifecycle =
    lifecycle_bit(AYNI_LIFECYCLE_CONFIGURING) | lifecycle_bit(AYNI_LIFECYCLE_RECOVERING) |
    lifecycle_bit(AYNI_LIFECYCLE_RUNNING) | lifecycle_bit(AYNI_LIFECYCLE_RESTRICTED) |
    lifecycle_bit(AYNI_LIFECYCLE_DRAINING) | lifecycle_bit(AYNI_LIFECYCLE_STOPPED) |
    lifecycle_bit(AYNI_LIFECYCLE_FENCED);
// Queries answer from RECOVERING to DRAINING, with state that may not be
// durable yet (C-COMMIT-004).
constexpr uint32_t query_lifecycles =
    every_lifecycle &
    ~(lifecycle_bit(AYNI_LIFECYCLE_CONFIGURING) | lifecycle_bit(AYNI_LIFECYCLE_STOPPED));

// Registration closes when ayni_start succeeds.
inline constexpr ApiGate registration{false, lifecycle_bit(AYNI_LIFECYCLE_CONFIGURING),
                                      AYNI_INVALID_STATE};
// Changes only work makes: a session, an intent, an observation.
inline constexpr ApiGate running{false, lifecycle_bit(AYNI_LIFECYCLE_RUNNING), AYNI_NOT_READY};
// Owner-thread entries that check the lifecycle themselves (start, step,
// shutdown) or read volatile owner state in any state.
inline constexpr ApiGate owner{false, every_lifecycle, AYNI_INVALID_STATE};
// Reads of the model; a strategy or validator may make them too.
inline constexpr ApiGate query{true, query_lifecycles, AYNI_INVALID_STATE};
// Closing a session changes the model in any state a query answers.
inline constexpr ApiGate session_close{false, query_lifecycles, AYNI_INVALID_STATE};
// Resource health counts before and while work runs (C-RES-013).
inline constexpr ApiGate resource_health{
    false, lifecycle_bit(AYNI_LIFECYCLE_CONFIGURING) | lifecycle_bit(AYNI_LIFECYCLE_RUNNING),
    AYNI_INVALID_STATE};
// The memory is the caller's again only before start or once stopped.
inline constexpr ApiGate destroy{
    false, lifecycle_bit(AYNI_LIFECYCLE_CONFIGURING) | lifecycle_bit(AYNI_LIFECYCLE_STOPPED),
    AYNI_INVALID_STATE};

} // namespace gate

// AYNI_INVALID_STATE while caller code runs unless the gate allows it, the
// gate's outside status in a state it does not list, else AYNI_OK.
inline ayni_status_t admit(const ayni_kernel &kernel, const ApiGate &entry) {
    if (!entry.from_callback && kernel.in_callback()) {
        return AYNI_INVALID_STATE;
    }
    return (entry.lifecycles & lifecycle_bit(kernel.state())) != 0U ? AYNI_OK : entry.outside;
}

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_API_API_GATE_H
