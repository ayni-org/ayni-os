// Trusted owner diagnostics; Provider worker posting stays in observation_api's shared decoder.
#include "catalog/abi_checks.h"
#include "kernel/api/api_gate.h"
#include "kernel/runtime/kernel_state.h"
#include <ayni/subscriptions.h>
namespace {
ayni_status_t find_subscription(const ayni_kernel_t &kernel, uint64_t id, uint32_t &index) {
    const auto gate = ayni::kernel::admit(kernel, ayni::kernel::gate::owner);
    if (gate != AYNI_OK) {
        return gate;
    }
    return kernel.model().calls.find(id, index) && kernel.model().calls.state(index).subscription
               ? AYNI_OK
               : AYNI_NOT_FOUND;
}
} // namespace
extern "C" {
ayni_status_t ayni_get_subscription(const ayni_kernel_t *kernel, uint64_t id,
                                    ayni_subscription_info_v1 *out) {
    if (!kernel || !out || id == 0) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto shape =
        ayni::catalog::check_struct_size(out->struct_size, sizeof(*out), sizeof(*out));
    if (shape != AYNI_OK) {
        return shape;
    }
    uint32_t index = 0;
    const auto status = find_subscription(*kernel, id, index);
    if (status != AYNI_OK) {
        return status;
    }
    return kernel->call_view().subscription_info(index, kernel->now_us(),
                                                 kernel->writer_durable_seq(), *out);
}
ayni_status_t ayni_get_subscription_observation(const ayni_kernel_t *kernel, uint64_t id,
                                                ayni_observation_info_v1 *out) {
    if (!kernel || !out || id == 0) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto shape =
        ayni::catalog::check_struct_size(out->struct_size, sizeof(*out), sizeof(*out));
    if (shape != AYNI_OK) {
        return shape;
    }
    uint32_t index = 0;
    const auto status = find_subscription(*kernel, id, index);
    if (status != AYNI_OK) {
        return status;
    }
    const auto now = kernel->now_us();
    const auto read = kernel->observation_view().subscription_info(id, now, *out);
    if (read == AYNI_OK &&
        (kernel->state() != AYNI_LIFECYCLE_RUNNING ||
         !kernel->call_view().subscription_usable(index, now, kernel->writer_durable_seq()))) {
        out->usable = 0;
    }
    return read;
}
}
