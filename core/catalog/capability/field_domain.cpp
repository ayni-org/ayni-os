#include "catalog/capability/field_domain.h"

#include "catalog/catalog_id.h"
#include "foundation/little_endian.h"

#include <cstring>
#include <limits>

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
constexpr uint32_t field_header_size = 8;
constexpr uint32_t integer_endpoint_size = 8;
constexpr uint32_t integer_pair_size = 2U * integer_endpoint_size;
constexpr uint32_t token_length_size = 1;

ayni_status_t check_size(uint32_t actual, uint32_t expected) {
    return actual == expected  ? AYNI_OK
           : actual < expected ? AYNI_INVALID_ARGUMENT
                               : AYNI_UNSUPPORTED_VERSION;
}

bool legal_usage(ayni_authority_field_usage_t usage, ayni_authority_value_kind_t kind) {
    return usage == AYNI_AUTHORITY_PARAMETER || ((usage == AYNI_AUTHORITY_EXECUTION_TARGET ||
                                                  usage == AYNI_AUTHORITY_EGRESS_DESTINATION) &&
                                                 kind == AYNI_AUTHORITY_IDENTIFIER);
}

// No implementation-defined conversion from a large unsigned value to i64.
int64_t signed_value(uint64_t bits) {
    return bits <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
               ? static_cast<int64_t>(bits)
               : -1 - static_cast<int64_t>(~bits);
}

} // namespace

int compare_tokens(ayni_string_v1 left, ayni_string_v1 right) {
    const uint32_t size = left.size < right.size ? left.size : right.size;
    const int order = size == 0U ? 0 : std::memcmp(left.data, right.data, size);
    return order != 0 ? order : left.size < right.size ? -1 : left.size > right.size ? 1 : 0;
}

ayni_status_t read_field_values(const ayni_authority_value_set_v1 &input, FieldValues &out) {
    const auto status = check_size(input.struct_size, sizeof(input));
    if (status != AYNI_OK) {
        return status;
    }
    FieldValues value{};
    value.kind = input.kind;
    value.unit = input.unit;
    if (input.kind == AYNI_AUTHORITY_I64) {
        if (!CatalogId::is_valid(input.unit) ||
            input.values.integer.min_inclusive > input.values.integer.max_inclusive) {
            return AYNI_INVALID_ARGUMENT;
        }
        value.minimum = input.values.integer.min_inclusive;
        value.maximum = input.values.integer.max_inclusive;
    } else if (input.kind == AYNI_AUTHORITY_ENUM || input.kind == AYNI_AUTHORITY_IDENTIFIER) {
        const auto &tokens = input.values.tokens;
        if (input.unit.size != 0U || tokens.reserved != 0U || tokens.member_count == 0U ||
            tokens.member_count > AYNI_AUTHORITY_SET_MEMBER_COUNT_MAX ||
            tokens.members == nullptr) {
            return AYNI_INVALID_ARGUMENT;
        }
        for (uint32_t index = 0; index < tokens.member_count; ++index) {
            const auto token = tokens.members[index];
            if (!CatalogId::is_valid(token)) {
                return AYNI_INVALID_ARGUMENT;
            }
            uint32_t at = 0;
            while (at < value.token_count && compare_tokens(value.tokens[at], token) < 0) {
                ++at;
            }
            if (at < value.token_count && compare_tokens(value.tokens[at], token) == 0) {
                continue;
            }
            for (uint32_t move = value.token_count; move > at; --move) {
                value.tokens[move] = value.tokens[move - 1U];
            }
            value.tokens[at] = token;
            ++value.token_count;
        }
    } else {
        return AYNI_INVALID_ARGUMENT;
    }
    out = value;
    return AYNI_OK;
}

uint32_t encoded_field_size(const DomainField &field) {
    uint32_t size = field_header_size + field.values.unit.size;
    if (field.values.kind == AYNI_AUTHORITY_I64) {
        return size + integer_pair_size;
    }
    for (uint32_t index = 0; index < field.values.token_count; ++index) {
        size += token_length_size + field.values.tokens[index].size;
    }
    return size;
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
    uint32_t at = field_header_size;
    if (field.values.unit.size != 0U) {
        std::memcpy(out + at, field.values.unit.data, field.values.unit.size);
        at += field.values.unit.size;
    }
    if (field.values.kind == AYNI_AUTHORITY_I64) {
        foundation::store_le64(out + at, static_cast<uint64_t>(field.values.minimum));
        foundation::store_le64(out + at + integer_endpoint_size,
                               static_cast<uint64_t>(field.values.maximum));
        return;
    }
    for (uint32_t index = 0; index < field.values.token_count; ++index) {
        const auto token = field.values.tokens[index];
        out[at++] = static_cast<uint8_t>(token.size);
        std::memcpy(out + at, token.data, token.size);
        at += token.size;
    }
}

ayni_status_t encode_domain(const ayni_authority_field_desc_v1 *fields, uint32_t field_count,
                            uint8_t *out, uint32_t capacity, uint32_t &out_size) {
    if (field_count > AYNI_AUTHORITY_FIELD_COUNT_MAX || (field_count != 0U && fields == nullptr) ||
        (capacity != 0U && out == nullptr)) {
        return AYNI_INVALID_ARGUMENT;
    }
    std::array<uint32_t, AYNI_AUTHORITY_FIELD_COUNT_MAX> order{};
    uint32_t needed = field_domain_header_size;
    uint32_t uses = 0;
    for (uint32_t index = 0; index < field_count; ++index) {
        const auto &input = fields[index];
        const auto size_status = check_size(input.struct_size, sizeof(input));
        if (size_status != AYNI_OK) {
            return size_status;
        }
        DomainField field{input.field_id, input.usage, {}};
        const auto value_status = read_field_values(input.domain, field.values);
        if (value_status != AYNI_OK) {
            return value_status;
        }
        if (input.field_id == 0U || input.reserved != 0U ||
            !legal_usage(input.usage, field.values.kind)) {
            return AYNI_INVALID_ARGUMENT;
        }
        const uint32_t use_bit = 1U << input.usage;
        if (input.usage != AYNI_AUTHORITY_PARAMETER && (uses & use_bit) != 0U) {
            return AYNI_INVALID_ARGUMENT;
        }
        uses |= use_bit;
        uint32_t at = index;
        while (at > 0U && fields[order[at - 1U]].field_id > input.field_id) {
            order[at] = order[at - 1U];
            --at;
        }
        if (at > 0U && fields[order[at - 1U]].field_id == input.field_id) {
            return AYNI_INVALID_ARGUMENT;
        }
        order[at] = index;
        needed += encoded_field_size(field);
    }
    if (capacity < needed) {
        out_size = needed;
        return AYNI_BUFFER_TOO_SMALL;
    }
    write_domain_header(out, field_count);
    uint32_t at = field_domain_header_size;
    for (uint32_t index = 0; index < field_count; ++index) {
        const auto &input = fields[order[index]];
        DomainField field{input.field_id, input.usage, {}};
        static_cast<void>(read_field_values(input.domain, field.values));
        write_domain_field(field, out + at);
        at += encoded_field_size(field);
    }
    out_size = needed;
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
    if (field.id <= previous_id_ || unit_size > CatalogId::max_size ||
        !legal_usage(field.usage, field.values.kind) || !take(unit_size, bytes)) {
        valid_ = false;
        return false;
    }
    field.values.unit = {reinterpret_cast<const char *>(bytes), unit_size};
    const uint32_t use_bit = 1U << field.usage;
    if (field.usage != AYNI_AUTHORITY_PARAMETER && (uses_ & use_bit) != 0U) {
        valid_ = false;
        return false;
    }
    if (field.values.kind == AYNI_AUTHORITY_I64) {
        if (!CatalogId::is_valid(field.values.unit) || field.values.token_count != 0U ||
            !take(integer_pair_size, bytes)) {
            valid_ = false;
            return false;
        }
        field.values.minimum = signed_value(foundation::load_le64(bytes));
        field.values.maximum = signed_value(foundation::load_le64(bytes + integer_endpoint_size));
        if (field.values.minimum > field.values.maximum) {
            valid_ = false;
            return false;
        }
    } else if (field.values.kind == AYNI_AUTHORITY_ENUM ||
               field.values.kind == AYNI_AUTHORITY_IDENTIFIER) {
        if (unit_size != 0U || field.values.token_count == 0U ||
            field.values.token_count > AYNI_AUTHORITY_SET_MEMBER_COUNT_MAX) {
            valid_ = false;
            return false;
        }
        for (uint32_t index = 0; index < field.values.token_count; ++index) {
            if (!take(token_length_size, bytes)) {
                return false;
            }
            const uint32_t size = bytes[0];
            if (size > CatalogId::max_size || !take(size, bytes)) {
                valid_ = false;
                return false;
            }
            const ayni_string_v1 token{reinterpret_cast<const char *>(bytes), size};
            if (!CatalogId::is_valid(token) ||
                (index != 0U && compare_tokens(field.values.tokens[index - 1U], token) >= 0)) {
                valid_ = false;
                return false;
            }
            field.values.tokens[index] = token;
        }
    } else {
        valid_ = false;
        return false;
    }
    previous_id_ = field.id;
    uses_ |= use_bit;
    --remaining_;
    out = field;
    return true;
}

} // namespace ayni::catalog
