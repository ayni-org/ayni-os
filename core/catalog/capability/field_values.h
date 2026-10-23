// Shared finite values for domain format 1 and raw command scope format 1.
// Their framing differs (contracts/storage/records.md); value bytes do not.
#ifndef AYNI_CORE_CATALOG_CAPABILITY_FIELD_VALUES_H
#define AYNI_CORE_CATALOG_CAPABILITY_FIELD_VALUES_H

#include <ayni/authority.h>

#include <array>
#include <cstdint>

namespace ayni::catalog {

// Both field headers occupy eight bytes; their owners define distinct offsets.
constexpr uint32_t field_header_size = 8;

// Non-owning canonical values. Views borrow the descriptor or encoded body.
struct FieldValues {
    ayni_authority_value_kind_t kind = 0; // 0: uninitialized; otherwise one public kind.
    ayni_string_v1 unit{};                // I64: CatalogId unit; tokens: empty.
    int64_t minimum = 0;                  // Inclusive I64 endpoints in unit; min <= max.
    int64_t maximum = 0;
    // Strict bytewise order after normalization; unused entries have no meaning.
    std::array<ayni_string_v1, AYNI_AUTHORITY_SET_MEMBER_COUNT_MAX> tokens{};
    uint32_t token_count = 0; // I64: 0; ENUM/IDENTIFIER: 1..8 distinct tokens.
};

// Bytewise CatalogId comparison, negative/zero/positive as left </==/> right.
int compare_tokens(ayni_string_v1 left, ayni_string_v1 right);
// Validates the ABI shape and values, then sorts/deduplicates. Failure preserves out.
ayni_status_t read_field_values(const ayni_authority_value_set_v1 &input, FieldValues &out);
// Requires this unchanged input to have passed read_field_values in this call.
// Rebuilds its borrowed canonical view without repeating semantic validation.
FieldValues normalized_field_values(const ayni_authority_value_set_v1 &input);
// Canonical value payload size, excluding the framing owner's field header.
uint32_t encoded_values_size(const FieldValues &values);
// Requires validated canonical values and at least encoded_values_size bytes.
void write_field_values(const FieldValues &values, uint8_t *out);
// Decodes one payload using its owner's header metadata. No output changes on
// failure; success publishes consumed bytes and borrows token/unit views from body.
bool read_encoded_values(ayni_bytes_v1 body, ayni_authority_value_kind_t kind, uint32_t unit_size,
                         uint32_t token_count, FieldValues &out, uint32_t &consumed);
// Equality of validated canonical values, independent of their framing.
bool same_values(const FieldValues &left, const FieldValues &right);
// Type identity includes the unit; equal numbers in different units differ.
bool same_value_type(const FieldValues &left, const FieldValues &right);
// C-AUTH-017: target/egress require IDENTIFIER and occur at most once each.
// uses is the per-domain usage-bit set; failure leaves it unchanged.
bool accept_field_usage(ayni_authority_field_usage_t usage, ayni_authority_value_kind_t kind,
                        uint32_t &uses);

} // namespace ayni::catalog
#endif // AYNI_CORE_CATALOG_CAPABILITY_FIELD_VALUES_H
