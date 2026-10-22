// Canonical finite field domains. This codec owns their representation;
// authority owns relationships between domains, constraints and requests.
#ifndef AYNI_CORE_CATALOG_CAPABILITY_FIELD_DOMAIN_H
#define AYNI_CORE_CATALOG_CAPABILITY_FIELD_DOMAIN_H

#include <ayni/authority.h>

#include <array>
#include <cstdint>

namespace ayni::catalog {

constexpr uint16_t field_domain_format = 1;
constexpr uint32_t field_domain_header_size = 4;

// Non-owning decoded values; token views borrow the input descriptor/body.
struct FieldValues {
    ayni_authority_value_kind_t kind = 0;
    ayni_string_v1 unit{};
    int64_t minimum = 0;
    int64_t maximum = 0;
    std::array<ayni_string_v1, AYNI_AUTHORITY_SET_MEMBER_COUNT_MAX> tokens{};
    uint32_t token_count = 0;
};

struct DomainField {
    uint32_t id = 0;
    ayni_authority_field_usage_t usage = 0;
    FieldValues values;
};

// Bytewise CatalogId comparison; negative/zero/positive as left </==/> right.
int compare_tokens(ayni_string_v1 left, ayni_string_v1 right);
ayni_status_t read_field_values(const ayni_authority_value_set_v1 &input, FieldValues &out);
uint32_t encoded_field_size(const DomainField &field);
void write_domain_header(uint8_t *out, uint32_t field_count);
void write_domain_field(const DomainField &field, uint8_t *out);

// Two passes: malformed input never changes output. BUFFER_TOO_SMALL sets
// out_size to the required bytes; every other failure leaves it unchanged.
ayni_status_t encode_domain(const ayni_authority_field_desc_v1 *fields, uint32_t field_count,
                            uint8_t *out, uint32_t capacity, uint32_t &out_size);

// Accepts only the canonical versioned field shape: ascending distinct IDs
// and token members, legal types/units/usages, no trailing or missing bytes.
class DomainReader {
  public:
    explicit DomainReader(ayni_bytes_v1 body);
    bool next(DomainField &out);
    bool ok() const { return valid_ && remaining_ == 0U && at_ == body_.size; }
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
