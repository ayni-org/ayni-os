#include "catalog/abi_checks.h"
#include "kernel/kernel_state.h"
#include <ayni/scenes.h>

extern "C" {
ayni_status_t ayni_register_scene(ayni_kernel_t *kernel, const ayni_scene_desc_v1 *scene) {
    if (kernel == nullptr || scene == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (kernel->in_callback() || kernel->lifecycle != AYNI_LIFECYCLE_CONFIGURING) {
        return AYNI_INVALID_STATE;
    }
    return kernel->work.scenes.register_description(*scene);
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
    if (kernel->in_callback()) {
        return AYNI_INVALID_STATE;
    }
    const auto status = kernel->work.scenes.info(scene_id, kernel->durable_seq(), *out_info);
    if (status == AYNI_OK && kernel->lifecycle != AYNI_LIFECYCLE_RUNNING) {
        out_info->consumable = 0;
    }
    return status;
}
}
