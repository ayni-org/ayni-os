#ifndef AYNI_CORE_CATALOG_RESOURCE_RESOURCE_REGISTRY_H
#define AYNI_CORE_CATALOG_RESOURCE_RESOURCE_REGISTRY_H

#include "catalog/catalog_id.h"
#include "foundation/bounded_array.h"
#include <ayni/resources.h>

namespace ayni::catalog {

// Maximum members in one atomic resource requirement group, not registry size.
constexpr uint32_t resource_group_max = 8;

// Trusted immutable description; no health, ownership or device handles here.
struct Resource {
    CatalogId id;              // Copied nonempty unique name.
    ayni_resource_kind_t kind; // Shared or exclusive; capacity obeys kind.
    uint32_t capacity;         // Positive units; exclusive is exactly 1.
    uint32_t flags;            // 0 or ACTUATOR; an actuator is exclusive.
};

// Stable slot indices for the Kernel lifetime. No removal or replacement:
// resolved capability requirements may retain an index after registration.
class ResourceRegistry {
  public:
    static constexpr std::size_t storage_alignment = alignof(Resource);
    // Caller-owned bytes, including zero for a disabled registry.
    static constexpr std::size_t storage_size(uint32_t capacity) {
        return foundation::BoundedArray<Resource>::storage_size(capacity);
    }
    // Storage outlives the registry, aligned to storage_alignment.
    ResourceRegistry(void *storage, uint32_t capacity) : entries_(storage, capacity) {}
    // Validates ABI and domain once, without changing the output on failure.
    static ayni_status_t decode(const ayni_resource_desc_v1 &input, Resource &out);
    // Input came from decode in this call. Failure preserves registration.
    ayni_status_t add_validated(const Resource &resource);
    // Exact name lookup; failure preserves index.
    bool find(ayni_string_v1 id, uint32_t &index) const;
    // Borrowed immutable entry, index < size().
    const Resource &at(uint32_t index) const { return entries_[index]; }
    // Number of registrations, not configured capacity.
    uint32_t size() const { return entries_.size(); }

  private:
    foundation::BoundedArray<Resource> entries_; // Owns descriptions in caller storage.
};

} // namespace ayni::catalog
#endif
