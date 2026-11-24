// C ABI entry points for local deterministic ReflexRules (include/ayni/reflex.h, ADR-0112).
#include "catalog/abi_checks.h"
#include "catalog/catalog_id.h"
#include "kernel/api/api_gate.h"
#include "kernel/runtime/kernel_state.h"

#include <ayni/reflex.h>

extern "C" {

ayni_status_t ayni_register_reflex_rule(ayni_kernel_t *kernel, const ayni_reflex_rule_v1 *rule) {
    if (kernel == nullptr || rule == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::registration);
        gated != AYNI_OK) {
        return gated;
    }
    return kernel->configure(*rule);
}

ayni_status_t ayni_set_reflex_enabled(ayni_kernel_t *kernel, ayni_string_v1 rule_id,
                                      uint32_t enabled) {
    if (kernel == nullptr || !ayni::catalog::CatalogId::is_valid(rule_id) || enabled > 1U) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::running);
        gated != AYNI_OK) {
        return gated;
    }
    return kernel->set_reflex_enabled(rule_id, enabled);
}

ayni_status_t ayni_get_reflex_info(const ayni_kernel_t *kernel, ayni_string_v1 rule_id,
                                   ayni_reflex_info_v1 *out_info) {
    if (kernel == nullptr || out_info == nullptr || !ayni::catalog::CatalogId::is_valid(rule_id)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (const auto shape = ayni::catalog::check_struct_size(out_info->struct_size,
                                                            sizeof(*out_info), sizeof(*out_info));
        shape != AYNI_OK) {
        return shape;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::owner);
        gated != AYNI_OK) {
        return gated;
    }
    return kernel->get_reflex_info(rule_id, *out_info);
}

ayni_status_t ayni_take_reflex_event(ayni_kernel_t *kernel, ayni_reflex_event_v1 *out_event) {
    if (kernel == nullptr || out_event == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (const auto shape = ayni::catalog::check_struct_size(out_event->struct_size,
                                                            sizeof(*out_event), sizeof(*out_event));
        shape != AYNI_OK) {
        return shape;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::owner);
        gated != AYNI_OK) {
        return gated;
    }
    return kernel->take_reflex_event(*out_event);
}

} // extern "C"
