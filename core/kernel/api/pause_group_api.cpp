// Generic group diagnostics; applications assign meaning to group numbers.
#include "catalog/abi_checks.h"
#include "kernel/api/api_gate.h"
#include "kernel/runtime/kernel_state.h"
extern "C" ayni_status_t ayni_get_pause_group(const ayni_kernel_t *kernel, uint32_t group,
                                              ayni_pause_group_info_v1 *out) {
    if (kernel == nullptr || out == nullptr || !ayni::work::PauseGroups::valid_group(group)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto shape =
        ayni::catalog::check_struct_size(out->struct_size, sizeof(*out), sizeof(*out));
    if (shape != AYNI_OK) {
        return shape;
    }
    const auto status = ayni::kernel::admit(*kernel, ayni::kernel::gate::owner);
    if (status != AYNI_OK) {
        return status;
    }
    const auto &row = kernel->model().pause_groups.at(group);
    *out = {sizeof(*out),         group,         row.revision,
            row.paused ? 1U : 0U, row.object_id, row.stamp.transition_id,
            row.stamp.commit_seq};
    return AYNI_OK;
}
