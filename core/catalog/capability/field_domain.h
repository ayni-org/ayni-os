// Canonical finite field domains. This codec owns their representation;
// authority owns relationships between domains, constraints and requests.
#ifndef AYNI_CORE_CATALOG_CAPABILITY_FIELD_DOMAIN_H
#define AYNI_CORE_CATALOG_CAPABILITY_FIELD_DOMAIN_H

#include "catalog/capability/field_values.h"

#include <array>
#include <cstdint>

namespace ayni::catalog {

constexpr uint16_t field_domain_format = 1;
constexpr uint32_t field_domain_header_size = 4;

// Sorted indices into either ABI domain fields or ABI scope fields.
using FieldOrder = std::array<uint32_t, AYNI_AUTHORITY_FIELD_COUNT_MAX>;

// Insert one field ID after the caller validates its shape/values. The prefix
// [0, index) is already sorted; failure discards this local admission plan.
template <typename Field>
bool insert_field_order(const Field *fields, uint32_t index, FieldOrder &order) {
    const uint32_t id = fields[index].field_id;
    uint32_t at = index;
    while (at > 0U && fields[order[at - 1U]].field_id > id) {
        order[at] = order[at - 1U];
        --at;
    }
    if (at > 0U && fields[order[at - 1U]].field_id == id) {
        return false;
    }
    order[at] = index;
    return true;
}

// One canonical domain field; id/usage are framing, values borrow their source.
struct DomainField {
    uint32_t id = 0;                        // Nonzero within a domain; 0 only before decoding.
    ayni_authority_field_usage_t usage = 0; // Public usage; 0 is uninitialized.
    FieldValues values; // Canonical value set, in the units declared by this field.
};

// Complete field bytes for validated values, including its domain header.
uint32_t encoded_field_size(const DomainField &field);
// Writes format/count/reserved after the caller has bounded count and output.
void write_domain_header(uint8_t *out, uint32_t field_count);
// Writes an inspected field into encoded_field_size writable bytes.
void write_domain_field(const DomainField &field, uint8_t *out);

// Short-lived admission result for unchanged descriptor views. inspect_domain
// is the only producer; valid only after AYNI_OK and until the input changes.
struct DomainEncoding {
    FieldOrder order{};                       // Sorted input indices; only field_count are valid.
    uint32_t field_count = 0;                 // Valid prefix of order, including an empty domain.
    uint32_t size = field_domain_header_size; // Complete encoded bytes, header included.
};
// Validates and measures without writing bytes; failure preserves out.
ayni_status_t inspect_domain(const ayni_authority_field_desc_v1 *fields, uint32_t field_count,
                             DomainEncoding &out);
// Requires unchanged inspected fields and encoding.size writable bytes.
void write_domain(const ayni_authority_field_desc_v1 *fields, const DomainEncoding &encoding,
                  uint8_t *out);

// Two passes: malformed input never changes output. BUFFER_TOO_SMALL sets
// out_size to the required bytes; every other failure leaves it unchanged.
ayni_status_t encode_domain(const ayni_authority_field_desc_v1 *fields, uint32_t field_count,
                            uint8_t *out, uint32_t capacity, uint32_t &out_size);

// Accepts only the canonical versioned field shape: ascending distinct IDs
// and token members, legal types/units/usages, no trailing or missing bytes.
class DomainReader {
  public:
    // Borrows a complete body; malformed framing makes next/ok fail.
    explicit DomainReader(ayni_bytes_v1 body);
    // Advances one field; false preserves out and distinguishes end via ok().
    bool next(DomainField &out);
    // True only after consuming every field and every body byte.
    bool ok() const { return valid_ && remaining_ == 0U && at_ == body_.size; }
    // Declared field count, meaningful after valid framing.
    uint32_t field_count() const { return count_; }

  private:
    bool take(uint32_t count, const uint8_t *&out);
    ayni_bytes_v1 body_;
    uint32_t at_ = 0;
    uint32_t count_ = 0;
    uint32_t remaining_ = 0;
    uint32_t previous_id_ = 0;
    uint32_t uses_ = 0;
    bool valid_ = true;
};

} // namespace ayni::catalog
#endif // AYNI_CORE_CATALOG_CAPABILITY_FIELD_DOMAIN_H
