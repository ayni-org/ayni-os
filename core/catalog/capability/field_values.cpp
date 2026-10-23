#include "catalog/capability/field_values.h"

#include "catalog/abi_checks.h"
#include "catalog/catalog_id.h"
#include "foundation/little_endian.h"

#include <cstring>
#include <limits>

namespace ayni::catalog {
namespace {

// contracts/storage/records.md: unit bytes, then two LE i64 endpoints or
// length-prefixed tokens. Domain and raw-command framing share this payload.
constexpr uint32_t integer_endpoint_size = sizeof(uint64_t);
constexpr uint32_t integer_pair_size = sizeof(uint64_t) + sizeof(uint64_t);
constexpr uint32_t token_length_size = sizeof(uint8_t);

bool valid_integer(ayni_string_v1 unit, int64_t minimum, int64_t maximum) {
    return CatalogId::is_valid(unit) && minimum <= maximum;
}

bool valid_tokens(ayni_authority_value_kind_t kind, ayni_string_v1 unit, uint32_t count) {
    return (kind == AYNI_AUTHORITY_ENUM || kind == AYNI_AUTHORITY_IDENTIFIER) && unit.size == 0U &&
           count != 0U && count <= AYNI_AUTHORITY_SET_MEMBER_COUNT_MAX;
}

// No implementation-defined unsigned-to-signed conversion, including INT64_MIN.
int64_t signed_value(uint64_t bits) {
    return bits <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
               ? static_cast<int64_t>(bits)
               : -1 - static_cast<int64_t>(~bits);
}

bool take(ayni_bytes_v1 &remaining, uint32_t count, const uint8_t *&out) {
    if (count > remaining.size) {
        return false;
    }
    out = remaining.data;
    remaining.data += count;
    remaining.size -= count;
    return true;
}

} // namespace

int compare_tokens(ayni_string_v1 left, ayni_string_v1 right) {
    const uint32_t size = left.size < right.size ? left.size : right.size;
    const int order = size == 0U ? 0 : std::memcmp(left.data, right.data, size);
    return order != 0 ? order : left.size < right.size ? -1 : left.size > right.size ? 1 : 0;
}

// C-AUTH-017 accepts unordered/repeated ABI members but stores one sorted set.
// Validation has already bounded and checked every view before comparisons.
FieldValues normalized_field_values(const ayni_authority_value_set_v1 &input) {
    FieldValues value{};
    value.kind = input.kind;
    value.unit = input.unit;
    if (input.kind == AYNI_AUTHORITY_I64) {
        value.minimum = input.values.integer.min_inclusive;
        value.maximum = input.values.integer.max_inclusive;
        return value;
    }
    for (uint32_t index = 0; index < input.values.tokens.member_count; ++index) {
        const auto token = input.values.tokens.members[index];
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
    return value;
}

// ABI-only pointer/reserved/size checks precede the shared value rules. Unlike
// encoded input, typed input may contain unordered and duplicate token members.
ayni_status_t read_field_values(const ayni_authority_value_set_v1 &input, FieldValues &out) {
    const auto status = check_struct_size(input.struct_size, sizeof(input), sizeof(input));
    if (status != AYNI_OK) {
        return status;
    }
    if (input.kind == AYNI_AUTHORITY_I64) {
        if (!valid_integer(input.unit, input.values.integer.min_inclusive,
                           input.values.integer.max_inclusive)) {
            return AYNI_INVALID_ARGUMENT;
        }
    } else if (input.kind == AYNI_AUTHORITY_ENUM || input.kind == AYNI_AUTHORITY_IDENTIFIER) {
        const auto &tokens = input.values.tokens;
        if (!valid_tokens(input.kind, input.unit, tokens.member_count) || tokens.reserved != 0U ||
            tokens.members == nullptr) {
            return AYNI_INVALID_ARGUMENT;
        }
        for (uint32_t index = 0; index < tokens.member_count; ++index) {
            if (!CatalogId::is_valid(tokens.members[index])) {
                return AYNI_INVALID_ARGUMENT;
            }
        }
    } else {
        return AYNI_INVALID_ARGUMENT;
    }
    out = normalized_field_values(input);
    return AYNI_OK;
}

uint32_t encoded_values_size(const FieldValues &values) {
    uint32_t size = values.unit.size;
    if (values.kind == AYNI_AUTHORITY_I64) {
        return size + integer_pair_size;
    }
    for (uint32_t index = 0; index < values.token_count; ++index) {
        size += token_length_size + values.tokens[index].size;
    }
    return size;
}

// The caller's admission pass proved shape, order and output capacity. Keeping
// this infallible avoids a partial record after an ignored second validation.
void write_field_values(const FieldValues &values, uint8_t *out) {
    uint32_t at = 0;
    if (values.unit.size != 0U) {
        std::memcpy(out, values.unit.data, values.unit.size);
        at += values.unit.size;
    }
    if (values.kind == AYNI_AUTHORITY_I64) {
        foundation::store_le64(out + at, static_cast<uint64_t>(values.minimum));
        foundation::store_le64(out + at + integer_endpoint_size,
                               static_cast<uint64_t>(values.maximum));
        return;
    }
    for (uint32_t index = 0; index < values.token_count; ++index) {
        const auto token = values.tokens[index];
        out[at++] = static_cast<uint8_t>(token.size);
        std::memcpy(out + at, token.data, token.size);
        at += token.size;
    }
}

// Both record owners supply header metadata; this shared reader owns bounds,
// signed endpoint decoding and canonical value validation, not field IDs/usages.
bool read_encoded_values(ayni_bytes_v1 body, ayni_authority_value_kind_t kind, uint32_t unit_size,
                         uint32_t token_count, FieldValues &out, uint32_t &consumed) {
    if (body.data == nullptr || unit_size > CatalogId::max_size) {
        return false;
    }
    auto remaining = body;
    const uint8_t *bytes = nullptr;
    if (!take(remaining, unit_size, bytes)) {
        return false;
    }
    FieldValues value{};
    value.kind = kind;
    value.unit = {reinterpret_cast<const char *>(bytes), unit_size};
    value.token_count = token_count;
    if (kind == AYNI_AUTHORITY_I64) {
        if (token_count != 0U || !take(remaining, integer_pair_size, bytes)) {
            return false;
        }
        value.minimum = signed_value(foundation::load_le64(bytes));
        value.maximum = signed_value(foundation::load_le64(bytes + integer_endpoint_size));
        if (!valid_integer(value.unit, value.minimum, value.maximum)) {
            return false;
        }
    } else {
        if (!valid_tokens(kind, value.unit, token_count)) {
            return false;
        }
        for (uint32_t index = 0; index < token_count; ++index) {
            if (!take(remaining, token_length_size, bytes)) {
                return false;
            }
            const uint32_t size = bytes[0];
            if (!take(remaining, size, bytes)) {
                return false;
            }
            const ayni_string_v1 token{reinterpret_cast<const char *>(bytes), size};
            if (!CatalogId::is_valid(token) ||
                (index != 0U && compare_tokens(value.tokens[index - 1U], token) >= 0)) {
                return false;
            }
            value.tokens[index] = token;
        }
    }
    out = value;
    consumed = body.size - remaining.size;
    return true;
}

bool same_value_type(const FieldValues &left, const FieldValues &right) {
    return left.kind == right.kind && compare_tokens(left.unit, right.unit) == 0;
}

bool same_values(const FieldValues &left, const FieldValues &right) {
    if (!same_value_type(left, right)) {
        return false;
    }
    if (left.kind == AYNI_AUTHORITY_I64) {
        return left.minimum == right.minimum && left.maximum == right.maximum;
    }
    if (left.token_count != right.token_count) {
        return false;
    }
    for (uint32_t index = 0; index < left.token_count; ++index) {
        if (compare_tokens(left.tokens[index], right.tokens[index]) != 0) {
            return false;
        }
    }
    return true;
}

bool accept_field_usage(ayni_authority_field_usage_t usage, ayni_authority_value_kind_t kind,
                        uint32_t &uses) {
    if (usage != AYNI_AUTHORITY_PARAMETER && !((usage == AYNI_AUTHORITY_EXECUTION_TARGET ||
                                                usage == AYNI_AUTHORITY_EGRESS_DESTINATION) &&
                                               kind == AYNI_AUTHORITY_IDENTIFIER)) {
        return false;
    }
    // Only validated public usages reach the shift. Bit 0 is unused; parameter
    // fields may repeat, while target and egress each consume one bit (C-AUTH-017).
    const uint32_t use_bit = 1U << usage;
    if (usage != AYNI_AUTHORITY_PARAMETER && (uses & use_bit) != 0U) {
        return false;
    }
    uses |= use_bit;
    return true;
}

} // namespace ayni::catalog
