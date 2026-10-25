// Trusted inventory links; no health, execution authority or network routing.
#ifndef AYNI_CORE_CATALOG_DEVICE_DEVICE_CATALOG_H
#define AYNI_CORE_CATALOG_DEVICE_DEVICE_CATALOG_H
#include "catalog/catalog_id.h"
#include "foundation/bounded_array.h"
#include <ayni/resources.h>

namespace ayni::catalog {
// Device inventory identity; no mutable runtime facts.
struct Device {
    CatalogId id; // Nonempty unique ID in this Kernel.
};
// One component has one immutable parent device, resolved at start.
struct Component {
    CatalogId id;     // Nonempty unique component ID in this Kernel.
    CatalogId device; // Nonempty parent device ID, never inferred from a name prefix.
};
// Owns the two-level inventory forest. Resources refer to components by stable IDs.
class DeviceCatalog {
  public:
    static constexpr std::size_t storage_alignment = alignof(Component);
    // Each separate block is caller owned; zero capacity is disabled.
    static constexpr std::size_t device_storage_size(uint32_t capacity) {
        return foundation::BoundedArray<Device>::storage_size(capacity);
    }
    static constexpr std::size_t component_storage_size(uint32_t capacity) {
        return foundation::BoundedArray<Component>::storage_size(capacity);
    }
    // Both aligned blocks outlive this object; no allocation or dynamic discovery.
    DeviceCatalog(void *devices, uint32_t device_capacity, void *components,
                  uint32_t component_capacity)
        : devices_(devices, device_capacity), components_(components, component_capacity) {}
    // Shape validators copy input; failure preserves output, links checked only at start.
    static ayni_status_t decode(const ayni_device_desc_v1 &input, Device &out);
    static ayni_status_t decode(const ayni_component_desc_v1 &input, Component &out);
    // Input already decoded in this call; duplicate/full preserves all prior entries.
    ayni_status_t add_validated(const Device &device);
    ayni_status_t add_validated(const Component &component);
    // Exact lookup, no discovery or fallback; failure preserves index.
    bool find_device(ayni_string_v1 id, uint32_t &index) const;
    bool find_component(ayni_string_v1 id, uint32_t &index) const;
    // Pure cross-reference check; registration order does not affect the result.
    ayni_status_t validate() const;
    // Number of components naming this device, independent of resource health.
    uint32_t device_component_count(ayni_string_v1 device) const;
    // Immutable component found by find_component; no mutable relationship escapes.
    const Component &component(uint32_t index) const { return components_[index]; }

  private:
    foundation::BoundedArray<Device> devices_;       // Sole owner of device identities.
    foundation::BoundedArray<Component> components_; // Sole owner of parent relationships.
};
} // namespace ayni::catalog
#endif
