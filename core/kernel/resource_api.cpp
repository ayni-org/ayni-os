// Resource registration, health and copied diagnostics. Only this boundary
// selects lifecycle policy; the catalog and admission modules own all state.
#include "catalog/abi_checks.h"
#include "kernel/kernel_state.h"

#include <ayni/resources.h>

extern "C" {

ayni_status_t ayni_get_resource_queue(const ayni_kernel_t *kernel,
                                      ayni_resource_queue_info_v1 *out_info) {
    if (kernel == nullptr || out_info == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto status = ayni::catalog::check_struct_size(out_info->struct_size, sizeof(*out_info),
                                                         sizeof(*out_info));
    if (status != AYNI_OK) {
        return status;
    }
    if (kernel->in_callback()) {
        return AYNI_INVALID_STATE;
    }
    *out_info = kernel->resources.queue_info(kernel->host.monotonic_now_us(kernel->host.ctx));
    return AYNI_OK;
}

ayni_status_t ayni_register_device(ayni_kernel_t *kernel, const ayni_device_desc_v1 *device) {
    if (kernel == nullptr || device == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    ayni::catalog::Device decoded{};
    const auto status = ayni::catalog::DeviceCatalog::decode(*device, decoded);
    if (status != AYNI_OK) {
        return status;
    }
    if (kernel->in_callback() || kernel->lifecycle != AYNI_LIFECYCLE_CONFIGURING) {
        return AYNI_INVALID_STATE;
    }
    return kernel->devices.add_validated(decoded);
}

ayni_status_t ayni_register_component(ayni_kernel_t *kernel,
                                      const ayni_component_desc_v1 *component) {
    if (kernel == nullptr || component == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    ayni::catalog::Component decoded{};
    const auto status = ayni::catalog::DeviceCatalog::decode(*component, decoded);
    if (status != AYNI_OK) {
        return status;
    }
    if (kernel->in_callback() || kernel->lifecycle != AYNI_LIFECYCLE_CONFIGURING) {
        return AYNI_INVALID_STATE;
    }
    return kernel->devices.add_validated(decoded);
}

// Inventory diagnostics are copied and include unavailable resources. Health is
// a separate fact and cannot silently remove a component or its resource claims.
ayni_status_t ayni_get_device(const ayni_kernel_t *kernel, ayni_string_v1 id,
                              ayni_device_info_v1 *out_info) {
    if (kernel == nullptr || out_info == nullptr || !ayni::catalog::CatalogId::is_valid(id)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto status = ayni::catalog::check_struct_size(out_info->struct_size, sizeof(*out_info),
                                                         sizeof(*out_info));
    if (status != AYNI_OK) {
        return status;
    }
    if (kernel->in_callback()) {
        return AYNI_INVALID_STATE;
    }
    uint32_t index = 0;
    if (!kernel->devices.find_device(id, index)) {
        return AYNI_NOT_FOUND;
    }
    *out_info = {sizeof(*out_info), kernel->devices.device_component_count(id),
                 kernel->resource_catalog.device_resources(kernel->devices, id), 0};
    return AYNI_OK;
}

// Parent ID is copied into caller storage; no catalog pointer escapes.
ayni_status_t ayni_get_component(const ayni_kernel_t *kernel, ayni_string_v1 id,
                                 ayni_component_info_v1 *out_info) {
    if (kernel == nullptr || out_info == nullptr || !ayni::catalog::CatalogId::is_valid(id)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto status = ayni::catalog::check_struct_size(out_info->struct_size, sizeof(*out_info),
                                                         sizeof(*out_info));
    if (status != AYNI_OK) {
        return status;
    }
    if (kernel->in_callback()) {
        return AYNI_INVALID_STATE;
    }
    uint32_t index = 0;
    if (!kernel->devices.find_component(id, index)) {
        return AYNI_NOT_FOUND;
    }
    *out_info = {sizeof(*out_info), kernel->resource_catalog.component_resources(id),
                 kernel->devices.component(index).device.snapshot()};
    return AYNI_OK;
}

ayni_status_t ayni_register_resource(ayni_kernel_t *kernel, const ayni_resource_desc_v1 *resource) {
    if (kernel == nullptr || resource == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    ayni::catalog::Resource decoded{};
    const auto status = ayni::catalog::ResourceRegistry::decode(*resource, decoded);
    if (status != AYNI_OK) {
        return status;
    }
    if (kernel->in_callback() || kernel->lifecycle != AYNI_LIFECYCLE_CONFIGURING) {
        return AYNI_INVALID_STATE;
    }
    return kernel->resource_catalog.add_validated(decoded);
}

// Health affects future admission, not an acquired claim's lifetime. A wake
// is useful only after an actual change; no persistence is implied (C-RES-013).
ayni_status_t ayni_set_resource_health(ayni_kernel_t *kernel, ayni_string_v1 resource_id,
                                       ayni_resource_health_t health) {
    if (kernel == nullptr || !ayni::catalog::CatalogId::is_valid(resource_id)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (kernel->in_callback() || (kernel->lifecycle != AYNI_LIFECYCLE_CONFIGURING &&
                                  kernel->lifecycle != AYNI_LIFECYCLE_RUNNING)) {
        return AYNI_INVALID_STATE;
    }
    uint32_t index = 0;
    if (!kernel->resource_catalog.find(resource_id, index)) {
        return AYNI_NOT_FOUND;
    }
    bool changed = false;
    const auto status = kernel->resources.set_health(index, health, changed);
    if (status == AYNI_OK && changed) {
        kernel->host.wake(kernel->host.ctx);
    }
    return status;
}

// Validate the caller's output before copying current diagnostics; no borrowed
// pointer escapes, and errors leave the caller's previous snapshot untouched.
ayni_status_t ayni_get_resource(const ayni_kernel_t *kernel, ayni_string_v1 resource_id,
                                ayni_resource_info_v1 *out_info) {
    if (kernel == nullptr || out_info == nullptr ||
        !ayni::catalog::CatalogId::is_valid(resource_id)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto status = ayni::catalog::check_struct_size(out_info->struct_size, sizeof(*out_info),
                                                         sizeof(*out_info));
    if (status != AYNI_OK) {
        return status;
    }
    if (kernel->in_callback()) {
        return AYNI_INVALID_STATE;
    }
    uint32_t index = 0;
    if (!kernel->resource_catalog.find(resource_id, index)) {
        return AYNI_NOT_FOUND;
    }
    *out_info = kernel->resources.info(index);
    return AYNI_OK;
}

} // extern "C"
