#include "catalog/provider/provider_registry.h"

#include "catalog/abi_checks.h"

namespace ayni::catalog {
namespace {
// The aggregate descriptor must make supported operations unambiguous. A
// local-only Provider cannot retain an unused external destination token.
bool callbacks_match(const Provider &value) {
    bool cancel = false;
    bool query = false;
    bool egress = false;
    for (uint32_t at = 0; at < value.count; ++at) {
        const auto &contract = value.offers[at].contract;
        cancel = cancel || contract.cancel != CancelMode::none;
        query = query || contract.query != QueryAuthority::none;
        egress = egress || (contract.effects & call_effect_egress) != 0U;
    }
    return cancel == (value.cancel != nullptr) && query == (value.query != nullptr) &&
           egress == (value.destination.size() != 0U);
}
} // namespace

// Validate the whole nested registration first; neither earlier offers nor
// borrowed views survive failure, including duplicate identity or oversized ABI.
ayni_status_t ProviderRegistry::decode(const ayni_provider_v1 &input, Provider &out) {
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
    for (uint32_t at = 0; at < input.capability_count; ++at) {
        auto &next = value.offers[at];
        const auto decoded = decode_call_capability(input.capabilities[at], next);
        if (decoded != AYNI_OK) {
            return decoded;
        }
        if (offer(value, {next.id.data(), next.id.size()}, next.version) != nullptr) {
            return AYNI_INVALID_ARGUMENT;
        }
        ++value.count;
    }
    if (!callbacks_match(value)) {
        return AYNI_INVALID_ARGUMENT;
    }
    out = value;
    return AYNI_OK;
}

ayni_status_t ProviderRegistry::add_validated(const Provider &provider) {
    uint32_t index = 0;
    if (find({provider.id.data(), provider.id.size()}, provider.version, index)) {
        return AYNI_INVALID_ARGUMENT;
    }
    return entries_.push_back(provider) ? AYNI_OK : AYNI_CAPACITY_EXCEEDED;
}

bool ProviderRegistry::find(ayni_string_v1 id, uint32_t version, uint32_t &index) const {
    for (uint32_t at = 0; at < entries_.size(); ++at) {
        if (entries_[at].id.equals(id) && entries_[at].version == version) {
            index = at;
            return true;
        }
    }
    return false;
}

const CallCapability *ProviderRegistry::offer(const Provider &provider, ayni_string_v1 capability,
                                              uint32_t version) {
    for (uint32_t at = 0; at < provider.count; ++at) {
        const auto &entry = provider.offers[at];
        if (entry.id.equals(capability) && entry.version == version) {
            return &entry;
        }
    }
    return nullptr;
}
// C-BIND-001: selection cannot strengthen an offer by changing its contract
// or target. Only egress offers bind the Provider's external destination.
bool ProviderRegistry::matches(const Provider &provider, ayni_string_v1 capability,
                               uint32_t version, const CallContract &contract,
                               ayni_string_v1 endpoint, ayni_string_v1 destination) {
    const auto *offered = offer(provider, capability, version);
    return offered != nullptr && same_call_contract(contract, offered->contract) &&
           provider.endpoint.equals(endpoint) &&
           ((contract.effects & call_effect_egress) != 0U ? provider.destination.equals(destination)
                                                          : destination.size == 0U);
}
} // namespace ayni::catalog
