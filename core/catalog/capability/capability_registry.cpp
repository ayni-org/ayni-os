#include "catalog/capability/capability_registry.h"

#include "catalog/abi_checks.h"
#include "catalog/capability/field_domain.h"

#include <cstring>
#include <limits>

namespace ayni::catalog {

static_assert(CapabilityRegistry::storage_size(AYNI_CAPABILITY_CAPACITY_MAX,
                                               AYNI_AUTHORITY_SCOPE_MAX_BYTES_MAX) <=
                  std::numeric_limits<uint32_t>::max(),
              "the configured arena size fits on a 32-bit Host");

CapabilityRegistry::CapabilityRegistry(void *storage, uint32_t capacity, uint32_t scope_max_bytes)
    : capabilities_(storage, capacity),
      domains_(capacity == 0U ? nullptr
                              : static_cast<uint8_t *>(storage) +
                                    foundation::BoundedArray<Capability>::storage_size(capacity)),
      scope_max_bytes_(scope_max_bytes) {}

ayni_status_t CapabilityRegistry::validate(const ayni_capability_desc_v1 &input,
                                           DomainEncoding &out) {
    const auto size_status = check_struct_size(input.struct_size, sizeof(input), sizeof(input));
    if (size_status != AYNI_OK) {
        return size_status;
    }
    if (input.version == 0U || input.reserved != 0U || !CatalogId::is_valid(input.capability_id)) {
        return AYNI_INVALID_ARGUMENT;
    }
    return inspect_domain(input.fields, input.field_count, out);
}

ayni_status_t CapabilityRegistry::add(const ayni_capability_desc_v1 &input) {
    DomainEncoding encoding{};
    const auto checked = validate(input, encoding);
    return checked == AYNI_OK ? add_validated(input, encoding) : checked;
}

// The caller admitted the unchanged descriptor before lifecycle/capacity checks.
// Only this method consumes a slot, after duplicate and complete-byte admission.
ayni_status_t CapabilityRegistry::add_validated(const ayni_capability_desc_v1 &input,
                                                const DomainEncoding &encoding) {
    if (find(input.capability_id, input.version).kind == Lookup::Kind::found) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (capabilities_.full() || encoding.size > scope_max_bytes_) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    Capability capability{};
    static_cast<void>(capability.id.assign(input.capability_id));
    capability.version = input.version;
    write_domain(input.fields, encoding, domain_at(size()));
    capability.domain_size = encoding.size;
    static_cast<void>(capabilities_.push_back(capability));
    return AYNI_OK;
}

CapabilityRegistry::Lookup CapabilityRegistry::find(ayni_string_v1 id, uint32_t version) const {
    Lookup found{Lookup::Kind::unknown_id, 0};
    for (uint32_t index = 0; index < size(); ++index) {
        const auto &capability = capabilities_[index];
        if (!capability.id.equals(id)) {
            continue;
        }
        if (capability.version == version) {
            return {Lookup::Kind::found, index};
        }
        found.kind = Lookup::Kind::unknown_version;
    }
    return found;
}

uint8_t *CapabilityRegistry::domain_at(uint32_t index) const {
    return domains_ + static_cast<std::size_t>(index) * scope_max_bytes_;
}

ayni_bytes_v1 CapabilityRegistry::domain_view(uint32_t index) const {
    return {domain_at(index), at(index).domain_size};
}

bool CapabilityRegistry::matches_domain(uint32_t index, ayni_bytes_v1 original) const {
    const auto current = domain_view(index);
    return original.size == current.size && original.data != nullptr &&
           std::memcmp(original.data, current.data, current.size) == 0;
}

} // namespace ayni::catalog
