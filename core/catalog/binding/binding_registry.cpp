#include "catalog/binding/binding_registry.h"
#include <cstring>
#include <new>

#include "catalog/abi_checks.h"

namespace ayni::catalog {
BindingRegistry::BindingRegistry(void *storage, uint32_t capacity)
    : descriptions_(
          storage ? reinterpret_cast<Description *>(static_cast<Binding *>(storage) + capacity)
                  : nullptr),
      entries_(storage, capacity) {
    static_assert(alignof(Description) <= alignof(Binding), "metadata arena preserves alignment");
    for (uint32_t at = 0; at < capacity; ++at) {
        new (descriptions_ + at) Description{};
    }
}
ayni_status_t BindingRegistry::describe(uint32_t index, ayni_string_v1 summary,
                                        const ayni_tool_usage_v1 &usage, const ayni_tool_v1 &tool) {
    if (index >= entries_.size() || !summary.size ||
        summary.size > AYNI_CAPABILITY_SUMMARY_MAX_BYTES ||
        ayni_validate_utf8(summary) != AYNI_OK) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (descriptions_[index].present) {
        return AYNI_INVALID_ARGUMENT;
    }
    detail::ToolUsage copied;
    if (!copied.assign(usage, tool)) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto &description = descriptions_[index];
    description.usage = copied;
    std::memcpy(description.summary.data(), summary.data, summary.size);
    description.summary_size = summary.size;
    description.present = true;
    return AYNI_OK;
}
bool BindingRegistry::has_description(uint32_t index) const {
    return index < entries_.size() && descriptions_[index].present;
}
ayni_string_v1 BindingRegistry::summary(uint32_t index) const {
    return {descriptions_[index].summary.data(), descriptions_[index].summary_size};
}
void BindingRegistry::usage(
    uint32_t index, ayni_tool_usage_v1 &out,
    std::array<ayni_tool_field_usage_v1, AYNI_AUTHORITY_FIELD_COUNT_MAX> &fields) const {
    descriptions_[index].usage.read(out, fields);
}

namespace {
ayni_string_v1 view(const CatalogId &id) {
    return {id.data(), id.size()};
}

bool contains_token(const FieldValues &values, const CatalogId &token) {
    for (uint32_t at = 0; at < values.token_count; ++at) {
        if (token.equals(values.tokens[at])) {
            return true;
        }
    }
    return false;
}

// C-AUTH-017: the authority schema, not a second routing projection, defines
// target and egress fields. Dispatch will check the concrete values again;
// merely belonging to a registered domain confers no permission to use them.
bool domain_matches(ayni_bytes_v1 domain, const Binding &binding, const CallContract &contract) {
    DomainReader reader(domain);
    DomainField field;
    bool egress_field = false;
    while (reader.next(field)) {
        if (field.usage == AYNI_AUTHORITY_EXECUTION_TARGET &&
            !contains_token(field.values, binding.endpoint)) {
            return false;
        }
        if (field.usage == AYNI_AUTHORITY_EGRESS_DESTINATION) {
            egress_field = true;
            if (!contains_token(field.values, binding.destination)) {
                return false;
            }
        }
    }
    return reader.ok() && egress_field == ((contract.effects & call_effect_egress) != 0U);
}

} // namespace

ayni_status_t BindingRegistry::decode(const ayni_binding_v1 &input, Binding &out) {
    const auto status = check_struct_size(input.struct_size, sizeof(input), sizeof(input));
    if (status != AYNI_OK) {
        return status;
    }
    Binding value{};
    if (input.version == 0U || input.capability_version == 0U || input.provider_version == 0U ||
        !value.id.assign(input.binding_id) || !value.capability.assign(input.capability_id) ||
        !value.provider.assign(input.provider_id) || !value.endpoint.assign(input.endpoint_id) ||
        (input.destination_id.size != 0U && !value.destination.assign(input.destination_id))) {
        return AYNI_INVALID_ARGUMENT;
    }
    value.version = input.version;
    value.capability_version = input.capability_version;
    value.provider_version = input.provider_version;
    out = value;
    return AYNI_OK;
}

ayni_status_t BindingRegistry::add_validated(const Binding &binding) {
    uint32_t index = 0;
    if (find(view(binding.id), binding.version, index)) {
        return AYNI_INVALID_ARGUMENT;
    }
    return entries_.push_back(binding) ? AYNI_OK : AYNI_CAPACITY_EXCEEDED;
}

bool BindingRegistry::find(ayni_string_v1 id, uint32_t version, uint32_t &index) const {
    for (uint32_t at = 0; at < entries_.size(); ++at) {
        if (entries_[at].id.equals(id) && entries_[at].version == version) {
            index = at;
            return true;
        }
    }
    return false;
}

// All checks use immutable registrations and publish links only on success.
// Resource health belongs to runtime admission, so OFFLINE is not a link error.
ayni_status_t BindingRegistry::resolve(const Binding &binding,
                                       const CapabilityRegistry &capabilities,
                                       const CallCapabilityRegistry &calls,
                                       const ProviderRegistry &providers,
                                       const ResourceRegistry &resources, BindingLinks &out) {
    BindingLinks links{};
    const auto capability = capabilities.find(view(binding.capability), binding.capability_version);
    if (capability.kind != CapabilityRegistry::Lookup::Kind::found ||
        !calls.find(view(binding.capability), binding.capability_version, links.call) ||
        !providers.find(view(binding.provider), binding.provider_version, links.provider)) {
        return AYNI_NOT_FOUND;
    }
    links.capability = capability.index;
    const auto &call = calls.at(links.call);
    if (!ProviderRegistry::matches(providers.at(links.provider), view(binding.capability),
                                   binding.capability_version, call.contract,
                                   view(binding.endpoint), view(binding.destination)) ||
        !domain_matches(capabilities.domain_view(links.capability), binding, call.contract)) {
        return AYNI_INVALID_ARGUMENT;
    }
    for (uint32_t at = 0; at < call.contract.resource_count; ++at) {
        const auto &required = call.contract.resources[at];
        if (!resources.find(view(required.id), links.resources[at])) {
            return AYNI_NOT_FOUND;
        }
        if (resources.at(links.resources[at]).capacity < required.units) {
            return AYNI_CAPACITY_EXCEEDED;
        }
    }
    if (!resources.compatible_group(links.resources.data(), call.contract.resource_count)) {
        return AYNI_INVALID_ARGUMENT;
    }
    out = links;
    return AYNI_OK;
}

bool BindingRegistry::same_selection(const Binding &left, const Binding &right) {
    return left.id.equals(view(right.id)) && left.version == right.version &&
           left.capability.equals(view(right.capability)) &&
           left.capability_version == right.capability_version &&
           left.provider.equals(view(right.provider)) &&
           left.provider_version == right.provider_version &&
           left.endpoint.equals(view(right.endpoint)) &&
           left.destination.equals(view(right.destination));
}

ayni_status_t BindingRegistry::validate_all(const CapabilityRegistry &capabilities,
                                            const CallCapabilityRegistry &calls,
                                            const ProviderRegistry &providers,
                                            const ResourceRegistry &resources) const {
    for (uint32_t at = 0; at < entries_.size(); ++at) {
        BindingLinks links{};
        const auto status = resolve(entries_[at], capabilities, calls, providers, resources, links);
        if (status != AYNI_OK) {
            return status;
        }
    }
    return AYNI_OK;
}
} // namespace ayni::catalog
