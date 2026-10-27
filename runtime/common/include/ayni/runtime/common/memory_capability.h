// Trusted M1 memory.access startup domain, shared by deployment profiles.
#ifndef AYNI_RUNTIME_COMMON_MEMORY_CAPABILITY_H
#define AYNI_RUNTIME_COMMON_MEMORY_CAPABILITY_H
#include <array>
#include <ayni/authority.h>
#include <ayni/memory.h>
#include <limits>
namespace ayni::runtime {
// Four public scope-builder fields (C-MEM-003): record ID components, purpose and
// operation. Units borrow static strings; callers own the copied descriptors.
// Core's scope helpers and authorization boundary remain the validators.
inline std::array<ayni_authority_field_desc_v1, AYNI_MEMORY_SCOPE_FIELD_COUNT>
memory_access_fields() {
    std::array<ayni_authority_field_desc_v1, AYNI_MEMORY_SCOPE_FIELD_COUNT> fields{};
    constexpr uint32_t identity_fields = 2;
    constexpr int64_t operation_write = 2;
    for (uint32_t at = 0; at < fields.size(); ++at) {
        auto &field = fields[at];
        field.struct_size = sizeof(field);
        field.field_id = at + 1;
        field.usage = AYNI_AUTHORITY_PARAMETER;
        field.domain.struct_size = sizeof(field.domain);
        field.domain.kind = AYNI_AUTHORITY_I64;
        field.domain.unit =
            at < identity_fields ? ayni_string_v1{"id", 2} : ayni_string_v1{"code", 4};
        field.domain.values.integer = {1, at < identity_fields ? std::numeric_limits<int64_t>::max()
                                                               : operation_write};
    }
    return fields;
}
} // namespace ayni::runtime
#endif
