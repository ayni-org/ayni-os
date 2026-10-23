#include "catalog/capability/field_domain.h"

#include "catalog/abi_checks.h"
#include "foundation/little_endian.h"

#include <array>

namespace ayni::catalog {
namespace {

// ADR-0029 and authority_and_trust.md, finite domain format 1. These offsets
// describe bytes on storage, independently of ABI alignment or C++ layout.
constexpr uint32_t domain_count_at = 2;
constexpr uint32_t domain_reserved_at = 3;
constexpr uint32_t field_id_at = 0;
constexpr uint32_t field_kind_at = 4;
constexpr uint32_t field_usage_at = 5;
constexpr uint32_t field_unit_size_at = 6;
constexpr uint32_t field_member_count_at = 7;
} // namespace

uint32_t encoded_field_size(const DomainField &field) {
    return field_header_size + encoded_values_size(field.values);
}

void write_domain_header(uint8_t *out, uint32_t field_count) {
    foundation::store_le16(out, field_domain_format);
    out[domain_count_at] = static_cast<uint8_t>(field_count);
    out[domain_reserved_at] = 0;
}

void write_domain_field(const DomainField &field, uint8_t *out) {
    foundation::store_le32(out + field_id_at, field.id);
    out[field_kind_at] = static_cast<uint8_t>(field.values.kind);
    out[field_usage_at] = static_cast<uint8_t>(field.usage);
    out[field_unit_size_at] = static_cast<uint8_t>(field.values.unit.size);
    out[field_member_count_at] = static_cast<uint8_t>(field.values.token_count);
    write_field_values(field.values, out + field_header_size);
}

// Admission preserves the original field/value/error order. The small plan
// carries sorted indices and byte count to registration without a second check.
ayni_status_t inspect_domain(const ayni_authority_field_desc_v1 *fields, uint32_t field_count,
                             DomainEncoding &out) {
    if (field_count > AYNI_AUTHORITY_FIELD_COUNT_MAX || (field_count != 0U && fields == nullptr)) {
        return AYNI_INVALID_ARGUMENT;
    }
    DomainEncoding encoding{};
    encoding.field_count = field_count;
    uint32_t uses = 0;
    for (uint32_t index = 0; index < field_count; ++index) {
        const auto &input = fields[index];
        const auto size_status = check_struct_size(input.struct_size, sizeof(input), sizeof(input));
        if (size_status != AYNI_OK) {
            return size_status;
        }
        DomainField field{input.field_id, input.usage, {}};
        const auto value_status = read_field_values(input.domain, field.values);
        if (value_status != AYNI_OK) {
            return value_status;
        }
        if (input.field_id == 0U || input.reserved != 0U ||
            !accept_field_usage(input.usage, field.values.kind, uses)) {
            return AYNI_INVALID_ARGUMENT;
        }
        if (!insert_field_order(fields, index, encoding.order)) {
            return AYNI_INVALID_ARGUMENT;
        }
        encoding.size += encoded_field_size(field);
    }
    out = encoding;
    return AYNI_OK;
}

void write_domain(const ayni_authority_field_desc_v1 *fields, const DomainEncoding &encoding,
                  uint8_t *out) {
    write_domain_header(out, encoding.field_count);
    uint32_t at = field_domain_header_size;
    for (uint32_t index = 0; index < encoding.field_count; ++index) {
        const auto &input = fields[encoding.order[index]];
        // inspect_domain checked this unchanged descriptor (C-AUTH-017).
        const DomainField field{input.field_id, input.usage, normalized_field_values(input.domain)};
        write_domain_field(field, out + at);
        at += encoded_field_size(field);
    }
}

ayni_status_t encode_domain(const ayni_authority_field_desc_v1 *fields, uint32_t field_count,
                            uint8_t *out, uint32_t capacity, uint32_t &out_size) {
    if (capacity != 0U && out == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    DomainEncoding encoding{};
    const auto status = inspect_domain(fields, field_count, encoding);
    if (status != AYNI_OK) {
        return status;
    }
    if (capacity < encoding.size) {
        out_size = encoding.size;
        return AYNI_BUFFER_TOO_SMALL;
    }
    write_domain(fields, encoding, out);
    out_size = encoding.size;
    return AYNI_OK;
}

DomainReader::DomainReader(ayni_bytes_v1 body) : body_(body) {
    if (body.data == nullptr || body.size < field_domain_header_size ||
        foundation::load_le16(body.data) != field_domain_format ||
        body.data[domain_count_at] > AYNI_AUTHORITY_FIELD_COUNT_MAX ||
        body.data[domain_reserved_at] != 0U) {
        valid_ = false;
        return;
    }
    at_ = field_domain_header_size;
    count_ = remaining_ = body.data[domain_count_at];
}

bool DomainReader::take(uint32_t count, const uint8_t *&out) {
    if (!valid_ || count > body_.size - at_) {
        valid_ = false;
        return false;
    }
    out = body_.data + at_;
    at_ += count;
    return true;
}

// Field framing owns ID ordering and unique usages; value semantics and
// payload bounds are shared with the raw-command codec.
bool DomainReader::next(DomainField &out) {
    if (!valid_ || remaining_ == 0U) {
        return false;
    }
    const uint8_t *bytes = nullptr;
    if (!take(field_header_size, bytes)) {
        return false;
    }
    DomainField field{};
    field.id = foundation::load_le32(bytes + field_id_at);
    field.values.kind = bytes[field_kind_at];
    field.usage = bytes[field_usage_at];
    const uint32_t unit_size = bytes[field_unit_size_at];
    field.values.token_count = bytes[field_member_count_at];
    uint32_t consumed = 0;
    if (field.id <= previous_id_ ||
        !read_encoded_values({body_.data + at_, body_.size - at_}, field.values.kind, unit_size,
                             field.values.token_count, field.values, consumed) ||
        !accept_field_usage(field.usage, field.values.kind, uses_)) {
        valid_ = false;
        return false;
    }
    at_ += consumed;
    previous_id_ = field.id;
    --remaining_;
    out = field;
    return true;
}

} // namespace ayni::catalog
