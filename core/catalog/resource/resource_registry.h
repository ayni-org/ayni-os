#ifndef AYNI_CORE_CATALOG_RESOURCE_RESOURCE_REGISTRY_H
#define AYNI_CORE_CATALOG_RESOURCE_RESOURCE_REGISTRY_H

#include "catalog/catalog_id.h"
#include "catalog/device/device_catalog.h"
#include "foundation/bounded_array.h"
#include <array>
#include <ayni/resources.h>

namespace ayni::catalog {

// Maximum members in one atomic resource requirement group, not registry size.
constexpr uint32_t resource_group_max = AYNI_RESOURCE_GROUP_COUNT_MAX;

// Trusted immutable description; no health, ownership or device handles here.
struct Resource {
    CatalogId id;              // Copied nonempty unique name.
    ayni_resource_kind_t kind; // Shared or exclusive; capacity obeys kind.
    uint32_t capacity;         // Positive units; exclusive is exactly 1.
    uint32_t flags;            // 0 or ACTUATOR; an actuator is exclusive.
    uint32_t component_count;  // Occupied prefix of components; zero for virtual resources.
    std::array<CatalogId, AYNI_RESOURCE_COMPONENT_COUNT_MAX>
        components;            // Unique copied associations.
    CatalogId conflict_domain; // Empty means independent; different same-domain resources conflict.
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
    // Startup inventory validation, without resolving mutable health or occupancy.
    ayni_status_t validate(const DeviceCatalog &devices) const;
    // Different resources conflict only when both name the same nonempty domain.
    bool conflict(uint32_t left, uint32_t right) const;
    // All indices are validated; one request must not contain mutually exclusive resources.
    bool compatible_group(const uint32_t *indices, uint32_t count) const;
    // Count resources naming this component; each association is unique.
    uint32_t component_resources(ayni_string_v1 component) const;
    // A resource spanning several components of one device is counted only once.
    uint32_t device_resources(const DeviceCatalog &devices, ayni_string_v1 device) const;
    // Borrowed immutable entry, index < size().
    const Resource &at(uint32_t index) const { return entries_[index]; }
    // Number of registrations, not configured capacity.
    uint32_t size() const { return entries_.size(); }

  private:
    foundation::BoundedArray<Resource> entries_; // Owns descriptions in caller storage.
};

} // namespace ayni::catalog
#endif
