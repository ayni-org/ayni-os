// Trusted registration boundaries decode borrowed layouts; catalogs own typed contracts;
// Kernel decides lifecycle and later joins them with live work and authority.
#include "catalog/abi_checks.h"
#include "catalog/capability/business_encoding.h"
#include "kernel/api/api_gate.h"
#include "kernel/runtime/kernel_state.h"

#include <ayni/calls.h>

namespace {
using namespace ayni::catalog;
// Only boundary shape is handled here. normalize_call_contract remains the
// single owner of enum, limit, resource uniqueness and canonical-order rules.
ayni_status_t decode_contract(const ayni_call_contract_v1 &input, CallContract &out) {
    const auto status = check_struct_size(input.struct_size, sizeof(input), sizeof(input));
    if (status != AYNI_OK) {
        return status;
    }
    if (input.resource_count > resource_group_max ||
        (input.resource_count != 0U && input.resources == nullptr)) {
        return AYNI_INVALID_ARGUMENT;
    }
    CallContract value{};
    value.form = static_cast<CallForm>(input.form);
    value.category = static_cast<CallClass>(input.category);
    value.effects = input.effects;
    value.idempotency = static_cast<CallIdempotency>(input.idempotency);
    value.query = static_cast<QueryAuthority>(input.query);
    value.cancel = static_cast<CancelMode>(input.cancel);
    value.dedupe_window_ms = input.dedupe_window_ms;
    value.timeout_ms = input.timeout_ms;
    value.input_format = input.input_format;
    value.result_format = input.result_format;
    value.input_text_limit = input.input_text_limit;
    value.result_text_limit = input.result_text_limit;
    value.resource_count = input.resource_count;
    // Nested layouts are checked before reading their tails; typed normalization
    // then applies the same rules as durable records and internal contracts.
    if (input.subscription != nullptr) {
        const auto &raw = *input.subscription;
        const auto shape = check_struct_size(raw.struct_size, sizeof(raw), sizeof(raw));
        if (shape != AYNI_OK) {
            return shape;
        }
        SubscriptionContract spec{};
        if (raw.reserved != 0 || !spec.source.assign(raw.source_id)) {
            return AYNI_INVALID_ARGUMENT;
        }
        spec.source_version = raw.source_version;
        spec.lease_ms = raw.lease_ms == 0 ? AYNI_SUBSCRIPTION_LEASE_MS_DEFAULT : raw.lease_ms;
        value.subscription = spec;
    }

    for (uint32_t at = 0; at < value.resource_count; ++at) {
        const auto &resource = input.resources[at];
        if (resource.reserved != 0U || !value.resources[at].id.assign(resource.resource_id)) {
            return AYNI_INVALID_ARGUMENT;
        }
        value.resources[at].units = resource.units;
    }
    return normalize_call_contract(value, out);
}

ayni_status_t decode_call_capability(const ayni_call_capability_v1 &input, CallCapability &out) {
    const auto status = check_struct_size(input.struct_size, sizeof(input), sizeof(input));
    if (status != AYNI_OK) {
        return status;
    }
    CallCapability value{};
    if (input.version == 0U || !value.id.assign(input.capability_id)) {
        return AYNI_INVALID_ARGUMENT;
    }
    value.version = input.version;
    const auto contract_status = decode_contract(input.contract, value.contract);
    if (contract_status != AYNI_OK) {
        return contract_status;
    }
    if ((input.contract.input_format == AYNI_CALL_INPUT_VALUE ||
         input.contract.result_format == AYNI_CALL_RESULT_VALUE) &&
        input.business == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (input.business != nullptr) {
        if (input.contract.input_format != AYNI_CALL_INPUT_VALUE) {
            return AYNI_INVALID_ARGUMENT;
        }
        ProjectionShape shape;
        const auto business = validate_business_definition(*input.business, shape);
        if (business != AYNI_OK) {
            return business;
        }
    }
    out = value;
    return AYNI_OK;
}

// Validate the whole nested registration first; neither earlier offers nor
// borrowed views survive failure, including duplicate identity or oversized ABI.
ayni_status_t decode_provider(const ayni_provider_v1 &input, Provider &out) {
    const auto status = check_struct_size(input.struct_size, sizeof(input), sizeof(input));
    if (status != AYNI_OK) {
        return status;
    }
    Provider value{};
    if (input.version == 0U || !value.id.assign(input.provider_id) ||
        !value.endpoint.assign(input.endpoint_id) || input.reserved != 0U ||
        input.capability_count == 0U || input.capability_count > value.offers.size() ||
        input.capabilities == nullptr || input.submit == nullptr ||
        (input.destination_id.size != 0U && !value.destination.assign(input.destination_id))) {
        return AYNI_INVALID_ARGUMENT;
    }
    value.version = input.version;
    value.context = input.ctx;
    value.submit = input.submit;
    value.cancel = input.cancel;
    value.query = input.query;
    value.renew = input.renew;
    for (uint32_t at = 0; at < input.capability_count; ++at) {
        auto &next = value.offers[at];
        const auto decoded = decode_call_capability(input.capabilities[at], next);
        if (decoded != AYNI_OK) {
            return decoded;
        }
        if (ProviderRegistry::offer(value, {next.id.data(), next.id.size()}, next.version) !=
            nullptr) {
            return AYNI_INVALID_ARGUMENT;
        }
        ++value.count;
    }
    if (!valid_provider_callbacks(value)) {
        return AYNI_INVALID_ARGUMENT;
    }
    out = value;
    return AYNI_OK;
}

} // namespace

extern "C" {
ayni_status_t ayni_register_call_capability(ayni_kernel_t *kernel,
                                            const ayni_call_capability_v1 *capability) {
    if (kernel == nullptr || capability == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    ayni::catalog::CallCapability decoded{};
    const auto status = decode_call_capability(*capability, decoded);
    if (status != AYNI_OK) {
        return status;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::registration);
        gated != AYNI_OK) {
        return gated;
    }
    return kernel->configure(decoded, capability->business);
}

ayni_status_t ayni_register_provider(ayni_kernel_t *kernel, const ayni_provider_v1 *provider) {
    if (kernel == nullptr || provider == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    ayni::catalog::Provider decoded{};
    const auto status = decode_provider(*provider, decoded);
    if (status != AYNI_OK) {
        return status;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::registration);
        gated != AYNI_OK) {
        return gated;
    }
    ayni::catalog::ProviderBusinessDefinitions definitions{};
    for (uint32_t at = 0; at < decoded.count; ++at) {
        definitions[at] = provider->capabilities[at].business;
    }
    return kernel->configure(decoded, definitions);
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
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::registration);
        gated != AYNI_OK) {
        return gated;
    }
    return kernel->configure(decoded);
}
} // extern "C"
