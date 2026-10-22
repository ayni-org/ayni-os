#ifndef AYNI_CORE_CATALOG_CAPABILITY_CAPABILITY_REGISTRY_H
#define AYNI_CORE_CATALOG_CAPABILITY_CAPABILITY_REGISTRY_H

#include "catalog/catalog_id.h"
#include "foundation/bounded_array.h"

#include <ayni/authority.h>

#include <cstddef>
#include <cstdint>

namespace ayni::catalog {

struct Capability {
    CatalogId id;
    uint32_t version = 0;
    uint32_t domain_size = 0;
};

// Immutable registration metadata followed by one fixed byte stride per
// capability. The caller owns the storage; no input descriptor stays borrowed.
class CapabilityRegistry {
  public:
    static constexpr std::size_t storage_alignment = alignof(Capability);
    // capacity and scope_max_bytes obey the public configuration bounds.
    static constexpr std::size_t storage_size(uint32_t capacity, uint32_t scope_max_bytes) {
        return foundation::BoundedArray<Capability>::storage_size(capacity) +
               static_cast<std::size_t>(capacity) * scope_max_bytes;
    }

    CapabilityRegistry(void *storage, uint32_t capacity, uint32_t scope_max_bytes);
    // Pure descriptor validation, independent of lifecycle and configured
    // capacity. Publishes the required canonical bytes only on success.
    static ayni_status_t validate(const ayni_capability_desc_v1 &input, uint32_t &out_domain_size);
    ayni_status_t add(const ayni_capability_desc_v1 &input);
    struct Lookup {
        enum class Kind { found, unknown_id, unknown_version } kind;
        uint32_t index;
    };
    Lookup find(ayni_string_v1 id, uint32_t version) const;
    const Capability &at(uint32_t index) const { return capabilities_[index]; }
    uint32_t size() const { return capabilities_.size(); }
    ayni_bytes_v1 domain_view(uint32_t index) const;

  private:
    foundation::BoundedArray<Capability> capabilities_;
    uint8_t *domains_;
    uint32_t scope_max_bytes_;
};

} // namespace ayni::catalog
#endif // AYNI_CORE_CATALOG_CAPABILITY_CAPABILITY_REGISTRY_H
