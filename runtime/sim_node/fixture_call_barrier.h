// Public diagnostic barrier check, shared by simulated service adapters.
#ifndef AYNI_RUNTIME_FIXTURE_CALL_BARRIER_H
#define AYNI_RUNTIME_FIXTURE_CALL_BARRIER_H
#include <ayni/ayni.hpp>
#include <ayni/calls.h>
namespace ayni::runtime::fixtures {
// Inspect the public barrier immediately before the fixture effect (C-COMMIT-004).
// No query reenters Core from submit; queued acceptance is not business execution.
inline ayni_status_t check_barrier(ayni_kernel_t *kernel, uint64_t id) {
    auto call = initialized<ayni_call_info_v1>();
    auto info = initialized<ayni_kernel_info_v1>();
    auto status = ayni_get_call(kernel, id, &call);
    if (status == AYNI_OK) {
        status = ayni_get_kernel_info(kernel, &info);
    }
    if (status != AYNI_OK) {
        return status;
    }
    return call.requires_seq && call.requires_seq <= info.durable_seq ? AYNI_OK : AYNI_NOT_READY;
}
} // namespace ayni::runtime::fixtures
#endif
