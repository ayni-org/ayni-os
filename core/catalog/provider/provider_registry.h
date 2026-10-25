// Trusted Provider metadata and callback ownership. No I/O or live attempts.
#ifndef AYNI_CORE_CATALOG_PROVIDER_PROVIDER_REGISTRY_H
#define AYNI_CORE_CATALOG_PROVIDER_PROVIDER_REGISTRY_H

#include "catalog/capability/call_contract.h"

namespace ayni::catalog {

// Fixed copied descriptor. Function/context lifetime remains with runtime;
// no callback may run until the Kernel has durably authorized its attempt.
struct Provider {
    CatalogId id;          // Nonempty stable registration identity.
    uint32_t version;      // Positive exact version; immutable after registration.
    CatalogId endpoint;    // Trusted local endpoint token, never network credentials.
    CatalogId destination; // Empty iff no advertised capability has EGRESS effects.
    void *context;         // Borrowed; null permitted, valid until successful destruction.
    // Complete independent offers; prefix count is valid and every ID/version is distinct.
    std::array<CallCapability, AYNI_PROVIDER_CAPABILITY_COUNT_MAX> offers;
    uint32_t count; // 1..AYNI_PROVIDER_CAPABILITY_COUNT_MAX; unused cells are zeroed.
    decltype(ayni_provider_v1::submit) submit; // Required bounded owner-thread submission.
    decltype(ayni_provider_v1::cancel) cancel; // Null iff no offer supports cancellation.
    decltype(ayni_provider_v1::query) query;   // Null iff no offer supports outcome queries.
};

// Caller-arena directory with stable indices; cannot replace a Provider while
// a saved Binding or in-flight operation still names its exact version.
class ProviderRegistry {
  public:
    // Alignment/bytes for caller storage; zero capacity consumes zero bytes.
    static constexpr std::size_t storage_alignment = alignof(Provider);
    static constexpr std::size_t storage_size(uint32_t capacity) {
        return foundation::BoundedArray<Provider>::storage_size(capacity);
    }
    // Borrows storage for the Kernel lifetime; no ownership of callback contexts.
    ProviderRegistry(void *storage, uint32_t capacity) : entries_(storage, capacity) {}
    // Validates all nested offers before publishing a copy. Failure preserves out.
    static ayni_status_t decode(const ayni_provider_v1 &input, Provider &out);
    // Input came from decode in this synchronous call; duplicate/full preserves entries.
    ayni_status_t add_validated(const Provider &provider);
    // Exact immutable identity lookup; failure preserves index.
    bool find(ayni_string_v1 id, uint32_t version, uint32_t &index) const;
    // Returns an exact offer or null; view lasts until registry destruction.
    static const CallCapability *offer(const Provider &provider, ayni_string_v1 capability,
                                       uint32_t version);
    // Same offer/target rule for initial Binding and saved-operation queries.
    // A mixed Provider's egress token does not belong to its local-only offers.
    static bool matches(const Provider &provider, ayni_string_v1 capability, uint32_t version,
                        const CallContract &contract, ayni_string_v1 endpoint,
                        ayni_string_v1 destination);
    // Immutable occupied entry, index < size().
    const Provider &at(uint32_t index) const { return entries_[index]; }
    // Number of registered Providers, not arena capacity.
    uint32_t size() const { return entries_.size(); }

  private:
    foundation::BoundedArray<Provider> entries_; // Sole owner of static copied descriptors.
};

} // namespace ayni::catalog
#endif
