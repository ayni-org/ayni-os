// Trusted source/publication boundary; catalog and knowledge own all facts.
#include "catalog/abi_checks.h"
#include "kernel/api/api_gate.h"
#include "kernel/runtime/kernel_state.h"
#include <ayni/observations.h>

extern "C" {
ayni_status_t ayni_register_observation_source(ayni_kernel_t *kernel,
                                               const ayni_observation_source_v1 *source) {
    if (kernel == nullptr || source == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    ayni::catalog::ObservationSource decoded{};
    const auto status = ayni::catalog::decode_source(*source, decoded);
    if (status != AYNI_OK) {
        return status;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::registration);
        gated != AYNI_OK) {
        return gated;
    }
    return kernel->configure(decoded);
}
ayni_status_t ayni_publish_observation(ayni_kernel_t *kernel,
                                       const ayni_observation_v1 *observation) {
    if (kernel == nullptr || observation == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::running);
        gated != AYNI_OK) {
        return gated;
    }
    return kernel->publish_observation(*observation);
}
ayni_status_t ayni_get_observation(const ayni_kernel_t *kernel, ayni_string_v1 source_id,
                                   ayni_observation_info_v1 *out_info) {
    if (kernel == nullptr || out_info == nullptr ||
        !ayni::catalog::CatalogId::is_valid(source_id)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto shape = ayni::catalog::check_struct_size(out_info->struct_size, sizeof(*out_info),
                                                        sizeof(*out_info));
    if (shape != AYNI_OK) {
        return shape;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::owner);
        gated != AYNI_OK) {
        return gated;
    }
    const auto status = kernel->observation_view().info(source_id, kernel->now_us(), *out_info);
    if (status == AYNI_OK && kernel->state() != AYNI_LIFECYCLE_RUNNING) {
        out_info->usable = 0; // A fenced/recovering Kernel cannot consume old physical context.
    }
    return status;
}
}
