#include "catalog/device/device_catalog.h"
#include "catalog/abi_checks.h"

namespace ayni::catalog {
namespace {
// Both inventory levels use the same exact ID rule without sharing representation.
template <typename Entry>
bool find(const foundation::BoundedArray<Entry> &entries, ayni_string_v1 id, uint32_t &index) {
    for (uint32_t at = 0; at < entries.size(); ++at) {
        if (entries[at].id.equals(id)) {
            index = at;
            return true;
        }
    }
    return false;
}
} // namespace

ayni_status_t DeviceCatalog::decode(const ayni_device_desc_v1 &input, Device &out) {
    const auto status = check_struct_size(input.struct_size, sizeof(input), sizeof(input));
    if (status != AYNI_OK) {
        return status;
    }
    Device decoded{};
    if (input.reserved != 0U || !decoded.id.assign(input.device_id)) {
        return AYNI_INVALID_ARGUMENT;
    }
    out = decoded;
    return AYNI_OK;
}
ayni_status_t DeviceCatalog::decode(const ayni_component_desc_v1 &input, Component &out) {
    const auto status = check_struct_size(input.struct_size, sizeof(input), sizeof(input));
    if (status != AYNI_OK) {
        return status;
    }
    Component decoded{};
    if (input.reserved != 0U || !decoded.id.assign(input.component_id) ||
        !decoded.device.assign(input.device_id)) {
        return AYNI_INVALID_ARGUMENT;
    }
    out = decoded;
    return AYNI_OK;
}
ayni_status_t DeviceCatalog::add_validated(const Device &device) {
    uint32_t index = 0;
    if (find_device({device.id.data(), device.id.size()}, index)) {
        return AYNI_INVALID_ARGUMENT;
    }
    return devices_.push_back(device) ? AYNI_OK : AYNI_CAPACITY_EXCEEDED;
}
ayni_status_t DeviceCatalog::add_validated(const Component &component) {
    uint32_t index = 0;
    if (find_component({component.id.data(), component.id.size()}, index)) {
        return AYNI_INVALID_ARGUMENT;
    }
    return components_.push_back(component) ? AYNI_OK : AYNI_CAPACITY_EXCEEDED;
}
bool DeviceCatalog::find_device(ayni_string_v1 id, uint32_t &index) const {
    return find(devices_, id, index);
}
bool DeviceCatalog::find_component(ayni_string_v1 id, uint32_t &index) const {
    return find(components_, id, index);
}
uint32_t DeviceCatalog::device_component_count(ayni_string_v1 device) const {
    uint32_t count = 0;
    for (uint32_t at = 0; at < components_.size(); ++at) {
        if (components_[at].device.equals(device)) {
            ++count;
        }
    }
    return count;
}
ayni_status_t DeviceCatalog::validate() const {
    for (uint32_t at = 0; at < components_.size(); ++at) {
        const auto &id = components_[at].device;
        uint32_t index = 0;
        if (!find_device({id.data(), id.size()}, index)) {
            return AYNI_NOT_FOUND;
        }
    }
    return AYNI_OK;
}
} // namespace ayni::catalog
