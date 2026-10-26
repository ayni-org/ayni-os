// C ABI entries of Scenes: declaration in CONFIGURING and the info query.
// Authority owns their definitions, saved preferences and generations.

#include "catalog/abi_checks.h"
#include "kernel/api/api_gate.h"
#include "kernel/runtime/kernel_state.h"
#include <ayni/scenes.h>

extern "C" {
ayni_status_t ayni_register_scene(ayni_kernel_t *kernel, const ayni_scene_desc_v1 *scene) {
    if (kernel == nullptr || scene == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::registration);
        gated != AYNI_OK) {
        return gated;
    }
    return kernel->configure(*scene);
}
ayni_status_t ayni_get_scene(const ayni_kernel_t *kernel, ayni_string_v1 scene_id,
                             ayni_scene_info_v1 *out_info) {
    if (kernel == nullptr || out_info == nullptr || !ayni::catalog::CatalogId::is_valid(scene_id)) {
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
    const auto status = kernel->model().scenes.info(scene_id, kernel->durable_seq(), *out_info);
    if (status == AYNI_OK && kernel->state() != AYNI_LIFECYCLE_RUNNING) {
        out_info->consumable = 0;
    }
    return status;
}
}
