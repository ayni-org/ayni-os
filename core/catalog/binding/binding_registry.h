// Immutable selection of exact capability/Provider versions and endpoints.
#ifndef AYNI_CORE_CATALOG_BINDING_BINDING_REGISTRY_H
#define AYNI_CORE_CATALOG_BINDING_BINDING_REGISTRY_H

#include "catalog/capability/capability_registry.h"
#include "catalog/provider/provider_registry.h"

namespace ayni::catalog {

// Copied declarative selection; resolution never replaces these identities.
struct Binding {
    CatalogId id;                // Nonempty exact Binding identity.
    uint32_t version;            // Positive version; no implicit latest selection.
    CatalogId capability;        // Registered authority and execution capability ID.
    uint32_t capability_version; // Positive exact version of both declarations.
    CatalogId provider;          // Registered trusted implementation ID.
    uint32_t provider_version;   // Positive exact implementation version.
    CatalogId endpoint;          // Must match that Provider's endpoint.
    CatalogId destination;       // Empty for local calls; matches Provider for EGRESS.
};

// Local resolution result, not another owner of catalog state. Indices stay
// valid after start closes registration; resources retain normalized ID order.
struct BindingLinks {
    uint32_t capability; // Index in CapabilityRegistry (authority fields).
    uint32_t call;       // Index in CallCapabilityRegistry (execution contract).
    uint32_t provider;   // Index in ProviderRegistry (runtime-owned callback context).
    std::array<uint32_t, resource_group_max> resources; // Prefix length from call contract.
};

// Bounded immutable directory; lookup and validation never execute callbacks.
class BindingRegistry {
  public:
    // Caller arena layout; zero capacity is a disabled directory.
    static constexpr std::size_t storage_alignment = alignof(Binding);
    static constexpr std::size_t storage_size(uint32_t capacity) {
        return foundation::BoundedArray<Binding>::storage_size(capacity);
    }
    // Borrows aligned storage for this registry's lifetime.
    BindingRegistry(void *storage, uint32_t capacity) : entries_(storage, capacity) {}
    // Validate ABI/identity only; references may be registered later. Failure preserves out.
    static ayni_status_t decode(const ayni_binding_v1 &input, Binding &out);
    // Already decoded in this call; duplicate/full changes no registration.
    ayni_status_t add_validated(const Binding &binding);
    // Exact selection, no fallback to another version; failure preserves index.
    bool find(ayni_string_v1 id, uint32_t version, uint32_t &index) const;
    // Resolve exact declarations/resources and trusted target/destination membership.
    // Does not check Grant, health or occupancy. Any error preserves out.
    static ayni_status_t resolve(const Binding &binding, const CapabilityRegistry &capabilities,
                                 const CallCapabilityRegistry &calls,
                                 const ProviderRegistry &providers,
                                 const ResourceRegistry &resources, BindingLinks &out);
    // Exact immutable selection equality, independent of current directory availability.
    static bool same_selection(const Binding &left, const Binding &right);
    // Pure startup cross-reference validation; failure keeps CONFIGURING usable.
    ayni_status_t validate_all(const CapabilityRegistry &capabilities,
                               const CallCapabilityRegistry &calls,
                               const ProviderRegistry &providers,
                               const ResourceRegistry &resources) const;
    // Immutable occupied entry, index < size().
    const Binding &at(uint32_t index) const { return entries_[index]; }
    // Occupied entries, not configured capacity.
    uint32_t size() const { return entries_.size(); }

  private:
    foundation::BoundedArray<Binding> entries_; // Sole owner of immutable selections.
};

} // namespace ayni::catalog
#endif
