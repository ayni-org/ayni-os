#include "catalog/capability/capability_registry.h"

#include "catalog/capability/field_domain.h"

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
                                           uint32_t &out_domain_size) {
    if (input.struct_size != sizeof(input)) {
        return input.struct_size < sizeof(input) ? AYNI_INVALID_ARGUMENT : AYNI_UNSUPPORTED_VERSION;
    }
    if (input.version == 0U || input.reserved != 0U || !CatalogId::is_valid(input.capability_id)) {
        return AYNI_INVALID_ARGUMENT;
    }
    uint32_t needed = 0;
    const auto checked = encode_domain(input.fields, input.field_count, nullptr, 0, needed);
    if (checked != AYNI_BUFFER_TOO_SMALL) {
        return checked;
    }
    out_domain_size = needed;
    return AYNI_OK;
}

ayni_status_t CapabilityRegistry::add(const ayni_capability_desc_v1 &input) {
    uint32_t needed = 0;
    const auto checked = validate(input, needed);
    if (checked != AYNI_OK) {
        return checked;
    }
    if (find(input.capability_id, input.version).kind == Lookup::Kind::found) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (capabilities_.full() || needed > scope_max_bytes_) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    Capability capability{};
    static_cast<void>(capability.id.assign(input.capability_id));
    capability.version = input.version;
    uint8_t *domain = domains_ + static_cast<std::size_t>(size()) * scope_max_bytes_;
    const auto encoded =
        encode_domain(input.fields, input.field_count, domain, scope_max_bytes_, needed);
    if (encoded != AYNI_OK) {
        return encoded;
    }
    capability.domain_size = needed;
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

ayni_bytes_v1 CapabilityRegistry::domain_view(uint32_t index) const {
    return {domains_ + static_cast<std::size_t>(index) * scope_max_bytes_, at(index).domain_size};
}

} // namespace ayni::catalog
