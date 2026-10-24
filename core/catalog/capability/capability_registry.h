#ifndef AYNI_CORE_CATALOG_CAPABILITY_CAPABILITY_REGISTRY_H
#define AYNI_CORE_CATALOG_CAPABILITY_CAPABILITY_REGISTRY_H

#include "catalog/capability/field_domain.h"
#include "catalog/catalog_id.h"
#include "foundation/bounded_array.h"

#include <ayni/authority.h>

#include <cstddef>
#include <cstdint>

namespace ayni::catalog {

// Copied identity; the canonical domain occupies the matching byte slot.
struct Capability {
    CatalogId id;             // Exact, immutable capability name.
    uint32_t version = 0;     // Nonzero published version.
    uint32_t domain_size = 0; // Used bytes within the configured domain stride.
};

// Immutable registration metadata followed by one fixed byte stride per
// capability. The caller owns the storage; no input descriptor stays borrowed.
class CapabilityRegistry {
  public:
    // Alignment of caller-owned metadata and domain slots.
    static constexpr std::size_t storage_alignment = alignof(Capability);
    // capacity and scope_max_bytes obey the public configuration bounds.
    static constexpr std::size_t storage_size(uint32_t capacity, uint32_t scope_max_bytes) {
        return foundation::BoundedArray<Capability>::storage_size(capacity) +
               static_cast<std::size_t>(capacity) * scope_max_bytes;
    }

    // Borrows storage for this registry lifetime.
    CapabilityRegistry(void *storage, uint32_t capacity, uint32_t scope_max_bytes);
    // Pure descriptor admission, independent of lifecycle/configured capacity.
    // Only success publishes the domain plan; input views must stay unchanged.
    static ayni_status_t validate(const ayni_capability_desc_v1 &input, DomainEncoding &out);
    // Checked standalone registration; failures preserve all slots.
    ayni_status_t add(const ayni_capability_desc_v1 &input);
    // Same registration after validate() in this call; input/encoding stay paired.
    ayni_status_t add_validated(const ayni_capability_desc_v1 &input,
                                const DomainEncoding &encoding);
    // Distinguishes a missing ID from a missing version of a known ID.
    struct Lookup {
        enum class Kind { found, unknown_id, unknown_version } kind;
        uint32_t index; // Slot index, meaningful only for found.
    };
    // Exact immutable ID/version lookup.
    Lookup find(ayni_string_v1 id, uint32_t version) const;
    // Borrows metadata at a valid slot index.
    const Capability &at(uint32_t index) const { return capabilities_[index]; }
    // Complete canonical domain equality for a saved authorization ceiling.
    bool matches_domain(uint32_t index, ayni_bytes_v1 original) const;
    // Number of occupied slots.
    uint32_t size() const { return capabilities_.size(); }
    // Borrows canonical bytes at a valid slot until registry destruction.
    ayni_bytes_v1 domain_view(uint32_t index) const;

  private:
    // One slot-address formula; only successful registration writes bytes.
    uint8_t *domain_at(uint32_t index) const;
    foundation::BoundedArray<Capability> capabilities_;
    uint8_t *domains_;
    uint32_t scope_max_bytes_;
};

} // namespace ayni::catalog
#endif // AYNI_CORE_CATALOG_CAPABILITY_CAPABILITY_REGISTRY_H
