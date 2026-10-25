// C ABI entries of physical execution: executor and execution binding
// registration, and a logical call's execution status. The catalog checks
// their contracts; ExecutionControl owns permits and stops.

#include "catalog/abi_checks.h"
#include "kernel/api/api_gate.h"
#include "kernel/kernel_state.h"
#include <ayni/execution.h>

extern "C" {
ayni_status_t ayni_register_executor(ayni_kernel_t *kernel, const ayni_executor_v1 *executor) {
    if (kernel == nullptr || executor == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::registration);
        gated != AYNI_OK) {
        return gated;
    }
    return kernel->catalogs.execution.add_executor(*executor);
}
ayni_status_t ayni_register_execution_binding(ayni_kernel_t *kernel,
                                              const ayni_execution_binding_v1 *binding) {
    if (kernel == nullptr || binding == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::registration);
        gated != AYNI_OK) {
        return gated;
    }
    return kernel->catalogs.execution.add_binding(*binding);
}
ayni_status_t ayni_get_execution(const ayni_kernel_t *kernel, uint64_t logical_call_id,
                                 ayni_execution_info_v1 *out_info) {
    if (kernel == nullptr || out_info == nullptr || logical_call_id == 0) {
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
    uint32_t index = 0;
    if (!kernel->work.calls.find(logical_call_id, index)) {
        return AYNI_NOT_FOUND;
    }
    const auto &state = kernel->work.calls.state(index);
    if (!state.execution) {
        return AYNI_NOT_FOUND;
    }
    const auto &identity = kernel->work.calls.identity(index);
    const auto &execution = *state.execution;
    const auto *lease = kernel->permits.find(logical_call_id);
    *out_info = {sizeof(*out_info),
                 execution.phase,
                 logical_call_id,
                 identity.operation_id,
                 state.attempt.request_id,
                 state.latest.transition.commit_seq,
                 state.outcome == 0 ? 0U : 1U,
                 execution.target,
                 execution.stop_cause,
                 lease == nullptr ? AYNI_OK : lease->port_status};
    return AYNI_OK;
}
}
