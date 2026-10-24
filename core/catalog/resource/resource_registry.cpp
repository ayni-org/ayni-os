#include "catalog/resource/resource_registry.h"
#include "catalog/abi_checks.h"

namespace ayni::catalog {

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
        (input.flags == AYNI_RESOURCE_ACTUATOR && input.kind != AYNI_RESOURCE_EXCLUSIVE)) {
        return AYNI_INVALID_ARGUMENT;
    }
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

} // namespace ayni::catalog
