#include "catalog/resource/resource_registry.h"
#include "catalog/abi_checks.h"

namespace ayni::catalog {

// Copy and validate the entire descriptor before reserving a slot. References
// are resolved only at start so registration order cannot change acceptance.
ayni_status_t ResourceRegistry::decode(const ayni_resource_desc_v1 &input, Resource &out) {
    const auto status = check_struct_size(input.struct_size, sizeof(input), sizeof(input));
    if (status != AYNI_OK) {
        return status;
    }
    Resource decoded{};
    if (!decoded.id.assign(input.resource_id) || input.capacity == 0U ||
        (input.kind != AYNI_RESOURCE_EXCLUSIVE && input.kind != AYNI_RESOURCE_SHARED) ||
        (input.kind == AYNI_RESOURCE_EXCLUSIVE && input.capacity != 1U) ||
        (input.flags & ~AYNI_RESOURCE_ACTUATOR) != 0U ||
        (input.flags == AYNI_RESOURCE_ACTUATOR && input.kind != AYNI_RESOURCE_EXCLUSIVE) ||
        input.reserved != 0U || input.component_count > AYNI_RESOURCE_COMPONENT_COUNT_MAX ||
        (input.component_count != 0U && input.components == nullptr) ||
        (input.conflict_domain_id.size != 0U &&
         !decoded.conflict_domain.assign(input.conflict_domain_id))) {
        return AYNI_INVALID_ARGUMENT;
    }
    for (uint32_t at = 0; at < input.component_count; ++at) {
        if (!decoded.components[at].assign(input.components[at])) {
            return AYNI_INVALID_ARGUMENT;
        }
        for (uint32_t prior = 0; prior < at; ++prior) {
            if (decoded.components[prior].equals(input.components[at])) {
                return AYNI_INVALID_ARGUMENT;
            }
        }
    }
    decoded.component_count = input.component_count;
    decoded.kind = input.kind;
    decoded.capacity = input.capacity;
    decoded.flags = input.flags;
    out = decoded;
    return AYNI_OK;
}

ayni_status_t ResourceRegistry::add_validated(const Resource &resource) {
    uint32_t existing = 0;
    if (find({resource.id.data(), resource.id.size()}, existing)) {
        return AYNI_INVALID_ARGUMENT;
    }
    return entries_.push_back(resource) ? AYNI_OK : AYNI_CAPACITY_EXCEEDED;
}

bool ResourceRegistry::find(ayni_string_v1 id, uint32_t &index) const {
    for (uint32_t candidate = 0; candidate < entries_.size(); ++candidate) {
        if (entries_[candidate].id.equals(id)) {
            index = candidate;
            return true;
        }
    }
    return false;
}

ayni_status_t ResourceRegistry::validate(const DeviceCatalog &devices) const {
    for (uint32_t at = 0; at < entries_.size(); ++at) {
        const auto &resource = entries_[at];
        for (uint32_t member = 0; member < resource.component_count; ++member) {
            const auto &component = resource.components[member];
            uint32_t index = 0;
            if (!devices.find_component({component.data(), component.size()}, index)) {
                return AYNI_NOT_FOUND;
            }
        }
    }
    return AYNI_OK;
}

bool ResourceRegistry::conflict(uint32_t left, uint32_t right) const {
    const auto &domain = entries_[left].conflict_domain;
    const auto &other = entries_[right].conflict_domain;
    return left != right && domain.size() != 0U && domain.equals({other.data(), other.size()});
}

bool ResourceRegistry::compatible_group(const uint32_t *indices, uint32_t count) const {
    for (uint32_t at = 0; at < count; ++at) {
        for (uint32_t other = 0; other < at; ++other) {
            if (conflict(indices[at], indices[other])) {
                return false;
            }
        }
    }
    return true;
}

uint32_t ResourceRegistry::component_resources(ayni_string_v1 component) const {
    uint32_t count = 0;
    for (uint32_t at = 0; at < entries_.size(); ++at) {
        const auto &resource = entries_[at];
        for (uint32_t member = 0; member < resource.component_count; ++member) {
            if (resource.components[member].equals(component)) {
                ++count;
                break; // A set member contributes one resource, independent of other associations.
            }
        }
    }
    return count;
}

uint32_t ResourceRegistry::device_resources(const DeviceCatalog &devices,
                                            ayni_string_v1 device) const {
    uint32_t count = 0;
    for (uint32_t at = 0; at < entries_.size(); ++at) {
        const auto &resource = entries_[at];
        for (uint32_t member = 0; member < resource.component_count; ++member) {
            const auto &id = resource.components[member];
            uint32_t index = 0;
            if (devices.find_component({id.data(), id.size()}, index) &&
                devices.component(index).device.equals(device)) {
                ++count;
                break; // Two wheels on one device still describe one shared chassis resource.
            }
        }
    }
    return count;
}

} // namespace ayni::catalog
