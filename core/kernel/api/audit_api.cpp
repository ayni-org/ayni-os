// C ABI entry points for the asynchronous RAM audit ring (include/ayni/audit.h, ADR-0113).
#include "catalog/abi_checks.h"
#include "kernel/api/api_gate.h"
#include "kernel/runtime/kernel_state.h"

#include <ayni/audit.h>

extern "C" {

ayni_status_t ayni_get_audit_info(const ayni_kernel_t *kernel, ayni_audit_info_v1 *out) {
    if (kernel == nullptr || out == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (const auto shape =
            ayni::catalog::check_struct_size(out->struct_size, sizeof(*out), sizeof(*out));
        shape != AYNI_OK) {
        return shape;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::owner);
        gated != AYNI_OK) {
        return gated;
    }
    kernel->describe_audit(*out);
    return AYNI_OK;
}

ayni_status_t ayni_take_audit_records(ayni_kernel_t *kernel, ayni_audit_record_v1 *out,
                                      uint32_t capacity, uint32_t *out_count) {
    if (kernel == nullptr || out_count == nullptr || (capacity != 0U && out == nullptr)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::owner);
        gated != AYNI_OK) {
        return gated;
    }
    *out_count = kernel->take_audit_records(out, capacity);
    return AYNI_OK;
}

} // extern "C"
