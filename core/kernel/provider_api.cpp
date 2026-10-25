// Trusted registration boundaries. Catalogs own parsing and static contracts;
// Kernel decides lifecycle and later joins them with live work and authority.
#include "kernel/kernel_state.h"

#include <ayni/calls.h>

extern "C" {
ayni_status_t ayni_register_call_capability(ayni_kernel_t *kernel,
                                            const ayni_call_capability_v1 *capability) {
    if (kernel == nullptr || capability == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    ayni::catalog::CallCapability decoded{};
    const auto status = ayni::catalog::decode_call_capability(*capability, decoded);
    if (status != AYNI_OK) {
        return status;
    }
    if (kernel->in_callback() || kernel->lifecycle != AYNI_LIFECYCLE_CONFIGURING) {
        return AYNI_INVALID_STATE;
    }
    return kernel->call_capabilities.add_validated(decoded);
}

ayni_status_t ayni_register_provider(ayni_kernel_t *kernel, const ayni_provider_v1 *provider) {
    if (kernel == nullptr || provider == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    ayni::catalog::Provider decoded{};
    const auto status = ayni::catalog::ProviderRegistry::decode(*provider, decoded);
    if (status != AYNI_OK) {
        return status;
    }
    if (kernel->in_callback() || kernel->lifecycle != AYNI_LIFECYCLE_CONFIGURING) {
        return AYNI_INVALID_STATE;
    }
    return kernel->providers.add_validated(decoded);
}

ayni_status_t ayni_register_binding(ayni_kernel_t *kernel, const ayni_binding_v1 *binding) {
    if (kernel == nullptr || binding == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    ayni::catalog::Binding decoded{};
    const auto status = ayni::catalog::BindingRegistry::decode(*binding, decoded);
    if (status != AYNI_OK) {
        return status;
    }
    if (kernel->in_callback() || kernel->lifecycle != AYNI_LIFECYCLE_CONFIGURING) {
        return AYNI_INVALID_STATE;
    }
    return kernel->bindings.add_validated(decoded);
}
} // extern "C"
