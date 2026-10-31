// Immutable selection of exact capability/Provider versions and endpoints.
#ifndef AYNI_CORE_CATALOG_BINDING_BINDING_REGISTRY_H
#define AYNI_CORE_CATALOG_BINDING_BINDING_REGISTRY_H

#include "catalog/capability/capability_registry.h"
#include "catalog/provider/provider_registry.h"
#include <ayni/detail/tool_usage.hpp>
#include <ayni/directory.h>

namespace ayni::catalog {

// Configuration owns a logical-to-concrete resource substitution, never occupancy.
struct BindingResource {
    CatalogId logical;  // Nonempty source in the capability's immutable demand group.
    CatalogId concrete; // Nonempty destination; units stay with the logical demand.
};

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
    uint32_t resource_count = 0; // Valid mapping prefix; zero preserves all logical IDs.
    std::array<BindingResource, resource_group_max> resources{}; // Owned, unique source IDs.
};

// Local resolution result, not another owner of catalog state. Indices stay
// valid after start closes registration; resources retain normalized ID order.
struct BindingLinks {
    uint32_t capability;   // Index in CapabilityRegistry (authority fields).
    uint32_t call;         // Index in CallCapabilityRegistry (execution contract).
    uint32_t provider;     // Index in ProviderRegistry (runtime-owned callback context).
    CallContract contract; // Resolved execution value; only resource IDs differ from capability.
    std::array<uint32_t, resource_group_max> resources; // Same sorted prefix as contract resources.
};

// Bounded immutable directory; lookup and validation never execute callbacks.
class BindingRegistry {
  public:
    // Caller arena layout; zero capacity is a disabled directory.
    static constexpr std::size_t storage_alignment = alignof(Binding);
    static constexpr std::size_t storage_size(uint32_t capacity) {
        return foundation::BoundedArray<Binding>::storage_size(capacity) +
               sizeof(Description) * capacity;
    }
    // Borrows aligned storage for this registry's lifetime.
    BindingRegistry(void *storage, uint32_t capacity);
    // Validate ABI/identity only; references may be registered later. Failure preserves out.
    static ayni_status_t decode(const ayni_binding_v1 &input, Binding &out);
    // Already decoded in this call; duplicate/full changes no registration.
    ayni_status_t add_validated(const Binding &binding);
    // Metadata is adopted separately after exact capability/Binding registration.
    // Failure preserves the registry; only one description per frozen selection.
    ayni_status_t describe(uint32_t index, ayni_string_v1 summary, const ayni_tool_usage_v1 &usage,
                           const ayni_tool_v1 &tool);
    bool has_description(uint32_t index) const;   // Missing metadata is not discoverable.
    ayni_string_v1 summary(uint32_t index) const; // Borrowed immutable adopted summary.
    void usage(uint32_t index, ayni_tool_usage_v1 &out,
               std::array<ayni_tool_field_usage_v1, AYNI_AUTHORITY_FIELD_COUNT_MAX> &fields) const;
    // Exact selection, no fallback to another version; failure preserves index.
    bool find(ayni_string_v1 id, uint32_t version, uint32_t &index) const;
    // Apply a validated map to a normalized logical contract. Shared by execution
    // ownership and ordinary resolution; failure preserves out.
    static ayni_status_t resolve_contract(const Binding &binding, const CallContract &logical,
                                          CallContract &out);
    // Resolve exact declarations/resources and trusted target/destination membership.
    // Does not check Grant, health or occupancy. Any error preserves out.
    static ayni_status_t resolve(const Binding &binding, const CapabilityRegistry &capabilities,
                                 const CallCapabilityRegistry &calls,
                                 const ProviderRegistry &providers,
                                 const ResourceRegistry &resources, BindingLinks &out);
    // Compare route identities. The caller separately compares the saved effective contract,
    // including resolved resources; CallIdentity deliberately does not copy configuration maps.
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
    uint32_t capacity() const {
        return entries_.capacity();
    } // Maximum frozen selections and policy refs.

  private:
    struct Description {
        bool present = false;      // False until complete exact-pair adoption succeeds.
        uint32_t summary_size = 0; // Bytes, positive iff present; no truncation.
        std::array<char, AYNI_CAPABILITY_SUMMARY_MAX_BYTES> summary{}; // Owns UTF-8 bytes.
        detail::ToolUsage usage; // Owns complete description and per-field explanations.
    };
    Description
        *descriptions_; // Same stable index as entries_; caller arena, never pointers into backend.
    foundation::BoundedArray<Binding> entries_; // Sole owner of immutable selections.
};

} // namespace ayni::catalog
#endif
