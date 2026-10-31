#include "catalog/provider/provider_registry.h"
#include "catalog/capability/business_encoding.h"

namespace ayni::catalog {
// The aggregate descriptor must make supported operations unambiguous. A
// local-only Provider cannot retain an unused external destination token.
bool valid_provider_callbacks(const Provider &value) {
    bool cancel = false;
    bool query = false;
    bool egress = false;
    bool subscription = false;
    for (uint32_t at = 0; at < value.count; ++at) {
        const auto &contract = value.offers[at].contract;
        cancel = cancel || contract.cancel != CancelMode::none;
        query = query || contract.query != QueryAuthority::none;
        subscription = subscription || contract.form == CallForm::subscription;
        egress = egress || (contract.effects & call_effect_egress) != 0U;
    }
    return cancel == (value.cancel != nullptr) && query == (value.query != nullptr) &&
           egress == (value.destination.size() != 0U) && subscription == (value.renew != nullptr);
}

ayni_status_t ProviderRegistry::add_validated(const Provider &provider,
                                              const ProviderBusinessDefinitions &definitions) {
    uint32_t index = 0;
    if (find({provider.id.data(), provider.id.size()}, provider.version, index)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (entries_.full()) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    Provider candidate = provider;
    for (uint32_t at = 0; at < provider.count; ++at) {
        auto *slot =
            proofs_ +
            (static_cast<std::size_t>(entries_.size()) * AYNI_PROVIDER_CAPABILITY_COUNT_MAX + at) *
                proof_bytes_;
        const auto stored =
            store_business_contract(provider.offers[at].contract, definitions[at], slot,
                                    proof_bytes_, candidate.offers[at].contract.business_contract);
        if (stored != AYNI_OK) {
            return stored;
        }
    }
    return entries_.push_back(candidate) ? AYNI_OK : AYNI_CAPACITY_EXCEEDED;
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
namespace {
bool matches_route(const Provider &provider, const CallContract &contract, ayni_string_v1 endpoint,
                   ayni_string_v1 destination) {
    return provider.endpoint.equals(endpoint) &&
           ((contract.effects & call_effect_egress) != 0U ? provider.destination.equals(destination)
                                                          : destination.size == 0U);
}
} // namespace

// C-BIND-001: selection cannot strengthen an offer by changing its contract
// or target. Only egress offers bind the Provider's external destination.
bool ProviderRegistry::matches(const Provider &provider, ayni_string_v1 capability,
                               uint32_t version, const CallContract &contract,
                               ayni_string_v1 endpoint, ayni_string_v1 destination) {
    const auto *offered = offer(provider, capability, version);
    return offered != nullptr && same_call_contract(contract, offered->contract) &&
           matches_route(provider, contract, endpoint, destination);
}

bool ProviderRegistry::matches_saved_call(const Provider &provider, ayni_string_v1 capability,
                                          uint32_t version, const CallContract &effective,
                                          ayni_string_v1 endpoint, ayni_string_v1 destination) {
    const auto *offered = offer(provider, capability, version);
    return offered != nullptr && bound_call_contract(offered->contract, effective) &&
           matches_route(provider, effective, endpoint, destination);
}
} // namespace ayni::catalog
